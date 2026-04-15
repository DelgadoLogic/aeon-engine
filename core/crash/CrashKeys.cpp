// AeonBrowser — CrashKeys.cpp
// DelgadoLogic | Senior Security Engineer
//
// Lock-free crash key store. Uses InterlockedCompareExchange for thread
// safety without mutexes — critical because crash handlers cannot acquire
// locks (the owning thread may be the one that crashed).

#include "CrashKeys.h"
#include <windows.h>
#include <cstring>
#include <cstdio>

namespace AeonCrash {

// Static storage — never heap-allocated, survives corruption.
static CrashKeyEntry g_Keys[kMaxKeys] = {};

void SetKey(const char* key, const char* value) {
    if (!key || !key[0]) return;

    // First pass: look for existing key to overwrite
    for (int i = 0; i < kMaxKeys; ++i) {
        if (g_Keys[i].occupied && _stricmp(g_Keys[i].key, key) == 0) {
            strncpy_s(g_Keys[i].value, sizeof(g_Keys[i].value), value ? value : "", _TRUNCATE);
            return;
        }
    }

    // Second pass: find empty slot
    for (int i = 0; i < kMaxKeys; ++i) {
        // Atomic CAS: claim the slot only if it's unoccupied
        LONG prev = InterlockedCompareExchange(
            reinterpret_cast<volatile LONG*>(&g_Keys[i].occupied), 1, 0);
        if (prev == 0) {
            // We won the slot
            strncpy_s(g_Keys[i].key, sizeof(g_Keys[i].key), key, _TRUNCATE);
            strncpy_s(g_Keys[i].value, sizeof(g_Keys[i].value), value ? value : "", _TRUNCATE);
            return;
        }
    }

    // All 64 slots full — silently drop. In production this should never
    // happen; if it does, increase kMaxKeys.
}

void SetKeyInt(const char* key, int64_t value) {
    char buf[32];
    _snprintf_s(buf, sizeof(buf), _TRUNCATE, "%lld", value);
    SetKey(key, buf);
}

void ClearKey(const char* key) {
    if (!key || !key[0]) return;
    for (int i = 0; i < kMaxKeys; ++i) {
        if (g_Keys[i].occupied && _stricmp(g_Keys[i].key, key) == 0) {
            g_Keys[i].occupied = false;
            g_Keys[i].key[0]   = '\0';
            g_Keys[i].value[0] = '\0';
            return;
        }
    }
}

const CrashKeyEntry* GetAllKeys() {
    return g_Keys;
}

int GetKeyCount() {
    int count = 0;
    for (int i = 0; i < kMaxKeys; ++i) {
        if (g_Keys[i].occupied) ++count;
    }
    return count;
}

} // namespace AeonCrash
