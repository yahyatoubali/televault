cmake_minimum_required(VERSION 3.22)
project(televault
    VERSION 3.5.0
    DESCRIPTION "Encrypted cloud storage on Telegram channels"
    LANGUAGES CXX C
)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# ── Options ────────────────────────────────────────────────────────────
option(TV_BUILD_TDLIB "Build with tdlib Telegram support" OFF)
option(TV_BUILD_FUSE  "Build FUSE support"  OFF)
option(TV_BUILD_WEBDAV "Build WebDAV support" OFF)
option(TV_BUILD_TUI   "Build TUI support"   ON)
option(TV_BUILD_TESTS "Build tests"         ON)
option(TV_LOW_RESOURCE "Low-resource defaults" OFF)
option(TV_ENABLE_SANITIZERS "Enable AddressSanitizer (ASan) and UndefinedBehaviorSanitizer (UBSan)" OFF)

# ── System dependencies ───────────────────────────────────────────────
find_package(OpenSSL REQUIRED)
find_package(PkgConfig REQUIRED)
pkg_check_modules(ZSTD REQUIRED IMPORTED_TARGET libzstd)
pkg_check_modules(BLAKE3 REQUIRED IMPORTED_TARGET libblake3)

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
