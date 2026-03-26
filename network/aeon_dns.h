// =============================================================================
// aeon_dns.h — AeonDNS Public API
// Graduated from: dnscrypt-proxy (ISC) + GoodbyeDPI (Apache 2.0) + zapret (MIT)
//
// AeonDNS is the sovereign DNS and DPI circumvention engine.
// Replaces Chrome's built-in DNS entirely at the browser-process level.
//
// What we build vs upstream:
//   [+] DoH (DNS-over-HTTPS) to user-chosen resolvers (default: Cloudflare/Quad9)
//   [+] DoT (DNS-over-TLS) fallback
//   [+] dnscrypt (DNSCrypt v2 protocol) — ISC licensed, safe to use
//   [+] GoodbyeDPI bypass: DPI fragment + TTL tricks at raw socket level
//   [+] Sovereign bridge config: censored regions get bridge lists via Hive
//   [+] Ad/tracker DNS blocking: sovereign-signed blocklist
//   [+] ESNI/ECH support: Encrypted Client Hello hides SNI from ISPs
//   [+] XP-safe: WinDivert on XP for packet interception (GoodbyeDPI approach)
//   [+] Zero phone-home: all resolver choices local, bridge lists from Hive
//
// License strategy:
//   dnscrypt-proxy: ISC (permissive) — we implement the DNSCrypt v2 protocol
//   GoodbyeDPI: Apache 2.0 — study DPI bypass techniques, implement our own
//   zapret: MIT — same
//   psiphon: GPL ❌ — study architecture ONLY, do not copy code
// =============================================================================

#pragma once

#include "aeon_component.h"
#include <functional>
#include <string>
#include <vector>

// ---------------------------------------------------------------------------
// AeonDNSResolver — a DNS resolver endpoint
// ---------------------------------------------------------------------------
struct AeonDNSResolver {
    char   name[64];        // "Cloudflare DoH", "Quad9 DoT", etc.
    char   url[256];        // "https://1.1.1.1/dns-query" or "tls://9.9.9.9"
    char   protocol[8];     // "doh", "dot", "dnscrypt", "plain"
    bool   supports_ech;    // Encrypted Client Hello
    float  latency_ms;      // measured latency (updated periodically)
    bool   active;
};

// ---------------------------------------------------------------------------
// AeonDNSRecord — resolved DNS record
// ---------------------------------------------------------------------------
struct AeonDNSRecord {
    char     hostname[256];
    char     ip[46];         // IPv4 or IPv6
    uint32_t ttl_seconds;
    bool     from_cache;
    bool     blocked;        // true = matched blocklist
    char     blocked_reason[64]; // "ad", "tracker", "malware"
};

// ---------------------------------------------------------------------------
// AeonDPIConfig — DPI circumvention settings
// ---------------------------------------------------------------------------
struct AeonDPIConfig {
    bool enabled;
    bool fragment_http;     // Split HTTP requests across multiple TCP segments
    bool fragment_https;    // Split TLS ClientHello across segments
    bool set_ttl;           // Use low TTL trick to bypass DPI
    bool wrong_checksum;    // GoodbyeDPI fake packet trick
    bool fake_sni;          // Send fake SNI in parallel decoy packet
    int  fragment_size;     // bytes per fragment (default 2)
    int  ttl_value;         // TTL for fake packet (default 4)
};

// ---------------------------------------------------------------------------
// Callbacks
// ---------------------------------------------------------------------------
using AeonDNSCallback     = std::function<void(const AeonDNSRecord& record)>;
using AeonDNSBlockCallback = std::function<void(const char* hostname, const char* reason)>;

// ---------------------------------------------------------------------------
// AeonDNS — sovereign DNS + DPI bypass engine
// ---------------------------------------------------------------------------
class AeonDNSImpl;

class AeonDNS final : public AeonComponentBase {
public:
    AeonDNS();
    ~AeonDNS() override;

    // ── IAeonComponent identity ───────────────────────────────────────────
    const char* ComponentId()      const override { return "aeon.dns"; }
    const char* ComponentVersion() const override { return "1.0.0"; }
    const char* UpstreamRef()      const override {
        return "dnscrypt-proxy@ISC + GoodbyeDPI@Apache2 + zapret@MIT";
    }

    // ── Lifecycle ─────────────────────────────────────────────────────────
    bool Initialize(const ResourceBudget& budget) override;
    void Shutdown() override;

    // ── Resolver Management ───────────────────────────────────────────────

    // Set active resolver. Called from settings UI.
    bool SetResolver(const AeonDNSResolver& resolver);

    // Get built-in resolver presets
    std::vector<AeonDNSResolver> BuiltinResolvers() const;

    // Add a custom resolver
    void AddCustomResolver(const AeonDNSResolver& resolver);

    // Benchmark all resolvers and auto-select fastest
    void AutoSelectFastest(std::function<void(const AeonDNSResolver&)> done);

    // ── DNS Resolution ────────────────────────────────────────────────────

    // Async DNS resolution (replaces Chrome's HostResolver)
    void Resolve(const char* hostname, AeonDNSCallback callback);

    // Synchronous (for test/diagnostic use only)
    AeonDNSRecord ResolveSync(const char* hostname);

    // Flush DNS cache
    void FlushCache();

    // ── Blocklist Management ──────────────────────────────────────────────

    // Load a sovereign-signed blocklist (delivered via AeonHive manifest)
    bool LoadBlocklist(const char* path);

    // Check if hostname is blocked
    bool IsBlocked(const char* hostname) const;

    // Register callback for when a hostname is blocked
    void OnBlocked(AeonDNSBlockCallback callback);

    // Temporarily allow a blocked hostname (user override)
    void AllowOnce(const char* hostname);

    // ── DPI Circumvention ─────────────────────────────────────────────────

    // Enable/configure DPI bypass (for censored regions)
    // Config is delivered via Hive CircumventBridge topic
    bool SetDPIConfig(const AeonDPIConfig& config);
    AeonDPIConfig GetDPIConfig() const;

    // Apply packet-level DPI bypass for a connection
    // Called by the network stack for each new TCP connection
    bool ApplyDPIBypass(uint32_t socket_fd, const char* hostname);

    // ── Hive Bridge Discovery ─────────────────────────────────────────────

    // Called by AeonHive when new bridge configs arrive on CircumventBridge topic
    void OnBridgeConfigUpdate(const uint8_t* config_data, size_t len);

    // Get current bridge addresses (for regions with heavy censorship)
    std::vector<std::string> GetBridgeAddresses() const;

    // ── ECH / ESNI ───────────────────────────────────────────────────────

    // Fetch ECH keys for a hostname from DNS (draft-ietf-tls-esni)
    void FetchECHKeys(const char* hostname,
                      std::function<void(const std::vector<uint8_t>& keys)> callback);

    // ── Resource Awareness ────────────────────────────────────────────────
    bool CanOffloadToHive() const override { return false; }

private:
    AeonDNSImpl* m_impl = nullptr;
};

// ── Global singleton ──────────────────────────────────────────────────────────
AeonDNS& AeonDNSInstance();
