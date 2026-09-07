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
if [ "$OS" != "linux" ]; then
    echo "⚠️  Currently, pre-built binaries are optimized for Linux. Detected OS: $OS"
fi

# 2. Detect CPU Architecture
RAW_ARCH="$(uname -m)"
case "$RAW_ARCH" in
    x86_64|amd64)
        ARCH="x86_64"
        ;;
    aarch64|arm64)
        ARCH="aarch64"
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
    INSTALL_DIR="${HOME}/.local/bin"
    mkdir -p "$INSTALL_DIR"
    # Ensure ~/.local/bin is in PATH for current session
    case ":$PATH:" in
        *":$INSTALL_DIR:"*) ;;
        *) export PATH="$INSTALL_DIR:$PATH" ;;
    esac
fi

TARBALL="televault-v${VERSION}-${OS}-${ARCH}.tar.gz"
DOWNLOAD_URL="https://github.com/${REPO}/releases/download/v${VERSION}/${TARBALL}"
CHECKSUM_URL="${DOWNLOAD_URL}.sha256"

TMP_DIR="$(mktemp -d)"
trap 'rm -rf "$TMP_DIR"' EXIT

echo "==> Downloading ${TARBALL} from GitHub Releases..."
if curl -fsSL "$DOWNLOAD_URL" -o "$TMP_DIR/$TARBALL"; then
    echo "==> Download complete. Verifying SHA256 checksum..."
    if curl -fsSL "$CHECKSUM_URL" -o "$TMP_DIR/${TARBALL}.sha256"; then
        cd "$TMP_DIR"
        if sha256sum -c "${TARBALL}.sha256"; then
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

    cp "$EXTRACTED_DIR/televault" "$INSTALL_DIR/televault"
    chmod +x "$INSTALL_DIR/televault"
    ln -sf "$INSTALL_DIR/televault" "$INSTALL_DIR/tvt"

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
else
    echo "⚠️  Pre-built binary for ${OS}-${ARCH} is not yet available on GitHub release v${VERSION}."
    echo "==> You can build directly from source with:"
    echo "      git clone https://github.com/${REPO}.git"
    echo "      cd televault"
    echo "      cmake -B build -DCMAKE_BUILD_TYPE=Release -DTV_BUILD_TDLIB=ON -DTV_BUILD_TUI=ON"
    echo "      cmake --build build -j\$(nproc)"
    echo "      sudo cmake --install build"
    exit 1
fi
