# Multi-CPU Architecture Support

TeleVault v3.5.0 is engineered from the ground up in standard ISO C++23 to run natively and portably across diverse CPU architectures with zero code changes or emulation overhead.

## Supported Architectures

| Architecture | Canonical Triplet | Common Platforms | Status |
|---|---|---|---|
| **x86_64** / `amd64` | `x86_64-linux-gnu` | Modern Intel and AMD 64-bit PCs, servers, CI/CD runners | **Tier 1 (Official Pre-built)** |
| **aarch64** / `arm64` | `aarch64-linux-gnu` | Raspberry Pi 4/5, Apple Silicon (Asahi Linux), AWS Graviton, Ampere Altra | **Tier 1 (Official Pre-built)** |
| **armhf** / `armv7l` | `arm-linux-gnueabihf` | 32-bit ARM SBCs and embedded systems | Tier 2 (Build from source) |

---

## Architectural Guarantees

1. **Zero Inline Assembly**: All crypto and chunking algorithms rely on standard C++23 standard library constructs, OpenSSL 3 cryptographic primitives, Blake3 C implementation, and compiler auto-vectorization (AVX-512, AVX2, NEON, SVE).
2. **Endianness Safe**: Wire formats, encryption headers, and chunk metadata serialize numbers with explicit little-endian byte ordering.
3. **Memory Bounded**: The streaming chunker processes data in configurable memory buffers (32 MB to 256 MB) instead of reading entire files into RAM, allowing TeleVault to operate reliably on systems with as little as 512 MB of RAM.

---

## Pre-compiled Binary Installation

The release script auto-detects your system's architecture:

```bash
# Detect architecture
ARCH=$(uname -m)
[ "$ARCH" = "arm64" ] && ARCH="aarch64"

# Download release tarball
curl -sLO "https://github.com/yahyatoubali/televault/releases/latest/download/televault-v3.5.0-linux-${ARCH}.tar.gz"

# Verify SHA256 checksum
curl -sLO "https://github.com/yahyatoubali/televault/releases/latest/download/televault-v3.5.0-linux-${ARCH}.tar.gz.sha256"
sha256sum -c "televault-v3.5.0-linux-${ARCH}.tar.gz.sha256"

# Extract and install
tar -xzf "televault-v3.5.0-linux-${ARCH}.tar.gz"
cd "televault-v3.5.0-linux-${ARCH}"
sudo cp televault /usr/local/bin/televault
sudo ln -sf /usr/local/bin/televault /usr/local/bin/tvt

# Verify execution
tvt --version
```

---

## Containerized Multi-Arch Builds

To build a standalone containerized binary for any architecture using Docker:

```bash
# Build for ARM64
./scripts/build_docker.sh linux/arm64 3.5.0

# Build for x86_64
./scripts/build_docker.sh linux/amd64 3.5.0
```

Release packages and checksums are placed into `dist/`.
