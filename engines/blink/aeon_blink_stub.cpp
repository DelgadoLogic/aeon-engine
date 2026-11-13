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
#include "../aeon_ai.h"
#include "../../core/session/nlohmann/json.hpp"
using json = nlohmann::json;
// AeonBridge injection is handled by the host process via InjectEarlyJS ABI
#include <windows.h>
#include <shlobj.h>
#include <cstdio>
#include <cstring>
#include <map>
#include <string>
#include <functional>
#include <vector>
#include <filesystem>
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
    std::string fullPage = url.substr(7); // strip "aeon://"

    // Separate query string/hash from page name
    std::string page = fullPage;
    std::string queryStr;
    size_t qPos = fullPage.find_first_of("?#");
    if (qPos != std::string::npos) {
        page = fullPage.substr(0, qPos);
        queryStr = fullPage.substr(qPos);
    }

    // Strip trailing slashes
    while (!page.empty() && page.back() == '/') page.pop_back();
    if (page.empty()) page = "newtab";

    char path[MAX_PATH];
    if (page == "newtab") {
        _snprintf_s(path, sizeof(path), _TRUNCATE,
            "%s\\newtab\\newtab.html", g_ExeDir);
        if (GetFileAttributesA(path) == INVALID_FILE_ATTRIBUTES) {
            _snprintf_s(path, sizeof(path), _TRUNCATE,
                "%s\\resources\\newtab\\newtab.html", g_ExeDir);
        }
    } else {
        _snprintf_s(path, sizeof(path), _TRUNCATE,
            "%s\\pages\\%s.html", g_ExeDir, page.c_str());
        if (GetFileAttributesA(path) == INVALID_FILE_ATTRIBUTES) {
            _snprintf_s(path, sizeof(path), _TRUNCATE,
                "%s\\resources\\pages\\%s.html", g_ExeDir, page.c_str());
        }
        if (GetFileAttributesA(path) == INVALID_FILE_ATTRIBUTES) {
            _snprintf_s(path, sizeof(path), _TRUNCATE,
                "%s\\resources\\%s\\%s.html", g_ExeDir, page.c_str(), page.c_str());
        }
    }

    // If still not found, fallback safely to newtab instead of passing unhandled scheme to WebView2
    if (GetFileAttributesA(path) == INVALID_FILE_ATTRIBUTES) {
        fprintf(stderr, "[Blink] aeon://%s not found at %s — falling back to newtab\n",
            page.c_str(), path);
        _snprintf_s(path, sizeof(path), _TRUNCATE,
            "%s\\newtab\\newtab.html", g_ExeDir);
        if (GetFileAttributesA(path) == INVALID_FILE_ATTRIBUTES) {
            _snprintf_s(path, sizeof(path), _TRUNCATE,
                "%s\\resources\\newtab\\newtab.html", g_ExeDir);
        }
    }

    // Convert backslashes to forward slashes for file:// URI
    std::string fileUrl = "file:///";
    for (const char* p = path; *p; p++) {
        if (*p == '\\') fileUrl += '/';
        else if (*p == ' ') fileUrl += "%20";
        else fileUrl += *p;
    }
    fileUrl += queryStr;

    fprintf(stdout, "[Blink] aeon://%s → %s\n", fullPage.c_str(), fileUrl.c_str());
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
        case WM_PAINT: {
            PAINTSTRUCT ps;
            HDC hdc = BeginPaint(hWnd, &ps);
            EndPaint(hWnd, &ps);
            return 0;
        }
    }
    return DefWindowProcW(hWnd, msg, wParam, lParam);
}

static void EnsureWindowClass(HINSTANCE hInst) {
    if (g_ClassRegistered) return;
    WNDCLASSEXW wc = {};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = WV2HostProc;
    wc.hInstance = hInst;
    wc.hbrBackground = nullptr;
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
static ComPtr<ICoreWebView2Environment> g_SharedEnvironment = nullptr;
static bool g_EnvironmentInitializing = false;
static std::vector<unsigned int> g_PendingInitTabs;

static void CreateControllerForTab(ICoreWebView2Environment* env, unsigned int tabId) {
    auto it = g_Tabs.find(tabId);
    if (it == g_Tabs.end() || !it->second.childHwnd || !env) return;
    TabState& tab = it->second;

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
                bool shouldShow = (tab.w > 0 && tab.h > 0 && IsWindowVisible(tab.childHwnd));
                controller->put_IsVisible(shouldShow ? TRUE : FALSE);

                // Wire up navigation events
                tab.webview->add_NavigationCompleted(
                    Callback<ICoreWebView2NavigationCompletedEventHandler>(
                        [tabId = tab.id](ICoreWebView2* sender,
                            ICoreWebView2NavigationCompletedEventArgs* args) -> HRESULT {
                            auto it = g_Tabs.find(tabId);
                            if (it == g_Tabs.end()) return S_OK;

                            BOOL isSuccess = FALSE;
                            if (args) args->get_IsSuccess(&isSuccess);
                            COREWEBVIEW2_WEB_ERROR_STATUS status = COREWEBVIEW2_WEB_ERROR_STATUS_UNKNOWN;
                            if (args) args->get_WebErrorStatus(&status);

                            if (!isSuccess && status == COREWEBVIEW2_WEB_ERROR_STATUS_OPERATION_CANCELED) {
                                if (it->second.loading) {
                                    it->second.loading = false;
                                    if (g_Callbacks.OnLoaded)
                                        g_Callbacks.OnLoaded(tabId);
                                }
                                return S_OK;
                            }

                            bool wasLoading = it->second.loading;
                            it->second.loading = false;
                            if (wasLoading && g_Callbacks.OnProgress)
                                g_Callbacks.OnProgress(tabId, 100);
                            if (g_Callbacks.OnLoaded)
                                g_Callbacks.OnLoaded(tabId);

                            // ── Cosmetic CSS: element-hide rules ──
                            // Inject after page load to hide ad containers,
                            // cookie banners, and other unwanted elements.
                            const char* css = NativeAdBlock::GetCosmeticCSS();
                            if (css) {
                                // Wrap in <style> injection via JS
                                std::string js = "(() => { const s = document.createElement('style'); s.textContent = `";
                                js += css;
                                js += "`; document.head.appendChild(s); })();";
                                free(const_cast<char*>(css));

                                int wLen = MultiByteToWideChar(CP_UTF8, 0,
                                    js.c_str(), -1, nullptr, 0);
                                wchar_t* wJs = new wchar_t[wLen];
                                MultiByteToWideChar(CP_UTF8, 0,
                                    js.c_str(), -1, wJs, wLen);
                                sender->ExecuteScript(wJs, nullptr);
                                delete[] wJs;
                            }
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

                // ── Intercept aeon:// scheme navigations to avoid OS protocol prompts ──
                tab.webview->add_NavigationStarting(
                    Callback<ICoreWebView2NavigationStartingEventHandler>(
                        [tabId = tab.id](ICoreWebView2* sender,
                            ICoreWebView2NavigationStartingEventArgs* args) -> HRESULT {
                            LPWSTR wUri = nullptr;
                            args->get_Uri(&wUri);
                            char uri[2048] = {0};
                            if (wUri) {
                                WideCharToMultiByte(CP_UTF8, 0, wUri, -1, uri, sizeof(uri), nullptr, nullptr);
                                CoTaskMemFree(wUri);
                            }

                            bool isInternalNewTab = (strncmp(uri, "aeon://newtab", 13) == 0 ||
                                                     strstr(uri, "newtab.html") != nullptr ||
                                                     strncmp(uri, "about:blank", 11) == 0);

                            auto it = g_Tabs.find(tabId);
                            if (it != g_Tabs.end()) {
                                it->second.loading = !isInternalNewTab;
                            }
                            if (!isInternalNewTab && g_Callbacks.OnProgress)
                                g_Callbacks.OnProgress(tabId, 20);

                            if (strncmp(uri, "aeon://", 7) == 0) {
                                args->put_Cancel(TRUE);
                                std::string resolved = ResolveAeonUrl(uri);
                                if (!resolved.empty()) {
                                    wchar_t wResolved[2048];
                                    MultiByteToWideChar(CP_UTF8, 0, resolved.c_str(), -1, wResolved, 2048);
                                    sender->Navigate(wResolved);
                                }
                                if (g_Callbacks.OnNavigated)
                                    g_Callbacks.OnNavigated(tabId, uri);
                                return S_OK;
                            }
                            return S_OK;
                        }).Get(), nullptr);

                // ── ContentLoading: advance loading progress when DOM parsing begins ──
                tab.webview->add_ContentLoading(
                    Callback<ICoreWebView2ContentLoadingEventHandler>(
                        [tabId = tab.id](ICoreWebView2* sender,
                            ICoreWebView2ContentLoadingEventArgs* args) -> HRESULT {
                            auto it = g_Tabs.find(tabId);
                            if (it != g_Tabs.end() && !it->second.loading) {
                                return S_OK;
                            }
                            if (g_Callbacks.OnProgress)
                                g_Callbacks.OnProgress(tabId, 65);
                            return S_OK;
                        }).Get(), nullptr);

                // ── Handle web messages (e.g. postMessage navigate and window.ai Prompt API) ──
                tab.webview->add_WebMessageReceived(
                    Callback<ICoreWebView2WebMessageReceivedEventHandler>(
                        [tabId = tab.id](ICoreWebView2* sender,
                            ICoreWebView2WebMessageReceivedEventArgs* args) -> HRESULT {
                            LPWSTR wMsg = nullptr;
                            args->get_WebMessageAsJson(&wMsg);
                            if (wMsg) {
                                char msg[16384];
                                WideCharToMultiByte(CP_UTF8, 0, wMsg, -1, msg, sizeof(msg), nullptr, nullptr);
                                CoTaskMemFree(wMsg);

                                std::string jsonStr = msg;

                                // 1. Navigate message
                                size_t navPos = jsonStr.find("\"navigate\"");
                                if (navPos != std::string::npos) {
                                    size_t urlKey = jsonStr.find("\"url\":", navPos);
                                    if (urlKey != std::string::npos) {
                                        size_t qStart = jsonStr.find("\"", urlKey + 6);
                                        if (qStart != std::string::npos) {
                                            size_t qEnd = jsonStr.find("\"", qStart + 1);
                                            if (qEnd != std::string::npos) {
                                                std::string targetUrl = jsonStr.substr(qStart + 1, qEnd - qStart - 1);
                                                std::string resolved = ResolveAeonUrl(targetUrl);
                                                const std::string& actualUrl = resolved.empty() ? targetUrl : resolved;
                                                wchar_t wTarget[2048];
                                                MultiByteToWideChar(CP_UTF8, 0, actualUrl.c_str(), -1, wTarget, 2048);
                                                sender->Navigate(wTarget);
                                                if (g_Callbacks.OnNavigated)
                                                    g_Callbacks.OnNavigated(tabId, targetUrl.c_str());
                                            }
                                        }
                                    }
                                }

                                // 2. window_ai_prompt (WICG Prompt API single-turn prompt)
                                if (jsonStr.find("\"window_ai_prompt\"") != std::string::npos) {
                                    std::string reqId = "ai_req_default";
                                    std::string prompt;
                                    try {
                                        auto j = json::parse(jsonStr);
                                        if (j.contains("id") && j["id"].is_string()) reqId = j["id"].get<std::string>();
                                        if (j.contains("prompt") && j["prompt"].is_string()) prompt = j["prompt"].get<std::string>();
                                    } catch (...) {}

                                    ComPtr<ICoreWebView2> wvSender(sender);
                                    AeonAIInstance().PromptAsync(prompt, [wvSender, reqId](const std::string& text, bool is_final) {
                                        if (is_final) {
                                            json resp;
                                            resp["type"] = "window_ai_response";
                                            resp["id"] = reqId;
                                            resp["ok"] = true;
                                            resp["text"] = text;
                                            std::string respStr = resp.dump();

                                            int wLen = MultiByteToWideChar(CP_UTF8, 0, respStr.c_str(), -1, nullptr, 0);
                                            std::wstring wResp(wLen, L'\0');
                                            MultiByteToWideChar(CP_UTF8, 0, respStr.c_str(), -1, &wResp[0], wLen);
                                            wvSender->PostWebMessageAsJson(wResp.c_str());
                                        }
                                    });
                                }

                                // 3. window_ai_prompt_stream (WICG Prompt API streaming prompt)
                                if (jsonStr.find("\"window_ai_prompt_stream\"") != std::string::npos) {
                                    std::string reqId = "ai_stream_default";
                                    std::string prompt;
                                    try {
                                        auto j = json::parse(jsonStr);
                                        if (j.contains("id") && j["id"].is_string()) reqId = j["id"].get<std::string>();
                                        if (j.contains("prompt") && j["prompt"].is_string()) prompt = j["prompt"].get<std::string>();
                                    } catch (...) {}

                                    ComPtr<ICoreWebView2> wvSender(sender);
                                    AeonAIInstance().PromptAsync(prompt, [wvSender, reqId](const std::string& token, bool is_final) {
                                        json resp;
                                        resp["type"] = "window_ai_stream";
                                        resp["id"] = reqId;
                                        resp["chunk"] = token;
                                        resp["done"] = is_final;
                                        std::string respStr = resp.dump();

                                        int wLen = MultiByteToWideChar(CP_UTF8, 0, respStr.c_str(), -1, nullptr, 0);
                                        std::wstring wResp(wLen, L'\0');
                                        MultiByteToWideChar(CP_UTF8, 0, respStr.c_str(), -1, &wResp[0], wLen);
                                        wvSender->PostWebMessageAsJson(wResp.c_str());
                                    });
                                }
                            }
                            return S_OK;
                        }).Get(), nullptr);

                // ── NativeAdBlock: WebResourceRequested with CNAME Uncloaking ──
                // Register a wildcard filter to intercept ALL
                // sub-resource requests (scripts, images, iframes,
                // XHR, etc.) and block ad/tracker and cloaked tracker domains.
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

                            // Fast path: ignore non-HTTP requests instantly without UTF-8 conversion or mutex locks
                            if (wUri[0] != L'h' && wUri[0] != L'H') {
                                CoTaskMemFree(wUri);
                                return S_OK;
                            }
                            if (_wcsnicmp(wUri, L"http://", 7) != 0 && _wcsnicmp(wUri, L"https://", 8) != 0) {
                                CoTaskMemFree(wUri);
                                return S_OK;
                            }

                            // Convert to UTF-8 for NativeAdBlock
                            char uri[2048];
                            WideCharToMultiByte(CP_UTF8, 0, wUri, -1,
                                uri, sizeof(uri), nullptr, nullptr);
                            CoTaskMemFree(wUri);

                            // Check against loaded filter lists & recursive CNAME uncloaking
                            std::string blockedDomain;
                            if (NativeAdBlock::ShouldBlockWithCname(uri, blockedDomain)) {
                                // Block: create proper 403 response
                                if (envPtr) {
                                    ComPtr<ICoreWebView2WebResourceResponse> response;
                                    envPtr->CreateWebResourceResponse(
                                        nullptr, 403,
                                        L"Blocked by Aeon Shield (CNAME Uncloaked)",
                                        L"X-Aeon-Shield: Blocked-CNAME-Tracker\r\nContent-Type: text/plain\r\n",
                                        &response);
                                    if (response)
                                        args->put_Response(response.Get());
                                }
                                fprintf(stdout, "[AdBlock:CNAME] Blocked cloaked tracker: %s (uncloaked: %s)\n",
                                    uri, blockedDomain.c_str());
                            }
                            return S_OK;
                        }).Get(), nullptr);

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

                // ── window.ai Standard WICG Prompt API Polyfill ──
                {
                    const wchar_t* windowAiPolyfillJs = LR"JS(
(function() {
    'use strict';
    if (window.ai && window.ai.createTextSession && window.ai.languageModel) return;

    class AITextSession {
        constructor(options = {}) {
            this.options = {
                topK: options.topK || 3,
                temperature: options.temperature !== undefined ? options.temperature : 0.7,
                systemPrompt: options.systemPrompt || ''
            };
            this._destroyed = false;
        }

        async prompt(input) {
            if (this._destroyed) throw new Error('AITextSession has been destroyed');
            const id = 'ai_req_' + Math.random().toString(36).substring(2, 11);

            return new Promise((resolve, reject) => {
                const handler = (e) => {
                    let data = e.data;
                    if (typeof data === 'string') {
                        try { data = JSON.parse(data); } catch(err) {}
                    }
                    if (data && data.type === 'window_ai_response' && data.id === id) {
                        if (window.chrome && window.chrome.webview) {
                            window.chrome.webview.removeEventListener('message', handler);
                        }
                        if (data.ok === false) {
                            reject(new Error(data.error || 'Inference failed'));
                        } else {
                            resolve(data.text || '');
                        }
                    }
                };

                if (window.chrome && window.chrome.webview && window.chrome.webview.postMessage) {
                    window.chrome.webview.addEventListener('message', handler);
                    window.chrome.webview.postMessage({
                        type: 'window_ai_prompt',
                        id: id,
                        prompt: input,
                        options: this.options
                    });
                } else if (window.aeonBridge && window.aeonBridge.queryAI) {
                    window.aeonBridge.queryAI(input, (res) => {
                        if (res && res.ok !== false) resolve(res.text || res);
                        else reject(new Error(res ? res.error : 'Bridge query failed'));
                    });
                } else {
                    resolve('Aeon Sovereign AI response: ' + input);
                }
            });
        }

        promptStreaming(input) {
            if (this._destroyed) throw new Error('AITextSession has been destroyed');
            const id = 'ai_stream_' + Math.random().toString(36).substring(2, 11);

            return new ReadableStream({
                start(controller) {
                    const handler = (e) => {
                        let data = e.data;
                        if (typeof data === 'string') {
                            try { data = JSON.parse(data); } catch(err) {}
                        }
                        if (data && data.type === 'window_ai_stream' && data.id === id) {
                            if (data.chunk) controller.enqueue(data.chunk);
                            if (data.done) {
                                if (window.chrome && window.chrome.webview) {
                                    window.chrome.webview.removeEventListener('message', handler);
                                }
                                controller.close();
                            }
                        }
                    };
                    if (window.chrome && window.chrome.webview && window.chrome.webview.postMessage) {
                        window.chrome.webview.addEventListener('message', handler);
                        window.chrome.webview.postMessage({
                            type: 'window_ai_prompt_stream',
                            id: id,
                            prompt: input,
                            options: this.options
                        });
                    } else if (window.aeonBridge && window.aeonBridge.queryAI) {
                        window.aeonBridge.queryAI(input, (res) => {
                            const text = (res && res.text) ? res.text : (res || '');
                            controller.enqueue(text);
                            controller.close();
                        });
                    } else {
                        controller.enqueue('Aeon Sovereign AI streaming: ' + input);
                        controller.close();
                    }
                }
            });
        }

        destroy() {
            this._destroyed = true;
        }

        clone() {
            if (this._destroyed) throw new Error('AITextSession has been destroyed');
            return new AITextSession(this.options);
        }
    }

    const aiApi = {
        canCreateTextSession: async () => 'readily',
        defaultTextSessionOptions: async () => ({ topK: 3, temperature: 0.7 }),
        createTextSession: async (options) => new AITextSession(options),
        languageModel: {
            capabilities: async () => ({ available: 'readily', defaultTopK: 3, maxTopK: 10, defaultTemperature: 0.7 }),
            create: async (options) => new AITextSession(options)
        }
    };

    window.ai = aiApi;
    if (!window.modelContext) window.modelContext = window.ai;
    console.log('[AeonAI] window.ai standard WICG Prompt API polyfill active');
})();
)JS";
                    tab.webview->AddScriptToExecuteOnDocumentCreated(
                        windowAiPolyfillJs, nullptr);
                    fprintf(stdout,
                        "[Blink] window.ai Prompt API polyfill injected for tab #%u\n",
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
}

static void InitWebView2ForTab(TabState& tab) {
    if (!tab.childHwnd) return;

    if (g_SharedEnvironment) {
        CreateControllerForTab(g_SharedEnvironment.Get(), tab.id);
        return;
    }

    g_PendingInitTabs.push_back(tab.id);
    if (g_EnvironmentInitializing) {
        return;
    }
    g_EnvironmentInitializing = true;

    // Get user data folder for WebView2 profile
    char appData[MAX_PATH] = {};
    SHGetFolderPathA(nullptr, CSIDL_LOCAL_APPDATA, nullptr, 0, appData);
    char profileDir[MAX_PATH];
    _snprintf_s(profileDir, sizeof(profileDir), _TRUNCATE,
        "%s\\DelgadoLogic\\Aeon\\WebView2Profile", appData);
    SHCreateDirectoryExA(nullptr, profileDir, nullptr);

    wchar_t wProfileDir[MAX_PATH];
    MultiByteToWideChar(CP_UTF8, 0, profileDir, -1, wProfileDir, MAX_PATH);

    // Create environment with additional browser args:
    //   - Disable unnecessary Google services for sovereign operation
    //   - Enable CDP remote debugging on port 9222 for AI agent control
    // Note: CoreWebView2EnvironmentOptions is provided by WebView2EnvironmentOptions.h
    auto envOptions = Microsoft::WRL::Make<CoreWebView2EnvironmentOptions>();
    envOptions->put_AdditionalBrowserArguments(
        L"--enable-features=PostQuantumKyber,X25519Kyber768Draft00,X25519MLKEM768,ParallelDownloading"
        L" --disable-features=SafeBrowsing,PrintPreview,TranslateUI"
        L" --disable-sync"
        L" --no-first-run"
        L" --disable-default-apps"
        L" --disable-component-update"
        L" --remote-allow-origins=*"
    );

    HRESULT hr = CreateCoreWebView2EnvironmentWithOptions(
        nullptr,  // browserExecutableFolder — uses installed Edge
        wProfileDir,
        envOptions.Get(),
        Callback<ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler>(
            [](HRESULT result, ICoreWebView2Environment* env) -> HRESULT {
                g_EnvironmentInitializing = false;
                if (FAILED(result) || !env) {
                    fprintf(stderr, "[Blink] WebView2 env creation failed: 0x%08x\n", result);
                    for (unsigned int pendingId : g_PendingInitTabs) {
                        auto it = g_Tabs.find(pendingId);
                        if (it != g_Tabs.end()) PaintFallback(it->second);
                    }
                    g_PendingInitTabs.clear();
                    return S_OK;
                }

                g_SharedEnvironment = env;
                std::vector<unsigned int> pending = g_PendingInitTabs;
                g_PendingInitTabs.clear();

                for (unsigned int pendingId : pending) {
                    CreateControllerForTab(g_SharedEnvironment.Get(), pendingId);
                }
                return S_OK;
            }).Get());

    if (FAILED(hr)) {
        g_EnvironmentInitializing = false;
        fprintf(stderr, "[Blink] CreateCoreWebView2EnvironmentWithOptions failed: 0x%08x\n", hr);
        for (unsigned int pendingId : g_PendingInitTabs) {
            auto it = g_Tabs.find(pendingId);
            if (it != g_Tabs.end()) PaintFallback(it->second);
        }
        g_PendingInitTabs.clear();
    }
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
    g_SharedEnvironment = nullptr;
    g_PendingInitTabs.clear();
    g_EnvironmentInitializing = false;
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

    bool isInternalNewTab = (tab.url == "aeon://newtab" ||
                             tab.url.find("newtab.html") != std::string::npos ||
                             tab.url == "about:blank");
    tab.loading = !isInternalNewTab;

    if (!isInternalNewTab && g_Callbacks.OnProgress) g_Callbacks.OnProgress(tab_id, 10);

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
        if (!isInternalNewTab && g_Callbacks.OnProgress) g_Callbacks.OnProgress(tab_id, 100);
        if (g_Callbacks.OnLoaded)   g_Callbacks.OnLoaded(tab_id);
        tab.loading = false;
    }
#else
    PaintFallback(tab);
    if (g_Callbacks.OnNavigated) g_Callbacks.OnNavigated(tab_id, tab.url.c_str());
    if (!isInternalNewTab && g_Callbacks.OnProgress)  g_Callbacks.OnProgress(tab_id, 100);
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
    auto it = g_Tabs.find(tab_id);
    if (it != g_Tabs.end()) {
        TabState& tab = it->second;
        if (tab.childHwnd) {
            ::ShowWindow(tab.childHwnd, SW_SHOW);
            ::SetFocus(tab.childHwnd);
#ifdef AEON_HAS_WEBVIEW2
            if (tab.controller) {
                tab.controller->put_IsVisible(TRUE);
                tab.controller->MoveFocus(COREWEBVIEW2_MOVE_FOCUS_REASON_PROGRAMMATIC);
            }
#endif
        } else {
            PaintFallback(tab);
        }
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

    bool shouldShow = (w > 0 && h > 0);

#ifdef AEON_HAS_WEBVIEW2
    // Create child window for WebView2 if it doesn't exist
    if (!t.childHwnd && t.hostHwnd) {
        EnsureWindowClass(g_hInst);
        t.childHwnd = CreateWindowExW(
            0, AEON_WV2_CLASS, L"",
            WS_CHILD | (shouldShow ? WS_VISIBLE : 0) | WS_CLIPCHILDREN,
            x, y, (std::max)(1, w), (std::max)(1, h),
            t.hostHwnd, nullptr, g_hInst, nullptr);
        SetWindowLongPtr(t.childHwnd, GWLP_USERDATA, (LONG_PTR)tab_id);

        // Initialize WebView2 in the child window
        InitWebView2ForTab(t);
    } else if (t.childHwnd) {
        MoveWindow(t.childHwnd, x, y, (std::max)(0, w), (std::max)(0, h), TRUE);
        ::ShowWindow(t.childHwnd, shouldShow ? SW_SHOW : SW_HIDE);
        if (t.controller) {
            RECT bounds = { 0, 0, (std::max)(0, w), (std::max)(0, h) };
            t.controller->put_Bounds(bounds);
            t.controller->put_IsVisible(shouldShow ? TRUE : FALSE);
        }
    }
#else
    if (shouldShow) PaintFallback(t);
#endif
}

// ---------------------------------------------------------------------------
// Storage State (Playwright auth.json compatibility)
// ---------------------------------------------------------------------------
struct StoredCookie {
    std::string name;
    std::string value;
    std::string domain;
    std::string path;
    double expires = -1.0;
    bool httpOnly = false;
    bool secure = false;
    std::string sameSite = "Lax";
};

struct StoredLocalStorageItem {
    std::string name;
    std::string value;
};

struct StoredOrigin {
    std::string origin;
    std::vector<StoredLocalStorageItem> localStorage;
};

static std::vector<StoredCookie> g_MemoryCookies;
static std::vector<StoredOrigin> g_MemoryOrigins;

static std::string JsonEscapeString(const std::string& input) {
    std::string out;
    out.reserve(input.size() * 2);
    for (char c : input) {
        switch (c) {
            case '"':  out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\b': out += "\\b";  break;
            case '\f': out += "\\f";  break;
            case '\n': out += "\\n";  break;
            case '\r': out += "\\r";  break;
            case '\t': out += "\\t";  break;
            default:
                if (static_cast<unsigned char>(c) < 32) {
                    char buf[16];
                    _snprintf_s(buf, sizeof(buf), _TRUNCATE, "\\u%04x", static_cast<unsigned char>(c));
                    out += buf;
                } else {
                    out += c;
                }
                break;
        }
    }
    return out;
}

static int __cdecl Engine_ExportStorageState(const char* json_path) {
    if (!json_path || !*json_path) return -1;

    std::vector<StoredCookie> cookies = g_MemoryCookies;
    std::vector<StoredOrigin> origins = g_MemoryOrigins;

#ifdef AEON_HAS_WEBVIEW2
    // 1. Live extraction of cookies via WebView2 CookieManager
    for (auto& [id, tab] : g_Tabs) {
        if (tab.webview) {
            ComPtr<ICoreWebView2_2> wv2_2;
            if (SUCCEEDED(tab.webview.As(&wv2_2)) && wv2_2) {
                ComPtr<ICoreWebView2CookieManager> cookieManager;
                if (SUCCEEDED(wv2_2->get_CookieManager(&cookieManager)) && cookieManager) {
                    struct CookieContext {
                        HANDLE hEvent = nullptr;
                        std::vector<StoredCookie> liveCookies;
                        std::atomic<bool> done{false};
                    };
                    auto cCtx = std::make_shared<CookieContext>();
                    cCtx->hEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);

                    HRESULT hr = cookieManager->GetCookies(nullptr,
                        Callback<ICoreWebView2GetCookiesCompletedHandler>(
                            [cCtx](HRESULT res, ICoreWebView2CookieList* list) -> HRESULT {
                                if (SUCCEEDED(res) && list) {
                                    UINT count = 0;
                                    list->get_Count(&count);
                                    for (UINT i = 0; i < count; i++) {
                                        ComPtr<ICoreWebView2Cookie> c;
                                        if (SUCCEEDED(list->GetValueAtIndex(i, &c)) && c) {
                                            StoredCookie sc;
                                            LPWSTR wStr = nullptr;
                                            if (SUCCEEDED(c->get_Name(&wStr)) && wStr) {
                                                char buf[512] = {};
                                                WideCharToMultiByte(CP_UTF8, 0, wStr, -1, buf, sizeof(buf), nullptr, nullptr);
                                                sc.name = buf; CoTaskMemFree(wStr);
                                            }
                                            if (SUCCEEDED(c->get_Value(&wStr)) && wStr) {
                                                char buf[8192] = {};
                                                WideCharToMultiByte(CP_UTF8, 0, wStr, -1, buf, sizeof(buf), nullptr, nullptr);
                                                sc.value = buf; CoTaskMemFree(wStr);
                                            }
                                            if (SUCCEEDED(c->get_Domain(&wStr)) && wStr) {
                                                char buf[512] = {};
                                                WideCharToMultiByte(CP_UTF8, 0, wStr, -1, buf, sizeof(buf), nullptr, nullptr);
                                                sc.domain = buf; CoTaskMemFree(wStr);
                                            }
                                            if (SUCCEEDED(c->get_Path(&wStr)) && wStr) {
                                                char buf[512] = {};
                                                WideCharToMultiByte(CP_UTF8, 0, wStr, -1, buf, sizeof(buf), nullptr, nullptr);
                                                sc.path = buf; CoTaskMemFree(wStr);
                                            }
                                            BOOL isSession = FALSE;
                                            c->get_IsSession(&isSession);
                                            if (!isSession) c->get_Expires(&sc.expires);
                                            else sc.expires = -1.0;

                                            BOOL httpOnly = FALSE; c->get_IsHttpOnly(&httpOnly); sc.httpOnly = (httpOnly == TRUE);
                                            BOOL secure = FALSE; c->get_IsSecure(&secure); sc.secure = (secure == TRUE);

                                            COREWEBVIEW2_COOKIE_SAME_SITE_KIND kind;
                                            if (SUCCEEDED(c->get_SameSite(&kind))) {
                                                if (kind == COREWEBVIEW2_COOKIE_SAME_SITE_KIND_STRICT) sc.sameSite = "Strict";
                                                else if (kind == COREWEBVIEW2_COOKIE_SAME_SITE_KIND_NONE) sc.sameSite = "None";
                                                else sc.sameSite = "Lax";
                                            }
                                            cCtx->liveCookies.push_back(sc);
                                        }
                                    }
                                }
                                cCtx->done.store(true);
                                if (cCtx->hEvent) SetEvent(cCtx->hEvent);
                                return S_OK;
                            }).Get());

                    if (SUCCEEDED(hr)) {
                        DWORD start = GetTickCount();
                        while (WaitForSingleObject(cCtx->hEvent, 0) != WAIT_OBJECT_0) {
                            MSG msg;
                            if (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
                                if (msg.message == WM_QUIT) {
                                    PostQuitMessage((int)msg.wParam);
                                    break;
                                }
                                TranslateMessage(&msg); DispatchMessageW(&msg);
                            } else Sleep(5);
                            if (GetTickCount() - start > 5000) break;
                        }
                        if (!cCtx->liveCookies.empty()) cookies = cCtx->liveCookies;
                    }
                    HANDLE hEv = cCtx->hEvent;
                    cCtx->hEvent = nullptr;
                    if (hEv) CloseHandle(hEv);
                }
            }
            break;
        }
    }

    // 2. Extract LocalStorage across active web origins
    for (auto& [id, tab] : g_Tabs) {
        if (tab.webview && !tab.url.empty() && (tab.url.rfind("http://", 0) == 0 || tab.url.rfind("https://", 0) == 0)) {
            size_t slashPos = tab.url.find('/', 8);
            std::string originStr = (slashPos != std::string::npos) ? tab.url.substr(0, slashPos) : tab.url;

            struct LsContext {
                HANDLE hLsEvent = nullptr;
                std::string lsJson;
                std::atomic<bool> done{false};
            };
            auto lsCtx = std::make_shared<LsContext>();
            lsCtx->hLsEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);

            tab.webview->ExecuteScript(
                L"(function(){ try { return JSON.stringify(Object.entries(localStorage)); } catch(e) { return '[]'; } })()",
                Callback<ICoreWebView2ExecuteScriptCompletedHandler>(
                    [lsCtx](HRESULT res, LPCWSTR resultObjectAsJson) -> HRESULT {
                        if (SUCCEEDED(res) && resultObjectAsJson) {
                            char buf[16384] = {};
                            WideCharToMultiByte(CP_UTF8, 0, resultObjectAsJson, -1, buf, sizeof(buf), nullptr, nullptr);
                            lsCtx->lsJson = buf;
                        }
                        lsCtx->done.store(true);
                        if (lsCtx->hLsEvent) SetEvent(lsCtx->hLsEvent);
                        return S_OK;
                    }).Get());

            DWORD start = GetTickCount();
            while (WaitForSingleObject(lsCtx->hLsEvent, 0) != WAIT_OBJECT_0) {
                MSG msg;
                if (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
                    if (msg.message == WM_QUIT) {
                        PostQuitMessage((int)msg.wParam);
                        break;
                    }
                    TranslateMessage(&msg); DispatchMessageW(&msg);
                } else Sleep(5);
                if (GetTickCount() - start > 2000) break;
            }
            HANDLE hEv = lsCtx->hLsEvent;
            lsCtx->hLsEvent = nullptr;
            if (hEv) CloseHandle(hEv);

            if (!lsCtx->lsJson.empty() && lsCtx->lsJson != "\"[]\"" && lsCtx->lsJson != "[]") {
                std::string raw = lsCtx->lsJson;
                if (raw.size() >= 2 && raw.front() == '"' && raw.back() == '"') {
                    raw = raw.substr(1, raw.size() - 2);
                    std::string unesc;
                    for (size_t i = 0; i < raw.size(); i++) {
                        if (raw[i] == '\\' && i + 1 < raw.size()) {
                            i++; unesc += raw[i];
                        } else unesc += raw[i];
                    }
                    raw = unesc;
                }

                StoredOrigin orig;
                orig.origin = originStr;
                size_t p = 0;
                while ((p = raw.find("[\"", p)) != std::string::npos) {
                    size_t kEnd = raw.find("\",\"", p + 2);
                    if (kEnd == std::string::npos) break;
                    std::string key = raw.substr(p + 2, kEnd - (p + 2));
                    size_t vEnd = raw.find("\"]", kEnd + 3);
                    if (vEnd == std::string::npos) break;
                    std::string val = raw.substr(kEnd + 3, vEnd - (kEnd + 3));
                    orig.localStorage.push_back({ key, val });
                    p = vEnd + 2;
                }
                if (!orig.localStorage.empty()) {
                    origins.push_back(orig);
                }
            }
        }
    }
#endif

    // Ensure parent directory exists
    std::filesystem::path fsPath(json_path);
    if (fsPath.has_parent_path()) {
        std::error_code ec;
        std::filesystem::create_directories(fsPath.parent_path(), ec);
    }

    FILE* f = nullptr;
    fopen_s(&f, json_path, "w");
    if (!f) return -1;

    fprintf(f, "{\n  \"cookies\": [\n");
    for (size_t i = 0; i < cookies.size(); i++) {
        fprintf(f, "    {\n");
        fprintf(f, "      \"name\": \"%s\",\n", JsonEscapeString(cookies[i].name).c_str());
        fprintf(f, "      \"value\": \"%s\",\n", JsonEscapeString(cookies[i].value).c_str());
        fprintf(f, "      \"domain\": \"%s\",\n", JsonEscapeString(cookies[i].domain).c_str());
        fprintf(f, "      \"path\": \"%s\",\n", JsonEscapeString(cookies[i].path).c_str());
        if (cookies[i].expires < 0) {
            fprintf(f, "      \"expires\": -1,\n");
        } else {
            fprintf(f, "      \"expires\": %.3f,\n", cookies[i].expires);
        }
        fprintf(f, "      \"httpOnly\": %s,\n", cookies[i].httpOnly ? "true" : "false");
        fprintf(f, "      \"secure\": %s,\n", cookies[i].secure ? "true" : "false");
        fprintf(f, "      \"sameSite\": \"%s\"\n", JsonEscapeString(cookies[i].sameSite).c_str());
        fprintf(f, "    }%s\n", (i + 1 < cookies.size()) ? "," : "");
    }
    fprintf(f, "  ],\n  \"origins\": [\n");
    for (size_t i = 0; i < origins.size(); i++) {
        fprintf(f, "    {\n");
        fprintf(f, "      \"origin\": \"%s\",\n", JsonEscapeString(origins[i].origin).c_str());
        fprintf(f, "      \"localStorage\": [\n");
        for (size_t j = 0; j < origins[i].localStorage.size(); j++) {
            fprintf(f, "        {\n");
            fprintf(f, "          \"name\": \"%s\",\n", JsonEscapeString(origins[i].localStorage[j].name).c_str());
            fprintf(f, "          \"value\": \"%s\"\n", JsonEscapeString(origins[i].localStorage[j].value).c_str());
            fprintf(f, "        }%s\n", (j + 1 < origins[i].localStorage.size()) ? "," : "");
        }
        fprintf(f, "      ]\n");
        fprintf(f, "    }%s\n", (i + 1 < origins.size()) ? "," : "");
    }
    fprintf(f, "  ]\n}\n");
    fclose(f);

    fprintf(stdout, "[Blink] Exported storage state to %s (%zu cookies, %zu origins)\n",
        json_path, cookies.size(), origins.size());

    return static_cast<int>(cookies.size());
}

static int __cdecl Engine_ImportStorageState(const char* json_path) {
    if (!json_path || !*json_path) return -1;

    FILE* f = nullptr;
    fopen_s(&f, json_path, "r");
    if (!f) return -1;

    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);

    if (sz <= 0 || sz > 10 * 1024 * 1024) {
        fclose(f);
        return -1;
    }

    std::string content((size_t)sz, '\0');
    fread(&content[0], 1, (size_t)sz, f);
    fclose(f);

    json jRoot;
    try {
        jRoot = json::parse(content);
        if (!jRoot.is_object()) return -1;
    } catch (...) {
        return -1;
    }

    std::vector<StoredCookie> importedCookies;
    std::vector<StoredOrigin> importedOrigins;

    if (jRoot.contains("cookies") && jRoot["cookies"].is_array()) {
        for (const auto& cObj : jRoot["cookies"]) {
            if (cObj.is_object()) {
                StoredCookie sc;
                if (cObj.contains("name") && cObj["name"].is_string()) sc.name = cObj["name"].get<std::string>();
                if (cObj.contains("value") && cObj["value"].is_string()) sc.value = cObj["value"].get<std::string>();
                if (cObj.contains("domain") && cObj["domain"].is_string()) sc.domain = cObj["domain"].get<std::string>();
                if (cObj.contains("path") && cObj["path"].is_string()) sc.path = cObj["path"].get<std::string>();
                if (cObj.contains("sameSite") && cObj["sameSite"].is_string()) sc.sameSite = cObj["sameSite"].get<std::string>();
                if (cObj.contains("expires") && cObj["expires"].is_number()) sc.expires = cObj["expires"].get<double>();
                if (cObj.contains("httpOnly") && cObj["httpOnly"].is_boolean()) sc.httpOnly = cObj["httpOnly"].get<bool>();
                if (cObj.contains("secure") && cObj["secure"].is_boolean()) sc.secure = cObj["secure"].get<bool>();

                if (!sc.name.empty()) {
                    importedCookies.push_back(sc);
                }
            }
        }
    }

    if (jRoot.contains("origins") && jRoot["origins"].is_array()) {
        for (const auto& oObj : jRoot["origins"]) {
            if (oObj.is_object()) {
                StoredOrigin so;
                if (oObj.contains("origin") && oObj["origin"].is_string()) so.origin = oObj["origin"].get<std::string>();
                if (oObj.contains("localStorage") && oObj["localStorage"].is_array()) {
                    for (const auto& itemObj : oObj["localStorage"]) {
                        if (itemObj.is_object()) {
                            StoredLocalStorageItem item;
                            if (itemObj.contains("name") && itemObj["name"].is_string()) item.name = itemObj["name"].get<std::string>();
                            if (itemObj.contains("value") && itemObj["value"].is_string()) item.value = itemObj["value"].get<std::string>();
                            if (!item.name.empty()) {
                                so.localStorage.push_back(item);
                            }
                        }
                    }
                }
                if (!so.origin.empty()) {
                    importedOrigins.push_back(so);
                }
            }
        }
    }

    g_MemoryCookies = importedCookies;
    g_MemoryOrigins = importedOrigins;

#ifdef AEON_HAS_WEBVIEW2
    // Apply cookies to active WebView2 CookieManager
    for (auto& [id, tab] : g_Tabs) {
        if (tab.webview) {
            ComPtr<ICoreWebView2_2> wv2_2;
            if (SUCCEEDED(tab.webview.As(&wv2_2)) && wv2_2) {
                ComPtr<ICoreWebView2CookieManager> cookieManager;
                if (SUCCEEDED(wv2_2->get_CookieManager(&cookieManager)) && cookieManager) {
                    for (const auto& c : importedCookies) {
                        wchar_t wName[512] = {}, wVal[8192] = {}, wDom[512] = {}, wPath[512] = {};
                        MultiByteToWideChar(CP_UTF8, 0, c.name.c_str(), -1, wName, 512);
                        MultiByteToWideChar(CP_UTF8, 0, c.value.c_str(), -1, wVal, 8192);
                        MultiByteToWideChar(CP_UTF8, 0, c.domain.c_str(), -1, wDom, 512);
                        MultiByteToWideChar(CP_UTF8, 0, c.path.c_str(), -1, wPath, 512);

                        ComPtr<ICoreWebView2Cookie> wvCookie;
                        if (SUCCEEDED(cookieManager->CreateCookie(wName, wVal, wDom, wPath, &wvCookie)) && wvCookie) {
                            if (c.expires >= 0) {
                                double expSec = c.expires;
                                if (expSec > 1e11) expSec /= 1000.0;
                                wvCookie->put_Expires(expSec);
                            }

                            wvCookie->put_IsHttpOnly(c.httpOnly ? TRUE : FALSE);
                            wvCookie->put_IsSecure(c.secure ? TRUE : FALSE);

                            if (c.sameSite == "Strict") wvCookie->put_SameSite(COREWEBVIEW2_COOKIE_SAME_SITE_KIND_STRICT);
                            else if (c.sameSite == "None") wvCookie->put_SameSite(COREWEBVIEW2_COOKIE_SAME_SITE_KIND_NONE);
                            else wvCookie->put_SameSite(COREWEBVIEW2_COOKIE_SAME_SITE_KIND_LAX);

                            cookieManager->AddOrUpdateCookie(wvCookie.Get());
                        }
                    }
                }
            }
            break;
        }
    }

    // Apply LocalStorage via safe JS restoration script injection
    for (const auto& so : importedOrigins) {
        if (so.localStorage.empty()) continue;

        json itemsArr = json::array();
        for (const auto& item : so.localStorage) {
            itemsArr.push_back({
                {"name", item.name},
                {"value", item.value}
            });
        }

        json originJson = so.origin;
        json itemsJsonStr = itemsArr.dump();

        std::string js = "(function(){\n try {\n  if (window.location.origin === " + originJson.dump() + ") {\n"
                         "   const data = JSON.parse(" + itemsJsonStr.dump() + ");\n"
                         "   for (const item of data) {\n"
                         "    localStorage.setItem(item.name, item.value);\n"
                         "   }\n"
                         "  }\n } catch(e) {}\n})();\n";

        wchar_t* wJs = new wchar_t[js.size() + 1];
        MultiByteToWideChar(CP_UTF8, 0, js.c_str(), -1, wJs, (int)js.size() + 1);

        for (auto& [id, tab] : g_Tabs) {
            if (tab.webview) {
                tab.webview->AddScriptToExecuteOnDocumentCreated(wJs, nullptr);
                tab.webview->ExecuteScript(wJs, nullptr);
            }
        }
        delete[] wJs;
    }
#endif

    fprintf(stdout, "[Blink] Imported storage state from %s (%zu cookies, %zu origins)\n",
        json_path, importedCookies.size(), importedOrigins.size());

    return static_cast<int>(importedCookies.size());
}

static int __cdecl Engine_ExecuteScript(unsigned int tab_id, const char* script, char* out_buf, unsigned int out_buf_len) {
    if (!out_buf || out_buf_len == 0) return 0;
    out_buf[0] = '\0';
    if (!script || !*script) return 0;

#ifdef AEON_HAS_WEBVIEW2
    auto it = g_Tabs.find(tab_id);
    if (it == g_Tabs.end() || !it->second.webview) return 0;

    int wLen = MultiByteToWideChar(CP_UTF8, 0, script, -1, nullptr, 0);
    if (wLen <= 0) return 0;
    std::wstring wScript(wLen, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, script, -1, &wScript[0], wLen);

    struct ExecContext {
        HANDLE hEvent = nullptr;
        std::string jsRes;
        std::atomic<bool> done{false};
    };
    auto ctx = std::make_shared<ExecContext>();
    ctx->hEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);

    HRESULT hr = it->second.webview->ExecuteScript(
        wScript.c_str(),
        Callback<ICoreWebView2ExecuteScriptCompletedHandler>(
            [ctx](HRESULT res, LPCWSTR resultObjectAsJson) -> HRESULT {
                if (SUCCEEDED(res) && resultObjectAsJson) {
                    char buf[65536] = {};
                    WideCharToMultiByte(CP_UTF8, 0, resultObjectAsJson, -1, buf, sizeof(buf), nullptr, nullptr);
                    ctx->jsRes = buf;
                }
                ctx->done.store(true);
                if (ctx->hEvent) SetEvent(ctx->hEvent);
                return S_OK;
            }).Get());

    if (SUCCEEDED(hr)) {
        DWORD start = GetTickCount();
        while (WaitForSingleObject(ctx->hEvent, 0) != WAIT_OBJECT_0) {
            MSG msg;
            if (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
                if (msg.message == WM_QUIT) {
                    PostQuitMessage((int)msg.wParam);
                    break;
                }
                TranslateMessage(&msg); DispatchMessageW(&msg);
            } else Sleep(5);
            if (GetTickCount() - start > 3000) break;
        }
    }
    HANDLE hEv = ctx->hEvent;
    ctx->hEvent = nullptr;
    if (hEv) CloseHandle(hEv);

    if (!ctx->jsRes.empty()) {
        strncpy_s(out_buf, out_buf_len, ctx->jsRes.c_str(), _TRUNCATE);
        return static_cast<int>(strlen(out_buf));
    }
#else
    (void)tab_id; (void)script;
#endif

    return 0;
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
    Engine_ExportStorageState,
    Engine_ImportStorageState,
    Engine_ExecuteScript
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
    if (reason == DLL_PROCESS_ATTACH || reason == DLL_THREAD_ATTACH) {
        SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX | SEM_NOOPENFILEERRORBOX);
        HMODULE hK32 = GetModuleHandleW(L"kernel32.dll");
        if (hK32) {
            typedef BOOL(WINAPI* PFN_SetProcessErrorMode)(DWORD);
            PFN_SetProcessErrorMode pfn = (PFN_SetProcessErrorMode)GetProcAddress(hK32, "SetProcessErrorMode");
            if (pfn) pfn(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX | SEM_NOOPENFILEERRORBOX);
            typedef HRESULT(WINAPI* PFN_WerSetFlags)(DWORD);
            PFN_WerSetFlags pfnWer = (PFN_WerSetFlags)GetProcAddress(hK32, "WerSetFlags");
            if (pfnWer) pfnWer(1 /* WER_FAULT_REPORTING_NO_UI */);
        }
        HMODULE hNtdll = GetModuleHandleW(L"ntdll.dll");
        if (hNtdll) {
            typedef LONG(NTAPI* PFN_NtSetInformationProcess)(HANDLE, ULONG, PVOID, ULONG);
            PFN_NtSetInformationProcess pfnNt = (PFN_NtSetInformationProcess)GetProcAddress(hNtdll, "NtSetInformationProcess");
            if (pfnNt) {
                ULONG hardErrorMode = 0;
                pfnNt(GetCurrentProcess(), 12 /* ProcessDefaultHardErrorMode */, &hardErrorMode, sizeof(hardErrorMode));
            }
        }
        _set_abort_behavior(0, _WRITE_ABORT_MSG | _CALL_REPORTFAULT);
        _set_invalid_parameter_handler([](const wchar_t*, const wchar_t*, const wchar_t*, unsigned int, uintptr_t) {});
        _set_purecall_handler([]() {});
    }
    if (reason == DLL_PROCESS_ATTACH) {
        g_hInst = hInst;
        fprintf(stdout, "[Blink] aeon_blink.dll loaded (WebView2 adapter).\n");
    }
    return TRUE;
}
