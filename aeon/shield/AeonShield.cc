// AeonShield.cc — Runtime enforcement layer
// Intercepts navigation, network requests, and API calls
// to enforce AeonDNA sovereignty at runtime (not just at compile time).

#pragma once
#include <string>
#include <vector>
#include <regex>

namespace aeon {
namespace shield {

// ─── Blocked GCP / Google cloud endpoints ────────────────────────────────────
// These are the endpoints Gemini, Safe Browsing, and Sync use.
// Intercepted at the NetworkDelegate layer before any request leaves the device.
static const std::vector<std::string> kBlockedEndpoints = {
    // Gemini AI
    "generativelanguage.googleapis.com",
    "bard.google.com",
    "gemini.google.com",
    // Safe Browsing (server-side model)
    "safebrowsing.googleapis.com",
    "sb-ssl.google.com",
    // Chrome Sync
    "clients4.google.com",
    "clients6.google.com",
    "chrome-sync.googleapis.com",
    // UMA / Crash reporting
    "clients2.google.com/cr/report",
    "uma.googleapis.com",
    // Google account / OAuth forced promos
    "accounts.google.com/embedded",
    // Auto Browse AI backend (if ever deployed)
    "autobrowse.googleapis.com",
    // Field trial / Finch (prevents Google from re-enabling stripped features)
    "finch.googleapis.com",
    "clients2.google.com/service/update2",  // except handled by sovereign updater
};

// ─── WebMCP restriction: block remote MCP servers ────────────────────────────
// WebMCP is adopted, but ONLY for local tool servers (127.0.0.1 / localhost).
// Remote MCP endpoints are treated as surveillance vectors.
static bool IsMCPAllowed(const std::string& url) {
    return url.find("127.0.0.1") != std::string::npos ||
           url.find("localhost") != std::string::npos ||
           url.find("::1") != std::string::npos;
}

// ─── DBSC enforcement ─────────────────────────────────────────────────────────
// Device Bound Session Credentials must only apply to non-Google origins.
// (Ironic if DBSC were used to bind sessions TO Google.)
static bool ShouldEnableDBSC(const std::string& origin) {
    // Skip binding for Google-owned origins since they're stripped anyway.
    if (origin.find(".google.com") != std::string::npos) return false;
    if (origin.find(".googleapis.com") != std::string::npos) return false;
    return true;
}

// ─── AeonShield::ShouldBlockRequest (called by NetworkDelegate) ──────────────
inline bool ShouldBlockRequest(const std::string& url) {
    for (const auto& endpoint : kBlockedEndpoints) {
        if (url.find(endpoint) != std::string::npos) {
            return true;  // Silently dropped — no error page, no retry
        }
    }
    return false;
}

} // namespace shield
} // namespace aeon
