# ── Core library (Independent of Telegram / Tdlib) ─────────────────────
set(CORE_SOURCES
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
    preview/preview.cpp
    gc/gc.cpp
)

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
    preview/preview.hpp
    gc/gc.hpp
    models/file_metadata.hpp
    models/vault_index.hpp
    models/snapshot.hpp
    models/config.hpp
    models/transfer_progress.hpp
)

add_library(tv_core STATIC ${CORE_SOURCES} ${CORE_HEADERS})
target_include_directories(tv_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_definitions(tv_core PUBLIC TELEVAULT_VERSION="${PROJECT_VERSION}")
target_compile_definitions(tv_core PUBLIC $<$<BOOL:${TV_BUILD_TDLIB}>:TV_BUILD_TDLIB>)

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

# ── Application & Services Layer ───────────────────────────────────────
set(APP_SOURCES
    cli/cli.cpp
)

set(APP_HEADERS
    cli/cli.hpp
    cli/progress.hpp
)

if(TV_BUILD_TDLIB)
    list(APPEND APP_SOURCES
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
    list(APPEND APP_HEADERS
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
else()
    list(APPEND APP_SOURCES stubs.cpp)
endif()

if(TV_BUILD_FUSE)
    list(APPEND APP_SOURCES fuse/fuse_ops.cpp fuse/cache.cpp)
    list(APPEND APP_HEADERS fuse/fuse_ops.hpp fuse/cache.hpp)
    find_package(PkgConfig REQUIRED)
    pkg_check_modules(FUSE3 REQUIRED IMPORTED_TARGET fuse3)
endif()

if(TV_BUILD_WEBDAV)
    list(APPEND APP_SOURCES webdav/server.cpp webdav/handler.cpp)
    list(APPEND APP_HEADERS webdav/server.hpp webdav/handler.hpp)
endif()

if(TV_BUILD_TUI)
    list(APPEND APP_SOURCES tui/tui.cpp)
    list(APPEND APP_HEADERS tui/tui.hpp)
endif()

add_library(tv_app STATIC ${APP_SOURCES} ${APP_HEADERS})
target_include_directories(tv_app PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_definitions(tv_app PUBLIC TELEVAULT_VERSION="${PROJECT_VERSION}")
target_compile_definitions(tv_app PUBLIC $<$<BOOL:${TV_BUILD_TDLIB}>:TV_BUILD_TDLIB>)
target_link_libraries(tv_app PUBLIC tv_core)

if(TV_BUILD_TDLIB)
    target_include_directories(tv_app PUBLIC ${Boost_INCLUDE_DIRS})
    target_link_libraries(tv_app PUBLIC
        "-Wl,--start-group"
        Td::tdclient
        Td::tdcore
        Td::tdapi
        Td::tdnet
        Td::tddb
        Td::tdactor
        Td::tdutils
        Td::tdsqlite
        OpenSSL::SSL
        OpenSSL::Crypto
        "-Wl,--end-group"
        ${CMAKE_DL_LIBS}
        z
    )
endif()

if(TV_BUILD_FUSE)
    target_link_libraries(tv_app PUBLIC PkgConfig::FUSE3)
endif()

if(TV_BUILD_TUI)
    target_link_libraries(tv_app PUBLIC ftxui::screen ftxui::dom ftxui::component)
endif()

# ── Main binary ────────────────────────────────────────────────────────
add_executable(televault main.cpp)
target_link_libraries(televault PRIVATE tv_app)

# ── Alias ──────────────────────────────────────────────────────────────
add_executable(tvt ALIAS televault)

# ── Install ────────────────────────────────────────────────────────────
install(TARGETS televault DESTINATION bin)
