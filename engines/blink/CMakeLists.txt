# AeonBrowser — Blink Engine DLL (WebView2 Adapter)
# DelgadoLogic | Lead Systems Architect
#
# BUILD:
#   cmake -B build -S . -DCMAKE_BUILD_TYPE=Release
#   cmake --build build --config Release

cmake_minimum_required(VERSION 3.20)
set(CMAKE_TRY_COMPILE_TARGET_TYPE "STATIC_LIBRARY")
project(AeonBlinkEngine VERSION 1.0.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
if(MSVC)
    add_compile_options(/bigobj /EHsc /MP /std:c++17)
endif()

# ---------------------------------------------------------------------------
# WebView2 SDK paths (NuGet package)
# ---------------------------------------------------------------------------
set(WV2_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/packages/Microsoft.Web.WebView2")
set(WIL_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/packages/Microsoft.Windows.ImplementationLibrary")

set(WV2_INCLUDE "${WV2_ROOT}/build/native/include")
set(WV2_LIB_DIR "${WV2_ROOT}/build/native/x64")
set(WIL_INCLUDE "${WIL_ROOT}/include")

# Verify SDK exists
if(NOT EXISTS "${WV2_INCLUDE}/WebView2.h")
    message(FATAL_ERROR "WebView2 SDK not found at ${WV2_INCLUDE}. Run: nuget install Microsoft.Web.WebView2 -OutputDirectory packages -ExcludeVersion")
endif()

# ---------------------------------------------------------------------------
# aeon_blink.dll — WebView2-backed rendering engine
# ---------------------------------------------------------------------------
add_library(aeon_blink SHARED
    aeon_blink_stub.cpp
    "${CMAKE_CURRENT_SOURCE_DIR}/../../core/extensions/ExtensionRuntime.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/../../core/network/DnsResolver.cpp"
)

target_compile_definitions(aeon_blink PRIVATE
    AEON_ENGINE_EXPORTS
    AEON_HAS_WEBVIEW2
    _WINDOWS
    UNICODE
    _UNICODE
    WIN32_LEAN_AND_MEAN
    NOMINMAX
)

target_compile_features(aeon_blink PRIVATE cxx_std_17)
if(MSVC)
    target_compile_options(aeon_blink PRIVATE /std:c++17 /EHsc)
endif()

# Include paths
target_include_directories(aeon_blink PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/../../core/engine"     # AeonEngine_Interface.h
    "${CMAKE_CURRENT_SOURCE_DIR}/../../core/probe"      # HardwareProbe.h (for SystemProfile)
    "${CMAKE_CURRENT_SOURCE_DIR}/../../core/extensions" # ExtensionRuntime.h (NativeAdBlock)
    "${CMAKE_CURRENT_SOURCE_DIR}/../../core/network"
    "${CMAKE_CURRENT_SOURCE_DIR}/../../engines"
    "${CMAKE_CURRENT_SOURCE_DIR}/../.."
    "${WV2_INCLUDE}"                                     # WebView2.h
    "${WIL_INCLUDE}"                                     # wil/com.h
)

# Link libraries
target_link_directories(aeon_blink PRIVATE
    "${WV2_LIB_DIR}"
    "${CMAKE_CURRENT_SOURCE_DIR}/../../build/Pro/$<CONFIG>"
    "${CMAKE_CURRENT_SOURCE_DIR}/../../build/Pro/Release"
    "${CMAKE_CURRENT_SOURCE_DIR}/../../build/Modern/$<CONFIG>"
)
target_link_libraries(aeon_blink PRIVATE
    aeon_ai.lib
    "${WV2_LIB_DIR}/WebView2LoaderStatic.lib"
    kernel32.lib user32.lib gdi32.lib shell32.lib ole32.lib oleaut32.lib
    shlwapi.lib advapi32.lib crypt32.lib ws2_32.lib iphlpapi.lib winhttp.lib
)

# Output name
set_target_properties(aeon_blink PROPERTIES
    OUTPUT_NAME "aeon_blink"
    SUFFIX ".dll"
    ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/out/$<CONFIG>"
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/out/$<CONFIG>"
    LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/out/$<CONFIG>"
)

# Post-build: copy to dist_beta if it exists (skipped on CI)
add_custom_command(TARGET aeon_blink POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E make_directory
        "${CMAKE_CURRENT_SOURCE_DIR}/../../dist_beta"
    COMMAND ${CMAKE_COMMAND} -DSRC="$<TARGET_FILE:aeon_blink>" -DDST="${CMAKE_CURRENT_SOURCE_DIR}/../../dist_beta/aeon_blink.dll" -P "${CMAKE_CURRENT_SOURCE_DIR}/../../cmake/copy_safe.cmake"
    COMMENT "[Aeon] Copying aeon_blink.dll to dist_beta"
)
