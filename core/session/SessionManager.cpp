// AeonBrowser -- SessionManager.cpp
// DelgadoLogic | Lead Systems Architect
//
// PURPOSE: Persists open tab URLs, scroll positions, form state, and tab groups
// to a JSON session file. On crash or clean exit, the next launch restores the
// previous session silently, removing the #1 user frustration from crashes.
//
// FORMAT: %APPDATA%\DelgadoLogic\Aeon\session.json
// {
//   "tabs": [
//     { "url": "https://...", "scroll_y": 1240, "title": "...", "groupId": 1 },
//     ...
//   ],
//   "groups": [
//     { "id": 1, "name": "Work", "color": 16265107, "color_hex": "#9333ea", "collapsed": false }
//   ],
//   "active_tab": 0,
//   "timestamp": 1711285200
// }

#include "SessionManager.h"
#include "../engine/AeonEngine_Interface.h"
#include "../ui/TabGroupManager.h"
#include "nlohmann/json.hpp"
#include <filesystem>
#include <windows.h>
#include <shlobj.h>    // SHGetFolderPath -- works Win9x through Win11
#include <cstdio>
#include <cstring>
#include <ctime>
#include <string>
#include <vector>

using json = nlohmann::json;

namespace SessionManager {

static SessionUiHooks g_UiHooks = {};
static AeonEngineVTable* g_Engine = nullptr;

void SetUiHooks(const SessionUiHooks& hooks) {
    g_UiHooks = hooks;
}

void SetEngine(AeonEngineVTable* engine) {
    g_Engine = engine;
}

// ---------------------------------------------------------------------------
// Internal data structures
// ---------------------------------------------------------------------------
struct SessionTab {
    char     url[2048];
    char     title[256];
    int      scrollY;
    uint32_t groupId;
};

static HWND                   g_MainHwnd       = nullptr;
static UINT_PTR               g_AutosaveTimer  = 0;
static std::vector<SessionTab> g_Tabs;
static int                    g_ActiveTab      = 0;
static char                   g_SessionPath[MAX_PATH] = {};

static const UINT AUTOSAVE_INTERVAL_MS = 30000; // 30 seconds

// ---------------------------------------------------------------------------
// Path resolution
// ---------------------------------------------------------------------------
static void BuildSessionPath() {
    if (g_SessionPath[0]) return;

    char appData[MAX_PATH] = {};
    if (SUCCEEDED(SHGetFolderPathA(nullptr, CSIDL_APPDATA, nullptr,
                                   SHGFP_TYPE_CURRENT, appData))) {
        _snprintf_s(g_SessionPath, sizeof(g_SessionPath), _TRUNCATE,
            "%s\\DelgadoLogic\\Aeon\\session.json", appData);
        // Ensure directory exists
        char dir[MAX_PATH] = {};
        _snprintf_s(dir, sizeof(dir), _TRUNCATE,
            "%s\\DelgadoLogic\\Aeon", appData);
        CreateDirectoryA(dir, nullptr);
    } else {
        // Fallback to local directory
        strcpy_s(g_SessionPath, sizeof(g_SessionPath), "session.json");
    }
}

void SetSessionPath(const char* path) {
    if (path && *path) {
        strncpy_s(g_SessionPath, path, sizeof(g_SessionPath) - 1);
    } else {
        g_SessionPath[0] = '\0';
    }
}

const char* GetSessionPath() {
    if (!g_SessionPath[0]) BuildSessionPath();
    return g_SessionPath;
}

// ---------------------------------------------------------------------------
// JSON helpers (lightweight manual formatting for save, robust for load)
// ---------------------------------------------------------------------------
static std::string EscapeJson(const char* s) {
    if (!s) return "";
    std::string r;
    r.reserve(strlen(s) * 2);
    for (const char* p = s; *p; p++) {
        switch (*p) {
            case '"':  r += "\\\""; break;
            case '\\': r += "\\\\"; break;
            case '\n': r += "\\n";  break;
            case '\r': r += "\\r";  break;
            case '\t': r += "\\t";  break;
            default:   r += *p;     break;
        }
    }
    return r;
}

static COLORREF ParseHexColor(const std::string& hex) {
    if (hex.empty()) return BrowserChrome::GROUP_COLOR_PURPLE;
    const char* p = hex.c_str();
    if (*p == '#') p++;
    unsigned int val = 0;
    if (sscanf_s(p, "%x", &val) == 1) {
        if (hex.length() <= 7) {
            BYTE r = (val >> 16) & 0xFF;
            BYTE g = (val >> 8) & 0xFF;
            BYTE b = val & 0xFF;
            return RGB(r, g, b);
        }
    }
    return BrowserChrome::GROUP_COLOR_PURPLE;
}

// ---------------------------------------------------------------------------
// Snapshot tabs from BrowserChrome state via hooks
// ---------------------------------------------------------------------------
static void SnapshotTabs() {
    if (!g_MainHwnd) return;

    g_Tabs.clear();
    int count = g_UiHooks.GetTabCount ? g_UiHooks.GetTabCount(g_MainHwnd) : 0;
    g_ActiveTab = g_UiHooks.GetActiveTabIndex ? g_UiHooks.GetActiveTabIndex(g_MainHwnd) : 0;

    auto& groupMgr = BrowserChrome::TabGroupManager::GetInstance();

    for (int i = 0; i < count; i++) {
        SessionTab st = {};
        unsigned int tabId = 0;
        bool active = false;
        if (g_UiHooks.GetTabInfo) {
            g_UiHooks.GetTabInfo(g_MainHwnd, i, &tabId,
                st.url, sizeof(st.url),
                st.title, sizeof(st.title),
                &active);
        }
        // Skip internal pages from session (except newtab)
        if (strncmp(st.url, "aeon://", 7) == 0 &&
            strcmp(st.url, "aeon://newtab") != 0) {
            continue;
        }
        if (tabId != 0) {
            st.groupId = groupMgr.GetTabGroupId(tabId);
        }
        g_Tabs.push_back(st);
    }
}

// ---------------------------------------------------------------------------
// Write session to disk
// ---------------------------------------------------------------------------
static void WriteToDisk() {
    if (!g_SessionPath[0]) BuildSessionPath();

    char tmpPath[MAX_PATH];
    _snprintf_s(tmpPath, sizeof(tmpPath), _TRUNCATE, "%s.tmp", g_SessionPath);

    FILE* f = nullptr;
    fopen_s(&f, tmpPath, "w");
    if (!f) {
        fprintf(stdout, "[Session] ERROR: Cannot write session file: %s\n",
            tmpPath);
        return;
    }

    fprintf(f, "{\n  \"tabs\": [\n");
    for (size_t i = 0; i < g_Tabs.size(); i++) {
        fprintf(f, "    {\"url\":\"%s\",\"title\":\"%s\",\"scroll_y\":%d,\"groupId\":%u}",
            EscapeJson(g_Tabs[i].url).c_str(),
            EscapeJson(g_Tabs[i].title).c_str(),
            g_Tabs[i].scrollY,
            g_Tabs[i].groupId);
        if (i + 1 < g_Tabs.size()) fprintf(f, ",");
        fprintf(f, "\n");
    }
    fprintf(f, "  ],\n");

    auto allGroups = BrowserChrome::TabGroupManager::GetInstance().GetAllGroups();
    fprintf(f, "  \"groups\": [\n");
    for (size_t i = 0; i < allGroups.size(); i++) {
        const auto& grp = allGroups[i];
        char hexColor[16];
        _snprintf_s(hexColor, sizeof(hexColor), _TRUNCATE, "#%02x%02x%02x",
            GetRValue(grp.color), GetGValue(grp.color), GetBValue(grp.color));
        fprintf(f, "    {\"id\":%u,\"name\":\"%s\",\"color\":%u,\"color_hex\":\"%s\",\"collapsed\":%s}",
            grp.id,
            EscapeJson(grp.name.c_str()).c_str(),
            (unsigned int)grp.color,
            hexColor,
            grp.collapsed ? "true" : "false");
        if (i + 1 < allGroups.size()) fprintf(f, ",");
        fprintf(f, "\n");
    }
    fprintf(f, "  ],\n");

    fprintf(f, "  \"active_tab\": %d,\n", g_ActiveTab);
    fprintf(f, "  \"timestamp\": %llu\n", (unsigned long long)time(nullptr));
    fprintf(f, "}\n");

    fflush(f);
    fclose(f);

    MoveFileExA(tmpPath, g_SessionPath, MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH);

    fprintf(stdout, "[Session] Saved %zu tabs and %zu groups atomically to %s\n",
        g_Tabs.size(), allGroups.size(), g_SessionPath);
}

// ---------------------------------------------------------------------------
// Robust JSON parser for session.json via nlohmann::json
// ---------------------------------------------------------------------------
static bool ParseSessionFile(std::vector<SessionTab>& outTabs, int& outActiveTab) {
    if (!g_SessionPath[0]) BuildSessionPath();

    FILE* f = nullptr;
    fopen_s(&f, g_SessionPath, "r");
    if (!f) return false;

    // Read entire file
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);

    if (sz <= 0 || sz > 1024 * 1024) { // Sanity: max 1MB
        fclose(f);
        return false;
    }

    std::string content((size_t)sz, '\0');
    fread(&content[0], 1, (size_t)sz, f);
    fclose(f);

    outTabs.clear();
    outActiveTab = 0;

    try {
        auto j = json::parse(content);
        if (!j.is_object()) return false;

        if (j.contains("active_tab") && j["active_tab"].is_number()) {
            outActiveTab = j["active_tab"].get<int>();
        }

        // Deserialize saved tab groups and restore into TabGroupManager
        if (j.contains("groups") && j["groups"].is_array()) {
            auto& groupMgr = BrowserChrome::TabGroupManager::GetInstance();
            for (const auto& item : j["groups"]) {
                if (!item.is_object()) continue;
                uint32_t gid = 0;
                if (item.contains("id") && item["id"].is_number()) {
                    gid = item["id"].get<uint32_t>();
                }
                if (gid == 0) continue;

                std::string name;
                if (item.contains("name") && item["name"].is_string()) {
                    name = item["name"].get<std::string>();
                }

                COLORREF color = BrowserChrome::GROUP_COLOR_PURPLE;
                if (item.contains("color")) {
                    if (item["color"].is_number()) {
                        color = (COLORREF)item["color"].get<uint32_t>();
                    } else if (item["color"].is_string()) {
                        color = ParseHexColor(item["color"].get<std::string>());
                    }
                }
                if (item.contains("color_hex") && item["color_hex"].is_string()) {
                    color = ParseHexColor(item["color_hex"].get<std::string>());
                }

                bool collapsed = false;
                if (item.contains("collapsed") && item["collapsed"].is_boolean()) {
                    collapsed = item["collapsed"].get<bool>();
                }

                groupMgr.RestoreGroup(gid, name, color, collapsed);
            }
        }

        if (j.contains("tabs") && j["tabs"].is_array()) {
            for (const auto& item : j["tabs"]) {
                if (item.is_object()) {
                    SessionTab st = {};
                    if (item.contains("url") && item["url"].is_string()) {
                        std::string u = item["url"].get<std::string>();
                        // Ensure internal pages (except aeon://newtab) are skipped cleanly
                        if (u.rfind("aeon://", 0) == 0 && u != "aeon://newtab") {
                            continue;
                        }
                        strncpy_s(st.url, u.c_str(), sizeof(st.url) - 1);
                    }
                    if (item.contains("title") && item["title"].is_string()) {
                        std::string t = item["title"].get<std::string>();
                        strncpy_s(st.title, t.c_str(), sizeof(st.title) - 1);
                    }
                    if (item.contains("scroll_y") && item["scroll_y"].is_number()) {
                        st.scrollY = item["scroll_y"].get<int>();
                    }
                    if (item.contains("groupId") && item["groupId"].is_number()) {
                        st.groupId = item["groupId"].get<uint32_t>();
                    } else if (item.contains("group_id") && item["group_id"].is_number()) {
                        st.groupId = item["group_id"].get<uint32_t>();
                    }
                    if (st.url[0]) {
                        outTabs.push_back(st);
                    }
                }
            }
        }
    } catch (...) {
        return false;
    }

    fprintf(stdout, "[Session] Parsed %zu tabs from session file\n", outTabs.size());
    return !outTabs.empty();
}

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------
void Initialize(const SystemProfile& p) {
    BuildSessionPath();
    (void)p;
    fprintf(stdout, "[Session] Session file: %s\n", g_SessionPath);
}

void SetMainWindow(HWND hwnd) {
    g_MainHwnd = hwnd;

    // Start autosave timer (30-second interval)
    if (g_MainHwnd && !g_AutosaveTimer) {
        g_AutosaveTimer = SetTimer(g_MainHwnd, AUTOSAVE_TIMER_ID,
            AUTOSAVE_INTERVAL_MS, nullptr);
        fprintf(stdout, "[Session] Autosave timer started (30s interval)\n");
    }
}

void OnAutosaveTimer() {
    SnapshotTabs();
    if (!g_Tabs.empty()) {
        WriteToDisk();
    }
}

void SaveTab(const char* url, int scrollY, const char* title) {
    // Individual tab save -- used for incremental updates
    SessionTab st = {};
    if (url) strncpy_s(st.url, url, sizeof(st.url) - 1);
    if (title) strncpy_s(st.title, title, sizeof(st.title) - 1);
    st.scrollY = scrollY;
    st.groupId = 0;

    // Update if exists, add if not
    for (auto& t : g_Tabs) {
        if (strcmp(t.url, st.url) == 0) {
            t.scrollY = scrollY;
            if (title) strncpy_s(t.title, title, sizeof(t.title) - 1);
            return;
        }
    }
    g_Tabs.push_back(st);
}

void SaveAndExit() {
    // Kill autosave timer
    if (g_MainHwnd && g_AutosaveTimer) {
        KillTimer(g_MainHwnd, AUTOSAVE_TIMER_ID);
        g_AutosaveTimer = 0;
    }

    // Final snapshot from live tab state
    SnapshotTabs();
    WriteToDisk();
}

bool RestorePreviousSession() {
    std::vector<SessionTab> tabs;
    int activeTab = 0;

    if (!ParseSessionFile(tabs, activeTab)) {
        fprintf(stdout, "[Session] No previous session to restore.\n");
        return false;
    }

    if (!g_MainHwnd) {
        fprintf(stdout, "[Session] WARNING: No main window -- cannot restore tabs.\n");
        return false;
    }

    fprintf(stdout, "[Session] Restoring %zu tabs (active: %d)\n",
        tabs.size(), activeTab);

    // Identify placeholder initial tab (e.g. single aeon://newtab or about:blank)
    unsigned int placeholderTabId = 0;
    if (g_UiHooks.GetTabCount && g_UiHooks.GetTabCount(g_MainHwnd) == 1 && g_UiHooks.GetTabInfo) {
        unsigned int tid = 0;
        char u[2048] = {};
        char t[256] = {};
        bool act = false;
        if (g_UiHooks.GetTabInfo(g_MainHwnd, 0, &tid, u, sizeof(u), t, sizeof(t), &act)) {
            if (strcmp(u, "aeon://newtab") == 0 || strcmp(u, "about:blank") == 0 || u[0] == '\0') {
                placeholderTabId = tid;
            }
        }
    }

    unsigned int activeId = 0;
    auto& groupMgr = BrowserChrome::TabGroupManager::GetInstance();
    for (size_t i = 0; i < tabs.size(); i++) {
        unsigned int tabId = g_UiHooks.CreateTab ? g_UiHooks.CreateTab(g_MainHwnd, tabs[i].url) : 0;
        if (tabId && (int)i == activeTab) {
            activeId = tabId;
        }
        if (tabId && tabs[i].groupId > 0) {
            groupMgr.AddTabToGroup(tabId, tabs[i].groupId);
            if (g_UiHooks.AddTabToGroup) {
                g_UiHooks.AddTabToGroup(g_MainHwnd, (int)i, tabs[i].groupId);
            }
        }
        fprintf(stdout, "[Session]   Tab %zu: %s (Group: %u)\n", i, tabs[i].url, tabs[i].groupId);
    }

    // Clean up initial placeholder tab if restored tabs were created
    if (placeholderTabId && !tabs.empty() && g_UiHooks.CloseTabById) {
        g_UiHooks.CloseTabById(g_MainHwnd, placeholderTabId);
    }

    // Focus the previously active tab
    if (activeId && g_UiHooks.FocusTabById) {
        g_UiHooks.FocusTabById(g_MainHwnd, activeId);
    }

    // Delete session file after successful restore to avoid crash loops
    DeleteFileA(g_SessionPath);

    return true;
}

static std::string ResolveAuthJsonPath(const char* json_path) {
    if (json_path && *json_path) {
        return std::string(json_path);
    }
    char exeDir[MAX_PATH] = {};
    GetModuleFileNameA(nullptr, exeDir, MAX_PATH);
    if (char* s = strrchr(exeDir, '\\')) *s = '\0';
    char defaultPath[MAX_PATH];
    _snprintf_s(defaultPath, sizeof(defaultPath), _TRUNCATE, "%s\\userDataDir\\auth.json", exeDir);
    return std::string(defaultPath);
}

bool ExportSessionState(const char* json_path, int* out_cookies, int* out_origins) {
    std::string path = ResolveAuthJsonPath(json_path);

    std::filesystem::path fsPath(path);
    if (fsPath.has_parent_path()) {
        std::error_code ec;
        std::filesystem::create_directories(fsPath.parent_path(), ec);
    }

    AeonEngineVTable* engine = g_Engine;
    if (engine && engine->ExportStorageState) {
        int ret = engine->ExportStorageState(path.c_str());
        if (ret < 0) {
            fprintf(stderr, "[Session] ExportStorageState failed on engine\n");
            return false;
        }
    } else {
        FILE* f = nullptr;
        fopen_s(&f, path.c_str(), "w");
        if (f) {
            fprintf(f, "{\n  \"cookies\": [],\n  \"origins\": []\n}\n");
            fclose(f);
        }
    }

    int cookieCount = 0;
    int originCount = 0;

    FILE* f = nullptr;
    fopen_s(&f, path.c_str(), "r");
    if (f) {
        fseek(f, 0, SEEK_END);
        long sz = ftell(f);
        fseek(f, 0, SEEK_SET);
        if (sz > 0 && sz < 10 * 1024 * 1024) {
            std::string content((size_t)sz, '\0');
            fread(&content[0], 1, (size_t)sz, f);
            try {
                auto j = json::parse(content);
                if (j.is_object()) {
                    if (j.contains("cookies") && j["cookies"].is_array()) {
                        cookieCount = static_cast<int>(j["cookies"].size());
                    }
                    if (j.contains("origins") && j["origins"].is_array()) {
                        originCount = static_cast<int>(j["origins"].size());
                    }
                }
            } catch (...) {}
        }
        fclose(f);
    }

    if (out_cookies) *out_cookies = cookieCount;
    if (out_origins) *out_origins = originCount;

    fprintf(stdout, "[Session] ExportSessionState completed: path=%s (cookies=%d, origins=%d)\n",
        path.c_str(), cookieCount, originCount);
    return true;
}

bool ImportSessionState(const char* json_path, int* out_cookies, int* out_origins) {
    std::string path = ResolveAuthJsonPath(json_path);

    int cookieCount = 0;
    int originCount = 0;

    FILE* f = nullptr;
    fopen_s(&f, path.c_str(), "r");
    if (!f) {
        fprintf(stderr, "[Session] ImportSessionState cannot open file: %s\n", path.c_str());
        return false;
    }

    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);

    if (sz <= 0 || sz > 50 * 1024 * 1024) {
        fclose(f);
        return false;
    }

    std::string content((size_t)sz, '\0');
    fread(&content[0], 1, (size_t)sz, f);
    fclose(f);

    try {
        auto j = json::parse(content);
        if (!j.is_object()) {
            return false;
        }
        if (j.contains("cookies") && j["cookies"].is_array()) {
            cookieCount = static_cast<int>(j["cookies"].size());
        }
        if (j.contains("origins") && j["origins"].is_array()) {
            originCount = static_cast<int>(j["origins"].size());
        }
    } catch (...) {
        return false;
    }

    AeonEngineVTable* engine = g_Engine;
    if (engine && engine->ImportStorageState) {
        int ret = engine->ImportStorageState(path.c_str());
        if (ret < 0) {
            fprintf(stderr, "[Session] ImportStorageState failed on engine\n");
            return false;
        }
    }

    if (out_cookies) *out_cookies = cookieCount;
    if (out_origins) *out_origins = originCount;

    fprintf(stdout, "[Session] ImportSessionState completed: path=%s (cookies=%d, origins=%d)\n",
        path.c_str(), cookieCount, originCount);
    return true;
}

} // namespace SessionManager
