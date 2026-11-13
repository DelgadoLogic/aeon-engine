// AeonBrowser -- SessionManager.h
#pragma once
#include <windows.h>
#include "../probe/HardwareProbe.h"

#include <cstdio>
#include <cstring>
#include <string>
#include <cstdint>

struct AeonEngineVTable;

namespace SessionManager {
    typedef int (*FnGetTabCount)(HWND);
    typedef int (*FnGetActiveTabIndex)(HWND);
    typedef bool (*FnGetTabInfo)(HWND, int, unsigned int*, char*, int, char*, int, bool*);
    typedef unsigned int (*FnCreateTab)(HWND, const char*);
    typedef bool (*FnFocusTabById)(HWND, unsigned int);
    typedef bool (*FnAddTabToGroup)(HWND, int, uint32_t);
    typedef bool (*FnCloseTabById)(HWND, unsigned int);

    struct SessionUiHooks {
        FnGetTabCount GetTabCount = nullptr;
        FnGetActiveTabIndex GetActiveTabIndex = nullptr;
        FnGetTabInfo GetTabInfo = nullptr;
        FnCreateTab CreateTab = nullptr;
        FnFocusTabById FocusTabById = nullptr;
        FnAddTabToGroup AddTabToGroup = nullptr;
        FnCloseTabById CloseTabById = nullptr;
    };

    // Initialize session subsystem with hardware profile.
    void Initialize(const SystemProfile& p);

    // Set the main browser window handle -- starts autosave timer.
    void SetMainWindow(HWND hwnd);

    // Wire UI callbacks for tab querying and creation (from BrowserChrome).
    void SetUiHooks(const SessionUiHooks& hooks);

    // Set active engine vtable for storage state serialization.
    void SetEngine(AeonEngineVTable* engine);

    // Called by WM_TIMER when autosave timer fires (every 30s).
    void OnAutosaveTimer();

    // Save a single tab's state (incremental update).
    void SaveTab(const char* url, int scrollY, const char* title);

    // Full snapshot and write to disk -- call on clean exit.
    void SaveAndExit();

    // Restore tabs from previous session file. Returns false if none found.
    // Creates tabs via UI hook CreateTab and restores groups.
    bool RestorePreviousSession();

    // Session path overrides for testing / alternate profiles
    void SetSessionPath(const char* path);
    const char* GetSessionPath();

    // Export storage state (cookies + LocalStorage) in Playwright auth.json format.
    // Default path: %MODULE_DIR%\userDataDir\auth.json
    bool ExportSessionState(const char* json_path = nullptr, int* out_cookies = nullptr, int* out_origins = nullptr);

    // Import storage state from Playwright auth.json format into browser profile.
    // Default path: %MODULE_DIR%\userDataDir\auth.json
    bool ImportSessionState(const char* json_path = nullptr, int* out_cookies = nullptr, int* out_origins = nullptr);

    class Instance {
    public:
        static Instance& GetInstance() { static Instance inst; return inst; }
        bool ExportSessionState(const char* json_path = nullptr, int* out_cookies = nullptr, int* out_origins = nullptr) {
            return SessionManager::ExportSessionState(json_path, out_cookies, out_origins);
        }
        bool ImportSessionState(const char* json_path = nullptr, int* out_cookies = nullptr, int* out_origins = nullptr) {
            return SessionManager::ImportSessionState(json_path, out_cookies, out_origins);
        }
    };
    inline Instance& GetInstance() { return Instance::GetInstance(); }

    // Autosave timer ID -- check against this in WM_TIMER handler.
    static const UINT_PTR AUTOSAVE_TIMER_ID = 0xAE05;
}