// AeonBrowser — AeonEngine_Interface.h
// DelgadoLogic | Lead Systems Architect
//
// PURPOSE: Standardised ABI that ALL rendering engine DLLs must implement.
// This is the contract between the C++ browser core and any engine DLL
// (aeon_blink.dll, aeon_gecko.dll, aeon_html4.dll).
//
// DESIGN PHILOSOPHY:
//   - Pure C ABI: No C++ classes, no virtual tables, no name mangling.
//     This is critical for cross-compiler compat (MSVC core vs MingW engine).
//   - Minimal surface: Only the calls the core MUST make go here.
//     Everything else is engine-internal.
//   - Stable versioning: AEON_ENGINE_ABI_VERSION must be bumped on any change.
//     Core refuses to load DLLs with incompatible ABI versions.
//
// TO IMPLEMENT A NEW ENGINE:
//   1. Create a new DLL project.
//   2. Include this header.
//   3. Implement all functions marked [REQUIRED].
//   4. Export them with exactly these names.
//   5. Set AEON_ENGINE_ABI_VERSION constant to current value (see below).

#ifndef AEON_ENGINE_INTERFACE_H
#define AEON_ENGINE_INTERFACE_H

#ifdef __cplusplus
extern "C" {
#endif

#define AEON_ENGINE_ABI_VERSION 1  // Bump this when interface changes

// ---------------------------------------------------------------------------
// Lifecycle [REQUIRED]
// ---------------------------------------------------------------------------

// Called after LoadLibrary(). Returns 1 = success, 0 = failure.
// profile = the SystemProfile from HardwareProbe
// hInst   = HINSTANCE of the host browser process
typedef int  (__cdecl *AeonEngine_Init_t)(const void* profile, void* hInst);

// Called at browser shutdown. Release all resources. Must NOT crash if called
// before Init (defensive shutdown during error recovery).
typedef void (__cdecl *AeonEngine_Shutdown_t)(void);

// Return this DLL's ABI version. Core aborts if != AEON_ENGINE_ABI_VERSION.
typedef int  (__cdecl *AeonEngine_AbiVersion_t)(void);

// ---------------------------------------------------------------------------
// Navigation [REQUIRED]
// ---------------------------------------------------------------------------

// Navigate to a URL in the given tab (tab_id = 0 for primary tab).
// Returns a request ID, or 0 on failure.
typedef unsigned int (__cdecl *AeonEngine_Navigate_t)(
    unsigned int tab_id,
    const char*  url,
    const char*  referrer  /* may be NULL */
);

// Stop the current load for a tab.
typedef void (__cdecl *AeonEngine_Stop_t)(unsigned int tab_id);

// Reload the current page for a tab.
typedef void (__cdecl *AeonEngine_Reload_t)(unsigned int tab_id, int bypass_cache);

// Go back / forward in history.
typedef void (__cdecl *AeonEngine_GoBack_t)   (unsigned int tab_id);
typedef void (__cdecl *AeonEngine_GoForward_t)(unsigned int tab_id);

// ---------------------------------------------------------------------------
// Tab Management [REQUIRED]
// ---------------------------------------------------------------------------

// Create a new tab, optionally with an initial URL. Returns new tab_id.
typedef unsigned int (__cdecl *AeonEngine_NewTab_t)(
    void*       parent_hwnd,
    const char* initial_url  /* may be NULL */
);

// Close a tab. Engine must clean up its process/thread for this tab.
typedef void (__cdecl *AeonEngine_CloseTab_t)(unsigned int tab_id);

// Focus a tab (engine should paint it visible, suspend others).
typedef void (__cdecl *AeonEngine_FocusTab_t)(unsigned int tab_id);

// ---------------------------------------------------------------------------
// Content Interaction [REQUIRED]
// ---------------------------------------------------------------------------

// Inject CSS into the current page (used by ContentBlocker cosmetic rules).
typedef void (__cdecl *AeonEngine_InjectCSS_t)(
    unsigned int tab_id,
    const char*  css
);

// Inject JS before any page scripts run (used by fingerprint guard).
typedef void (__cdecl *AeonEngine_InjectEarlyJS_t)(
    unsigned int tab_id,
    const char*  js
);

// Get the current page title (for tab label). Writes to buf.
typedef void (__cdecl *AeonEngine_GetTitle_t)(
    unsigned int tab_id,
    char*        buf,
    unsigned int buf_len
);

// Get the current page URL. Writes to buf.
typedef void (__cdecl *AeonEngine_GetUrl_t)(
    unsigned int tab_id,
    char*        buf,
    unsigned int buf_len
);

// ---------------------------------------------------------------------------
// Event Callbacks [REQUIRED]
// Engine calls these function pointers to notify the core of events.
// ---------------------------------------------------------------------------

typedef struct AeonEngineCallbacks {
    // Navigation progress (0-100)
    void (__cdecl *OnProgress)(unsigned int tab_id, int percent);

    // Page title changed
    void (__cdecl *OnTitleChanged)(unsigned int tab_id, const char* title);

    // Navigation committed (URL is now final)
    void (__cdecl *OnNavigated)(unsigned int tab_id, const char* url);

    // Page fully loaded
    void (__cdecl *OnLoaded)(unsigned int tab_id);

    // Engine crashed (renderer process died)
    void (__cdecl *OnCrash)(unsigned int tab_id, const char* reason);

    // New window/tab requested by page (window.open, target=_blank)
    void (__cdecl *OnNewTab)(unsigned int parent_tab_id, const char* url);
} AeonEngineCallbacks;

// Register callbacks. Call once after Init.
typedef void (__cdecl *AeonEngine_SetCallbacks_t)(const AeonEngineCallbacks* cb);

// ---------------------------------------------------------------------------
// Rendering Area [REQUIRED]
// ---------------------------------------------------------------------------

// Set the HWND + RECT where the engine should paint its content.
// Call when the browser window is created or resized.
typedef void (__cdecl *AeonEngine_SetViewport_t)(
    unsigned int tab_id,
    void*        hwnd,
    int x, int y, int w, int h
);

// ---------------------------------------------------------------------------
// Convenience struct — core caches all function pointers here
// ---------------------------------------------------------------------------
typedef struct AeonEngineVTable {
    AeonEngine_Init_t          Init;
    AeonEngine_Shutdown_t      Shutdown;
    AeonEngine_AbiVersion_t    AbiVersion;
    AeonEngine_Navigate_t      Navigate;
    AeonEngine_Stop_t          Stop;
    AeonEngine_Reload_t        Reload;
    AeonEngine_GoBack_t        GoBack;
    AeonEngine_GoForward_t     GoForward;
    AeonEngine_NewTab_t        NewTab;
    AeonEngine_CloseTab_t      CloseTab;
    AeonEngine_FocusTab_t      FocusTab;
    AeonEngine_InjectCSS_t     InjectCSS;
    AeonEngine_InjectEarlyJS_t InjectEarlyJS;
    AeonEngine_GetTitle_t      GetTitle;
    AeonEngine_GetUrl_t        GetUrl;
    AeonEngine_SetCallbacks_t  SetCallbacks;
    AeonEngine_SetViewport_t   SetViewport;
} AeonEngineVTable;

#ifdef __cplusplus
}
#endif

#endif /* AEON_ENGINE_INTERFACE_H */
