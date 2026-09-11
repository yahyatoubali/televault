# TeleVault

**High-Performance Encrypted Cloud Storage on Telegram MTProto**  
*Full Native C++23 Core • Military-Grade Zero-Trust Encryption • Multi-Gigabit Throughput*

---

TeleVault transforms private Telegram channels into unlimited, zero-trust cloud storage. Plaintext files and passwords never leave your machine — data is compressed with Zstandard, encrypted with OpenSSL 3 AES-256-GCM, and verified with Blake3 cryptographic integrity before streaming directly over Telegram's native MTProto network via TDLib.

```bash
# Instant install for x86_64 and ARM64 / aarch64
curl -fsSL https://raw.githubusercontent.com/yahyatoubali/televault/main/scripts/install.sh | bash

# Authenticate & push file
tvt login
tvt push secret-archive.tar.gz
```

---

## Core Specifications

| Feature | Specification |
|---|---|
| **Core Architecture** | Native ISO C++23 (`tv_core` / `tv_app`), compiled with GCC 14+ / Clang 18+ |
| **Network Protocol** | Native Telegram MTProto via TDLib v1.8.0+ |
| **Encryption** | OpenSSL 3 AES-256-GCM (256-bit key, 12-byte random nonce, 16-byte auth tag) |
| **Key Derivation** | Argon2id & PBKDF2 with dual-salt backward-compatible negotiation |
| **Integrity Verification** | Multithreaded Blake3 (chunk-level and full-file verification) |
| **Compression** | Zstandard level 3 (intelligent media type detection & bypass) |
| **Chunking Engine** | Memory-bounded streaming chunker (default 256 MB, low-resource 32 MB) |
| **Instant Preview** | Sub-second chunk-0 retrieval (`tvt preview`) with syntax & MIME detection |
| **Interactive TUI** | Native FTXUI dashboard (`tvt tui`) with keyboard navigation & modals |
| **Backup Engine** | Isolated Grandfather-Father-Son (GFS) snapshot engine (`tvt backup`) |
| **Supported Platforms** | Linux (`x86_64`, `aarch64`), macOS Darwin (`arm64` Apple Silicon, `x86_64` Intel) |

---

## Key Capabilities

=== "Zero-Trust Security"
    - **Client-Side Encryption**: AES-256-GCM with a random 12-byte nonce per chunk.
    - **Dual-Salt Derivation**: Eliminates protocol lock-in with seamless backward compatibility.
    - **Integrity Guarantee**: Every chunk has a Blake3 hash verified before and after upload.

=== "Multi-Gigabit Throughput"
    - **C++23 Native Pipeline**: Replaced Python prototype with native multithreaded streaming.
    - **Zero Whole-File Buffering**: Gigabyte-scale files stream in chunks without exhausting RAM.
    - **Parallel Transfers**: Up to 8 concurrent upload / 10 concurrent download streams.

=== "Zero-Data-Loss Reliability"
    - **Atomic Writes**: Downloads stream into temporary `.tmp.tv` files and atomic-rename on Blake3 match.
    - **Self-Healing Recovery**: `tvt recover` reconstructs corrupt local indexes from channel history.
    - **Snapshot Backups**: Dedicated reply chains isolate snapshots from the primary vault index.

=== "Developer Interfaces"
    - **Interactive TUI**: Rich terminal UI (`tvt tui`) with real-time filtering, instant preview, and modals.
    - **Virtual Filesystem**: Mount your vault via FUSE3 (`tvt mount`).
    - **WebDAV Server**: Native HTTP/WebDAV server (`tvt serve`) for Finder and Windows Explorer.
    - **Directory Watcher**: Real-time automated synchronization (`tvt watch`) with clean signal handling.

---

## Quick Start

### 1. Download & Install

=== "Universal One-Liner (Linux & macOS)"
    ```bash
    curl -fsSL https://raw.githubusercontent.com/yahyatoubali/televault/main/scripts/install.sh | bash
    ```

=== "Manual Download"
    ```bash
    # Auto-detect OS & CPU
    OS=$(uname -s | tr '[:upper:]' '[:lower:]')
    ARCH=$(uname -m)
    [ "$ARCH" = "x86_64" ] && [ "$OS" = "linux" ] && TARGET="linux-x86_64"
    [ "$ARCH" = "aarch64" ] || [ "$ARCH" = "arm64" ] && [ "$OS" = "linux" ] && TARGET="linux-aarch64"
    [ "$ARCH" = "arm64" ] && [ "$OS" = "darwin" ] && TARGET="darwin-arm64"
    [ "$ARCH" = "x86_64" ] && [ "$OS" = "darwin" ] && TARGET="darwin-x86_64"

    curl -sLO "https://github.com/yahyatoubali/televault/releases/latest/download/televault-v4.0.2-${TARGET}.tar.gz"
    tar -xzf "televault-v4.0.2-${TARGET}.tar.gz"
    sudo cp "televault-v4.0.2-${TARGET}/televault" /usr/local/bin/televault
    sudo ln -sf /usr/local/bin/televault /usr/local/bin/tvt
    tvt --version
    ```

=== "Build from Source"
    ```bash
    # Ubuntu / Debian
    sudo apt update && sudo apt install -y cmake g++-14 libssl-dev libzstd-dev libblake3-dev libboost-dev pkg-config

    # macOS (Homebrew)
    brew install cmake boost openssl@3 zstd pkg-config

    # Configure & Compile
    cmake -B build -DCMAKE_BUILD_TYPE=Release -DTV_BUILD_TESTS=ON
    cmake --build build -j$(nproc 2>/dev/null || sysctl -n hw.ncpu)

    # Test & Install
    ctest --test-dir build --output-on-failure
    sudo cp build/src/televault /usr/local/bin/televault
    sudo ln -sf /usr/local/bin/televault /usr/local/bin/tvt
    ```

### 2. Authenticate & Setup Channel

```bash
# Authenticate with phone code, 2FA, or interactive terminal QR code
tvt login

# ...or skip the SMS round-trip entirely (recommended: no code needed)
tvt login --qr

# Create and configure the encrypted Telegram storage channel
tvt setup
```

### 3. Everyday Operations

```bash
# Push single file or entire directory recursively
tvt push document.pdf
tvt push --recursive /data/projects

# Sub-second preview of remote encrypted file (fetches only chunk 0)
tvt preview document.pdf

# Download file with atomic swap & Blake3 verification
tvt pull document.pdf -o ~/Downloads/document.pdf

# List vault files with sizes, dates, and chunk counts
tvt ls

# Search files by pattern
tvt find "*.pdf"

# Create an isolated snapshot backup
tvt backup create /home/user/docs -n "weekly-backup"
```

---

## Documentation Index

- **[Architecture Design](architecture.md)** — Core C++23 modules, state machines, and threat model.
- **[Message Topology](engine.md)** — Channel layout, pinned index, chunk replies, and snapshot chains.
- **[Security Protocol](security.md)** — AES-256-GCM, dual-salt KDF, wire format, and Blake3 integrity.
- **[Multi-Architecture](multiarch.md)** — Support for x86_64, aarch64, Raspberry Pi, and ARM servers.
- **[Hardware Optimization](hardware.md)** — Low-resource mode, memory bounding, and performance tuning.
- **[Virtualization](virtualization.md)** — FUSE3 filesystem mount and WebDAV server.
- **[Command Manual](commands.md)** — Complete `tvt` CLI reference with all flags and examples.
- **[Developer Notes](development/project-scope.md)** — Project scope, test infrastructure, and audit verification.
