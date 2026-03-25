// AeonBrowser — NewTabHandler.cpp
// DelgadoLogic | Lead Systems Architect
//
// PURPOSE: Intercepts aeon:// protocol URLs and serves built-in content.
// Called by the protocol router (router.rs) via C FFI when scheme = "aeon".
//
// PROTOCOL: aeon://
//   aeon://newtab            → resources/newtab/newtab.html (from EXE resources)
//   aeon://settings          → resources/settings/settings.html
//   aeon://history           → resources/history/history.html
//   aeon://bookmarks         → resources/bookmarks/bookmarks.html
//   aeon://downloads         → resources/downloads/downloads.html
//   aeon://crash             → crash recovery page
//   aeon://about             → about Aeon + version info
//
// SETTINGS INJECTION: Before serving newtab.html, we inject the current
// settings as a <script> tag so the JS privacy bar reads them without
// any async IPC:
//   window.aeon = { settings: { adblock: true, tor: false, ... } };
//
// SECURITY: aeon:// pages cannot access file:// or external URLs.
// They run in a trusted context with access to window.aeon bridge only.

#include "NewTabHandler.h"
#include "../settings/SettingsEngine.h"
#include "../engine/AeonBridge.h"
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
        fprintf(stderr, "[NewTab] Resource not found: %s\n", fullPath);
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
// Build the settings injection script from current AeonSettings
// ---------------------------------------------------------------------------
static void BuildSettingsScript(char* out, int outLen) {
    AeonSettings s = SettingsEngine::Load();
    _snprintf_s(out, outLen, _TRUNCATE,
        "<script>"
        "window.aeon = {"
        "  settings: {"
        "    adblock: %s,"
        "    tor: %s,"
        "    https_upgrade: %s,"
        "    fp_guard: %s,"
        "    i2p: %s,"
        "    search_engine: \"%s\""
        "  },"
        "  navigate: function(url) { window.location.href = url; },"
        "  addDial: function() { /* bridge will override */ }"
        "};"
        "</script>",
        s.adblock_enabled       ? "true" : "false",
        s.tor_enabled           ? "true" : "false",
        s.https_upgrade         ? "true" : "false",
        s.fingerprint_guard     ? "true" : "false",
        s.i2p_enabled           ? "true" : "false",
        s.search_engine
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

    char resourceFile[128] = {};
    if      (strcmp(path, "newtab")     == 0) strcpy_s(resourceFile, "newtab/newtab.html");
    else if (strcmp(path, "settings")   == 0) strcpy_s(resourceFile, "pages/settings.html");
    else if (strcmp(path, "history")    == 0) strcpy_s(resourceFile, "pages/history.html");
    else if (strcmp(path, "bookmarks")  == 0) strcpy_s(resourceFile, "pages/bookmarks.html");
    else if (strcmp(path, "downloads")  == 0) strcpy_s(resourceFile, "pages/downloads.html");
    else if (strcmp(path, "passwords")  == 0) strcpy_s(resourceFile, "pages/passwords.html");
    else if (strcmp(path, "crash")      == 0) strcpy_s(resourceFile, "pages/crash.html");
    else if (strcmp(path, "about")      == 0) {
        // Generate about page inline
        char* buf = static_cast<char*>(malloc(4096));
        if (!buf) return nullptr;
        _snprintf_s(buf, 4096, _TRUNCATE,
            "<!DOCTYPE html><html><head><title>About Aeon</title>"
            "<style>body{font-family:system-ui;background:#0d0e14;color:#e8e8f0;"
            "display:flex;align-items:center;justify-content:center;height:100vh;margin:0;}"
            ".card{background:#16182a;border:1px solid rgba(108,99,255,.2);border-radius:14px;"
            "padding:40px;text-align:center;max-width:360px;}"
            "h1{font-size:28px;font-weight:300;color:#a78bfa;margin-bottom:8px;}"
            "p{color:#8888aa;font-size:14px;line-height:1.6;}</style></head><body>"
            "<div class='card'>"
            "<h1>Aeon Browser</h1>"
            "<p>Version 1.0.0</p>"
            "<p>by <strong style='color:#e8e8f0'>DelgadoLogic</strong></p>"
            "<p style='margin-top:16px;font-size:12px'>Timeless. From Windows 3.1 to Windows 11.<br>"
            "<a href='https://delgadologic.tech' style='color:#6c63ff'>delgadologic.tech</a></p>"
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

    // Inject settings into newtab page (before closing </head>)
    if (strcmp(path, "newtab") == 0) {
        char scriptTag[1024] = {};
        BuildSettingsScript(scriptTag, sizeof(scriptTag));

        // Insert script before </head>
        char* headClose = strstr(html, "</head>");
        if (headClose) {
            // Shift content right to make room
            int insertLen  = (int)strlen(scriptTag);
            int tailLen    = (int)strlen(headClose);
            memmove(headClose + insertLen, headClose, tailLen + 1);
            memcpy(headClose, scriptTag, insertLen);
        }
    }

    return html; // caller calls free()
}

bool IsAeonUrl(const char* url) {
    return url && strncmp(url, "aeon://", 7) == 0;
}

} // namespace NewTabHandler
