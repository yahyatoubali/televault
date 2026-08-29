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
option(TV_BUILD_TUI   "Build TUI support"   OFF)
option(TV_BUILD_TESTS "Build tests"         ON)
option(TV_LOW_RESOURCE "Low-resource defaults" OFF)

# ── System dependencies ───────────────────────────────────────────────
find_package(OpenSSL REQUIRED)
find_package(PkgConfig REQUIRED)
pkg_check_modules(ZSTD REQUIRED IMPORTED_TARGET libzstd)
pkg_check_modules(BLAKE3 REQUIRED IMPORTED_TARGET libblake3)

# ── Fetched dependencies ──────────────────────────────────────────────
include(FetchContent)
include(cmake/FetchDependencies.cmake)

# ── Compiler warnings and flags ─────────────────────────────────────────
if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    add_compile_options(-Wall -Wextra -Wpedantic -Werror=return-type)
    add_compile_options(-Wno-unused-parameter)
endif()

# ── Sources ────────────────────────────────────────────────────────────
add_subdirectory(src)

# ── Tests ──────────────────────────────────────────────────────────────
if(TV_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()
