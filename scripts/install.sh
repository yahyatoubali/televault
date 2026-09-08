#!/usr/bin/env bash
set -euo pipefail

# ── TeleVault Universal Installer ─────────────────────────────────────────────
# Automatically detects OS and CPU architecture (x86_64, aarch64), downloads the
# latest release from GitHub, validates SHA256 integrity, and installs the binary.
# ─────────────────────────────────────────────────────────────────────────────

REPO="yahyatoubali/televault"
VERSION="${1:-4.0.0}"

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
if [ "$OS" = "darwin" ]; then
    if [ -d "/opt/homebrew/bin" ]; then
        INSTALL_DIR="/opt/homebrew/bin"
    else
        INSTALL_DIR="/usr/local/bin"
    fi
else
    if [ "$(id -u)" -eq 0 ] || [ -w "/usr/local/bin" ]; then
        INSTALL_DIR="/usr/local/bin"
    else
        INSTALL_DIR="${HOME}/.local/bin"
        mkdir -p "$INSTALL_DIR"
        case ":$PATH:" in
            *":$INSTALL_DIR:"*) ;;
            *) export PATH="$INSTALL_DIR:$PATH" ;;
        esac
    fi
fi

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
    if [ ! -f "$EXTRACTED_DIR/televault" ]; then
        echo "❌ Extracted archive is missing 'televault' binary ($EXTRACTED_DIR)." >&2
        echo "   Falling back to source build..." >&2
    else
        # Warn about missing shared libs / musl before installing
        check_runtime_deps "$EXTRACTED_DIR/televault" || true

        install_file "$EXTRACTED_DIR/televault" "$INSTALL_DIR/televault"
        install_symlink "$INSTALL_DIR/televault" "$INSTALL_DIR/tvt"

        echo ""
        echo "🎉 TeleVault v${VERSION} installed successfully to ${INSTALL_DIR}/televault"
        echo "   Symlink created at: ${INSTALL_DIR}/tvt"
        echo ""
        if ! "$INSTALL_DIR/tvt" --version; then
            echo "" >&2
            echo "⚠️  Installed binary failed to run. Likely missing runtime libs or glibc too old." >&2
            echo "   Try: sudo apt install -y libssl3 libzstd1 libfuse3-3  (+ libblake3)" >&2
            echo "   Or re-run: curl -fsSL https://raw.githubusercontent.com/${REPO}/main/scripts/install.sh | bash -s -- ${VERSION}" >&2
            echo "   If the binary still fails, the script will fall back to a source build on next run" >&2
            echo "   after removing the broken binary, or build manually (see README)." >&2
        fi
        echo ""
        echo "Next steps:"
        echo "  1) tvt login      # Authenticate with Telegram"
        echo "  2) tvt setup      # Set up storage channel"
        echo "  3) tvt tui        # Launch interactive Terminal UI"
        exit 0
    fi
fi

# Fallback: Automatic build from source
echo "ℹ️  Pre-built binary for ${OS}-${ARCH} is not yet hosted on GitHub release v${VERSION}."
case "${OS}-${ARCH}" in
    linux-i686|linux-riscv64|linux-mips*|linux-powerpc*|linux-s390x)
        echo "⚠️  CPU architecture '${RAW_ARCH}' has no pre-built binary. Attempting source build..." >&2
        ;;
esac
echo "==> Automatically compiling and installing TeleVault v${VERSION} from source..."

for _tool in git cmake; do
    if ! command -v "$_tool" >/dev/null 2>&1; then
        echo "❌ Required tool '$_tool' not found. Installing build dependencies first..." >&2
        break
    fi
done
if ! command -v cc >/dev/null 2>&1 && ! command -v gcc >/dev/null 2>&1 && ! command -v clang >/dev/null 2>&1; then
    echo "❌ No C++ compiler found (need g++-14 or clang++-18 for C++23)." >&2
fi

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
        echo "❌ Homebrew is required on macOS to install dependencies automatically." >&2
        echo "   Install Homebrew from https://brew.sh and re-run this script." >&2
        exit 1
    fi
    echo "==> Installing / updating dependencies via Homebrew..."
    # NOTE: tdlib is fetched via CMake FetchContent; do not require a brew formula.
    brew install cmake boost openssl@3 zstd pkg-config || true
    export PKG_CONFIG_PATH="/opt/homebrew/opt/openssl@3/lib/pkgconfig:/usr/local/opt/openssl@3/lib/pkgconfig:${PKG_CONFIG_PATH:-}"
else
    install_linux_build_deps
fi

echo "==> Fetching TeleVault source (tag v${VERSION}, fallback: main)..."
SRC_DIR="$TMP_DIR/source"
if ! git clone --depth 1 -b "v${VERSION}" "https://github.com/${REPO}.git" "$SRC_DIR" 2>/dev/null; then
    git clone --depth 1 -b main "https://github.com/${REPO}.git" "$SRC_DIR"
fi

echo "==> Configuring and compiling TeleVault (C++23 Native)..."
CORES=$(sysctl -n hw.ncpu 2>/dev/null || nproc 2>/dev/null || echo 4)
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
echo "🎉 TeleVault v${VERSION} built and installed successfully to ${INSTALL_DIR}/televault"
echo "   Symlink created at: ${INSTALL_DIR}/tvt"
echo ""
"$INSTALL_DIR/tvt" --version || true
echo ""
echo "Next steps:"
echo "  1) tvt login      # Authenticate with Telegram"
echo "  2) tvt setup      # Set up storage channel"
echo "  3) tvt tui        # Launch interactive Terminal UI"

