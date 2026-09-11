#!/usr/bin/env bash
set -euo pipefail

# ── TeleVault Package Release Script ─────────────────────────────────────────
# Packages compiled binaries into distribution tarballs with SHA256 checksums
# ─────────────────────────────────────────────────────────────────────────────

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT_DIR"

VERSION="${1:-4.0.0}"
VERSION="${VERSION#v}"
OS="$(uname -s | tr '[:upper:]' '[:lower:]')"
ARCH="${2:-$(uname -m)}"

case "$ARCH" in
    x86_64|amd64) ARCH="x86_64" ;;
    aarch64|arm64)
        if [ "$OS" = "darwin" ]; then
            ARCH="arm64"
        else
            ARCH="aarch64"
        fi
        ;;
    universal|universal2) ARCH="universal" ;;
    armv7l|armhf) ARCH="armhf" ;;
    *) ARCH="$ARCH" ;;
esac

get_cpu_cores() {
    getconf _NPROCESSORS_ONLN 2>/dev/null || nproc 2>/dev/null || sysctl -n hw.ncpu 2>/dev/null || echo 4
}

calc_sha256() {
    local file="$1"
    if command -v sha256sum >/dev/null 2>&1; then
        sha256sum "$file"
    elif command -v shasum >/dev/null 2>&1; then
        shasum -a 256 "$file"
    else
        echo "$(openssl dgst -sha256 "$file" | awk '{print $NF}')  $file"
    fi
}

DIST_DIR="$ROOT_DIR/dist"
STAGE_NAME="televault-v${VERSION}-${OS}-${ARCH}"
STAGE_DIR="$ROOT_DIR/build/stage/${STAGE_NAME}"
ARCHIVE_NAME="${STAGE_NAME}.tar.gz"

BINARY_PATH="${3:-$ROOT_DIR/build/src/televault}"
SKIP_BUILD="${SKIP_BUILD:-0}"

if [ "$SKIP_BUILD" != "1" ] && [ ! -f "$BINARY_PATH" ]; then
    echo "==> Building TeleVault v${VERSION} for ${OS}-${ARCH}..."
    mkdir -p "$ROOT_DIR/build"
    cd "$ROOT_DIR/build"
    cmake .. \
        -DCMAKE_BUILD_TYPE=Release \
        -DTV_BUILD_TDLIB=ON \
        -DTV_BUILD_TESTS=ON \
        -DTV_BUILD_TUI=ON

    cmake --build . --target televault -j"$(get_cpu_cores)"
    cd "$ROOT_DIR"
fi

if [ ! -f "$BINARY_PATH" ]; then
    echo "❌ Error: Binary not found at $BINARY_PATH" >&2
    exit 1
fi

echo "==> Packaging release archive for ${OS}-${ARCH}..."
rm -rf "$STAGE_DIR"
mkdir -p "$DIST_DIR" "$STAGE_DIR"

cp "$BINARY_PATH" "$STAGE_DIR/televault"
chmod +x "$STAGE_DIR/televault"

# Strip symbols to minimize binary size
if [ "$OS" = "darwin" ]; then
    strip -S "$STAGE_DIR/televault" 2>/dev/null || true
else
    strip --strip-all "$STAGE_DIR/televault" 2>/dev/null || strip "$STAGE_DIR/televault" 2>/dev/null || true
fi

# Create convenient symlink
ln -sf televault "$STAGE_DIR/tvt"

cp "$ROOT_DIR/README.md" "$STAGE_DIR/" 2>/dev/null || true
cp "$ROOT_DIR/LICENSE" "$STAGE_DIR/" 2>/dev/null || true

tar -czf "$DIST_DIR/$ARCHIVE_NAME" -C "$ROOT_DIR/build/stage" "${STAGE_NAME}"

cd "$DIST_DIR"
calc_sha256 "$ARCHIVE_NAME" > "${ARCHIVE_NAME}.sha256"

echo "==> Release packaging complete:"
echo "    Archive:  $DIST_DIR/$ARCHIVE_NAME"
echo "    SHA256:   $(cat "${ARCHIVE_NAME}.sha256")"
