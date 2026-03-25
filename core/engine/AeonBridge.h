// AeonBrowser — AeonBridge.h
// DelgadoLogic | Engine Team
#pragma once
#include <windows.h>
#include <string>

// Custom window message for UI-thread operations
#define WM_AEONBRIDGE  (WM_USER + 0x7AE)

// wParam values for WM_AEONBRIDGE
#define BRIDGE_CMD_BROWSE_DL   1   // Open folder-picker for download path
#define BRIDGE_CMD_UPDATE_DONE 2   // AutoUpdater finished — notify UI
#define BRIDGE_CMD_NAVIGATE    3   // Navigate to URL in lParam (LPSTR)

// Callback type for navigation requests coming from JS
typedef void(*AeonBridgeNavigateFn)(const char* url);

namespace AeonBridge {
    // Call once at startup, before any aeon:// page is served.
    void Init(HWND mainHwnd, AeonBridgeNavigateFn navigateFn);

    // Dispatch a JS method call to the correct C++ subsystem.
    // Returns JSON result string.
    std::string Dispatch(const char* method, const char* argsJson);

    // Set a single setting by key/value (called by WebView2 host object adapter).
    void SetSetting(const char* key, const char* value);

    // Handle WM_AEONBRIDGE on the main message pump.
    void HandleWmAeonBridge(WPARAM wParam, LPARAM lParam);

    // Build the JavaScript bootstrap injected before </head> on every aeon:// page.
    // Populates window.__aeon.settings + window.__aeon.network from live C++ state.
    // Also provides a fallback window.aeonBridge in case WebView2 hasn't injected
    // its IDispatch host object yet.
    std::string BuildInjectionScript();
}
