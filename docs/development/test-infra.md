# TeleVault E2E Test Infrastructure & Methodology Specification

## 1. Test Philosophy: Opaque-Box & Requirement-Driven

The TeleVault End-to-End (E2E) Test Suite is designed under strict **opaque-box (black-box)** testing principles:
- **Zero Internal Coupling**: Tests interact with TeleVault exclusively through its observable public interfaces: the compiled command-line binary (`televault`), standard input/output streams, exit codes, configuration files (`config.json`), filesystem artifacts, network protocols (HTTP/WebDAV), and binary ELF structures.
- **Requirement-Grounded Expected Outputs**: Test inputs and expected outputs are derived from authoritative sources:
  1. `ORIGINAL_REQUEST.md`: Architectural audit requirements, memory safety hardening (ASan/UBSan), and binary hardening verification (radare2 / readelf).
  2. `PROJECT.md`: Feature Inventory (27 features across 5 milestones), interface contracts, wire format specifications, and code layout.
  3. Python Reference Implementation Oracle (`televault` Python package): Used as a comparative oracle for cryptographic wire format, BLAKE3 hash prefixes, JSON schemas, and compression behavior.
  4. Standards & Specifications: RFC 5116 (AES-GCM), RFC 8439, BLAKE3 specification, RFC 4918 (WebDAV), System V AMD64 ABI ELF specifications.
- **Progressive Testability & Milestone Awareness**: Tests are tagged by milestone (M1 through M5) and feature (F01 through F27). The framework allows running tests against the current milestone's deliverables while maintaining strict regression prevention for completed capabilities.

---

## 2. Test Methodology & Design Techniques

The test framework applies four formal testing disciplines across four distinct tiers:

### Tier 1: Category-Partition Feature Coverage
- Every feature from the 27-feature inventory is isolated into its functional domain, partitioned into equivalence classes, and tested with a minimum of **5 deterministic test cases per feature** (minimum 135 tests).
- Focuses on the primary contract and happy-path operations of each feature in isolation.

### Tier 2: Boundary Value Analysis (BVA) & Adversarial Robustness
- Tests boundaries, extremes, and stress inputs with a minimum of **5 test cases per feature** (minimum 135 tests):
  - **Size Boundaries**: 0-byte files, 1-byte files, exact chunk boundary files (100MB, 32MB), 1-byte over-chunk files, multi-chunk files (500MB+).
  - **Encoding & Escaping Integrity**: Filenames and paths with Unicode characters, spaces, punctuation, null bytes, special control characters, and deep directory trees (>30 levels).
  - **Memory & Resource Limits**: Memory-bounded chunk streaming, simulated disk fullness, permission denials, and truncated inputs.
  - **Cryptographic & Wire Format Corruptions**: 1-bit tag flips, invalid nonce lengths, missing salt headers, invalid ciphertext padding, corrupted metadata JSON.
  - **Error & Exception Handling**: Nonexistent files, invalid channel IDs, unauthorized sessions, and rapid SIGINT interruptions.

### Tier 3: Pairwise (Combinatorial) Interaction Testing
- Systematically covers 2-way combinations between operational dimensions to uncover subtle cross-subsystem defects:
  - **Encryption × Compression**: (AES-GCM enabled / disabled / custom password) × (ZSTD enabled / disabled / blacklisted media types).
  - **Transfer Modes × Payload Sizes**: (Direct file path / stdin `-` / streaming pipe) × (0B / 1KB / 1MB / 50MB / 150MB).
  - **Resource Modes × Network Settings**: (Standard mode / Low-resource mode) × (Resume enabled / disabled) × (File output / stdout redirection).
  - **Directory Topologies × Snapshot Strategies**: (Flat tree / deeply nested / hidden files / symlinks) × (Full snapshot / incremental delta).
  - **Concurrency Levels × Cache Limits**: (1 worker / 4 workers / 8 workers) × (Default cache / constrained LRU cache).

### Tier 4: Real-World Application Workloads
- 5 end-to-end realistic production workflows executing full lifecycles:
  1. **Full Project Directory Backup & Snapshot Restore**: Creating a multi-level source tree, backing it up to the vault, verifying the snapshot index, restoring to an independent location, and validating recursive bit-for-bit parity.
  2. **Incremental Multi-Version Delta Snapshotting**: Ingesting version 1 -> modifying, adding, and deleting files -> taking incremental snapshot version 2 -> verifying delta efficiency (only changed chunks uploaded) -> restoring versions 1 and 2 to independent trees.
  3. **High-Throughput Streaming Pipeline**: Unix pipe streaming from stdout through `televault push - --name stream.bin` into `televault cat stream.bin` through downstream consumer, validating uninterrupted streaming and hash equivalence.
  4. **Vault Maintenance, Corruption Detection & GC Lifecycle**: Ingesting files -> deliberately introducing bit corruption into storage chunks -> executing `televault verify` -> verifying failure reporting -> deleting files via `televault rm` -> running `televault gc --force` -> verifying complete orphan reclamation.
  5. **Multi-Client & WebDAV/Preview Lifecycle**: Ingesting varied MIME assets (code, markdown, image, binary) -> executing `televault preview` for non-download inspection -> launching WebDAV server (`televault serve`) -> issuing WebDAV PROPFIND and GET requests to verify standards compliance.

---

## 3. Feature Inventory Coverage Map

Every feature defined in `PROJECT.md § Feature Inventory` is mapped to its test modules and coverage tiers:

| # | Feature Name | Milestone | Test Module | T1 Cases | T2 Cases | T3/T4 Integration |
|---|--------------|-----------|-------------|----------|----------|-------------------|
| F01 | Build Security Hardening | M1 | `tier1_features/test_f01_build_hardening.py` | >=5 | >=5 | T3 Pairwise, T4 Workload 4 |
| F02 | Sanitizer Integration | M1 | `tier1_features/test_f02_sanitizers.py` | >=5 | >=5 | T3 Pairwise, All Workloads |
| F03 | Async Executor Build Target | M1 | `tier1_features/test_f03_async_executor.py` | >=5 | >=5 | T3 Concurrency, T4 Workload 3 |
| F04 | Cryptographic Wire Format Parity | M2 | `tier1_features/test_f04_crypto_wire_format.py` | >=5 | >=5 | T3 Crypto×Compress, T4 Workload 1 |
| F05 | Streaming Encryption / Decryption | M2 | `tier1_features/test_f05_streaming_crypto.py` | >=5 | >=5 | T3 Streaming, T4 Workload 3 |
| F06 | Crypto RAII & Memory Safety | M2 | `tier1_features/test_f06_crypto_raii.py` | >=5 | >=5 | T3 Concurrency, T4 Workload 4 |
| F07 | Compression Parity & Robustness | M2 | `tier1_features/test_f07_compression.py` | >=5 | >=5 | T3 Crypto×Compress, T4 Workload 1 |
| F08 | BLAKE3 Hash Parity | M2 | `tier1_features/test_f08_blake3_hash.py` | >=5 | >=5 | T3 Transfer sizes, T4 Workload 4 |
| F09 | Chunker Memory Bounding | M2 | `tier1_features/test_f09_chunker_memory.py` | >=5 | >=5 | T3 Low-resource, T4 Workload 3 |
| F10 | ChunkWriter Hardening | M2 | `tier1_features/test_f10_chunk_writer.py` | >=5 | >=5 | T3 Resume, T4 Workload 1 |
| F11 | Models Schema & Deserialization | M2 | `tier1_features/test_f11_models_schema.py` | >=5 | >=5 | T3 JSON modes, T4 Workload 2 |
| F12 | Telegram Message 4096-char Limit | M3 | `tier1_features/test_f12_tg_msg_limit.py` | >=5 | >=5 | T3 Scalability, T4 Workload 1 |
| F13 | Telegram Client Concurrency & Fixes | M3 | `tier1_features/test_f13_tg_client_concurrency.py` | >=5 | >=5 | T3 Concurrency, T4 Workload 5 |
| F14 | Vault Engine Memory & 0-Byte Fix | M3 | `tier1_features/test_f14_vault_0byte_fix.py` | >=5 | >=5 | T3 Boundaries, T4 Workload 1 |
| F15 | Backup Index Protection | M3 | `tier1_features/test_f15_backup_index_protect.py` | >=5 | >=5 | T3 Backup types, T4 Workload 2 |
| F16 | Incremental Backup & Restore | M3 | `tier1_features/test_f16_incremental_backup.py` | >=5 | >=5 | T3 Backup types, T4 Workload 2 |
| F17 | Garbage Collection & Pruning | M3 | `tier1_features/test_f17_gc_pruning.py` | >=5 | >=5 | T3 Maintenance, T4 Workload 4 |
| F18 | File Watcher Hardening | M3 | `tier1_features/test_f18_watcher.py` | >=5 | >=5 | T3 Watcher globs, T4 Workload 5 |
| F19 | CLI Flag & Option Parity | M4 | `tier1_features/test_f19_cli_flag_parity.py` | >=5 | >=5 | T3 Flag matrix, All Workloads |
| F20 | CLI Subcommands & Debug Logging | M4 | `tier1_features/test_f20_cli_subcommands.py` | >=5 | >=5 | T3 Subcommand matrix, All Workloads |
| F21 | Progress & Speed Tracking | M4 | `tier1_features/test_f21_progress_tracking.py` | >=5 | >=5 | T3 Non-interactive, T4 Workload 3 |
| F22 | FUSE LRU Cache & Mount Safety | M4 | `tier1_features/test_f22_fuse_lru_cache.py` | >=5 | >=5 | T3 Cache capacity, T4 Workload 5 |
| F23 | WebDAV & Preview Modules | M4 | `tier1_features/test_f23_webdav_preview.py` | >=5 | >=5 | T3 MIME matrix, T4 Workload 5 |
| F24 | Unit Test Suite Expansion | M5 | `tier1_features/test_f24_unit_test_expansion.py` | >=5 | >=5 | Test validation, All Milestones |
| F25 | ASan / UBSan Dynamic Sanitization | M5 | `tier1_features/test_f25_asan_ubsan_sanitization.py` | >=5 | >=5 | Sanitizer runtime, All Workloads |
| F26 | Binary Security Verification | M5 | `tier1_features/test_f26_binary_security_radare2.py` | >=5 | >=5 | Binary audit, M1/M5 gates |
| F27 | Clean Git Commit | M5 | `tier1_features/test_f27_clean_git_commit.py` | >=5 | >=5 | Repo status, Final gate |

---

## 4. Test Architecture & Directory Layout

### Directory Layout
```
tests/e2e/
├── __init__.py
├── run_e2e.py                     # Main test runner CLI & reporter
├── harness/                       # Opaque test harness & oracles
│   ├── __init__.py
│   ├── config.py                  # Test configurations & environment setup
│   ├── binary_runner.py           # Subprocess execution of televault binary
│   ├── python_oracle.py           # Python reference implementation oracle
│   ├── crypto_oracle.py           # Independent AES-GCM / BLAKE3 verification
│   ├── binary_inspector.py        # ELF security inspector (readelf, rabin2)
│   ├── mock_storage.py            # Local mock storage / vault harness
│   ├── fixtures.py                # Isolated directory & data generators
│   └── assertions.py              # Domain assertions and error matchers
├── tier1_features/                # Tier 1: Feature Isolation (>=5 tests/feature)
│   ├── __init__.py
│   ├── test_f01_build_hardening.py
│   ├── test_f02_sanitizers.py
│   ├── ... (f03 through f27)
│   └── test_f27_clean_git_commit.py
├── tier2_boundaries/              # Tier 2: Boundary Value Analysis (>=5 tests/feature)
│   ├── __init__.py
│   ├── test_b01_to_b05_build_crypto_boundaries.py
│   ├── test_b06_to_b10_crypto_chunk_boundaries.py
│   ├── test_b11_to_b15_models_vault_boundaries.py
│   ├── test_b16_to_b20_backup_cli_boundaries.py
│   └── test_b21_to_b27_progress_fuse_git_boundaries.py
├── tier3_pairwise/                # Tier 3: Combinatorial & Pairwise Matrix
│   ├── __init__.py
│   ├── test_pairwise_crypto_compress.py
│   ├── test_pairwise_transfer_sizes.py
│   ├── test_pairwise_cli_options.py
│   ├── test_pairwise_backup_types.py
│   └── test_pairwise_concurrency_cache.py
└── tier4_workloads/               # Tier 4: Real-World Workloads
    ├── __init__.py
    ├── test_workload_1_full_project_backup_restore.py
    ├── test_workload_2_incremental_snapshots.py
    ├── test_workload_3_streaming_pipeline.py
    ├── test_workload_4_vault_corruption_and_gc.py
    └── test_workload_5_multiclient_webdav_preview.py
```

### Invocation Commands

1. **Run Full Test Suite**:
   ```bash
   python3 tests/e2e/run_e2e.py
   ```

2. **Run by Tier**:
   ```bash
   python3 tests/e2e/run_e2e.py --tier 1
   python3 tests/e2e/run_e2e.py --tier 2
   python3 tests/e2e/run_e2e.py --tier 3
   python3 tests/e2e/run_e2e.py --tier 4
   ```

3. **Run by Feature or Milestone**:
   ```bash
   python3 tests/e2e/run_e2e.py --feature F01
   python3 tests/e2e/run_e2e.py --milestone M1
   ```

4. **Pytest Integration**:
   ```bash
   PYTHONPATH=. .venv/bin/pytest tests/e2e -v
   ```

### Pass/Fail Semantics & Reporting
- **Pass (OK)**: Observable output strictly satisfies specification and oracle assertions.
- **Fail (DEFECT)**: Output deviates from specification, crash detected, sanitizer violation reported, or unexpected exit code.
- **Skip (PLANNED)**: Explicitly tagged for a milestone that has not yet completed its implementation dependencies.
- **Exit Codes**:
  - `0`: All executed tests passed.
  - `1`: One or more tests failed (defects detected).
  - `2`: Configuration or test runner execution error.
