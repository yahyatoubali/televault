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
        VERSION="4.0.0"
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

verify_sha256() {
    local checksum_file="$1"
    if command -v sha256sum >/dev/null 2>&1; then
        sha256sum -c "$checksum_file"
    elif command -v shasum >/dev/null 2>&1; then
        shasum -a 256 -c "$checksum_file"
    else
        echo "⚠️  sha256sum/shasum not found, skipping checksum verification"
        return 0
    fi
}

TARBALL="televault-v${VERSION}-${OS}-${ARCH}.tar.gz"
DOWNLOAD_URL="https://github.com/${REPO}/releases/download/v${VERSION}/${TARBALL}"
CHECKSUM_URL="${DOWNLOAD_URL}.sha256"

TMP_DIR="$(mktemp -d)"
trap 'rm -rf "$TMP_DIR"' EXIT

echo "==> Downloading pre-built release: ${TARBALL}..."
if curl -sSL -f "$DOWNLOAD_URL" -o "$TMP_DIR/$TARBALL" 2>/dev/null; then
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
    EXTRACTED_DIR="$TMP_DIR/televault-v${VERSION}-${OS}-${ARCH}"

    if [ ! -f "$EXTRACTED_DIR/televault" ]; then
        # Fallback if tarball contains root files directly
        EXTRACTED_DIR="$TMP_DIR"
    fi

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
    echo ""
    echo "Quick Start:"
    echo "  1) tvt login      # Authenticate Telegram session"
    echo "  2) tvt setup      # Connect storage channel"
    echo "  3) tvt tui        # Launch GUI-grade Terminal UI"
    echo "  4) tvt push <f>   # Upload & encrypt file"
    echo "  5) tvt ls         # List vault contents"
    exit 0
fi

# Fallback: Build from source if pre-built binary is unavailable
echo "ℹ️  Pre-built binary for ${OS}-${ARCH} is not available on release v${VERSION}."
echo "==> Building TeleVault from source (C++23 Native)..."

if [ "$OS" = "darwin" ]; then
    if ! command -v brew >/dev/null 2>&1; then
        echo "❌ Homebrew is required on macOS to install dependencies." >&2
        echo "   Install Homebrew from https://brew.sh and re-run." >&2
        exit 1
    fi
    echo "==> Installing dependencies via Homebrew..."
    brew install cmake boost openssl@3 zstd pkg-config tdlib || true
elif [ "$OS" = "linux" ]; then
    if command -v apt-get >/dev/null 2>&1; then
        echo "==> Installing build dependencies via apt..."
        sudo apt-get update -qq || true
        sudo apt-get install -y -qq cmake g++-14 libssl-dev libzstd-dev libblake3-dev libboost-dev libfuse3-dev || true
    fi
fi

SRC_DIR="$TMP_DIR/source"
echo "==> Fetching TeleVault source..."
git clone --depth 1 "https://github.com/${REPO}.git" "$SRC_DIR"

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
