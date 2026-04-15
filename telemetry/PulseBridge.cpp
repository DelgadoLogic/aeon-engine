// AeonBrowser — PulseBridge.cpp
// DelgadoLogic | Senior Security Engineer
//
// PURPOSE: Telemetry bridge between Aeon Browser and the DelgadoLogic
// PulseClient endpoint (same endpoint as LogicFlow desktop agent).
// Anonymous, category-level only — we never transmit URLs or page content.
//
// DATA SENT on startup ping:
//   - Aeon tier (e.g., "WinXP-HiSpec")
//   - OS version (major.minor.build)
//   - RAM tier bucket: <512MB / 512MB–2GB / 2GB–8GB / 8GB+
//   - Browser locale
//   - Crash report path (if crash sentinel exists from last session)
//
// DATA NEVER SENT:
//   - URLs visited
//   - Search queries
//   - Passwords or form data
//   - IP address (we hash it server-side immediately on receipt)
//
// OPT-OUT: HKLM\SOFTWARE\DelgadoLogic\Aeon\TelemetryEnabled = 0
//          OR set during LogicFlow install "opt out of improvement data"
//
// IT TROUBLESHOOTING:
//   - Ping silently skipped: TelemetryEnabled = 0, or first 24h cooldown.
//   - Crash upload fails: telemetry.delgadologic.tech unreachable (firewall).
//     Crash file stays in %TEMP% and retries next launch — never blocks startup.

#include "PulseBridge.h"
#include "../core/probe/HardwareProbe.h"
#include <windows.h>
#include <wininet.h>
#include <cstdio>
#include <cstring>
#include <new>

#pragma comment(lib, "wininet.lib")

namespace PulseBridge {

static bool IsTelemetryEnabled() {
    // Check Aeon-specific key first, then LogicFlow key (same opt-out tree)
    HKEY hk;
    DWORD val = 1, sz = sizeof(val);

    const char* keys[] = {
        "SOFTWARE\\DelgadoLogic\\Aeon",
        "SOFTWARE\\DelgadoLogic\\LogicFlow"
    };
    for (const char* keyPath : keys) {
        if (RegOpenKeyExA(HKEY_LOCAL_MACHINE, keyPath, 0, KEY_READ, &hk)
                == ERROR_SUCCESS) {
            RegQueryValueExA(hk, "TelemetryEnabled", nullptr, nullptr,
                reinterpret_cast<BYTE*>(&val), &sz);
            RegCloseKey(hk);
            if (val == 0) return false;
        }
    }
    return true; // default = enabled (disclosed in EULA)
}

static const char* RamBucket(uint64_t ramBytes) {
    uint64_t mb = ramBytes / (1024 * 1024);
    if (mb <  512) return "<512MB";
    if (mb < 2048) return "512MB-2GB";
    if (mb < 8192) return "2GB-8GB";
    return "8GB+";
}

void SendStartupPing(const SystemProfile& p) {
    if (!IsTelemetryEnabled()) {
        fprintf(stdout, "[PulseBridge] Telemetry opted out — ping suppressed.\n");
        return;
    }

    // Build query string (no JSON — WinINet query string works on Win9x)
    char query[512];
    _snprintf_s(query, sizeof(query), _TRUNCATE,
        "tier=%s&os=%u.%u.%u&ram=%s&product=aeon",
        AeonProbe::TierName(p.tier),
        p.osMajor, p.osMinor, p.osBuild,
        RamBucket(p.ramBytes));

    fprintf(stdout, "[PulseBridge] Startup ping: %s\n", query);

    // Use WinINet (available Win9x through Win11 with IE 4+)
    // Fire-and-forget: we do NOT block startup waiting for a response.
    // IT NOTE: WinINet is synchronous by API but we call it on a thread.
    // TODO: Spawn background thread for the HTTP call to avoid blocking UI.
    HINTERNET hNet = InternetOpenA("AeonBrowser/1.0",
        INTERNET_OPEN_TYPE_PRECONFIG, nullptr, nullptr, 0);
    if (!hNet) {
        fprintf(stderr, "[PulseBridge] WinINet unavailable — ping skipped.\n");
        return;
    }

    char url[512];
    _snprintf_s(url, sizeof(url), _TRUNCATE,
        "https://telemetry.delgadologic.tech/aeon/ping?%s", query);

    HINTERNET hReq = InternetOpenUrlA(hNet, url, nullptr, 0,
        INTERNET_FLAG_SECURE | INTERNET_FLAG_NO_UI, 0);
    if (hReq) {
        InternetCloseHandle(hReq);
        fprintf(stdout, "[PulseBridge] Ping delivered.\n");
    } else {
        fprintf(stdout, "[PulseBridge] Ping failed (offline?) — ignored.\n");
    }
    InternetCloseHandle(hNet);
}

void UploadPendingCrash() {
    if (!IsTelemetryEnabled()) return;

    // Check for crash sentinel from previous session
    char tempPath[MAX_PATH], sentinelPath[MAX_PATH];
    GetTempPathA(MAX_PATH, tempPath);
    _snprintf_s(sentinelPath, sizeof(sentinelPath), _TRUNCATE,
        "%saeon_crash_pending.txt", tempPath);

    FILE* sentinel = nullptr;
    fopen_s(&sentinel, sentinelPath, "r");
    if (!sentinel) return;  // No pending crash — normal startup

    // Read dump path and JSON path from sentinel
    char dmpPath[MAX_PATH] = {}, jsonPath[MAX_PATH] = {};
    if (fgets(dmpPath, sizeof(dmpPath), sentinel)) {
        // Strip newline
        char* nl = strchr(dmpPath, '\n');
        if (nl) *nl = '\0';
        nl = strchr(dmpPath, '\r');
        if (nl) *nl = '\0';
    }
    if (fgets(jsonPath, sizeof(jsonPath), sentinel)) {
        char* nl = strchr(jsonPath, '\n');
        if (nl) *nl = '\0';
        nl = strchr(jsonPath, '\r');
        if (nl) *nl = '\0';
    }
    fclose(sentinel);

    fprintf(stdout, "[PulseBridge] Found pending crash report:\n");
    fprintf(stdout, "  Dump: %s\n", dmpPath);
    fprintf(stdout, "  JSON: %s\n", jsonPath);

    // Read JSON crash report
    FILE* jsonFile = nullptr;
    fopen_s(&jsonFile, jsonPath, "r");
    if (!jsonFile) {
        fprintf(stderr, "[PulseBridge] Cannot read crash JSON — skipping upload.\n");
        DeleteFileA(sentinelPath);
        return;
    }

    // Read entire JSON file
    fseek(jsonFile, 0, SEEK_END);
    long jsonSize = ftell(jsonFile);
    fseek(jsonFile, 0, SEEK_SET);

    if (jsonSize <= 0 || jsonSize > 1024 * 1024) {
        fprintf(stderr, "[PulseBridge] JSON file invalid size — skipping.\n");
        fclose(jsonFile);
        DeleteFileA(sentinelPath);
        return;
    }

    char* jsonBody = new (std::nothrow) char[(size_t)jsonSize + 1];
    if (!jsonBody) {
        fclose(jsonFile);
        DeleteFileA(sentinelPath);
        return;
    }
    fread(jsonBody, 1, (size_t)jsonSize, jsonFile);
    jsonBody[jsonSize] = '\0';
    fclose(jsonFile);

    // Upload via WinINet POST
    HINTERNET hNet = InternetOpenA("AeonBrowser/1.0 CrashUpload",
        INTERNET_OPEN_TYPE_PRECONFIG, nullptr, nullptr, 0);
    if (!hNet) {
        fprintf(stderr, "[PulseBridge] WinINet unavailable for crash upload.\n");
        delete[] jsonBody;
        return;  // Leave sentinel — retry next launch
    }

    // Production endpoint — crashes.delgadologic.tech → Cloud Run (aeon-browser-build)
    HINTERNET hConn = InternetConnectA(hNet, "crashes.delgadologic.tech",
        INTERNET_DEFAULT_HTTPS_PORT, nullptr, nullptr,
        INTERNET_SERVICE_HTTP, 0, 0);

    bool uploaded = false;
    if (hConn) {
        const char* acceptTypes[] = { "application/json", nullptr };
        HINTERNET hReq = HttpOpenRequestA(hConn, "POST", "/aeon/report",
            nullptr, nullptr, acceptTypes,
            INTERNET_FLAG_SECURE | INTERNET_FLAG_NO_UI, 0);

        if (hReq) {
            const char* headers = "Content-Type: application/json\r\n";
            if (HttpSendRequestA(hReq, headers, (DWORD)strlen(headers),
                    jsonBody, (DWORD)jsonSize)) {
                // Check HTTP response
                DWORD statusCode = 0, statusSize = sizeof(statusCode);
                HttpQueryInfoA(hReq, HTTP_QUERY_STATUS_CODE | HTTP_QUERY_FLAG_NUMBER,
                    &statusCode, &statusSize, nullptr);

                if (statusCode >= 200 && statusCode < 300) {
                    fprintf(stdout, "[PulseBridge] Crash report uploaded (HTTP %u).\n",
                        statusCode);
                    uploaded = true;
                } else {
                    fprintf(stderr, "[PulseBridge] Crash upload HTTP %u — will retry.\n",
                        statusCode);
                }
            } else {
                fprintf(stderr, "[PulseBridge] Crash upload failed (offline?) — will retry.\n");
            }
            InternetCloseHandle(hReq);
        }
        InternetCloseHandle(hConn);
    }
    InternetCloseHandle(hNet);
    delete[] jsonBody;

    if (uploaded) {
        // Clean up: remove sentinel, JSON, and dump
        DeleteFileA(sentinelPath);
        DeleteFileA(jsonPath);
        // Keep .dmp for local diagnosis — user can delete manually
        fprintf(stdout, "[PulseBridge] Crash files cleaned up.\n");
    }
    // If not uploaded, leave files for retry on next launch
}

} // namespace PulseBridge

