// AeonBrowser — test_settings_bridge_standalone.cpp
// DelgadoLogic | Settings Engine & C++ Bridge Dedicated Verification Test

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <shlobj.h>
#include <iostream>
#include <string>
#include <vector>
#include <cassert>
#include <cstdio>
#include <cstring>
#include <functional>

#include "../settings/SettingsEngine.h"
#include "../engine/AeonBridge.h"
#include "../history/HistoryEngine.h"
#include "../security/PasswordVault.h"
#include "../download/DownloadManager.h"
#include "../agent/AeonAgentPipe.h"
#include "../network/NetworkSentinel.h"
#include "../network/DnsResolver.h"
#include "../session/SessionManager.h"
#include "../engine/TierDispatcher.h"
#include "../ui/BrowserChrome.h"
#include "../../updater/AutoUpdater.h"

// ── Provide implementations for external subsystems ───────────────────────────
namespace DnsResolver {
    bool Initialize() { return true; }
}

namespace NetworkSentinel {
    static NetworkEnvironment s_env = {};
    static SentinelState s_state = {};
    const NetworkEnvironment& GetEnvironment() { return s_env; }
    const SentinelState& GetState() { return s_state; }
    void ApplyBestStrategy() {}
}

namespace AeonAgentPipe {
    static bool s_mockRunning = false;
    bool Start(HWND) { s_mockRunning = true; return true; }
    void Stop() { s_mockRunning = false; }
    bool IsRunning() { return s_mockRunning; }
}

namespace BrowserChrome {
    int GetActiveTabIndex(HWND) { return 0; }
    int GetTabCount(HWND) { return 1; }
    unsigned int CreateTab(HWND, const char*) { return 1; }
    bool FocusTabById(HWND, unsigned int) { return true; }
    bool GetTabInfo(HWND, int, unsigned int* id, char* url, int urlLen, char* title, int titleLen, bool* active) {
        if (id) *id = 1;
        if (url && urlLen > 0) strncpy_s(url, urlLen, "https://delgadologic.tech", _TRUNCATE);
        if (title && titleLen > 0) strncpy_s(title, titleLen, "DelgadoLogic", _TRUNCATE);
        if (active) *active = true;
        return true;
    }
}

namespace DownloadManager {
    static std::vector<DownloadItem> s_dl;
    void Pause(int64_t) {}
    void Resume(int64_t) {}
    void Cancel(int64_t) {}
    void Remove(int64_t id) {
        for (auto it = s_dl.begin(); it != s_dl.end(); ++it) {
            if ((int64_t)it->id == id) { s_dl.erase(it); break; }
        }
    }
    void RemoveDownload(int64_t id) { Remove(id); }
    void ClearCompleted() { s_dl.clear(); }
    void RevealInExplorer(int64_t) {}
    void OpenDownloadFolder() {}
    void OpenFile(int64_t) {}
    std::vector<DownloadItem> GetAll() { return s_dl; }
    std::vector<DownloadItem> GetAllDownloads() { return s_dl; }
}

TierDispatcher& TierDispatcher::GetInstance() {
    static SystemProfile sp = {};
    static TierDispatcher td(sp, nullptr);
    return td;
}
TierDispatcher::TierDispatcher(const SystemProfile& p, HINSTANCE hInst) : m_Profile(p), m_hInst(hInst) {}
TierDispatcher::~TierDispatcher() {}
bool TierDispatcher::IsProtectedDomain(const char*) { return false; }
AeonEngineVTable* TierDispatcher::GetEngineForUrl(const char*) { return nullptr; }

int main() {
    std::cout << "=== Running AeonBrowser Milestone 2 & 5 Backend Engine & Bridge Tests ===" << std::endl;
    int passed = 0;

    // ─────────────────────────────────────────────────────────────
    // 1. SettingsEngine Defaults Verification
    // ─────────────────────────────────────────────────────────────
    AeonSettings def = SettingsEngine::Defaults();

    // AI Provider defaults
    assert(std::string(def.ai_provider) == "sovereign");
    assert(std::string(def.ai_api_key) == "");
    assert(std::string(def.ai_model) == "gemma-4-e2b");
    assert(std::string(def.ai_endpoint) == "http://localhost:11434");
    assert(def.agent_pipe_enabled == true);

    // 2026 Circumvention & DPI defaults
    assert(def.byedpi_enabled == false);
    assert(std::string(def.byedpi_socks_addr) == "127.0.0.1:1080");
    assert(def.goodbyedpi_enabled == false);
    assert(def.goodbyedpi_mode == 1);
    assert(def.ech_enabled == true);
    assert(def.kyber_enabled == true);

    // Camoufox C++ Stealth & Privacy Sentinel defaults
    assert(def.camoufox_enabled == true);
    assert(def.canvas_noise_enabled == true);
    assert(def.webgl_noise_enabled == true);
    assert(def.audio_noise_enabled == true);
    assert(std::string(def.tracker_blocking_mode) == "strict");

    // Modern Appearance defaults
    assert(std::string(def.theme_preset) == "obsidian");
    assert(def.vertical_tabs == false);

    std::cout << "[PASS] Test 1: SettingsEngine::Defaults() initializes all R2 settings with valid defaults" << std::endl;
    passed++;

    // ─────────────────────────────────────────────────────────────
    // 2. SettingsEngine Serialization & ToJson Round-Trip
    // ─────────────────────────────────────────────────────────────
    AeonSettings custom = def;
    strcpy_s(custom.ai_provider, "deepseek");
    strcpy_s(custom.ai_api_key, "sk-test-key-2026");
    strcpy_s(custom.ai_model, "deepseek-r1");
    strcpy_s(custom.ai_endpoint, "https://api.deepseek.com/v1");
    custom.agent_pipe_enabled = true;
    custom.byedpi_enabled = true;
    strcpy_s(custom.byedpi_socks_addr, "127.0.0.1:9050");
    custom.goodbyedpi_enabled = true;
    custom.goodbyedpi_mode = 5;
    custom.ech_enabled = true;
    custom.kyber_enabled = true;
    custom.camoufox_enabled = true;
    custom.canvas_noise_enabled = true;
    custom.webgl_noise_enabled = true;
    custom.audio_noise_enabled = true;
    strcpy_s(custom.tracker_blocking_mode, "aggressive");
    strcpy_s(custom.theme_preset, "cyber_neon");
    custom.vertical_tabs = true;

    // Test ToJson
    std::string json = SettingsEngine::ToJson(custom);
    assert(json.find("\"ai_provider\": \"deepseek\"") != std::string::npos);
    assert(json.find("\"ai_api_key\": \"sk-test-key-2026\"") != std::string::npos);
    assert(json.find("\"goodbyedpi_mode\": 5") != std::string::npos);
    assert(json.find("\"theme_preset\": \"cyber_neon\"") != std::string::npos);
    assert(json.find("\"vertical_tabs\": true") != std::string::npos);

    // Test Save & Load
    bool saved = SettingsEngine::Save(custom);
    assert(saved == true);

    AeonSettings loaded = SettingsEngine::Load();
    assert(std::string(loaded.ai_provider) == "deepseek");
    assert(std::string(loaded.ai_api_key) == "sk-test-key-2026");
    assert(std::string(loaded.ai_model) == "deepseek-r1");
    assert(std::string(loaded.ai_endpoint) == "https://api.deepseek.com/v1");
    assert(loaded.agent_pipe_enabled == true);
    assert(loaded.byedpi_enabled == true);
    assert(std::string(loaded.byedpi_socks_addr) == "127.0.0.1:9050");
    assert(loaded.goodbyedpi_enabled == true);
    assert(loaded.goodbyedpi_mode == 5);
    assert(loaded.ech_enabled == true);
    assert(loaded.kyber_enabled == true);
    assert(loaded.camoufox_enabled == true);
    assert(loaded.canvas_noise_enabled == true);
    assert(loaded.webgl_noise_enabled == true);
    assert(loaded.audio_noise_enabled == true);
    assert(std::string(loaded.tracker_blocking_mode) == "aggressive");
    assert(std::string(loaded.theme_preset) == "cyber_neon");
    assert(loaded.vertical_tabs == true);

    std::cout << "[PASS] Test 2: SettingsEngine::Save() & Load() correctly preserve all R2 settings" << std::endl;
    passed++;

    // ─────────────────────────────────────────────────────────────
    // 3. AeonBridge Settings Dispatch
    // ─────────────────────────────────────────────────────────────
    std::string aiProv = AeonBridge::Dispatch("getSetting", "\"ai_provider\"");
    assert(aiProv == "\"deepseek\"");

    AeonBridge::Dispatch("setSetting", "[\"ai_provider\", \"groq\"]");
    aiProv = AeonBridge::Dispatch("getSetting", "\"ai_provider\"");
    assert(aiProv == "\"groq\"");

    std::string vTabs = AeonBridge::Dispatch("getSetting", "\"vertical_tabs\"");
    assert(vTabs == "true");

    AeonBridge::Dispatch("setSetting", "[\"vertical_tabs\", false]");
    vTabs = AeonBridge::Dispatch("getSetting", "\"vertical_tabs\"");
    assert(vTabs == "false");

    std::string commitRes = AeonBridge::Dispatch("commitSettings", "");
    assert(commitRes == "{\"ok\":true}");

    std::cout << "[PASS] Test 3: AeonBridge::Dispatch() getSetting, setSetting, commitSettings succeed" << std::endl;
    passed++;

    // ─────────────────────────────────────────────────────────────
    // 4. AeonBridge AI Settings API
    // ─────────────────────────────────────────────────────────────
    std::string aiSettingsJson = AeonBridge::Dispatch("getAiSettings", "");
    assert(aiSettingsJson.find("\"provider\":\"groq\"") != std::string::npos);
    assert(aiSettingsJson.find("\"agent_pipe_enabled\":true") != std::string::npos);

    AeonBridge::Dispatch("setAiSetting", "[\"provider\", \"gemini\"]");
    aiSettingsJson = AeonBridge::Dispatch("getAiSettings", "");
    assert(aiSettingsJson.find("\"provider\":\"gemini\"") != std::string::npos);

    std::cout << "[PASS] Test 4: AeonBridge::Dispatch() getAiSettings & setAiSetting succeed" << std::endl;
    passed++;

    // ─────────────────────────────────────────────────────────────
    // 5. AeonBridge Password Vault & revealPassword API
    // ─────────────────────────────────────────────────────────────
    char appData[MAX_PATH] = {};
    SHGetFolderPathA(nullptr, CSIDL_APPDATA, nullptr, 0, appData);
    char vaultPath[MAX_PATH];
    _snprintf_s(vaultPath, sizeof(vaultPath), _TRUNCATE, "%s\\DelgadoLogic\\Aeon\\vault_test.db", appData);
    PasswordVault::Init(vaultPath);

    AeonBridge::Dispatch("addPassword", "[\"https://delgadologic.tech\",\"manuel\",\"ObsidianVault2026!\"]");

    std::string pwList = AeonBridge::Dispatch("getPasswords", "");
    assert(pwList.find("delgadologic.tech") != std::string::npos);

    std::string revealed = AeonBridge::Dispatch("revealPassword", "0");
    assert(revealed == "\"ObsidianVault2026!\"");

    std::cout << "[PASS] Test 5: AeonBridge::Dispatch() revealPassword decrypts and returns plaintext credential" << std::endl;
    passed++;

    // ─────────────────────────────────────────────────────────────
    // 6. AeonBridge Bookmarks Import / Export API
    // ─────────────────────────────────────────────────────────────
    char histPath[MAX_PATH];
    _snprintf_s(histPath, sizeof(histPath), _TRUNCATE, "%s\\DelgadoLogic\\Aeon\\history_test.db", appData);
    HistoryEngine::Init(histPath);

    // Import Netscape HTML bookmarks
    const char* netscapeHtml =
        "<!DOCTYPE NETSCAPE-Bookmark-file-1>\n"
        "<META HTTP-EQUIV=\"Content-Type\" CONTENT=\"text/html; charset=UTF-8\">\n"
        "<TITLE>Bookmarks</TITLE>\n"
        "<H1>Bookmarks</H1>\n"
        "<DL><p>\n"
        "    <DT><H3>Engineering</H3>\n"
        "    <DL><p>\n"
        "        <DT><A HREF=\"https://github.com/DelgadoLogic/AeonBrowser\">Aeon Browser GitHub</A>\n"
        "        <DT><A HREF=\"https://delgadologic.tech\">DelgadoLogic Tech</A>\n"
        "    </DL><p>\n"
        "</DL><p>\n";

    std::string importRes = AeonBridge::Dispatch("importBookmarks", netscapeHtml);
    assert(importRes.find("\"ok\":true") != std::string::npos);
    assert(importRes.find("\"imported_count\":2") != std::string::npos);

    // Export HTML bookmarks
    std::string exportedHtml = AeonBridge::Dispatch("exportBookmarks", "\"html\"");
    assert(exportedHtml.find("Aeon Browser GitHub") != std::string::npos);
    assert(exportedHtml.find("delgadologic.tech") != std::string::npos);

    // Export JSON bookmarks
    std::string exportedJson = AeonBridge::Dispatch("exportBookmarks", "\"json\"");
    assert(exportedJson.find("https://github.com/DelgadoLogic/AeonBrowser") != std::string::npos);

    std::cout << "[PASS] Test 6: AeonBridge::Dispatch() importBookmarks & exportBookmarks (HTML & JSON) succeed" << std::endl;
    passed++;

    // ─────────────────────────────────────────────────────────────
    // 7. AeonBridge History Search & Batch Deletion
    // ─────────────────────────────────────────────────────────────
    HistoryEngine::RecordVisit("https://delgadologic.tech/products/aeon", "Aeon Browser Sovereign");
    HistoryEngine::RecordVisit("https://news.ycombinator.com", "Hacker News");

    std::string searchRes = AeonBridge::Dispatch("searchHistory", "\"Sovereign\"");
    assert(searchRes.find("Aeon Browser Sovereign") != std::string::npos);
    assert(searchRes.find("Hacker News") == std::string::npos);

    std::string delRes = AeonBridge::Dispatch("deleteHistoryEntries", "[\"https://news.ycombinator.com\"]");
    assert(delRes.find("\"ok\":true") != std::string::npos);
    assert(delRes.find("\"deleted_count\":1") != std::string::npos);

    std::cout << "[PASS] Test 7: AeonBridge::Dispatch() searchHistory & deleteHistoryEntries succeed" << std::endl;
    passed++;

    // ─────────────────────────────────────────────────────────────
    // 8. AeonBridge Clear Browsing Data API
    // ─────────────────────────────────────────────────────────────
    std::string clearRes = AeonBridge::Dispatch("clearBrowsingData", "{\"history\":true,\"downloads\":true,\"cookies\":true,\"cache\":true,\"passwords\":true}");
    assert(clearRes == "{\"ok\":true,\"cleared\":true}");

    std::cout << "[PASS] Test 8: AeonBridge::Dispatch() clearBrowsingData succeeds" << std::endl;
    passed++;

    // ─────────────────────────────────────────────────────────────
    // 9. AeonBridge BuildInjectionScript Verification
    // ─────────────────────────────────────────────────────────────
    std::string script = AeonBridge::BuildInjectionScript();
    assert(script.find("window.__aeon = {") != std::string::npos);
    assert(script.find("ai_provider:") != std::string::npos);
    assert(script.find("byedpi_enabled:") != std::string::npos);
    assert(script.find("goodbyedpi_mode:") != std::string::npos);
    assert(script.find("ech_enabled:") != std::string::npos);
    assert(script.find("kyber_enabled:") != std::string::npos);
    assert(script.find("camoufox_enabled:") != std::string::npos);
    assert(script.find("canvas_noise_enabled:") != std::string::npos);
    assert(script.find("theme_preset:") != std::string::npos);
    assert(script.find("vertical_tabs:") != std::string::npos);
    assert(script.find("revealPassword:") != std::string::npos);
    assert(script.find("clearBrowsingData:") != std::string::npos);
    assert(script.find("importBookmarks:") != std::string::npos);
    assert(script.find("exportBookmarks:") != std::string::npos);
    assert(script.find("searchHistory:") != std::string::npos);
    assert(script.find("deleteHistoryEntries:") != std::string::npos);
    assert(script.find("removeDownload:") != std::string::npos);

    std::cout << "[PASS] Test 9: AeonBridge::BuildInjectionScript() generates complete live settings & method bindings" << std::endl;
    passed++;

    std::cout << "\n=== All " << passed << "/9 Milestone 2 & 5 Backend Engine & Bridge Tests Passed Successfully! ===" << std::endl;
    return 0;
}
