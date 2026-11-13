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
project(Aeon VERSION 1.0.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(MSVC)
    add_compile_options(/bigobj /EHsc /MP /std:c++17)
endif()

# Global output directories for multi-config generators (Visual Studio)
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/$<CONFIG>")
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/$<CONFIG>")
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/$<CONFIG>")
link_directories("${CMAKE_BINARY_DIR}/$<CONFIG>")

# ---------------------------------------------------------------------------
# Tier configuration
# ---------------------------------------------------------------------------
set(AEON_TARGET_TIER "Pro" CACHE STRING
    "Target OS tier: Pro / Modern / Extended / XPHi / XPLo / Retro")

message(STATUS "Aeon Browser build tier: ${AEON_TARGET_TIER}")

# Version string — sourced from core/AeonVersion.h (single source of truth).
# Do NOT define AEON_VERSION here; AeonVersion.h defines it as "0.19.0"
# and CMake's PROJECT_VERSION is metadata-only (used by install/packaging).

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
# Base component library (C++)
# Shared static library to decouple common component sources
# ---------------------------------------------------------------------------
add_library(aeon_component_base STATIC
    core/aeon_component.cpp
    core/session/SessionManager.cpp
    core/ui/TabGroupManager.cpp
)
target_include_directories(aeon_component_base PUBLIC
    ${CMAKE_SOURCE_DIR}
    ${CMAKE_SOURCE_DIR}/core
    ${CMAKE_SOURCE_DIR}/core/session
    ${CMAKE_SOURCE_DIR}/core/ui
)
set_target_properties(aeon_component_base PROPERTIES
    ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/$<CONFIG>"
    LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/$<CONFIG>"
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/$<CONFIG>"
)

# ---------------------------------------------------------------------------
# SQLite3 database engine (C) — isolated static library
# ---------------------------------------------------------------------------
add_library(aeon_sqlite STATIC
    core/history/sqlite3.c
)
set_target_properties(aeon_sqlite PROPERTIES
    LINKER_LANGUAGE C
    COMPILE_FLAGS "/TC /bigobj /EHsc /FS"
    ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/$<CONFIG>"
    LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/$<CONFIG>"
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/$<CONFIG>"
)
target_compile_definitions(aeon_sqlite PRIVATE SQLITE_THREADSAFE=0 SQLITE_ENABLE_FTS5)
target_include_directories(aeon_sqlite PUBLIC core/history)

# ---------------------------------------------------------------------------
# Core browser executable (Aeon.exe)
# ---------------------------------------------------------------------------
set(CORE_SOURCES
    AeonMain.cpp
    core/probe/HardwareProbe.cpp
    core/engine/TierDispatcher.cpp
    core/engine/AeonBridge.cpp
    core/ui/BrowserChrome.cpp
    core/ui/SplitViewController.cpp
    core/ui/OmniSuggestHUD.cpp
    core/ui/Aeon.rc
    core/ui/EraChrome.cpp
    core/ui/AppMenu.cpp
    core/ui/BookmarkBar.cpp
    core/ui/BookmarkToast.cpp
    core/ui/ShieldHUD.cpp
    core/ui/DownloadButton.cpp
    core/network/CircumventionEngine.cpp
    core/network/NetworkSentinel.cpp
    core/network/DnsResolver.cpp
    core/network/I2pdShim.cpp
    core/protocol/NewTabHandler.cpp
    core/settings/SettingsEngine.cpp
    core/security/PasswordVault.cpp
    core/history/HistoryEngine.cpp
    core/download/DownloadManager.cpp
    core/memory/TabSleepManager.cpp
    core/tls/TlsAbstraction.cpp
    core/crash/CrashHandler.cpp
    core/crash/CrashKeys.cpp
    core/crash/Breadcrumbs.cpp
    core/crash/AeonLog.cpp
    core/extensions/ExtensionRuntime.cpp
    core/billing/OmniLicense.cpp
    updater/AutoUpdater.cpp
    privacy/ContentBlocker.cpp
    telemetry/PulseBridge.cpp
    ai/aeon_tab_intelligence.cpp
    ai/aeon_journey_analytics.cpp
    core/agent/AeonAgentPipe.cpp
)

add_executable(Aeon WIN32 ${CORE_SOURCES})
add_dependencies(Aeon aeon_sqlite)

# Windows system libraries
target_link_libraries(Aeon PRIVATE
    aeon_sqlite
    $<TARGET_FILE:aeon_component_base>
    kernel32 user32 gdi32 gdiplus shell32 advapi32 shlwapi ole32 oleaut32
    dwmapi uxtheme dbghelp
    crypt32     # DPAPI — password vault
    winhttp     # Corporate proxy detection + captive portal probe
    wininet     # WinInet — update manifest fetch
    wintrust    # Authenticode verification
    bcrypt      # SHA-256 for update integrity
    iphlpapi    # IP adapter info
    ws2_32      # Winsock
)

# ── Post-build: copy HTML resources to output dir ─────────────────────
add_custom_command(TARGET Aeon POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E make_directory "$<TARGET_FILE_DIR:Aeon>/newtab"
    COMMAND ${CMAKE_COMMAND} -E make_directory "$<TARGET_FILE_DIR:Aeon>/pages"
    COMMAND ${CMAKE_COMMAND} -E copy_directory
        "${CMAKE_SOURCE_DIR}/resources/newtab"
        "$<TARGET_FILE_DIR:Aeon>/newtab"
    COMMAND ${CMAKE_COMMAND} -E copy_directory
        "${CMAKE_SOURCE_DIR}/resources/pages"
        "$<TARGET_FILE_DIR:Aeon>/pages"
    COMMENT "[Aeon] Copying HTML resources to output"
)

# ---------------------------------------------------------------------------
# Rust Protocol Router (cdylib → aeon_router.dll)
# Only built for NT-based tiers (Win2000+). Win9x links it statically.
# ---------------------------------------------------------------------------
if(NOT AEON_TARGET_TIER STREQUAL "Retro")
    find_program(CARGO cargo REQUIRED)
    file(GLOB_RECURSE ROUTER_SOURCES
        "${CMAKE_SOURCE_DIR}/router/src/*.rs"
        "${CMAKE_SOURCE_DIR}/router/Cargo.toml"
    )
    set(ROUTER_DLL "${CMAKE_SOURCE_DIR}/router/target/release/aeon_router.dll")
    set(ROUTER_IMPLIB "${CMAKE_SOURCE_DIR}/router/target/release/aeon_router.dll.lib")

    add_custom_command(
        OUTPUT "${ROUTER_DLL}"
        COMMAND ${CMAKE_COMMAND} -DCMAKE_SOURCE_DIR=${CMAKE_SOURCE_DIR} -P "${CMAKE_SOURCE_DIR}/cmake/build_router.cmake"
        WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
        DEPENDS ${ROUTER_SOURCES}
        COMMENT "Verifying / building Rust protocol router..."
        VERBATIM
    )

    add_custom_target(aeon_router_build DEPENDS "${ROUTER_DLL}")
    add_dependencies(Aeon aeon_router_build)

    # Link import lib from Rust cdylib
    target_link_libraries(Aeon PRIVATE "${ROUTER_IMPLIB}")
endif()

# ---------------------------------------------------------------------------
# Install
# ---------------------------------------------------------------------------
install(TARGETS Aeon DESTINATION bin)

# ---------------------------------------------------------------------------
# Retro tier HTML4 GDI renderer DLL  (aeon_html4.dll)
# Built only when AEON_TARGET_TIER=Retro; loaded at runtime by TierDispatcher.
# ---------------------------------------------------------------------------
if(AEON_TARGET_TIER STREQUAL "Retro")
    add_compile_definitions(AEON_HTML4_BUILD)
    add_library(aeon_html4 SHARED
        retro/aeon_html4.c
    )
    target_compile_definitions(aeon_html4 PRIVATE AEON_HTML4_BUILD)
    target_link_libraries(aeon_html4 kernel32 user32 gdi32)
    set_target_properties(aeon_html4 PROPERTIES
        OUTPUT_NAME "aeon_html4"
        SUFFIX ".dll"
    )
    install(TARGETS aeon_html4 DESTINATION bin)
    add_dependencies(Aeon aeon_html4)
endif()

# ---------------------------------------------------------------------------
# Camoufox C++ Stealth Engine DLL (aeon_gecko_stealth.dll)
# Built for Pro/Modern/Extended tiers for dynamic domain anti-bot routing.
# ---------------------------------------------------------------------------
add_library(aeon_gecko_stealth SHARED
    engines/gecko_stealth/aeon_gecko_stealth.cpp
)
add_dependencies(aeon_gecko_stealth aeon_component_base)
target_compile_definitions(aeon_gecko_stealth PRIVATE AEON_GECKO_STEALTH_EXPORTS)
target_link_libraries(aeon_gecko_stealth PRIVATE $<TARGET_FILE:aeon_component_base> kernel32 user32 gdi32)
set_target_properties(aeon_gecko_stealth PROPERTIES
    OUTPUT_NAME "aeon_gecko_stealth"
    PDB_NAME "aeon_gecko_stealth"
    SUFFIX ".dll"
    ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/$<CONFIG>"
    LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/$<CONFIG>"
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/$<CONFIG>"
)
add_dependencies(Aeon aeon_gecko_stealth)

# ---------------------------------------------------------------------------
# Aeon Local AI Engine DLL (aeon_ai.dll)
# Built for Pro/Modern/Extended tiers using Gemma 4 / GGUF model formats.
# ---------------------------------------------------------------------------
add_library(aeon_ai SHARED
    engines/aeon_ai.cpp
)
add_dependencies(aeon_ai aeon_component_base aeon_gecko_stealth)
target_compile_definitions(aeon_ai PRIVATE AEON_AI_EXPORTS)
target_compile_options(aeon_ai PRIVATE /EHsc /std:c++17)
target_compile_features(aeon_ai PRIVATE cxx_std_17)
target_link_libraries(aeon_ai PRIVATE $<TARGET_FILE:aeon_component_base> kernel32 user32 gdi32 shell32 advapi32)
set_target_properties(aeon_ai PROPERTIES
    OUTPUT_NAME "aeon_ai"
    PDB_NAME "aeon_ai"
    CXX_STANDARD 17
    CXX_STANDARD_REQUIRED ON
    ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/$<CONFIG>"
    LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/$<CONFIG>"
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/$<CONFIG>"
)
add_dependencies(Aeon aeon_ai)
target_link_libraries(Aeon PRIVATE aeon_ai)

install(FILES
    "${CMAKE_SOURCE_DIR}/router/target/release/aeon_router.dll"
    DESTINATION bin OPTIONAL)

# ---------------------------------------------------------------------------
# Unit & Integration Tests
# ---------------------------------------------------------------------------
add_executable(test_browser_chrome_m1 core/tests/test_browser_chrome_m1.cpp)
target_compile_options(test_browser_chrome_m1 PRIVATE /EHsc /std:c++17)
target_link_libraries(test_browser_chrome_m1 PRIVATE user32 gdi32)

add_executable(test_loading_bar core/tests/test_loading_bar.cpp)
target_compile_options(test_loading_bar PRIVATE /EHsc /std:c++17)
target_link_libraries(test_loading_bar PRIVATE user32 gdi32)

add_executable(test_browser_chrome_m2
    core/tests/test_browser_chrome_m2.cpp
    core/ui/TabGroupManager.cpp
    core/ui/SplitViewController.cpp
    core/memory/TabSleepManager.cpp
    core/session/SessionManager.cpp
)
target_compile_options(test_browser_chrome_m2 PRIVATE /EHsc /std:c++17)
target_include_directories(test_browser_chrome_m2 PRIVATE ${CMAKE_SOURCE_DIR} ${CMAKE_SOURCE_DIR}/core ${CMAKE_SOURCE_DIR}/core/session ${CMAKE_SOURCE_DIR}/core/ui)
target_link_libraries(test_browser_chrome_m2 PRIVATE user32 gdi32 gdiplus dwmapi shell32)

add_executable(test_browser_ui_m2_m3_m5 core/tests/test_browser_ui_m2_m3_m5.cpp)
target_compile_options(test_browser_ui_m2_m3_m5 PRIVATE /EHsc /std:c++17)
target_link_libraries(test_browser_ui_m2_m3_m5 PRIVATE user32 gdi32)

add_executable(test_cname_and_window_ai
    core/tests/test_cname_and_window_ai.cpp
    core/network/DnsResolver.cpp
    core/extensions/ExtensionRuntime.cpp
    core/security/PasswordVault.cpp
    core/probe/HardwareProbe.cpp
)
target_compile_options(test_cname_and_window_ai PRIVATE /EHsc /std:c++17)
target_include_directories(test_cname_and_window_ai PRIVATE
    ${CMAKE_SOURCE_DIR}
    ${CMAKE_SOURCE_DIR}/core
    ${CMAKE_SOURCE_DIR}/engines
    ${CMAKE_SOURCE_DIR}/network
    ${CMAKE_SOURCE_DIR}/core/history
)
add_dependencies(test_cname_and_window_ai aeon_sqlite)
target_link_libraries(test_cname_and_window_ai PRIVATE
    aeon_ai
    aeon_sqlite
    $<TARGET_FILE:aeon_component_base>
    user32 gdi32 shell32 advapi32 winhttp ws2_32 iphlpapi crypt32
)
