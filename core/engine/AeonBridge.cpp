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
#include <vector>
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
// Password Vault bridge
// ─────────────────────────────────────────────────────────────────────────────
static std::string Bridge_GetPasswords() {
    char origins[256][128];
    int count = PasswordVault::ListOrigins(origins, 256);
    std::string json = "[";
    for (int i = 0; i < count; i++) {
        if (i > 0) json += ",";
        PasswordVault::Credential cred = {};
        bool found = PasswordVault::Find(origins[i], &cred);
        // Build display info
        std::string site(origins[i]);
        // Extract domain for display
        std::string displayUrl = site;
        if (displayUrl.find("://") != std::string::npos)
            displayUrl = displayUrl.substr(displayUrl.find("://") + 3);
        if (displayUrl.find('/') != std::string::npos)
            displayUrl = displayUrl.substr(0, displayUrl.find('/'));

        json += "{\"id\":" + std::to_string(i)
             + ",\"site\":" + JStr(displayUrl.c_str())
             + ",\"url\":" + JStr(origins[i])
             + ",\"user\":" + JStr(found ? cred.username : "")
             + ",\"icon\":\"🌐\""
             + ",\"health\":\"safe\"}";
    }
    json += "]";
    fprintf(stdout, "[Bridge] getPasswords (%d entries)\n", count);
    return json;
}

static void Bridge_AddPassword(const char* url, const char* user, const char* pass) {
    PasswordVault::Credential cred = {};
    strncpy_s(cred.url, url, sizeof(cred.url) - 1);
    strncpy_s(cred.username, user, sizeof(cred.username) - 1);
    strncpy_s(cred.password, pass, sizeof(cred.password) - 1);
    // Extract display_url from full url
    std::string d(url);
    if (d.find("://") != std::string::npos) d = d.substr(d.find("://") + 3);
    if (d.find('/') != std::string::npos) d = d.substr(0, d.find('/'));
    strncpy_s(cred.display_url, d.c_str(), sizeof(cred.display_url) - 1);
    PasswordVault::Save(cred);
    fprintf(stdout, "[Bridge] addPassword: %s / %s\n", url, user);
}

static void Bridge_UpdatePassword(int id, const char* url, const char* user, const char* pass) {
    // Delete old entry by origin, then save new
    PasswordVault::Delete(url);
    Bridge_AddPassword(url, user, pass);
    fprintf(stdout, "[Bridge] updatePassword #%d\n", id);
}

static void Bridge_DeletePassword(int id) {
    // Need to resolve id → origin. ListOrigins and index.
    char origins[256][128];
    int count = PasswordVault::ListOrigins(origins, 256);
    if (id >= 0 && id < count) {
        PasswordVault::Delete(origins[id]);
        fprintf(stdout, "[Bridge] deletePassword #%d (%s)\n", id, origins[id]);
    }
}

static void Bridge_CopyPassword(int id) {
    char origins[256][128];
    int count = PasswordVault::ListOrigins(origins, 256);
    if (id >= 0 && id < count) {
        PasswordVault::Credential cred = {};
        if (PasswordVault::Find(origins[id], &cred)) {
            // Copy to clipboard
            if (OpenClipboard(g_mainHwnd)) {
                EmptyClipboard();
                size_t len = strlen(cred.password) + 1;
                HGLOBAL hMem = GlobalAlloc(GMEM_MOVEABLE, len);
                if (hMem) {
                    memcpy(GlobalLock(hMem), cred.password, len);
                    GlobalUnlock(hMem);
                    SetClipboardData(CF_TEXT, hMem);
                }
                CloseClipboard();
            }
            // Zero the password from RAM immediately
            SecureZeroMemory(cred.password, sizeof(cred.password));
            fprintf(stdout, "[Bridge] copyPassword #%d → clipboard\n", id);
        }
    }
}

static bool Bridge_UnlockVault(const char* masterPassword) {
    // PasswordVault::Init already decrypts via DPAPI.
    // Master password validation is an additional layer.
    // For now: vault is always unlocked after Init() — master PW is a UI gate.
    fprintf(stdout, "[Bridge] unlockVault attempt\n");
    return (masterPassword && strlen(masterPassword) >= 4);
}

static void Bridge_ExportPasswords() {
    // Open save dialog and export CSV
    fprintf(stdout, "[Bridge] exportPasswords — stub (needs file dialog)\n");
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
        // argsJson expected: "\"keyname\"" — strip quotes
        std::string key = argsJson ? argsJson : "";
        if (!key.empty() && key.front() == '"') key = key.substr(1);
        if (!key.empty() && key.back() == '"')  key.pop_back();

        if      (key == "adblock")          return JBool(s.adblock_enabled);
        else if (key == "tor_enabled")      return JBool(s.tor_enabled);
        else if (key == "i2p_enabled")      return JBool(s.i2p_enabled);
        else if (key == "doh_enabled")      return JBool(s.doh_enabled);
        else if (key == "bypass_enabled")   return JBool(s.bypass_enabled);
        else if (key == "fingerprint_guard")return JBool(s.fingerprint_guard);
        else if (key == "gpc")             return JBool(s.gpc_enabled);
        else if (key == "cookie_banners")  return JBool(s.cookie_banners);
        else if (key == "block_3p_cookies") return JBool(s.block_3p_cookies);
        else if (key == "safe_browsing")    return JBool(s.safe_browsing);
        else if (key == "mixed_content")    return JBool(s.mixed_content_block);
        else if (key == "telemetry")        return JBool(s.telemetry_enabled);
        else if (key == "dl_ask")           return JBool(s.dl_ask);
        else if (key == "search_engine")    return JStr(s.search_engine);
        else if (key == "doh_provider")     return JStr(s.doh_provider);
        else if (key == "dl_path")          return JStr(s.dl_path);
        else if (key == "startup_page")     return JStr(s.startup_page);
        else if (key == "homepage")         return JStr(s.homepage);
        else if (key == "min_tls")          return JStr(s.min_tls);
        else if (key == "dl_streams")       return JNum(s.dl_streams);
        return "null";
    }
    if (!strcmp(method, "setSetting")) {
        // argsJson: ["key","value"] or ["key",true]
        // Simple parse: find key and value between quotes/commas
        std::string raw = argsJson ? argsJson : "";
        // Strip outer brackets
        if (!raw.empty() && raw.front() == '[') raw = raw.substr(1);
        if (!raw.empty() && raw.back() == ']')  raw.pop_back();
        // Split on first comma
        size_t comma = raw.find(',');
        if (comma != std::string::npos) {
            std::string kPart = raw.substr(0, comma);
            std::string vPart = raw.substr(comma + 1);
            // Strip quotes and whitespace
            auto stripQuotes = [](std::string& s) {
                while (!s.empty() && (s.front() == ' ' || s.front() == '"')) s.erase(s.begin());
                while (!s.empty() && (s.back() == ' ' || s.back() == '"'))  s.pop_back();
            };
            stripQuotes(kPart);
            stripQuotes(vPart);
            Bridge_SetSetting(kPart.c_str(), vPart.c_str());
        }
        return "null";
    }
    if (!strcmp(method, "commitSettings")) { Bridge_CommitSettings(); return "null"; }
    if (!strcmp(method, "revertSettings")) { Bridge_RevertSettings(); return "null"; }
    if (!strcmp(method, "setDefaultBrowser")) { Bridge_SetDefaultBrowser(); return "null"; }

    // History
    if (!strcmp(method, "getHistory"))          return Bridge_GetHistory(200);
    if (!strcmp(method, "clearHistory"))        { Bridge_ClearHistory(); return "null"; }
    if (!strcmp(method, "deleteHistoryEntry")) {
        // argsJson: ["url", timestamp] or "url"
        std::string raw = argsJson ? argsJson : "";
        // Strip brackets
        if (!raw.empty() && raw.front() == '[') raw = raw.substr(1);
        if (!raw.empty() && raw.back() == ']')  raw.pop_back();
        // Find URL (first quoted string)
        size_t q1 = raw.find('"');
        size_t q2 = (q1 != std::string::npos) ? raw.find('"', q1 + 1) : std::string::npos;
        if (q1 != std::string::npos && q2 != std::string::npos) {
            std::string url = raw.substr(q1 + 1, q2 - q1 - 1);
            // Parse optional timestamp after comma
            size_t comma = raw.find(',', q2);
            long long ts = 0;
            if (comma != std::string::npos) {
                ts = atoll(raw.c_str() + comma + 1);
            }
            Bridge_DeleteHistoryEntry(url.c_str(), ts);
        }
        return "null";
    }

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
    if (!strcmp(method, "addBookmark")) {
        // argsJson: ["title","url","folder"]
        std::string raw = argsJson ? argsJson : "";
        std::vector<std::string> parts;
        size_t pos = 0;
        while (pos < raw.size() && parts.size() < 3) {
            size_t q1 = raw.find('"', pos);
            if (q1 == std::string::npos) break;
            size_t q2 = raw.find('"', q1 + 1);
            if (q2 == std::string::npos) break;
            parts.push_back(raw.substr(q1 + 1, q2 - q1 - 1));
            pos = q2 + 1;
        }
        if (parts.size() >= 2)
            Bridge_AddBookmark(parts[0].c_str(), parts[1].c_str(),
                               parts.size() >= 3 ? parts[2].c_str() : "Bookmarks");
        return "null";
    }
    if (!strcmp(method, "updateBookmark")) {
        // argsJson: [id,"title","url","folder"]
        std::string raw = argsJson ? argsJson : "";
        int id = -1;
        size_t bracket = raw.find('[');
        if (bracket != std::string::npos)
            id = atoi(raw.c_str() + bracket + 1);
        std::vector<std::string> parts;
        size_t pos = 0;
        while (pos < raw.size() && parts.size() < 3) {
            size_t q1 = raw.find('"', pos);
            if (q1 == std::string::npos) break;
            size_t q2 = raw.find('"', q1 + 1);
            if (q2 == std::string::npos) break;
            parts.push_back(raw.substr(q1 + 1, q2 - q1 - 1));
            pos = q2 + 1;
        }
        if (parts.size() >= 2)
            Bridge_UpdateBookmark(id, parts[0].c_str(), parts[1].c_str(),
                                  parts.size() >= 3 ? parts[2].c_str() : "Bookmarks");
        return "null";
    }
    if (!strcmp(method, "deleteBookmark")) {
        auto parseId = [](const char* args) -> int {
            if (!args) return -1;
            std::string s(args);
            while (!s.empty() && (s.front() == '[' || s.front() == '"' || s.front() == ' ')) s.erase(s.begin());
            while (!s.empty() && (s.back() == ']' || s.back() == '"' || s.back() == ' ')) s.pop_back();
            return s.empty() ? -1 : atoi(s.c_str());
        };
        Bridge_DeleteBookmark(parseId(argsJson));
        return "null";
    }
    if (!strcmp(method, "createFolder")) {
        std::string name = argsJson ? argsJson : "";
        if (!name.empty() && name.front() == '"') name = name.substr(1);
        if (!name.empty() && name.back() == '"')  name.pop_back();
        Bridge_CreateFolder(name.c_str());
        return "null";
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

    // Download control — parse integer ID from argsJson
    auto parseId = [](const char* args) -> int {
        if (!args) return -1;
        std::string s(args);
        // Strip quotes, brackets, whitespace
        while (!s.empty() && (s.front() == '[' || s.front() == '"' || s.front() == ' ')) s.erase(s.begin());
        while (!s.empty() && (s.back() == ']' || s.back() == '"' || s.back() == ' ')) s.pop_back();
        return s.empty() ? -1 : atoi(s.c_str());
    };
    if (!strcmp(method, "pauseDownload"))   { Bridge_PauseDownload(parseId(argsJson));  return "null"; }
    if (!strcmp(method, "resumeDownload"))  { Bridge_ResumeDownload(parseId(argsJson)); return "null"; }
    if (!strcmp(method, "retryDownload"))   { Bridge_RetryDownload(parseId(argsJson));  return "null"; }
    if (!strcmp(method, "cancelDownload"))  { Bridge_CancelDownload(parseId(argsJson)); return "null"; }
    if (!strcmp(method, "openDownload"))    { Bridge_OpenDownload(parseId(argsJson));   return "null"; }
    if (!strcmp(method, "showInFolder"))    { Bridge_ShowInFolder(parseId(argsJson));   return "null"; }
    if (!strcmp(method, "browseDlPath"))    { Bridge_BrowseDlPath(); return "null"; }

    // Passwords
    if (!strcmp(method, "getPasswords"))    return Bridge_GetPasswords();
    if (!strcmp(method, "unlockVault")) {
        std::string pw = argsJson ? argsJson : "";
        if (!pw.empty() && pw.front() == '"') pw = pw.substr(1);
        if (!pw.empty() && pw.back() == '"')  pw.pop_back();
        return Bridge_UnlockVault(pw.c_str()) ? "true" : "false";
    }
    if (!strcmp(method, "addPassword")) {
        // argsJson: ["url","user","pass"]
        // Parse three quoted strings
        std::string raw = argsJson ? argsJson : "";
        std::vector<std::string> parts;
        size_t pos = 0;
        while (pos < raw.size() && parts.size() < 3) {
            size_t q1 = raw.find('"', pos);
            if (q1 == std::string::npos) break;
            size_t q2 = raw.find('"', q1 + 1);
            if (q2 == std::string::npos) break;
            parts.push_back(raw.substr(q1 + 1, q2 - q1 - 1));
            pos = q2 + 1;
        }
        if (parts.size() >= 3)
            Bridge_AddPassword(parts[0].c_str(), parts[1].c_str(), parts[2].c_str());
        return "null";
    }
    if (!strcmp(method, "updatePassword")) {
        // argsJson: [id,"url","user","pass"]
        std::string raw = argsJson ? argsJson : "";
        int id = -1;
        // Parse leading ID
        size_t bracket = raw.find('[');
        if (bracket != std::string::npos) {
            id = atoi(raw.c_str() + bracket + 1);
        }
        std::vector<std::string> parts;
        size_t pos = 0;
        while (pos < raw.size() && parts.size() < 3) {
            size_t q1 = raw.find('"', pos);
            if (q1 == std::string::npos) break;
            size_t q2 = raw.find('"', q1 + 1);
            if (q2 == std::string::npos) break;
            parts.push_back(raw.substr(q1 + 1, q2 - q1 - 1));
            pos = q2 + 1;
        }
        if (parts.size() >= 3)
            Bridge_UpdatePassword(id, parts[0].c_str(), parts[1].c_str(), parts[2].c_str());
        return "null";
    }
    if (!strcmp(method, "deletePassword"))  { Bridge_DeletePassword(parseId(argsJson)); return "null"; }
    if (!strcmp(method, "copyPassword"))    { Bridge_CopyPassword(parseId(argsJson));   return "null"; }
    if (!strcmp(method, "exportPasswords")) { Bridge_ExportPasswords(); return "null"; }

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
        "  },\n"
        "  vault_unlocked: true\n"  // DPAPI vault is always available after Init()
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
        "    restoreTabs: () => {},\n"
        "    getPasswords: () => [],\n"
        "    addPassword: () => {},\n"
        "    updatePassword: () => {},\n"
        "    deletePassword: () => {},\n"
        "    copyPassword: () => {},\n"
        "    unlockVault: (pw) => (pw && pw.length >= 4),\n"
        "    exportPasswords: () => {}\n"
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
