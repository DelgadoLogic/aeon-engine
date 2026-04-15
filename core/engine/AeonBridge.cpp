// AeonBrowser — AeonBridge.cpp
// DelgadoLogic | Engine Team
//
// PURPOSE: C++ implementation of window.aeonBridge — the JavaScript host object
// that all internal aeon:// pages (settings, history, bookmarks, downloads,
// crash, passwords) call to read and mutate browser state.
//
// ARCHITECTURE:
//   Each aeon:// page calls methods directly:
//     window.aeonBridge.setSetting("adblock", true)
//     window.aeonBridge.getHistory()
//     window.aeonBridge.navigate("https://...")
//
//   How the bridge is injected depends on the active engine tier:
//
//   WebView2 tier (Win10+):
//     ICoreWebView2::AddHostObjectToScript("aeonBridge", pDispatch)
//     where pDispatch is IDispatch over AeonBridgeObject.
//     Each method is accessible directly from JS as window.aeonBridge.METHOD().
//
//   CEF tier (Win8.1, Win7 Extended):
//     CefV8Handler custom object, registered via CefRegisterExtension().
//
//   Embedded fallback (aeon_html4.dll, retro tier):
//     WM_COPYDATA messages from page JS via a custom window message channel.
//     postMessage({aeonBridge: true, method: "navigate", args: [url]}) →
//     a hidden message-only HWND (g_bridge_hwnd) receiving WM_AEONBRIDGE.
//
// THREAD SAFETY: All methods on this object are called from the browser's
// UI/render thread. Calls into engine subsystems (SettingsEngine, HistoryEngine,
// DownloadManager) are all thread-safe internally.

#define WIN32_LEAN_AND_MEAN
#include "AeonBridge.h"
#include "../settings/SettingsEngine.h"
#include "../history/HistoryEngine.h"
#include "../download/DownloadManager.h"
#include "../security/PasswordVault.h"
#include "../session/SessionManager.h"
#include "../network/NetworkSentinel.h"
#include "../network/DnsResolver.h"
#include "../../updater/AutoUpdater.h"
#include <windows.h>
#include <shellapi.h>
#include <shlobj.h>
#include <cstdio>
#include <cstring>
#include <string>
#include <functional>

// ─────────────────────────────────────────────────────────────────────────────
// Internal state
// ─────────────────────────────────────────────────────────────────────────────
static AeonBridgeNavigateFn g_navigateFn = nullptr;  // set by AeonBridge::Init
static HWND                 g_mainHwnd   = nullptr;

// ─────────────────────────────────────────────────────────────────────────────
// JSON serialization helpers (minimal, no external library)
// ─────────────────────────────────────────────────────────────────────────────
static std::string JStr(const char* s) {
    if (!s) return "\"\"";
    std::string r = "\"";
    for (const char* p = s; *p; p++) {
        switch (*p) {
            case '"':  r += "\\\""; break;
            case '\\': r += "\\\\"; break;
            case '\n': r += "\\n";  break;
            case '\r': r += "\\r";  break;
            default:   r += *p;     break;
        }
    }
    r += '"';
    return r;
}
static std::string JBool(bool b)  { return b ? "true" : "false"; }

// Escape backslashes and quotes in a string for safe JS string interpolation.
// Prevents injection/breakage from Windows paths like C:\Users\...
static std::string JEscape(const char* s) {
    std::string out;
    if (!s) return out;
    out.reserve(strlen(s) + 16);
    for (const char* p = s; *p; ++p) {
        switch (*p) {
            case '\\': out += "\\\\"; break;
            case '"':  out += "\\\""; break;
            case '\n': out += "\\n";  break;
            case '\r': out += "\\r";  break;
            default:   out += *p;     break;
        }
    }
    return out;
}
static std::string JNum(int n)    { return std::to_string(n); }

// ─────────────────────────────────────────────────────────────────────────────
// Settings bridge
// ─────────────────────────────────────────────────────────────────────────────
static void Bridge_SetSetting(const char* key, const char* value) {
    AeonSettings s = SettingsEngine::Load();
    bool bval = (strcmp(value,"true") == 0 || strcmp(value,"1") == 0);

    if      (!strcmp(key, "adblock"))          s.adblock_enabled   = bval;
    else if (!strcmp(key, "tor_enabled"))       s.tor_enabled       = bval;
    else if (!strcmp(key, "i2p_enabled"))       s.i2p_enabled       = bval;
    else if (!strcmp(key, "https_upgrade"))     s.https_upgrade     = bval;
    else if (!strcmp(key, "fingerprint_guard")) s.fingerprint_guard = bval;
    else if (!strcmp(key, "bypass_enabled"))    s.bypass_enabled    = bval;
    else if (!strcmp(key, "doh_enabled"))       s.doh_enabled       = bval;
    else if (!strcmp(key, "block_3p_cookies"))  s.block_3p_cookies  = bval;
    else if (!strcmp(key, "gpc"))               s.gpc_enabled       = bval;
    else if (!strcmp(key, "cookie_banners"))    s.cookie_banners    = bval;
    else if (!strcmp(key, "mixed_content"))     s.mixed_content_block = bval;
    else if (!strcmp(key, "safe_browsing"))     s.safe_browsing     = bval;
    else if (!strcmp(key, "telemetry"))         s.telemetry_enabled = bval;
    else if (!strcmp(key, "dl_ask"))            s.dl_ask            = bval;
    else if (!strcmp(key, "search_engine"))     strncpy_s(s.search_engine, value, sizeof(s.search_engine)-1);
    else if (!strcmp(key, "doh_provider"))      strncpy_s(s.doh_provider,  value, sizeof(s.doh_provider)-1);
    else if (!strcmp(key, "dl_path"))           strncpy_s(s.dl_path,       value, sizeof(s.dl_path)-1);
    else if (!strcmp(key, "min_tls"))           strncpy_s(s.min_tls,       value, sizeof(s.min_tls)-1);
    else if (!strcmp(key, "startup_page"))      strncpy_s(s.startup_page,  value, sizeof(s.startup_page)-1);
    else if (!strcmp(key, "homepage"))          strncpy_s(s.homepage,      value, sizeof(s.homepage)-1);
    else if (!strcmp(key, "dl_streams"))        s.dl_streams = atoi(value);

    SettingsEngine::Save(s);

    // Apply live changes immediately
    if (!strcmp(key, "doh_enabled") || !strcmp(key, "doh_provider")) {
        // Re-init DNS resolver with new provider preference
        DnsResolver::Initialize();
    }
    if (!strcmp(key, "bypass_enabled") || !strcmp(key, "tor_enabled")) {
        NetworkSentinel::ApplyBestStrategy();
    }

    fprintf(stdout, "[Bridge] setSetting: %s = %s\n", key, value);
}

static void Bridge_CommitSettings() {
    fprintf(stdout, "[Bridge] commitSettings — settings already saved on each change.\n");
}

static void Bridge_RevertSettings() {
    fprintf(stdout, "[Bridge] revertSettings — page reload will re-read from disk.\n");
}

static void Bridge_SetDefaultBrowser() {
    // Register Aeon as default browser via Windows Default Apps
    // Full implementation: write Aeon UserChoice keys and call SHChangeNotify
    ShellExecuteA(nullptr, "open",
        "ms-settings:defaultapps", nullptr, nullptr, SW_SHOW);
    fprintf(stdout, "[Bridge] setDefaultBrowser — opened Windows Default Apps.\n");
}

// ─────────────────────────────────────────────────────────────────────────────
// History bridge
// ─────────────────────────────────────────────────────────────────────────────
static std::string Bridge_GetHistory(int maxItems = 200) {
    auto entries = HistoryEngine::GetRecent(maxItems);
    std::string json = "[";
    for (size_t i = 0; i < entries.size(); i++) {
        if (i > 0) json += ",";
        json += "{\"title\":" + JStr(entries[i].title)
             + ",\"url\":"   + JStr(entries[i].url)
             + ",\"ts\":"    + std::to_string(entries[i].visitTime)
             + ",\"count\":" + JNum(entries[i].visitCount) + "}";
    }
    json += "]";
    fprintf(stdout, "[Bridge] getHistory (%d max, %zu returned)\n", maxItems, entries.size());
    return json;
}

static void Bridge_DeleteHistoryEntry(const char* url, long long ts) {
    HistoryEngine::DeleteEntry(url);
    fprintf(stdout, "[Bridge] deleteHistoryEntry: %s (ts=%lld)\n", url, ts);
}

static void Bridge_ClearHistory() {
    HistoryEngine::WipeAll();
    fprintf(stdout, "[Bridge] clearHistory — wiped\n");
}

// ─────────────────────────────────────────────────────────────────────────────
// Bookmarks bridge
// ─────────────────────────────────────────────────────────────────────────────
static void Bridge_AddBookmark(const char* title, const char* url, const char* folder) {
    HistoryEngine::AddBookmark(url, title, folder ? folder : "Bookmarks");
    fprintf(stdout, "[Bridge] addBookmark: %s | %s | folder=%s\n", title, url, folder);
}

static void Bridge_UpdateBookmark(int id, const char* title, const char* url, const char* folder) {
    // Update = delete old + add new (HistoryEngine has no Update API)
    HistoryEngine::DeleteBookmark((uint64_t)id);
    HistoryEngine::AddBookmark(url, title, folder ? folder : "Bookmarks");
    fprintf(stdout, "[Bridge] updateBookmark #%d: %s\n", id, title);
}

static void Bridge_DeleteBookmark(int id) {
    HistoryEngine::DeleteBookmark((uint64_t)id);
    fprintf(stdout, "[Bridge] deleteBookmark #%d\n", id);
}

static void Bridge_CreateFolder(const char* name) {
    // Folder creation is implicit in HistoryEngine — folders exist when bookmarks reference them
    fprintf(stdout, "[Bridge] createFolder: %s (implicit)\n", name);
}

// ─────────────────────────────────────────────────────────────────────────────
// Downloads bridge
// ─────────────────────────────────────────────────────────────────────────────
static void Bridge_PauseDownload(int id)   { DownloadManager::Pause(id);  fprintf(stdout, "[Bridge] pause %d\n", id); }
static void Bridge_ResumeDownload(int id)  { DownloadManager::Resume(id); fprintf(stdout, "[Bridge] resume %d\n", id); }
static void Bridge_RetryDownload(int id)   { DownloadManager::Resume(id); fprintf(stdout, "[Bridge] retry %d\n", id); }
static void Bridge_CancelDownload(int id)  { DownloadManager::Cancel(id); fprintf(stdout, "[Bridge] cancel %d\n", id); }
static void Bridge_ClearCompleted()        { DownloadManager::ClearCompleted(); fprintf(stdout, "[Bridge] clearCompleted\n"); }

static void Bridge_ShowInFolder(int id) {
    DownloadManager::RevealInExplorer(id);
    fprintf(stdout, "[Bridge] showInFolder #%d\n", id);
}

static void Bridge_OpenDownloadFolder() {
    DownloadManager::OpenDownloadFolder();
}

static void Bridge_BrowseDlPath() {
    PostMessage(g_mainHwnd, WM_AEONBRIDGE, BRIDGE_CMD_BROWSE_DL, 0);
}

static void Bridge_OpenDownload(int id) {
    DownloadManager::OpenFile(id);
    fprintf(stdout, "[Bridge] openDownload #%d\n", id);
}

// ─────────────────────────────────────────────────────────────────────────────
// Navigation
// ─────────────────────────────────────────────────────────────────────────────
static void Bridge_Navigate(const char* url) {
    if (g_navigateFn && url) {
        g_navigateFn(url);
        fprintf(stdout, "[Bridge] navigate → %s\n", url);
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// Updates
// ─────────────────────────────────────────────────────────────────────────────
static void Bridge_CheckUpdate() {
    AutoUpdater::CheckNow(); // async — result posted via WM_AEONBRIDGE when done
    fprintf(stdout, "[Bridge] checkUpdate — async\n");
}

// ─────────────────────────────────────────────────────────────────────────────
// Session / crash recover
// ─────────────────────────────────────────────────────────────────────────────
static void Bridge_RestoreTabs(const int* ids, int count) {
    for (int i = 0; i < count; i++) {
        // SessionManager::RestoreTab(ids[i]);
        fprintf(stdout, "[Bridge] restoreTab #%d\n", ids[i]);
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// Public API — called by engine integration layer (WebView2 / CEF / postMessage)
// ─────────────────────────────────────────────────────────────────────────────
namespace AeonBridge {

void Init(HWND mainHwnd, AeonBridgeNavigateFn navigateFn) {
    g_mainHwnd   = mainHwnd;
    g_navigateFn = navigateFn;
    fprintf(stdout, "[Bridge] AeonBridge initialized. MainHwnd: %p\n", (void*)mainHwnd);
}

// Dispatch a host-to-page method call (called by WM_COPYDATA handler or WebView2
// AddHostObjectToScript adapter).
// Returns JSON result string — caller must free().
std::string Dispatch(const char* method, const char* argsJson) {
    if (!method) return "null";

    fprintf(stdout, "[Bridge] dispatch: %s(%s)\n", method, argsJson ? argsJson : "");

    // Settings
    if (!strcmp(method, "getSetting")) {
        AeonSettings s = SettingsEngine::Load();
        // argsJson expected: "\"adblock\"" (JSON string)
        // stub: return null (page pre-fills from its own defaults)
        return "null";
    }
    if (!strcmp(method, "setSetting")) {
        // argsJson: ["key", "value"] — parse key and value directly
        // e.g. ["adblock","true"]
        // Simple parse: find first and last quoted strings
        const char* p = argsJson ? argsJson : "";
        return "null";
    }
    if (!strcmp(method, "commitSettings")) { Bridge_CommitSettings(); return "null"; }
    if (!strcmp(method, "revertSettings")) { Bridge_RevertSettings(); return "null"; }
    if (!strcmp(method, "setDefaultBrowser")) { Bridge_SetDefaultBrowser(); return "null"; }

    // History
    if (!strcmp(method, "getHistory"))          return Bridge_GetHistory(200);
    if (!strcmp(method, "clearHistory"))        { Bridge_ClearHistory(); return "null"; }

    // Bookmarks
    if (!strcmp(method, "getBookmarks")) {
        auto bk = HistoryEngine::GetBookmarks();
        std::string json = "[";
        for (size_t i = 0; i < bk.size(); i++) {
            if (i > 0) json += ",";
            json += "{\"id\":" + std::to_string(bk[i].id)
                 + ",\"title\":" + JStr(bk[i].title)
                 + ",\"url\":"   + JStr(bk[i].url)
                 + ",\"folder\":" + JStr(bk[i].folder)
                 + ",\"added\":" + std::to_string(bk[i].added_time) + "}";
        }
        json += "]";
        return json;
    }

    // Downloads
    if (!strcmp(method, "getDownloads")) {
        auto dl = DownloadManager::GetAll();
        std::string json = "[";
        for (size_t i = 0; i < dl.size(); i++) {
            if (i > 0) json += ",";
            json += "{\"id\":" + std::to_string(dl[i].id)
                 + ",\"url\":"      + JStr(dl[i].url)
                 + ",\"filename\":" + JStr(dl[i].filename)
                 + ",\"total\":"    + std::to_string(dl[i].totalBytes)
                 + ",\"received\":" + std::to_string(dl[i].receivedBytes)
                 + ",\"speed\":"    + std::to_string(dl[i].speed_bps)
                 + ",\"eta\":"      + JNum(dl[i].eta_sec)
                 + ",\"state\":"    + JNum((int)dl[i].state)
                 + ",\"error\":"    + JNum(dl[i].errorCode) + "}";
        }
        json += "]";
        return json;
    }
    if (!strcmp(method, "clearCompleted"))      { Bridge_ClearCompleted(); return "null"; }
    if (!strcmp(method, "openDownloadFolder"))  { Bridge_OpenDownloadFolder(); return "null"; }

    // Navigation
    if (!strcmp(method, "navigate")) {
        if (argsJson && argsJson[0] == '"') {
            // JSON string — strip quotes
            std::string url(argsJson + 1);
            if (!url.empty() && url.back() == '"') url.pop_back();
            Bridge_Navigate(url.c_str());
        } else {
            Bridge_Navigate(argsJson);
        }
        return "null";
    }

    // Updates
    if (!strcmp(method, "checkUpdate")) { Bridge_CheckUpdate(); return "null"; }

    fprintf(stdout, "[Bridge] Unknown method: %s\n", method);
    return "null";
}

// Called by engine's WebView2 adapter via AeonEngineVTable extension — sets
// a single setting by key+value without JSON overhead.
void SetSetting(const char* key, const char* value) {
    Bridge_SetSetting(key, value);
}

// Called by BrowserChrome when WM_AEONBRIDGE arrives on the message pump
void HandleWmAeonBridge(WPARAM wParam, LPARAM lParam) {
    switch ((int)wParam) {
        case BRIDGE_CMD_BROWSE_DL: {
            // Show folder-picker dialog on UI thread (COM STA requirement)
            IFileDialog* pfd = nullptr;
            if (SUCCEEDED(CoCreateInstance(CLSID_FileOpenDialog, nullptr,
                    CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&pfd)))) {
                DWORD opts;
                pfd->GetOptions(&opts);
                pfd->SetOptions(opts | FOS_PICKFOLDERS);
                pfd->SetTitle(L"Choose Download Folder");
                if (SUCCEEDED(pfd->Show(g_mainHwnd))) {
                    IShellItem* psi = nullptr;
                    if (SUCCEEDED(pfd->GetResult(&psi))) {
                        PWSTR path = nullptr;
                        psi->GetDisplayName(SIGDN_FILESYSPATH, &path);
                        if (path) {
                            char mpath[MAX_PATH];
                            WideCharToMultiByte(CP_UTF8, 0, path, -1, mpath, MAX_PATH, nullptr, nullptr);
                            Bridge_SetSetting("dl_path", mpath);
                            CoTaskMemFree(path);
                        }
                        psi->Release();
                    }
                }
                pfd->Release();
            }
            break;
        }
        default:
            break;
    }
}

// Build the JavaScript bootstrap injected into every aeon:// page.
// Injects window.aeonBridge = { ... } with all methods as stubs that call
// the C++ side via the message channel.
std::string BuildInjectionScript() {
    AeonSettings s = SettingsEngine::Load();
    const auto& env = NetworkSentinel::GetEnvironment();
    const auto& sst = NetworkSentinel::GetState();

    // Enumerate network type name
    const char* netNames[] = {
        "Open","CoffeeShop","Hotel","Corporate","School","NationalFirewall","ISP_Throttle"
    };
    const char* netName = netNames[min((int)env.type, 6)];

    char script[8192];
    _snprintf_s(script, sizeof(script), _TRUNCATE,
        "window.__aeon = {\n"
        "  settings: {\n"
        "    adblock: %s,\n"
        "    tor_enabled: %s,\n"
        "    i2p_enabled: %s,\n"
        "    doh_enabled: %s,\n"
        "    bypass_enabled: %s,\n"
        "    fingerprint_guard: %s,\n"
        "    gpc: %s,\n"
        "    cookie_banners: %s,\n"
        "    block_3p_cookies: %s,\n"
        "    safe_browsing: %s,\n"
        "    telemetry: %s,\n"
        "    search_engine: \"%s\",\n"
        "    doh_provider: \"%s\",\n"
        "    dl_path: \"%s\",\n"
        "    startup_page: \"%s\",\n"
        "    homepage: \"%s\",\n"
        "    min_tls: \"%s\",\n"
        "    dl_streams: %d,\n"
        "    dl_ask: %s\n"
        "  },\n"
        "  network: {\n"
        "    type: \"%s\",\n"
        "    internet_ok: %s,\n"
        "    captive_portal: %s,\n"
        "    corporate_proxy: %s,\n"
        "    ssl_intercepted: %s,\n"
        "    bypass_active: %s\n"
        "  }\n"
        "};\n"
        "// If WebView2 has not injected window.aeonBridge yet, provide a\n"
        "// postMessage-based fallback so pages work in dev mode too.\n"
        "if (!window.aeonBridge) {\n"
        "  window.aeonBridge = {\n"
        "    setSetting: (k,v) => window.__aeon.settings[k] = v,\n"
        "    getSetting: (k)   => window.__aeon.settings[k],\n"
        "    commitSettings: () => {},\n"
        "    revertSettings: () => window.location.reload(),\n"
        "    setDefaultBrowser: () => {},\n"
        "    getHistory: () => [],\n"
        "    clearHistory: () => {},\n"
        "    deleteHistoryEntry: () => {},\n"
        "    getBookmarks: () => [],\n"
        "    addBookmark: () => {},\n"
        "    updateBookmark: () => {},\n"
        "    deleteBookmark: () => {},\n"
        "    createFolder: () => {},\n"
        "    getDownloads: () => [],\n"
        "    pauseDownload: () => {},\n"
        "    resumeDownload: () => {},\n"
        "    retryDownload: () => {},\n"
        "    cancelDownload: () => {},\n"
        "    clearCompleted: () => {},\n"
        "    openDownload: () => {},\n"
        "    showInFolder: () => {},\n"
        "    openDownloadFolder: () => {},\n"
        "    browseDlPath: () => {},\n"
        "    navigate: (url) => { window.location.href = url; },\n"
        "    checkUpdate: () => {},\n"
        "    getCrashedTabs: () => [],\n"
        "    restoreTabs: () => {}\n"
        "  };\n"
        "}",
        JBool(s.adblock_enabled).c_str(),
        JBool(s.tor_enabled).c_str(),
        JBool(s.i2p_enabled).c_str(),
        JBool(s.doh_enabled).c_str(),
        JBool(s.bypass_enabled).c_str(),
        JBool(s.fingerprint_guard).c_str(),
        JBool(s.gpc_enabled).c_str(),
        JBool(s.cookie_banners).c_str(),
        JBool(s.block_3p_cookies).c_str(),
        JBool(s.safe_browsing).c_str(),
        JBool(s.telemetry_enabled).c_str(),
        JEscape(s.search_engine).c_str(),
        JEscape(s.doh_provider).c_str(),
        JEscape(s.dl_path).c_str(),
        JEscape(s.startup_page).c_str(),
        JEscape(s.homepage).c_str(),
        s.min_tls,
        s.dl_streams,
        JBool(s.dl_ask).c_str(),
        netName,
        JBool(env.internet_ok).c_str(),
        JBool(env.captive_portal).c_str(),
        JBool(env.has_corporate_proxy).c_str(),
        JBool(env.ssl_intercepted).c_str(),
        JBool(sst.bypass_active).c_str()
    );

    return std::string(script);
}

} // namespace AeonBridge
