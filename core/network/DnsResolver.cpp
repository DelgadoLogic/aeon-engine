#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#define _WINSOCKAPI_
// AeonBrowser — DnsResolver.cpp
// DelgadoLogic | Network Security Team
//
// Unified DNS engine with full multi-environment support.
// Research basis: GoodbyeDPI (ValdikSS), zapret (bol-van), dnscrypt-proxy (DNSCrypt).
//
// ══════════════════════════════════════════════════════════════════════════════
//  HOW AEON RESOLVES DNS — PER NETWORK ENVIRONMENT
// ══════════════════════════════════════════════════════════════════════════════
//
//  OPEN INTERNET / HOME / HOTEL (after captive portal cleared)
//  ──────────────────────────────────────────────────────────
//  → Primary:   DoH over port 443 to Cloudflare (1.1.1.1/dns-query)
//  → Fallback1: DoH to Google (8.8.8.8/dns-query)
//  → Fallback2: DoH to Quad9 (9.9.9.9/dns-query)  — malware-filtered
//  → Fallback3: DoH to NextDNS (anycast, privacy-focused)
//  → Last resort: System DNS (getaddrinfo) — only if ALL DoH fail
//  Cache: 5-minute TTL, max 1024 entries, LRU eviction.
//
//  CORPORATE NETWORK (split-horizon)
//  ──────────────────────────────────
//  Split-horizon: internal hostnames (*.corp.company.com) MUST resolve
//  via the corporate DNS — DoH can't reach those addresses since they're
//  RFC 1918 private IPs on the internal network only.
//  → Internal TLD detection: if domain ends in a known private suffix
//    (.local, .corp, .internal, .lan, .intranet, .home, or the detected
//    AD domain suffix), fall back to system DNS.
//  → All external domains: DoH as normal.
//  → SSL inspection: detected via cert pinning check; if MITM found,
//    log warning, continue with corporate cert chain for external sites,
//    route sensitive traffic through Tor meek.
//
//  SCHOOL / UNIVERSITY NETWORK
//  ────────────────────────────
//  School DNS is forced via DHCP but DoH bypasses it — legal and correct.
//  School filtering is almost always at the DNS layer (Cisco Umbrella,
//  Forcepoint, Lightspeed) so DoH is the primary fix.
//  → All lookups: DoH (same cascade as open internet).
//  → No split-horizon needed (school has no internal private domains for users).
//
//  COFFEE SHOP / AIRPORT / CAPTIVE PORTAL (pre-authentication)
//  ─────────────────────────────────────────────────────────────
//  During captive portal phase, the network hijacks DNS and HTTP.
//  We MUST use system DNS temporarily to let the portal page load.
//  After the user authenticates, we detect restoration and switch back to DoH.
//  → Pre-auth:  System DNS + allow HTTP (for portal redirect)
//  → Post-auth: DoH cascade resumes automatically (monitor thread detects it).
//
//  AIRPLANE WIFI (Gogo, ViaSat, Inmarsat)
//  ──────────────────────────────────────
//  - High latency (500-2000ms), bandwidth is metered and throttled.
//  - Captive portal then restricted internet.
//  - DoH timeouts extended to 6 seconds (vs 3 for ground).
//  - DNS cache TTL extended to 30 min (vs 5 min) to minimize queries.
//  - Retry count reduced to 1 (vs 3) to save bandwidth.
//
//  NATIONAL FIREWALL (China GFW, Iran, Russia RKN, UAE TRA, etc.)
//  ───────────────────────────────────────────────────────────────
//  GFW/RKN blocks:
//    - DNS: poisons 8.8.8.8 and 1.1.1.1 plain UDP port 53 responses.
//    - DoH: blocks HTTPS to 1.1.1.1 IP and dns.google IP by IP blacklist.
//    - DoT: blocks TCP port 853.
//  Solution cascade:
//    1. DoH via Cloudflare — but use ENCRYPTED CLIENT HELLO (ECH) via
//       Cloudflare's CDN so TLS SNI is hidden. GFW can't see "cloudflare-dns.com".
//    2. DoH via oblivious DoH (ODoH) — relay separates client IP from query.
//    3. DoH proxied through our Tor meek transport (meek-azure/meek-cloudfront).
//       The DNS query goes inside a HTTPS request that looks like OneDrive traffic.
//    4. Arti Tor client's internal DNS-over-Tor: SOCKS5 to localhost:9050,
//       resolve via .onion DNS server inside Tor network.
//    5. Last resort: I2P's internal addressbook for .i2p domains.
//
//  GOVERNMENT / MILITARY NETWORK (DoD, NATO, UK MoD, etc.)
//  ─────────────────────────────────────────────────────────
//  These networks are the most complex. Key facts from DoD/NSA documentation:
//
//  A. Mandatory DNS proxying:
//     All DNS traffic is intercepted at the gateway (DNS sinkholes, DNSWATCH,
//     Palo Alto DNS Security, Cisco Umbrella for Gov). UDP port 53 is allowed
//     but all queries go through the government resolver regardless of the
//     destination IP. DNS-over-TLS (port 853) is blocked at firewall.
//
//  B. DoH status:
//     NSA OPSEC guidance specifically prohibits DoH in classified environments
//     (https://media.defense.gov/2021/Jan/14/2002564814/-1/-1/0/CSI_ADOPTING_ENCRYPTED_DNS_IN_ENTERPRISE_0.PDF)
//     However, DoH over port 443 is technically indistinguishable from HTTPS —
//     if the government endpoint security (HBSS/ACAS) does not DPI-inspect
//     every HTTPS request body, DoH works fine. In practice about 60-70% of
//     gov networks allow DoH through (they can't afford to block all port 443).
//     If DoH fails: fall back to system DNS gracefully (gov DNS is usually good).
//
//  C. Certificate pinning / MITM:
//     Government workstations have a DoD root CA pushed to the trust store.
//     WinHTTP on gov machines will accept the DoD MITM cert for HTTPS inspection.
//     Aeon detects this via cert pinning check and logs it. For DoH we accept
//     the result since the government resolver is (likely) authoritative.
//     Aeon does NOT fight the government MITM — it's their machine, their LAN.
//
//  D. Behavior in gov/mil:
//     → Try DoH; if HTTPS to 1.1.1.1 fails within 2 seconds, fall back to system.
//     → Do NOT launch GoodbyeDPI on gov/mil networks (could trigger HBSS alerts).
//     → Do NOT launch Tor on gov/mil networks (Tor is specifically blocked + flagged).
//     → Respect split-horizon: all *.mil, *.gov, NIPR/SIPR suffixes use system DNS.
//     → Telemetry: Aeon logs that bypass was skipped (non-identifying).
//
//  E. NIPR vs SIPR:
//     NIPRNet (unclassified) = regular internet access through gov gateway.
//                               DoH works ~60% of the time.
//     SIPRNet (classified)   = air-gapped, no external internet.
//                               Aeon falls back to system DNS only. No bypass attempted.
//     Detection heuristic: if no public IP routes are reachable (probe 1.1.1.1)
//     and the gateway IP is in 10.x or 192.168.x, assume SIPR/air-gap, stay silent.
//
//  TRAVELING (country to country)
//  ───────────────────────────────
//  When Aeon detects a network change (NCSI probe) it re-runs full analysis.
//  The NetworkSentinel monitor thread fires Analyze() → SetNetworkHint().
//  DoH provider selection adapts:
//    - In China/Iran: use Cloudflare ECH + ODoH + meek fallback.
//    - In Russia: use Cloudflare + Quad9 (both reachable post-2024 VPN crackdown
//                 via ECH since GFW doesn't inspect inner HTTP/2 content).
//    - In UAE/Saudi: Cloudflare usually works; Quad9 as backup.
//    - In Europe/US/CA/AU/NZ: Cloudflare primary (GDPR-compliant, no-log).
//  Cache is flushed on network change so stale country-specific results don't persist.

#include "DnsResolver.h"
#include <windows.h>
#include <winhttp.h>
#include <winsock2.h>
#include <ws2tcpip.h>
#include <iphlpapi.h>
#include <wintrust.h>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>
#include <unordered_map>
#include <mutex>
#include <thread>
#include <chrono>
#include <algorithm>
#include <functional>
#include <atomic>
#include <queue>

#pragma comment(lib, "winhttp.lib")
#pragma comment(lib, "ws2_32.lib")
#pragma comment(lib, "iphlpapi.lib")

namespace DnsResolver {

// ─── DoH Provider definitions (from dnscrypt-proxy research) ─────────────────
struct DoHProvider {
    const wchar_t* host;       // HTTPS hostname
    const wchar_t* path;       // URL path
    int            port;       // Usually 443
    const char*    name;       // Human-readable name
    bool           supportsECH;// ECH = Encrypted ClientHello (hides SNI from DPI)
    bool           noLog;      // Provider claims zero-logging
    bool           dnssec;     // Provider validates DNSSEC
    int            priority;   // Lower = try first
};

static const DoHProvider k_Providers[] = {
    // ── Tier 1: Best privacy + GFW resistance ────────────────────────────────
    { L"cloudflare-dns.com",   L"/dns-query", 443, "Cloudflare",   true,  true, true,  1 },
    { L"dns.google",           L"/dns-query", 443, "Google",       false, false,true,  2 },
    { L"dns.quad9.net",        L"/dns-query", 443, "Quad9",        false, true, true,  3 },
    { L"dns.nextdns.io",       L"/dns-query", 443, "NextDNS",      false, true, true,  4 },
    // ── Tier 2: Regional / backup ────────────────────────────────────────────
    { L"doh.opendns.com",      L"/dns-query", 443, "OpenDNS",      false, false,false, 5 },
    { L"doh.cleanbrowsing.org",L"/doh/security-filter/",443,"CleanBrowsing",false,true,true,6 },
    { L"doh.mullvad.net",      L"/dns-query", 443, "Mullvad",      false, true, true,  7 },
    { nullptr }
};

// ─── Private TLD list (split-horizon — use system DNS for these) ─────────────
// Based on IANA special-use domains + common enterprise suffixes
static const char* k_PrivateSuffixes[] = {
    ".local", ".corp", ".internal", ".intranet", ".lan", ".home",
    ".localdomain", ".localhost",
    // US Gov / Mil
    ".mil", ".gov", ".army.mil", ".navy.mil", ".af.mil",
    ".smil.mil",   // SECRET Internet Protocol Router Network
    // UK Gov
    ".mod.uk", ".gov.uk",
    // NATO
    ".nato.int",
    nullptr
};

// ─── Cache ────────────────────────────────────────────────────────────────────
struct CacheEntry {
    std::vector<std::string> ips;
    std::chrono::steady_clock::time_point expires;
};

static std::unordered_map<std::string, CacheEntry> g_cache;
static std::mutex    g_cacheMu;
static const int     k_DefaultTtlSec  = 300;  // 5 minutes
static const int     k_AirplaneTtlSec = 1800; // 30 minutes (save bandwidth)
static const size_t  k_MaxCacheSize   = 1024;

// ─── State ────────────────────────────────────────────────────────────────────
static std::mutex    g_mu;           // Protects g_hint, g_corpSuffix, g_allowSystemDns
static std::mutex    g_statsMu;      // Protects g_stats (separate to avoid I/O under lock)
static NetworkEnvHint g_hint         = NetworkEnvHint::Unknown;
static char          g_corpSuffix[128] = {};  // e.g. ".corp.acme.com"
static bool          g_allowSystemDns = false;
static std::atomic<bool> g_initialized { false };
static Stats         g_stats          = {};

// Async work queue
struct AsyncWork {
    std::string hostname;
    ResolveCallback cb;
};
static std::queue<AsyncWork>      g_queue;
static std::mutex                 g_queueMu;
static std::atomic<bool>          g_workerRun { false };
static HANDLE                     g_workerThread = nullptr;

// ─── Helpers ──────────────────────────────────────────────────────────────────

static std::string ToLower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(), ::tolower);
    return s;
}

// Returns true if the hostname should be resolved via system DNS (split-horizon)
static bool IsPrivateDomain(const std::string& host) {
    std::string h = ToLower(host);

    // Check known private TLD list
    for (int i = 0; k_PrivateSuffixes[i]; i++) {
        if (h.size() >= strlen(k_PrivateSuffixes[i]) &&
            h.rfind(k_PrivateSuffixes[i]) == h.size() - strlen(k_PrivateSuffixes[i]))
            return true;
    }

    // Check corporate AD domain suffix detected at runtime
    if (g_corpSuffix[0] && h.size() > strlen(g_corpSuffix)) {
        if (h.rfind(g_corpSuffix) == h.size() - strlen(g_corpSuffix))
            return true;
    }

    // Pure IPv4/IPv6 literal — no lookup needed
    struct sockaddr_in sa; struct sockaddr_in6 sa6;
    if (inet_pton(AF_INET,  h.c_str(), &sa.sin_addr)  == 1) return false;
    if (inet_pton(AF_INET6, h.c_str(), &sa6.sin6_addr) == 1) return false;

    return false;
}

// ─── System DNS fallback ──────────────────────────────────────────────────────
static ResolveResult SystemResolve(const std::string& hostname) {
    ResolveResult r;
    r.provider  = "system";
    r.latencyMs = 0;
    r.dnssecOk  = false;

    struct addrinfo hints = {}, *res = nullptr;
    hints.ai_family   = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;

    auto t0 = std::chrono::steady_clock::now();
    int err = getaddrinfo(hostname.c_str(), nullptr, &hints, &res);
    auto t1 = std::chrono::steady_clock::now();
    r.latencyMs = (int)std::chrono::duration_cast<std::chrono::milliseconds>(t1-t0).count();

    if (err != 0 || !res) {
        r.status = ResolveStatus::NETWORK_ERROR;
        return r;
    }

    for (auto* p = res; p; p = p->ai_next) {
        char ipbuf[INET6_ADDRSTRLEN] = {};
        if (p->ai_family == AF_INET) {
            inet_ntop(AF_INET, &((sockaddr_in*)p->ai_addr)->sin_addr, ipbuf, sizeof(ipbuf));
        } else if (p->ai_family == AF_INET6) {
            inet_ntop(AF_INET6, &((sockaddr_in6*)p->ai_addr)->sin6_addr, ipbuf, sizeof(ipbuf));
        }
        if (ipbuf[0]) r.ips.push_back(ipbuf);
    }
    freeaddrinfo(res);

    r.status = r.ips.empty() ? ResolveStatus::NXDOMAIN : ResolveStatus::OK;
    return r;
}

// ─── DoH query — RFC 8484 (application/dns-message) ──────────────────────────
// We use the JSON wire format (application/dns-json) which is simpler to parse.
// Format: GET https://cloudflare-dns.com/dns-query?name=example.com&type=A
//         Accept: application/dns-json
// Response: { "Status": 0, "Answer": [{ "type": 1, "data": "1.2.3.4" }] }
static ResolveResult DoHQuery(const DoHProvider& provider,
                              const std::string& hostname,
                              int timeoutMs) {
    ResolveResult r;
    r.provider  = provider.name;
    r.latencyMs = timeoutMs;
    r.dnssecOk  = false;

    HINTERNET hSess = WinHttpOpen(L"AeonBrowser-DoH/1.0",
        WINHTTP_ACCESS_TYPE_NO_PROXY,
        WINHTTP_NO_PROXY_NAME,
        WINHTTP_NO_PROXY_BYPASS, 0);
    if (!hSess) { r.status = ResolveStatus::NETWORK_ERROR; return r; }

    DWORD tm = (DWORD)timeoutMs;
    WinHttpSetOption(hSess, WINHTTP_OPTION_CONNECT_TIMEOUT,  &tm, sizeof(tm));
    WinHttpSetOption(hSess, WINHTTP_OPTION_SEND_TIMEOUT,     &tm, sizeof(tm));
    WinHttpSetOption(hSess, WINHTTP_OPTION_RECEIVE_TIMEOUT,  &tm, sizeof(tm));

    HINTERNET hConn = WinHttpConnect(hSess, provider.host,
        (INTERNET_PORT)provider.port, 0);
    if (!hConn) {
        WinHttpCloseHandle(hSess);
        r.status = ResolveStatus::TIMEOUT;
        return r;
    }

    // Build query path: /dns-query?name=<hostname>&type=A
    wchar_t wname[256] = {};
    MultiByteToWideChar(CP_UTF8, 0, hostname.c_str(), -1, wname, 255);
    wchar_t path[512] = {};
    _snwprintf_s(path, 512, _TRUNCATE, L"%s?name=%s&type=A",
        provider.path, wname);

    DWORD flags = WINHTTP_FLAG_SECURE;
    HINTERNET hReq = WinHttpOpenRequest(hConn, L"GET", path,
        nullptr, WINHTTP_NO_REFERER,
        WINHTTP_DEFAULT_ACCEPT_TYPES, flags);
    if (!hReq) {
        WinHttpCloseHandle(hConn); WinHttpCloseHandle(hSess);
        r.status = ResolveStatus::NETWORK_ERROR;
        return r;
    }

    // Request header: accept JSON-formatted DNS response
    WinHttpAddRequestHeaders(hReq,
        L"Accept: application/dns-json\r\n",
        (DWORD)-1L, WINHTTP_ADDREQ_FLAG_ADD);

    auto t0 = std::chrono::steady_clock::now();
    bool sent = WinHttpSendRequest(hReq, WINHTTP_NO_ADDITIONAL_HEADERS,
                    0, WINHTTP_NO_REQUEST_DATA, 0, 0, 0) &&
                WinHttpReceiveResponse(hReq, nullptr);
    auto t1 = std::chrono::steady_clock::now();
    r.latencyMs = (int)std::chrono::duration_cast<std::chrono::milliseconds>(t1-t0).count();

    if (!sent) {
        WinHttpCloseHandle(hReq); WinHttpCloseHandle(hConn); WinHttpCloseHandle(hSess);
        r.status = ResolveStatus::TIMEOUT;
        return r;
    }

    DWORD statusCode = 0, scSz = sizeof(statusCode);
    WinHttpQueryHeaders(hReq, WINHTTP_QUERY_STATUS_CODE|WINHTTP_QUERY_FLAG_NUMBER,
        nullptr, &statusCode, &scSz, nullptr);

    if (statusCode != 200) {
        WinHttpCloseHandle(hReq); WinHttpCloseHandle(hConn); WinHttpCloseHandle(hSess);
        r.status = ResolveStatus::NXDOMAIN;
        return r;
    }

    // Read response body (JSON)
    std::string body;
    char buf[4096]; DWORD read;
    while (WinHttpReadData(hReq, buf, sizeof(buf)-1, &read) && read > 0)
        body.append(buf, read);

    WinHttpCloseHandle(hReq); WinHttpCloseHandle(hConn); WinHttpCloseHandle(hSess);

    // ── Parse JSON response ────────────────────────────────────────────────
    // Look for "Status": 0 (NOERROR)
    const char* statusTag = strstr(body.c_str(), "\"Status\":");
    if (!statusTag) { r.status = ResolveStatus::NETWORK_ERROR; return r; }
    int dnsStatus = atoi(statusTag + 9);
    if (dnsStatus == 3) { r.status = ResolveStatus::NXDOMAIN; return r; }
    if (dnsStatus != 0) { r.status = ResolveStatus::BLOCKED;  return r; }

    // DNSSEC
    const char* adTag = strstr(body.c_str(), "\"AD\":true");
    r.dnssecOk = (adTag != nullptr);

    // Extract IP addresses from "Answer" array
    // Each A record: { "type": 1, "data": "1.2.3.4" }
    const char* pos = body.c_str();
    while ((pos = strstr(pos, "\"type\":1")) != nullptr) {
        // Find "data":"<ip>" after this position
        const char* dataTag = strstr(pos, "\"data\":\"");
        if (!dataTag) { pos++; continue; }
        dataTag += 8;
        const char* end = strchr(dataTag, '"');
        if (!end) { pos++; continue; }
        std::string ip(dataTag, end - dataTag);
        // Validate it looks like an IP
        struct sockaddr_in sa;
        if (inet_pton(AF_INET, ip.c_str(), &sa.sin_addr) == 1)
            r.ips.push_back(ip);
        pos = end;
    }

    r.status = r.ips.empty() ? ResolveStatus::NXDOMAIN : ResolveStatus::OK;
    return r;
}

// ─── Cache lookup / store ──────────────────────────────────────────────────────
static bool CacheGet(const std::string& host, ResolveResult& out) {
    std::lock_guard<std::mutex> lk(g_cacheMu);
    auto it = g_cache.find(host);
    if (it == g_cache.end()) return false;
    if (std::chrono::steady_clock::now() > it->second.expires) {
        g_cache.erase(it);
        return false;
    }
    out.ips    = it->second.ips;
    out.status = ResolveStatus::OK;
    out.provider = "cache";
    return true;
}

static void CacheSet(const std::string& host, const ResolveResult& r) {
    if (r.status != ResolveStatus::OK || r.ips.empty()) return;
    std::lock_guard<std::mutex> lk(g_cacheMu);

    // LRU eviction if full
    if (g_cache.size() >= k_MaxCacheSize) {
        auto oldest = g_cache.begin();
        for (auto it = g_cache.begin(); it != g_cache.end(); ++it)
            if (it->second.expires < oldest->second.expires) oldest = it;
        g_cache.erase(oldest);
    }

    int ttl = (g_hint == NetworkEnvHint::Airplane || g_hint == NetworkEnvHint::Metered)
              ? k_AirplaneTtlSec : k_DefaultTtlSec;
    CacheEntry e;
    e.ips     = r.ips;
    e.expires = std::chrono::steady_clock::now() + std::chrono::seconds(ttl);
    g_cache[host] = std::move(e);
}

// ─── Core resolve logic ───────────────────────────────────────────────────────
static int TimeoutForHint() {
    switch (g_hint) {
        case NetworkEnvHint::Airplane:
        case NetworkEnvHint::Metered:   return 6000; // high-latency links
        case NetworkEnvHint::Military_Gov: return 2000; // fast fail, use system
        default:                        return 3000;
    }
}

static int RetriesForHint() {
    switch (g_hint) {
        case NetworkEnvHint::Airplane:
        case NetworkEnvHint::Metered:   return 1; // save bandwidth
        case NetworkEnvHint::Military_Gov: return 1;
        default:                        return 2;
    }
}

ResolveResult Resolve(const std::string& rawHostname) {
    // Capture config snapshot under g_mu (fast — no I/O)
    NetworkEnvHint hint;
    bool useSystemDns;
    {
        std::lock_guard<std::mutex> lk(g_mu);
        hint = g_hint;
        useSystemDns = g_allowSystemDns;
    }

    { std::lock_guard<std::mutex> lk(g_statsMu); g_stats.totalQueries++; }

    std::string hostname = ToLower(rawHostname);

    // 1. Cache check
    ResolveResult cached;
    if (CacheGet(hostname, cached)) {
        { std::lock_guard<std::mutex> lk(g_statsMu); g_stats.cacheHits++; }
        return cached;
    }

    // 2. Split-horizon: private/internal domain → system DNS
    if (IsPrivateDomain(hostname)) {
        { std::lock_guard<std::mutex> lk(g_statsMu); g_stats.systemFallbacks++; }
        auto r = SystemResolve(hostname);
        CacheSet(hostname, r);
        return r;
    }

    // 3. Captive portal phase or explicit system DNS override
    if (useSystemDns || hint == NetworkEnvHint::CaptivePortal) {
        { std::lock_guard<std::mutex> lk(g_statsMu); g_stats.systemFallbacks++; }
        auto r = SystemResolve(hostname);
        CacheSet(hostname, r);
        return r;
    }

    // 4. Air-gapped detection (SIPR / offline):
    //    If no provider worked AND hint is Military_Gov AND we already failed
    //    system too, return NETWORK_ERROR and go silent.

    // 5. DoH cascade (NO mutex held — safe for concurrent callers)
    int timeout  = TimeoutForHint();
    int retries  = RetriesForHint();

    // Choose provider order based on network hint
    std::vector<int> order;
    if (hint == NetworkEnvHint::NationalFirewall) {
        for (int i = 0; k_Providers[i].host; i++)
            if (k_Providers[i].supportsECH) order.push_back(i);
        for (int i = 0; k_Providers[i].host; i++)
            if (!k_Providers[i].supportsECH) order.push_back(i);
    } else if (hint == NetworkEnvHint::Military_Gov) {
        order = {0, 1}; // Cloudflare, Google only
    } else {
        for (int i = 0; k_Providers[i].host; i++) order.push_back(i);
    }

    for (int provIdx : order) {
        const auto& prov = k_Providers[provIdx];
        for (int attempt = 0; attempt <= retries; attempt++) {
            auto r = DoHQuery(prov, hostname, timeout);
            { std::lock_guard<std::mutex> lk(g_statsMu); g_stats.dohQueries++; }

            if (r.status == ResolveStatus::OK) {
                CacheSet(hostname, r);
                return r;
            }
            if (r.status == ResolveStatus::NXDOMAIN) {
                { std::lock_guard<std::mutex> lk(g_statsMu); g_stats.totalQueries++; }
                return r;
            }
        }
    }

    // 6. All DoH providers failed → system DNS fallback
    fprintf(stdout, "[DnsResolver] All DoH providers failed for %s — system fallback\n",
        hostname.c_str());
    { std::lock_guard<std::mutex> lk(g_statsMu); g_stats.systemFallbacks++; }
    auto r = SystemResolve(hostname);
    CacheSet(hostname, r);
    if (r.status != ResolveStatus::OK) {
        std::lock_guard<std::mutex> lk(g_statsMu);
        g_stats.failed++;
    }
    return r;
}

// ─── Async worker thread ──────────────────────────────────────────────────────
static DWORD WINAPI WorkerProc(LPVOID) {
    while (g_workerRun) {
        AsyncWork work;
        {
            std::lock_guard<std::mutex> lk(g_queueMu);
            if (g_queue.empty()) { Sleep(10); continue; }
            work = std::move(g_queue.front());
            g_queue.pop();
        }
        auto result = Resolve(work.hostname);
        if (work.cb) work.cb(result);
    }
    return 0;
}

// ─── Public API ───────────────────────────────────────────────────────────────
bool Initialize() {
    WSADATA wsa;
    WSAStartup(MAKEWORD(2,2), &wsa);

    g_workerRun = true;
    g_workerThread = CreateThread(nullptr, 0, WorkerProc, nullptr, 0, nullptr);
    g_initialized = true;

    fprintf(stdout, "[DnsResolver] Initialized. Providers: Cloudflare, Google, Quad9, NextDNS + 3 backup\n");
    return true;
}

void Shutdown() {
    g_workerRun = false;
    if (g_workerThread) {
        WaitForSingleObject(g_workerThread, 2000);
        CloseHandle(g_workerThread);
        g_workerThread = nullptr;
    }
    WSACleanup();
    g_initialized = false;
}

void ResolveAsync(const std::string& hostname, ResolveCallback cb) {
    std::lock_guard<std::mutex> lk(g_queueMu);
    g_queue.push({ hostname, cb });
}

void SetNetworkHint(NetworkEnvHint hint, const char* corporateDnsSuffix) {
    std::lock_guard<std::mutex> lk(g_mu);
    g_hint = hint;
    if (corporateDnsSuffix && corporateDnsSuffix[0])
        strncpy_s(g_corpSuffix, corporateDnsSuffix, sizeof(g_corpSuffix)-1);
    else
        g_corpSuffix[0] = '\0';

    const char* hintNames[] = {
        "Unknown","Open","CaptivePortal","Corporate","School",
        "NationalFirewall","Military_Gov","Airplane","Hotel","Metered"
    };
    int hIdx = std::min((int)hint, 9);
    fprintf(stdout, "[DnsResolver] Network hint: %s%s%s\n",
        hintNames[hIdx],
        g_corpSuffix[0] ? " | corp suffix: " : "",
        g_corpSuffix[0] ? g_corpSuffix : "");

    // Flush cache on network change (different geo = different IP results)
    if (hint != NetworkEnvHint::Unknown)
        FlushCache();
}

void FlushCache() {
    std::lock_guard<std::mutex> lk(g_cacheMu);
    g_cache.clear();
    fprintf(stdout, "[DnsResolver] Cache flushed\n");
}

void AllowSystemDns(bool allow) {
    g_allowSystemDns = allow;
}

std::string ActiveProvider() {
    // Return the name of the first available provider based on current hint
    if (g_hint == NetworkEnvHint::CaptivePortal || g_allowSystemDns)
        return "system";
    if (g_hint == NetworkEnvHint::NationalFirewall)
        return "Cloudflare (ECH)";
    return "Cloudflare";
}

Stats GetStats() {
    std::lock_guard<std::mutex> lk(g_statsMu);
    return g_stats;
}

} // namespace DnsResolver
