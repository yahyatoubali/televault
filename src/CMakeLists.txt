set(CORE_SOURCES
    main.cpp
    util/logging.cpp
    util/config.cpp
    util/retry.cpp
    util/format.cpp
    util/platform.cpp
    crypto/aes256gcm.cpp
    crypto/kdf.cpp
    crypto/stream.cpp
    compress/zstd.cpp
    compress/stream.cpp
    chunker/chunker.cpp
    chunker/hash.cpp
    chunker/writer.cpp
    cli/cli.cpp
    preview/preview.cpp
    gc/gc.cpp
)

# Stubs for missing tdlib functions
if(NOT TV_BUILD_TDLIB)
    list(APPEND CORE_SOURCES stubs.cpp)
endif()

set(CORE_HEADERS
    util/logging.hpp
    util/config.hpp
    util/retry.hpp
    util/format.hpp
    util/platform.hpp
    crypto/aes256gcm.hpp
    crypto/kdf.hpp
    crypto/stream.hpp
    compress/zstd.hpp
    compress/stream.hpp
    chunker/chunker.hpp
    chunker/hash.hpp
    chunker/writer.hpp
    cli/cli.hpp
    cli/progress.hpp
    preview/preview.hpp
    gc/gc.hpp
    models/file_metadata.hpp
    models/vault_index.hpp
    models/snapshot.hpp
    models/config.hpp
    models/transfer_progress.hpp
)

# ── Tdlib-dependent sources ────────────────────────────────────────────
if(TV_BUILD_TDLIB)
    list(APPEND CORE_SOURCES
        telegram/client.cpp
        telegram/auth.cpp
        telegram/session.cpp
        core/vault.cpp
        core/transfer.cpp
        core/index.cpp
        core/app_context.cpp
        backup/engine.cpp
        backup/snapshot.cpp
        watcher/watcher.cpp
        schedule/schedule.cpp
        schedule/systemd.cpp
    )
    list(APPEND CORE_HEADERS
        telegram/client.hpp
        telegram/auth.hpp
        telegram/session.hpp
        core/vault.hpp
        core/transfer.hpp
        core/index.hpp
        core/app_context.hpp
        backup/engine.hpp
        backup/snapshot.hpp
        watcher/watcher.hpp
        schedule/schedule.hpp
        schedule/systemd.hpp
    )
endif()

# ── Optional features ──────────────────────────────────────────────────
if(TV_BUILD_FUSE)
    list(APPEND CORE_SOURCES fuse/fuse_ops.cpp fuse/cache.cpp)
    list(APPEND CORE_HEADERS fuse/fuse_ops.hpp fuse/cache.hpp)
    find_package(PkgConfig REQUIRED)
    pkg_check_modules(FUSE3 REQUIRED IMPORTED_TARGET fuse3)
endif()

if(TV_BUILD_WEBDAV)
    list(APPEND CORE_SOURCES webdav/server.cpp webdav/handler.cpp)
    list(APPEND CORE_HEADERS webdav/server.hpp webdav/handler.hpp)
endif()

if(TV_BUILD_TUI)
    list(APPEND CORE_SOURCES tui/tui.cpp)
    list(APPEND CORE_HEADERS tui/tui.hpp)
endif()

# ── Core library (shared among tests and main binary) ──────────────────
add_library(tv_core OBJECT ${CORE_SOURCES} ${CORE_HEADERS})
target_include_directories(tv_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_definitions(tv_core PRIVATE TELEVAULT_VERSION="${PROJECT_VERSION}")
target_compile_definitions(tv_core PRIVATE $<$<BOOL:${TV_BUILD_TDLIB}>:TV_BUILD_TDLIB>)

target_link_libraries(tv_core PUBLIC
    CLI11::CLI11
    nlohmann_json::nlohmann_json
    spdlog::spdlog
    OpenSSL::SSL
    OpenSSL::Crypto
    PkgConfig::ZSTD
    PkgConfig::BLAKE3
    pthread
)

if(TV_BUILD_TDLIB)
    target_include_directories(tv_core PUBLIC ${Boost_INCLUDE_DIRS})
    # Link tdlib in correct order for static linking (reverse dep graph)
    target_link_libraries(tv_core PUBLIC
        Td::tdclient
        Td::tdcore
        Td::tdapi
        Td::tdnet
        Td::tddb
        Td::tdactor
        Td::tdutils
        Td::tdsqlite)
endif()

if(TV_BUILD_FUSE)
    target_link_libraries(tv_core PUBLIC PkgConfig::FUSE3)
endif()

if(TV_BUILD_TUI)
    target_link_libraries(tv_core PUBLIC ftxui::screen ftxui::dom ftxui::component)
endif()

# ── Main binary ────────────────────────────────────────────────────────
add_executable(televault)
target_link_libraries(televault PRIVATE tv_core)

if(TV_BUILD_TDLIB)
    # Use --whole-archive to force all tdlib objects into the binary
    target_link_options(televault PRIVATE
        "LINKER:--whole-archive"
        "LINKER:$<TARGET_LINKER_FILE:Td::tdclient>"
        "LINKER:$<TARGET_LINKER_FILE:Td::tdcore>"
        "LINKER:$<TARGET_LINKER_FILE:Td::tdapi>"
        "LINKER:$<TARGET_LINKER_FILE:Td::tdnet>"
        "LINKER:$<TARGET_LINKER_FILE:Td::tddb>"
        "LINKER:$<TARGET_LINKER_FILE:Td::tdactor>"
        "LINKER:$<TARGET_LINKER_FILE:Td::tdutils>"
        "LINKER:$<TARGET_LINKER_FILE:Td::tdsqlite>"
        "LINKER:--no-whole-archive")
endif()

# ── Alias ──────────────────────────────────────────────────────────────
add_executable(tvt ALIAS televault)

# ── Install ────────────────────────────────────────────────────────────
install(TARGETS televault DESTINATION bin)
