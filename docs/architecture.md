# TeleVault Architecture (C++23 Native)

## 1. System Overview

TeleVault provides high-performance, unlimited cloud storage by using a private Telegram channel as an encrypted backend store, accessed via MTProto through the official Telegram Database Library (TDLib). 

There is no local database — all persistent state is maintained directly on Telegram as pinned index messages and message reply chains within the channel. Files are compressed with Zstandard, hashed with Blake3, and encrypted with AES-256-GCM on the client before upload, ensuring plaintext never leaves the user's control.

The CLI tool compiles to a single, high-performance native binary `televault` with a convenient `tvt` symlink:

```bash
televault <command> [options]
tvt <command> [options]
```

### Module Architecture

The C++ codebase is organized into a modular static library (`tv_core`) and the application layer (`tv_app` / `televault`):

| Component / Header | Implementation | Responsibility |
|---|---|---|
| `tv::TeleVault` (`core/vault.hpp`) | `src/core/vault.cpp` | Core orchestrator for `push`, `pull`, `cat`, `ls`, `info`, `stat`, `rm`, `verify`, `recover`, and chunk-0 streaming |
| `tv::TelegramClient` (`telegram/client.hpp`) | `src/telegram/client.cpp` | Native TDLib wrapper: asynchronous event loop, MTProto message I/O, queries, file streaming, and authentication |
| `tv::AuthFlow` (`telegram/auth.hpp`) | `src/telegram/auth.cpp` | Authentication lifecycle: phone code, 2FA password, and terminal visual QR-code login |
| `tv::BackupEngine` (`backup/engine.hpp`) | `src/backup/engine.cpp` | Isolated snapshot backup engine: `create`, `list`, `restore`, `prune`, `delete` |
| `tv::PreviewEngine` (`preview/preview.hpp`) | `src/preview/preview.cpp` | Instant sub-second chunk-0 terminal previewing, syntax formatting, and MIME classification |
| `tv::FileWatcher` (`watcher/watcher.hpp`) | `src/watcher/watcher.cpp` | Background directory watcher with real-time change detection and auto-sync |
| `tv::Crypto` (`crypto/crypto.hpp`) | `src/crypto/crypto.cpp` | AES-256-GCM cipher, Argon2id & PBKDF2 dual-salt key derivation, random nonces |
| `tv::Compress` (`compress/compress.hpp`) | `src/compress/compress.cpp` | Zstandard streaming compression and decompression with automatic media bypass |
| `tv::Chunker` (`chunker/chunker.hpp`) | `src/chunker/chunker.cpp` | Multi-threaded file chunking, byte-offset tracking, and Blake3 integrity hashing |
| `tv::ConfigManager` (`util/config.hpp`) | `src/util/config.cpp` | Thread-safe XDG config persistence (`~/.config/televault/config.json`) |
| `tv::RetryPolicy` (`util/retry.hpp`) | `src/util/retry.cpp` | Exponential backoff with full jitter for Telegram network operations |

---

## 2. Storage Model & Channel Topology

A single private Telegram channel holds all vault data. The channel contains a single pinned master index message and content messages organized in reply chains:

```
Private Storage Channel
 ├── [PINNED] VaultIndex Message (JSON: file_id -> metadata_message_id)
 │
 ├── FileMetadata Message 1 (JSON: name, size, hash, chunk list, etc.)
 │    ├── Chunk 0 Document (File attachment replying to FileMetadata 1)
 │    ├── Chunk 1 Document (File attachment replying to FileMetadata 1)
 │    └── Chunk 2 Document (File attachment replying to FileMetadata 1)
 │
 ├── FileMetadata Message 2 (JSON)
 │    └── Chunk 0 Document (File attachment replying to FileMetadata 2)
 │
 └── SnapshotIndex Message (JSON: snapshot_id -> snapshot_metadata_message_id)
      ├── Snapshot Message 1 (JSON: files, paths, hashes, sizes)
      └── Snapshot Message 2 (JSON: files, paths, hashes, sizes)
```

### Safety Rule: Strict Index Isolation
- **The pinned message is exclusively reserved for the primary `VaultIndex` (file listing).**
- Snapshots and backups are decoupled into dedicated message trees tracked by `snapshot_index_msg_id` in `config.json` and channel discovery (`type == "snapshot_index"`).
- This isolation guarantees that creating, listing, restoring, or pruning snapshots will **never** overwrite or corrupt the user's primary file vault index.

### Message Formats

#### FileMetadata (JSON text message)
```json
{
  "id": "210b74c03e8985de",
  "name": "archive.tar.gz",
  "size": 104857600,
  "hash": "c8b5136b7c...64 chars of BLAKE3",
  "chunks": [
    {
      "index": 0,
      "message_id": 809500672,
      "file_id": 1236,
      "size": 33554432,
      "offset": 0,
      "hash": "95766a60e1... (post-encryption ciphertext)",
      "original_hash": "fb67b363aa... (pre-encryption plaintext)"
    }
  ],
  "encrypted": true,
  "compressed": true,
  "created_at": 1788723380.184,
  "modified_at": 1788723380.184,
  "message_id": 808452096
}
```

#### VaultIndex (Pinned JSON text message)
```json
{
  "version": 49,
  "files": {
    "ef3c0f853fb8": 729808896,
    "210b74c03e8985de": 808452096
  },
  "updated_at": 1788723380.184
}
```

---

## 3. Data Integrity & Crash Safety

### Atomic Write & Temporary Swap on Download (`tvt pull`)
1. Destination directory verified and checked for write permissions.
2. Download streams to a sibling temporary file: `output.partial.XXXXXX`.
3. Each chunk is verified against `ci.hash` (Blake3 of ciphertext) upon download.
4. Chunk is decrypted, decompressed, and verified against `ci.original_hash` (Blake3 of plaintext).
5. Once all chunks are written, the full file is verified against `FileMetadata.hash`.
6. Atomic file swap (`std::filesystem::rename`) places the file into the destination path. If cross-filesystem, safe copy-then-delete is executed.

### Disaster Recovery & Self-Healing (`tvt recover`)
If the local cache or the pinned message pointer is ever lost:
- `tvt recover` performs a channel history traversal.
- Locates all valid `FileMetadata` messages by JSON signature.
- Rebuilds the `VaultIndex` mapping in memory and re-pins the newly consolidated index message on the channel.

---

## 4. Cryptographic Pipeline

```
Original Plaintext File
     │
     ▼
Multi-Threaded Chunker (Default: 100 MB slices; Low-resource: 32 MB)
     │
     ▼
Blake3 Plaintext Hash (ci.original_hash)
     │
     ▼
Zstandard Compression (level 3) -- auto-bypassed for incompressible media
     │
     ▼
OpenSSL 3 AES-256-GCM Encryption
  ├── Key Derivation: Argon2id / PBKDF2 with dual-salt fallback
  ├── Random 12-byte Nonce per chunk
  └── 16-byte Authentication Tag
     │
     ▼
Blake3 Ciphertext Hash (ci.hash)
     │
     ▼
Telegram Document Upload (via TDLib MTProto, replying to FileMetadata)
```

### Dual-Salt Backward Compatibility
To guarantee that files uploaded by different versions and configurations decrypt seamlessly:
1. First attempt: Standard 16-byte random salt stored in the chunk payload header.
2. Fallback attempt: Deterministic channel salt (`"televault" + channel_id`).
3. Verification: Decrypted plaintext is verified against `original_hash` using Blake3 to immediately detect password or KDF mismatch.

---

## 5. Instant Chunk-0 Previews (`tvt preview`)

Instead of pulling multi-gigabyte files to preview content:
- `tvt preview <path>` fetches only chunk 0 (`ci.index == 0`).
- Decrypts and decompresses the leading chunk in-memory or in an ephemeral temp file.
- Detects MIME type via magic bytes and extension mapping (~90 known types).
- Renders formatted syntax or text preview directly to the terminal in < 1 second.

---

## 6. Directory Watcher (`tvt watch`)

- Uses inotify/event-driven directory monitoring via `tv::FileWatcher`.
- Detects created, modified, or moved files with exclusion pattern filtering.
- Automatically pushes modified files to the Telegram vault.
- Controlled by an atomic stop flag and clean `SIGINT` / `SIGTERM` signal handlers to ensure zero corrupted uploads or orphan state on interruption.

---

## 7. Multi-Architecture & Cross-Platform Support

TeleVault's core engine is written in portable standard ISO C++23:
- **Zero Architecture-Specific Assembly**: Avoids unportable inline assembly; leverages compiler intrinsics and vector extensions where supported.
- **Endian Independence**: Network and index protocols enforce explicit little-endian byte ordering for chunk headers and Blake3 / AES authentication primitives.
- **Target Architectures**:
  - `x86_64` (`amd64`): Modern Intel and AMD 64-bit platforms.
  - `aarch64` (`arm64`): 64-bit ARM platforms including Apple Silicon (via Linux), Raspberry Pi 4/5, and cloud ARM servers (AWS Graviton, Ampere Altra).
- **Automated Multi-Arch CI/CD**: GitHub Actions workflow builds and packages native binaries for both `x86_64` and `aarch64` on native runner hardware.

