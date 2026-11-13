// AeonBrowser — TabSleepManager.h
// DelgadoLogic | Lead Systems Architect
#pragma once
#include <windows.h>
#include <cstddef>

struct AeonEngineVTable;

namespace TabSleepManager {

    struct TabSleepState {
        unsigned int tab_id;
        DWORD        last_active_tick;   // GetTickCount() at last user action
        HANDLE       renderer_thread;    // renderer thread handle (may be null)
        bool         is_sleeping;
        bool         is_pinned;
        bool         has_audio;
        bool         has_active_download;
        size_t       memory_saved_bytes;
        char         url[512];
    };

    // Start the background sleep timer thread
    void Initialize(unsigned int tier_flags);

    // Register a new tab with its renderer thread handle
    void RegisterTab(unsigned int tab_id, const char* url, HANDLE renderer_thread = nullptr);

    // Mark tab as focused — resets idle timer, wakes if sleeping
    void OnTabFocused(unsigned int tab_id);

    // Reset idle timer on user activity (scroll, click, key)
    void OnTabActivity(unsigned int tab_id);

    // Remove tab on close
    void UnregisterTab(unsigned int tab_id);

    // Exemption guardrails
    void UpdateTabAudioState(unsigned int tab_id, bool is_playing_audio);
    void UpdateTabDownloadState(unsigned int tab_id, bool is_downloading);
    void SetTabPinned(unsigned int tab_id, bool is_pinned);

    // Status queries
    bool IsTabSleeping(unsigned int tab_id);
    size_t GetSleepingTabCount();
    size_t GetTotalMemorySavedBytes();
    void SetSleepThresholdMinutes(int minutes);
    int  GetSleepThresholdMinutes();
    DWORD GetSleepThresholdMs();

    // Direct programmatic suspension/wake
    bool SuspendTab(unsigned int tab_id);
    bool WakeTab(unsigned int tab_id);

    // Engine & Window wiring
    void SetEngine(AeonEngineVTable* engine, HWND mainHwnd);

    // Stop timer thread and resume all sleeping tabs (call before exit)
    void Shutdown();
}
