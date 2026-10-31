cmake_minimum_required(VERSION 3.25)

project(ldns
    VERSION 1.0.0
    DESCRIPTION "Peer-to-peer local DNS for LAN"
    LANGUAGES CXX
)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# Match the static vcpkg triplet (x64-windows-static) which links the static CRT.
if(MSVC)
    set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
endif()

if(WIN32)
    add_compile_definitions(_WIN32_WINNT=0x0601 WIN32_LEAN_AND_MEAN)
endif()

option(LDNS_BUILD_TESTS "Build unit tests" ON)
option(LDNS_SANITIZE "Enable AddressSanitizer/UBSan (GCC/Clang)" OFF)
option(LDNS_WERROR "Treat compiler warnings as errors" ON)

# ---------------------------------------------------------------------------
# Compiler warnings
# ---------------------------------------------------------------------------
function(ldns_set_warnings target)
    if(MSVC)
        target_compile_options(${target} PRIVATE /W4 /permissive- /utf-8 /Zc:__cplusplus /FS)
        if(LDNS_WERROR)
            target_compile_options(${target} PRIVATE /WX)
        endif()
    else()
        target_compile_options(${target} PRIVATE -Wall -Wextra -Wpedantic -Wshadow -Wconversion)
        if(LDNS_WERROR)
            target_compile_options(${target} PRIVATE -Werror)
        endif()
    endif()
endfunction()

if(LDNS_SANITIZE)
    add_compile_options(-fsanitize=address,undefined -fno-omit-frame-pointer)
    add_link_options(-fsanitize=address,undefined)
endif()

find_package(spdlog CONFIG REQUIRED)
find_package(CLI11 CONFIG REQUIRED)
find_package(doctest CONFIG REQUIRED)
find_package(asio CONFIG REQUIRED)
find_package(nlohmann_json CONFIG REQUIRED)
find_package(unofficial-sqlite3 CONFIG REQUIRED)
find_package(tomlplusplus CONFIG REQUIRED)
find_package(unofficial-sodium CONFIG REQUIRED)

# Asio is header-only; ASIO_NO_DEPRECATED keeps us on the modern API.
add_library(ldns_asio INTERFACE)
target_link_libraries(ldns_asio INTERFACE asio::asio)
target_compile_definitions(ldns_asio INTERFACE ASIO_NO_DEPRECATED ASIO_STANDALONE)

# ---------------------------------------------------------------------------
# Core library (UI-agnostic). Grows milestone by milestone per docs/PLAN.md.
# ---------------------------------------------------------------------------
add_library(ldns_core STATIC
    src/core/dns/name.cpp
    src/core/dns/message.cpp
    src/core/dns/local_zone.cpp
    src/core/net/interfaces.cpp
    src/core/server/acl.cpp
    src/core/server/query_handler.cpp
    src/core/server/dns_server.cpp
    src/core/util/logger.cpp
    src/core/util/id.cpp
    src/core/util/hlc.cpp
    src/core/version.cpp
    src/core/store/store.cpp
    src/core/store/migrations.cpp
    src/core/config/config.cpp
    src/core/config/paths.cpp
    src/core/domain/self_address.cpp
    src/core/domain/public_tlds.cpp
    src/core/domain/domain_service.cpp
    src/core/domain/import_export.cpp
    src/core/cli/commands.cpp
    src/core/runtime/runtime.cpp
    src/core/core_api.cpp
    src/core/net/system_dns.cpp
    src/core/resolver/cache.cpp
    src/core/resolver/upstream_client.cpp
    src/core/resolver/query_log.cpp
    src/core/resolver/pipeline.cpp
    src/core/resolver/peer_ranking.cpp
    src/core/resolver/peer_resolver.cpp
    src/core/net/crypto.cpp
    src/core/net/beacon.cpp
    src/core/net/discovery.cpp
    src/core/net/protocol_message.cpp
    src/core/net/session.cpp
    src/core/net/peer_manager.cpp
    src/core/registry/registry.cpp
    src/core/registry/sync_manager.cpp
    src/ipc/json_rpc.cpp
    src/ipc/ipc_server.cpp
    src/ipc/ipc_client.cpp
    src/os/doctor.cpp
    src/os/system_dns_manager.cpp
    src/os/firewall.cpp
    src/os/service.cpp
)
target_include_directories(ldns_core PUBLIC src)
target_link_libraries(ldns_core PUBLIC
    spdlog::spdlog
    ldns_asio
    nlohmann_json::nlohmann_json
    unofficial::sqlite3::sqlite3
    tomlplusplus::tomlplusplus
    unofficial-sodium::sodium
)
if(WIN32)
    # GetAdaptersAddresses (core/net/interfaces.cpp), SCM / AdvAPI (os/service.cpp).
    target_link_libraries(ldns_core PUBLIC iphlpapi ws2_32 advapi32)
endif()
target_compile_definitions(ldns_core PUBLIC LDNS_VERSION="${PROJECT_VERSION}")
ldns_set_warnings(ldns_core)

# ---------------------------------------------------------------------------
# UI library (Claude Code style inline TUI & terminal widgets)
# ---------------------------------------------------------------------------
add_library(ldns_ui STATIC
    src/ui/term/terminal.cpp
    src/ui/widgets/welcome_panel.cpp
    src/ui/widgets/slash_menu.cpp
    src/ui/widgets/dialog.cpp
    src/ui/widgets/spinner.cpp
    src/ui/widgets/status_line.cpp
    src/ui/widgets/input_box.cpp
    src/ui/widgets/trace_view.cpp
    src/ui/widgets/doctor_view.cpp
    src/ui/widgets/conflict_view.cpp
    src/ui/widgets/info_view.cpp
    src/ui/widgets/log_view.cpp
    src/ui/widgets/peers_view.cpp
    src/ui/widgets/upstream_view.cpp
    src/ui/widgets/network_view.cpp
    src/ui/inline_renderer.cpp
    src/ui/tui_app.cpp
)
target_include_directories(ldns_ui PUBLIC src)
target_link_libraries(ldns_ui PUBLIC ldns_core)
ldns_set_warnings(ldns_ui)

# ---------------------------------------------------------------------------
# Executable
# ---------------------------------------------------------------------------
add_executable(ldns src/main.cpp)
target_link_libraries(ldns PRIVATE ldns_core ldns_ui CLI11::CLI11)
ldns_set_warnings(ldns)

# ---------------------------------------------------------------------------
# Tests
# ---------------------------------------------------------------------------
if(LDNS_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()

# ---------------------------------------------------------------------------
# Installation
# ---------------------------------------------------------------------------
include(GNUInstallDirs)

install(TARGETS ldns
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
    COMPONENT Application
)

install(FILES config/config.example.toml
    DESTINATION ${CMAKE_INSTALL_DATADIR}/ldns
    COMPONENT Application
)

install(FILES LICENSE README.md
    DESTINATION ${CMAKE_INSTALL_DOCDIR}
    COMPONENT Application
)

if(UNIX AND NOT APPLE)
    install(FILES packaging/systemd/ldns.service
        DESTINATION lib/systemd/system
        COMPONENT Application
    )
endif()

# ---------------------------------------------------------------------------
# CPack Packaging
# ---------------------------------------------------------------------------
set(CPACK_PACKAGE_NAME "ldns")
set(CPACK_PACKAGE_VENDOR "Ldns Contributors")
set(CPACK_PACKAGE_DESCRIPTION_SUMMARY "Peer-to-peer local DNS for LAN")
set(CPACK_PACKAGE_VERSION "${PROJECT_VERSION}")
set(CPACK_PACKAGE_CONTACT "https://github.com/ldns/ldns")
set(CPACK_RESOURCE_FILE_LICENSE "${CMAKE_CURRENT_SOURCE_DIR}/LICENSE")
set(CPACK_RESOURCE_FILE_README "${CMAKE_CURRENT_SOURCE_DIR}/README.md")

if(WIN32)
    set(CPACK_GENERATOR "ZIP")
    set(CPACK_PACKAGE_FILE_NAME "ldns-${PROJECT_VERSION}-windows-x64")
else()
    set(CPACK_GENERATOR "TGZ;DEB")
    set(CPACK_PACKAGE_FILE_NAME "ldns-${PROJECT_VERSION}-linux-x86_64")

    # Debian package specifics
    set(CPACK_DEBIAN_PACKAGE_MAINTAINER "Ldns Contributors <maintainers@ldns.lan>")
    set(CPACK_DEBIAN_PACKAGE_SECTION "net")
    set(CPACK_DEBIAN_PACKAGE_PRIORITY "optional")
    set(CPACK_DEBIAN_PACKAGE_HOMEPAGE "https://github.com/ldns/ldns")
    set(CPACK_DEBIAN_PACKAGE_SHLIBDEPS OFF)
    set(CPACK_DEBIAN_PACKAGE_DEPENDS "libc6 (>= 2.34)")
    set(CPACK_DEBIAN_PACKAGE_CONTROL_EXTRA
        "${CMAKE_CURRENT_SOURCE_DIR}/packaging/deb/postinst;${CMAKE_CURRENT_SOURCE_DIR}/packaging/deb/prerm"
    )
endif()

include(CPack)

