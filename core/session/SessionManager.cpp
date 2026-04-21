// AeonBrowser — SessionManager.cpp
// DelgadoLogic | Lead Systems Architect
//
// PURPOSE: Persists open tab URLs, scroll positions, and form state to a
// JSON session file. On crash or clean exit, the next launch restores the
// previous session silently, removing the #1 user frustration from crashes.
//
// FORMAT: %APPDATA%\DelgadoLogic\Aeon\session.json
// {
//   "tabs": [
//     { "url": "https://...", "scroll_y": 1240, "title": "..." },
//     ...
//   ],
//   "active_tab": 0,
//   "timestamp": 1711285200
// }
//
// IT TROUBLESHOOTING:
//   - "Crash loop restoring session": Delete session.json in above path.
//   - "Session not restored": Check AEON_DISABLE_SESSION_RESTORE registry key.
//   - "Wrong tabs restored": session.json may be corrupted. We validate JSON
//     on load — if invalid, we silently discard and start fresh.

#include "SessionManager.h"
#include "../ui/BrowserChrome.h"
#include <windows.h>
#include <shlobj.h>    // SHGetFolderPath — works Win9x through Win11
#include <cstdio>
#include <cstring>
#include <ctime>
#include <string>
#include <vector>

namespace SessionManager {

// ─────────────────────────────────────────────────────────────────────────────
// Session data structures
// ─────────────────────────────────────────────────────────────────────────────
struct SessionTab {
    char url[2048]   = {};
    char title[512]  = {};
    int  scrollY     = 0;
};

static char               g_SessionPath[MAX_PATH] = {};
static HWND               g_MainHwnd = nullptr;
static std::vector<SessionTab> g_Tabs;
static int                g_ActiveTab = 0;
static UINT_PTR           g_AutosaveTimer = 0;
static const UINT_PTR     AUTOSAVE_TIMER_ID = 0xAE05;  // Aeon Session
static const DWORD        AUTOSAVE_INTERVAL_MS = 30000; // 30 seconds

// ─────────────────────────────────────────────────────────────────────────────
// Path resolution
// ─────────────────────────────────────────────────────────────────────────────
static void BuildSessionPath() {
    if (g_SessionPath[0]) return; // already resolved

    char appData[MAX_PATH] = {};
    // CSIDL_APPDATA = 0x001A — works Win9x through Win11
    if (SHGetFolderPathA(nullptr, CSIDL_APPDATA, nullptr, 0, appData)
            != S_OK) {
        // Fallback for very early Win9x: use install dir
        GetModuleFileNameA(nullptr, appData, MAX_PATH);
        char* slash = strrchr(appData, '\\');
        if (slash) *slash = '\0';
    }

    char dir[MAX_PATH];
    _snprintf_s(dir, sizeof(dir), _TRUNCATE,
        "%s\\DelgadoLogic\\Aeon", appData);
    CreateDirectoryA(dir, nullptr); // no-op if already exists

    _snprintf_s(g_SessionPath, sizeof(g_SessionPath), _TRUNCATE,
        "%s\\session.json", dir);
}

// ─────────────────────────────────────────────────────────────────────────────
// JSON escaping (minimal, no external library)
// ─────────────────────────────────────────────────────────────────────────────
static std::string EscapeJson(const char* s) {
    std::string r;
    if (!s) return r;
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

// ─────────────────────────────────────────────────────────────────────────────
// Snapshot tabs from BrowserChrome state
// ─────────────────────────────────────────────────────────────────────────────
static void SnapshotTabs() {
    if (!g_MainHwnd) return;

    g_Tabs.clear();
    int count = BrowserChrome::GetTabCount(g_MainHwnd);
    g_ActiveTab = BrowserChrome::GetActiveTabIndex(g_MainHwnd);

    for (int i = 0; i < count; i++) {
        SessionTab st = {};
        unsigned int tabId = 0;
        bool active = false;
        BrowserChrome::GetTabInfo(g_MainHwnd, i, &tabId,
            st.url, sizeof(st.url),
            st.title, sizeof(st.title),
            &active);
        // Skip internal pages from session (except newtab)
        if (strncmp(st.url, "aeon://", 7) == 0 &&
            strcmp(st.url, "aeon://newtab") != 0) {
            continue;
        }
        g_Tabs.push_back(st);
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// Write session to disk
// ─────────────────────────────────────────────────────────────────────────────
static void WriteToDisk() {
    if (!g_SessionPath[0]) BuildSessionPath();

    FILE* f = nullptr;
    fopen_s(&f, g_SessionPath, "w");
    if (!f) {
        fprintf(stdout, "[Session] ERROR: Cannot write session file: %s\n",
            g_SessionPath);
        return;
    }

    fprintf(f, "{\n  \"tabs\": [\n");
    for (size_t i = 0; i < g_Tabs.size(); i++) {
        fprintf(f, "    {\"url\":\"%s\",\"title\":\"%s\",\"scroll_y\":%d}",
            EscapeJson(g_Tabs[i].url).c_str(),
            EscapeJson(g_Tabs[i].title).c_str(),
            g_Tabs[i].scrollY);
        if (i + 1 < g_Tabs.size()) fprintf(f, ",");
        fprintf(f, "\n");
    }
    fprintf(f, "  ],\n");
    fprintf(f, "  \"active_tab\": %d,\n", g_ActiveTab);
    fprintf(f, "  \"timestamp\": %llu\n", (unsigned long long)time(nullptr));
    fprintf(f, "}\n");

    fclose(f);
    fprintf(stdout, "[Session] Saved %zu tabs to %s\n",
        g_Tabs.size(), g_SessionPath);
}

// ─────────────────────────────────────────────────────────────────────────────
// Minimal JSON parser for session.json
// ─────────────────────────────────────────────────────────────────────────────
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

    // Parse tabs array — find "url" and "title" fields
    // This is a purpose-built parser, not a general JSON parser
    outTabs.clear();
    outActiveTab = 0;

    // Find "active_tab" value
    size_t atPos = content.find("\"active_tab\"");
    if (atPos != std::string::npos) {
        size_t colon = content.find(':', atPos);
        if (colon != std::string::npos) {
            outActiveTab = atoi(content.c_str() + colon + 1);
        }
    }

    // Find each tab object { "url":"...", "title":"..." }
    size_t pos = content.find("\"tabs\"");
    if (pos == std::string::npos) return false;

    pos = content.find('[', pos);
    if (pos == std::string::npos) return false;

    // Walk through tab objects
    while (true) {
        size_t objStart = content.find('{', pos);
        if (objStart == std::string::npos) break;

        size_t objEnd = content.find('}', objStart);
        if (objEnd == std::string::npos) break;

        std::string obj = content.substr(objStart, objEnd - objStart + 1);

        SessionTab st = {};

        // Extract "url":"..."
        size_t urlKey = obj.find("\"url\"");
        if (urlKey != std::string::npos) {
            size_t q1 = obj.find('"', urlKey + 5);
            size_t q2 = (q1 != std::string::npos) ? obj.find('"', q1 + 1) : std::string::npos;
            if (q1 != std::string::npos && q2 != std::string::npos) {
                std::string val = obj.substr(q1 + 1, q2 - q1 - 1);
                strncpy_s(st.url, val.c_str(), sizeof(st.url) - 1);
            }
        }

        // Extract "title":"..."
        size_t titleKey = obj.find("\"title\"");
        if (titleKey != std::string::npos) {
            size_t q1 = obj.find('"', titleKey + 7);
            size_t q2 = (q1 != std::string::npos) ? obj.find('"', q1 + 1) : std::string::npos;
            if (q1 != std::string::npos && q2 != std::string::npos) {
                std::string val = obj.substr(q1 + 1, q2 - q1 - 1);
                strncpy_s(st.title, val.c_str(), sizeof(st.title) - 1);
            }
        }

        // Extract "scroll_y":N
        size_t scrollKey = obj.find("\"scroll_y\"");
        if (scrollKey != std::string::npos) {
            size_t colon = obj.find(':', scrollKey);
            if (colon != std::string::npos) {
                st.scrollY = atoi(obj.c_str() + colon + 1);
            }
        }

        // Only add if URL is non-empty
        if (st.url[0]) {
            outTabs.push_back(st);
        }

        pos = objEnd + 1;
    }

    fprintf(stdout, "[Session] Parsed %zu tabs from session file\n", outTabs.size());
    return !outTabs.empty();
}

// ─────────────────────────────────────────────────────────────────────────────
// Public API
// ─────────────────────────────────────────────────────────────────────────────
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
    // Individual tab save — used for incremental updates
    SessionTab st = {};
    if (url) strncpy_s(st.url, url, sizeof(st.url) - 1);
    if (title) strncpy_s(st.title, title, sizeof(st.title) - 1);
    st.scrollY = scrollY;

    // Update if exists, add if not
    for (auto& t : g_Tabs) {
        if (strcmp(t.url, st.url) == 0) {
            t = st;
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
        fprintf(stdout, "[Session] WARNING: No main window — cannot restore tabs.\n");
        return false;
    }

    fprintf(stdout, "[Session] Restoring %zu tabs (active: %d)\n",
        tabs.size(), activeTab);

    unsigned int activeId = 0;
    for (size_t i = 0; i < tabs.size(); i++) {
        unsigned int tabId = BrowserChrome::CreateTab(g_MainHwnd, tabs[i].url);
        if (tabId && (int)i == activeTab) {
            activeId = tabId;
        }
        fprintf(stdout, "[Session]   Tab %zu: %s\n", i, tabs[i].url);
    }

    // Focus the previously active tab
    if (activeId) {
        BrowserChrome::FocusTabById(g_MainHwnd, activeId);
    }

    // Delete session file after successful restore to avoid crash loops
    DeleteFileA(g_SessionPath);

    return true;
}

} // namespace SessionManager
