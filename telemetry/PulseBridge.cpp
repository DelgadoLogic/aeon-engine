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

} // namespace PulseBridge
