// AeonBrowser — Breadcrumbs.h
// DelgadoLogic | Senior Security Engineer
//
// Lock-free circular ring buffer that records the last N events leading up
// to a crash. Written from any thread; read by the crash handler.
//
// Each breadcrumb is a short event string + timestamp delta (seconds before
// crash). The buffer is fixed-size, stack-allocated, and safe to read from
// inside an exception handler.
//
// USAGE:
//   AeonCrash::AddBreadcrumb("tab.new", "https://example.com");
//   AeonCrash::AddBreadcrumb("navigation.commit", nullptr);
//   AeonCrash::AddBreadcrumb("ipc.command", "tab.close");
//
// At crash time, CrashHandler reads all breadcrumbs ordered newest-first
// and writes them into crash_report.json.

#pragma once
#include <cstdint>

namespace AeonCrash {

static constexpr int kMaxBreadcrumbs = 50;
static constexpr int kMaxEventLen    = 64;
static constexpr int kMaxDetailLen   = 192;

struct Breadcrumb {
    char     event[kMaxEventLen];      // e.g. "tab.new", "navigation.commit"
    char     detail[kMaxDetailLen];    // e.g. URL, command name, or empty
    uint64_t timestamp_ms;             // GetTickCount64() at time of recording
    volatile bool occupied;
};

// Record a breadcrumb. Thread-safe. Overwrites oldest entry when full.
void AddBreadcrumb(const char* event, const char* detail = nullptr);

// Read-only access for crash handler. Returns pointer to static array.
// Use GetBreadcrumbHead() to know where the newest entry is.
const Breadcrumb* GetBreadcrumbs();

// Get the index of the most recently written breadcrumb.
int GetBreadcrumbHead();

// Get the uptime in seconds since first breadcrumb was written.
double GetUptimeSeconds();

} // namespace AeonCrash
