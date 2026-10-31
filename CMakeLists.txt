cmake_minimum_required(VERSION 3.25)

project(ldns
    VERSION 0.1.0
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
# Executable
# ---------------------------------------------------------------------------
add_executable(ldns src/main.cpp)
target_link_libraries(ldns PRIVATE ldns_core CLI11::CLI11)
ldns_set_warnings(ldns)

# ---------------------------------------------------------------------------
# Tests
# ---------------------------------------------------------------------------
if(LDNS_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()
