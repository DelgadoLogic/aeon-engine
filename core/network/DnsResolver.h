// AeonBrowser — DnsResolver.h
// DelgadoLogic | Network Security Team
//
// Unified DNS resolution engine. Every DNS lookup in Aeon goes through here.
// Never uses the OS resolver (getaddrinfo) for privacy-sensitive lookups —
// that would leak hostnames to whatever DNS server the network operator chose.
#pragma once
#include <windows.h>
#include <functional>
#include <string>
#include <vector>

namespace DnsResolver {

// ─── Result ──────────────────────────────────────────────────────────────────
enum class ResolveStatus {
    OK,               // Resolved successfully
    NXDOMAIN,         // Domain does not exist
    BLOCKED,          // Filtered by network (returned NXDOMAIN/loopback for known reasons)
    TIMEOUT,          // All providers timed out
    NETWORK_ERROR,    // No connectivity at all
    SPLIT_HORIZON,    // Corporate internal domain — must use network DNS
    CAPTIVE_PORTAL,   // Captive portal is hijacking DNS responses
};

struct ResolveResult {
    ResolveStatus        status  = ResolveStatus::NETWORK_ERROR;
    std::vector<std::string> ips;     // One or more A records
    std::string          provider;    // Which DoH provider answered
    int                  latencyMs;   // Round-trip time for this query
    bool                 dnssecOk;    // DNSSEC validation passed (if supported)
};

// ─── Environment override ─────────────────────────────────────────────────────
// Injected by NetworkSentinel after it classifies the network.
enum class NetworkEnvHint {
    Unknown,
    Open,               // Regular internet
    CaptivePortal,      // Must use system DNS until portal clears
    Corporate,          // Has split-horizon; internal domains need system DNS
    School,             // DNS filtered but no split-horizon; full DoH is fine
    NationalFirewall,   // DoH over port 443 is critical; use ECH-capable providers
    Military_Gov,       // May have mandatory DNS proxy; DoH tunneled via port 443
                        // also watch for DNSSEC-enforcing resolvers
    Airplane,           // Gogo/ViaSat portal; captive + throttled
    Hotel,              // Similar to captive, often TCP-only on port 80/443
    Metered,            // Satellite / mobile; minimize DNS retries
};

// ─── Async callback ───────────────────────────────────────────────────────────
using ResolveCallback = std::function<void(const ResolveResult&)>;

// ─── Public API ───────────────────────────────────────────────────────────────

// Call once at browser startup.
bool Initialize();
void Shutdown();

// Synchronous resolve (blocks calling thread — use on worker threads only).
ResolveResult Resolve(const std::string& hostname);

// Asynchronous resolve (fires callback on a threadpool thread).
void ResolveAsync(const std::string& hostname, ResolveCallback cb);

// Tell the resolver what kind of network we're on (called by NetworkSentinel).
void SetNetworkHint(NetworkEnvHint hint, const char* corporateDnsSuffix = nullptr);

// Flush the in-memory DNS cache (call on network change).
void FlushCache();

// Returns the name of the currently active DoH provider.
std::string ActiveProvider();

// For captive portal phase: temporarily allow system DNS for connectivity checks.
void AllowSystemDns(bool allow);

// Statistics
struct Stats {
    unsigned long long totalQueries;
    unsigned long long cacheHits;
    unsigned long long dohQueries;
    unsigned long long systemFallbacks;  // times we fell back to system DNS
    unsigned long long blocked;          // domains blocked by network
    unsigned long long failed;
};
Stats GetStats();

} // namespace DnsResolver
