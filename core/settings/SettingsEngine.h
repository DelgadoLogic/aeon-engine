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
    bool  gpc_header_enabled;       // default: true
    bool  doh_enabled;              // default: true
    char  doh_provider[64];         // default: "https://1.1.1.1/dns-query"

    // --- Security ---
    bool  https_upgrade;            // default: true  (auto-upgrade http→https)
    bool  safe_browsing;            // default: true
    bool  cert_transparency;        // default: true

    // --- Network / Protocols ---
    bool  tor_enabled;              // default: false (off unless user enables)
    bool  i2p_enabled;              // default: false
    char  tor_socks_addr[64];       // default: "127.0.0.1:9150"
    char  i2p_http_proxy[64];       // default: "127.0.0.1:4444"
    bool  auto_detect_proxy;        // default: true

    // --- Downloads ---
    char  download_dir[MAX_PATH];   // default: %USERPROFILE%\Downloads\Aeon
    bool  ask_per_download;         // default: true
    bool  auto_open_after;          // default: false
    int   max_parallel_downloads;   // default: 4

    // --- UI / Features ---
    bool  tab_sleep_enabled;        // default: true
    int   tab_sleep_minutes;        // default: 30
    bool  restore_session;          // default: true
    bool  show_bookmarks_bar;       // default: true
    bool  smooth_scrolling;         // default: true
    char  homepage[512];            // default: "aeon://newtab"
    char  search_engine[64];        // default: "https://search.brave.com/search?q="
    char  theme;                    // 'A'=auto  'D'=dark  'L'=light  'R'=retro
    bool  hardware_accel;           // default: true (false auto-set on retro tier)

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
