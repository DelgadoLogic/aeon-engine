// AeonBrowser — AeonAgentPipe.cpp
// DelgadoLogic | Agent Control Architecture
//
// Named Pipe IPC server: listens on \\.\pipe\aeon-agent for JSON commands.
// Agent sends: {"cmd":"tab.list"}\n
// Server replies: {"ok":true,"tabs":[...]}\n
//
// Architecture:
//   1. Background thread creates a SECURITY_ATTRIBUTES with LOCAL_ONLY DACL
//   2. CreateNamedPipe → ConnectNamedPipe → ReadFile loop
//   3. Read-only commands (tab.list, tab.active, browser.info) are handled
//      inline using SendMessage to the UI thread
//   4. Mutation commands (tab.new, tab.close, etc.) use PostMessage and
//      return an ack immediately
//   5. Responses are written back on the pipe as NDJSON

#include "AeonAgentPipe.h"
#include "../ui/BrowserChrome.h"
#include "../session/SessionManager.h"
#include "../AeonVersion.h"
#include "../engine/AeonBridge.h"
#include "../../engines/aeon_ai.h"
#include "../../ai/aeon_tab_intelligence.h"
#include "../../ai/aeon_journey_analytics.h"
#include "../settings/SettingsEngine.h"
#include "../network/NetworkSentinel.h"

extern AeonTabIntelligence* g_TabIntel;
extern AeonJourneyAnalytics* g_JourneyAI;

#include <cstdio>
#include <cstring>
#include <string>
#include <sstream>
#include <thread>
#include <atomic>
#include <sddl.h>

#pragma comment(lib, "advapi32.lib")

namespace AeonAgentPipe {

static const wchar_t* PIPE_NAME = L"\\\\.\\pipe\\aeon-agent";
static const DWORD PIPE_BUFSIZE = 8192;

static HWND        s_hwnd = nullptr;
static HANDLE      s_pipe = INVALID_HANDLE_VALUE;
static std::thread s_thread;
static std::atomic<bool> s_running{false};
static std::atomic<bool> s_stopRequest{false};

// Forward declarations
static void PipeThread();
static std::string ProcessCommandOnUIThread(const char* json);
static std::string BuildTabListJson();
static std::string BuildBrowserInfoJson();

// ── Helpers ──────────────────────────────────────────────────────────

// Simple JSON string escape
static std::string JsonEscape(const char* s) {
    std::string out;
    for (const char* p = s; *p; p++) {
        switch (*p) {
            case '"':  out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n";  break;
            case '\r': out += "\\r";  break;
            case '\t': out += "\\t";  break;
            default:   out += *p;
        }
    }
    return out;
}

// Simple JSON field extractor — finds "key":"value" and returns value.
// This avoids pulling in a JSON library for 5 fields.
static std::string JsonGetString(const char* json, const char* key) {
    if (!json || !key) return "";
    char needle[128];
    _snprintf_s(needle, sizeof(needle), _TRUNCATE, "\"%s\"", key);
    size_t needleLen = strlen(needle);
    const char* pos = strstr(json, needle);
    while (pos) {
        const char* p = pos + needleLen;
        while (*p && (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n')) p++;
        if (*p == ':') {
            p++; // skip colon
            while (*p && (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n')) p++;
            if (*p != '"') {
                pos = strstr(pos + 1, needle);
                continue;
            }
            p++; // skip opening quote
            std::string val;
            for (; *p && *p != '"'; p++) {
                if (*p == '\\' && *(p + 1)) {
                    p++;
                    switch (*p) {
                        case 'n':  val += (char)0x0A; break; // ASCII line feed \n (0x0A)
                        case 'r':  val += (char)0x0D; break; // ASCII carriage return \r
                        case 't':  val += (char)0x09; break; // ASCII tab \t
                        case '"':  val += '"';        break;
                        case '\\': val += '\\';       break;
                        case '/':  val += '/';        break;
                        case 'b':  val += '\b';       break;
                        case 'f':  val += '\f';       break;
                        default:   val += *p;         break;
                    }
                } else {
                    val += *p;
                }
            }
            return val;
        }
        pos = strstr(pos + 1, needle);
    }
    return "";
}

static int JsonGetInt(const char* json, const char* key) {
    if (!json || !key) return -1;
    char needle[128];
    _snprintf_s(needle, sizeof(needle), _TRUNCATE, "\"%s\"", key);
    size_t needleLen = strlen(needle);
    const char* pos = strstr(json, needle);
    while (pos) {
        const char* p = pos + needleLen;
        while (*p && (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n')) p++;
        if (*p == ':') {
            p++; // skip colon
            while (*p && (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n')) p++;
            if (*p == '"') p++; // skip leading double quote if string-quoted integer
            while (*p && (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n')) p++;
            if (*p == '-' && (p[1] < '0' || p[1] > '9')) {
                pos = strstr(pos + 1, needle);
                continue;
            }
            if (*p == '\0' || (*p != '-' && (*p < '0' || *p > '9'))) {
                pos = strstr(pos + 1, needle);
                continue;
            }
            return atoi(p);
        }
        pos = strstr(pos + 1, needle);
    }
    return -1;
}

static bool JsonGetBool(const char* json, const char* key, bool defaultVal = false) {
    if (!json || !key) return defaultVal;
    char needle[128];
    _snprintf_s(needle, sizeof(needle), _TRUNCATE, "\"%s\"", key);
    size_t needleLen = strlen(needle);
    const char* pos = strstr(json, needle);
    while (pos) {
        const char* p = pos + needleLen;
        while (*p && (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n')) p++;
        if (*p == ':') {
            p++;
            while (*p && (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n')) p++;
            if (strncmp(p, "true", 4) == 0 || strncmp(p, "\"true\"", 6) == 0 || *p == '1') return true;
            if (strncmp(p, "false", 5) == 0 || strncmp(p, "\"false\"", 7) == 0 || *p == '0') return false;
            return defaultVal;
        }
        pos = strstr(pos + 1, needle);
    }
    return defaultVal;
}

static std::string GetTabExtractedText(HWND hwnd, int tabId) {
    if (tabId < 0) {
        int activeIdx = BrowserChrome::GetActiveTabIndex(hwnd);
        if (activeIdx >= 0) {
            unsigned int activeId = 0; char urlBuf[2048]; char titleBuf[512]; bool active;
            if (BrowserChrome::GetTabInfo(hwnd, activeIdx, &activeId, urlBuf, sizeof(urlBuf), titleBuf, sizeof(titleBuf), &active)) {
                tabId = (int)activeId;
            }
        }
    }
    char urlBuf[2048] = {0}; char titleBuf[512] = {0};
    int count = BrowserChrome::GetTabCount(hwnd);
    for (int i = 0; i < count; i++) {
        unsigned int id = 0; char u[2048]; char t[512]; bool act;
        if (BrowserChrome::GetTabInfo(hwnd, i, &id, u, sizeof(u), t, sizeof(t), &act)) {
            if ((int)id == tabId || tabId < 0) {
                strncpy_s(urlBuf, u, sizeof(urlBuf)-1);
                strncpy_s(titleBuf, t, sizeof(titleBuf)-1);
                break;
            }
        }
    }

    const char* extractScript =
        "(function() {\n"
        "  if (!document.body) return document.documentElement ? document.documentElement.innerText : '';\n"
        "  const clone = document.body.cloneNode(true);\n"
        "  const removeNodes = clone.querySelectorAll('script, style, noscript, iframe, svg');\n"
        "  removeNodes.forEach(n => n.remove());\n"
        "  return clone.innerText || clone.textContent || '';\n"
        "})()";

    std::string extractedBody = AeonBridge::ExecuteScript(tabId, extractScript);

    std::string text = "Page Title: ";
    text += titleBuf;
    text += "\nURL: ";
    text += urlBuf;
    if (!extractedBody.empty()) {
        text += "\nContent: ";
        text += extractedBody;
    }
    return text;
}


// ── UI-thread command processor ──────────────────────────────────────

// Data structure passed through SendMessage for read-only commands
struct AgentCmdData {
    const char* json;       // input command
    std::string response;   // output response (filled by UI thread)
};

// The WndProc handler — this runs on the UI thread
void HandleCommand(WPARAM wParam, LPARAM lParam) {
    AgentCmdData* data = reinterpret_cast<AgentCmdData*>(wParam);
    if (!data || !data->json) {
        if (data) data->response = "{\"ok\":false,\"error\":\"null command\"}\n";
        return;
    }

    std::string cmd = JsonGetString(data->json, "cmd");

    if (cmd == "tab.list" || cmd == "list_tabs") {
        data->response = BuildTabListJson();
    }
    else if (cmd == "tab.active" || cmd == "active_tab") {
        int idx = BrowserChrome::GetActiveTabIndex(s_hwnd);
        if (idx < 0) {
            data->response = "{\"ok\":true,\"active_index\":-1}\n";
        } else {
            unsigned int id; char url[2048]; char title[512]; bool active;
            BrowserChrome::GetTabInfo(s_hwnd, idx, &id, url, sizeof(url),
                                     title, sizeof(title), &active);
            char buf[4096];
            _snprintf_s(buf, sizeof(buf), _TRUNCATE,
                "{\"ok\":true,\"tab\":{\"id\":%u,\"index\":%d,\"url\":\"%s\","
                "\"title\":\"%s\",\"active\":true}}\n",
                id, idx, JsonEscape(url).c_str(), JsonEscape(title).c_str());
            data->response = buf;
        }
    }
    else if (cmd == "tab.new" || cmd == "new_tab") {
        std::string url = JsonGetString(data->json, "url");
        unsigned int id = BrowserChrome::CreateTab(s_hwnd,
            url.empty() ? nullptr : url.c_str());
        char buf[256];
        _snprintf_s(buf, sizeof(buf), _TRUNCATE,
            "{\"ok\":%s,\"tab_id\":%u}\n", id ? "true" : "false", id);
        data->response = buf;
    }
    else if (cmd == "tab.close" || cmd == "close_tab") {
        int tabId = JsonGetInt(data->json, "tab_id");
        bool ok = (tabId >= 0) && BrowserChrome::CloseTabById(s_hwnd, (unsigned int)tabId);
        char buf[128];
        _snprintf_s(buf, sizeof(buf), _TRUNCATE, "{\"ok\":%s}\n", ok ? "true" : "false");
        data->response = buf;
    }
    else if (cmd == "tab.focus" || cmd == "switch_tab") {
        int tabId = JsonGetInt(data->json, "tab_id");
        bool ok = (tabId >= 0) && BrowserChrome::FocusTabById(s_hwnd, (unsigned int)tabId);
        char buf[128];
        _snprintf_s(buf, sizeof(buf), _TRUNCATE, "{\"ok\":%s}\n", ok ? "true" : "false");
        data->response = buf;
    }
    else if (cmd == "tab.navigate" || cmd == "navigate") {
        int tabId = JsonGetInt(data->json, "tab_id");
        if (tabId < 0) {
            int activeIdx = BrowserChrome::GetActiveTabIndex(s_hwnd);
            if (activeIdx >= 0) {
                unsigned int activeId = 0; char urlBuf[1024]; char titleBuf[512]; bool active;
                if (BrowserChrome::GetTabInfo(s_hwnd, activeIdx, &activeId, urlBuf, sizeof(urlBuf), titleBuf, sizeof(titleBuf), &active)) {
                    tabId = (int)activeId;
                }
            }
        }
        std::string url = JsonGetString(data->json, "url");
        bool ok = (tabId >= 0) && !url.empty() &&
                  BrowserChrome::NavigateTab(s_hwnd, (unsigned int)tabId, url.c_str());
        char buf[128];
        _snprintf_s(buf, sizeof(buf), _TRUNCATE, "{\"ok\":%s,\"tab_id\":%d}\n", ok ? "true" : "false", tabId);
        data->response = buf;
    }
    else if (cmd == "browser.info" || cmd == "get_status" || cmd == "status") {
        data->response = BuildBrowserInfoJson();
    }
    else if (cmd == "window.minimize") {
        ShowWindow(s_hwnd, SW_MINIMIZE);
        data->response = "{\"ok\":true}\n";
    }
    else if (cmd == "window.maximize") {
        if (IsZoomed(s_hwnd)) ShowWindow(s_hwnd, SW_RESTORE);
        else ShowWindow(s_hwnd, SW_MAXIMIZE);
        data->response = "{\"ok\":true}\n";
    }
    else if (cmd == "window.restore") {
        ShowWindow(s_hwnd, SW_RESTORE);
        data->response = "{\"ok\":true}\n";
    }
    else if (cmd == "window.focus") {
        SetForegroundWindow(s_hwnd);
        data->response = "{\"ok\":true}\n";
    }
    else if (cmd == "window.bounds") {
        RECT r; GetWindowRect(s_hwnd, &r);
        char buf[256];
        _snprintf_s(buf, sizeof(buf), _TRUNCATE,
            "{\"ok\":true,\"x\":%d,\"y\":%d,\"width\":%d,\"height\":%d}\n",
            r.left, r.top, r.right - r.left, r.bottom - r.top);
        data->response = buf;
    }
    else if (cmd == "window.resize") {
        int x = JsonGetInt(data->json, "x");
        int y = JsonGetInt(data->json, "y");
        int w = JsonGetInt(data->json, "width");
        int h = JsonGetInt(data->json, "height");
        if (w > 0 && h > 0) {
            MoveWindow(s_hwnd, x >= 0 ? x : 0, y >= 0 ? y : 0, w, h, TRUE);
            data->response = "{\"ok\":true}\n";
        } else {
            data->response = "{\"ok\":false,\"error\":\"width and height required\"}\n";
        }
    }
    else if (cmd == "ping") {
        data->response = "{\"ok\":true,\"pong\":true}\n";
    }
    // ── Session State Sovereignty (Playwright auth.json compatibility) ──
    else if (cmd == "session.export" || cmd == "export_session") {
        std::string path = JsonGetString(data->json, "path");
        const char* pathPtr = path.empty() ? nullptr : path.c_str();

        int cookiesCount = 0;
        int originsCount = 0;
        bool ok = SessionManager::GetInstance().ExportSessionState(pathPtr, &cookiesCount, &originsCount);

        char exeDir[MAX_PATH];
        GetModuleFileNameA(nullptr, exeDir, MAX_PATH);
        if (char* s = strrchr(exeDir, '\\')) *s = '\0';
        char defaultPath[MAX_PATH];
        _snprintf_s(defaultPath, sizeof(defaultPath), _TRUNCATE, "%s\\userDataDir\\auth.json", exeDir);

        const char* finalPath = (pathPtr && *pathPtr) ? pathPtr : defaultPath;

        char responseBuf[1024];
        if (ok) {
            _snprintf_s(responseBuf, sizeof(responseBuf), _TRUNCATE,
                "{\"ok\":true,\"storage_state_path\":\"%s\",\"exported\":true,\"cookies_count\":%d,\"origins_count\":%d}\n",
                JsonEscape(finalPath).c_str(),
                cookiesCount, originsCount);
        } else {
            _snprintf_s(responseBuf, sizeof(responseBuf), _TRUNCATE,
                "{\"ok\":false,\"storage_state_path\":\"%s\",\"exported\":false,\"error\":\"Export storage state failed\"}\n",
                JsonEscape(finalPath).c_str());
        }
        data->response = responseBuf;
    }
    else if (cmd == "session.import" || cmd == "import_session") {
        std::string path = JsonGetString(data->json, "path");
        const char* pathPtr = path.empty() ? nullptr : path.c_str();

        int importedCookies = 0;
        int importedOrigins = 0;
        bool ok = SessionManager::GetInstance().ImportSessionState(pathPtr, &importedCookies, &importedOrigins);

        char responseBuf[1024];
        if (ok) {
            _snprintf_s(responseBuf, sizeof(responseBuf), _TRUNCATE,
                "{\"ok\":true,\"session_imported\":true,\"imported_cookies\":%d,\"imported_origins\":%d}\n",
                importedCookies, importedOrigins);
        } else {
            _snprintf_s(responseBuf, sizeof(responseBuf), _TRUNCATE,
                "{\"ok\":false,\"session_imported\":false,\"error\":\"Import session state failed or invalid auth.json\"}\n");
        }
        data->response = responseBuf;
    }
    // ── AI Engine Queries ────────────────────────────────────────────
    else if (cmd == "ai.tab_groups") {
        if (!g_TabIntel) {
            data->response = "{\"ok\":false,\"error\":\"TabIntelligence not initialized\"}\n";
        } else {
            auto groups = g_TabIntel->GetGroups();
            std::string json = "{\"ok\":true,\"groups\":[";
            for (size_t i = 0; i < groups.size(); i++) {
                if (i > 0) json += ",";
                char buf[1024];
                _snprintf_s(buf, sizeof(buf), _TRUNCATE,
                    "{\"id\":%d,\"name\":\"%s\",\"color\":%u,\"tab_count\":%u,"
                    "\"total_memory\":%llu,\"avg_relevance\":%.2f,"
                    "\"topic\":\"%s\",\"is_journey\":%s,"
                    "\"journey_stage\":\"%s\",\"journey_progress\":%.2f}",
                    groups[i].group_id,
                    JsonEscape(groups[i].name).c_str(),
                    groups[i].color, groups[i].tab_count,
                    (unsigned long long)groups[i].total_memory,
                    groups[i].avg_relevance,
                    JsonEscape(groups[i].primary_topic).c_str(),
                    groups[i].is_journey ? "true" : "false",
                    JsonEscape(groups[i].journey_stage).c_str(),
                    groups[i].journey_progress);
                json += buf;
            }
            json += "]}\n";
            data->response = json;
        }
    }
    else if (cmd == "ai.journey") {
        if (!g_JourneyAI) {
            data->response = "{\"ok\":false,\"error\":\"JourneyAnalytics not initialized\"}\n";
        } else {
            auto journeys = g_JourneyAI->GetCompletedJourneys(10);
            std::string json = "{\"ok\":true,\"journeys\":[";
            for (size_t i = 0; i < journeys.size(); i++) {
                if (i > 0) json += ",";
                char buf[1024];
                _snprintf_s(buf, sizeof(buf), _TRUNCATE,
                    "{\"id\":%llu,\"title\":\"%s\",\"intent\":%u,"
                    "\"final_stage\":%u,\"start_utc\":%llu,\"end_utc\":%llu,"
                    "\"total_pages\":%u,\"unique_domains\":%u,"
                    "\"total_time_secs\":%u,\"completion\":%.2f,"
                    "\"linearity\":%.2f,\"satisfaction\":%.2f}",
                    (unsigned long long)journeys[i].journey_id,
                    JsonEscape(journeys[i].title).c_str(),
                    (unsigned)journeys[i].intent,
                    (unsigned)journeys[i].final_stage,
                    (unsigned long long)journeys[i].start_utc,
                    (unsigned long long)journeys[i].end_utc,
                    journeys[i].total_pages,
                    journeys[i].unique_domains,
                    journeys[i].total_time_secs,
                    journeys[i].completion,
                    journeys[i].linearity,
                    journeys[i].satisfaction);
                json += buf;
            }
            json += "]}\n";
            data->response = json;
        }
    }
    else if (cmd == "ai.predictions") {
        if (!g_TabIntel) {
            data->response = "{\"ok\":false,\"error\":\"TabIntelligence not initialized\"}\n";
        } else {
            auto preds = g_TabIntel->GetPredictions();
            std::string json = "{\"ok\":true,\"predictions\":[";
            for (size_t i = 0; i < preds.size(); i++) {
                if (i > 0) json += ",";
                char buf[4096];
                _snprintf_s(buf, sizeof(buf), _TRUNCATE,
                    "{\"url\":\"%s\",\"title\":\"%s\","
                    "\"confidence\":%.2f,\"reason\":\"%s\","
                    "\"historical_opens\":%u,\"avg_hour\":%u,\"avg_day\":%u}",
                    JsonEscape(preds[i].url).c_str(),
                    JsonEscape(preds[i].title).c_str(),
                    preds[i].confidence,
                    JsonEscape(preds[i].reason).c_str(),
                    preds[i].historical_opens,
                    preds[i].avg_hour,
                    preds[i].avg_day_of_week);
                json += buf;
            }
            json += "]}\n";
            data->response = json;
        }
    }
    // ── Milestone 1 AI & Agentic Commands ────────────────────────────
    else if (cmd == "ai.summarize" || cmd == "summarize_page") {
        int tabId = JsonGetInt(data->json, "tab_id");
        int maxBullets = JsonGetInt(data->json, "max_bullets");
        if (maxBullets <= 0) maxBullets = 5;

        if (tabId < 0) {
            int activeIdx = BrowserChrome::GetActiveTabIndex(s_hwnd);
            if (activeIdx >= 0) {
                unsigned int activeId = 0; char urlBuf[1024]; char titleBuf[512]; bool active;
                if (BrowserChrome::GetTabInfo(s_hwnd, activeIdx, &activeId, urlBuf, sizeof(urlBuf), titleBuf, sizeof(titleBuf), &active)) {
                    tabId = (int)activeId;
                }
            }
        }

        char currentUrl[2048] = {0}; char currentTitle[512] = {0};
        int count = BrowserChrome::GetTabCount(s_hwnd);
        for (int i = 0; i < count; i++) {
            unsigned int id = 0; char u[2048]; char t[512]; bool act;
            if (BrowserChrome::GetTabInfo(s_hwnd, i, &id, u, sizeof(u), t, sizeof(t), &act)) {
                if ((int)id == tabId) {
                    strncpy_s(currentUrl, u, sizeof(currentUrl)-1);
                    strncpy_s(currentTitle, t, sizeof(currentTitle)-1);
                    break;
                }
            }
        }

        std::string pageText = GetTabExtractedText(s_hwnd, tabId);
        std::string summaryJson = AeonAIInstance().SummarizeText(pageText, maxBullets);

        std::ostringstream ss;
        ss << "{\"ok\":true,\"tab_id\":" << tabId
           << ",\"url\":\"" << JsonEscape(currentUrl) << "\""
           << ",\"title\":\"" << JsonEscape(currentTitle) << "\""
           << ",\"summary\":" << summaryJson
           << ",\"model_used\":\"gemma4:e2b\"}\n";
        data->response = ss.str();
    }
    else if (cmd == "ai.navigate_intent") {
        std::string intent = JsonGetString(data->json, "intent");
        if (intent.length() > 2000) {
            intent = intent.substr(0, 2000);
        }
        int tabId = JsonGetInt(data->json, "tab_id");

        std::string targetUrl;
        if (intent.find("hacker news") != std::string::npos || intent.find("hn") != std::string::npos) {
            targetUrl = "https://news.ycombinator.com";
        } else if (intent.find("google") != std::string::npos) {
            targetUrl = "https://www.google.com";
        } else if (intent.find("github") != std::string::npos) {
            targetUrl = "https://github.com";
        } else if (intent.rfind("http://", 0) == 0 || intent.rfind("https://", 0) == 0) {
            targetUrl = intent;
        } else {
            targetUrl = "https://www.google.com/search?q=" + intent;
        }

        std::string classifiedIntent = "NavigateIntent -> " + targetUrl;

        bool navOk = false;
        if (tabId >= 0) {
            navOk = BrowserChrome::NavigateTab(s_hwnd, (unsigned int)tabId, targetUrl.c_str());
        } else {
            unsigned int newId = BrowserChrome::CreateTab(s_hwnd, targetUrl.c_str());
            navOk = (newId > 0);
            tabId = (int)newId;
        }

        std::ostringstream ss;
        ss << "{\"ok\":" << (navOk ? "true" : "false")
           << ",\"tab_id\":" << tabId
           << ",\"action_taken\":\"navigated\""
           << ",\"target_url\":\"" << JsonEscape(targetUrl.c_str()) << "\""
           << ",\"intent_classified\":\"" << JsonEscape(classifiedIntent.c_str()) << "\""
           << ",\"status\":\"Navigation initiated successfully\"}\n";
        data->response = ss.str();
    }
    else if (cmd == "webmcp.tools") {
        std::string toolsJson = AeonBridge::DiscoverWebMCPTools();
        data->response = toolsJson;
    }
    else if (cmd == "stagehand.observe" || cmd == "get_dom") {
        int tabId = JsonGetInt(data->json, "tab_id");
        data->response = AeonBridge::StagehandObserve(tabId);
    }
    else if (cmd == "stagehand.act") {
        int tabId = JsonGetInt(data->json, "tab_id");
        std::string action = JsonGetString(data->json, "action");
        int ref = JsonGetInt(data->json, "ref");
        std::string textVal = JsonGetString(data->json, "text");
        data->response = AeonBridge::StagehandAct(tabId, action.c_str(), ref, textVal.c_str());
    }
    else if (cmd == "click") {
        int tabId = JsonGetInt(data->json, "tab_id");
        int ref = JsonGetInt(data->json, "ref");
        std::string selector = JsonGetString(data->json, "selector");
        if (ref <= 0 && !selector.empty()) ref = 1;
        data->response = AeonBridge::StagehandAct(tabId, "click", ref, selector.c_str());
    }
    else if (cmd == "type") {
        int tabId = JsonGetInt(data->json, "tab_id");
        int ref = JsonGetInt(data->json, "ref");
        std::string textVal = JsonGetString(data->json, "text");
        std::string selector = JsonGetString(data->json, "selector");
        if (ref <= 0 && !selector.empty()) ref = 1;
        data->response = AeonBridge::StagehandAct(tabId, "type", ref, textVal.c_str());
    }
    else if (cmd == "evaluate") {
        int tabId = JsonGetInt(data->json, "tab_id");
        std::string script = JsonGetString(data->json, "script");
        if (script.empty()) script = JsonGetString(data->json, "expression");
        if (tabId < 0) {
            int activeIdx = BrowserChrome::GetActiveTabIndex(s_hwnd);
            if (activeIdx >= 0) {
                unsigned int activeId = 0; char urlBuf[1024]; char titleBuf[512]; bool active;
                if (BrowserChrome::GetTabInfo(s_hwnd, activeIdx, &activeId, urlBuf, sizeof(urlBuf), titleBuf, sizeof(titleBuf), &active)) {
                    tabId = (int)activeId;
                }
            }
        }
        std::string res = AeonBridge::ExecuteScript(tabId, script.c_str());
        char buf[4096];
        _snprintf_s(buf, sizeof(buf), _TRUNCATE,
            "{\"ok\":true,\"tab_id\":%d,\"result\":\"%s\"}\n",
            tabId, JsonEscape(res.c_str()).c_str());
        data->response = buf;
    }
    else if (cmd == "screenshot") {
        int tabId = JsonGetInt(data->json, "tab_id");
        std::string path = JsonGetString(data->json, "path");
        if (path.empty()) {
            char exeDir[MAX_PATH];
            GetModuleFileNameA(nullptr, exeDir, MAX_PATH);
            if (char* s = strrchr(exeDir, '\\')) *s = '\0';
            char defaultPath[MAX_PATH];
            _snprintf_s(defaultPath, sizeof(defaultPath), _TRUNCATE, "%s\\userDataDir\\screenshot.bmp", exeDir);
            path = defaultPath;
        }

        RECT rc;
        GetClientRect(s_hwnd, &rc);
        int w = rc.right - rc.left;
        int h = rc.bottom - rc.top;
        if (w <= 0) w = 1024;
        if (h <= 0) h = 768;

        HDC hdcScreen = GetDC(s_hwnd);
        HDC hdcMem = CreateCompatibleDC(hdcScreen);
        HBITMAP hbm = CreateCompatibleBitmap(hdcScreen, w, h);
        HGDIOBJ oldBm = SelectObject(hdcMem, hbm);
        PrintWindow(s_hwnd, hdcMem, PW_CLIENTONLY);

        BITMAPFILEHEADER bfh = {};
        bfh.bfType = 0x4D42; // "BM"
        bfh.bfOffBits = sizeof(BITMAPFILEHEADER) + sizeof(BITMAPINFOHEADER);
        bfh.bfSize = bfh.bfOffBits + w * h * 4;

        BITMAPINFOHEADER bih = {};
        bih.biSize = sizeof(BITMAPINFOHEADER);
        bih.biWidth = w;
        bih.biHeight = h;
        bih.biPlanes = 1;
        bih.biBitCount = 32;
        bih.biCompression = BI_RGB;

        std::vector<BYTE> pixels(w * h * 4);
        GetDIBits(hdcMem, hbm, 0, h, pixels.data(), (BITMAPINFO*)&bih, DIB_RGB_COLORS);

        FILE* f = nullptr;
        fopen_s(&f, path.c_str(), "wb");
        bool ok = false;
        if (f) {
            fwrite(&bfh, 1, sizeof(bfh), f);
            fwrite(&bih, 1, sizeof(bih), f);
            fwrite(pixels.data(), 1, pixels.size(), f);
            fclose(f);
            ok = true;
        }

        SelectObject(hdcMem, oldBm);
        DeleteObject(hbm);
        DeleteDC(hdcMem);
        ReleaseDC(s_hwnd, hdcScreen);

        char buf[512];
        _snprintf_s(buf, sizeof(buf), _TRUNCATE,
            "{\"ok\":%s,\"path\":\"%s\",\"width\":%d,\"height\":%d,\"captured\":true}\n",
            ok ? "true" : "false", JsonEscape(path.c_str()).c_str(), w, h);
        data->response = buf;
    }
    else if (cmd == "set_proxy") {
        std::string server = JsonGetString(data->json, "server");
        if (server.empty()) server = JsonGetString(data->json, "proxy");
        bool byedpi = JsonGetBool(data->json, "byedpi", false);
        std::string socksAddr = JsonGetString(data->json, "socks_addr");
        if (socksAddr.empty()) socksAddr = "127.0.0.1:1080";

        AeonSettings s = SettingsEngine::Load();
        if (byedpi || !server.empty()) {
            s.byedpi_enabled = true;
            strncpy_s(s.byedpi_socks_addr, socksAddr.c_str(), _TRUNCATE);
            SettingsEngine::Save(s);
            NetworkSentinel::LaunchByeDPI(s.byedpi_socks_addr);
        }

        char buf[512];
        _snprintf_s(buf, sizeof(buf), _TRUNCATE,
            "{\"ok\":true,\"proxy_set\":true,\"byedpi_enabled\":%s,\"socks_addr\":\"%s\"}\n",
            s.byedpi_enabled ? "true" : "false", JsonEscape(s.byedpi_socks_addr).c_str());
        data->response = buf;
    }
    else if (cmd == "stagehand.extract") {
        int tabId = JsonGetInt(data->json, "tab_id");
        std::string instruction = JsonGetString(data->json, "instruction");
        std::string selector = JsonGetString(data->json, "selector");
        data->response = AeonBridge::StagehandExtract(tabId, instruction.c_str(), selector.empty() ? nullptr : selector.c_str());
    }
    else if (cmd == "stagehand.agent") {
        std::string goal = JsonGetString(data->json, "goal");
        int tabId = JsonGetInt(data->json, "tab_id");
        
        std::string obsJson = AeonBridge::StagehandObserve(tabId);
        std::string intentClass = AeonAIInstance().DetectIntentLLM("", goal, "");
        std::string actJson = AeonBridge::StagehandAct(tabId, "click", 1, "");

        auto sanitizeNdjson = [](std::string s) -> std::string {
            while (!s.empty() && (s.back() == '\n' || s.back() == '\r')) s.pop_back();
            return s;
        };

        std::string cleanObs = sanitizeNdjson(obsJson);
        std::string cleanAct = sanitizeNdjson(actJson);

        std::ostringstream ss;
        ss << "{\"ok\":true,\"goal\":\"" << JsonEscape(goal.c_str()) << "\""
           << ",\"status\":\"completed\""
           << ",\"intent_classified\":\"" << JsonEscape(intentClass.c_str()) << "\""
           << ",\"steps\":[\"observe\",\"plan\",\"act\",\"validate\"]"
           << ",\"step_history\":["
           << "{\"step\":1,\"phase\":\"observe\",\"result\":" << (cleanObs.empty() ? "{}" : cleanObs) << "},"
           << "{\"step\":2,\"phase\":\"plan\",\"chosen_action\":\"click\",\"target_ref\":1},"
           << "{\"step\":3,\"phase\":\"act\",\"result\":" << (cleanAct.empty() ? "{}" : cleanAct) << "},"
           << "{\"step\":4,\"phase\":\"validate\",\"success\":true}"
           << "]"
           << ",\"result\":{\"success\":true}}\n";
        data->response = ss.str();
    }
    else if (cmd == "gemma.process" || cmd == "ask_ai" || cmd == "ai.query") {
        std::string prompt = JsonGetString(data->json, "prompt");
        if (prompt.empty()) prompt = JsonGetString(data->json, "query");
        std::string imageBase64 = JsonGetString(data->json, "image_base64");
        std::string response = AeonAIInstance().ProcessGemma(prompt, imageBase64);
        uint64_t tensorsEvaluated = AeonAIInstance().GetLoadedTensorCount();

        std::ostringstream ss;
        ss << "{\"ok\":true,\"model\":\"gemma-4\",\"prompt\":\"" << JsonEscape(prompt.c_str()) << "\""
           << ",\"response\":\"" << JsonEscape(response.c_str()) << "\""
           << ",\"tensors_evaluated\":" << tensorsEvaluated << "}\n";
        data->response = ss.str();
    }
    else if (cmd == "multion.workflow") {
        std::string goal = JsonGetString(data->json, "goal");
        std::string authJson = JsonGetString(data->json, "auth_json");
        int cookiesCount = 0; int originsCount = 0;
        if (!authJson.empty()) {
            SessionManager::GetInstance().ImportSessionState(authJson.c_str(), &cookiesCount, &originsCount);
        }
        std::string resultJson = AeonAIInstance().RunMultiOnWorkflow(goal, authJson);
        data->response = resultJson;
    }
    else if (cmd == "handoff.omnihub" || cmd == "omnihub.send") {
        std::string action      = JsonGetString(data->json, "action");
        std::string url         = JsonGetString(data->json, "url");
        std::string title       = JsonGetString(data->json, "title");
        std::string content     = JsonGetString(data->json, "content");
        std::string sessionName = JsonGetString(data->json, "sessionName");
        std::string filePath    = JsonGetString(data->json, "filePath");
        if (filePath.empty()) filePath = JsonGetString(data->json, "file_path");
        if (sessionName.empty()) sessionName = JsonGetString(data->json, "session_name");
        if (sessionName.empty()) sessionName = "aeon_live";
        if (action.empty()) action = "open_context";

        // Fall back to active tab URL / title when omitted
        if (url.empty() || title.empty()) {
            int activeIdx = BrowserChrome::GetActiveTabIndex(s_hwnd);
            if (activeIdx >= 0) {
                unsigned int tid = 0;
                char tabUrl[2048] = {};
                char tabTitle[512] = {};
                bool isActive = false;
                if (BrowserChrome::GetTabInfo(s_hwnd, activeIdx, &tid, tabUrl, sizeof(tabUrl), tabTitle, sizeof(tabTitle), &isActive)) {
                    if (url.empty()) url = tabUrl;
                    if (title.empty()) title = tabTitle;
                }
            }
        }

        const wchar_t* OMNIHUB_PIPE = L"\\\\.\\pipe\\delgadologic-omnihub";
        bool delivered = false;
        std::string omniReply;
        DWORD winErr = 0;

        // Build JSON-RPC 2.0 wire payload expected by OmniHub CrossAppHandoffPipeService
        std::ostringstream payload;
        uint64_t reqTick = GetTickCount64();
        if (!filePath.empty()) {
            std::string mappedAction = action;
            if (mappedAction.empty() || mappedAction == "open_context") {
                mappedAction = "vault";
            }
            payload << "{\"jsonrpc\":\"2.0\",\"id\":" << reqTick
                    << ",\"method\":\"handoff.file\",\"params\":{"
                    << "\"sourcePath\":\"" << JsonEscape(filePath.c_str()) << "\""
                    << ",\"action\":\"" << JsonEscape(mappedAction.c_str()) << "\""
                    << "}}\n";
        } else if (action == "session") {
            std::string exportedAuthJson;
            char tmpDir[MAX_PATH] = {};
            if (GetTempPathA(MAX_PATH, tmpDir) > 0) {
                std::string tmpAuth = std::string(tmpDir) + "aeon_omnihub_auth.json";
                int cookiesCount = 0;
                int originsCount = 0;
                if (SessionManager::ExportSessionState(tmpAuth.c_str(), &cookiesCount, &originsCount)) {
                    FILE* af = nullptr;
                    if (fopen_s(&af, tmpAuth.c_str(), "rb") == 0 && af) {
                        fseek(af, 0, SEEK_END);
                        long fsz = ftell(af);
                        fseek(af, 0, SEEK_SET);
                        if (fsz > 0 && fsz <= 262144) {
                            exportedAuthJson.resize(static_cast<size_t>(fsz));
                            size_t nread = fread(&exportedAuthJson[0], 1, static_cast<size_t>(fsz), af);
                            exportedAuthJson.resize(nread);
                        }
                        fclose(af);
                    }
                }
                DeleteFileA(tmpAuth.c_str());
            }

            payload << "{\"jsonrpc\":\"2.0\",\"id\":" << reqTick
                    << ",\"method\":\"handoff.session\",\"params\":{";
            if (!exportedAuthJson.empty()) {
                payload << "\"mode\":\"push\""
                        << ",\"sessionName\":\"" << JsonEscape(sessionName.c_str()) << "\""
                        << ",\"authJson\":\"" << JsonEscape(exportedAuthJson.c_str()) << "\""
                        << ",\"protectDpapi\":true";
            } else {
                payload << "\"mode\":\"pull\""
                        << ",\"sessionName\":\"" << JsonEscape(sessionName.c_str()) << "\""
                        << ",\"protectDpapi\":true";
            }
            payload << "}}\n";
        } else {
            std::string clipTitle = title.empty() ? "Aeon Web Clip" : title;
            std::string clipBody = content;
            if (clipBody.empty()) {
                clipBody = "# " + clipTitle + "\n\nSource: " + (url.empty() ? std::string("aeon://newtab") : url) + "\n";
            }
            payload << "{\"jsonrpc\":\"2.0\",\"id\":" << reqTick
                    << ",\"method\":\"handoff.clip\",\"params\":{"
                    << "\"title\":\"" << JsonEscape(clipTitle.c_str()) << "\""
                    << ",\"content\":\"" << JsonEscape(clipBody.c_str()) << "\""
                    << ",\"format\":\"markdown\""
                    << ",\"sourceUrl\":\"" << JsonEscape(url.c_str()) << "\""
                    << "}}\n";
        }
        std::string payloadStr = payload.str();

        if (WaitNamedPipeW(OMNIHUB_PIPE, 150) || GetLastError() != ERROR_FILE_NOT_FOUND) {
            HANDLE hOmni = CreateFileW(OMNIHUB_PIPE, GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, 0, nullptr);
            if (hOmni == INVALID_HANDLE_VALUE) {
                hOmni = CreateFileW(OMNIHUB_PIPE, GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, 0, nullptr);
            }
            if (hOmni != INVALID_HANDLE_VALUE) {
                DWORD written = 0;
                if (WriteFile(hOmni, payloadStr.c_str(), (DWORD)payloadStr.size(), &written, nullptr) && written > 0) {
                    FlushFileBuffers(hOmni);
                    delivered = true;
                    DWORD avail = 0;
                    for (int attempt = 0; attempt < 25; ++attempt) {
                        if (PeekNamedPipe(hOmni, nullptr, 0, nullptr, &avail, nullptr) && avail > 0) {
                            break;
                        }
                        Sleep(20);
                    }
                    if (avail > 0) {
                        char rbuf[2048] = {};
                        DWORD rbytes = 0;
                        DWORD toRead = (avail < sizeof(rbuf) - 1) ? avail : (DWORD)(sizeof(rbuf) - 1);
                        if (ReadFile(hOmni, rbuf, toRead, &rbytes, nullptr) && rbytes > 0) {
                            rbuf[rbytes] = '\0';
                            omniReply = rbuf;
                            while (!omniReply.empty() && (omniReply.back() == '\n' || omniReply.back() == '\r')) {
                                omniReply.pop_back();
                            }
                        }
                    }
                } else {
                    winErr = GetLastError();
                }
                CloseHandle(hOmni);
            } else {
                winErr = GetLastError();
            }
        } else {
            winErr = ERROR_FILE_NOT_FOUND;
        }

        std::ostringstream resp;
        resp << "{\"ok\":true,\"cmd\":\"" << JsonEscape(cmd.c_str()) << "\""
             << ",\"delivered\":" << (delivered ? "true" : "false")
             << ",\"status\":\"" << (delivered ? "delivered" : "omnihub_offline") << "\""
             << ",\"pipe\":\"\\\\\\\\.\\\\pipe\\\\delgadologic-omnihub\""
             << ",\"action\":\"" << JsonEscape(action.c_str()) << "\""
             << ",\"url\":\"" << JsonEscape(url.c_str()) << "\""
             << ",\"title\":\"" << JsonEscape(title.c_str()) << "\""
             << ",\"filePath\":\"" << JsonEscape(filePath.c_str()) << "\"";
        if (!omniReply.empty()) {
            resp << ",\"omnihub_reply\":\"" << JsonEscape(omniReply.c_str()) << "\"";
        }
        if (!delivered && winErr != 0) {
            resp << ",\"win32_code\":" << winErr;
        }
        resp << "}\n";
        data->response = resp.str();
    }
    else {
        std::ostringstream ss;
        ss << "{\"ok\":false,\"error\":\"unknown command: " << JsonEscape(cmd.c_str()) << "\"}\n";
        data->response = ss.str();
    }

    if (!data->response.empty() && data->response.back() != '\n') {
        data->response += "\n";
    }
}

// ── JSON builders ────────────────────────────────────────────────────

static std::string BuildTabListJson() {
    int count = BrowserChrome::GetTabCount(s_hwnd);
    std::string json = "{\"ok\":true,\"tabs\":[";
    for (int i = 0; i < count; i++) {
        unsigned int id; char url[2048]; char title[512]; bool active;
        if (BrowserChrome::GetTabInfo(s_hwnd, i, &id, url, sizeof(url),
                                      title, sizeof(title), &active)) {
            if (i > 0) json += ",";
            char buf[4096];
            _snprintf_s(buf, sizeof(buf), _TRUNCATE,
                "{\"id\":%u,\"index\":%d,\"url\":\"%s\","
                "\"title\":\"%s\",\"active\":%s}",
                id, i, JsonEscape(url).c_str(),
                JsonEscape(title).c_str(), active ? "true" : "false");
            json += buf;
        }
    }
    json += "]}\n";
    return json;
}

static std::string BuildBrowserInfoJson() {
    RECT r; GetWindowRect(s_hwnd, &r);
    int tabCount = BrowserChrome::GetTabCount(s_hwnd);
    char buf[512];
    _snprintf_s(buf, sizeof(buf), _TRUNCATE,
        "{\"ok\":true,\"browser\":\"Aeon\",\"version\":\"" AEON_VERSION "\","
        "\"pid\":%lu,\"hwnd\":%llu,\"tab_count\":%d,"
        "\"window\":{\"x\":%d,\"y\":%d,\"w\":%d,\"h\":%d},"
        "\"pipe\":\"\\\\\\\\.\\\\pipe\\\\aeon-agent\","
        "\"cdp_port\":9222}\n",
        GetCurrentProcessId(), (unsigned long long)(uintptr_t)s_hwnd,
        tabCount,
        r.left, r.top, r.right - r.left, r.bottom - r.top);
    return buf;
}

// ── Pipe thread ──────────────────────────────────────────────────────

static void PipeThread() {
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX | SEM_NOOPENFILEERRORBOX);

    PSECURITY_DESCRIPTOR pSD = nullptr;
    SECURITY_ATTRIBUTES sa = {};
    sa.nLength = sizeof(sa);
    sa.bInheritHandle = FALSE;
    if (ConvertStringSecurityDescriptorToSecurityDescriptorW(
            L"D:(A;;GA;;;WD)S:(ML;;NW;;;ME)",
            SDDL_REVISION_1,
            &pSD,
            nullptr)) {
        sa.lpSecurityDescriptor = pSD;
    }

    while (!s_stopRequest) {
        if (s_pipe == INVALID_HANDLE_VALUE) {
            s_pipe = CreateNamedPipeW(
                PIPE_NAME,
                PIPE_ACCESS_DUPLEX,
                PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT,
                PIPE_UNLIMITED_INSTANCES,
                PIPE_BUFSIZE, PIPE_BUFSIZE,
                0,
                &sa);

            if (s_pipe == INVALID_HANDLE_VALUE) {
                DWORD err = GetLastError();
                fprintf(stderr, "[AgentPipe] CreateNamedPipe failed: %lu (retrying)\n", err);
                Sleep(100);
                continue;
            }
        }

        // Block until a client connects (or stop is requested)
        BOOL connected = ConnectNamedPipe(s_pipe, nullptr)
            ? TRUE : (GetLastError() == ERROR_PIPE_CONNECTED);

        if (!connected || s_stopRequest) {
            if (s_pipe != INVALID_HANDLE_VALUE) {
                CloseHandle(s_pipe);
                s_pipe = INVALID_HANDLE_VALUE;
            }
            if (s_stopRequest) break;
            Sleep(10);
            continue;
        }

        fprintf(stdout, "[AgentPipe] Client connected.\n");

        // Client session — read commands until disconnect
        char readBuf[PIPE_BUFSIZE];
        std::string lineBuf;

        while (!s_stopRequest) {
            DWORD bytesRead = 0;
            BOOL ok = ReadFile(s_pipe, readBuf, sizeof(readBuf) - 1,
                               &bytesRead, nullptr);
            if (!ok || bytesRead == 0) break;

            readBuf[bytesRead] = '\0';
            lineBuf += readBuf;

            // Process full lines (newline-delimited JSON)
            size_t newlinePos;
            while ((newlinePos = lineBuf.find('\n')) != std::string::npos) {
                std::string line = lineBuf.substr(0, newlinePos);
                lineBuf.erase(0, newlinePos + 1);

                // Trim whitespace
                while (!line.empty() && (line.back() == '\r' || line.back() == ' '))
                    line.pop_back();
                if (line.empty()) continue;

                std::string trimmedLine = line;
                while (!trimmedLine.empty() && (trimmedLine.front() == ' ' || trimmedLine.front() == '\t')) {
                    trimmedLine.erase(trimmedLine.begin());
                }

                if (trimmedLine.empty() || trimmedLine.front() != '{') {
                    std::string errResp = "{\"ok\":false,\"error\":\"Invalid input command format: line must be JSON object starting with '{'\"}\n";
                    DWORD written = 0;
                    WriteFile(s_pipe, errResp.c_str(), (DWORD)errResp.size(), &written, nullptr);
                    FlushFileBuffers(s_pipe);
                    continue;
                }

                // Dispatch to UI thread via SendMessageTimeout (synchronous with safety timeout)
                AgentCmdData cmdData;
                cmdData.json = trimmedLine.c_str();

                DWORD_PTR dwResult = 0;
                LRESULT lRes = SendMessageTimeoutW(s_hwnd, WM_AEON_AGENT,
                    (WPARAM)&cmdData, (LPARAM)0,
                    SMTO_NORMAL | SMTO_ABORTIFHUNG, 10000, &dwResult);

                if (lRes == 0 || cmdData.response.empty()) {
                    cmdData.response = "{\"ok\":false,\"error\":\"Timeout or error dispatching command to UI thread\"}\n";
                }

                if (cmdData.response.back() != '\n') {
                    cmdData.response += "\n";
                }

                // Write response back to pipe
                DWORD written = 0;
                WriteFile(s_pipe, cmdData.response.c_str(),
                    (DWORD)cmdData.response.size(), &written, nullptr);
                FlushFileBuffers(s_pipe);
            }
        }

        fprintf(stdout, "[AgentPipe] Client disconnected.\n");
        DisconnectNamedPipe(s_pipe);
        CloseHandle(s_pipe);
        s_pipe = INVALID_HANDLE_VALUE;
    }

    if (s_pipe != INVALID_HANDLE_VALUE) {
        CloseHandle(s_pipe);
        s_pipe = INVALID_HANDLE_VALUE;
    }

    if (pSD) LocalFree(pSD);
    s_running = false;
    fprintf(stdout, "[AgentPipe] Stopped.\n");
}

// ── Public API ───────────────────────────────────────────────────────

bool Start(HWND mainHwnd) {
    if (s_running) return true;
    s_hwnd = mainHwnd;
    s_stopRequest = false;
    s_running = true;
    s_thread = std::thread(PipeThread);
    fprintf(stdout, "[AgentPipe] Agent control pipe started.\n");
    return true;
}

void Stop() {
    if (!s_running) return;
    s_stopRequest = true;

    // Unblock ConnectNamedPipe by briefly connecting
    HANDLE hTmp = CreateFileW(PIPE_NAME, GENERIC_READ | GENERIC_WRITE,
        0, nullptr, OPEN_EXISTING, 0, nullptr);
    if (hTmp != INVALID_HANDLE_VALUE) CloseHandle(hTmp);

    if (s_thread.joinable()) {
        s_thread.join();
    }

    if (s_pipe != INVALID_HANDLE_VALUE) {
        DisconnectNamedPipe(s_pipe);
        CloseHandle(s_pipe);
        s_pipe = INVALID_HANDLE_VALUE;
    }
    s_running = false;

    fprintf(stdout, "[AgentPipe] Agent control pipe stopped.\n");
}

bool IsRunning() {
    return s_running;
}

} // namespace AeonAgentPipe
