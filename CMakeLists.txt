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

# Version string — used by AutoUpdater
add_compile_definitions(AEON_VERSION="${PROJECT_VERSION}")

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
    AeonMain.cpp
    core/probe/HardwareProbe.cpp
    core/engine/TierDispatcher.cpp
    core/engine/AeonBridge.cpp
    core/ui/BrowserChrome.cpp
    core/ui/EraChrome.cpp
    core/ui/AppMenu.cpp
    core/ui/BookmarkBar.cpp
    core/ui/BookmarkToast.cpp
    core/ui/DownloadButton.cpp
    core/network/CircumventionEngine.cpp
    core/network/NetworkSentinel.cpp
    core/network/DnsResolver.cpp
    core/network/I2pdShim.cpp
    core/protocol/NewTabHandler.cpp
    core/settings/SettingsEngine.cpp
    core/security/PasswordVault.cpp
    core/history/HistoryEngine.cpp
    core/history/sqlite3.c
    core/download/DownloadManager.cpp
    core/memory/TabSleepManager.cpp
    core/session/SessionManager.cpp
    core/tls/TlsAbstraction.cpp
    core/crash/CrashHandler.cpp
    core/extensions/ExtensionRuntime.cpp
    core/billing/OmniLicense.cpp
    core/aeon_component.cpp
    updater/AutoUpdater.cpp
    privacy/ContentBlocker.cpp
    telemetry/PulseBridge.cpp
    ai/aeon_tab_intelligence.cpp
    ai/aeon_journey_analytics.cpp
    core/agent/AeonAgentPipe.cpp
)

# SQLite3 — vendor, single-threaded (browser is single-process)
set_source_files_properties(core/history/sqlite3.c PROPERTIES
    COMPILE_DEFINITIONS "SQLITE_THREADSAFE=0;SQLITE_ENABLE_FTS5"
)

add_executable(Aeon WIN32 ${CORE_SOURCES})

# Windows system libraries
target_link_libraries(Aeon
    kernel32 user32 gdi32 shell32 advapi32 shlwapi ole32 oleaut32
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
    COMMAND ${CMAKE_COMMAND} -E copy
        "${CMAKE_SOURCE_DIR}/resources/newtab/newtab.html"
        "$<TARGET_FILE_DIR:Aeon>/newtab/"
    COMMAND ${CMAKE_COMMAND} -E copy
        "${CMAKE_SOURCE_DIR}/resources/pages/settings.html"
        "${CMAKE_SOURCE_DIR}/resources/pages/history.html"
        "${CMAKE_SOURCE_DIR}/resources/pages/downloads.html"
        "${CMAKE_SOURCE_DIR}/resources/pages/bookmarks.html"
        "${CMAKE_SOURCE_DIR}/resources/pages/passwords.html"
        "${CMAKE_SOURCE_DIR}/resources/pages/crash.html"
        "${CMAKE_SOURCE_DIR}/resources/pages/diagnostics.html"
        "${CMAKE_SOURCE_DIR}/resources/pages/drm-info.html"
        "$<TARGET_FILE_DIR:Aeon>/pages/"
    COMMENT "[Aeon] Copying HTML resources to output"
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
    # Copy alongside Aeon.exe post-build
    add_custom_command(TARGET aeon_html4 POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy
            "$<TARGET_FILE:aeon_html4>"
            "$<TARGET_FILE_DIR:Aeon>/aeon_html4.dll"
        COMMENT "[Aeon] Copying aeon_html4.dll to output"
    )
    add_dependencies(Aeon aeon_html4)
endif()

install(FILES
    "${CMAKE_SOURCE_DIR}/router/target/release/aeon_router.dll"
    DESTINATION bin OPTIONAL)
