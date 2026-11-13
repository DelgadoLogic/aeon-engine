// AeonBrowser — CrashHandler.h
#pragma once

#include <windows.h>

namespace AeonCrash {
    // Call FIRST — before any other module. Installs exception handlers & error mode.
    void Install();
    void Initialize();
    LONG WINAPI UnhandledExceptionFilter(EXCEPTION_POINTERS* ep);
}

