#!/usr/bin/env bash
set -euo pipefail

# ── TeleVault Universal One-Liner Installer ──────────────────────────────────
# Automatically detects OS (Linux, macOS) and CPU architecture (x86_64, aarch64,
# arm64), downloads the official pre-built binary release from GitHub, verifies
# SHA256 cryptographic integrity, and installs the 'televault' & 'tvt' binaries.
# ─────────────────────────────────────────────────────────────────────────────

REPO="yahyatoubali/televault"

# 1. Detect OS
OS="$(uname -s | tr '[:upper:]' '[:lower:]')"
case "$OS" in
    linux)  OS="linux" ;;
    darwin) OS="darwin" ;;
    *)
        echo "❌ Unsupported operating system: $OS" >&2
        exit 1
        ;;
esac

# 2. Detect CPU Architecture
RAW_ARCH="$(uname -m)"
case "$RAW_ARCH" in
    x86_64|amd64)
        ARCH="x86_64"
        ;;
    aarch64|arm64)
        if [ "$OS" = "darwin" ]; then
            ARCH="arm64"
        else
            ARCH="aarch64"
        fi
        ;;
    *)
        ARCH="$RAW_ARCH"
        ;;
esac

# 3. Determine Version
VERSION="${1:-}"
if [ -z "$VERSION" ]; then
    echo "🔍 Fetching latest TeleVault release version from GitHub..."
    LATEST_TAG=$(curl -sSL -H "Accept: application/vnd.github.v3+json" \
        "https://api.github.com/repos/${REPO}/releases/latest" 2>/dev/null \
        | grep -m1 '"tag_name":' | sed -E 's/.*"tag_name":[[:space:]]*"([^"]+)".*/\1/' || true)

    if [ -n "$LATEST_TAG" ] && [ "$LATEST_TAG" != "null" ]; then
        VERSION="${LATEST_TAG#v}"
    else
        VERSION="4.0.2"
    fi
else
    VERSION="${VERSION#v}"
fi

echo "🛡️  TeleVault Universal Installer v${VERSION}"
echo "   Platform: ${OS}-${ARCH} (${RAW_ARCH})"

# 4. Determine Install Destination
if [ "$OS" = "darwin" ]; then
    if [ -d "/opt/homebrew/bin" ] && [ -w "/opt/homebrew/bin" ]; then
        INSTALL_DIR="/opt/homebrew/bin"
    elif [ -w "/usr/local/bin" ] || [ "$(id -u)" -eq 0 ]; then
        INSTALL_DIR="/usr/local/bin"
    else
        INSTALL_DIR="${HOME}/.local/bin"
    fi
else
    if [ "$(id -u)" -eq 0 ] || [ -w "/usr/local/bin" ]; then
        INSTALL_DIR="/usr/local/bin"
    else
        INSTALL_DIR="${HOME}/.local/bin"
    fi
fi

mkdir -p "$INSTALL_DIR" 2>/dev/null || true

# Ensure INSTALL_DIR is in PATH
case ":${PATH:-}:" in
    *":$INSTALL_DIR:"*) ;;
    *)
        export PATH="$INSTALL_DIR:$PATH"
        echo "ℹ️  Note: Added $INSTALL_DIR to current PATH"
        ;;
esac

install_file() {
    local src="$1"
    local dest="$2"
    mkdir -p "$(dirname "$dest")" 2>/dev/null || sudo mkdir -p "$(dirname "$dest")"
    if [ -w "$(dirname "$dest")" ]; then
        cp -f "$src" "$dest"
        chmod +x "$dest"
    else
        sudo cp -f "$src" "$dest"
        sudo chmod +x "$dest"
    fi
}

install_symlink() {
    local target="$1"
    local link="$2"
    if [ -w "$(dirname "$link")" ]; then
        ln -sf "$target" "$link"
    else
        sudo ln -sf "$target" "$link"
    fi
}

install_linux_build_deps() {
    # Best-effort per-distro dependency install for source fallback.
    # Runtime libs needed by pre-built binaries: openssl, zstd, blake3, fuse3.
    # Build-time: cmake, g++-14/clang, ssl/zstd/blake3/boost/fuse dev packages.
    if command -v apt-get >/dev/null 2>&1; then
        echo "==> Installing build dependencies via apt..."
        sudo apt-get update -qq || apt-get update -qq || true
        sudo apt-get install -y -qq git cmake g++-14 pkg-config libssl-dev libzstd-dev libblake3-dev libboost-dev libfuse3-dev 2>/dev/null \
            || sudo apt-get install -y -qq git cmake g++ pkg-config libssl-dev libzstd-dev libboost-dev libfuse3-dev || true
    elif command -v dnf >/dev/null 2>&1; then
        echo "==> Installing build dependencies via dnf..."
        sudo dnf install -y -q git cmake gcc-c++ pkg-config openssl-devel libzstd-devel blake3-devel boost-devel fuse3-devel || true
    elif command -v pacman >/dev/null 2>&1; then
        echo "==> Installing build dependencies via pacman..."
        sudo pacman -Sy --noconfirm --needed git cmake gcc pkg-config openssl zstd libblake3 boost fuse3 || true
    elif command -v apk >/dev/null 2>&1; then
        echo "==> Installing build dependencies via apk (musl: source build required)..."
        sudo apk add --no-cache git cmake g++ make pkgconfig openssl-dev zstd-dev blake3-dev boost-dev fuse3-dev linux-headers || true
    elif command -v zypper >/dev/null 2>&1; then
        echo "==> Installing build dependencies via zypper..."
        sudo zypper install -y git cmake gcc14-c++ pkg-config libopenssl-devel libzstd-devel libblake3-devel boost-devel fuse3-devel || true
    else
        echo "⚠️  Unknown package manager. Please install manually: cmake, C++23 compiler (g++-14), openssl, zstd, blake3, boost, fuse3." >&2
    fi
}

check_runtime_deps() {
    # Warn early if the pre-built glibc binary cannot run here
    # (e.g. Alpine/musl, old glibc, or missing libblake3/libfuse3).
    local bin="$1"
    if command -v ldd >/dev/null 2>&1; then
        if ldd "$bin" 2>&1 | grep -q "not found"; then
            echo "⚠️  Missing runtime libraries for pre-built binary:" >&2
            ldd "$bin" 2>&1 | grep "not found" >&2 || true
            echo "   Install them for your distro, e.g.:" >&2
            echo "     Ubuntu/Debian: sudo apt install -y libssl3 libzstd1 libblake3-dev libfuse3-3" >&2
            echo "     Fedora:        sudo dnf install -y openssl-libs libzstd fuse3-libs  (+ build blake3 from source)" >&2
            echo "     Arch:          sudo pacman -S --needed libblake3 onetbb fuse3 openssl zstd" >&2
            echo "     Alpine (musl): pre-built glibc binary is NOT supported — use source fallback below." >&2
        fi
    fi
    if [ "$OS" = "linux" ] && [ -f /etc/alpine-release ]; then
        echo "⚠️  Alpine/musl detected: glibc pre-built binary cannot run. Falling back to source build." >&2
        return 1
    fi
    return 0
}

TARBALL="televault-v${VERSION}-${OS}-${ARCH}.tar.gz"
DOWNLOAD_URL="https://github.com/${REPO}/releases/download/v${VERSION}/${TARBALL}"
CHECKSUM_URL="${DOWNLOAD_URL}.sha256"

TMP_DIR="$(mktemp -d)"
trap 'rm -rf "$TMP_DIR"' EXIT

verify_sha256() {
    local checksum_file="$1"
    if command -v sha256sum >/dev/null 2>&1; then
        sha256sum -c "$checksum_file"
    elif command -v shasum >/dev/null 2>&1; then
        shasum -a 256 -c "$checksum_file"
    elif command -v openssl >/dev/null 2>&1; then
        # Manual verify via openssl when neither sha256sum nor shasum exists
        local expected actual file
        expected="$(awk '{print $1}' "$checksum_file")"
        file="$(awk '{print $2}' "$checksum_file")"
        # checksum file may prefix with '*' for binary mode; strip it
        file="${file#\*}"
        actual="$(openssl dgst -sha256 "$file" | awk '{print $NF}')"
        if [ "$expected" = "$actual" ]; then
            echo "${file}: OK"
            return 0
        else
            echo "${file}: FAILED (expected $expected, got $actual)" >&2
            return 1
        fi
    else
        echo "⚠️  sha256sum/shasum/openssl not found, skipping checksum verification"
        return 0
    fi
}

TARBALL="televault-v${VERSION}-${OS}-${ARCH}.tar.gz"
DOWNLOAD_URL="https://github.com/${REPO}/releases/download/v${VERSION}/${TARBALL}"
CHECKSUM_URL="${DOWNLOAD_URL}.sha256"

TMP_DIR="$(mktemp -d)"
trap 'rm -rf "$TMP_DIR"' EXIT

echo "==> Checking for pre-built release: ${TARBALL}..."
DOWNLOAD_SUCCESS=false

if curl -sSL -f "$DOWNLOAD_URL" -o "$TMP_DIR/$TARBALL" 2>/dev/null; then
    DOWNLOAD_SUCCESS=true
elif [ "$OS" = "darwin" ]; then
    # Fallback to universal binary on macOS
    TARBALL="televault-v${VERSION}-darwin-universal.tar.gz"
    DOWNLOAD_URL="https://github.com/${REPO}/releases/download/v${VERSION}/${TARBALL}"
    CHECKSUM_URL="${DOWNLOAD_URL}.sha256"
    echo "==> Checking for macOS Universal release: ${TARBALL}..."
    if curl -sSL -f "$DOWNLOAD_URL" -o "$TMP_DIR/$TARBALL" 2>/dev/null; then
        DOWNLOAD_SUCCESS=true
    fi
fi

if [ "$DOWNLOAD_SUCCESS" = "true" ]; then
    echo "==> Verifying SHA256 checksum..."
    if curl -sSL -f "$CHECKSUM_URL" -o "$TMP_DIR/${TARBALL}.sha256" 2>/dev/null; then
        cd "$TMP_DIR"
        if verify_sha256 "${TARBALL}.sha256"; then
            echo "✓ SHA256 checksum verified successfully."
        else
            echo "❌ Checksum verification failed! File may have been corrupted." >&2
            exit 1
        fi
        cd - >/dev/null
    else
        echo "ℹ️  No remote checksum file found, proceeding with extraction..."
    fi

    echo "==> Installing binaries to ${INSTALL_DIR}..."
    tar -xzf "$TMP_DIR/$TARBALL" -C "$TMP_DIR"
    EXTRACTED_DIR="$TMP_DIR/${TARBALL%.tar.gz}"

    if [ ! -f "$EXTRACTED_DIR/televault" ]; then
        # Fallback if tarball contains root files directly
        EXTRACTED_DIR="$TMP_DIR"
    fi
    if [ ! -f "$EXTRACTED_DIR/televault" ]; then
        echo "❌ Extracted archive is missing 'televault' binary ($EXTRACTED_DIR)." >&2
        echo "   Falling back to source build..." >&2
    else
        # Warn about missing shared libs / musl before installing
        check_runtime_deps "$EXTRACTED_DIR/televault" || true

        install_file "$EXTRACTED_DIR/televault" "$INSTALL_DIR/televault"
        install_symlink "$INSTALL_DIR/televault" "$INSTALL_DIR/tvt"

    echo ""
    echo "🎉 TeleVault v${VERSION} installed successfully!"
    echo "   Binary:  ${INSTALL_DIR}/televault"
    echo "   Symlink: ${INSTALL_DIR}/tvt"
    echo ""
    if command -v tvt >/dev/null 2>&1; then
        tvt --version || true
    else
        "$INSTALL_DIR/tvt" --version || true
    fi
    if ! "$INSTALL_DIR/tvt" --version >/dev/null 2>&1; then
        echo "" >&2
        echo "⚠️  Installed binary failed to run. Likely missing runtime libs or glibc too old." >&2
        echo "   Try: sudo apt install -y libssl3 libzstd1 libfuse3-3  (+ libblake3)" >&2
    fi
    echo ""
    echo "Quick Start:"
    echo "  1) tvt login      # Authenticate Telegram session"
    echo "  2) tvt setup      # Connect storage channel"
    echo "  3) tvt tui        # Launch GUI-grade Terminal UI"
    echo "  4) tvt push <f>   # Upload & encrypt file"
    echo "  5) tvt ls         # List vault contents"
    exit 0
    fi
fi

# Fallback: Build from source if pre-built binary is unavailable
echo "ℹ️  Pre-built binary for ${OS}-${ARCH} is not available on release v${VERSION}."
case "${OS}-${ARCH}" in
    linux-i686|linux-riscv64|linux-mips*|linux-powerpc*|linux-s390x)
        echo "⚠️  CPU architecture '${RAW_ARCH}' has no pre-built binary. Attempting source build..." >&2
        ;;
esac
echo "==> Building TeleVault from source (C++23 Native)... (automatic compilation)"

for _tool in git cmake; do
    if ! command -v "$_tool" >/dev/null 2>&1; then
        echo "❌ Required tool '$_tool' not found. Installing build dependencies first..." >&2
        break
    fi
done
if ! command -v cc >/dev/null 2>&1 && ! command -v gcc >/dev/null 2>&1 && ! command -v clang >/dev/null 2>&1; then
    echo "❌ No C++ compiler found (need g++-14 or clang++-18 for C++23)." >&2
fi

if [ "$OS" = "darwin" ]; then
    if ! command -v brew >/dev/null 2>&1; then
        echo "❌ Homebrew is required on macOS to install dependencies." >&2
        echo "   Install Homebrew from https://brew.sh and re-run." >&2
        exit 1
    fi
    echo "==> Installing / updating dependencies via Homebrew..."
    # NOTE: tdlib is fetched via CMake FetchContent; there is no brew
    # formula for it, so it must not be listed here.
    brew install cmake boost openssl@3 zstd pkg-config || true
    export PKG_CONFIG_PATH="/opt/homebrew/opt/openssl@3/lib/pkgconfig:/usr/local/opt/openssl@3/lib/pkgconfig:${PKG_CONFIG_PATH:-}"
else
    install_linux_build_deps
fi

SRC_DIR="$TMP_DIR/source"
echo "==> Fetching TeleVault source (tag v${VERSION}, fallback: main)..."
if ! git clone --depth 1 -b "v${VERSION}" "https://github.com/${REPO}.git" "$SRC_DIR" 2>/dev/null; then
    git clone --depth 1 -b main "https://github.com/${REPO}.git" "$SRC_DIR"
fi

echo "==> Compiling TeleVault..."
CORES=$(getconf _NPROCESSORS_ONLN 2>/dev/null || nproc 2>/dev/null || sysctl -n hw.ncpu 2>/dev/null || echo 4)
cmake -B "$SRC_DIR/build" -S "$SRC_DIR" \
    -DCMAKE_BUILD_TYPE=Release \
    -DTV_BUILD_TDLIB=ON \
    -DTV_BUILD_TUI=ON \
    -DTV_BUILD_TESTS=OFF

cmake --build "$SRC_DIR/build" -j"$CORES"

echo "==> Installing to ${INSTALL_DIR}..."
install_file "$SRC_DIR/build/src/televault" "$INSTALL_DIR/televault"
install_symlink "$INSTALL_DIR/televault" "$INSTALL_DIR/tvt"

echo ""
echo "🎉 TeleVault v${VERSION} compiled and installed successfully!"
echo "   Binary:  ${INSTALL_DIR}/televault"
echo "   Symlink: ${INSTALL_DIR}/tvt"
echo ""
"$INSTALL_DIR/tvt" --version || true
