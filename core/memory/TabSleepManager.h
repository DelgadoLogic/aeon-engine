// AeonBrowser — TabSleepManager.h
#pragma once
#include <windows.h>

namespace TabSleepManager {
    // Start the background sleep timer thread
    void Initialize(unsigned int tier_flags);

    // Register a new tab with its renderer thread handle
    void RegisterTab(unsigned int tab_id, const char* url, HANDLE renderer_thread);

    // Mark tab as focused — resets idle timer, wakes if sleeping
    void OnTabFocused(unsigned int tab_id);

    // Reset idle timer on user activity (scroll, click, key)
    void OnTabActivity(unsigned int tab_id);

    // Remove tab on close
    void UnregisterTab(unsigned int tab_id);

    // Stop timer thread and resume all sleeping tabs (call before exit)
    void Shutdown();
}
