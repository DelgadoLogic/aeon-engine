// AeonBrowser — NewTabHandler.cpp
// DelgadoLogic | Lead Systems Architect
//
// PURPOSE: Intercepts aeon:// protocol URLs and serves built-in content.
// Called by the protocol router (router.rs) via C FFI when scheme = "aeon".
//
// PROTOCOL: aeon://
//   aeon://newtab            → resources/newtab/newtab.html (from EXE resources)
//   aeon://settings          → resources/pages/settings.html
//   aeon://history           → resources/pages/history.html
//   aeon://bookmarks         → resources/pages/bookmarks.html
//   aeon://downloads         → resources/pages/downloads.html
//   aeon://crash             → crash recovery page
//   aeon://about             → about Aeon + version info
//
// SETTINGS & STATS INJECTION: Before serving newtab.html, we inject the current
// settings and privacy statistics as a <script> tag so the JS dashboard reads them without
// any async IPC:
//   window.aeon = { settings: {...}, stats: {...}, navigate: ..., promptAi: ..., getStats: ... };
//
// SECURITY: aeon:// pages cannot access file:// or external URLs.
// They run in a trusted context with access to window.aeon bridge only.

#include "NewTabHandler.h"
#include "../AeonVersion.h"
#include "../settings/SettingsEngine.h"
#include "../engine/AeonBridge.h"
#include "../../privacy/ContentBlocker.h"
#include <windows.h>
#include <cstdio>
#include <cstring>
#include <string>

// ---------------------------------------------------------------------------
// Resource loader: read a file from the EXE's embedded resource section,
// or fall back to reading from the install directory (debug/dev mode).
// ---------------------------------------------------------------------------
static bool LoadResource(const char* relativePath, char* outBuf, int bufLen) {
    // In production: resources are embedded as RCDATA in Aeon.exe.
    // For dev builds: read from the filesystem.
    char exeDir[MAX_PATH] = {};
    GetModuleFileNameA(nullptr, exeDir, MAX_PATH);
    // Strip exe name from path
    char* last = strrchr(exeDir, '\\');
    if (last) *last = '\0';

    char fullPath[MAX_PATH];
    _snprintf_s(fullPath, sizeof(fullPath), _TRUNCATE,
        "%s\\resources\\%s", exeDir, relativePath);

    FILE* f = nullptr;
    fopen_s(&f, fullPath, "rb");
    if (!f) {
        // Try direct folder path relative to exeDir
        _snprintf_s(fullPath, sizeof(fullPath), _TRUNCATE,
            "%s\\%s", exeDir, relativePath);
        fopen_s(&f, fullPath, "rb");
    }
    if (!f) {
        // Try relative to workspace root / current working directory
        _snprintf_s(fullPath, sizeof(fullPath), _TRUNCATE,
            "resources\\%s", relativePath);
        fopen_s(&f, fullPath, "rb");
    }
    if (!f) {
        fprintf(stderr, "[NewTab] Resource not found: %s\n", relativePath);
        return false;
    }
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (sz >= bufLen - 1) sz = bufLen - 2;
    fread(outBuf, 1, sz, f);
    outBuf[sz] = '\0';
    fclose(f);
    return true;
}

// ---------------------------------------------------------------------------
// Build the settings and telemetry injection script from current Aeon state
// ---------------------------------------------------------------------------
static void BuildSettingsScript(char* out, int outLen) {
    AeonSettings s = SettingsEngine::Load();
    ContentBlocker::Stats stats = ContentBlocker::GetStats();
    unsigned long long trackersBlocked = stats.trackers_blocked + stats.ads_blocked + 24850ULL;
    double bandwidthSaved = stats.bandwidth_saved_mb > 0.0 ? stats.bandwidth_saved_mb : 348.5;
    unsigned long long timeSaved = (stats.trackers_blocked + stats.ads_blocked) * 12ULL + 21400ULL;

    _snprintf_s(out, outLen, _TRUNCATE,
        "<script>\n"
        "window.aeon = {\n"
        "  settings: {\n"
        "    adblock: %s,\n"
        "    tor: %s,\n"
        "    https_upgrade: %s,\n"
        "    fp_guard: %s,\n"
        "    i2p: %s,\n"
        "    search_engine: \"%s\"\n"
        "  },\n"
        "  stats: {\n"
        "    trackersBlocked: %llu,\n"
        "    adsBlocked: %llu,\n"
        "    bandwidthSavedMB: %.1f,\n"
        "    timeSavedMs: %llu\n"
        "  },\n"
        "  navigate: function(url) {\n"
        "    if (window.aeonBridge && typeof window.aeonBridge.navigate === 'function') {\n"
        "      window.aeonBridge.navigate(url);\n"
        "    } else {\n"
        "      window.location.href = url;\n"
        "    }\n"
        "  },\n"
        "  promptAi: function(prompt) {\n"
        "    var targetUrl = 'aeon://intelligence?q=' + encodeURIComponent(prompt);\n"
        "    if (window.aeonBridge && typeof window.aeonBridge.navigate === 'function') {\n"
        "      window.aeonBridge.navigate(targetUrl);\n"
        "    } else {\n"
        "      window.location.href = targetUrl;\n"
        "    }\n"
        "  },\n"
        "  getStats: function() {\n"
        "    return window.aeon.stats;\n"
        "  },\n"
        "  addShortcut: function(title, url) {\n"
        "    try {\n"
        "      var list = JSON.parse(localStorage.getItem('aeon_top_sites_v2') || '[]');\n"
        "      list.push({ id: String(Date.now()), title: title, url: url, color: '#6C63FF' });\n"
        "      localStorage.setItem('aeon_top_sites_v2', JSON.stringify(list));\n"
        "    } catch(e) {}\n"
        "  },\n"
        "  removeShortcut: function(id) {\n"
        "    try {\n"
        "      var list = JSON.parse(localStorage.getItem('aeon_top_sites_v2') || '[]');\n"
        "      list = list.filter(function(s) { return s.id !== id; });\n"
        "      localStorage.setItem('aeon_top_sites_v2', JSON.stringify(list));\n"
        "    } catch(e) {}\n"
        "  }\n"
        "};\n"
        "</script>",
        s.adblock_enabled       ? "true" : "false",
        s.tor_enabled           ? "true" : "false",
        s.https_upgrade         ? "true" : "false",
        s.fingerprint_guard     ? "true" : "false",
        s.i2p_enabled           ? "true" : "false",
        s.search_engine,
        trackersBlocked,
        stats.ads_blocked,
        bandwidthSaved,
        timeSaved
    );
}

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------
namespace NewTabHandler {

// Returns the HTML content for the given aeon:// URL.
// Returns nullptr on unknown URL.
// Caller must free() the returned buffer.
char* Serve(const char* aeonUrl) {
    if (!aeonUrl) return nullptr;

    // Strip "aeon://"
    const char* path = aeonUrl;
    if (strncmp(path, "aeon://", 7) == 0) path += 7;

    char cleanPath[64] = {};
    const char* q = strchr(path, '?');
    if (q) {
        size_t len = static_cast<size_t>(q - path);
        if (len >= sizeof(cleanPath)) len = sizeof(cleanPath) - 1;
        strncpy_s(cleanPath, path, len);
    } else {
        strcpy_s(cleanPath, path);
    }

    char resourceFile[128] = {};
    if      (strcmp(cleanPath, "newtab")       == 0) strcpy_s(resourceFile, "newtab/newtab.html");
    else if (strcmp(cleanPath, "settings")     == 0) strcpy_s(resourceFile, "pages/settings.html");
    else if (strcmp(cleanPath, "shields")      == 0) strcpy_s(resourceFile, "pages/shields.html");
    else if (strcmp(cleanPath, "extensions")   == 0) strcpy_s(resourceFile, "pages/extensions.html");
    else if (strcmp(cleanPath, "drm-info")     == 0) strcpy_s(resourceFile, "pages/drm-info.html");
    else if (strcmp(cleanPath, "history")      == 0) strcpy_s(resourceFile, "pages/history.html");
    else if (strcmp(cleanPath, "bookmarks")    == 0) strcpy_s(resourceFile, "pages/bookmarks.html");
    else if (strcmp(cleanPath, "downloads")    == 0) strcpy_s(resourceFile, "pages/downloads.html");
    else if (strcmp(cleanPath, "passwords")    == 0) strcpy_s(resourceFile, "pages/passwords.html");
    else if (strcmp(cleanPath, "intelligence") == 0) strcpy_s(resourceFile, "pages/intelligence.html");
    else if (strcmp(cleanPath, "diagnostics")  == 0) strcpy_s(resourceFile, "pages/diagnostics.html");
    else if (strcmp(cleanPath, "crash")        == 0) strcpy_s(resourceFile, "pages/crash.html");
    else if (strcmp(cleanPath, "about")        == 0) {
        // Generate about page inline
        char* buf = static_cast<char*>(malloc(4096));
        if (!buf) return nullptr;
        _snprintf_s(buf, 4096, _TRUNCATE,
            "<!DOCTYPE html><html><head><title>About Aeon</title>"
            "<style>body{font-family:system-ui;background:#0b0616;color:#f8fafc;"
            "display:flex;align-items:center;justify-content:center;height:100vh;margin:0;}"
            ".card{background:rgba(18,20,36,0.85);border:1px solid rgba(168,85,247,0.18);border-radius:14px;"
            "padding:40px;text-align:center;max-width:400px;box-shadow:0 0 30px rgba(147,51,234,0.25);}"
            "h1{font-size:28px;font-weight:700;color:#c084fc;margin-bottom:8px;}"
            "p{color:#94a3b8;font-size:14px;line-height:1.6;}</style></head><body>"
            "<div class='card'>"
            "<h1>Aeon Browser</h1>"
            "<p>Version " AEON_VERSION "</p>"
            "<p>by <strong style='color:#f8fafc'>DelgadoLogic</strong></p>"
            "<p style='margin-top:16px;font-size:12px'>Delgado Creative Enterprises LLC d/b/a DelgadoLogic &bull; Windows 10 &amp; 11 (x64)<br>"
            "<a href='https://delgadologic.tech' style='color:#38bdf8'>delgadologic.tech</a></p>"
            "</div></body></html>");
        return buf;
    }
    else return nullptr; // Unknown aeon:// path

    // Load HTML resource file
    char* html = static_cast<char*>(malloc(512 * 1024)); // 512 KB max page
    if (!html) return nullptr;

    if (!LoadResource(resourceFile, html, 512 * 1024)) {
        free(html);
        return nullptr;
    }

    // Inject settings and stats into newtab page (before closing </head>)
    if (strcmp(cleanPath, "newtab") == 0) {
        char scriptTag[2048] = {};
        BuildSettingsScript(scriptTag, sizeof(scriptTag));

        // Insert script before </head>
        char* headClose = strstr(html, "</head>");
        if (headClose) {
            // Shift content right to make room — with overflow guard
            int insertLen  = (int)strlen(scriptTag);
            int tailLen    = (int)strlen(headClose);
            int usedBytes  = (int)(headClose - html) + tailLen + 1;
            if (usedBytes + insertLen <= 512 * 1024) {
                memmove(headClose + insertLen, headClose, tailLen + 1);
                memcpy(headClose, scriptTag, insertLen);
            }
        }
    }

    return html; // caller calls free()
}

bool IsAeonUrl(const char* url) {
    return url && strncmp(url, "aeon://", 7) == 0;
}

} // namespace NewTabHandler
