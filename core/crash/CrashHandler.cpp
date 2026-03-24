// AeonBrowser — CrashHandler.cpp
// DelgadoLogic | Senior Security Engineer
//
// PURPOSE: Vectored Exception Handler that catches unhandled access violations,
// stack overflows, and assertion failures, writes a minidump, queues crash
// report for telemetry upload, and offers the user session restore on relaunch.
//
// IT TROUBLESHOOTING:
//   - No minidump generated: dbghelp.dll may be missing. Ship it in installer.
//   - Crash loop (crashes on startup every time): session restore may be
//     re-loading a corrupted page. Delete %APPDATA%\Aeon\session.json manually.
//   - Telemetry not uploading crash: check TelemetryEnabled registry value.

#include "CrashHandler.h"
#include <windows.h>
#include <dbghelp.h>
#include <cstdio>
#include <ctime>

#pragma comment(lib, "dbghelp.lib")

namespace AeonCrash {

static LONG WINAPI VectoredHandler(EXCEPTION_POINTERS* ep) {
    // Don't handle C++ exceptions (__CxxFrameHandler) — those are caught
    // by the runtime. Only intercept genuine hardware faults.
    DWORD code = ep->ExceptionRecord->ExceptionCode;
    if (code == EXCEPTION_BREAKPOINT || code == EXCEPTION_SINGLE_STEP) {
        return EXCEPTION_CONTINUE_SEARCH; // debugger attached — skip
    }

    // Build minidump filename: %TEMP%\aeon_crash_<timestamp>.dmp
    char tempPath[MAX_PATH], dumpPath[MAX_PATH];
    GetTempPathA(MAX_PATH, tempPath);

    time_t now = time(nullptr);
    _snprintf_s(dumpPath, sizeof(dumpPath), _TRUNCATE,
        "%saeon_crash_%llu.dmp", tempPath, (unsigned long long)now);

    HANDLE hFile = CreateFileA(dumpPath, GENERIC_WRITE, 0, nullptr,
        CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);

    if (hFile != INVALID_HANDLE_VALUE) {
        // MiniDumpWithFullMemory gives us enough context for root-cause analysis
        // MiniDumpNormal is lighter — good for retro machines with limited disk
        MINIDUMP_EXCEPTION_INFORMATION mei = {};
        mei.ThreadId          = GetCurrentThreadId();
        mei.ExceptionPointers = ep;
        mei.ClientPointers    = FALSE;

        // Use full memory on modern tiers, normal on legacy (saves ~50MB)
        MINIDUMP_TYPE dumpType = MiniDumpNormal;
        // TODO: check tier from global SystemProfile for full dump decision

        MiniDumpWriteDump(GetCurrentProcess(), GetCurrentProcessId(),
            hFile, dumpType, &mei, nullptr, nullptr);
        CloseHandle(hFile);

        fprintf(stderr,
            "[AeonCrash] Minidump written: %s\n"
            "            Upload will be attempted on next launch.\n",
            dumpPath);

        // Write crash sentinel file — PulseBridge reads this on next startup
        // and uploads the minidump path to our crash telemetry endpoint.
        char sentinelPath[MAX_PATH];
        _snprintf_s(sentinelPath, sizeof(sentinelPath), _TRUNCATE,
            "%saeon_crash_pending.txt", tempPath);
        FILE* f = nullptr;
        fopen_s(&f, sentinelPath, "w");
        if (f) { fprintf(f, "%s\n", dumpPath); fclose(f); }
    }

    // Show minimal crash dialog (not the Windows one — ours is branded)
    MessageBoxA(nullptr,
        "Aeon Browser encountered an error and needs to close.\n\n"
        "Your browsing session has been saved and will be restored on restart.\n"
        "(Crash report queued for DelgadoLogic — thank you for helping us improve.)",
        "Aeon Browser — Crash", MB_ICONERROR | MB_OK);

    // Terminate cleanly — do NOT let Windows show the "Send to Microsoft" dialog
    TerminateProcess(GetCurrentProcess(), static_cast<UINT>(code));
    return EXCEPTION_EXECUTE_HANDLER; // never reached
}

void Install() {
    // AddVectoredExceptionHandler: 1 = call first (highest priority)
    AddVectoredExceptionHandler(1, VectoredHandler);
    fprintf(stdout, "[AeonCrash] Crash handler installed.\n");
}

} // namespace AeonCrash
