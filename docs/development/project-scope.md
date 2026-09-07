# Project: TeleVault C++23 Hardening, Parity Audit & Verification

## Architecture
TeleVault is a secure cloud storage bridge leveraging Telegram channels as a block storage backend with client-side encryption and compression.
- **Core Engine & Crypto**: `tv_core` provides AES-256-GCM encryption, ZSTD compression, BLAKE3 chunking, and JSON serialization.
- **Network & Storage Layer**: `tv_app` interfaces with TDLib for telegram transport, managing chunk uploads/downloads, channel-pinned index messages, snapshot backups, and garbage collection.
- **Frontends & Interfaces**: CLI11 command-line interface, FUSE3 virtual filesystem mount, WebDAV HTTP server, and inotify-based file watcher.
- **Build & Hardening**: CMake build system compiling with C++23, hardened with PIE, Full RELRO (`BIND_NOW`), stack canaries, NX stack, and FORTIFY_SOURCE.

## Feature Inventory
| # | Feature | Description | Milestone | Source |
|---|---------|-------------|-----------|--------|
| 1 | Build Security Hardening | Add `-fstack-protector-strong`, `-D_FORTIFY_SOURCE=2`, `-fPIE`, `-pie`, `-Wl,-z,relro,-z,now`, `-Wl,-z,noexecstack` in CMakeLists.txt | M1 | Survey 3 |
| 2 | Sanitizer Integration | Add `TV_ENABLE_SANITIZERS` supporting `-fsanitize=address,undefined -fno-sanitize=vptr` in CMake | M1 | Survey 3 |
| 3 | Async Executor Build Target | Add `src/async/executor.cpp` to CMake build targets | M1 | Survey 3 |
| 4 | Cryptographic Wire Format Parity | Implement `EncryptionHeader` (16-byte salt, 12-byte nonce, 16-byte tag = 44-byte overhead) and support dual wire formats | M2 | Survey 1 |
| 5 | Streaming Encryption / Decryption | Complete `StreamingEncryptor` and `StreamingDecryptor` implementation with counter-based nonce derivation | M2 | Survey 1 |
| 6 | Crypto RAII & Memory Safety | RAII wrappers for OpenSSL `EVP_CIPHER_CTX`, safe integer casting, eliminating const_cast | M2 | Survey 1 |
| 7 | Compression Parity & Robustness | Update extension blacklist, fix `estimate_compressed_size`, handle unknown frame content size, fix decompressor flush | M2 | Survey 1 |
| 8 | BLAKE3 Hash Parity | Support 32-character hex prefix matching Python reference alongside 64-char full hashes | M2 | Survey 1 |
| 9 | Chunker Memory Bounding | Stream chunks in `iter_chunks` without buffering entire multi-GB files in RAM vectors | M2 | Survey 1 |
| 10 | ChunkWriter Hardening | Create parent directories, handle chunk retries/duplicates, add completion checks | M2 | Survey 1 |
| 11 | Models Schema & Deserialization | Snapshot JSON parity (path vs name, optional fields), float timestamp parsing in FileMetadata | M2 | Survey 1, 2 |
| 12 | Telegram Message 4096-char Limit | Implement `_compress_message` (`__TV1__` zlib+base64) for large index/metadata messages | M3 | Survey 1, 2 |
| 13 | Telegram Client Concurrency & Fixes | Fix `download_file` completion wait, fix login infinite loop, protect `file_progress_cb_` against data races, safe `stop()` | M3 | Survey 2 |
| 14 | Vault Engine Memory & 0-Byte Fix | Fix 0-byte file crash in pull, fix `tellg()` -1 bad_alloc on missing file, reduce memory footprint during push | M3 | Survey 1, 2 |
| 15 | Backup Index Protection | Prevent `save_index` from overwriting pinned `VaultIndex` (distinguish message types or store dedicated message ID) | M3 | Survey 1 |
| 16 | Incremental Backup & Restore | Check hash + size for incremental skipping; download all required snapshot files during restore; fix relative paths | M3 | Survey 1 |
| 17 | Garbage Collection & Pruning | Implement `collect_garbage` and `cleanup_partial_uploads` with real TDLib scanning and message deletion | M3 | Survey 1 |
| 18 | File Watcher Hardening | Replace substring search with glob pattern matching, synchronize state access with mutex, connect to vault upload | M3 | Survey 2 |
| 19 | CLI Flag & Option Parity | Add `--password/-p`, `--no-compress`, `--no-encrypt`, `--name`, stdin `-` to push/pull; fix directory recursion crash | M4 | Survey 2 |
| 20 | CLI Subcommands & Debug Logging | Fix debug log initialization order in `main.cpp`; wire subcommands (`backup`, `gc`, `watch`, `preview`, `mount`, `serve`) | M4 | Survey 2 |
| 21 | Progress & Speed Tracking | Implement `SpeedTracker` and `ProgressBar` in `src/cli/progress.cpp` | M4 | Survey 2 |
| 22 | FUSE LRU Cache & Mount Safety | Fix `LRUChunkCache` eviction order (true LRU) and duplicate key corruption; harden FUSE stubs | M4 | Survey 2 |
| 23 | WebDAV & Preview Modules | Implement WebDAV HTTP handlers and verify MIME/preview extraction logic | M4 | Survey 2 |
| 24 | Unit Test Suite Expansion | Add test suites for models/snapshot, preview, fuse cache, backup, and watcher | M5 | Survey 3 |
| 25 | ASan / UBSan Dynamic Sanitization | Verify 100% test execution and binary execution with zero memory errors or leaks under AddressSanitizer and UBSan | M5 | Survey 3 |
| 26 | Binary Security Verification | Verify `rabin2 -I` flags (canary: true, nx: true, pic: true, relro: full) and verify no banned libc functions in critical paths | M5 | Survey 3 |
| 27 | Clean Git Commit | Ensure all audited and verified changes are committed cleanly to branch `needspeed` | M5 | User Request |

## Milestones
| # | Name | Scope | Dependencies | Status |
|---|------|-------|-------------|--------|
| 1 | M1: Build System Hardening & Sanitizers | CMakeLists.txt, compiler/linker flags, sanitizer configuration, async target, stress tests | none | DONE |
| 2 | M2: Crypto, Compression, Chunker & Models Parity | src/crypto, src/compress, src/chunker, src/models, tests | M1 | DONE |
| 3 | M3: Core Vault, Telegram Client, Concurrency & Storage | src/core, src/telegram, src/backup, src/gc, src/watcher, src/async | M2 | DONE |
| 4 | M4: CLI Completeness, Progress & Filesystem Subsystems | src/cli, src/main.cpp, src/fuse, src/webdav, src/preview | M3 | DONE |
| 5 | M5: Verification, Sanitizer Testing & Binary radare2 Audit | tests, ASan/UBSan, radare2 binary inspection, git commit | M4 | DONE |

## Interface Contracts
### `crypto` ↔ `core::vault`
- `encrypt_chunk(span<const uint8_t> data, span<const uint8_t> key, span<const uint8_t> salt)`: outputs `[salt:16][nonce:12][ciphertext][tag:16]`.
- `decrypt_chunk(span<const uint8_t> ciphertext, span<const uint8_t> key)`: parses header, derives key or verifies tag, returns plaintext. Supports both 44-byte salt-bearing chunks and 28-byte legacy chunks for backward compatibility.
- `StreamingEncryptor` / `StreamingDecryptor`: `process(span<const uint8_t> block)` returns transformed chunk without throwing.

### `telegram` ↔ `core::vault`
- `TelegramClient::send_message_compressed(int64_t chat_id, const std::string& text)`: compresses with zlib and base64 prefix `__TV1__` if text exceeds 3000 chars or requires compaction.
- `TelegramClient::download_file(int32_t file_id, ProgressCallback cb)`: blocks or asynchronously waits until `is_downloading_completed_ == true` before returning success.
- `TelegramClient::stop()`: cleanly joins client thread and invalidates references without SIGSEGV.

### `backup` ↔ `telegram`
- `BackupEngine::save_index`: saves snapshot index to dedicated message without modifying or replacing the pinned `VaultIndex`.

## Code Layout
- `src/core/`: `vault.cpp`, `vault.hpp`, `index.cpp`, `transfer.cpp`, `app_context.cpp`
- `src/crypto/`: `aes256gcm.cpp`, `kdf.cpp`, `stream.cpp`, headers
- `src/compress/`: `zstd.cpp`, `stream.cpp`, headers
- `src/chunker/`: `chunker.cpp`, `hash.cpp`, `writer.cpp`, headers
- `src/models/`: `file_metadata.hpp`, `vault_index.hpp`, `snapshot.hpp`, `config.hpp`
- `src/telegram/`: `client.cpp`, `auth.cpp`, `session.cpp`, headers
- `src/backup/`: `engine.cpp`, `snapshot.cpp`, headers
- `src/gc/`: `gc.cpp`, `gc.hpp`
- `src/watcher/`: `watcher.cpp`, `watcher.hpp`
- `src/cli/`: `cli.cpp`, `progress.cpp`, `progress.hpp`, headers
- `src/fuse/`: `fuse_ops.cpp`, `cache.cpp`, headers
- `src/webdav/`: `server.cpp`, `handler.cpp`, headers
- `src/preview/`: `preview.cpp`, `preview.hpp`
- `tests/`: Unit and integration test suites
