// AeonBrowser — aeon_html4.h
// Retro HTML4 GDI renderer — DLL interface
#pragma once
#include <windows.h>
#ifdef _WIN32
#  ifdef AEON_HTML4_BUILD
#    define AEON_HTML4_API __declspec(dllexport)
#  else
#    define AEON_HTML4_API __declspec(dllimport)
#  endif
#  define AEON_HTML4_CALL __cdecl
#else
#  define AEON_HTML4_API
#  define AEON_HTML4_CALL
#endif

#ifdef __cplusplus
extern "C" {
#endif

// Initialize the HTML4 renderer. Call once at process start.
AEON_HTML4_API int  AEON_HTML4_CALL AeonHTML4_Init(void);

// Shut down and free resources.
AEON_HTML4_API void AEON_HTML4_CALL AeonHTML4_Shutdown(void);

// Render HTML into hwnd's client area (called from WM_PAINT).
// scrollY: vertical scroll offset in pixels.
// Returns: total document height in pixels (use for scrollbar range).
AEON_HTML4_API int AEON_HTML4_CALL AeonHTML4_Render(
    HWND hwnd, const char* html, int scrollY);

// Hit-test a mouse click. Returns href string to navigate to,
// or NULL if nothing was clicked.
// Call from WM_LBUTTONDOWN in the browser chrome.
AEON_HTML4_API const char* AEON_HTML4_CALL AeonHTML4_HitTest(int x, int y);

// API version — TierDispatcher checks this on load.
#define AEON_HTML4_API_VERSION 1

#ifdef __cplusplus
}
#endif
