# TeleVault Architecture

## System Overview

TeleVault provides unlimited cloud storage by using a private Telegram channel as a persistent data store, accessed via the MTProto protocol through Telethon. There is no local database -- all state is maintained as pinned messages and message reply graphs within the channel. File content is encrypted on the client machine before upload, ensuring the plaintext never leaves the user's control.

The CLI tool is installed as `tvt`, with `televault` and `tv` as aliases:

```
[project.scripts]
televault = "televault.cli:main"
tv        = "televault.cli:main"
```

### Module Map

| Module | Responsibility |
|---|---|
| `cli.py` | Click-based CLI, command dispatch, progress display |
| `core.py` | `TeleVault` class -- upload, download, list, search, delete, resume |
| `telegram.py` | `TelegramVault` -- MTProto client wrapper, index management, message I/O |
| `models.py` | Data models: `FileMetadata`, `ChunkInfo`, `VaultIndex`, `TransferProgress` |
| `chunker.py` | File splitting/merging, `ChunkWriter`, BLAKE3 hashing |
| `crypto.py` | AES-256-GCM encryption, scrypt KDF, streaming encryptor/decryptor |
| `compress.py` | Zstandard compression/decompression, extension-based skip logic |
| `config.py` | `Config` dataclass, config directory resolution, persistence |
| `retry.py` | Exponential backoff with jitter, FloodWait handling |
| `backup.py` | `BackupEngine` -- snapshot create/restore/list/delete/prune/verify |
| `snapshot.py` | `Snapshot`, `SnapshotFile`, `SnapshotIndex`, `RetentionPolicy` |
| `fuse.py` | `TeleVaultFuse` -- FUSE filesystem driver (requires `fusepy`) |
| `webdav.py` | `WebDAVHandler` + `WebDAVServer` -- HTTP/WebDAV access (requires `aiohttp`) |
| `watcher.py` | `FileWatcher` -- polling-based directory monitor with BLAKE2 hashing |
| `schedule.py` | Schedule CRUD, systemd timer generation, cron entry generation |
| `gc.py` | Orphan message detection and cleanup |
| `logging.py` | `RotatingFileHandler` setup, console + file output |
| `tui.py` | Textual-based interactive terminal UI |

---

## Storage Model

A single private Telegram channel holds every piece of TeleVault data. The channel contains two categories of pinned messages and two categories of content messages:

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

### SnapshotIndex (stored as pinned text message)

```json
{
  "version": 2,
  "type": "snapshot_index",
  "snapshots": { "snap01abc": 150 },
  "updated_at": 1700000200.0
}
```

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
  -- skipped for incompressible extensions (see below)
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
  -- catches wrong-password decryption that nonetheless passes GCM
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

### Chunk Layout

Each chunk is uploaded as a Telegram `send_file` message with `reply_to` set to the file's metadata message ID. This creates a reply chain that allows iterating all chunks for a given file:

```python
async for msg in client.iter_messages(channel, reply_to=metadata_msg_id):
    # msg is a chunk message
```

### Parallel Transfers

| Operation | Default Concurrency | Semaphore |
|---|---|---|
| Upload | 3 concurrent | `asyncio.Semaphore(config.parallel_uploads)` |
| Download | 5 concurrent | `asyncio.Semaphore(config.parallel_downloads)` |

### ChunkWriter

The `ChunkWriter` pre-allocates the output file with `f.truncate(total_size)` and writes each chunk at its computed offset (`chunk.index * chunk_size`). This supports out-of-order writes during parallel downloads.

### TransferProgress with CRC32

For resumable downloads, `TransferProgress` tracks completed chunk indices. Progress is saved to a `.progress` file alongside the `.partial` download file:

```
output.dat.partial       <- pre-allocated, partially written
output.dat.progress      <- CRC32-protected JSON: {operation, file_id, file_name, total_chunks, completed_chunks, started_at}
```

The CRC32 integrity check ensures corrupted progress files are discarded and the download starts fresh.

---

## Index System

### VaultIndex

- Pinned message in the channel
- Maps `file_id` -> `metadata_message_id`
- Version field for optimistic concurrency (retries up to 5 times on conflict)
- Updated atomically on every file add/remove

### SnapshotIndex

- Second pinned message in the channel (distinguished by `"type": "snapshot_index"`)
- Maps `snapshot_id` -> `message_id`
- Has its own version counter

### FileMetadata

- One per file, stored as a compact JSON text message
- Contains: id, name, size, BLAKE3 hash, list of `ChunkInfo`, encryption/compression flags, timestamps
- Chunk messages reply to this metadata message, forming a queryable thread

### Snapshot

- One per backup, stored as a compact JSON text message with `"type": "snapshot"`
- Contains: id, name, source_path, file_count, total_size, parent_id (for incremental), list of `SnapshotFile`
- Each `SnapshotFile` references a file_id already stored in the vault

### Index Conflict Resolution

When saving the index, the client:
1. Reads the current pinned VaultIndex/SnapshotIndex
2. Checks its version against the expected version
3. If versions match: increments version and edits the pinned message
4. If versions conflict: retries up to 5 times with increasing backoff (0.5s, 1.0s, ...)
5. If no index exists: creates a new pinned message

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

tvt rm <file_id_or_name>           Delete file
  --yes / -y                       Skip confirmation

tvt cat <file_id_or_name>          Output file to stdout (pipeable)

tvt preview <file_id_or_name>      File preview

tvt info <file_id_or_name>         Detailed file information

tvt stat                           Vault status/overview

tvt find <query>                   Search files by name

tvt verify <file_id_or_name>       Verify file integrity
  --password / -p                  Decryption password

tvt gc                             Garbage collection
  --dry-run                        List orphans without deleting
  --clean-partials                 Remove incomplete uploads

tvt mount <mount_point>            Mount as FUSE filesystem
  --password / -p                  Encryption password
  --read-only                      Read-only mount
  --cache-dir                      Local cache directory
  --allow-other                     Allow other users
  --foreground / --background     Foreground or daemon mode

tvt serve                          Start WebDAV server
  --host / -h <addr>               Bind address (default 0.0.0.0)
  --port / -p <port>               Port (default 8080)
  --password / -P                  Encryption password
  --read-only                      Read-only mode
  --cache-dir                      Local cache directory

tvt watch                          Watch directories for changes
  --path / -p <dir>                Directory to watch (multiple)
  --password / -P                  Encryption password
  --interval <seconds>             Poll interval (default 5.0)
  --exclude / -e <pattern>         Exclude patterns

tvt backup create <path>           Create backup snapshot
  --name / -n                      Snapshot name
  --password / -p                  Encryption password
  --incremental / -i               Incremental backup
  --parent <id>                    Parent snapshot ID
  --dry-run                        Show plan without uploading

tvt backup restore <snapshot_id>   Restore from snapshot
  --output / -o                    Output directory
  --password / -p                  Decryption password
  --files / -f                     Specific files to restore

tvt backup list                    List snapshots
tvt backup delete <snapshot_id>    Delete snapshot
tvt backup prune                   Prune old snapshots
  --keep-daily <N>                 Keep N daily (default 7)
  --keep-weekly <N>                Keep N weekly (default 4)
  --keep-monthly <N>               Keep N monthly (default 6)
  --dry-run                        Show what would be pruned
tvt backup verify <snapshot_id>    Verify snapshot integrity

tvt schedule create <path>         Create backup schedule
  --name / -n                      Schedule name
  --interval <hourly|daily|weekly|monthly>
  --password / -p                  Encryption password
  --incremental                    Incremental backups

tvt schedule list                  List schedules
tvt schedule run <name>            Run a scheduled backup
tvt schedule delete <name>         Delete schedule
tvt schedule install <name>        Install as systemd timer (Linux)
tvt schedule uninstall <name>      Remove systemd timer
tvt schedule show-systemd <name>  Show systemd unit files

tvt login                          Telegram authentication
  --phone / -p <number>            Phone number

tvt setup                          Set up storage channel
  --channel-id / -c <id>           Use existing channel
  --auto-create                    Create without prompting

tvt logout                         Clear session
tvt whoami                         Show current Telegram user
tvt tui                            Launch interactive TUI

tvt completion                     Shell completion
```

### Global Flags

| Flag | Effect |
|---|---|
| `-v` / `--verbose` | INFO-level logging |
| `--debug` | DEBUG-level logging |
| `--json` | Machine-readable output (on `ls`, `stat`, `info`, `find`) |

### Pipeable Commands

- `tvt push -` reads from stdin
- `tvt pull -o -` writes to stdout
- `tvt cat <file>` writes to stdout
- `tvt ls --json` outputs JSON array

---

## Virtual Drive Architecture

### FUSE Mount

The `TeleVaultFuse` class implements `fusepy.Operations` and maps vault files onto a local mount point.

Key behaviors:
- **Read path**: On `open()`, downloads the file from Telegram to `~/.local/share/televault/fuse_cache/` and serves subsequent `read()` calls from the local cache.
- **Write path**: On `flush()`, uploads the buffered data to the vault and updates the in-memory index.
- **Index refresh**: Refreshes the file listing on every `getattr()`/`readdir()` call, with a 2-second debounce to avoid excessive API calls.
- **Cache location**: `~/.local/share/televault/fuse_cache/`
- **Read-only mode**: Supports mounting as read-only, returning `FuseOSError(30)` for write attempts.
- **StatFS**: Reports a virtual 4 TB filesystem.

### WebDAV Server

The `WebDAVHandler` + `WebDAVServer` classes provide WebDAV access using `aiohttp`. Supports the following HTTP methods:

| Method | WebDAV Method | Behavior |
|---|---|---|
| `GET` | File download + HTML directory listing | Downloads file to cache, serves content |
| `HEAD` | File metadata | Returns content-type and content-length |
| `PUT` | File upload | Writes to local cache then uploads to vault |
| `DELETE` | File deletion | Deletes file from vault and local cache |
| `PROPFIND` | Directory listing | Returns XML multistatus response |
| `PROPPATCH` | No-op | Returns 200 |
| `OPTIONS` | Capabilities | Returns `DAV: 1, 2` header |
| `LOCK` | Advisory lock | Returns lock token |
| `MKCOL` | Not allowed | Returns 405 |

Index refresh has a 5-second debounce. Cache location defaults to `~/.local/share/televault/webdav_cache/`.

### Streaming FUSE (Planned)

On-demand chunk fetching with LRU cache is planned but not yet implemented. The current FUSE implementation always downloads the entire file before serving reads.

---

## Auto-Backup System

### FileWatcher (`watcher.py`)

A polling-based filesystem monitor that detects changed files and uploads them automatically.

Key characteristics:
- **Polling**: Scans watched directories at a configurable interval (default 5 seconds)
- **Change detection**: Uses BLAKE2b (16-byte digest) file hashing to detect modifications
- **Exclusion patterns**: `.git`, `__pycache__`, `.DS_Store`, `*.pyc`, `*.partial`, `*.tmp`, etc.
- **Initial scan**: On startup, treats all new files as changed and uploads them
- **State persistence**: Saves file hashes and watched directories to `watcher_state.json`

### ScheduleConfig

Stored as individual JSON files in `~/.config/televault/schedules/<name>.json`:

```json
{
  "name": "daily-docs",
  "path": "/home/user/documents",
  "interval": "daily",
  "enabled": true,
  "incremental": false,
  "password": null,
  "last_run": 1700000000.0,
  "last_status": "success"
}
```

### systemd Timer Integration

Installing a schedule creates two files in `~/.config/systemd/user/`:

**`televault-<name>.timer`:**
```ini
[Unit]
Description=TeleVault backup: <name>
After=network-online.target
Wants=network-online.target

[Timer]
OnCalendar=Daily
Persistent=true
RandomizedDelaySec=300

[Install]
WantedBy=timers.target
```

**`televault-<name>.service`:**
```ini
[Unit]
Description=TeleVault backup: <name>
After=network-online.target
Wants=network-online.target

[Service]
Type=oneshot
ExecStart=televault backup create "<path>" --name "<name>" [--incremental]
```

After writing the unit files, runs `systemctl --user daemon-reload`, `enable`, and `start`.

### Cron Entry Generation

For non-Linux systems, `generate_cron_entry()` produces:

| Interval | Cron Expression |
|---|---|
| hourly | `0 * * * *` |
| daily | `0 2 * * *` |
| weekly | `0 2 * * 0` |
| monthly | `0 2 1 * *` |

---

## Retry and Reliability

### Exponential Backoff

```
delay = min(base_delay * 2^attempt, max_delay)
delay = delay * (0.5 + random())   # jitter
```

Default parameters from `Config`:
- `max_retries = 3`
- `retry_delay = 1.0` (base_delay)
- `max_delay = 60.0` seconds

### FloodWaitError Handling

Telegram's `FloodWaitError` includes a `seconds` attribute. The retry logic:
- Uses the server-suggested wait time if it exceeds the calculated backoff delay
- Caps retries at 3 attempts for flood waits
- Raises immediately if `seconds > 300` (5 minutes)

### Atomic Index Saves

`save_index()` uses version-based optimistic concurrency:
1. Read current pinned index, note its version
2. Increment version and edit the pinned message
3. On conflict (version mismatch), retry up to 5 times with `0.5 * (attempt + 1)` second delay

### Upload Cleanup on Failure

If an `asyncio.gather()` of chunk uploads fails:
- All uploaded chunk messages and the metadata message are deleted from the channel
- The index is not updated, so no orphaned references remain

### CRC32-Protected Progress Files

Resumable downloads write progress to a `.progress` file formatted as:

```
<CRC32 hex 8 chars>
<JSON TransferProgress>
```

On resume, the CRC32 is recomputed and compared. If it does not match, the progress file is discarded and the download restarts.

### Garbage Collection

The `gc` module provides two operations:

1. **`collect_garbage()`**: Scans all messages in the channel and identifies any message whose ID is not referenced by the VaultIndex (orphaned chunks, stale metadata). Reports orphaned size and optionally deletes them in batches of 100.

2. **`cleanup_partial_uploads()`**: Iterates through all files in the index and removes any where `is_complete()` returns false (metadata exists but not all chunks are present).

---

## Security Model

### Encryption

- **Algorithm**: AES-256-GCM (Authenticated Encryption with Associated Data)
- **Key Derivation**: scrypt with parameters `N=2^17, r=8, p=1`, output length 32 bytes
- **Per-chunk randomness**: Each chunk gets a fresh 16-byte salt and 12-byte nonce
- **Authentication**: GCM produces a 16-byte authentication tag, providing integrity verification
- **Password isolation**: The password never leaves the client machine. Only derived keys interact with the encrypted data.

### Integrity Verification

```
BLAKE3(original plaintext)  ->  ChunkInfo.original_hash
BLAKE3(encrypted ciphertext) ->  ChunkInfo.hash
BLAKE3(full file plaintext)  ->  FileMetadata.hash
```

The `original_hash` field in `ChunkInfo` serves a dual purpose:
1. Verifies chunk integrity before re-encryption
2. Detects wrong-password decryption (even if GCM tag verification passes erroneously, the plaintext hash will not match)

### Threat Model

| Threat | Mitigation |
|---|---|
| Telegram server reads data | AES-256-GCM encryption; server only sees ciphertext |
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

### Telegram Credentials (`telegram.json`)

```json
{
  "api_id": 12345,
  "api_hash": "your_api_hash_here",
  "session_string": "base64_encoded_session"
}
```

Credentials are resolved in order: environment variables `TELEGRAM_API_ID` / `TELEGRAM_API_HASH`, then the config file.

### Logging

`RotatingFileHandler` with:
- Maximum file size: 10 MB
- Backup count: 3
- Log format: `%(asctime)s [%(levelname)s] %(name)s:%(funcName)s:%(lineno)d - %(message)s`
- Console: stderr at the configured level
- File: DEBUG level, always active

---

## Data Flow Diagrams

### Upload Flow

```
                        User
                         |
                     tvt push file.bin
                         |
                         v
                   +------------+
                   | core.py    |
                   | TeleVault  |
                   | .upload()  |
                   +------------+
                         |
            +------------+------------+
            |                         |
            v                         v
     +-------------+          +----------------+
     | chunker.py  |          | config.py      |
     | Split into  |          | chunk_size,    |
     | 100MB chunks|          | encryption,    |
     +-------------+          | compression    |
            |                +----------------+
            v
     Per chunk (parallel, semaphore=3):
            |
            +---> Compute original_hash (BLAKE3 of plaintext)
            |
            +---> Optional: compress_data() [zstd level 3]
            |         if should_compress(filename)
            |
            +---> encrypt_chunk() [AES-256-GCM]
            |         28-byte header: salt(16) + nonce(12)
            |         ciphertext + 16-byte GCM tag
            |
            +---> Compute hash (BLAKE3 of encrypted data)
            |
            +---> telegram.upload_chunk()
            |         send_file with reply_to=metadata_msg_id
            |
            v
     Collect all ChunkInfo objects
            |
            v
     Update FileMetadata with chunks list
            |
            v
     telegram.update_metadata(metadata_msg_id, metadata)
            |
            v
     Update VaultIndex: add_file(file_id, metadata_msg_id)
            |
            v
     telegram.save_index(index)  [version-gated]
```

### Download Flow

```
                        User
                         |
                     tvt pull abc123
                         |
                         v
                   +------------+
                   | core.py    |
                   | TeleVault  |
                   | .download()|
                   +------------+
                         |
                         v
     telegram.get_index() -> find file_id -> metadata_msg_id
                         |
                         v
     telegram.get_metadata(metadata_msg_id) -> FileMetadata
                         |
                         v
     ChunkWriter(output_path, total_size, chunk_size)
         pre-allocates file with f.truncate(total_size)
                         |
                         v
     Per chunk (parallel, semaphore=5):
            |
            +---> telegram.download_chunk(message_id)
            |
            +---> Verify hash: BLAKE3(data) == ChunkInfo.hash
            |
            +---> Decrypt: decrypt_chunk(data, password)
            |         extract 28-byte header, scrypt key derivation,
            |         AES-256-GCM decrypt
            |
            +---> Decompress: decompress_data() [zstd]
            |
            +---> Verify original_hash: BLAKE3(plaintext) == ChunkInfo.original_hash
            |
            +---> writer.write_chunk(Chunk(index, data))
            |         seeks to index * chunk_size, writes in place
            |
            v
     All chunks complete
            |
            v
     Verify file-level hash: BLAKE3(output_file) == FileMetadata.hash
            |
            v
     Success -> return output_path
     Failure -> delete output_file, raise error
```

### Backup Snapshot Flow

```
     tvt backup create /data/project --name weekly
            |
            v
     BackupEngine.create_snapshot()
            |
            +---> Walk directory, collect files
            |
            +---> [incremental] Compare BLAKE3 hashes with parent snapshot
            |         Skip unchanged files
            |
            +---> Upload each new/changed file via TeleVault.upload()
            |
            +---> Build Snapshot object with SnapshotFile list
            |
            +---> Upload snapshot as JSON text message
            |
            +---> Update SnapshotIndex (pinned message)
            |
            v
     Return Snapshot with id, file_count, total_size
```