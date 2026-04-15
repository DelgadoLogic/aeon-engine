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
#include "ExtensionRuntime.h"
// AeonBridge injection is handled by the host process via InjectEarlyJS ABI
#include <windows.h>
#include <shlobj.h>
#include <cstdio>
#include <cstring>
#include <map>
#include <string>
#include <functional>
#include <vector>
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

    // Scripts queued via InjectEarlyJS before WebView2 is ready
    std::vector<std::string> pendingEarlyJS;

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
static char                             g_ExeDir[MAX_PATH] = {};

// Child window class for hosting WebView2
#define AEON_WV2_CLASS L"AeonWebView2Host"
static bool g_ClassRegistered = false;

// ---------------------------------------------------------------------------
// aeon:// URL resolution → local file:// path
// Maps: aeon://newtab     → <exeDir>/newtab/newtab.html
//       aeon://settings   → <exeDir>/pages/settings.html
//       aeon://history    → <exeDir>/pages/history.html
//       etc.
// Returns empty string if not an aeon:// URL.
// ---------------------------------------------------------------------------
static std::string ResolveAeonUrl(const std::string& url) {
    if (url.rfind("aeon://", 0) != 0) return {};
    std::string page = url.substr(7); // strip "aeon://"

    // Strip trailing slashes
    while (!page.empty() && page.back() == '/') page.pop_back();
    if (page.empty()) page = "newtab";

    char path[MAX_PATH];
    if (page == "newtab") {
        _snprintf_s(path, sizeof(path), _TRUNCATE,
            "%s\\newtab\\newtab.html", g_ExeDir);
    } else {
        _snprintf_s(path, sizeof(path), _TRUNCATE,
            "%s\\pages\\%s.html", g_ExeDir, page.c_str());
    }

    // Verify file exists
    if (GetFileAttributesA(path) == INVALID_FILE_ATTRIBUTES) {
        fprintf(stderr, "[Blink] aeon://%s → file not found: %s\n",
            page.c_str(), path);
        return {};
    }

    // Convert backslashes to forward slashes for file:// URI
    std::string fileUrl = "file:///";
    for (const char* p = path; *p; p++) {
        if (*p == '\\') fileUrl += '/';
        else if (*p == ' ') fileUrl += "%20";
        else fileUrl += *p;
    }

    fprintf(stdout, "[Blink] aeon://%s → %s\n", page.c_str(), fileUrl.c_str());
    return fileUrl;
}

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

    // Create environment with additional browser args:
    //   - Disable unnecessary Google services for sovereign operation
    //   - Enable CDP remote debugging on port 9222 for AI agent control
    // Note: CoreWebView2EnvironmentOptions is provided by WebView2EnvironmentOptions.h
    auto envOptions = Microsoft::WRL::Make<CoreWebView2EnvironmentOptions>();
    envOptions->put_AdditionalBrowserArguments(
        L"--disable-features=SafeBrowsing,PrintPreview,TranslateUI"
        L" --disable-sync"
        L" --no-first-run"
        L" --disable-default-apps"
        L" --disable-component-update"
        L" --remote-debugging-port=9222"
        L" --remote-allow-origins=*"
    );

    HRESULT hr = CreateCoreWebView2EnvironmentWithOptions(
        nullptr,  // browserExecutableFolder — uses installed Edge
        wProfileDir,
        envOptions.Get(),
        Callback<ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler>(
            [tabId = tab.id](HRESULT result, ICoreWebView2Environment* env) -> HRESULT {
                auto it = g_Tabs.find(tabId);
                if (it == g_Tabs.end()) return S_OK; // Tab closed during init
                TabState& tab = it->second;

                if (FAILED(result) || !env) {
                    fprintf(stderr, "[Blink] WebView2 env creation failed: 0x%08x\n", result);
                    PaintFallback(tab);
                    return S_OK;
                }

                env->CreateCoreWebView2Controller(
                    tab.childHwnd,
                    Callback<ICoreWebView2CreateCoreWebView2ControllerCompletedHandler>(
                        [tabId, envPtr = ComPtr<ICoreWebView2Environment>(env)](HRESULT result, ICoreWebView2Controller* controller) -> HRESULT {
                            auto it2 = g_Tabs.find(tabId);
                            if (it2 == g_Tabs.end()) return S_OK; // Tab closed
                            TabState& tab = it2->second;

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

                            // ── NativeAdBlock: WebResourceRequested ────────
                            // Register a wildcard filter to intercept ALL
                            // sub-resource requests (scripts, images, iframes,
                            // XHR, etc.) and block ad/tracker domains.
                            tab.webview->AddWebResourceRequestedFilter(
                                L"*", COREWEBVIEW2_WEB_RESOURCE_CONTEXT_ALL);

                            tab.webview->add_WebResourceRequested(
                                Callback<ICoreWebView2WebResourceRequestedEventHandler>(
                                    [tabId = tab.id, envPtr](ICoreWebView2* sender,
                                        ICoreWebView2WebResourceRequestedEventArgs* args) -> HRESULT {

                                        ComPtr<ICoreWebView2WebResourceRequest> request;
                                        args->get_Request(&request);
                                        if (!request) return S_OK;

                                        LPWSTR wUri = nullptr;
                                        request->get_Uri(&wUri);
                                        if (!wUri) return S_OK;

                                        // Convert to UTF-8 for NativeAdBlock
                                        char uri[2048];
                                        WideCharToMultiByte(CP_UTF8, 0, wUri, -1,
                                            uri, sizeof(uri), nullptr, nullptr);
                                        CoTaskMemFree(wUri);

                                        // Check against loaded filter lists
                                        if (NativeAdBlock::ShouldBlock(uri)) {
                                            // Block: create proper 403 response
                                            if (envPtr) {
                                                ComPtr<ICoreWebView2WebResourceResponse> response;
                                                envPtr->CreateWebResourceResponse(
                                                    nullptr, 403,
                                                    L"Blocked by Aeon Shield",
                                                    L"", &response);
                                                if (response)
                                                    args->put_Response(response.Get());
                                            }
                                            fprintf(stdout, "[AdBlock] Blocked: %.80s\n", uri);
                                        }
                                        return S_OK;
                                    }).Get(), nullptr);

                            // NOTE: AeonBridge injection is handled by the
                            // host process (BrowserChrome) via the InjectEarlyJS
                            // ABI call — the DLL does not link AeonBridge.cpp.

                            // Flush any early-JS scripts queued before
                            // WebView2 was ready (e.g. AeonBridge bootstrap)
                            for (const auto& script : tab.pendingEarlyJS) {
                                int wLen = MultiByteToWideChar(CP_UTF8, 0,
                                    script.c_str(), -1, nullptr, 0);
                                wchar_t* wJs = new wchar_t[wLen];
                                MultiByteToWideChar(CP_UTF8, 0,
                                    script.c_str(), -1, wJs, wLen);
                                tab.webview->AddScriptToExecuteOnDocumentCreated(
                                    wJs, nullptr);
                                delete[] wJs;
                            }
                            if (!tab.pendingEarlyJS.empty()) {
                                fprintf(stdout,
                                    "[Blink] Flushed %zu queued early-JS scripts for tab #%u\n",
                                    tab.pendingEarlyJS.size(), tab.id);
                                tab.pendingEarlyJS.clear();
                            }

                            // ── DRM Monitor: Detect Widevine EME failures ──
                            // Intercepts requestMediaKeySystemAccess() to
                            // catch Widevine-only DRM negotiation and show
                            // a user-friendly fallback before the site's
                            // own error message appears.
                            {
                                const wchar_t* drmMonitorJs = LR"JS(
(function() {
    'use strict';
    if (window.__aeonDrmMonitor) return;
    window.__aeonDrmMonitor = true;

    const origRMKSA = navigator.requestMediaKeySystemAccess;
    if (!origRMKSA) return;

    const WIDEVINE_SYSTEMS = [
        'com.widevine.alpha',
        'com.widevine.alpha.experiment'
    ];

    navigator.requestMediaKeySystemAccess = function(keySystem, configs) {
        const isWidevine = WIDEVINE_SYSTEMS.includes(keySystem);

        if (isWidevine) {
            console.warn('[AeonDRM] Widevine key system requested:', keySystem);

            // Check if PlayReady is available as fallback
            const pr = origRMKSA.call(navigator,
                'com.microsoft.playready.recommendation',
                configs
            ).catch(() => null);

            return origRMKSA.call(navigator, keySystem, configs).catch(function(err) {
                console.warn('[AeonDRM] Widevine CDM not available:', err.message);

                // Show non-intrusive banner
                if (!document.getElementById('aeon-drm-banner')) {
                    const banner = document.createElement('div');
                    banner.id = 'aeon-drm-banner';
                    banner.innerHTML = `
                        <style>
                            #aeon-drm-banner {
                                position: fixed; top: 0; left: 0; right: 0;
                                background: linear-gradient(135deg, #1e2140 0%, #16182a 100%);
                                border-bottom: 1px solid #2e3064;
                                padding: 12px 20px;
                                display: flex; align-items: center; gap: 12px;
                                font-family: 'Inter', system-ui, sans-serif;
                                font-size: 13px; color: #e8e8f0;
                                z-index: 2147483647;
                                animation: aeonSlideIn .3s ease-out;
                                box-shadow: 0 4px 24px rgba(0,0,0,0.3);
                            }
                            @keyframes aeonSlideIn {
                                from { transform: translateY(-100%); }
                                to { transform: translateY(0); }
                            }
                            #aeon-drm-banner .aeon-drm-icon {
                                font-size: 20px; flex-shrink: 0;
                            }
                            #aeon-drm-banner .aeon-drm-msg { flex: 1; }
                            #aeon-drm-banner .aeon-drm-msg strong {
                                color: #a78bfa; font-weight: 600;
                            }
                            #aeon-drm-banner button {
                                padding: 6px 14px; border-radius: 6px;
                                border: none; font-size: 12px; font-weight: 600;
                                cursor: pointer; font-family: inherit;
                                transition: all .15s;
                            }
                            #aeon-drm-banner .aeon-btn-edge {
                                background: linear-gradient(135deg, #6c63ff, #a78bfa);
                                color: #fff;
                            }
                            #aeon-drm-banner .aeon-btn-edge:hover { opacity: 0.85; }
                            #aeon-drm-banner .aeon-btn-info {
                                background: #1e2140; color: #8888aa;
                                border: 1px solid #2e3064;
                            }
                            #aeon-drm-banner .aeon-btn-info:hover {
                                border-color: #6c63ff; color: #e8e8f0;
                            }
                            #aeon-drm-banner .aeon-btn-close {
                                background: none; color: #8888aa;
                                font-size: 18px; padding: 4px 8px;
                            }
                            #aeon-drm-banner .aeon-btn-close:hover { color: #e8e8f0; }
                        </style>
                        <span class="aeon-drm-icon">🔒</span>
                        <span class="aeon-drm-msg">
                            This site requires <strong>Widevine DRM</strong> for
                            protected content playback.
                        </span>
                        <button class="aeon-btn-edge" onclick="
                            try { window.location.href='microsoft-edge:'+window.location.href; }
                            catch(e) { window.open(window.location.href); }
                        ">Open in Edge</button>
                        <button class="aeon-btn-info" onclick="
                            window.location.href='aeon://drm-info?url='+
                            encodeURIComponent(window.location.href)+
                            '&drm='+encodeURIComponent('${keySystem}');
                        ">Learn More</button>
                        <button class="aeon-btn-close" onclick="
                            this.parentElement.remove();
                        ">×</button>
                    `;
                    document.body.appendChild(banner);
                }

                throw err; // Re-throw so the site handles its own fallback too
            });
        }

        // Non-Widevine (PlayReady, ClearKey, etc.) — pass through
        return origRMKSA.call(navigator, keySystem, configs);
    };

    console.log('[AeonDRM] DRM monitor active — Widevine detection enabled');
})();
)JS";
                                tab.webview->AddScriptToExecuteOnDocumentCreated(
                                    drmMonitorJs, nullptr);
                                fprintf(stdout,
                                    "[Blink] DRM monitor injected for tab #%u\n",
                                    tab.id);
                            }

                            // Navigate to initial URL (resolve aeon:// → file://)
                            if (!tab.url.empty() && tab.url != "about:blank") {
                                std::string resolved = ResolveAeonUrl(tab.url);
                                const std::string& navUrl = resolved.empty() ? tab.url : resolved;
                                wchar_t wUrl[2048];
                                MultiByteToWideChar(CP_UTF8, 0, navUrl.c_str(), -1,
                                    wUrl, 2048);
                                tab.webview->Navigate(wUrl);
                            }

                            fprintf(stdout, "[Blink] WebView2 ready for tab #%u (AdBlock: %d rules)\n",
                                tab.id, NativeAdBlock::GetTotalRules());
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

    // Cache exe directory for aeon:// URL resolution
    GetModuleFileNameA(nullptr, g_ExeDir, MAX_PATH);
    if (char* s = strrchr(g_ExeDir, '\\')) *s = '\0';
    fprintf(stdout, "[Blink] Exe dir: %s\n", g_ExeDir);

#ifdef AEON_HAS_WEBVIEW2
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    fprintf(stdout, "[Blink] WebView2 engine initialized (WebView2 + aeon:// resolver).\n");
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
        // Resolve aeon:// scheme to local file path
        std::string resolved = ResolveAeonUrl(tab.url);
        const std::string& navUrl = resolved.empty() ? tab.url : resolved;
        wchar_t wUrl[2048];
        MultiByteToWideChar(CP_UTF8, 0, navUrl.c_str(), -1, wUrl, 2048);
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
    if (it == g_Tabs.end() || !js) return;

    if (it->second.webview) {
        // WebView2 is ready — inject immediately
        int wLen = MultiByteToWideChar(CP_UTF8, 0, js, -1, nullptr, 0);
        wchar_t* wJs = new wchar_t[wLen];
        MultiByteToWideChar(CP_UTF8, 0, js, -1, wJs, wLen);
        it->second.webview->AddScriptToExecuteOnDocumentCreated(wJs, nullptr);
        delete[] wJs;
    } else {
        // WebView2 still initializing — queue for flush
        it->second.pendingEarlyJS.emplace_back(js);
        fprintf(stdout, "[Blink] Queued early-JS for tab #%u (pending: %zu)\n",
            tab_id, it->second.pendingEarlyJS.size());
    }
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
// Standalone ABI version export — TierDispatcher calls this via
// GetProcAddress("AeonEngine_AbiVersion") BEFORE AeonEngine_Create.
// Without this export, the DLL is silently rejected by the ABI check.
// ---------------------------------------------------------------------------
AEON_EXPORT int __cdecl AeonEngine_AbiVersion() {
    return AEON_ENGINE_ABI_VERSION;
}

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
