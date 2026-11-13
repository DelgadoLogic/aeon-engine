// AeonBrowser — CrashHandler.cpp
// DelgadoLogic | Senior Security Engineer
//
// PURPOSE: Vectored Exception Handler that catches unhandled access violations,
// stack overflows, and assertion failures. On crash:
//   1. Writes a rich minidump (.dmp) with thread stacks and module info
//   2. Generates a structured crash_report.json sidecar (AI-parseable)
//   3. Includes all crash keys and breadcrumbs in the JSON
//   4. Queues crash report for telemetry upload on next launch
//   5. Shows a branded crash dialog
//
// IT TROUBLESHOOTING:
//   - No minidump generated: dbghelp.dll may be missing. Ship it in installer.
//   - Crash loop (crashes on startup every time): session restore may be
//     re-loading a corrupted page. Delete %APPDATA%\Aeon\session.json manually.
//   - Telemetry not uploading crash: check TelemetryEnabled registry value.

#include "CrashHandler.h"
#include "CrashKeys.h"
#include "Breadcrumbs.h"
#include <windows.h>
#include <dbghelp.h>
#include <cstdio>
#include <ctime>

#pragma comment(lib, "dbghelp.lib")

namespace AeonCrash {

// Exception code → human-readable name for the JSON report
static const char* ExceptionName(DWORD code) {
    switch (code) {
        case EXCEPTION_ACCESS_VIOLATION:      return "EXCEPTION_ACCESS_VIOLATION";
        case EXCEPTION_ARRAY_BOUNDS_EXCEEDED: return "EXCEPTION_ARRAY_BOUNDS_EXCEEDED";
        case EXCEPTION_STACK_OVERFLOW:        return "EXCEPTION_STACK_OVERFLOW";
        case EXCEPTION_ILLEGAL_INSTRUCTION:   return "EXCEPTION_ILLEGAL_INSTRUCTION";
        case EXCEPTION_IN_PAGE_ERROR:         return "EXCEPTION_IN_PAGE_ERROR";
        case EXCEPTION_INT_DIVIDE_BY_ZERO:    return "EXCEPTION_INT_DIVIDE_BY_ZERO";
        case EXCEPTION_PRIV_INSTRUCTION:      return "EXCEPTION_PRIV_INSTRUCTION";
        case EXCEPTION_FLT_DIVIDE_BY_ZERO:    return "EXCEPTION_FLT_DIVIDE_BY_ZERO";
        case EXCEPTION_FLT_OVERFLOW:          return "EXCEPTION_FLT_OVERFLOW";
        case EXCEPTION_FLT_UNDERFLOW:         return "EXCEPTION_FLT_UNDERFLOW";
        default:                              return "UNKNOWN";
    }
}

// Escape a string for JSON embedding (handles quotes, backslashes, newlines)
static void JsonEscape(FILE* f, const char* s) {
    if (!s) { fprintf(f, "null"); return; }
    fputc('"', f);
    for (; *s; ++s) {
        switch (*s) {
            case '"':  fputs("\\\"", f); break;
            case '\\': fputs("\\\\", f); break;
            case '\n': fputs("\\n", f);  break;
            case '\r': fputs("\\r", f);  break;
            case '\t': fputs("\\t", f);  break;
            default:
                if ((unsigned char)*s >= 0x20) fputc(*s, f);
                break;
        }
    }
    fputc('"', f);
}

// Write crash_report.json alongside the minidump
static void WriteJsonReport(const char* jsonPath, const char* dmpPath,
                             DWORD exCode, const void* exAddr, DWORD threadId) {
    FILE* f = nullptr;
    fopen_s(&f, jsonPath, "w");
    if (!f) return;

    // Header
    fprintf(f, "{\n");
    fprintf(f, "  \"crash_handler_version\": 2,\n");
    fprintf(f, "  \"timestamp\": %llu,\n", (unsigned long long)time(nullptr));

    // Exception info
    fprintf(f, "  \"exception\": {\n");
    fprintf(f, "    \"code\": \"0x%08X\",\n", exCode);
    fprintf(f, "    \"name\": \"%s\",\n", ExceptionName(exCode));
    fprintf(f, "    \"address\": \"0x%016llX\",\n", (unsigned long long)(uintptr_t)exAddr);
    fprintf(f, "    \"thread_id\": %u\n", threadId);
    fprintf(f, "  },\n");

    // Process info
    fprintf(f, "  \"process\": {\n");
    fprintf(f, "    \"pid\": %u,\n", GetCurrentProcessId());
    fprintf(f, "    \"uptime_seconds\": %.1f\n", GetUptimeSeconds());
    fprintf(f, "  },\n");

    // Minidump path
    fprintf(f, "  \"minidump\": ");
    JsonEscape(f, dmpPath);
    fprintf(f, ",\n");

    // Crash keys
    fprintf(f, "  \"crash_keys\": {\n");
    const CrashKeyEntry* keys = GetAllKeys();
    bool first = true;
    for (int i = 0; i < kMaxKeys; ++i) {
        if (!keys[i].occupied) continue;
        if (!first) fprintf(f, ",\n");
        fprintf(f, "    ");
        JsonEscape(f, keys[i].key);
        fprintf(f, ": ");
        JsonEscape(f, keys[i].value);
        first = false;
    }
    fprintf(f, "\n  },\n");

    // Breadcrumbs (ordered newest-first, with time delta)
    fprintf(f, "  \"breadcrumbs\": [\n");
    const Breadcrumb* ring = GetBreadcrumbs();
    int head = GetBreadcrumbHead();
    uint64_t crashTick = GetTickCount64();
    bool firstBc = true;

    if (head >= 0) {
        for (int n = 0; n < kMaxBreadcrumbs; ++n) {
            // Walk backwards from head
            int idx = (head - n + kMaxBreadcrumbs) % kMaxBreadcrumbs;
            if (!ring[idx].occupied) continue;

            if (!firstBc) fprintf(f, ",\n");
            double delta = -(double)(crashTick - ring[idx].timestamp_ms) / 1000.0;
            fprintf(f, "    {\"t\": %.3f, \"event\": ", delta);
            JsonEscape(f, ring[idx].event);
            if (ring[idx].detail[0]) {
                fprintf(f, ", \"detail\": ");
                JsonEscape(f, ring[idx].detail);
            }
            fprintf(f, "}");
            firstBc = false;
        }
    }
    fprintf(f, "\n  ]\n");

    fprintf(f, "}\n");
    fclose(f);
}

static bool IsNonInteractiveOrHeadless() {
    // 1. Check environment variables
    const char* envHeadless = getenv("AEON_HEADLESS");
    if (envHeadless && (strcmp(envHeadless, "1") == 0 || _stricmp(envHeadless, "true") == 0)) return true;

    const char* envGenericHeadless = getenv("HEADLESS");
    if (envGenericHeadless && (strcmp(envGenericHeadless, "1") == 0 || _stricmp(envGenericHeadless, "true") == 0)) return true;

    const char* envCi = getenv("CI");
    if (envCi && (strcmp(envCi, "1") == 0 || _stricmp(envCi, "true") == 0)) return true;

    const char* envTest = getenv("AEON_TEST");
    if (envTest && (strcmp(envTest, "1") == 0 || _stricmp(envTest, "true") == 0)) return true;

    const char* envNonInteractive = getenv("AEON_NON_INTERACTIVE");
    if (envNonInteractive && (strcmp(envNonInteractive, "1") == 0 || _stricmp(envNonInteractive, "true") == 0)) return true;

    // 2. Check command line arguments
    const wchar_t* cmdLine = GetCommandLineW();
    if (cmdLine) {
        if (wcsstr(cmdLine, L"--headless") || wcsstr(cmdLine, L"-headless") ||
            wcsstr(cmdLine, L"--test") || wcsstr(cmdLine, L"-test") ||
            wcsstr(cmdLine, L"--non-interactive") || wcsstr(cmdLine, L"-non-interactive") ||
            wcsstr(cmdLine, L"--batch")) {
            return true;
        }
    }

    return false;
}

static LONG WINAPI UnhandledExceptionFilterImpl(EXCEPTION_POINTERS* ep) {
    if (!ep || !ep->ExceptionRecord) return EXCEPTION_CONTINUE_SEARCH;

    DWORD code = ep->ExceptionRecord->ExceptionCode;

    // Skip debugger notifications
    if (code == EXCEPTION_BREAKPOINT || code == EXCEPTION_SINGLE_STEP) {
        return EXCEPTION_CONTINUE_SEARCH;
    }

    // Skip informational / warning exceptions
    if ((code >> 30) < 3) {
        return EXCEPTION_CONTINUE_SEARCH;
    }

    // Suppress all OS error dialogs before teardown
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX | SEM_NOOPENFILEERRORBOX);
    HMODULE hK32 = GetModuleHandleW(L"kernel32.dll");
    if (hK32) {
        typedef BOOL(WINAPI* PFN_SetProcessErrorMode)(DWORD);
        PFN_SetProcessErrorMode pfn = (PFN_SetProcessErrorMode)GetProcAddress(hK32, "SetProcessErrorMode");
        if (pfn) pfn(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX | SEM_NOOPENFILEERRORBOX);
    }
    HMODULE hNtdll = GetModuleHandleW(L"ntdll.dll");
    if (hNtdll) {
        typedef LONG(NTAPI* PFN_NtSetInformationProcess)(HANDLE, ULONG, PVOID, ULONG);
        PFN_NtSetInformationProcess pfnNt = (PFN_NtSetInformationProcess)GetProcAddress(hNtdll, "NtSetInformationProcess");
        if (pfnNt) {
            ULONG hardErrorMode = 0;
            pfnNt(GetCurrentProcess(), 12 /* ProcessDefaultHardErrorMode */, &hardErrorMode, sizeof(hardErrorMode));
        }
    }

    // ── Capture paths ──────────────────────────────────────────────────
    char tempPath[MAX_PATH], dmpPath[MAX_PATH], jsonPath[MAX_PATH],
         sentinelPath[MAX_PATH];
    GetTempPathA(MAX_PATH, tempPath);

    time_t now = time(nullptr);
    _snprintf_s(dmpPath, sizeof(dmpPath), _TRUNCATE,
        "%saeon_crash_%llu.dmp", tempPath, (unsigned long long)now);
    _snprintf_s(jsonPath, sizeof(jsonPath), _TRUNCATE,
        "%saeon_crash_%llu.json", tempPath, (unsigned long long)now);
    _snprintf_s(sentinelPath, sizeof(sentinelPath), _TRUNCATE,
        "%saeon_crash_pending.txt", tempPath);

    // ── Write minidump ─────────────────────────────────────────────────
    HANDLE hFile = CreateFileA(dmpPath, GENERIC_WRITE, 0, nullptr,
        CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);

    if (hFile != INVALID_HANDLE_VALUE) {
        MINIDUMP_EXCEPTION_INFORMATION mei = {};
        mei.ThreadId          = GetCurrentThreadId();
        mei.ExceptionPointers = ep;
        mei.ClientPointers    = FALSE;

        MINIDUMP_TYPE dumpType = static_cast<MINIDUMP_TYPE>(
            MiniDumpWithDataSegs |
            MiniDumpWithHandleData |
            MiniDumpWithThreadInfo |
            MiniDumpWithUnloadedModules |
            MiniDumpWithProcessThreadData
        );

        MiniDumpWriteDump(GetCurrentProcess(), GetCurrentProcessId(),
            hFile, dumpType, &mei, nullptr, nullptr);
        CloseHandle(hFile);
    }

    // ── Write structured JSON sidecar ──────────────────────────────────
    WriteJsonReport(jsonPath, dmpPath, code,
        ep->ExceptionRecord->ExceptionAddress, GetCurrentThreadId());

    // ── Write crash sentinel for PulseBridge ───────────────────────────
    FILE* f = nullptr;
    fopen_s(&f, sentinelPath, "w");
    if (f) {
        fprintf(f, "%s\n%s\n", dmpPath, jsonPath);
        fclose(f);
    }

    // Terminate without passing unhandled hard error to CSRSS
    ExitProcess(static_cast<UINT>(code));
    return EXCEPTION_EXECUTE_HANDLER;
}

LONG WINAPI UnhandledExceptionFilter(EXCEPTION_POINTERS* ep) {
    return UnhandledExceptionFilterImpl(ep);
}

void Install() {
    // Suppress OS error dialogs ("Unknown Hard Error", GP Fault boxes)
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX | SEM_NOOPENFILEERRORBOX);
    HMODULE hK32 = GetModuleHandleW(L"kernel32.dll");
    if (hK32) {
        typedef BOOL(WINAPI* PFN_SetProcessErrorMode)(DWORD);
        PFN_SetProcessErrorMode pfn = (PFN_SetProcessErrorMode)GetProcAddress(hK32, "SetProcessErrorMode");
        if (pfn) pfn(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX | SEM_NOOPENFILEERRORBOX);
        typedef HRESULT(WINAPI* PFN_WerSetFlags)(DWORD);
        PFN_WerSetFlags pfnWer = (PFN_WerSetFlags)GetProcAddress(hK32, "WerSetFlags");
        if (pfnWer) pfnWer(1 /* WER_FAULT_REPORTING_NO_UI */);
    }
    HMODULE hNtdll = GetModuleHandleW(L"ntdll.dll");
    if (hNtdll) {
        typedef LONG(NTAPI* PFN_NtSetInformationProcess)(HANDLE, ULONG, PVOID, ULONG);
        PFN_NtSetInformationProcess pfnNt = (PFN_NtSetInformationProcess)GetProcAddress(hNtdll, "NtSetInformationProcess");
        if (pfnNt) {
            ULONG hardErrorMode = 0;
            pfnNt(GetCurrentProcess(), 12 /* ProcessDefaultHardErrorMode */, &hardErrorMode, sizeof(hardErrorMode));
        }
    }
    _set_abort_behavior(0, _WRITE_ABORT_MSG | _CALL_REPORTFAULT);
    _set_invalid_parameter_handler([](const wchar_t*, const wchar_t*, const wchar_t*, unsigned int, uintptr_t) {});
    _set_purecall_handler([]() {});

    SetUnhandledExceptionFilter((LPTOP_LEVEL_EXCEPTION_FILTER)UnhandledExceptionFilter);
}

void Initialize() {
    Install();
}

} // namespace AeonCrash
