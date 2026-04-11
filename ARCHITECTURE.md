# TeleVault Architecture

## System Overview

TeleVault provides unlimited cloud storage by using a private Telegram channel as a persistent data store, accessed via the MTProto protocol through Telethon. There is no local database -- all state is maintained as pinned messages and message reply graphs within the channel. File content is encrypted on the client before upload, ensuring plaintext never leaves the user's control.

The CLI tool installs as `tvt`, with `televault` as an alias:

```
[project.scripts]
tvt        = "televault.cli:main"
televault  = "televault.cli:main"
```

### Module Map

| Module | Responsibility |
|---|---|
| `cli.py` | Click-based CLI, command dispatch, progress display, friendly error handling |
| `core.py` | `TeleVault` class -- upload, download, list, search, delete, resume, stream |
| `telegram.py` | `TelegramVault` -- MTProto client wrapper, index management, message I/O, channel ops |
| `models.py` | Data models: `FileMetadata`, `ChunkInfo`, `VaultIndex`, `TransferProgress` |
| `chunker.py` | File splitting/merging, `ChunkWriter`, BLAKE3 hashing |
| `crypto.py` | AES-256-GCM encryption, scrypt KDF, streaming encryptor/decryptor |
| `compress.py` | Zstandard compression/decompression, extension-based skip logic |
| `config.py` | `Config` dataclass, config directory resolution, persistence |
| `retry.py` | Exponential backoff with jitter, FloodWait handling |
| `backup.py` | `BackupEngine` -- snapshot create/restore/list/delete/prune/verify |
| `snapshot.py` | `Snapshot`, `SnapshotFile`, `SnapshotIndex`, `RetentionPolicy` |
| `fuse.py` | `TeleVaultFuse` -- FUSE driver with on-demand chunk streaming and LRU cache |
| `webdav.py` | `WebDAVHandler` + `WebDAVServer` -- HTTP/WebDAV access |
| `preview.py` | `PreviewEngine` -- terminal previews from first 1-2 chunks |
| `completion.py` | Shell completion scripts (bash, zsh, fish, PowerShell) |
| `watcher.py` | `FileWatcher` -- polling-based directory monitor with BLAKE2 hashing |
| `schedule.py` | Schedule CRUD, systemd timer generation, cron entry generation |
| `gc.py` | Orphan message detection and cleanup |
| `logging.py` | `RotatingFileHandler` setup, console + file output |
| `tui.py` | Textual-based interactive terminal UI with detail panel |

---

## Storage Model

A single private Telegram channel holds every piece of TeleVault data. The channel contains pinned index messages and content messages organized in reply chains:

```
Channel
 +-- Pinned: VaultIndex (file_id -> metadata_message_id)
 +-- Pinned: SnapshotIndex (snapshot_id -> message_id)
 +-- Text messages: FileMetadata (JSON)
 +-- File messages: chunk data (replying to their FileMetadata message)
 +-- Text messages: Snapshot (JSON)
```

### Message Topology

```
                  Channel (private)
                 ==================
                 |  Pinned: VaultIndex  |---- files: { "abc123": 42, "def456": 87, ... }
                 |  Pinned: SnapshotIndex|---- snapshots: { "snap01": 150, ... }
                 ==================
                        |
           +------------+------------+
           |                         |
     msg 42 (text/JSON)        msg 87 (text/JSON)
     FileMetadata for           FileMetadata for
     file "abc123"              file "def456"
           |                         |
     +-----+-----+            +------+------+
     |     |     |            |      |      |
   reply  reply  reply       reply   reply  reply
   msg43  msg44  msg45       msg88  msg89  msg90
   chunk0 chunk1 chunk2      chunk0 chunk1  chunk2
```

### FileMetadata (stored as JSON text message)

```json
{
  "id": "abc123def456",
  "name": "photo.jpg",
  "size": 5242880,
  "hash": "a1b2c3d4...32 chars of BLAKE3",
  "chunks": [
    {
      "index": 0,
      "message_id": 43,
      "size": 10485780,
      "hash": "e5f6a7b8...32 chars of BLAKE3 (post-processing)",
      "original_hash": "c9d0e1f2...32 chars of BLAKE3 (pre-processing)"
    }
  ],
  "encrypted": true,
  "compressed": true,
  "compression_ratio": null,
  "mime_type": null,
  "created_at": 1700000000.0,
  "modified_at": null,
  "message_id": 42
}
```

### VaultIndex (stored as pinned text message)

```json
{
  "version": 7,
  "files": { "abc123def456": 42, "def789ghi012": 87 },
  "updated_at": 1700000100.0
}
```

The `version` field implements optimistic concurrency control. On save, the client reads the current version, increments it, and overwrites the pinned message. If the version mismatches (concurrent modification), the save is retried up to 5 times with backoff.

---

## Encryption Pipeline

### Upload (encrypt-then-upload)

```
Original File
     |
     v
  Chunker (100 MB slices)
     |
     v
  Per-chunk: compute original_hash via BLAKE3
     |
     v
  Optional Compression (zstd level 3)
  -- skipped for incompressible extensions
     |
     v
  Encryption (AES-256-GCM)
  -- scrypt(password, salt) -> 32-byte key
  -- 16-byte random salt + 12-byte random nonce = 28-byte header
  -- GCM produces ciphertext + 16-byte authentication tag
  -- total overhead per chunk: 44 bytes (28 header + 16 tag)
     |
     v
  Compute hash (BLAKE3 of encrypted+compressed data) -> ChunkInfo.hash
     |
     v
  Upload as Telegram file message, replying to the file's metadata message
```

### Download (download-then-decrypt)

```
Download chunk message by message_id
     |
     v
  Verify hash (BLAKE3 of raw data matches ChunkInfo.hash)
     |
     v
  Decryption (AES-256-GCM)
  -- extract 28-byte header (salt + nonce)
  -- scrypt(password, salt) -> key
  -- decrypt and verify GCM tag
     |
     v
  Optional Decompression (zstd)
     |
     v
  Verify original_hash (BLAKE3 matches ChunkInfo.original_hash)
  -- catches wrong-password decryption that passes GCM
     |
     v
  Write chunk at correct offset via ChunkWriter
     |
     v
  After all chunks: verify file-level BLAKE3 hash matches FileMetadata.hash
```

### Key Derivation

```
scrypt(password, salt):
  N = 2^17 (131072)
  r = 8
  p = 1
  output length = 32 bytes (256-bit AES key)
```

### Incompressible Extensions (compression skipped)

Images: `.jpg` `.jpeg` `.png` `.gif` `.webp` `.heic` `.heif` `.avif`
Video: `.mp4` `.mkv` `.avi` `.mov` `.webm` `.m4v` `.wmv` `.flv`
Audio: `.mp3` `.aac` `.ogg` `.opus` `.flac` `.m4a` `.wma`
Archives: `.zip` `.gz` `.bz2` `.xz` `.7z` `.rar` `.zst` `.lz4` `.lzma`
Documents: `.pdf` `.docx` `.xlsx` `.pptx` `.odt`
Other: `.woff` `.woff2` `.br`

---

##Chunk Management

### Chunk Size

Default: 100 MB (`100 * 1024 * 1024`). Configurable via `config.json`. Telegram's MTProto limit is ~2 GB per file; chunks stay well within this at 100 MB.

### Parallel Transfers

| Operation | Default Concurrency | Semaphore |
|---|---|---|
| Upload | 3 concurrent | `asyncio.Semaphore(config.parallel_uploads)` |
| Download | 5 concurrent | `asyncio.Semaphore(config.parallel_downloads)` |

### ChunkWriter

Pre-allocates the output file with `f.truncate(total_size)` and writes each chunk at its computed offset (`chunk.index * chunk_size`). Supports out-of-order writes during parallel downloads.

### TransferProgress with CRC32

For resumable downloads, progress is saved to a `.progress` file alongside the `.partial` download file. The CRC32 integrity check ensures corrupted progress files are discarded and the download starts fresh.

---

## FUSE Virtual Drive

The `TeleVaultFuse` class implements `fusepy.Operations` with on-demand chunk streaming:

- **On open**: Prefetches first 3 chunks into LRU cache
- **On read**: Downloads only the chunks overlapping the requested byte range
- **On write**: Buffers writes locally, uploads on flush
- **LRU Cache**: Configurable size (default 100 MB), `OrderedDict`-based eviction
- **Cache invalidation**: On file delete, removes all cached chunks
- **Debounced refresh**: 2-second debounce on index refresh for `getattr`/`readdir`
- **ChunkCache**: Per-file chunk manager that fetches only needed chunks
- **Read-only mode**: Returns `EACCES` (errno 30) for write attempts
- **StatFS**: Reports a virtual 4 TB filesystem

---

## Preview System

The `PreviewEngine` downloads only the first 1-2 chunks and extracts metadata from binary headers without requiring a full download:

- **Classification**: Categorizes files as image/video/audio/text/document/archive/binary
- **Image metadata**: PNG dimensions, JPEG dimensions, GIF dimensions
- **Video metadata**: MKV/WebM magic, MP4 ftyp brand, AVI RIFF header
- **Audio metadata**: MP3 ID3 tags, FLAC stream info, WAV format
- **Text preview**: UTF-8 detection, line counting, first N lines
- **Hex dump**: Binary files shown as hex + ASCII

---

## CLI Structure

```
tvt push <path>                    Upload file/directory
  --password / -p                  Encryption password
  --no-compress                    Disable compression
  --no-encrypt                     Disable encryption
  --recursive / -r                 Upload directory recursively
  --resume                         Resume interrupted upload

tvt pull <file_id_or_name>         Download file
  --output / -o                    Output path (-o - for stdout)
  --password / -p                  Decryption password
  --resume                         Resume interrupted download

tvt ls                             List files
  --json                           JSON output (pipeable)
  --sort <name|size|date>          Sort order

tvt cat <file_id_or_name>          Output file to stdout (pipeable)

tvt preview <file_id_or_name>      File preview without full download
  --password / -p                  Decryption password

tvt info <file_id_or_name>         Detailed file information
  --json                           JSON output

tvt stat                           Vault status/overview
  --json                           JSON output

tvt find <query>                   Search files by name
  --json                           JSON output

tvt rm <file_id_or_name>           Delete file
  --yes / -y                       Skip confirmation

tvt verify <file_id_or_name>       Verify file integrity
  --password / -p                  Decryption password

tvt gc                             Garbage collection
  --dry-run                        List orphans without deleting
  --clean-partials                 Remove incomplete uploads

tvt login                          Telegram authentication
  --phone / -p <number>            Phone number

tvt setup                          Configure storage channel
  --channel-id / -c <id>           Use existing channel by ID
  --auto / -a                      Auto-create channel without prompting

tvt channel                        Show current channel info

tvt mount <mount_point>            Mount as FUSE filesystem
  --password / -p                  Encryption password
  --read-only                      Read-only mount
  --cache-dir                      Local cache directory
  --cache-size                     Max cache size in MB (default 100)
  --allow-other                     Allow other users
  --foreground / --background     Foreground or daemon mode

tvt serve                          Start WebDAV server
  --host / -h <addr>               Bind address (default 0.0.0.0)
  --port / -p <port>               Port (default 8080)
  --password / -P                  Encryption password
  --read-only                      Read-only mode
  --cache-dir                      Local cache directory

tvt whoami                         Show current Telegram account
tvt logout                         Clear session
tvt tui                            Launch interactive TUI

tvt completion <shell>             Generate shell completion
  (bash, zsh, fish, powershell)

tvt backup create <path>           Create backup snapshot
  --name / -n                      Snapshot name
  --password / -p                  Encryption password
  --incremental / -i               Incremental backup
  --dry-run                        Show plan without uploading

tvt backup restore <snapshot_id>   Restore from snapshot
  --output / -o                    Output directory
  --password / -p                  Decryption password
  --files / -f                     Specific files to restore

tvt backup list                    List snapshots
tvt backup delete <snapshot_id>    Delete snapshot
tvt backup prune                   Prune old snapshots
  --keep-daily / --keep-weekly / --keep-monthly
  --dry-run
tvt backup verify <snapshot_id>    Verify snapshot integrity

tvt schedule create <path>         Create backup schedule
  --name / -n, --interval, --password, --incremental

tvt schedule list / run / delete / install / uninstall / show-systemd

tvt watch                          Watch directories for changes
  --path / -p, --interval, --exclude
```

### Global Flags

| Flag | Effect |
|---|---|
| `-v` / `--verbose` | INFO-level logging |
| `--debug` | DEBUG-level logging |
| `--version` | Show version |
| `--help` | Show help |

### Pipeable Commands

- `tvt push -` reads from stdin
- `tvt pull -o -` writes to stdout
- `tvt cat <file>` writes to stdout
- `tvt ls --json`, `tvt stat --json`, `tvt info --json`, `tvt find --json` output JSON

### Error Handling

All CLI errors are user-friendly. No Python tracebacks leak to the terminal:
- `ConnectionError` -> "Check your internet connection"
- `RuntimeError("Not connected")` -> "Run `tvt login` first"
- `FloodWaitError` -> "Wait a few minutes and try again"
- `AuthKeyError/Unauthorized` -> "Session expired, run `tvt login`"
- `ApiIdInvalidError` -> "Check your API credentials"
- `KeyboardInterrupt` -> clean "Interrupted"

---

## Auto-Backup System

### FileWatcher (`watcher.py`)

Polling-based filesystem monitor with BLAKE2b change detection, exclusion patterns (.git, __pycache__, etc.), and state persistence to `watcher_state.json`.

### ScheduleConfig

Stored as individual JSON files in `~/.config/televault/schedules/<name>.json` with interval, path, password, and last-run tracking.

### systemd Timer Integration

Installing a schedule creates `televault-<name>.timer` and `televault-<name>.service` in `~/.config/systemd/user/`.

---

## Retry and Reliability

### Exponential Backoff

```
delay = min(base_delay * 2^attempt, max_delay)
delay = delay * (0.5 + random())   # jitter
```

Default: `max_retries = 3`, `retry_delay = 1.0`, `max_delay = 60.0`

### FloodWaitError Handling

Uses server-suggested wait time if it exceeds calculated backoff. Caps retries at 3 for flood waits. Raises immediately if `seconds > 300`.

### Upload Cleanup on Failure

All uploaded chunk messages and the metadata message are deleted. Index is not updated.

### Garbage Collection

`collect_garbage()` scans for orphaned messages. `cleanup_partial_uploads()` removes incomplete uploads.

---

## Security Model

| Threat | Mitigation |
|---|---|
| Telegram server reads data | AES-256-GCM encryption; server sees only ciphertext |
| Data corruption in transit | BLAKE3 hash at chunk and file level |
| Wrong password decryption | GCM authentication + original_hash double check |
| Concurrent index modification | Optimistic concurrency with version counter |
| Flood limits / rate limiting | Exponential backoff with FloodWait handling |
| Orphaned data | Garbage collection command |

---

## Configuration

### File Locations

| Purpose | Path |
|---|---|
| Main config | `~/.config/televault/config.json` |
| Telegram credentials | `~/.config/televault/telegram.json` |
| Schedule configs | `~/.config/televault/schedules/<name>.json` |
| Log file | `~/.local/share/televault/televault.log` |
| FUSE cache | `~/.local/share/televault/fuse_cache/` |
| WebDAV cache | `~/.local/share/televault/webdav_cache/` |
| Completion cache | `~/.config/televault/completion_cache.json` |
| Watcher state | `~/.local/share/televault/watcher/watcher_state.json` |
| systemd timers | `~/.config/systemd/user/televault-<name>.{timer,service}` |

On Windows: config uses `%APPDATA%`, data uses `%LOCALAPPDATA%`. On other Unix: respects `XDG_CONFIG_HOME` and `XDG_DATA_HOME`.

### Config Schema (`config.json`)

```json
{
  "channel_id": -1001234567890,
  "chunk_size": 104857600,
  "compression": true,
  "encryption": true,
  "parallel_uploads": 3,
  "parallel_downloads": 5,
  "max_retries": 3,
  "retry_delay": 1.0
}
```