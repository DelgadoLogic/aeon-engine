// AeonBrowser — aeon_blink_stub.cpp
// DelgadoLogic | Lead Systems Architect
//
// PURPOSE: Skeleton implementation of the aeon_blink.dll rendering engine.
// This stub implements the full AeonEngine_Interface.h ABI so the C++ core
// can load and call it without crashing. All functions return safe defaults.
//
// STATUS: SCAFFOLD — replace the TODO sections with real Blink/CEF integration.
//
// HOW TO BUILD THIS DLL:
//   cl /D_WINDOWS /DAEON_ENGINE_EXPORTS /LD /O2 aeon_blink_stub.cpp
//   /link /OUT:aeon_blink.dll kernel32.lib user32.lib gdi32.lib
//
// IT TROUBLESHOOTING:
//   - "AeonEngine_Init entry point not found": DLL not export-decorated.
//     Check that all functions have __declspec(dllexport) via AEON_API macro.
//   - "ABI version mismatch": This DLL exports AEON_ENGINE_ABI_VERSION = 1.
//     Core expects the same. Bump both when the interface changes.
//   - Viewport not painting: SetViewport() must be called after Init.
//     Core calls it in EraChrome::CreateBrowserWindow().

#ifdef AEON_ENGINE_EXPORTS
#define AEON_API extern "C" __declspec(dllexport) __cdecl
#else
#define AEON_API extern "C" __declspec(dllimport) __cdecl
#endif

#include "AeonEngine_Interface.h"
#include <windows.h>
#include <cstdio>
#include <cstring>
#include <map>
#include <string>

// ---------------------------------------------------------------------------
// Per-tab state
// ---------------------------------------------------------------------------
struct TabState {
    unsigned int id;
    std::string  url;
    std::string  title;
    HWND         hwnd    = nullptr;
    int x = 0, y = 0, w = 800, h = 600;
    bool loading = false;
};

static std::map<unsigned int, TabState> g_Tabs;
static unsigned int                     g_NextTabId = 1;
static AeonEngineCallbacks              g_Callbacks = {};
static const SystemProfile*             g_Profile   = nullptr;

// ---------------------------------------------------------------------------
// Rendering: paint into the tab's viewport using GDI (placeholder)
// ---------------------------------------------------------------------------
static void PaintViewport(const TabState& tab) {
    if (!tab.hwnd) return;
    // In the real Blink build: Blink composites into a shared texture.
    // Here: paint the Aeon branded placeholder page.
    HDC hdc = GetDC(tab.hwnd);
    RECT rc = { tab.x, tab.y, tab.x + tab.w, tab.y + tab.h };

    // Dark grey background (loading placeholder)
    HBRUSH bg = CreateSolidBrush(RGB(24, 24, 27));
    FillRect(hdc, &rc, bg);
    DeleteObject(bg);

    // Center text
    SetTextColor(hdc, RGB(220, 220, 230));
    SetBkMode(hdc, TRANSPARENT);
    HFONT f = CreateFontA(28, 0, 0, 0, FW_BOLD, 0, 0, 0, ANSI_CHARSET,
        OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
        DEFAULT_PITCH | FF_SWISS, "Segoe UI");
    HFONT old = (HFONT)SelectObject(hdc, f);
    DrawTextA(hdc, "Aeon Browser", -1, &rc, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    SelectObject(hdc, old);
    DeleteObject(f);

    // URL below
    if (!tab.url.empty() && tab.url != "about:blank") {
        HFONT fs = CreateFontA(14, 0, 0, 0, FW_NORMAL, 0, 0, 0, ANSI_CHARSET,
            OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, DEFAULT_QUALITY,
            DEFAULT_PITCH | FF_SWISS, "Segoe UI");
        HFONT olds = (HFONT)SelectObject(hdc, fs);
        SetTextColor(hdc, RGB(120, 120, 140));
        RECT rcUrl = { rc.left, rc.top + (rc.bottom-rc.top)/2 + 24,
                       rc.right, rc.bottom };
        DrawTextA(hdc, tab.url.c_str(), -1, &rcUrl,
            DT_CENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
        SelectObject(hdc, olds);
        DeleteObject(fs);
    }
    ReleaseDC(tab.hwnd, hdc);
}

// ---------------------------------------------------------------------------
// AeonEngine ABI implementation
// ---------------------------------------------------------------------------

AEON_API int AeonEngine_AbiVersion() {
    return AEON_ENGINE_ABI_VERSION; // 1
}

AEON_API int AeonEngine_Init(const void* profile, void* /*hInst*/) {
    g_Profile = static_cast<const SystemProfile*>(profile);
    fprintf(stdout, "[BlinkStub] Initialised for tier: %u\n",
        g_Profile ? (unsigned)g_Profile->tier : 0u);
    return 1; // success
}

AEON_API void AeonEngine_Shutdown() {
    g_Tabs.clear();
    fprintf(stdout, "[BlinkStub] Shutdown.\n");
}

AEON_API void AeonEngine_SetCallbacks(const AeonEngineCallbacks* cb) {
    if (cb) g_Callbacks = *cb;
}

// Navigation
AEON_API unsigned int AeonEngine_Navigate(unsigned int tab_id,
                                          const char* url,
                                          const char* /*referrer*/) {
    auto it = g_Tabs.find(tab_id);
    if (it == g_Tabs.end()) return 0;

    TabState& tab = it->second;
    tab.url     = url ? url : "about:blank";
    tab.title   = url ? url : "New Tab";
    tab.loading = true;

    if (g_Callbacks.OnProgress)  g_Callbacks.OnProgress(tab_id, 10);
    if (g_Callbacks.OnNavigated) g_Callbacks.OnNavigated(tab_id, tab.url.c_str());

    // TODO: Start real page load via Blink CEF
    PaintViewport(tab);

    if (g_Callbacks.OnProgress) g_Callbacks.OnProgress(tab_id, 100);
    if (g_Callbacks.OnLoaded)   g_Callbacks.OnLoaded(tab_id);
    tab.loading = false;

    static unsigned int reqId = 1;
    return reqId++;
}

AEON_API void AeonEngine_Stop(unsigned int tab_id) {
    auto it = g_Tabs.find(tab_id);
    if (it != g_Tabs.end()) it->second.loading = false;
}

AEON_API void AeonEngine_Reload(unsigned int tab_id, int /*bypass_cache*/) {
    auto it = g_Tabs.find(tab_id);
    if (it != g_Tabs.end())
        AeonEngine_Navigate(tab_id, it->second.url.c_str(), nullptr);
}

AEON_API void AeonEngine_GoBack   (unsigned int /*tab_id*/) { /* TODO: history */ }
AEON_API void AeonEngine_GoForward(unsigned int /*tab_id*/) { /* TODO: history */ }

// Tab management
AEON_API unsigned int AeonEngine_NewTab(void* parent_hwnd, const char* initial_url) {
    unsigned int id = g_NextTabId++;
    TabState t;
    t.id   = id;
    t.hwnd = static_cast<HWND>(parent_hwnd);
    t.url  = initial_url ? initial_url : "about:blank";
    t.title= "New Tab";
    g_Tabs[id] = t;
    fprintf(stdout, "[BlinkStub] New tab #%u: %s\n", id, t.url.c_str());
    if (initial_url) AeonEngine_Navigate(id, initial_url, nullptr);
    return id;
}

AEON_API void AeonEngine_CloseTab(unsigned int tab_id) {
    g_Tabs.erase(tab_id);
    fprintf(stdout, "[BlinkStub] Closed tab #%u\n", tab_id);
}

AEON_API void AeonEngine_FocusTab(unsigned int tab_id) {
    auto it = g_Tabs.find(tab_id);
    if (it != g_Tabs.end()) PaintViewport(it->second);
}

// Content
AEON_API void AeonEngine_InjectCSS(unsigned int /*tab_id*/, const char* /*css*/) {
    // TODO: Pass CSS to Blink via DevTools Protocol or V8 API
    fprintf(stdout, "[BlinkStub] InjectCSS called.\n");
}

AEON_API void AeonEngine_InjectEarlyJS(unsigned int /*tab_id*/, const char* /*js*/) {
    // TODO: Register JS with Blink's OnBeforeScriptExecution hook
    fprintf(stdout, "[BlinkStub] InjectEarlyJS called.\n");
}

AEON_API void AeonEngine_GetTitle(unsigned int tab_id,
                                   char* buf, unsigned int len) {
    auto it = g_Tabs.find(tab_id);
    if (it != g_Tabs.end())
        strncpy_s(buf, len, it->second.title.c_str(), _TRUNCATE);
}

AEON_API void AeonEngine_GetUrl(unsigned int tab_id,
                                 char* buf, unsigned int len) {
    auto it = g_Tabs.find(tab_id);
    if (it != g_Tabs.end())
        strncpy_s(buf, len, it->second.url.c_str(), _TRUNCATE);
}

AEON_API void AeonEngine_SetViewport(unsigned int tab_id,
                                      void* hwnd, int x, int y, int w, int h) {
    auto it = g_Tabs.find(tab_id);
    if (it == g_Tabs.end()) return;
    TabState& t = it->second;
    t.hwnd = static_cast<HWND>(hwnd);
    t.x = x; t.y = y; t.w = w; t.h = h;
    PaintViewport(t);
}

// DLL entry point
BOOL WINAPI DllMain(HINSTANCE, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH)
        fprintf(stdout, "[BlinkStub] aeon_blink.dll loaded.\n");
    return TRUE;
}
