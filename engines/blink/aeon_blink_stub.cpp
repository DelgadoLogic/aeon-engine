// AeonBrowser — aeon_blink_stub.cpp (WebView2 Edition)
// DelgadoLogic | Lead Systems Architect
//
// PURPOSE: WebView2-backed rendering engine that implements the full
// AeonEngine_Interface.h ABI. Uses Microsoft Edge WebView2 for actual
// web page rendering while maintaining the sovereign Aeon shell.
//
// HOW TO BUILD THIS DLL:
//   1. Install WebView2 SDK NuGet: Microsoft.Web.WebView2
//   2. cl /D_WINDOWS /DAEON_ENGINE_EXPORTS /EHsc /std:c++17 /O2 /LD
//      aeon_blink_stub.cpp /I"path/to/webview2/include"
//      /link /OUT:aeon_blink.dll kernel32.lib user32.lib gdi32.lib ole32.lib
//      "path/to/WebView2Loader.dll.lib"
//
// ARCHITECTURE:
//   ┌───────────────────────────┐
//   │ Aeon Shell (AeonMain.cpp) │
//   │   calls AeonEngineVTable  │
//   └───────────┬───────────────┘
//               │ LoadLibrary + GetProcAddress("AeonEngine_Create")
//               ▼
//   ┌───────────────────────────┐
//   │ aeon_blink.dll (this file)│
//   │   WebView2 host per tab   │
//   │   Maps VTable → WebView2  │
//   └───────────────────────────┘
//               │ ICoreWebView2
//               ▼
//   ┌───────────────────────────┐
//   │ Microsoft Edge WebView2   │
//   │   Blink + V8 rendering    │
//   └───────────────────────────┘

#ifdef AEON_ENGINE_EXPORTS
#define AEON_EXPORT extern "C" __declspec(dllexport)
#else
#define AEON_EXPORT extern "C" __declspec(dllimport)
#endif

#include "AeonEngine_Interface.h"
#include <windows.h>
#include <shlobj.h>
#include <cstdio>
#include <cstring>
#include <map>
#include <string>
#include <functional>
#include <shlwapi.h>

#pragma comment(lib, "shlwapi.lib")

// ---------------------------------------------------------------------------
// WebView2 headers — conditionally included
// If WebView2 SDK is not available, fall back to GDI placeholder rendering.
// ---------------------------------------------------------------------------
#ifdef AEON_HAS_WEBVIEW2
#include <wrl.h>
#include <wil/com.h>
#include "WebView2.h"
#include "WebView2EnvironmentOptions.h"
using namespace Microsoft::WRL;
#endif

// ---------------------------------------------------------------------------
// Per-tab state
// ---------------------------------------------------------------------------
struct TabState {
    unsigned int id;
    std::string  url;
    std::string  title;
    HWND         hostHwnd   = nullptr;   // Parent window from shell
    HWND         childHwnd  = nullptr;   // Our child window for WebView2
    int x = 0, y = 0, w = 800, h = 600;
    bool loading = false;

#ifdef AEON_HAS_WEBVIEW2
    ComPtr<ICoreWebView2Controller> controller;
    ComPtr<ICoreWebView2>           webview;
#endif
};

static std::map<unsigned int, TabState> g_Tabs;
static unsigned int                     g_NextTabId = 1;
static AeonEngineCallbacks              g_Callbacks = {};
static const void*                      g_Profile   = nullptr;
static HINSTANCE                        g_hInst     = nullptr;

// Child window class for hosting WebView2
#define AEON_WV2_CLASS L"AeonWebView2Host"
static bool g_ClassRegistered = false;

static LRESULT CALLBACK WV2HostProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
        case WM_SIZE: {
#ifdef AEON_HAS_WEBVIEW2
            unsigned int tabId = (unsigned int)GetWindowLongPtr(hWnd, GWLP_USERDATA);
            auto it = g_Tabs.find(tabId);
            if (it != g_Tabs.end() && it->second.controller) {
                RECT bounds;
                GetClientRect(hWnd, &bounds);
                it->second.controller->put_Bounds(bounds);
            }
#endif
            return 0;
        }
        case WM_ERASEBKGND:
            return 1;
    }
    return DefWindowProcW(hWnd, msg, wParam, lParam);
}

static void EnsureWindowClass(HINSTANCE hInst) {
    if (g_ClassRegistered) return;
    WNDCLASSEXW wc = {};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = WV2HostProc;
    wc.hInstance = hInst;
    wc.hbrBackground = (HBRUSH)GetStockObject(BLACK_BRUSH);
    wc.lpszClassName = AEON_WV2_CLASS;
    RegisterClassExW(&wc);
    g_ClassRegistered = true;
}

// ---------------------------------------------------------------------------
// GDI fallback rendering (used when WebView2 is not available/initialized)
// ---------------------------------------------------------------------------
static void PaintFallback(const TabState& tab) {
    if (!tab.hostHwnd) return;
    HDC hdc = GetDC(tab.hostHwnd);
    RECT rc = { tab.x, tab.y, tab.x + tab.w, tab.y + tab.h };

    // Dark background
    HBRUSH bg = CreateSolidBrush(RGB(24, 24, 27));
    FillRect(hdc, &rc, bg);
    DeleteObject(bg);

    SetTextColor(hdc, RGB(220, 220, 230));
    SetBkMode(hdc, TRANSPARENT);

    HFONT f = CreateFontA(28, 0, 0, 0, FW_BOLD, 0, 0, 0, ANSI_CHARSET,
        OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
        DEFAULT_PITCH | FF_SWISS, "Segoe UI");
    HFONT old = (HFONT)SelectObject(hdc, f);
    DrawTextA(hdc, "Aeon Browser", -1, &rc, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    SelectObject(hdc, old);
    DeleteObject(f);

    if (!tab.url.empty() && tab.url != "about:blank") {
        HFONT fs = CreateFontA(14, 0, 0, 0, FW_NORMAL, 0, 0, 0, ANSI_CHARSET,
            OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, DEFAULT_QUALITY,
            DEFAULT_PITCH | FF_SWISS, "Segoe UI");
        HFONT olds = (HFONT)SelectObject(hdc, fs);
        SetTextColor(hdc, RGB(120, 120, 140));
        RECT rcUrl = { rc.left, rc.top + (rc.bottom-rc.top)/2 + 24, rc.right, rc.bottom };
        DrawTextA(hdc, tab.url.c_str(), -1, &rcUrl, DT_CENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
        SelectObject(hdc, olds);
        DeleteObject(fs);
    }
    ReleaseDC(tab.hostHwnd, hdc);
}

// ---------------------------------------------------------------------------
// WebView2 initialization per tab
// ---------------------------------------------------------------------------
#ifdef AEON_HAS_WEBVIEW2
static void InitWebView2ForTab(TabState& tab) {
    if (!tab.childHwnd) return;

    // Get user data folder for WebView2 profile
    char appData[MAX_PATH] = {};
    SHGetFolderPathA(nullptr, CSIDL_LOCAL_APPDATA, nullptr, 0, appData);
    char profileDir[MAX_PATH];
    _snprintf_s(profileDir, sizeof(profileDir), _TRUNCATE,
        "%s\\DelgadoLogic\\Aeon\\WebView2Profile", appData);

    wchar_t wProfileDir[MAX_PATH];
    MultiByteToWideChar(CP_UTF8, 0, profileDir, -1, wProfileDir, MAX_PATH);

    // Create environment with additional browser args to disable Google services
    // Note: CoreWebView2EnvironmentOptions is provided by WebView2EnvironmentOptions.h
    auto envOptions = Microsoft::WRL::Make<CoreWebView2EnvironmentOptions>();
    envOptions->put_AdditionalBrowserArguments(
        L"--disable-features=SafeBrowsing,PrintPreview,TranslateUI"
        L" --disable-sync"
        L" --no-first-run"
        L" --disable-default-apps"
        L" --disable-component-update"
    );

    HRESULT hr = CreateCoreWebView2EnvironmentWithOptions(
        nullptr,  // browserExecutableFolder — uses installed Edge
        wProfileDir,
        envOptions.Get(),
        Callback<ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler>(
            [&tab](HRESULT result, ICoreWebView2Environment* env) -> HRESULT {
                if (FAILED(result) || !env) {
                    fprintf(stderr, "[Blink] WebView2 env creation failed: 0x%08x\n", result);
                    PaintFallback(tab);
                    return S_OK;
                }

                env->CreateCoreWebView2Controller(
                    tab.childHwnd,
                    Callback<ICoreWebView2CreateCoreWebView2ControllerCompletedHandler>(
                        [&tab](HRESULT result, ICoreWebView2Controller* controller) -> HRESULT {
                            if (FAILED(result) || !controller) {
                                fprintf(stderr, "[Blink] WebView2 controller failed: 0x%08x\n", result);
                                PaintFallback(tab);
                                return S_OK;
                            }

                            tab.controller = controller;
                            controller->get_CoreWebView2(&tab.webview);

                            // Set bounds to fill the child window
                            RECT bounds;
                            GetClientRect(tab.childHwnd, &bounds);
                            controller->put_Bounds(bounds);
                            controller->put_IsVisible(TRUE);

                            // Wire up navigation events
                            tab.webview->add_NavigationCompleted(
                                Callback<ICoreWebView2NavigationCompletedEventHandler>(
                                    [tabId = tab.id](ICoreWebView2* sender,
                                        ICoreWebView2NavigationCompletedEventArgs* args) -> HRESULT {
                                        auto it = g_Tabs.find(tabId);
                                        if (it == g_Tabs.end()) return S_OK;

                                        it->second.loading = false;
                                        if (g_Callbacks.OnProgress)
                                            g_Callbacks.OnProgress(tabId, 100);
                                        if (g_Callbacks.OnLoaded)
                                            g_Callbacks.OnLoaded(tabId);
                                        return S_OK;
                                    }).Get(), nullptr);

                            tab.webview->add_DocumentTitleChanged(
                                Callback<ICoreWebView2DocumentTitleChangedEventHandler>(
                                    [tabId = tab.id](ICoreWebView2* sender, IUnknown* args) -> HRESULT {
                                        auto it = g_Tabs.find(tabId);
                                        if (it == g_Tabs.end()) return S_OK;

                                        LPWSTR wTitle = nullptr;
                                        sender->get_DocumentTitle(&wTitle);
                                        if (wTitle) {
                                            char title[512];
                                            WideCharToMultiByte(CP_UTF8, 0, wTitle, -1,
                                                title, sizeof(title), nullptr, nullptr);
                                            it->second.title = title;
                                            if (g_Callbacks.OnTitleChanged)
                                                g_Callbacks.OnTitleChanged(tabId, title);
                                            CoTaskMemFree(wTitle);
                                        }
                                        return S_OK;
                                    }).Get(), nullptr);

                            tab.webview->add_SourceChanged(
                                Callback<ICoreWebView2SourceChangedEventHandler>(
                                    [tabId = tab.id](ICoreWebView2* sender,
                                        ICoreWebView2SourceChangedEventArgs* args) -> HRESULT {
                                        auto it = g_Tabs.find(tabId);
                                        if (it == g_Tabs.end()) return S_OK;

                                        LPWSTR wUrl = nullptr;
                                        sender->get_Source(&wUrl);
                                        if (wUrl) {
                                            char url[2048];
                                            WideCharToMultiByte(CP_UTF8, 0, wUrl, -1,
                                                url, sizeof(url), nullptr, nullptr);
                                            it->second.url = url;
                                            if (g_Callbacks.OnNavigated)
                                                g_Callbacks.OnNavigated(tabId, url);
                                            CoTaskMemFree(wUrl);
                                        }
                                        return S_OK;
                                    }).Get(), nullptr);

                            // Navigate to initial URL
                            if (!tab.url.empty() && tab.url != "about:blank") {
                                wchar_t wUrl[2048];
                                MultiByteToWideChar(CP_UTF8, 0, tab.url.c_str(), -1,
                                    wUrl, 2048);
                                tab.webview->Navigate(wUrl);
                            }

                            fprintf(stdout, "[Blink] WebView2 ready for tab #%u\n", tab.id);
                            return S_OK;
                        }).Get());
                return S_OK;
            }).Get());
}
#endif

// ---------------------------------------------------------------------------
// AeonEngine ABI implementation
// ---------------------------------------------------------------------------

static int __cdecl Engine_AbiVersion() {
    return AEON_ENGINE_ABI_VERSION;
}

static int __cdecl Engine_Init(const void* profile, void* hInst) {
    g_Profile = profile;
    g_hInst = static_cast<HINSTANCE>(hInst);
    EnsureWindowClass(g_hInst);

#ifdef AEON_HAS_WEBVIEW2
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    fprintf(stdout, "[Blink] WebView2 engine initialized.\n");
#else
    fprintf(stdout, "[Blink] GDI fallback engine initialized (WebView2 SDK not linked).\n");
#endif
    return 1;
}

static void __cdecl Engine_Shutdown() {
#ifdef AEON_HAS_WEBVIEW2
    for (auto& [id, tab] : g_Tabs) {
        if (tab.controller) tab.controller->Close();
    }
    CoUninitialize();
#endif
    g_Tabs.clear();
    fprintf(stdout, "[Blink] Shutdown.\n");
}

static void __cdecl Engine_SetCallbacks(const AeonEngineCallbacks* cb) {
    if (cb) g_Callbacks = *cb;
}

static unsigned int __cdecl Engine_Navigate(unsigned int tab_id,
                                            const char* url,
                                            const char* /*referrer*/) {
    auto it = g_Tabs.find(tab_id);
    if (it == g_Tabs.end()) return 0;

    TabState& tab = it->second;
    tab.url     = url ? url : "about:blank";
    tab.loading = true;

    if (g_Callbacks.OnProgress) g_Callbacks.OnProgress(tab_id, 10);

#ifdef AEON_HAS_WEBVIEW2
    if (tab.webview) {
        wchar_t wUrl[2048];
        MultiByteToWideChar(CP_UTF8, 0, tab.url.c_str(), -1, wUrl, 2048);
        tab.webview->Navigate(wUrl);
    } else {
        PaintFallback(tab);
        if (g_Callbacks.OnProgress) g_Callbacks.OnProgress(tab_id, 100);
        if (g_Callbacks.OnLoaded)   g_Callbacks.OnLoaded(tab_id);
        tab.loading = false;
    }
#else
    PaintFallback(tab);
    if (g_Callbacks.OnNavigated) g_Callbacks.OnNavigated(tab_id, tab.url.c_str());
    if (g_Callbacks.OnProgress)  g_Callbacks.OnProgress(tab_id, 100);
    if (g_Callbacks.OnLoaded)    g_Callbacks.OnLoaded(tab_id);
    tab.loading = false;
#endif

    static unsigned int reqId = 1;
    return reqId++;
}

static void __cdecl Engine_Stop(unsigned int tab_id) {
    auto it = g_Tabs.find(tab_id);
    if (it == g_Tabs.end()) return;
    it->second.loading = false;
#ifdef AEON_HAS_WEBVIEW2
    if (it->second.webview) it->second.webview->Stop();
#endif
}

static void __cdecl Engine_Reload(unsigned int tab_id, int /*bypass_cache*/) {
    auto it = g_Tabs.find(tab_id);
    if (it == g_Tabs.end()) return;
#ifdef AEON_HAS_WEBVIEW2
    if (it->second.webview) it->second.webview->Reload();
    else
#endif
    Engine_Navigate(tab_id, it->second.url.c_str(), nullptr);
}

static void __cdecl Engine_GoBack(unsigned int tab_id) {
#ifdef AEON_HAS_WEBVIEW2
    auto it = g_Tabs.find(tab_id);
    if (it != g_Tabs.end() && it->second.webview) it->second.webview->GoBack();
#endif
}

static void __cdecl Engine_GoForward(unsigned int tab_id) {
#ifdef AEON_HAS_WEBVIEW2
    auto it = g_Tabs.find(tab_id);
    if (it != g_Tabs.end() && it->second.webview) it->second.webview->GoForward();
#endif
}

static unsigned int __cdecl Engine_NewTab(void* parent_hwnd, const char* initial_url) {
    unsigned int id = g_NextTabId++;
    TabState t;
    t.id       = id;
    t.hostHwnd = static_cast<HWND>(parent_hwnd);
    t.url      = initial_url ? initial_url : "about:blank";
    t.title    = "New Tab";
    g_Tabs[id] = t;

    fprintf(stdout, "[Blink] New tab #%u: %s\n", id, t.url.c_str());
    return id;
}

static void __cdecl Engine_CloseTab(unsigned int tab_id) {
    auto it = g_Tabs.find(tab_id);
    if (it == g_Tabs.end()) return;
#ifdef AEON_HAS_WEBVIEW2
    if (it->second.controller) it->second.controller->Close();
#endif
    if (it->second.childHwnd) DestroyWindow(it->second.childHwnd);
    g_Tabs.erase(it);
    fprintf(stdout, "[Blink] Closed tab #%u\n", tab_id);
}

static void __cdecl Engine_FocusTab(unsigned int tab_id) {
    // Hide all other tab windows, show this one
    for (auto& [id, tab] : g_Tabs) {
        if (tab.childHwnd) {
            ShowWindow(tab.childHwnd, (id == tab_id) ? SW_SHOW : SW_HIDE);
#ifdef AEON_HAS_WEBVIEW2
            if (tab.controller)
                tab.controller->put_IsVisible(id == tab_id ? TRUE : FALSE);
#endif
        }
    }
    auto it = g_Tabs.find(tab_id);
    if (it != g_Tabs.end() && !it->second.childHwnd) {
        PaintFallback(it->second);
    }
}

static void __cdecl Engine_InjectCSS(unsigned int tab_id, const char* css) {
#ifdef AEON_HAS_WEBVIEW2
    auto it = g_Tabs.find(tab_id);
    if (it == g_Tabs.end() || !it->second.webview || !css) return;

    // Wrap CSS in a <style> injection via JS
    std::string js = "(() => { const s = document.createElement('style'); s.textContent = `";
    js += css;
    js += "`; document.head.appendChild(s); })();";

    wchar_t* wJs = new wchar_t[js.size() + 1];
    MultiByteToWideChar(CP_UTF8, 0, js.c_str(), -1, wJs, (int)js.size() + 1);
    it->second.webview->ExecuteScript(wJs, nullptr);
    delete[] wJs;
#else
    (void)tab_id; (void)css;
#endif
}

static void __cdecl Engine_InjectEarlyJS(unsigned int tab_id, const char* js) {
#ifdef AEON_HAS_WEBVIEW2
    auto it = g_Tabs.find(tab_id);
    if (it == g_Tabs.end() || !it->second.webview || !js) return;

    wchar_t* wJs = new wchar_t[strlen(js) + 1];
    MultiByteToWideChar(CP_UTF8, 0, js, -1, wJs, (int)strlen(js) + 1);
    it->second.webview->AddScriptToExecuteOnDocumentCreated(wJs, nullptr);
    delete[] wJs;
#else
    (void)tab_id; (void)js;
#endif
}

static void __cdecl Engine_GetTitle(unsigned int tab_id, char* buf, unsigned int len) {
    auto it = g_Tabs.find(tab_id);
    if (it != g_Tabs.end())
        strncpy_s(buf, len, it->second.title.c_str(), _TRUNCATE);
}

static void __cdecl Engine_GetUrl(unsigned int tab_id, char* buf, unsigned int len) {
    auto it = g_Tabs.find(tab_id);
    if (it != g_Tabs.end())
        strncpy_s(buf, len, it->second.url.c_str(), _TRUNCATE);
}

static void __cdecl Engine_SetViewport(unsigned int tab_id,
                                        void* hwnd, int x, int y, int w, int h) {
    auto it = g_Tabs.find(tab_id);
    if (it == g_Tabs.end()) return;

    TabState& t = it->second;
    t.hostHwnd = static_cast<HWND>(hwnd);
    t.x = x; t.y = y; t.w = w; t.h = h;

#ifdef AEON_HAS_WEBVIEW2
    // Create child window for WebView2 if it doesn't exist
    if (!t.childHwnd && t.hostHwnd) {
        EnsureWindowClass(g_hInst);
        t.childHwnd = CreateWindowExW(
            0, AEON_WV2_CLASS, L"",
            WS_CHILD | WS_VISIBLE | WS_CLIPCHILDREN,
            x, y, w, h,
            t.hostHwnd, nullptr, g_hInst, nullptr);
        SetWindowLongPtr(t.childHwnd, GWLP_USERDATA, (LONG_PTR)tab_id);

        // Initialize WebView2 in the child window
        InitWebView2ForTab(t);
    } else if (t.childHwnd) {
        MoveWindow(t.childHwnd, x, y, w, h, TRUE);
        if (t.controller) {
            RECT bounds = { 0, 0, w, h };
            t.controller->put_Bounds(bounds);
        }
    }
#else
    PaintFallback(t);
#endif
}

// ---------------------------------------------------------------------------
// Static VTable singleton
// ---------------------------------------------------------------------------
static AeonEngineVTable g_VTable = {
    Engine_Init,
    Engine_Shutdown,
    Engine_AbiVersion,
    Engine_Navigate,
    Engine_Stop,
    Engine_Reload,
    Engine_GoBack,
    Engine_GoForward,
    Engine_NewTab,
    Engine_CloseTab,
    Engine_FocusTab,
    Engine_InjectCSS,
    Engine_InjectEarlyJS,
    Engine_GetTitle,
    Engine_GetUrl,
    Engine_SetCallbacks,
    Engine_SetViewport,
};

// ---------------------------------------------------------------------------
// Factory export — this is what TierDispatcher::LoadEngine() looks for
// ---------------------------------------------------------------------------
AEON_EXPORT AeonEngineVTable* __cdecl AeonEngine_Create() {
    return &g_VTable;
}

// DLL entry point
BOOL WINAPI DllMain(HINSTANCE hInst, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        g_hInst = hInst;
        fprintf(stdout, "[Blink] aeon_blink.dll loaded (WebView2 adapter).\n");
    }
    return TRUE;
}
