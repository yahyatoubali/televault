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

# ── Make available ─────────────────────────────────────────────────────
FetchContent_MakeAvailable(
    CLI11
    nlohmann_json
    spdlog
    googletest
)

if(TV_BUILD_TDLIB AND NOT Td_FOUND)
    FetchContent_MakeAvailable(tdlib)
endif()

if(TV_BUILD_TUI)
    FetchContent_MakeAvailable(ftxui)
endif()
