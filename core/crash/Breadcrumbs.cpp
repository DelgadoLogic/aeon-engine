// AeonBrowser — Breadcrumbs.cpp
// DelgadoLogic | Senior Security Engineer
//
// Lock-free ring buffer implementation. Uses InterlockedIncrement to
// advance the write head atomically — no mutexes, no heap.

#include "Breadcrumbs.h"
#include <windows.h>
#include <cstring>

namespace AeonCrash {

static Breadcrumb g_Ring[kMaxBreadcrumbs] = {};
static volatile LONG g_Head = -1;  // Index of last written entry
static uint64_t g_StartTick = 0;   // GetTickCount64() at first breadcrumb

void AddBreadcrumb(const char* event, const char* detail) {
    if (!event || !event[0]) return;

    // Record start time on first call
    if (g_StartTick == 0) {
        g_StartTick = GetTickCount64();
    }

    // Atomically advance write head (wraps around via modulo)
    LONG idx = InterlockedIncrement(&g_Head) % kMaxBreadcrumbs;

    // Write into the slot (overwrites oldest if buffer is full)
    g_Ring[idx].timestamp_ms = GetTickCount64();
    strncpy_s(g_Ring[idx].event, sizeof(g_Ring[idx].event), event, _TRUNCATE);
    if (detail && detail[0]) {
        strncpy_s(g_Ring[idx].detail, sizeof(g_Ring[idx].detail), detail, _TRUNCATE);
    } else {
        g_Ring[idx].detail[0] = '\0';
    }
    g_Ring[idx].occupied = true;
}

const Breadcrumb* GetBreadcrumbs() {
    return g_Ring;
}

int GetBreadcrumbHead() {
    LONG h = g_Head;
    if (h < 0) return -1;
    return h % kMaxBreadcrumbs;
}

double GetUptimeSeconds() {
    if (g_StartTick == 0) return 0.0;
    return (double)(GetTickCount64() - g_StartTick) / 1000.0;
}

} // namespace AeonCrash
