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
#include <string>
#include <sstream>

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
    // --- Privacy ---
    s.adblock_enabled       = true;
    s.tracker_block_enabled = true;
    s.fingerprint_guard     = true;
    s.gpc_enabled           = true;
    s.cookie_banners        = true;
    s.block_3p_cookies      = true;
    s.doh_enabled           = true;
    strcpy_s(s.doh_provider, "cloudflare");

    // --- Security ---
    s.https_upgrade         = true;
    s.safe_browsing         = true;
    s.cert_transparency     = true;
    s.mixed_content_block   = true;
    strcpy_s(s.min_tls, "tls13");

    // --- Network / Protocols ---
    s.tor_enabled           = false;
    s.i2p_enabled           = false;
    s.bypass_enabled        = false;
    s.firewall_mode         = false;
    strcpy_s(s.tor_socks_addr, "127.0.0.1:9150");
    strcpy_s(s.i2p_http_proxy, "127.0.0.1:4444");
    s.auto_detect_proxy     = true;
    s.ss_uri[0]             = '\0';

    // --- 2026 Circumvention & DPI Engine (R2) ---
    s.byedpi_enabled        = false;
    strcpy_s(s.byedpi_socks_addr, "127.0.0.1:1080");
    s.goodbyedpi_enabled    = false;
    s.goodbyedpi_mode       = 1;
    s.ech_enabled           = true;
    s.kyber_enabled         = true;

    // --- Camoufox C++ Stealth & Privacy Sentinel (R2) ---
    s.camoufox_enabled      = true;
    s.canvas_noise_enabled  = true;
    s.webgl_noise_enabled   = true;
    s.audio_noise_enabled   = true;
    strcpy_s(s.tracker_blocking_mode, "strict");

    // --- Modern Appearance & Chrome Controls (R2) ---
    strcpy_s(s.theme_preset, "obsidian");
    s.vertical_tabs         = false;
    s.theme                 = 'A';

    // --- Aeon Intelligence & AI Provider (R2) ---
    strcpy_s(s.ai_provider, "sovereign");
    s.ai_api_key[0]         = '\0';
    strcpy_s(s.ai_model,    "gemma-4-e2b");
    strcpy_s(s.ai_endpoint, "http://localhost:11434");
    s.agent_pipe_enabled    = true;
    s.ai_temperature        = 0.7;

    // --- Downloads: dl_path: %USERPROFILE%\Downloads ---
    char userProfile[MAX_PATH] = {};
    SHGetFolderPathA(nullptr, CSIDL_PROFILE, nullptr, 0, userProfile);
    _snprintf_s(s.dl_path, sizeof(s.dl_path), _TRUNCATE, "%s\\Downloads", userProfile);
    CreateDirectoryA(s.dl_path, nullptr);

    s.dl_ask                = false;
    s.auto_open_after       = false;
    s.dl_streams            = 4;

    // --- UI / Features ---
    s.tab_sleep_enabled     = true;
    s.tab_sleep_minutes     = 30;
    s.restore_session       = true;
    s.show_bookmarks_bar    = true;
    s.smooth_scrolling      = true;
    strcpy_s(s.homepage,      "aeon://newtab");
    strcpy_s(s.startup_page,  "newtab");
    strcpy_s(s.search_engine, "ddg");
    s.hardware_accel        = true;

    // --- Updates ---
    s.auto_update           = true;
    strcpy_s(s.update_channel, "stable");

    // --- Telemetry ---
    DWORD tele = ReadMachineDword("TelemetryEnabled", 1);
    s.telemetry_enabled = (tele != 0);

    return s;
}

// ---------------------------------------------------------------------------
// Minimal JSON helpers (no regex — simple key:value scanner)
// ---------------------------------------------------------------------------
static bool ReadJsonBool(const char* json, const char* key, bool def) {
    if (!json || !key) return def;
    char pattern[128];
    _snprintf_s(pattern, sizeof(pattern), _TRUNCATE, "\"%s\"", key);
    const char* p = strstr(json, pattern);
    if (!p) return def;

    p += strlen(pattern);
    while (*p && (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n')) p++;
    if (*p != ':') return def;
    p++;
    while (*p && (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n')) p++;
    if (strncmp(p, "true",  4) == 0) return true;
    if (strncmp(p, "false", 5) == 0) return false;
    if (*p == '1') return true;
    if (*p == '0') return false;
    return def;
}

static int ReadJsonInt(const char* json, const char* key, int def) {
    if (!json || !key) return def;
    char pattern[128];
    _snprintf_s(pattern, sizeof(pattern), _TRUNCATE, "\"%s\"", key);
    const char* p = strstr(json, pattern);
    if (!p) return def;

    p += strlen(pattern);
    while (*p && (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n')) p++;
    if (*p != ':') return def;
    p++;
    while (*p && (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n')) p++;
    if (*p == '"') p++;
    if ((*p >= '0' && *p <= '9') || *p == '-') return atoi(p);
    return def;
}

static double ReadJsonDouble(const char* json, const char* key, double def) {
    if (!json || !key) return def;
    char pattern[128];
    _snprintf_s(pattern, sizeof(pattern), _TRUNCATE, "\"%s\"", key);
    const char* p = strstr(json, pattern);
    if (!p) return def;

    p += strlen(pattern);
    while (*p && (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n')) p++;
    if (*p != ':') return def;
    p++;
    while (*p && (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n')) p++;
    if (*p == '"') p++;
    if ((*p >= '0' && *p <= '9') || *p == '-' || *p == '.') return atof(p);
    return def;
}

static void ReadJsonStr(const char* json, const char* key,
                        char* out, size_t out_len, const char* def) {
    if (!out || out_len == 0) return;
    if (!json || !key) { strncpy_s(out, out_len, def ? def : "", _TRUNCATE); return; }

    char pattern[128];
    _snprintf_s(pattern, sizeof(pattern), _TRUNCATE, "\"%s\"", key);
    const char* p = strstr(json, pattern);
    if (!p) { strncpy_s(out, out_len, def ? def : "", _TRUNCATE); return; }

    p += strlen(pattern);
    while (*p && (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n')) p++;
    if (*p != ':') { strncpy_s(out, out_len, def ? def : "", _TRUNCATE); return; }
    p++;
    while (*p && (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n')) p++;
    if (*p != '"') { strncpy_s(out, out_len, def ? def : "", _TRUNCATE); return; }
    p++; // skip opening quote

    size_t i = 0;
    while (*p && *p != '"' && i < out_len - 1) {
        if (*p == '\\' && *(p + 1)) {
            p++;
            switch (*p) {
                case 'n': out[i++] = '\n'; break;
                case 'r': out[i++] = '\r'; break;
                case 't': out[i++] = '\t'; break;
                case '\\': out[i++] = '\\'; break;
                case '"': out[i++] = '"'; break;
                default: out[i++] = *p; break;
            }
        } else {
            out[i++] = *p;
        }
        p++;
    }
    out[i] = '\0';
}

static std::string EscapeJsonString(const char* s) {
    if (!s) return "";
    std::string out;
    out.reserve(strlen(s) + 16);
    for (const char* p = s; *p; ++p) {
        switch (*p) {
            case '\\': out += "\\\\"; break;
            case '"':  out += "\\\""; break;
            case '\n': out += "\\n";  break;
            case '\r': out += "\\r";  break;
            case '\t': out += "\\t";  break;
            default:   out += *p;     break;
        }
    }
    return out;
}

std::string ToJson(const AeonSettings& s) {
    std::ostringstream ss;
    ss << "{\n";
    // Privacy
    ss << "  \"adblock_enabled\": " << (s.adblock_enabled ? "true" : "false") << ",\n";
    ss << "  \"tracker_block_enabled\": " << (s.tracker_block_enabled ? "true" : "false") << ",\n";
    ss << "  \"fingerprint_guard\": " << (s.fingerprint_guard ? "true" : "false") << ",\n";
    ss << "  \"gpc_enabled\": " << (s.gpc_enabled ? "true" : "false") << ",\n";
    ss << "  \"cookie_banners\": " << (s.cookie_banners ? "true" : "false") << ",\n";
    ss << "  \"block_3p_cookies\": " << (s.block_3p_cookies ? "true" : "false") << ",\n";
    ss << "  \"doh_enabled\": " << (s.doh_enabled ? "true" : "false") << ",\n";
    ss << "  \"doh_provider\": \"" << EscapeJsonString(s.doh_provider) << "\",\n";

    // Security
    ss << "  \"https_upgrade\": " << (s.https_upgrade ? "true" : "false") << ",\n";
    ss << "  \"safe_browsing\": " << (s.safe_browsing ? "true" : "false") << ",\n";
    ss << "  \"cert_transparency\": " << (s.cert_transparency ? "true" : "false") << ",\n";
    ss << "  \"mixed_content_block\": " << (s.mixed_content_block ? "true" : "false") << ",\n";
    ss << "  \"min_tls\": \"" << EscapeJsonString(s.min_tls) << "\",\n";

    // Network / Protocols
    ss << "  \"tor_enabled\": " << (s.tor_enabled ? "true" : "false") << ",\n";
    ss << "  \"i2p_enabled\": " << (s.i2p_enabled ? "true" : "false") << ",\n";
    ss << "  \"bypass_enabled\": " << (s.bypass_enabled ? "true" : "false") << ",\n";
    ss << "  \"firewall_mode\": " << (s.firewall_mode ? "true" : "false") << ",\n";
    ss << "  \"tor_socks_addr\": \"" << EscapeJsonString(s.tor_socks_addr) << "\",\n";
    ss << "  \"i2p_http_proxy\": \"" << EscapeJsonString(s.i2p_http_proxy) << "\",\n";
    ss << "  \"auto_detect_proxy\": " << (s.auto_detect_proxy ? "true" : "false") << ",\n";
    ss << "  \"ss_uri\": \"" << EscapeJsonString(s.ss_uri) << "\",\n";

    // 2026 Circumvention & DPI Engine (R2)
    ss << "  \"byedpi_enabled\": " << (s.byedpi_enabled ? "true" : "false") << ",\n";
    ss << "  \"byedpi_socks_addr\": \"" << EscapeJsonString(s.byedpi_socks_addr) << "\",\n";
    ss << "  \"goodbyedpi_enabled\": " << (s.goodbyedpi_enabled ? "true" : "false") << ",\n";
    ss << "  \"goodbyedpi_mode\": " << s.goodbyedpi_mode << ",\n";
    ss << "  \"ech_enabled\": " << (s.ech_enabled ? "true" : "false") << ",\n";
    ss << "  \"kyber_enabled\": " << (s.kyber_enabled ? "true" : "false") << ",\n";

    // Camoufox C++ Stealth & Privacy Sentinel (R2)
    ss << "  \"camoufox_enabled\": " << (s.camoufox_enabled ? "true" : "false") << ",\n";
    ss << "  \"canvas_noise_enabled\": " << (s.canvas_noise_enabled ? "true" : "false") << ",\n";
    ss << "  \"webgl_noise_enabled\": " << (s.webgl_noise_enabled ? "true" : "false") << ",\n";
    ss << "  \"audio_noise_enabled\": " << (s.audio_noise_enabled ? "true" : "false") << ",\n";
    ss << "  \"tracker_blocking_mode\": \"" << EscapeJsonString(s.tracker_blocking_mode) << "\",\n";

    // Modern Appearance & Chrome Controls (R2)
    ss << "  \"theme_preset\": \"" << EscapeJsonString(s.theme_preset) << "\",\n";
    ss << "  \"vertical_tabs\": " << (s.vertical_tabs ? "true" : "false") << ",\n";
    ss << "  \"theme\": \"" << s.theme << "\",\n";

    // AI Provider & Intelligence (R2)
    ss << "  \"ai_provider\": \"" << EscapeJsonString(s.ai_provider) << "\",\n";
    ss << "  \"ai_api_key\": \"" << EscapeJsonString(s.ai_api_key) << "\",\n";
    ss << "  \"ai_model\": \"" << EscapeJsonString(s.ai_model) << "\",\n";
    ss << "  \"ai_endpoint\": \"" << EscapeJsonString(s.ai_endpoint) << "\",\n";
    ss << "  \"agent_pipe_enabled\": " << (s.agent_pipe_enabled ? "true" : "false") << ",\n";
    ss << "  \"ai_temperature\": " << s.ai_temperature << ",\n";

    // Downloads
    ss << "  \"dl_path\": \"" << EscapeJsonString(s.dl_path) << "\",\n";
    ss << "  \"dl_ask\": " << (s.dl_ask ? "true" : "false") << ",\n";
    ss << "  \"auto_open_after\": " << (s.auto_open_after ? "true" : "false") << ",\n";
    ss << "  \"dl_streams\": " << s.dl_streams << ",\n";

    // UI / Features
    ss << "  \"tab_sleep_enabled\": " << (s.tab_sleep_enabled ? "true" : "false") << ",\n";
    ss << "  \"tab_sleep_minutes\": " << s.tab_sleep_minutes << ",\n";
    ss << "  \"restore_session\": " << (s.restore_session ? "true" : "false") << ",\n";
    ss << "  \"show_bookmarks_bar\": " << (s.show_bookmarks_bar ? "true" : "false") << ",\n";
    ss << "  \"smooth_scrolling\": " << (s.smooth_scrolling ? "true" : "false") << ",\n";
    ss << "  \"homepage\": \"" << EscapeJsonString(s.homepage) << "\",\n";
    ss << "  \"startup_page\": \"" << EscapeJsonString(s.startup_page) << "\",\n";
    ss << "  \"search_engine\": \"" << EscapeJsonString(s.search_engine) << "\",\n";
    ss << "  \"hardware_accel\": " << (s.hardware_accel ? "true" : "false") << ",\n";

    // Updates & Telemetry
    ss << "  \"auto_update\": " << (s.auto_update ? "true" : "false") << ",\n";
    ss << "  \"update_channel\": \"" << EscapeJsonString(s.update_channel) << "\",\n";
    ss << "  \"telemetry_enabled\": " << (s.telemetry_enabled ? "true" : "false") << "\n";
    ss << "}\n";

    return ss.str();
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
    if (sz <= 0 || sz > 131072) { fclose(f); return Defaults(); }

    char* buf = static_cast<char*>(malloc(sz + 1));
    if (!buf) { fclose(f); return Defaults(); }
    fread(buf, 1, sz, f);
    buf[sz] = '\0';
    fclose(f);

    AeonSettings s = Defaults();

    // Privacy
    s.adblock_enabled        = ReadJsonBool(buf, "adblock_enabled",       true);
    s.tracker_block_enabled  = ReadJsonBool(buf, "tracker_block_enabled", true);
    s.fingerprint_guard      = ReadJsonBool(buf, "fingerprint_guard",     true);
    s.gpc_enabled            = ReadJsonBool(buf, "gpc_enabled",           true);
    s.cookie_banners         = ReadJsonBool(buf, "cookie_banners",        true);
    s.block_3p_cookies       = ReadJsonBool(buf, "block_3p_cookies",      true);
    s.doh_enabled            = ReadJsonBool(buf, "doh_enabled",           true);
    ReadJsonStr(buf, "doh_provider", s.doh_provider, sizeof(s.doh_provider), "cloudflare");

    // Security
    s.https_upgrade          = ReadJsonBool(buf, "https_upgrade",         true);
    s.safe_browsing          = ReadJsonBool(buf, "safe_browsing",         true);
    s.cert_transparency      = ReadJsonBool(buf, "cert_transparency",     true);
    s.mixed_content_block    = ReadJsonBool(buf, "mixed_content_block",   true);
    ReadJsonStr(buf, "min_tls", s.min_tls, sizeof(s.min_tls), "tls13");

    // Network / Protocols
    s.tor_enabled            = ReadJsonBool(buf, "tor_enabled",           false);
    s.i2p_enabled            = ReadJsonBool(buf, "i2p_enabled",           false);
    s.bypass_enabled         = ReadJsonBool(buf, "bypass_enabled",        false);
    s.firewall_mode          = ReadJsonBool(buf, "firewall_mode",         false);
    ReadJsonStr(buf, "tor_socks_addr", s.tor_socks_addr, sizeof(s.tor_socks_addr), "127.0.0.1:9150");
    ReadJsonStr(buf, "i2p_http_proxy", s.i2p_http_proxy, sizeof(s.i2p_http_proxy), "127.0.0.1:4444");
    s.auto_detect_proxy      = ReadJsonBool(buf, "auto_detect_proxy",     true);
    ReadJsonStr(buf, "ss_uri", s.ss_uri, sizeof(s.ss_uri), "");

    // 2026 Circumvention & DPI Engine (R2)
    s.byedpi_enabled         = ReadJsonBool(buf, "byedpi_enabled",        false);
    ReadJsonStr(buf, "byedpi_socks_addr", s.byedpi_socks_addr, sizeof(s.byedpi_socks_addr), "127.0.0.1:1080");
    s.goodbyedpi_enabled     = ReadJsonBool(buf, "goodbyedpi_enabled",    false);
    s.goodbyedpi_mode        = ReadJsonInt(buf,  "goodbyedpi_mode",       1);
    s.ech_enabled            = ReadJsonBool(buf, "ech_enabled",           true);
    s.kyber_enabled          = ReadJsonBool(buf, "kyber_enabled",         true);

    // Camoufox C++ Stealth & Privacy Sentinel (R2)
    s.camoufox_enabled       = ReadJsonBool(buf, "camoufox_enabled",      true);
    s.canvas_noise_enabled   = ReadJsonBool(buf, "canvas_noise_enabled",  true);
    s.webgl_noise_enabled    = ReadJsonBool(buf, "webgl_noise_enabled",   true);
    s.audio_noise_enabled    = ReadJsonBool(buf, "audio_noise_enabled",   true);
    ReadJsonStr(buf, "tracker_blocking_mode", s.tracker_blocking_mode, sizeof(s.tracker_blocking_mode), "strict");

    // Modern Appearance & Chrome Controls (R2)
    ReadJsonStr(buf, "theme_preset", s.theme_preset, sizeof(s.theme_preset), "obsidian");
    s.vertical_tabs          = ReadJsonBool(buf, "vertical_tabs",         false);
    char themeStr[4] = {"A"};
    ReadJsonStr(buf, "theme", themeStr, sizeof(themeStr), "A");
    s.theme = themeStr[0];

    // AI Provider & Intelligence (R2)
    ReadJsonStr(buf, "ai_provider", s.ai_provider, sizeof(s.ai_provider), "sovereign");
    ReadJsonStr(buf, "ai_api_key", s.ai_api_key, sizeof(s.ai_api_key), "");
    ReadJsonStr(buf, "ai_model", s.ai_model, sizeof(s.ai_model), "gemma-4-e2b");
    ReadJsonStr(buf, "ai_endpoint", s.ai_endpoint, sizeof(s.ai_endpoint), "http://localhost:11434");
    s.agent_pipe_enabled     = ReadJsonBool(buf, "agent_pipe_enabled",    true);
    s.ai_temperature        = ReadJsonDouble(buf, "ai_temperature",      0.7);

    // Downloads
    ReadJsonStr(buf, "dl_path", s.dl_path, sizeof(s.dl_path), s.dl_path);
    s.dl_ask                 = ReadJsonBool(buf, "dl_ask",                false);
    s.auto_open_after        = ReadJsonBool(buf, "auto_open_after",       false);
    s.dl_streams             = ReadJsonInt(buf,  "dl_streams",            4);

    // UI / Features
    s.tab_sleep_enabled      = ReadJsonBool(buf, "tab_sleep_enabled",     true);
    s.tab_sleep_minutes      = ReadJsonInt(buf,  "tab_sleep_minutes",     30);
    s.restore_session        = ReadJsonBool(buf, "restore_session",       true);
    s.show_bookmarks_bar     = ReadJsonBool(buf, "show_bookmarks_bar",    true);
    s.smooth_scrolling       = ReadJsonBool(buf, "smooth_scrolling",      true);
    ReadJsonStr(buf, "homepage",       s.homepage,      sizeof(s.homepage),      "aeon://newtab");
    ReadJsonStr(buf, "startup_page",   s.startup_page,  sizeof(s.startup_page),  "newtab");
    ReadJsonStr(buf, "search_engine",  s.search_engine, sizeof(s.search_engine), "ddg");
    s.hardware_accel         = ReadJsonBool(buf, "hardware_accel",        true);

    // Updates & Telemetry
    s.auto_update            = ReadJsonBool(buf, "auto_update",           true);
    ReadJsonStr(buf, "update_channel", s.update_channel, sizeof(s.update_channel), "stable");
    s.telemetry_enabled      = ReadJsonBool(buf, "telemetry_enabled",     true);

    free(buf);
    return s;
}

bool Save(const AeonSettings& s) {
    char path[MAX_PATH];
    GetSettingsPath(path, sizeof(path));

    FILE* f = nullptr;
    fopen_s(&f, path, "w");
    if (!f) return false;

    std::string json = ToJson(s);
    fwrite(json.data(), 1, json.size(), f);
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
