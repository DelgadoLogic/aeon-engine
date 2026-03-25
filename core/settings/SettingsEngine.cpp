// AeonBrowser — SettingsEngine.cpp
// DelgadoLogic | Lead Systems Architect
//
// Minimal JSON writer/reader — no external dependencies.
// Security note: we NEVER eval JSON, only pattern-match key:"value" pairs.

#include "SettingsEngine.h"
#include <windows.h>
#include <shlobj.h>
#include <cstdio>
#include <cstring>
#include <cstdlib>

static constexpr char REG_KEY[] = "SOFTWARE\\DelgadoLogic\\Aeon";

namespace SettingsEngine {

void GetSettingsPath(char* buf, size_t buf_len) {
    char appData[MAX_PATH] = {};
    SHGetFolderPathA(nullptr, CSIDL_APPDATA, nullptr, 0, appData);
    char dir[MAX_PATH];
    _snprintf_s(dir, sizeof(dir), _TRUNCATE, "%s\\DelgadoLogic\\Aeon", appData);
    CreateDirectoryA(dir, nullptr);
    _snprintf_s(buf, buf_len, _TRUNCATE, "%s\\settings.json", dir);
}

AeonSettings Defaults() {
    AeonSettings s = {};
    s.adblock_enabled       = true;
    s.tracker_block_enabled = true;
    s.fingerprint_guard     = true;
    s.gpc_enabled           = true;
    s.cookie_banners        = true;
    s.block_3p_cookies      = true;
    s.doh_enabled           = true;
    strcpy_s(s.doh_provider, "cloudflare");
    s.https_upgrade         = true;
    s.safe_browsing         = true;
    s.cert_transparency     = true;
    s.mixed_content_block   = true;
    strcpy_s(s.min_tls, "tls13");
    s.tor_enabled           = false;
    s.i2p_enabled           = false;
    s.bypass_enabled        = false;
    s.firewall_mode         = false;
    strcpy_s(s.tor_socks_addr, "127.0.0.1:9150");
    strcpy_s(s.i2p_http_proxy, "127.0.0.1:4444");
    s.auto_detect_proxy     = true;

    // dl_path: %USERPROFILE%\Downloads
    char userProfile[MAX_PATH] = {};
    SHGetFolderPathA(nullptr, CSIDL_PROFILE, nullptr, 0, userProfile);
    _snprintf_s(s.dl_path, sizeof(s.dl_path), _TRUNCATE,
        "%s\\Downloads", userProfile);
    CreateDirectoryA(s.dl_path, nullptr);

    s.dl_ask                = false;
    s.auto_open_after       = false;
    s.dl_streams            = 4;
    s.tab_sleep_enabled     = true;
    s.tab_sleep_minutes     = 30;
    s.restore_session       = true;
    s.show_bookmarks_bar    = true;
    s.smooth_scrolling      = true;
    strcpy_s(s.homepage,      "aeon://newtab");
    strcpy_s(s.startup_page,  "newtab");
    strcpy_s(s.search_engine, "ddg");
    s.theme                 = 'A';
    s.hardware_accel        = true;
    s.auto_update           = true;
    strcpy_s(s.update_channel, "stable");

    DWORD tele = ReadMachineDword("TelemetryEnabled", 1);
    s.telemetry_enabled = (tele != 0);
    return s;
}

// ---------------------------------------------------------------------------
// Minimal JSON helpers (no regex — simple key:value scanner)
// ---------------------------------------------------------------------------
static bool ReadJsonBool(const char* json, const char* key, bool def) {
    char pattern[64];
    _snprintf_s(pattern, sizeof(pattern), _TRUNCATE, "\"%s\":", key);
    const char* p = strstr(json, pattern);
    if (!p) return def;
    p += strlen(pattern);
    while (*p == ' ') p++;
    if (strncmp(p, "true",  4) == 0) return true;
    if (strncmp(p, "false", 5) == 0) return false;
    return def;
}

static int ReadJsonInt(const char* json, const char* key, int def) {
    char pattern[64];
    _snprintf_s(pattern, sizeof(pattern), _TRUNCATE, "\"%s\":", key);
    const char* p = strstr(json, pattern);
    if (!p) return def;
    p += strlen(pattern);
    while (*p == ' ') p++;
    if (*p >= '0' && *p <= '9') return atoi(p);
    return def;
}

static void ReadJsonStr(const char* json, const char* key,
                        char* out, size_t out_len, const char* def) {
    char pattern[64];
    _snprintf_s(pattern, sizeof(pattern), _TRUNCATE, "\"%s\":\"", key);
    const char* p = strstr(json, pattern);
    if (!p) { strncpy_s(out, out_len, def, _TRUNCATE); return; }
    p += strlen(pattern);
    size_t i = 0;
    while (*p && *p != '"' && i < out_len - 1) out[i++] = *p++;
    out[i] = '\0';
}

AeonSettings Load() {
    char path[MAX_PATH];
    GetSettingsPath(path, sizeof(path));

    FILE* f = nullptr;
    fopen_s(&f, path, "r");
    if (!f) return Defaults();

    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (sz <= 0 || sz > 65536) { fclose(f); return Defaults(); }

    char* buf = static_cast<char*>(malloc(sz + 1));
    if (!buf) { fclose(f); return Defaults(); }
    fread(buf, 1, sz, f);
    buf[sz] = '\0';
    fclose(f);

    AeonSettings s = Defaults();
    s.adblock_enabled        = ReadJsonBool(buf, "adblock_enabled",       true);
    s.tracker_block_enabled  = ReadJsonBool(buf, "tracker_block_enabled", true);
    s.fingerprint_guard      = ReadJsonBool(buf, "fingerprint_guard",     true);
    s.gpc_enabled            = ReadJsonBool(buf, "gpc_enabled",           true);
    s.cookie_banners         = ReadJsonBool(buf, "cookie_banners",        true);
    s.block_3p_cookies       = ReadJsonBool(buf, "block_3p_cookies",      true);
    s.doh_enabled            = ReadJsonBool(buf, "doh_enabled",           true);
    ReadJsonStr(buf, "doh_provider", s.doh_provider, sizeof(s.doh_provider), "cloudflare");
    s.https_upgrade          = ReadJsonBool(buf, "https_upgrade",         true);
    s.mixed_content_block    = ReadJsonBool(buf, "mixed_content_block",   true);
    ReadJsonStr(buf, "min_tls", s.min_tls, sizeof(s.min_tls), "tls13");
    s.tor_enabled            = ReadJsonBool(buf, "tor_enabled",           false);
    s.i2p_enabled            = ReadJsonBool(buf, "i2p_enabled",           false);
    s.bypass_enabled         = ReadJsonBool(buf, "bypass_enabled",        false);
    s.firewall_mode          = ReadJsonBool(buf, "firewall_mode",         false);
    ReadJsonStr(buf, "tor_socks_addr", s.tor_socks_addr, sizeof(s.tor_socks_addr), "127.0.0.1:9150");
    ReadJsonStr(buf, "dl_path", s.dl_path, sizeof(s.dl_path), "");
    s.dl_ask                 = ReadJsonBool(buf, "dl_ask",                false);
    s.dl_streams             = ReadJsonInt(buf,  "dl_streams",            4);
    s.tab_sleep_enabled      = ReadJsonBool(buf, "tab_sleep_enabled",     true);
    s.tab_sleep_minutes      = ReadJsonInt(buf,  "tab_sleep_minutes",     30);
    s.restore_session        = ReadJsonBool(buf, "restore_session",       true);
    s.show_bookmarks_bar     = ReadJsonBool(buf, "show_bookmarks_bar",    true);
    ReadJsonStr(buf, "homepage",       s.homepage,      sizeof(s.homepage),      "aeon://newtab");
    ReadJsonStr(buf, "startup_page",   s.startup_page,  sizeof(s.startup_page),  "newtab");
    ReadJsonStr(buf, "search_engine",  s.search_engine, sizeof(s.search_engine), "ddg");
    char themeStr[4] = {"A"};
    ReadJsonStr(buf, "theme", themeStr, sizeof(themeStr), "A");
    s.theme = themeStr[0];
    s.auto_update = ReadJsonBool(buf, "auto_update", true);
    ReadJsonStr(buf, "update_channel", s.update_channel, sizeof(s.update_channel), "stable");

    free(buf);
    return s;
}

bool Save(const AeonSettings& s) {
    char path[MAX_PATH];
    GetSettingsPath(path, sizeof(path));

    FILE* f = nullptr;
    fopen_s(&f, path, "w");
    if (!f) return false;

    fprintf(f, "{\n");
    fprintf(f, "  \"adblock_enabled\": %s,\n",        s.adblock_enabled       ? "true" : "false");
    fprintf(f, "  \"tracker_block_enabled\": %s,\n",  s.tracker_block_enabled ? "true" : "false");
    fprintf(f, "  \"fingerprint_guard\": %s,\n",      s.fingerprint_guard     ? "true" : "false");
    fprintf(f, "  \"gpc_enabled\": %s,\n",            s.gpc_enabled           ? "true" : "false");
    fprintf(f, "  \"cookie_banners\": %s,\n",         s.cookie_banners        ? "true" : "false");
    fprintf(f, "  \"block_3p_cookies\": %s,\n",       s.block_3p_cookies      ? "true" : "false");
    fprintf(f, "  \"doh_enabled\": %s,\n",            s.doh_enabled           ? "true" : "false");
    fprintf(f, "  \"doh_provider\": \"%s\",\n",       s.doh_provider);
    fprintf(f, "  \"https_upgrade\": %s,\n",          s.https_upgrade         ? "true" : "false");
    fprintf(f, "  \"mixed_content_block\": %s,\n",    s.mixed_content_block   ? "true" : "false");
    fprintf(f, "  \"min_tls\": \"%s\",\n",            s.min_tls);
    fprintf(f, "  \"tor_enabled\": %s,\n",            s.tor_enabled           ? "true" : "false");
    fprintf(f, "  \"i2p_enabled\": %s,\n",            s.i2p_enabled           ? "true" : "false");
    fprintf(f, "  \"bypass_enabled\": %s,\n",         s.bypass_enabled        ? "true" : "false");
    fprintf(f, "  \"firewall_mode\": %s,\n",          s.firewall_mode         ? "true" : "false");
    fprintf(f, "  \"tor_socks_addr\": \"%s\",\n",     s.tor_socks_addr);
    fprintf(f, "  \"dl_path\": \"%s\",\n",            s.dl_path);
    fprintf(f, "  \"dl_ask\": %s,\n",                 s.dl_ask                ? "true" : "false");
    fprintf(f, "  \"dl_streams\": %d,\n",             s.dl_streams);
    fprintf(f, "  \"tab_sleep_enabled\": %s,\n",      s.tab_sleep_enabled     ? "true" : "false");
    fprintf(f, "  \"tab_sleep_minutes\": %d,\n",      s.tab_sleep_minutes);
    fprintf(f, "  \"restore_session\": %s,\n",        s.restore_session       ? "true" : "false");
    fprintf(f, "  \"show_bookmarks_bar\": %s,\n",     s.show_bookmarks_bar    ? "true" : "false");
    fprintf(f, "  \"homepage\": \"%s\",\n",           s.homepage);
    fprintf(f, "  \"startup_page\": \"%s\",\n",       s.startup_page);
    fprintf(f, "  \"search_engine\": \"%s\",\n",      s.search_engine);
    fprintf(f, "  \"theme\": \"%c\",\n",              s.theme);
    fprintf(f, "  \"auto_update\": %s,\n",            s.auto_update           ? "true" : "false");
    fprintf(f, "  \"update_channel\": \"%s\"\n",      s.update_channel);
    fprintf(f, "}\n");

    fclose(f);
    return true;
}

DWORD ReadMachineDword(const char* valueName, DWORD defaultVal) {
    HKEY hk;
    if (RegOpenKeyExA(HKEY_LOCAL_MACHINE, REG_KEY, 0, KEY_READ, &hk)
            != ERROR_SUCCESS) return defaultVal;
    DWORD val = defaultVal, sz = sizeof(val);
    RegQueryValueExA(hk, valueName, nullptr, nullptr,
        reinterpret_cast<BYTE*>(&val), &sz);
    RegCloseKey(hk);
    return val;
}

bool WriteMachineDword(const char* valueName, DWORD val) {
    HKEY hk;
    if (RegCreateKeyExA(HKEY_LOCAL_MACHINE, REG_KEY, 0, nullptr,
            REG_OPTION_NON_VOLATILE, KEY_WRITE, nullptr, &hk, nullptr)
            != ERROR_SUCCESS) return false;
    bool ok = (RegSetValueExA(hk, valueName, 0, REG_DWORD,
        reinterpret_cast<const BYTE*>(&val), sizeof(val)) == ERROR_SUCCESS);
    RegCloseKey(hk);
    return ok;
}

} // namespace SettingsEngine
