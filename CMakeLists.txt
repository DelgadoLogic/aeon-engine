# AeonBrowser — Root CMakeLists.txt
# DelgadoLogic | Lead Systems Architect
#
# BUILD INSTRUCTIONS:
#   mkdir build && cd build
#   cmake .. -DCMAKE_BUILD_TYPE=Release -DAEON_TARGET_TIER=Pro
#   cmake --build . --config Release
#
# TIER-SPECIFIC FLAGS:
#   -DAEON_TARGET_TIER=Pro       Win10/11  — Blink + Rust router (MSVC x64)
#   -DAEON_TARGET_TIER=Modern    Win8.x    — Blink + Rust router (MSVC x86/x64)
#   -DAEON_TARGET_TIER=Extended  Win7/Vista — Gecko + Rust router (MSVC x86)
#   -DAEON_TARGET_TIER=XPHi      WinXP SSE2 — Gecko (MSVC v7.1 / v8.0)
#   -DAEON_TARGET_TIER=XPLo      WinXP/2000 — HTML4 renderer (MSVC 6.0 or 2005)
#   -DAEON_TARGET_TIER=Retro     Win9x/3.x  — HTML4 renderer (OpenWatcom 2.0)
#
# IT TROUBLESHOOTING:
#   CMake can't find Rust: ensure `cargo` is in PATH. cargo 1.70+ required.
#   XP build fails with "subsystem version": set AEON_XP_COMPAT=ON.
#   Win9x build: use Open Watcom toolchain file: -DCMAKE_TOOLCHAIN_FILE=cmake/watcom.cmake

cmake_minimum_required(VERSION 3.20)
project(AeonBrowser VERSION 1.0.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# ---------------------------------------------------------------------------
# Tier configuration
# ---------------------------------------------------------------------------
set(AEON_TARGET_TIER "Pro" CACHE STRING
    "Target OS tier: Pro / Modern / Extended / XPHi / XPLo / Retro")

message(STATUS "Aeon Browser build tier: ${AEON_TARGET_TIER}")

# Tier-specific preprocessor defines
if(AEON_TARGET_TIER STREQUAL "Pro" OR AEON_TARGET_TIER STREQUAL "Modern")
    add_compile_definitions(AEON_TIER_MODERN AEON_BLINK_ENGINE)
elseif(AEON_TARGET_TIER STREQUAL "Extended")
    add_compile_definitions(AEON_TIER_EXTENDED AEON_GECKO_ENGINE)
elseif(AEON_TARGET_TIER STREQUAL "XPHi" OR AEON_TARGET_TIER STREQUAL "XPLo")
    add_compile_definitions(AEON_TIER_XP AEON_GECKO_ENGINE AEON_XP_COMPAT)
    # Target WINXP subsystem: MSVC must use /SUBSYSTEM:WINDOWS,5.01
    if(MSVC)
        add_link_options("/SUBSYSTEM:WINDOWS,5.01")
    endif()
elseif(AEON_TARGET_TIER STREQUAL "Retro")
    add_compile_definitions(AEON_TIER_RETRO AEON_HTML4_ENGINE)
endif()

# ---------------------------------------------------------------------------
# Browser core (C++)
# ---------------------------------------------------------------------------
set(CORE_SOURCES
    core/main.cpp
    core/probe/HardwareProbe.cpp
    core/engine/TierDispatcher.cpp
    core/ui/EraChrome.cpp
    core/tls/TlsAbstraction.cpp
    core/extensions/ExtensionRuntime.cpp
    core/session/SessionManager.cpp
    core/crash/CrashHandler.cpp
    core/memory/TabSleepManager.cpp
    privacy/ContentBlocker.cpp
    telemetry/PulseBridge.cpp
)

add_executable(Aeon WIN32 ${CORE_SOURCES})

# Windows system libraries
target_link_libraries(Aeon
    kernel32 user32 gdi32 shell32 advapi32
    dwmapi dbghelp crypt32
    ws2_32  # Winsock for network checks
)

# ---------------------------------------------------------------------------
# Rust Protocol Router (cdylib → aeon_router.dll)
# Only built for NT-based tiers (Win2000+). Win9x links it statically.
# ---------------------------------------------------------------------------
if(NOT AEON_TARGET_TIER STREQUAL "Retro")
    find_program(CARGO cargo REQUIRED)
    set(ROUTER_LIB "${CMAKE_BINARY_DIR}/router/aeon_router.dll")

    add_custom_target(aeon_router_build ALL
        COMMAND ${CARGO} build --release
        WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}/router
        COMMENT "Building Rust protocol router..."
    )
    add_dependencies(Aeon aeon_router_build)

    # Link import lib from Rust cdylib
    target_link_libraries(Aeon
        "${CMAKE_SOURCE_DIR}/router/target/release/aeon_router.dll.lib")
endif()

# ---------------------------------------------------------------------------
# Install
# ---------------------------------------------------------------------------
install(TARGETS Aeon DESTINATION bin)
install(FILES
    "${CMAKE_SOURCE_DIR}/router/target/release/aeon_router.dll"
    DESTINATION bin OPTIONAL)
