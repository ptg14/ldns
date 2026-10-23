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

option(LDNS_BUILD_TESTS "Build unit tests" ON)
option(LDNS_SANITIZE "Enable AddressSanitizer/UBSan (GCC/Clang)" OFF)
option(LDNS_WERROR "Treat compiler warnings as errors" ON)

# ---------------------------------------------------------------------------
# Compiler warnings
# ---------------------------------------------------------------------------
function(ldns_set_warnings target)
    if(MSVC)
        target_compile_options(${target} PRIVATE /W4 /permissive- /utf-8 /Zc:__cplusplus)
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
    src/core/server/acl.cpp
    src/core/server/query_handler.cpp
    src/core/server/dns_server.cpp
    src/core/util/logger.cpp
    src/core/util/id.cpp
    src/core/util/hlc.cpp
    src/core/version.cpp
)
target_include_directories(ldns_core PUBLIC src)
target_link_libraries(ldns_core PUBLIC spdlog::spdlog ldns_asio)
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
