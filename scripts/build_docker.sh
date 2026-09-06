#!/usr/bin/env bash
set -euo pipefail

# Determine project root
ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT_DIR"

PLATFORM="${1:-linux/amd64}"
VERSION="${2:-3.5.0}"

# Normalize architecture name
case "$PLATFORM" in
    *arm64*|*aarch64*) ARCH="aarch64" ;;
    *amd64*|*x86_64*) ARCH="x86_64" ;;
    *armv7*|*armhf*)  ARCH="armhf" ;;
    *)                ARCH="unknown" ;;
esac

IMAGE_TAG="televault-builder:${ARCH}"

echo "==> Building container builder image for platform $PLATFORM ($ARCH)..."
docker build --platform "$PLATFORM" -t "$IMAGE_TAG" -f Dockerfile.build .

echo "==> Compiling and packaging TeleVault for $ARCH in container..."
docker run --rm --platform "$PLATFORM" \
    -v "$ROOT_DIR:/workspace" \
    "$IMAGE_TAG" -c "
        set -euo pipefail
        mkdir -p build-${ARCH}
        cd build-${ARCH}
        cmake .. \
            -DCMAKE_BUILD_TYPE=Release \
            -DTV_BUILD_TDLIB=ON \
            -DTV_BUILD_TESTS=ON \
            -DTV_BUILD_FUSE=OFF \
            -DTV_BUILD_WEBDAV=OFF \
            -DTV_BUILD_TUI=OFF
        cmake --build . --target televault -j\$(nproc)
        ctest --output-on-failure
        cd ..
        SKIP_BUILD=1 ./scripts/package_release.sh \"$VERSION\" \"$ARCH\" \"./build-${ARCH}/src/televault\"
    "

echo "==> Finished build for $ARCH. Release assets available in dist/"
