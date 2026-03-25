// AeonBrowser — SettingsEngine.h + SettingsEngine.cpp
// DelgadoLogic | Lead Systems Architect
//
// PURPOSE: Unified settings store. All Aeon settings live in ONE JSON file
// (%APPDATA%\DelgadoLogic\Aeon\settings.json) with registry shadow for
// per-machine values (install path, telemetry key, same as LogicFlow).
//
// DESIGN: Two-tier storage:
//   User settings  → %APPDATA%\DelgadoLogic\Aeon\settings.json
//                    (per-user, survives app update)
//   Machine values → HKLM\SOFTWARE\DelgadoLogic\Aeon
//                    (admin-set: TelemetryEnabled, ForceTier, etc.)
//
// NO JSON LIBRARY DEPENDENCY: We implement a minimal JSON writer/reader
// (~200 lines) to avoid adding nlohmann/json or rapidjson and their CVEs.
//
// IT TROUBLESHOOTING:
//   - Settings not persisting: Check %APPDATA%\DelgadoLogic\Aeon\ exists.
//   - Machine setting not applying: Must be set as HKLM DWORD (requires admin).
//   - "ForceTier" not working: See TierDispatcher.h — DWORD 0-7.
//   - Reset to defaults: Delete settings.json. Registry machine values persist.

#pragma once

#include <windows.h>
#include <cstdint>
#include <cstddef>

// ---------------------------------------------------------------------------
// Settings schema — all user-configurable settings in one struct.
// Defaults represent the best experience on each tier.
// ---------------------------------------------------------------------------
struct AeonSettings {
    // --- Privacy ---
    bool  adblock_enabled;          // default: true
    bool  tracker_block_enabled;    // default: true
    bool  fingerprint_guard;        // default: true
    bool  gpc_enabled;              // default: true  (GPC opt-out header)
    bool  cookie_banners;           // default: true  (block cookie consent overlays)
    bool  block_3p_cookies;         // default: true
    bool  doh_enabled;              // default: true
    char  doh_provider[64];         // default: "cloudflare"

    // --- Security ---
    bool  https_upgrade;            // default: true
    bool  safe_browsing;            // default: true
    bool  cert_transparency;        // default: true
    bool  mixed_content_block;      // default: true
    char  min_tls[8];               // default: "tls13"

    // --- Network / Protocols ---
    bool  tor_enabled;              // default: false
    bool  i2p_enabled;              // default: false
    bool  bypass_enabled;           // default: false (DPI bypass / GoodbyeDPI)
    char  tor_socks_addr[64];       // default: "127.0.0.1:9150"
    char  i2p_http_proxy[64];       // default: "127.0.0.1:4444"
    bool  auto_detect_proxy;        // default: true

    // --- Firewall bypass (legacy field — mirrors bypass_enabled) ---
    bool  firewall_mode;            // default: false
    char  ss_uri[256];              // Shadowsocks URI (used by CircumventionEngine)

    // --- Downloads ---
    char  dl_path[MAX_PATH];        // default: %USERPROFILE%\Downloads
    bool  dl_ask;                   // default: false (auto-save to dl_path)
    bool  auto_open_after;          // default: false
    int   dl_streams;               // default: 4  (parallel connections per file)

    // --- UI / Features ---
    bool  tab_sleep_enabled;        // default: true
    int   tab_sleep_minutes;        // default: 30
    bool  restore_session;          // default: true
    bool  show_bookmarks_bar;       // default: true
    bool  smooth_scrolling;         // default: true
    char  homepage[512];            // default: "aeon://newtab"
    char  startup_page[32];         // default: "newtab"
    char  search_engine[64];        // default: "ddg"
    char  theme;                    // 'A'=auto  'D'=dark  'L'=light  'R'=retro
    bool  hardware_accel;           // default: true

    // --- Updates ---
    bool  auto_update;              // default: true
    char  update_channel[16];       // "stable" | "beta" | "nightly"

    // --- Telemetry ---
    bool  telemetry_enabled;        // mirrors HKLM key — UI read-only hint
};

namespace SettingsEngine {

    // Load settings from JSON file. Returns defaults if file not found.
    AeonSettings Load();

    // Save current settings to JSON file.
    bool Save(const AeonSettings& s);

    // Reset to factory defaults.
    AeonSettings Defaults();

    // Read a single machine-level registry DWORD (e.g., ForceTier, TelemetryEnabled).
    // Returns defaultVal if key not present.
    DWORD ReadMachineDword(const char* valueName, DWORD defaultVal);

    // Write a single machine-level registry DWORD (requires admin).
    bool  WriteMachineDword(const char* valueName, DWORD val);

    // Return path to settings JSON file.
    void  GetSettingsPath(char* buf, size_t buf_len);
}
