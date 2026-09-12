# Multi-CPU Architecture Support

TeleVault v4.0.3 is engineered from the ground up in standard ISO C++23 to run natively and portably across diverse CPU architectures with zero code changes or emulation overhead.

## Supported Architectures & Operating Systems

| OS / Architecture | Canonical Triplet | Common Platforms | Status |
|---|---|---|---|
| **Linux x86_64** / `amd64` | `x86_64-linux-gnu` | Modern Intel and AMD 64-bit PCs, servers, CI/CD runners | **Tier 1 (Official Pre-built)** |
| **Linux aarch64** / `arm64` | `aarch64-linux-gnu` | Raspberry Pi 4/5, Apple Silicon (Linux), AWS Graviton, Ampere | **Tier 1 (Official Pre-built)** |
| **macOS Apple Silicon** / `arm64` | `arm64-apple-darwin` | Mac (M1, M2, M3, M4, Pro/Max/Ultra) | **Tier 1 (Supported)** |
| **macOS Intel** / `x86_64` | `x86_64-apple-darwin` | Intel-based Macs (MacBook, iMac, Mac Pro) | **Tier 1 (Supported)** |
| **Linux armhf** / `armv7l` | `arm-linux-gnueabihf` | 32-bit ARM SBCs and embedded systems | Tier 2 (Build from source) |

---

## Architectural Guarantees for Darwin & Linux

1. **Zero Inline Assembly**: All crypto and chunking algorithms rely on standard C++23 standard library constructs, OpenSSL 3 cryptographic primitives, Blake3 C implementation, and compiler auto-vectorization (AVX-512, AVX2, NEON, SVE).
2. **Endianness Safe**: Wire formats, encryption headers, and chunk metadata serialize numbers with explicit little-endian byte ordering.
3. **Memory Bounded**: The streaming chunker processes data in configurable memory buffers (32 MB to 256 MB) instead of reading entire files into RAM, allowing TeleVault to operate reliably on systems with as little as 512 MB of RAM.
4. **Darwin Native Pre-allocation**: Uses macOS `F_PREALLOCATE` (`fstore_t`) and `ftruncate` in place of Linux-specific `fallocate`.
5. **Darwin Native System Info**: Queries macOS hardware limits via `sysctlbyname("hw.memsize", ...)` and `_SC_NPROCESSORS_ONLN`.
6. **Homebrew Auto-Discovery**: CMake automatically searches Apple Silicon Homebrew paths (`/opt/homebrew`), Intel Homebrew paths (`/usr/local`), and Keg-only OpenSSL 3 installations (`/opt/homebrew/opt/openssl@3`).

---

## Universal One-Line Installer (Linux & macOS)

The installer automatically detects your operating system (Linux / Darwin) and CPU architecture (`x86_64`, `arm64`, `aarch64`):

```bash
curl -fsSL https://raw.githubusercontent.com/yahyatoubali/televault/main/scripts/install.sh | bash
```

---

## Building from Source on macOS (Darwin)

### 1. Install Dependencies with Homebrew

```bash
brew install cmake boost openssl@3 zstd pkg-config
```

### 2. Configure and Compile

```bash
git clone https://github.com/yahyatoubali/televault.git
cd televault

cmake -B build \
    -DCMAKE_BUILD_TYPE=Release \
    -DTV_BUILD_TDLIB=ON \
    -DTV_BUILD_TUI=ON

cmake --build build -j$(sysctl -n hw.ncpu)

# Run test suite
ctest --test-dir build --output-on-failure

# Install to /usr/local/bin or ~/.local/bin
sudo cmake --install build
```

### 3. Native macOS Finder Integration via WebDAV

macOS includes native WebDAV mounting directly in Finder:

```bash
# Start WebDAV server
tvt serve --port 8080
```

In Finder:
1. Press `Cmd + K` (Go → Connect to Server).
2. Enter `http://127.0.0.1:8080`.
3. Your encrypted Telegram vault mounts directly as a native Mac network drive!

---

## Containerized Multi-Arch Builds (Linux)

To build a standalone containerized binary for any Linux architecture using Docker:

```bash
# Build for ARM64
./scripts/build_docker.sh linux/arm64 4.0.3

# Build for x86_64
./scripts/build_docker.sh linux/amd64 4.0.3
```

Release packages and checksums are placed into `dist/`.

> **Portability note:** release binaries are built on Ubuntu 24.04
> (glibc 2.39) with `-DTV_PORTABLE=ON` (`-static-libstdc++ -static-libgcc`)
> so they run on Ubuntu 24.04+, Fedora 40+, and Arch.
> Older LTS (Ubuntu 22.04 glibc 2.35, Debian 12 glibc 2.36) and
> Alpine/musl should build from source — the universal installer now
> detects this via `ldd` and prints per-distro hints, and falls back to
> an automatic source build.
