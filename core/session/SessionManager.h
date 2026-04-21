// AeonBrowser — SessionManager.h
#pragma once
#include <windows.h>
#include "../probe/HardwareProbe.h"

namespace SessionManager {
    // Initialize session subsystem with hardware profile.
    void Initialize(const SystemProfile& p);

    // Set the main browser window handle — starts autosave timer.
    void SetMainWindow(HWND hwnd);

    // Called by WM_TIMER when autosave timer fires (every 30s).
    void OnAutosaveTimer();

    // Save a single tab's state (incremental update).
    void SaveTab(const char* url, int scrollY, const char* title);

    // Full snapshot and write to disk — call on clean exit.
    void SaveAndExit();

    // Restore tabs from previous session file. Returns false if none found.
    // Creates tabs via BrowserChrome::CreateTab.
    bool RestorePreviousSession();

    // Autosave timer ID — check against this in WM_TIMER handler.
    static const UINT_PTR AUTOSAVE_TIMER_ID = 0xAE05;
}
