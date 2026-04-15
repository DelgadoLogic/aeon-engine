// AeonBrowser — AeonLog.cpp
// DelgadoLogic | Senior Security Engineer
//
// File-backed rotating logger. Thread-safe via CRITICAL_SECTION.
// Writes to %LOCALAPPDATA%\Aeon\logs\aeon.log.
// Rotates at 5MB, keeps last 3 files (aeon.log.1, aeon.log.2, aeon.log.3).

#include "AeonLog.h"
#include <windows.h>
#include <shlobj.h>
#include <cstdio>
#include <cstdarg>
#include <ctime>

namespace AeonLog {

static const char* kLevelNames[] = {
    "TRACE", "DEBUG", "INFO", "WARN", "ERROR", "FATAL"
};

static FILE*             g_File      = nullptr;
static Level             g_MinLevel  = Level::Info;
static bool              g_HasConsole = false;
static CRITICAL_SECTION  g_Lock;
static char              g_LogPath[MAX_PATH] = {};
static constexpr DWORD   kMaxFileSize = 5 * 1024 * 1024;  // 5MB
static constexpr int     kMaxBackups  = 3;

static void RotateIfNeeded() {
    if (!g_File) return;

    // Check current file size
    long pos = ftell(g_File);
    if (pos < 0 || (DWORD)pos < kMaxFileSize) return;

    fclose(g_File);
    g_File = nullptr;

    // Rotate: aeon.log.3 → delete, .2 → .3, .1 → .2, .log → .1
    for (int i = kMaxBackups; i >= 1; --i) {
        char from[MAX_PATH], to[MAX_PATH];
        if (i == 1) {
            _snprintf_s(from, sizeof(from), _TRUNCATE, "%s", g_LogPath);
        } else {
            _snprintf_s(from, sizeof(from), _TRUNCATE, "%s.%d", g_LogPath, i - 1);
        }
        _snprintf_s(to, sizeof(to), _TRUNCATE, "%s.%d", g_LogPath, i);

        DeleteFileA(to);
        MoveFileA(from, to);
    }

    fopen_s(&g_File, g_LogPath, "a");
}

void Init(const char* logDir, Level minLevel) {
    InitializeCriticalSection(&g_Lock);
    g_MinLevel = minLevel;

    // Detect console
    g_HasConsole = (GetStdHandle(STD_OUTPUT_HANDLE) != nullptr &&
                    GetStdHandle(STD_OUTPUT_HANDLE) != INVALID_HANDLE_VALUE);

    // Build log directory
    char baseDir[MAX_PATH] = {};
    if (logDir) {
        strncpy_s(baseDir, sizeof(baseDir), logDir, _TRUNCATE);
    } else {
        SHGetFolderPathA(nullptr, CSIDL_LOCAL_APPDATA, nullptr, 0, baseDir);
        strncat_s(baseDir, sizeof(baseDir), "\\Aeon\\logs", _TRUNCATE);
    }

    // Create directory tree
    CreateDirectoryA(baseDir, nullptr);
    char parent[MAX_PATH];
    _snprintf_s(parent, sizeof(parent), _TRUNCATE, "%s\\..", baseDir);
    // Ensure parent exists too
    char aeonDir[MAX_PATH];
    SHGetFolderPathA(nullptr, CSIDL_LOCAL_APPDATA, nullptr, 0, aeonDir);
    strncat_s(aeonDir, sizeof(aeonDir), "\\Aeon", _TRUNCATE);
    CreateDirectoryA(aeonDir, nullptr);
    CreateDirectoryA(baseDir, nullptr);

    _snprintf_s(g_LogPath, sizeof(g_LogPath), _TRUNCATE, "%s\\aeon.log", baseDir);

    fopen_s(&g_File, g_LogPath, "a");
    if (g_File) {
        fprintf(g_File, "\n--- Aeon Log Session Start ---\n");
        fflush(g_File);
    }
}

void Shutdown() {
    EnterCriticalSection(&g_Lock);
    if (g_File) {
        fprintf(g_File, "--- Aeon Log Session End ---\n");
        fclose(g_File);
        g_File = nullptr;
    }
    LeaveCriticalSection(&g_Lock);
    DeleteCriticalSection(&g_Lock);
}

void Log(Level level, const char* category, const char* fmt, ...) {
    if ((int)level < (int)g_MinLevel) return;

    EnterCriticalSection(&g_Lock);

    // Timestamp
    SYSTEMTIME st;
    GetLocalTime(&st);

    char timeBuf[32];
    _snprintf_s(timeBuf, sizeof(timeBuf), _TRUNCATE,
        "%04d-%02d-%02d %02d:%02d:%02d.%03d",
        st.wYear, st.wMonth, st.wDay,
        st.wHour, st.wMinute, st.wSecond, st.wMilliseconds);

    // Format the user message
    char msgBuf[1024];
    va_list args;
    va_start(args, fmt);
    _vsnprintf_s(msgBuf, sizeof(msgBuf), _TRUNCATE, fmt, args);
    va_end(args);

    // Full log line
    char line[1280];
    _snprintf_s(line, sizeof(line), _TRUNCATE,
        "[%s] [%s] [%s] %s\n",
        timeBuf, kLevelNames[(int)level], category ? category : "-", msgBuf);

    // Write to file
    if (g_File) {
        fputs(line, g_File);
        // Auto-flush on WARN+ for timely crash diagnostics
        if ((int)level >= (int)Level::Warn) {
            fflush(g_File);
        }
        RotateIfNeeded();
    }

    // Mirror to console if available
    if (g_HasConsole) {
        FILE* out = ((int)level >= (int)Level::Error) ? stderr : stdout;
        fputs(line, out);
    }

    LeaveCriticalSection(&g_Lock);
}

void Flush() {
    EnterCriticalSection(&g_Lock);
    if (g_File) fflush(g_File);
    LeaveCriticalSection(&g_Lock);
}

} // namespace AeonLog
