// AeonBrowser — aeon_html4_adapter.c
// DelgadoLogic | Retro Engine Team
//
// PURPOSE: VTable adapter that wraps the HTML4/CSS2 GDI renderer into the
// AeonEngine_Interface.h ABI. This allows TierDispatcher to load
// aeon_html4.dll identically to aeon_blink.dll or aeon_gecko.dll.
//
// ARCHITECTURE:
//   TierDispatcher calls GetProcAddress(hMod, "AeonEngine_Create")
//   → returns AeonEngineVTable* pointing to our shim functions
//   → shim functions translate VTable calls into HTML4 API calls
//
// LIMITATIONS (documented, not bugs):
//   - Single-tab model: NewTab() always returns tab_id=0
//   - No JavaScript injection: InjectCSS/InjectEarlyJS are no-ops
//   - No history: GoBack/GoForward are no-ops
//   - Navigation is synchronous: we fetch → parse → render in Navigate()

#ifdef AEON_HTML4_BUILD
#define AEON_HTML4_ADAPTER_API __declspec(dllexport)
#else
#define AEON_HTML4_ADAPTER_API __declspec(dllimport)
#endif

#include "aeon_html4.h"
/* AeonEngine_Interface.h — resolved via compiler -I flags:
 * Cloud Build / standalone: -I. finds local copy
 * Docker full mount:        -I../core/engine finds parent tree copy */
#include "AeonEngine_Interface.h"
#include "bearssl_bridge.h"
#include <windows.h>
#include <string.h>
#include <stdlib.h>

// ─── Adapter State ────────────────────────────────────────────────────────────
static HWND   g_hwnd       = NULL;
static char   g_currentUrl[2048] = "about:blank";
static char   g_currentTitle[256] = "Aeon Browser";
static char   g_htmlBuf[65536];   // 64KB page buffer (Win9x safe)
static int    g_scrollY    = 0;
static int    g_docHeight  = 0;   /* TODO: used when scroll support ships */
static BOOL   g_initialized = FALSE;

static AeonEngineCallbacks g_callbacks = {0};

// ─── Internal: Fetch and render a URL ──────────────────────────────────────────
static int FetchAndRender(const char* url) {
    int len = 0;

    if (g_callbacks.OnProgress)
        g_callbacks.OnProgress(0, 10);

    // Determine protocol and fetch
    if (strncmp(url, "https://", 8) == 0) {
        len = tls_get(url, g_htmlBuf, sizeof(g_htmlBuf));
    } else if (strncmp(url, "http://", 7) == 0) {
        len = http_get(url, g_htmlBuf, sizeof(g_htmlBuf));
    } else if (strncmp(url, "gemini://", 9) == 0) {
        len = gemini_get(url, g_htmlBuf, sizeof(g_htmlBuf));
    } else {
        // about:blank or internal pages
        const char* blank =
            "<html><body>"
            "<h1>Aeon Browser</h1>"
            "<p>by DelgadoLogic</p>"
            "<hr>"
            "<p>This is the HTML4/CSS2 retro engine.</p>"
            "<p>Supports HTTP, HTTPS, and Gemini protocols.</p>"
            "</body></html>";
        strncpy(g_htmlBuf, blank, sizeof(g_htmlBuf) - 1);
        g_htmlBuf[sizeof(g_htmlBuf) - 1] = '\0';
        len = (int)strlen(g_htmlBuf);
    }

    if (len <= 0) return 0;

    if (g_callbacks.OnProgress)
        g_callbacks.OnProgress(0, 50);

    // Store URL
    strncpy(g_currentUrl, url, sizeof(g_currentUrl) - 1);
    g_currentUrl[sizeof(g_currentUrl) - 1] = '\0';

    if (g_callbacks.OnNavigated)
        g_callbacks.OnNavigated(0, g_currentUrl);

    // Force repaint — the WM_PAINT handler calls AeonHTML4_Render
    g_scrollY = 0;
    if (g_hwnd) InvalidateRect(g_hwnd, NULL, TRUE);

    if (g_callbacks.OnProgress)
        g_callbacks.OnProgress(0, 100);

    if (g_callbacks.OnLoaded)
        g_callbacks.OnLoaded(0);

    return 1;
}

// ─── VTable Implementation ────────────────────────────────────────────────────

static int __cdecl Adapter_Init(const void* profile, void* hInst) {
    (void)profile; (void)hInst;
    tls_init();
    AeonHTML4_Init();
    g_initialized = TRUE;
    return 1;
}

static void __cdecl Adapter_Shutdown(void) {
    AeonHTML4_Shutdown();
    tls_cleanup();
    g_initialized = FALSE;
}

static int __cdecl Adapter_AbiVersion(void) {
    return AEON_ENGINE_ABI_VERSION;
}

static unsigned int __cdecl Adapter_Navigate(
        unsigned int tab_id, const char* url, const char* referrer) {
    (void)tab_id; (void)referrer;
    if (!url) return 0;
    return FetchAndRender(url) ? 1 : 0;
}

static void __cdecl Adapter_Stop(unsigned int tab_id) {
    (void)tab_id; // HTML4 engine is synchronous — nothing to cancel
}

static void __cdecl Adapter_Reload(unsigned int tab_id, int bypass_cache) {
    (void)tab_id; (void)bypass_cache;
    FetchAndRender(g_currentUrl);
}

static void __cdecl Adapter_GoBack(unsigned int tab_id) {
    (void)tab_id; // No history in HTML4 retro tier
}

static void __cdecl Adapter_GoForward(unsigned int tab_id) {
    (void)tab_id; // No history in HTML4 retro tier
}

static unsigned int __cdecl Adapter_NewTab(void* parent_hwnd, const char* initial_url) {
    // Single-document model — "tab 0" is the only tab
    g_hwnd = (HWND)parent_hwnd;
    if (initial_url && initial_url[0])
        FetchAndRender(initial_url);
    return 0;
}

static void __cdecl Adapter_CloseTab(unsigned int tab_id) {
    (void)tab_id;
    g_hwnd = NULL;
}

static void __cdecl Adapter_FocusTab(unsigned int tab_id) {
    (void)tab_id; // Only one tab — always focused
}

static void __cdecl Adapter_InjectCSS(unsigned int tab_id, const char* css) {
    (void)tab_id; (void)css;
    // HTML4 engine doesn't support dynamic CSS injection
}

static void __cdecl Adapter_InjectEarlyJS(unsigned int tab_id, const char* js) {
    (void)tab_id; (void)js;
    // HTML4 engine has no JavaScript support
}

static void __cdecl Adapter_GetTitle(unsigned int tab_id, char* buf, unsigned int buf_len) {
    (void)tab_id;
    strncpy(buf, g_currentTitle, buf_len - 1);
    buf[buf_len - 1] = '\0';
}

static void __cdecl Adapter_GetUrl(unsigned int tab_id, char* buf, unsigned int buf_len) {
    (void)tab_id;
    strncpy(buf, g_currentUrl, buf_len - 1);
    buf[buf_len - 1] = '\0';
}

static void __cdecl Adapter_SetCallbacks(const AeonEngineCallbacks* cb) {
    if (cb) g_callbacks = *cb;
}

static void __cdecl Adapter_SetViewport(
        unsigned int tab_id, void* hwnd, int x, int y, int w, int h) {
    (void)tab_id; (void)x; (void)y; (void)w; (void)h;
    g_hwnd = (HWND)hwnd;
}

// ─── Static VTable ────────────────────────────────────────────────────────────
static AeonEngineVTable g_vtable = {
    Adapter_Init,
    Adapter_Shutdown,
    Adapter_AbiVersion,
    Adapter_Navigate,
    Adapter_Stop,
    Adapter_Reload,
    Adapter_GoBack,
    Adapter_GoForward,
    Adapter_NewTab,
    Adapter_CloseTab,
    Adapter_FocusTab,
    Adapter_InjectCSS,
    Adapter_InjectEarlyJS,
    Adapter_GetTitle,
    Adapter_GetUrl,
    Adapter_SetCallbacks,
    Adapter_SetViewport
};

// ─── DLL Export — called by TierDispatcher ─────────────────────────────────────
AEON_HTML4_ADAPTER_API AeonEngineVTable* AeonEngine_Create(void) {
    return &g_vtable;
}
