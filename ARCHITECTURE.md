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
| `telegram.py` | `TelegramVault` -- MTProto client wrapper, index management, message I/O, channel ops, message compression |
| `models.py` | Data models: `FileMetadata`, `ChunkInfo`, `VaultIndex`, `TransferProgress` |
| `chunker.py` | File splitting/merging, `ChunkWriter`, BLAKE3 hashing |
| `crypto.py` | AES-256-GCM encryption, scrypt KDF, streaming encryptor/decryptor |
| `compress.py` | Zstandard compression/decompression, extension-based skip logic |
| `config.py` | `Config` dataclass, config directory resolution, atomic persistence |
| `retry.py` | Exponential backoff with jitter, FloodWait handling |
| `backup.py` | `BackupEngine` -- snapshot create/restore/list/delete/prune/verify |
| `snapshot.py` | `Snapshot`, `SnapshotFile`, `SnapshotIndex`, `RetentionPolicy` |
| `fuse.py` | `TeleVaultFuse` -- FUSE driver with on-demand chunk streaming and LRU cache |
| `webdav.py` | `WebDAVHandler` + `WebDAVServer` -- HTTP/WebDAV access |
| `preview.py` | `PreviewEngine` -- terminal previews from first 1-2 chunks |
| `watcher.py` | `FileWatcher` -- polling-based directory monitor with BLAKE2 hashing |
| `schedule.py` | Schedule CRUD, systemd timer generation, cron entry generation |
| `gc.py` | Orphan message detection and cleanup (dry-run default, pinned message protection) |
| `logging.py` | `RotatingFileHandler` setup, console + file output |
| `tui.py` | Textual-based interactive terminal UI (file browser, auth-aware loading) |

---

## Storage Model

A single private Telegram channel holds every piece of TeleVault data. The channel contains pinned index messages and content messages organized in reply chains:

```
Channel
 +-- Pinned: VaultIndex (file_id -> metadata_message_id)
 +-- Pinned: SnapshotIndex (snapshot_id -> message_id)
 +-- Text messages: FileMetadata (JSON, possibly compressed)
 +-- File messages: chunk data (replying to their FileMetadata message)
 +-- Text messages: Snapshot (JSON, possibly compressed)
```

### Message Compression

Telegram limits text messages to 4096 characters. TeleVault automatically compresses messages that exceed this limit using zlib + base64 encoding with a `__TV1__` prefix:

```python
# Compression (on send):
if len(json_text) > 4096:
    compressed = zlib.compress(json_text.encode("utf-8"), 9)
    encoded = base64.b64encode(compressed).decode("ascii")
    message = f"__TV1__{encoded}"

# Decompression (on read):
if message.startswith("__TV1__"):
    encoded = message[len("__TV1__"):]
    compressed = base64.b64decode(encoded)
    json_text = zlib.decompress(compressed).decode("utf-8")
```

This applies to all text messages: `VaultIndex`, `FileMetadata`, `Snapshot`, and `SnapshotIndex`. Backward compatible -- uncompressed messages are read as-is.

### Index Lookup Optimization

Earlier versions scanned only the last 10 channel messages to find the index, which caused data loss when many chunks were uploaded. The current implementation uses a two-tier caching strategy:

1. **In-memory cache** (`TelegramVault._index_msg_id`) for repeated lookups within a session
2. **Config persistence** (`Config.index_msg_id`) for cross-session fast lookup
3. **Full channel scan** (`limit=None`) as fallback only when no cached ID exists

The `save_index` method also persists the message ID via `_save_index_msg_id()` after creating or editing the pinned index message. Same pattern applies to `snapshot_index_msg_id`.

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

### FileMetadata (stored as JSON text message, auto-compressed if >4096 chars)

```json
{
  "id": "abc123def456",
  "name": "photo.jpg",
  "size": 5242880,
  "hash": "a1b2c3d4...64 chars of BLAKE3",
  "chunks": [
    {
      "index": 0,
      "message_id": 43,
      "size": 10485780,
      "hash": "e5f6a7b8...64 chars of BLAKE3 (post-processing)",
      "original_hash": "c9d0e1f2...64 chars of BLAKE3 (pre-processing)"
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

### VaultIndex (stored as pinned text message, auto-compressed if >4096 chars)

```json
{
  "version": 7,
  "files": { "abc123def456": 42, "def789ghi012": 87 },
  "updated_at": 1700000100.0
}
```

The `version` field is incremented on each save. The `save_index` method finds the pinned message, reads its version, increments it, and edits the message in place. Retries only on Telegram API errors (3 attempts with backoff), not on phantom version conflicts.

---

## Crash Safety

### Upload

1. Upload metadata message -> get `metadata_msg_id`
2. Upload all chunks in parallel (3 concurrent by default)
3. On chunk upload failure: delete all uploaded chunks + metadata (cleanup)
4. Update metadata with chunk info
5. **Save index with 3 retries** -- file data is already safe on Telegram even if index save fails

If the process crashes after step 2 but before step 5, the file data exists on Telegram but is not in the index. Running `tvt gc --clean-partials` will detect and clean up incomplete uploads, or re-pushing the file will add it to the index.

### Delete

1. **Remove file from index first** (crash safety: if interrupted, the file is no longer referenced but data still exists on Telegram)
2. Read metadata to collect chunk message IDs
3. Delete all messages (metadata + chunks) -- errors are suppressed since the index already no longer references them

If the process crashes after step 1 but before step 3, `tvt gc` will find the orphaned messages and clean them up.

### Stream Upload (`tvt push -`)

The `upload_stream` method writes piped data to a temp file, then delegates to `upload()` with the `name` parameter set to the desired filename. This is atomic -- the index is saved once with the correct filename, avoiding a previous bug where a second index save could fail and leave the index referencing a temp filename.

### Resumable Download

`download_resume` saves progress after each chunk to a `.progress` file with CRC32 integrity checking. On chunk hash mismatch, the download fails immediately but **partial progress is preserved** -- the user can retry and resume from the last successful chunk. The partial file (`.partial`) and progress file are only cleaned up on successful completion.

### Concurrent Index Access

All index read-modify-write operations in `TeleVault` are serialized with `asyncio.Lock` (`self._index_lock`). This prevents concurrent uploads from silently overwriting each other's file entries in the index.

### Garbage Collection

`tvt gc` is **dry-run by default**. Use `--force` to actually delete messages.

Protection rules:
- **Pinned messages are never deleted** -- the vault index and snapshot index are always preserved
- Only messages not referenced by any file in the index are considered orphans
- `--clean-partials` detects and removes incomplete uploads (files in index with missing chunks)

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

## Chunk Management

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

tvt gc                             Garbage collection (dry-run by default)
  --force                          Actually delete orphaned messages
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
  --foreground / --background      Foreground or daemon mode

tvt serve                          Start WebDAV server
  --host / -h <addr>               Bind address (default 0.0.0.0)
  --port / -p <port>               Port (default 8080)
  --password / -P                  Encryption password
  --read-only                      Read-only mode
  --cache-dir                      Local cache directory

tvt whoami                         Show current Telegram account
tvt logout                         Clear session
tvt tui                            Launch interactive TUI

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
tvt backup delete <snapshot_id>     Delete snapshot
tvt backup prune                   Prune old snapshots
  --keep-daily / --keep-weekly / --keep-monthly
  --dry-run
tvt backup verify <snapshot_id>     Verify snapshot integrity

tvt schedule create <path>          Create backup schedule
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

## TUI

The TUI (`tvt tui`) is a Textual-based file browser. It does **not** handle authentication or channel setup -- users must run `tvt login` and `tvt setup` via the CLI first.

Startup flow:
1. `compose()` renders the main screen immediately
2. `on_mount` starts an async auth check via `asyncio.create_task`
3. Status label shows connection state ("Connecting...", auth errors)
4. If no credentials/session: shows "Run: tvt login" instructions
5. If no channel: shows "Run: tvt setup" instructions
6. If authenticated: dynamically mounts search input, file table, and status bar
7. Reuses a single `TeleVault` connection instead of reconnecting per operation

The file browser shows: file list (ID, Name, Size, Chunks, Encrypted), detail panel (metadata preview), sidebar (stats, actions).

Key bindings: `q` quit, `r` refresh, `u` upload, `d` download, `s` search, `p` preview, `Delete` delete.

Terminal cleanup: `_cleanup_terminal()` ensures the terminal is always restored, even on crashes.

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

### Index Save Retries

Index saves retry up to 3 times with backoff (0.5s, 1.0s, 1.5s). Only Telegram API errors trigger retries. File data is already safe on Telegram before index save is attempted.

### Sequential Index Access

`asyncio.Lock` (`TeleVault._index_lock`) serializes all index read-modify-write operations in `upload()`, `upload_resume()`, and `upload_stream()`. This prevents concurrent uploads from silently overwriting each other's entries.

### Login Error Handling

`login()` catches only `SessionPasswordNeededError` for 2FA flow. Other sign-in errors propagate as-is -- no broad `except Exception` swallowing auth failures.

### Garbage Collection

`tvt gc` scans for orphaned messages. Pinned messages (index, snapshot_index) are always protected from deletion. Default is dry-run mode; use `--force` to actually delete. `tvt gc --clean-partials` detects incomplete uploads.

---

## Security Model

| Threat | Mitigation |
|---|---|
| Telegram server reads data | AES-256-GCM encryption; server sees only ciphertext |
| Data corruption in transit | BLAKE3 hash at chunk and file level |
| Wrong password decryption | GCM authentication + original_hash double check |
| Concurrent index modification | `asyncio.Lock` serializes all index read-modify-write ops |
| Flood limits / rate limiting | Exponential backoff with FloodWait handling |
| Accidental data deletion by gc | Dry-run by default, pinned messages always protected |
| Large metadata exceeding Telegram limit | Automatic zlib+base64 compression with `__TV1__` prefix |
| Index lookup misses | Cached `index_msg_id` in memory + config for O(1) fetch |
| Config file corruption | Atomic writes (temp file + `os.replace` + `fsync`) |
| Resume progress corruption | CRC32 checksums on progress files, partial files preserved on failure |
| Stream upload non-atomic name change | `upload()` `name` param ensures single index save |

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
| Watcher state | `~/.local/share/televault/watcher/watcher_state.json` |
| systemd timers | `~/.config/systemd/user/televault-<name>.{timer,service}` |

On Windows: config uses `%APPDATA%`, data uses `%LOCALAPPDATA%`. On other Unix: respects `XDG_CONFIG_HOME` and `XDG_DATA_HOME`.

### Config Schema (`config.json`)

```json
{
  "channel_id": -1001234567890,
  "index_msg_id": 42,
  "snapshot_index_msg_id": 150,
  "chunk_size": 104857600,
  "compression": true,
  "encryption": true,
  "parallel_uploads": 3,
  "parallel_downloads": 5,
  "max_retries": 3,
  "retry_delay": 1.0
}
```

The `index_msg_id` and `snapshot_index_msg_id` fields cache the pinned message IDs for O(1) index lookups. They are automatically populated and maintained by the application.

### Telegram Credentials (`telegram.json`)

```json
{
  "api_id": 12345678,
  "api_hash": "your_api_hash",
  "session_string": "base64_encoded_session"
}
```

The `session_string` is a `StringSession` from Telethon that allows reconnection without re-authentication. It is stored after `tvt login` and reused by all subsequent commands.

---

## Data Safety Summary

TeleVault prioritizes data safety above all else:

1. **Index lookup by cached msg ID** -- O(1) direct fetch prevents the data loss bug where `iter_messages(limit=10)` could miss the pinned index
2. **Sequential index access** -- `asyncio.Lock` prevents concurrent uploads from overwriting each other's entries
3. **Atomic config writes** -- temp file + `os.replace` + `fsync` prevents config corruption on crash
4. **Crash-safe deletes** -- removed from index before messages are deleted
5. **Crash-safe uploads** -- file data is on Telegram before index save is attempted; 3 retries with backoff
6. **Crash-safe stream** -- single index save with correct filename, no double-save window
7. **Resume-safe downloads** -- partial files preserved on failure, CRC32-protected progress files
8. **GC safety** -- dry-run default, pinned message protection, `--force` required
9. **Message compression** -- automatic for metadata exceeding 4096 chars, backward compatible
10. **Specific error handling** -- `SessionPasswordNeededError` only for 2FA, not broad `except Exception`