cmake_minimum_required(VERSION 3.22)
# TDLib v1.8.0 still declares cmake_minimum_required(<3.5), which CMake 4.x
# rejects unless the policy floor is lowered. Required for FetchContent tdlib.
set(CMAKE_POLICY_VERSION_MINIMUM "3.5" CACHE STRING "Minimum policy version for subprojects")
project(televault
    VERSION 4.0.2
    DESCRIPTION "Encrypted cloud storage on Telegram channels"
    LANGUAGES CXX C
)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# ── Options ────────────────────────────────────────────────────────────
option(TV_BUILD_TDLIB "Build with tdlib Telegram support" OFF)
if(APPLE)
    option(TV_BUILD_FUSE "Build FUSE support" OFF)
else()
    option(TV_BUILD_FUSE "Build FUSE support" ON)
endif()
option(TV_BUILD_WEBDAV "Build WebDAV support" ON)
option(TV_BUILD_TUI   "Build TUI support"   ON)
option(TV_BUILD_TESTS "Build tests"         ON)
option(TV_LOW_RESOURCE "Low-resource defaults" OFF)
option(TV_ENABLE_SANITIZERS "Enable AddressSanitizer (ASan) and UndefinedBehaviorSanitizer (UBSan)" OFF)
option(TV_STATIC_LIBSTDCXX "Statically link libstdc++ and libgcc on Linux for maximum distro portability" OFF)
# TV_PORTABLE additionally prefers static zstd/blake3/fuse archives where
# available; it composes with TV_STATIC_LIBSTDCXX (both retained so existing
# release workflows keep working unchanged).
option(TV_PORTABLE "Portable release: statically link libstdc++/libgcc and prefer static zstd/blake3/fuse" OFF)

if(TV_STATIC_LIBSTDCXX AND NOT APPLE AND NOT WIN32)
    link_libraries(-static-libstdc++ -static-libgcc)
endif()

# ── Darwin / macOS Homebrew Path Support ─────────────────────────────
if(APPLE)
    # Support Homebrew installations on Apple Silicon (/opt/homebrew) and Intel (/usr/local)
    list(APPEND CMAKE_PREFIX_PATH
        "/opt/homebrew"
        "/opt/homebrew/lib/cmake/Td"
        "/opt/homebrew/opt/openssl@3"
        "/opt/homebrew/opt/openssl"
        "/opt/homebrew/opt/libzstd"
        "/opt/homebrew/opt/zstd"
        "/usr/local"
        "/usr/local/lib/cmake/Td"
        "/usr/local/opt/openssl@3"
        "/usr/local/opt/openssl"
        "/usr/local/opt/libzstd"
        "/usr/local/opt/zstd"
    )
    if(EXISTS "/opt/homebrew/opt/openssl@3")
        set(OPENSSL_ROOT_DIR "/opt/homebrew/opt/openssl@3")
    elseif(EXISTS "/usr/local/opt/openssl@3")
        set(OPENSSL_ROOT_DIR "/usr/local/opt/openssl@3")
    endif()
    if(EXISTS "/opt/homebrew/bin/brew")
        set(ENV{PKG_CONFIG_PATH} "/opt/homebrew/lib/pkgconfig:/opt/homebrew/opt/openssl@3/lib/pkgconfig:$ENV{PKG_CONFIG_PATH}")
    elseif(EXISTS "/usr/local/bin/brew")
        set(ENV{PKG_CONFIG_PATH} "/usr/local/lib/pkgconfig:/usr/local/opt/openssl@3/lib/pkgconfig:$ENV{PKG_CONFIG_PATH}")
    endif()
endif()

# ── System dependencies ───────────────────────────────────────────────
find_package(OpenSSL REQUIRED)
find_package(PkgConfig REQUIRED)
pkg_check_modules(ZSTD QUIET IMPORTED_TARGET libzstd)
if(NOT ZSTD_FOUND)
    find_path(ZSTD_INCLUDE_DIR zstd.h HINTS /opt/homebrew/include /usr/local/include)
    find_library(ZSTD_LIBRARY NAMES zstd libzstd HINTS /opt/homebrew/lib /usr/local/lib)
    if(ZSTD_INCLUDE_DIR AND ZSTD_LIBRARY)
        add_library(PkgConfig::ZSTD UNKNOWN IMPORTED)
        set_target_properties(PkgConfig::ZSTD PROPERTIES
            IMPORTED_LOCATION "${ZSTD_LIBRARY}"
            INTERFACE_INCLUDE_DIRECTORIES "${ZSTD_INCLUDE_DIR}"
        )
    else()
        message(FATAL_ERROR "libzstd not found. Install with 'brew install zstd' or 'apt install libzstd-dev'")
    endif()
endif()
pkg_check_modules(BLAKE3 QUIET IMPORTED_TARGET libblake3)
if(NOT BLAKE3_FOUND)
    find_path(BLAKE3_INCLUDE_DIR blake3.h HINTS /opt/homebrew/include /usr/local/include)
    find_library(BLAKE3_LIBRARY NAMES blake3 libblake3 HINTS /opt/homebrew/lib /usr/local/lib)
    if(BLAKE3_INCLUDE_DIR AND BLAKE3_LIBRARY)
        add_library(PkgConfig::BLAKE3 UNKNOWN IMPORTED)
        set_target_properties(PkgConfig::BLAKE3 PROPERTIES
            IMPORTED_LOCATION "${BLAKE3_LIBRARY}"
            INTERFACE_INCLUDE_DIRECTORIES "${BLAKE3_INCLUDE_DIR}"
        )
    else()
        include(FetchContent)
        FetchContent_Declare(
            blake3_c
            GIT_REPOSITORY https://github.com/BLAKE3-team/BLAKE3.git
            GIT_TAG 1.5.4
            SOURCE_SUBDIR c
        )
        FetchContent_MakeAvailable(blake3_c)
        add_library(PkgConfig::BLAKE3 ALIAS blake3)
    endif()
endif()

# ── Fetched dependencies ──────────────────────────────────────────────
# TDLib v1.8.0 compat: these flags must be set BEFORE FetchContent
# configures the tdlib subproject below, otherwise they never apply to
# TDLib's own targets (directory-scoped options only affect targets
# created afterwards):
#  -fno-char8_t restores C++17 u8-literal behavior (TDLib predates C++20
#   char8_t and fails with "conversion from 'const char8_t[N]'" errors);
#  -Wno-error=register* neutralizes TDLib gperf output's `register`
#   keyword, which AppleClang promotes to an error. The C++14 spelling
#   only exists on Clang (GCC rejects the unknown option outright), so
#   each spelling is scoped to its compiler below.
if(TV_BUILD_TDLIB AND CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    add_compile_options(-fno-char8_t)
    if(CMAKE_CXX_COMPILER_ID MATCHES "Clang")
        add_compile_options(-Wno-error=register -Wno-error=deprecated-register)
    else()
        add_compile_options(-Wno-error=register)
    endif()
endif()
include(FetchContent)
include(cmake/FetchDependencies.cmake)

# ── Default build type ─────────────────────────────────────────────────
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE "Release" CACHE STRING "Choose the type of build." FORCE)
    set_property(CACHE CMAKE_BUILD_TYPE PROPERTY STRINGS "Debug" "Release" "MinSizeRel" "RelWithDebInfo")
endif()

# ── Position-independent code ──────────────────────────────────────────
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

# ── Compiler warnings, hardening, and sanitizers ────────────────────────
if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    # Base warnings
    add_compile_options(
        -Wall
        -Wextra
        -Wpedantic
        -Werror=return-type
        -Wno-unused-parameter
        $<$<COMPILE_LANGUAGE:CXX>:-fno-char8_t>
    )

    # Security Hardening: Stack protector & PIE
    add_compile_options(
        -fstack-protector-strong
        -fPIE
    )

    # Security Hardening: Glibc Fortify Source (only for optimized builds)
    add_compile_definitions(
        $<$<OR:$<CONFIG:Release>,$<CONFIG:RelWithDebInfo>,$<CONFIG:MinSizeRel>>:_FORTIFY_SOURCE=2>
    )

    # Security Hardening: Linker Full RELRO, NX Stack, PIE
    if(NOT APPLE AND NOT WIN32)
        add_link_options(
            -pie
            -Wl,-z,relro,-z,now
            -Wl,-z,noexecstack
        )
    endif()

    # Dynamic Sanitizers (AddressSanitizer + UndefinedBehaviorSanitizer)
    if(TV_ENABLE_SANITIZERS)
        message(STATUS "TeleVault: AddressSanitizer and UndefinedBehaviorSanitizer enabled")
        message(STATUS "TeleVault: Disabling vptr sanitizer (-fno-sanitize=vptr) due to TDLib -fno-rtti")
        add_compile_options(
            -fsanitize=address,undefined
            -fno-sanitize=vptr
            -fno-omit-frame-pointer
            -g
        )
        add_link_options(
            -fsanitize=address,undefined
            -fno-sanitize=vptr
        )
    endif()
endif()

# ── Portable release (static libstdc++/libgcc for multi-distro compat) ──
# Pre-built release binaries must run on Ubuntu 24.04 (glibc 2.39),
# Fedora 40+, and Arch without `GLIBCXX_3.4.36 not found` or
# `libblake3.so.0: cannot open shared object` errors.
if(TV_PORTABLE AND NOT APPLE)
    message(STATUS "TeleVault: portable release mode (static libstdc++/libgcc)")
    add_link_options(-static-libstdc++ -static-libgcc)
    # Prefer static archives for zstd/fuse3 when available, so the
    # release binary does not require libfuse3.so.3 vs .so.4 or
    # libzstd.so.1 on minimal installs. BLAKE3 already static via
    # FetchContent fallback; TBB not needed for portable blake3_c.
    find_library(TV_ZSTD_STATIC libzstd.a HINTS /usr/lib/x86_64-linux-gnu /usr/lib64 /usr/lib /usr/local/lib)
    if(TV_ZSTD_STATIC)
        message(STATUS "TeleVault: portable static zstd: ${TV_ZSTD_STATIC}")
        if(TARGET PkgConfig::ZSTD)
            set_target_properties(PkgConfig::ZSTD PROPERTIES IMPORTED_LOCATION "${TV_ZSTD_STATIC}")
        endif()
    endif()
endif()

# ── Sources ────────────────────────────────────────────────────────────
add_subdirectory(src)

# ── Tests ──────────────────────────────────────────────────────────────
if(TV_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()
