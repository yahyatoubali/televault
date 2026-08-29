#!/usr/bin/env bash
set -euo pipefail

# Determine project root
ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT_DIR"

VERSION="${1:-3.5.0}"
OS="$(uname -s | tr '[:upper:]' '[:lower:]')"
ARCH="$(uname -m)"

case "$ARCH" in
    x86_64|amd64) ARCH="x86_64" ;;
    aarch64|arm64) ARCH="aarch64" ;;
    armv7l|armhf) ARCH="armhf" ;;
    *) ARCH="$ARCH" ;;
esac

DIST_DIR="$ROOT_DIR/dist"
STAGE_DIR="$ROOT_DIR/build/stage/televault-v${VERSION}-${OS}-${ARCH}"
ARCHIVE_NAME="televault-v${VERSION}-${OS}-${ARCH}.tar.gz"

echo "==> Building TeleVault v${VERSION} for ${OS}-${ARCH}..."

mkdir -p "$ROOT_DIR/build"
cd "$ROOT_DIR/build"
cmake .. \
    -DCMAKE_BUILD_TYPE=Release \
    -DTV_BUILD_TDLIB=ON \
    -DTV_BUILD_TESTS=ON \
    -DTV_BUILD_FUSE=OFF \
    -DTV_BUILD_WEBDAV=OFF \
    -DTV_BUILD_TUI=OFF

cmake --build . --target televault -j"$(nproc)"

echo "==> Packaging release archive..."
mkdir -p "$DIST_DIR" "$STAGE_DIR"
cp "$ROOT_DIR/build/src/televault" "$STAGE_DIR/"
strip "$STAGE_DIR/televault" 2>/dev/null || true
cp "$ROOT_DIR/README.md" "$STAGE_DIR/" 2>/dev/null || true
cp "$ROOT_DIR/LICENSE" "$STAGE_DIR/" 2>/dev/null || true

tar -czf "$DIST_DIR/$ARCHIVE_NAME" -C "$ROOT_DIR/build/stage" "televault-v${VERSION}-${OS}-${ARCH}"
cd "$DIST_DIR"
sha256sum "$ARCHIVE_NAME" > "${ARCHIVE_NAME}.sha256"

echo "==> Created release archive at: $DIST_DIR/$ARCHIVE_NAME"
echo "==> SHA256: $(cat "${ARCHIVE_NAME}.sha256")"
