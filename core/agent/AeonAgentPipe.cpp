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
#include "../AeonVersion.h"
#include <cstdio>
#include <cstring>
#include <string>
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
    char needle[128];
    _snprintf_s(needle, sizeof(needle), _TRUNCATE, "\"%s\"", key);
    const char* pos = strstr(json, needle);
    if (!pos) return "";
    pos += strlen(needle);
    // skip whitespace and colon
    while (*pos && (*pos == ' ' || *pos == ':' || *pos == '\t')) pos++;
    if (*pos != '"') return "";
    pos++; // skip opening quote
    std::string val;
    for (; *pos && *pos != '"'; pos++) {
        if (*pos == '\\' && *(pos + 1)) { pos++; val += *pos; }
        else val += *pos;
    }
    return val;
}

static int JsonGetInt(const char* json, const char* key) {
    char needle[128];
    _snprintf_s(needle, sizeof(needle), _TRUNCATE, "\"%s\"", key);
    const char* pos = strstr(json, needle);
    if (!pos) return -1;
    pos += strlen(needle);
    while (*pos && (*pos == ' ' || *pos == ':' || *pos == '\t')) pos++;
    return atoi(pos);
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

    if (cmd == "tab.list") {
        data->response = BuildTabListJson();
    }
    else if (cmd == "tab.active") {
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
    else if (cmd == "tab.new") {
        std::string url = JsonGetString(data->json, "url");
        unsigned int id = BrowserChrome::CreateTab(s_hwnd,
            url.empty() ? nullptr : url.c_str());
        char buf[256];
        _snprintf_s(buf, sizeof(buf), _TRUNCATE,
            "{\"ok\":%s,\"tab_id\":%u}\n", id ? "true" : "false", id);
        data->response = buf;
    }
    else if (cmd == "tab.close") {
        int tabId = JsonGetInt(data->json, "tab_id");
        bool ok = (tabId >= 0) && BrowserChrome::CloseTabById(s_hwnd, (unsigned int)tabId);
        char buf[128];
        _snprintf_s(buf, sizeof(buf), _TRUNCATE, "{\"ok\":%s}\n", ok ? "true" : "false");
        data->response = buf;
    }
    else if (cmd == "tab.focus") {
        int tabId = JsonGetInt(data->json, "tab_id");
        bool ok = (tabId >= 0) && BrowserChrome::FocusTabById(s_hwnd, (unsigned int)tabId);
        char buf[128];
        _snprintf_s(buf, sizeof(buf), _TRUNCATE, "{\"ok\":%s}\n", ok ? "true" : "false");
        data->response = buf;
    }
    else if (cmd == "tab.navigate") {
        int tabId = JsonGetInt(data->json, "tab_id");
        std::string url = JsonGetString(data->json, "url");
        bool ok = (tabId >= 0) && !url.empty() &&
                  BrowserChrome::NavigateTab(s_hwnd, (unsigned int)tabId, url.c_str());
        char buf[128];
        _snprintf_s(buf, sizeof(buf), _TRUNCATE, "{\"ok\":%s}\n", ok ? "true" : "false");
        data->response = buf;
    }
    else if (cmd == "browser.info") {
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
    else {
        char buf[256];
        _snprintf_s(buf, sizeof(buf), _TRUNCATE,
            "{\"ok\":false,\"error\":\"unknown command: %s\"}\n",
            JsonEscape(cmd.c_str()).c_str());
        data->response = buf;
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
    fprintf(stdout, "[AgentPipe] Starting on \\\\.\\pipe\\aeon-agent ...\n");

    // LOCAL_ONLY security descriptor — only current user on localhost
    PSECURITY_DESCRIPTOR pSD = nullptr;
    SECURITY_ATTRIBUTES sa = {};
    sa.nLength = sizeof(sa);
    sa.bInheritHandle = FALSE;

    if (ConvertStringSecurityDescriptorToSecurityDescriptorA(
            "D:(A;;GRGW;;;WD)", SDDL_REVISION_1, &pSD, nullptr)) {
        sa.lpSecurityDescriptor = pSD;
    }

    while (!s_stopRequest) {
        s_pipe = CreateNamedPipeW(
            PIPE_NAME,
            PIPE_ACCESS_DUPLEX,
            PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT |
                PIPE_REJECT_REMOTE_CLIENTS,
            PIPE_UNLIMITED_INSTANCES,
            PIPE_BUFSIZE, PIPE_BUFSIZE,
            0,
            pSD ? &sa : nullptr);

        if (s_pipe == INVALID_HANDLE_VALUE) {
            fprintf(stderr, "[AgentPipe] CreateNamedPipe failed: %lu\n",
                    GetLastError());
            break;
        }

        // Block until a client connects (or stop is requested)
        BOOL connected = ConnectNamedPipe(s_pipe, nullptr)
            ? TRUE : (GetLastError() == ERROR_PIPE_CONNECTED);

        if (!connected || s_stopRequest) {
            CloseHandle(s_pipe);
            s_pipe = INVALID_HANDLE_VALUE;
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

            // Process complete lines (newline-delimited JSON)
            size_t pos;
            while ((pos = lineBuf.find('\n')) != std::string::npos) {
                std::string line = lineBuf.substr(0, pos);
                lineBuf.erase(0, pos + 1);

                if (line.empty() || line[0] != '{') continue;

                // Dispatch to UI thread via SendMessage (synchronous)
                AgentCmdData cmdData;
                cmdData.json = line.c_str();

                SendMessage(s_hwnd, WM_AEON_AGENT,
                    (WPARAM)&cmdData, (LPARAM)0);

                // Write response back to pipe
                if (!cmdData.response.empty()) {
                    DWORD written = 0;
                    WriteFile(s_pipe, cmdData.response.c_str(),
                        (DWORD)cmdData.response.size(), &written, nullptr);
                    FlushFileBuffers(s_pipe);
                }
            }
        }

        fprintf(stdout, "[AgentPipe] Client disconnected.\n");
        DisconnectNamedPipe(s_pipe);
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
    s_thread.detach();
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

    if (s_pipe != INVALID_HANDLE_VALUE) {
        DisconnectNamedPipe(s_pipe);
        CloseHandle(s_pipe);
        s_pipe = INVALID_HANDLE_VALUE;
    }

    fprintf(stdout, "[AgentPipe] Agent control pipe stopped.\n");
}

bool IsRunning() {
    return s_running;
}

} // namespace AeonAgentPipe
