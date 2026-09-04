# TeleVault E2E Test Suite Readiness Report (TEST_READY)

**Status**: READY FOR TESTING & CI INTEGRATION  
**Test Suite Directory**: `tests/e2e/`  
**Test Framework**: Pytest 9.0.2 with Custom Python/C++ Opaque-Box Test Harness  
**Authoritative Specifications**: `ORIGINAL_REQUEST.md`, `PROJECT.md § Feature Inventory` (F01–F27)  

---

## 1. Quick Start & Execution Commands

### Primary Test Runner CLI
The suite includes an interactive command-line runner (`tests/e2e/run_e2e.py`) providing tier, feature, and milestone filtering:

```bash
# Execute entire E2E test suite (all 4 tiers)
.venv/bin/python tests/e2e/run_e2e.py

# Execute specific tier
.venv/bin/python tests/e2e/run_e2e.py --tier 1       # Tier 1: Feature Isolation (>=5 tests/feature)
.venv/bin/python tests/e2e/run_e2e.py --tier 2       # Tier 2: Boundary Value Analysis (>=5 tests/feature)
.venv/bin/python tests/e2e/run_e2e.py --tier 3       # Tier 3: Pairwise Combinations
.venv/bin/python tests/e2e/run_e2e.py --tier 4       # Tier 4: Real-World Workloads

# Filter by feature or milestone
.venv/bin/python tests/e2e/run_e2e.py --feature F16   # Run tests targeting Feature 16
.venv/bin/python tests/e2e/run_e2e.py --milestone M1  # Run tests targeting Milestone 1 deliverables

# Generate machine-readable JSON report
.venv/bin/python tests/e2e/run_e2e.py --json-report e2e-report.json
```

### Direct Pytest Invocations
```bash
# Run entire test suite with quiet progress
.venv/bin/pytest tests/e2e/ -q

# Run specific tier directory
.venv/bin/pytest tests/e2e/tier4_workloads/ -v

# Run with verbose tracebacks and failure summary
.venv/bin/pytest tests/e2e/ --tb=short
```

---

## 2. Test Architecture & Directory Layout

The opaque-box test framework is structured into clean layers without coupling to C++ private symbols or headers:

```
tests/e2e/
├── harness/                           # Opaque-Box Test Infrastructure
│   ├── config.py                      # Binaries, XDG isolation paths, timeouts, ASan/UBSan flags
│   ├── binary_runner.py               # Process runner for ./build/src/televault with sanitizer detection
│   ├── python_oracle.py               # TeleVault Python CLI reference runner
│   ├── crypto_oracle.py               # Independent RFC-compliant AES-GCM, PBKDF2, BLAKE3, ZSTD, __TV1__ oracle
│   ├── binary_inspector.py            # readelf / rabin2 ELF security binary inspector
│   ├── mock_storage.py                # In-memory and disk-backed offline Telegram channel simulator
│   ├── fixtures.py                    # Multi-tier directory generators, deeply nested trees, Unicode filenames
│   └── assertions.py                  # Domain assertions (bit-for-bit file parity, wire format validation)
├── run_e2e.py                         # Unified test runner with filtering and JSON reporting
├── tier1_features/                    # Tier 1: Category-Partition Feature Coverage (135 tests)
│   ├── test_f01_build_hardening.py    # F01: Compiler flags, PIE, stack canary, RELRO
│   ├── test_f02_sanitizers.py         # F02: ASan/UBSan build configuration & clean execution
│   ├── test_f03_async_executor.py     # F03: Async executor pool, symbol resolution, task scheduling
│   ├── test_f04_crypto_wire_format.py # F04: 44-byte AES-256-GCM wire format, salt, nonce, tag
│   ├── test_f05_streaming_crypto.py   # F05: Chunked streaming encryption without whole-file memory buffering
│   ├── test_f06_crypto_raii.py        # F06: Secure memory zeroing, RAII key cleanup, no const_cast
│   ├── test_f07_compression.py        # F07: Zstandard compression, extension blacklist, streaming
│   ├── test_f08_blake3_hash.py        # F08: BLAKE3 32-char prefix parity and streaming hashing
│   ├── test_f09_chunker_memory.py     # F09: Fixed chunk size bounds, O(1) memory during chunking
│   ├── test_f10_chunk_writer.py       # F10: Random-access chunk reassembly, missing chunk detection
│   ├── test_f11_models_schema.py      # F11: FileMetadata, ChunkMetadata, Snapshot JSON schemas
│   ├── test_f12_tg_msg_limit.py       # F12: Telegram 4096-character limit enforcement, __TV1__ compression
│   ├── test_f13_tg_client_concurrency.py # F13: Thread safety, concurrency limits, mutex protection
│   ├── test_f14_vault_0byte_fix.py    # F14: Zero-byte file handling, empty file metadata
│   ├── test_f15_backup_index_protect.py # F15: Pinned snapshot index message protection
│   ├── test_f16_incremental_backup.py # F16: Hash-based differential snapshots, unchanged file reuse
│   ├── test_f17_gc_pruning.py         # F17: Garbage collection, unreferenced chunk orphan cleanup
│   ├── test_f18_watcher.py            # F18: Filesystem watcher debounce and event batching
│   ├── test_f19_cli_flag_parity.py    # F19: Standard vs low-resource mode CLI flags
│   ├── test_f20_cli_subcommands.py    # F20: Subcommand parsing, help output, exit codes
│   ├── test_f21_progress_tracking.py  # F21: Terminal progress bars, non-TTY pipe suppression
│   ├── test_f22_fuse_lru_cache.py     # F22: FUSE LRU chunk caching and memory limits
│   ├── test_f23_webdav_preview.py     # F23: WebDAV server headers, non-download file preview
│   ├── test_f24_unit_test_suite.py    # F24: CTest discovery, individual execution, sanitizers
│   ├── test_f25_coverage_target.py    # F25: Code coverage toolchain and report validation
│   ├── test_f26_git_history.py        # F26: Commit hygiene, conventional commits, author info
│   └── test_f27_clean_git_commit.py   # F27: Repository status, no untracked binaries or artifacts
├── tier2_boundaries/                  # Tier 2: Boundary Value Analysis & Stress (135 tests)
│   ├── test_b01_to_b05_build_crypto_boundaries.py
│   ├── test_b06_to_b10_chunk_wire_boundaries.py
│   ├── test_b11_to_b15_storage_vault_boundaries.py
│   ├── test_b16_to_b20_backup_cli_boundaries.py
│   └── test_b21_to_b27_progress_fuse_git_boundaries.py
├── tier3_pairwise/                    # Tier 3: Combinatorial Interaction Matrices (38 tests)
│   ├── test_pairwise_crypto_compress.py       # Encryption (None/AES-GCM/Pass) × Compression (ZSTD/None/Blacklist)
│   ├── test_pairwise_transfer_sizes.py        # Mode (File/Stdin/Stream) × Payload (0B/1KB/1MB/50MB/150MB)
│   ├── test_pairwise_cli_options.py           # Resource (Normal/Low) × Redirection (File/Stdout)
│   ├── test_pairwise_backup_types.py          # Topology (Flat/Nested/Hidden) × Snapshot (Full/Incremental)
│   └── test_pairwise_concurrency_cache.py     # Workers (1/4/8) × Cache (Default/Constrained LRU)
└── tier4_workloads/                   # Tier 4: Real-World Production Workflows (5 tests)
    ├── test_workload_1_full_project_backup_restore.py
    ├── test_workload_2_incremental_snapshots.py
    ├── test_workload_3_streaming_pipeline.py
    ├── test_workload_4_vault_corruption_and_gc.py
    └── test_workload_5_multiclient_webdav_preview.py
```

---

## 3. Test Coverage Matrix

| Test Tier | Feature Scope | Target Ratio | Implemented Files | Total Tests | Status |
|---|---|---|---|---|---|
| **Tier 1: Feature Isolation** | Features F01 – F27 | $\ge 5$ tests / feature | 27 files | 135 tests | Fully Implemented |
| **Tier 2: Boundary Value Analysis** | Features F01 – F27 | $\ge 5$ tests / feature | 5 files | 135 tests | Fully Implemented |
| **Tier 3: Pairwise Combinations** | Combinatorial matrices | Cross-feature | 5 files | 38 tests | Fully Implemented |
| **Tier 4: Real-World Workloads** | Full production lifecycles | $\ge 5$ workflows | 5 files | 5 workflows | Fully Implemented |
| **TOTAL** | **All 27 Features** | **Full Inventory** | **42 files** | **313 tests** | **100% Implemented** |

---

## 4. Current Execution Results

Execution against the baseline environment (`./build/src/televault` and Python package):

```
============================= test session summary =============================
Total Test Cases Executed: 313
PASSED: 303 tests (96.8%)
FAILED / MILESTONE GAPS: 10 tests (3.2%)
Execution Time: ~25.09s
================================================================================
```

### Breakdown of the 10 Milestone Gaps (Expected Parity / Hardening Findings)

The 10 failing tests are authoritative verification tests detecting features currently scheduled in Milestones 1 through 4 that are awaiting implementation. These tests correctly identify the exact missing capabilities:

1. **F01 (Build Hardening)**:
   - `test_f01_cmake_hardening_flags_configured`: Root `CMakeLists.txt` does not yet declare `-fstack-protector-strong`, `_FORTIFY_SOURCE=2`, `-Wl,-z,noexecstack`, and `-Wl,-z,relro,-z,now`.
   - `test_b01_compiler_security_flags_order`: Fortify source missing from build configuration.
2. **F02 (Sanitizers)**:
   - `test_f02_cmake_sanitizer_option`: `ENABLE_ASAN` / `ENABLE_SANITIZERS` CMake option is not yet wired into the top-level `CMakeLists.txt`.
3. **F03 (Async Executor)**:
   - `test_f03_cmake_target_links_executor`: Target `televault` does not yet compile and link `src/async/executor.cpp`.
   - `test_f03_executor_symbols_present`: Binary lacks `tv::Executor` symbols because `executor.cpp` is omitted from build targets.
   - `test_b03_concurrent_exception_safety`: `executor.cpp` lacks exception catch handlers around task execution.
4. **F06 (Crypto RAII)**:
   - `test_f06_no_const_cast_in_crypto_source`: `src/crypto/aes256gcm.cpp` contains `const_cast` usages that must be removed per M2 requirements.
5. **F13 (Telegram Client Concurrency)**:
   - `test_f13_progress_callback_mutex`: `src/telegram/client.hpp` does not yet protect progress callbacks with `std::mutex`.
6. **F21 (Transfer Progress Tracking)**:
   - `test_f21_progress_source_files_exist`: Missing source files `src/cli/progress.cpp` and `src/cli/progress.hpp` (M4 scope).
   - `test_f21_progress_non_interactive_pipe`: Progress tracking source files not present.

---

## 5. Implementation Bugs & Contract Discrepancies Escalated

During E2E suite validation, the following architectural discrepancies were uncovered between C++ and Python models:

1. **Config Schema Drift**:
   - C++ CLI writes `"low_resource": { "enabled": false, ... }` to `~/.config/televault/config.json`.
   - Python `Config` dataclass in `src/televault/config.py` expects `low_resource_mode: bool`. Loading config in Python without isolation causes a `TypeError`. The test harness isolates XDG directories to shield tests, but schema unification is required.
2. **Snapshot Model Field Discrepancy**:
   - C++ `Snapshot` uses `files` containing `SnapshotFile(name, size, hash)`.
   - Python `Snapshot` uses `files: list[SnapshotFile]` with `path`, `file_id`, `hash`, `size`, `modified_at`. Tests were written to adhere to the Python data contract while remaining opaque to the wire JSON format.
3. **RELRO Partial vs Full**:
   - Binary `./build/src/televault` currently has `relro: partial` (lacks `BIND_NOW` flag in the dynamic section). Full hardening is expected upon M1 completion.
