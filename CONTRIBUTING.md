# Contributing to TeleVault

Thanks for your interest in contributing! This guide covers the C++23 version on the `needspeed` branch.

## Quick Start

```bash
git clone https://github.com/YahyaToubali/televault.git
cd televault
git checkout dev

# Install dependencies (Ubuntu 24.04)
sudo apt install cmake g++-14 clang++-18 libtd-dev libssl-dev \
                 libzstd-dev libblake3-dev libboost-dev libfuse3-dev

# Build
cmake -B build -DCMAKE_BUILD_TYPE=Debug -DTV_BUILD_TESTS=ON
cmake --build build -j$(nproc)

# Run tests
ctest --test-dir build --output-on-failure

# Run the binary
./build/src/televault --help
```

## Code Style

- **C++23** — Use `std::println`, `std::expected`, `std::span`, `std::ranges`
- **Formatting** — Follow existing style (4-space indent, snake_case for functions, PascalCase for types)
- **Headers** — Use `.hpp` with `#pragma once`
- **Includes** — Group in order: own header, project headers, 3rd-party, stdlib
- **Error handling** — Use `std::expected<T, E>` or throw `std::runtime_error`
- **No raw pointers** — Use `std::unique_ptr` for ownership, references for non-ownership
- **Thread safety** — Document thread safety guarantees; use `std::mutex` + `std::shared_lock` where needed

## Architecture Overview

```
CLI (CLI11) → AppContext → TeleVault → TelegramClient (tdlib)
                              ├── IndexManager (pinned message)
                              ├── Chunker (file split + BLAKE3)
                              ├── Compressor (zstd)
                              └── Encryptor (AES-256-GCM)
```

All data lives in Telegram channel messages:
- **Pinned**: VaultIndex (JSON) — maps file_id → metadata_message_id
- **Text**: FileMetadata (JSON) — per-file info + chunk references
- **Files**: Chunk data as document messages, replying to metadata

## Testing

```bash
# Build and run all tests
cmake -B build -DCMAKE_BUILD_TYPE=Debug -DTV_BUILD_TESTS=ON
cmake --build build -j$(nproc)
ctest --test-dir build --output-on-failure

# Run specific test
./build/tests/test_crypto
./build/tests/test_compression
./build/tests/test_chunker
```

## Commit Messages

Follow conventional commits:
- `feat:` — New feature
- `fix:` — Bug fix
- `refactor:` — Code change without feature/fix
- `test:` — Adding/updating tests
- `docs:` — Documentation

## Pull Requests

1. Branch from `dev`
2. Keep changes focused — one feature/fix per PR
3. Add tests for new functionality
4. Ensure all tests pass
5. Update README if needed

## License

By contributing, you agree that your contributions will be licensed under the MIT License.
