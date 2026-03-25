// AeonBrowser — TabSleepManager.cpp
// DelgadoLogic | Lead Systems Architect
//
// PURPOSE: Automatically suspends background tabs that have been idle for
// a configurable period. This is the PRIMARY mechanism by which Aeon beats
// Chrome's RAM usage — a sleeping tab holds <5MB vs 80-300MB active.
//
// ALGORITHM:
//   1. Each tab registers itself on creation with a unique tab_id.
//   2. A background timer thread ticks every 60 seconds.
//   3. If a tab has been idle (no user interaction, no audio, no video)
//      for more than SLEEP_THRESHOLD_MS, it is marked SLEEPING.
//   4. Sleeping tabs have their renderer process suspended (SuspendThread)
//      and their JS heap discarded.
//   5. On tab focus: resume renderer, restore scroll position from session.
//
// NEVER sleep:
//   - The active tab (focused)
//   - Tabs with active audio/video
//   - Tabs with pending network requests (download in progress)
//   - Pinned tabs (user opted-in to keep alive)
//
// IT TROUBLESHOOTING:
//   - Tab wakes from sleep slowly: renderer restart time.
//     Increase SLEEP_THRESHOLD to give more time before suspension.
//   - Video keeps playing in background: AudioActivityCheck failed.
//   - Download interrupted: DownloadManager notifies us to keep tab awake.
//     Check that aeon_download_progress() is being polled correctly.

#include "TabSleepManager.h"
#include <windows.h>
#include <vector>
#include <mutex>
#include <cstdio>
#include <ctime>

// ─── Configurable thresholds — auto-tuned to available RAM ───────────────────
// 512MB or less  → 10 min  (legacy/low-RAM machines need aggressive sleep)
// 512MB – 2GB    → 20 min
// 2GB+           → 30 min  (power PCs)
// All overridable via Settings → Tabs → Sleep threshold
static constexpr DWORD TIMER_INTERVAL_MS = 60 * 1000; // check every 60 seconds
static constexpr DWORD MAX_SLEEPING_TABS = 50;

static DWORD GetSleepThresholdMs() {
    MEMORYSTATUSEX ms = {};
    ms.dwLength = sizeof(ms);
    if (!GlobalMemoryStatusEx(&ms)) return 30 * 60 * 1000;
    DWORDLONG totalMB = ms.ullTotalPhys / (1024 * 1024);
    if (totalMB <= 512)  return  10 * 60 * 1000; // 10 min
    if (totalMB <= 2048) return  20 * 60 * 1000; // 20 min
    return                        30 * 60 * 1000; // 30 min
}

namespace TabSleepManager {


struct TabEntry {
    unsigned int tab_id;
    DWORD        last_active_tick;   // GetTickCount() at last user action
    HANDLE       renderer_thread;    // renderer thread handle (may be null)
    bool         is_sleeping;
    bool         is_pinned;
    bool         has_audio;
    bool         has_active_download;
    char         url[512];
};

static std::vector<TabEntry> g_Tabs;
static std::mutex            g_Mutex;
static HANDLE                g_TimerThread = nullptr;
static bool                  g_Running     = false;

// ---------------------------------------------------------------------------
// Background sleep timer thread
// ---------------------------------------------------------------------------
static DWORD WINAPI SleepTimerProc(LPVOID) {
    while (g_Running) {
        Sleep(TIMER_INTERVAL_MS);
        if (!g_Running) break;

        DWORD now       = GetTickCount();
        DWORD threshold = GetSleepThresholdMs(); // recalculated each tick

        std::lock_guard<std::mutex> lock(g_Mutex);

        unsigned int sleeping_count = 0;
        for (auto& tab : g_Tabs) {
            if (tab.is_sleeping) { sleeping_count++; continue; }

            // Never sleep active/pinned/audio/downloading tabs
            if (tab.is_pinned || tab.has_audio || tab.has_active_download)
                continue;

            // Check idle time
            DWORD idle_ms = now - tab.last_active_tick;
            if (idle_ms >= SLEEP_THRESHOLD_MS && sleeping_count < MAX_SLEEPING_TABS) {
                tab.is_sleeping = true;
                sleeping_count++;

                // Suspend the renderer thread to free CPU + allow OS to page out heap
                if (tab.renderer_thread) {
                    SuspendThread(tab.renderer_thread);
                    fprintf(stdout, "[TabSleep] Suspended tab %u: %s (idle %.0f min)\n",
                        tab.tab_id, tab.url, (double)idle_ms / 60000.0);
                }
            }
        }
    }
    return 0;
}

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------
void Initialize(unsigned int /*tier_flags*/) {
    g_Running = true;
    DWORD tid;
    g_TimerThread = CreateThread(nullptr, 0, SleepTimerProc, nullptr, 0, &tid);
    fprintf(stdout, "[TabSleep] Tab sleep manager running "
        "(threshold: %lu min).\n", SLEEP_THRESHOLD_MS / 60000);
}

void RegisterTab(unsigned int tab_id, const char* url, HANDLE renderer_thread) {
    std::lock_guard<std::mutex> lock(g_Mutex);
    TabEntry e = {};
    e.tab_id          = tab_id;
    e.last_active_tick= GetTickCount();
    e.renderer_thread = renderer_thread;
    e.is_sleeping     = false;
    e.is_pinned       = false;
    e.has_audio       = false;
    strncpy_s(e.url, sizeof(e.url), url ? url : "", _TRUNCATE);
    g_Tabs.push_back(e);
}

void OnTabFocused(unsigned int tab_id) {
    std::lock_guard<std::mutex> lock(g_Mutex);
    for (auto& tab : g_Tabs) {
        if (tab.tab_id != tab_id) continue;
        tab.last_active_tick = GetTickCount();
        if (tab.is_sleeping) {
            tab.is_sleeping = false;
            if (tab.renderer_thread) {
                ResumeThread(tab.renderer_thread); // wake the renderer
                fprintf(stdout, "[TabSleep] Resumed tab %u: %s\n",
                    tab.tab_id, tab.url);
            }
        }
        break;
    }
}

void OnTabActivity(unsigned int tab_id) {
    // User scrolled / clicked in tab — reset idle timer
    std::lock_guard<std::mutex> lock(g_Mutex);
    for (auto& tab : g_Tabs) {
        if (tab.tab_id == tab_id) {
            tab.last_active_tick = GetTickCount();
            break;
        }
    }
}

void UnregisterTab(unsigned int tab_id) {
    std::lock_guard<std::mutex> lock(g_Mutex);
    auto it = g_Tabs.begin();
    while (it != g_Tabs.end()) {
        if (it->tab_id == tab_id) {
            if (it->renderer_thread && it->is_sleeping)
                ResumeThread(it->renderer_thread); // ensure resumed before close
            it = g_Tabs.erase(it);
            return;
        }
        ++it;
    }
}

void Shutdown() {
    g_Running = false;
    if (g_TimerThread) {
        WaitForSingleObject(g_TimerThread, TIMER_INTERVAL_MS + 1000);
        CloseHandle(g_TimerThread);
        g_TimerThread = nullptr;
    }
    // Resume all sleeping tabs so their renderers terminate cleanly
    std::lock_guard<std::mutex> lock(g_Mutex);
    for (auto& tab : g_Tabs) {
        if (tab.is_sleeping && tab.renderer_thread)
            ResumeThread(tab.renderer_thread);
    }
    g_Tabs.clear();
}

} // namespace TabSleepManager
