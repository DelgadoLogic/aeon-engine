// AeonBrowser — AeonLog.h
// DelgadoLogic | Senior Security Engineer
//
// Structured logging system with level filtering and file output.
// Call AeonLog::Init() early in startup. Then use the macros:
//
//   ALOG_INFO("Boot", "Browser version %s starting", AEON_VERSION);
//   ALOG_WARN("Network", "TLS handshake slow: %dms", elapsed);
//   ALOG_ERROR("Crash", "Unhandled exception 0x%08X", code);
//
// Logs to %LOCALAPPDATA%\Aeon\logs\aeon.log with automatic rotation
// (max 5MB per file, keeps last 3 files).
//
// In debug console mode (--debug), also writes to stdout/stderr.

#pragma once

namespace AeonLog {

enum class Level : int {
    Trace = 0,  // Extremely verbose — disabled in release
    Debug = 1,  // Developer diagnostics
    Info  = 2,  // Normal operational events
    Warn  = 3,  // Recoverable issues
    Error = 4,  // Failures requiring attention
    Fatal = 5   // Crash-imminent (logged just before crash handler fires)
};

// Initialize the logger. Call once at startup, after CrashHandler::Install().
// logDir: directory to write log files (nullptr = auto = %LOCALAPPDATA%\Aeon\logs)
void Init(const char* logDir = nullptr, Level minLevel = Level::Info);

// Shutdown — flush and close file handle.
void Shutdown();

// Core logging function. Prefer the ALOG_* macros below.
void Log(Level level, const char* category, const char* fmt, ...);

// Flush buffered output to disk immediately.
void Flush();

} // namespace AeonLog

// Convenience macros — include source location in debug builds
#define ALOG_TRACE(cat, fmt, ...) AeonLog::Log(AeonLog::Level::Trace, cat, fmt, ##__VA_ARGS__)
#define ALOG_DEBUG(cat, fmt, ...) AeonLog::Log(AeonLog::Level::Debug, cat, fmt, ##__VA_ARGS__)
#define ALOG_INFO(cat, fmt, ...)  AeonLog::Log(AeonLog::Level::Info,  cat, fmt, ##__VA_ARGS__)
#define ALOG_WARN(cat, fmt, ...)  AeonLog::Log(AeonLog::Level::Warn,  cat, fmt, ##__VA_ARGS__)
#define ALOG_ERROR(cat, fmt, ...) AeonLog::Log(AeonLog::Level::Error, cat, fmt, ##__VA_ARGS__)
#define ALOG_FATAL(cat, fmt, ...) AeonLog::Log(AeonLog::Level::Fatal, cat, fmt, ##__VA_ARGS__)
