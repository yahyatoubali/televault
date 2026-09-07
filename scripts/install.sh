#!/usr/bin/env bash
set -euo pipefail

# ── TeleVault Universal Installer ─────────────────────────────────────────────
# Automatically detects OS and CPU architecture (x86_64, aarch64), downloads the
# latest release from GitHub, validates SHA256 integrity, and installs the binary.
# ─────────────────────────────────────────────────────────────────────────────

REPO="yahyatoubali/televault"
VERSION="${1:-3.5.0}"

# 1. Detect OS
OS="$(uname -s | tr '[:upper:]' '[:lower:]')"
case "$OS" in
    linux)  OS="linux" ;;
    darwin) OS="darwin" ;;
    *)      OS="$OS" ;;
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
    armv7l|armhf)
        ARCH="armhf"
        ;;
    *)
        ARCH="$RAW_ARCH"
        ;;
esac

echo "🛡️  TeleVault Universal Installer v${VERSION}"
echo "   Detected Platform: ${OS}-${ARCH} (${RAW_ARCH})"

# 3. Determine install destination
if [ "$(id -u)" -eq 0 ]; then
    INSTALL_DIR="/usr/local/bin"
else
    if [ "$OS" = "darwin" ] && [ -d "/opt/homebrew/bin" ] && [ -w "/opt/homebrew/bin" ]; then
        INSTALL_DIR="/opt/homebrew/bin"
    else
        INSTALL_DIR="${HOME}/.local/bin"
        mkdir -p "$INSTALL_DIR"
        case ":$PATH:" in
            *":$INSTALL_DIR:"*) ;;
            *) export PATH="$INSTALL_DIR:$PATH" ;;
        esac
    fi
fi

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
    else
        echo "⚠️  sha256sum/shasum not found, skipping checksum verification"
        return 0
    fi
}

echo "==> Checking for pre-built binary on GitHub Releases (${TARBALL})..."
if curl -sSL -f "$DOWNLOAD_URL" -o "$TMP_DIR/$TARBALL" 2>/dev/null; then
    echo "==> Download complete. Verifying SHA256 checksum..."
    if curl -sSL -f "$CHECKSUM_URL" -o "$TMP_DIR/${TARBALL}.sha256" 2>/dev/null; then
        cd "$TMP_DIR"
        if verify_sha256 "${TARBALL}.sha256"; then
            echo "✓ SHA256 checksum verified successfully."
        else
            echo "❌ Checksum verification failed!" >&2
            exit 1
        fi
        cd - >/dev/null
    fi

    echo "==> Extracting and installing to ${INSTALL_DIR}..."
    tar -xzf "$TMP_DIR/$TARBALL" -C "$TMP_DIR"
    EXTRACTED_DIR="$TMP_DIR/televault-v${VERSION}-${OS}-${ARCH}"

    if [ -w "$INSTALL_DIR" ]; then
        cp "$EXTRACTED_DIR/televault" "$INSTALL_DIR/televault"
        chmod +x "$INSTALL_DIR/televault"
        ln -sf "$INSTALL_DIR/televault" "$INSTALL_DIR/tvt"
    else
        sudo cp "$EXTRACTED_DIR/televault" "$INSTALL_DIR/televault"
        sudo chmod +x "$INSTALL_DIR/televault"
        sudo ln -sf "$INSTALL_DIR/televault" "$INSTALL_DIR/tvt"
    fi

    echo ""
    echo "🎉 TeleVault v${VERSION} installed successfully to ${INSTALL_DIR}/televault"
    echo "   Symlink created at: ${INSTALL_DIR}/tvt"
    echo ""
    "$INSTALL_DIR/tvt" --version || true
    echo ""
    echo "Next steps:"
    echo "  1) tvt login      # Authenticate with Telegram"
    echo "  2) tvt setup      # Set up storage channel"
    echo "  3) tvt tui        # Launch interactive Terminal UI"
    exit 0
fi

# Fallback: Automatic build from source
echo "ℹ️  Pre-built binary for ${OS}-${ARCH} is not yet hosted on GitHub release v${VERSION}."
echo "==> Automatically compiling and installing TeleVault v${VERSION} from source..."

if [ "$OS" = "darwin" ]; then
    if ! command -v brew >/dev/null 2>&1; then
        echo "❌ Homebrew is required on macOS to install dependencies automatically." >&2
        echo "   Install Homebrew from https://brew.sh and re-run this script." >&2
        exit 1
    fi
    echo "==> Installing / updating dependencies via Homebrew..."
    brew install cmake boost openssl@3 zstd pkg-config tdlib || true
fi

echo "==> Fetching TeleVault source (branch main)..."
SRC_DIR="$TMP_DIR/source"
git clone --depth 1 -b main "https://github.com/${REPO}.git" "$SRC_DIR"

echo "==> Configuring and compiling TeleVault (C++23 Native)..."
CORES=$(sysctl -n hw.ncpu 2>/dev/null || nproc 2>/dev/null || echo 4)
cmake -B "$SRC_DIR/build" -S "$SRC_DIR" \
    -DCMAKE_BUILD_TYPE=Release \
    -DTV_BUILD_TDLIB=ON \
    -DTV_BUILD_TUI=ON \
    -DTV_BUILD_TESTS=OFF

cmake --build "$SRC_DIR/build" -j"$CORES"

echo "==> Installing to ${INSTALL_DIR}..."
mkdir -p "$INSTALL_DIR" 2>/dev/null || sudo mkdir -p "$INSTALL_DIR"
if [ -w "$INSTALL_DIR" ]; then
    cp "$SRC_DIR/build/src/televault" "$INSTALL_DIR/televault"
    chmod +x "$INSTALL_DIR/televault"
    ln -sf "$INSTALL_DIR/televault" "$INSTALL_DIR/tvt"
else
    sudo cp "$SRC_DIR/build/src/televault" "$INSTALL_DIR/televault"
    sudo chmod +x "$INSTALL_DIR/televault"
    sudo ln -sf "$INSTALL_DIR/televault" "$INSTALL_DIR/tvt"
fi

echo ""
echo "🎉 TeleVault v${VERSION} built and installed successfully to ${INSTALL_DIR}/televault"
echo "   Symlink created at: ${INSTALL_DIR}/tvt"
echo ""
"$INSTALL_DIR/tvt" --version || true
echo ""
echo "Next steps:"
echo "  1) tvt login      # Authenticate with Telegram"
echo "  2) tvt setup      # Set up storage channel"
echo "  3) tvt tui        # Launch interactive Terminal UI"

