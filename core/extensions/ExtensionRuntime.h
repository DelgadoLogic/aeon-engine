// AeonBrowser — ExtensionRuntime.h
#pragma once
#include "../probe/HardwareProbe.h"

namespace ExtensionRuntime {
    void Initialize(const SystemProfile& p);
    bool LoadExtension(const char* crx_path);
}

namespace NativeAdBlock {
    void LoadFilterList(const char* path);
    bool ShouldBlock(const char* url);
    void HideElements(HWND hwnd, const char* url);
}

namespace NativePassMgr {
    bool StoreCredential(const char* origin, const char* user, const char* pass);
}
