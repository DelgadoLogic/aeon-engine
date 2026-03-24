// AeonBrowser — CrashHandler.h
#pragma once

namespace AeonCrash {
    // Call FIRST — before any other module. Installs vectored exception handler.
    void Install();
}
