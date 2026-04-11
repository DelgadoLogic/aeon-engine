# AeonBrowser — Blink Engine DLL (WebView2 Adapter)
# DelgadoLogic | Lead Systems Architect
#
# BUILD:
#   cmake -B build -S . -DCMAKE_BUILD_TYPE=Release
#   cmake --build build --config Release

cmake_minimum_required(VERSION 3.20)
project(AeonBlinkEngine VERSION 1.0.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

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
add_library(aeon_blink SHARED aeon_blink_stub.cpp)

target_compile_definitions(aeon_blink PRIVATE
    AEON_ENGINE_EXPORTS
    AEON_HAS_WEBVIEW2
    _WINDOWS
    UNICODE
    _UNICODE
    WIN32_LEAN_AND_MEAN
    NOMINMAX
)

# Include paths
target_include_directories(aeon_blink PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/../../core/engine"   # AeonEngine_Interface.h
    "${CMAKE_CURRENT_SOURCE_DIR}/../../core/probe"    # HardwareProbe.h (for SystemProfile)
    "${WV2_INCLUDE}"                                   # WebView2.h
    "${WIL_INCLUDE}"                                   # wil/com.h
)

# Link libraries
target_link_directories(aeon_blink PRIVATE "${WV2_LIB_DIR}")
target_link_libraries(aeon_blink PRIVATE
    WebView2LoaderStatic    # Static link — no WebView2Loader.dll dependency
    kernel32 user32 gdi32 shell32 ole32 oleaut32
    shlwapi advapi32
)

# Output name
set_target_properties(aeon_blink PROPERTIES
    OUTPUT_NAME "aeon_blink"
    SUFFIX ".dll"
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/out"
    LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/out"
)

# Post-build: copy to main build output
add_custom_command(TARGET aeon_blink POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy
        "$<TARGET_FILE:aeon_blink>"
        "${CMAKE_CURRENT_SOURCE_DIR}/../../dist_beta/aeon_blink.dll"
    COMMENT "[Aeon] Copying aeon_blink.dll to dist_beta"
)
