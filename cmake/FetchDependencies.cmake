# ── CLI11 (header-only) ────────────────────────────────────────────────
FetchContent_Declare(
    CLI11
    GIT_REPOSITORY https://github.com/CLIUtils/CLI11.git
    GIT_TAG v2.4.2
)

# ── nlohmann/json (header-only) ────────────────────────────────────────
FetchContent_Declare(
    nlohmann_json
    GIT_REPOSITORY https://github.com/nlohmann/json.git
    GIT_TAG v3.11.3
)

# ── spdlog (header-only) ──────────────────────────────────────────────
FetchContent_Declare(
    spdlog
    GIT_REPOSITORY https://github.com/gabime/spdlog.git
    GIT_TAG v1.14.1
)

# ── Tdlib (optional, requires compilation) ────────────────────────────
if(TV_BUILD_TDLIB)
    find_package(Td QUIET)
    if(NOT Td_FOUND)
        message(STATUS "tdlib not found — will fetch from GitHub (this may take a while)")
        FetchContent_Declare(
            tdlib
            GIT_REPOSITORY https://github.com/tdlib/td.git
            GIT_TAG v1.8.0
        )
    else()
        message(STATUS "Found tdlib: ${Td_DIR}")
    endif()
endif()

# ── Google Test ────────────────────────────────────────────────────────
FetchContent_Declare(
    googletest
    GIT_REPOSITORY https://github.com/google/googletest.git
    GIT_TAG v1.15.2
)

# ── FTXUI ──────────────────────────────────────────────────────────────
if(TV_BUILD_TUI)
    FetchContent_Declare(
        ftxui
        GIT_REPOSITORY https://github.com/ArthurSonzogni/FTXUI.git
        GIT_TAG v5.0.0
    )
endif()

# ── Boost (Asio + Beast — header-only, needed for async/WebDAV/tdlib) ──
# Manually find Boost headers since FindBoost module is removed in CMake 4
find_path(Boost_INCLUDE_DIRS boost/asio.hpp
    HINTS
        /opt/homebrew/include
        $ENV{HOME}/.local/include
        /usr/include
        /usr/local/include
        ${CMAKE_PREFIX_PATH}/include
        ${BOOST_ROOT}/include
)
if(Boost_INCLUDE_DIRS)
    message(STATUS "Found Boost headers: ${Boost_INCLUDE_DIRS}")
else()
    message(FATAL_ERROR "Boost headers not found — install boost or set BOOST_ROOT")
endif()

# ── Nayuki QR-Code-generator (MIT; terminal QR rendering for login) ──
# Only qrcodegen.{hpp,cpp} are needed; fetch the pinned files directly.
set(QRGEN_DIR "${CMAKE_BINARY_DIR}/_qrgen")
set(QRGEN_HPP "${QRGEN_DIR}/qrcodegen.hpp")
set(QRGEN_CPP "${QRGEN_DIR}/qrcodegen.cpp")
# A previous failed fetch may have left 0-byte files; refetch those too.
foreach(_f IN ITEMS "${QRGEN_HPP}" "${QRGEN_CPP}")
    if(EXISTS "${_f}")
        file(SIZE "${_f}" _sz)
        if(_sz LESS 1000)
            file(REMOVE "${_f}")
        endif()
    endif()
endforeach()
if(NOT EXISTS "${QRGEN_HPP}" OR NOT EXISTS "${QRGEN_CPP}")
    message(STATUS "Downloading Nayuki QR-Code-generator v1.8.0...")
    file(DOWNLOAD
        "https://raw.githubusercontent.com/nayuki/QR-Code-generator/v1.8.0/cpp/qrcodegen.hpp"
        "${QRGEN_HPP}" TLS_VERIFY ON)
    file(DOWNLOAD
        "https://raw.githubusercontent.com/nayuki/QR-Code-generator/v1.8.0/cpp/qrcodegen.cpp"
        "${QRGEN_CPP}" TLS_VERIFY ON)
endif()
if(NOT EXISTS "${QRGEN_HPP}" OR NOT EXISTS "${QRGEN_CPP}")
    message(FATAL_ERROR "Could not fetch Nayuki QR-Code-generator; check network or vendor cpp/qrcodegen.{hpp,cpp}")
endif()
add_library(qrcodegen STATIC "${QRGEN_CPP}")
target_include_directories(qrcodegen PUBLIC "${QRGEN_DIR}")

# ── Make available ─────────────────────────────────────────────────────
FetchContent_MakeAvailable(
    CLI11
    nlohmann_json
    spdlog
)

# Only fetch GTest when tests are enabled; otherwise its install() rules
# pollute `cmake --install` (Homebrew, ~/.local prefix) and slow configure.
if(TV_BUILD_TESTS)
    FetchContent_MakeAvailable(googletest)
endif()

if(TV_BUILD_TDLIB AND NOT Td_FOUND)
    # TDLib v1.8.0 is a C++14-era codebase: newer language modes break it
    # (AppleClang/libc++ rejects incomplete-type traits it relies on), so
    # configure it explicitly as C++14 while our own code stays C++23.
    set(CMAKE_CXX_STANDARD 14)
    FetchContent_MakeAvailable(tdlib)
    set(CMAKE_CXX_STANDARD 23)
endif()

if(TV_BUILD_TUI)
    FetchContent_MakeAvailable(ftxui)
endif()
