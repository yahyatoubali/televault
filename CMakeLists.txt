cmake_minimum_required(VERSION 3.22)
set(CMAKE_POLICY_VERSION_MINIMUM "3.5" CACHE STRING "Minimum policy version for subprojects")
project(televault
    VERSION 4.0.1
    DESCRIPTION "Encrypted cloud storage on Telegram channels"
    LANGUAGES CXX C
)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# ── Options ────────────────────────────────────────────────────────────
option(TV_BUILD_TDLIB "Build with tdlib Telegram support" OFF)
option(TV_BUILD_FUSE  "Build FUSE support"  ON)
option(TV_BUILD_WEBDAV "Build WebDAV support" ON)
option(TV_BUILD_TUI   "Build TUI support"   ON)
option(TV_BUILD_TESTS "Build tests"         ON)
option(TV_LOW_RESOURCE "Low-resource defaults" OFF)
option(TV_ENABLE_SANITIZERS "Enable AddressSanitizer (ASan) and UndefinedBehaviorSanitizer (UBSan)" OFF)
option(TV_STATIC_LIBSTDCXX "Statically link libstdc++ and libgcc on Linux for maximum distro portability" OFF)

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

# ── Sources ────────────────────────────────────────────────────────────
add_subdirectory(src)

# ── Tests ──────────────────────────────────────────────────────────────
if(TV_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()
