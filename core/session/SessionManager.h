// AeonBrowser — SessionManager.h
#pragma once
#include "../probe/HardwareProbe.h"

namespace SessionManager {
    void Initialize(const SystemProfile& p);
    void SaveTab(const char* url, int scrollY, const char* title);
    void SaveAndExit();
    bool RestorePreviousSession();
}
