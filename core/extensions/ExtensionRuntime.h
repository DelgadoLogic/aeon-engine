// AeonBrowser — ExtensionRuntime.h
#pragma once
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include "../probe/HardwareProbe.h"

namespace ExtensionRuntime {
    void Initialize(const SystemProfile& p);
    bool LoadExtension(const char* crx_path);
}

namespace NativeAdBlock {
    void LoadFilterList(const char* path);
    bool ShouldBlock(const char* url);
    void HideElements(HWND hwnd, const char* url);
    int  GetBlockedCount();   // Number of domain-block rules loaded
    int  GetTotalRules();     // Total rules across all filter lists
}

namespace NativePassMgr {
    bool StoreCredential(const char* origin, const char* user, const char* pass);
}
