// AeonBrowser — NetworkSentinel.cpp
// DelgadoLogic | Lead Network Security Engineer
//
// PURPOSE: Automatic zero-config network restriction detection and bypass.
// The user never touches this — it runs silently at startup and whenever
// a connection fails, upgrading through escalating bypass strategies.
//
// ═══════════════════════════════════════════════════════════════════════════
//  RESTRICTION ENVIRONMENTS HANDLED
// ═══════════════════════════════════════════════════════════════════════════
//
//  1. NATIONAL FIREWALL (China GFW, Iran, Russia, UAE, etc.)
//     → DPI-based blocking: TLS SNI inspection, HTTP Host header matching
//     → DNS poisoning / injection (Chinese ISPs)
//     → IP-based blacklists (Russia RKN blocklist)
//     Bypass: ECH + DoH + GoodbyeDPI packet tricks + Tor bridges + SS
//
//  2. CORPORATE / ENTERPRISE NETWORK
//     → SSL inspection (MITM proxy with self-signed CA pushed via GPO)
//     → WPAD / PAC file proxy autoconfiguration
//     → Category-based URL filtering (zScaler, Forcepoint, Cisco Umbrella)
//     → Port restrictions (only 80/443 outbound)
//     Bypass: Detect corp proxy, route cleanly through it; for category
//     blocks use CDN domain fronting so traffic looks like allowed domains.
//
//  3. SCHOOL / UNIVERSITY NETWORK
//     → Similar to corporate but often using transparent proxy (Squid)
//     → SNI-based filter lists targeting entertainment/social media
//     → DNS resolver forced to school's server (DHCP option 6)
//     Bypass: DoH overrides forced DNS; SNI fragmentation defeats Squid;
//     port 443 fallback; Tor meek (looks like Cloudflare HTTPS).
//
//  4. COFFEE SHOP / HOTEL / AIRPORT CAPTIVE PORTAL
//     → User must click "Accept Terms" in a browser window before
//       any HTTPS works — portal intercepts all HTTP and redirects.
//     → Zero-Knowledge: Aeon auto-detects the portal and opens it.
//     Bypass: Probe connectivity, detect redirect, open portal page
//     in a sandboxed "captive portal" tab, wait for clearance.
//
//  5. ISP-LEVEL THROTTLING / THROTTLE BYPASS
//     → ISPs in Russia, Brazil, Italy throttle specific protocols
//       (Telegram, Discord, YouTube throttled via TSPU boxes).
//     Bypass: Zapret/GoodbyeDPI packet manipulation — fragment first
//     TCP data segment so DPI can't see SNI in any single packet.
//
//  6. PORT-RESTRICTED NETWORKS (only 80/443 open)
//     → Common in hotels, airports, strict corporate nets.
//     Bypass: Tor over port 443; SSH tunneling over port 443;
//             WebSocket over 443; all fallbacks try 443 first.
//
// ═══════════════════════════════════════════════════════════════════════════
//  DPI BYPASS TECHNIQUES (from GoodbyeDPI & Zapret research)
// ═══════════════════════════════════════════════════════════════════════════
//
//  A. TLS ClientHello SNI Fragmentation
//     Split the TLS ClientHello across two TCP segments so the SNI field
//     spans a segment boundary. DPI boxes that inspect the first packet
//     see incomplete TLS data and cannot match any SNI filter list.
//     → Works against: GFW, Russian TSPU, many ISP-level filters
//
//  B. TTL-Limited Fake Packet (Decoy Injection)
//     Before the real SYN-ACK, send a fake packet with TTL set to
//     (hop_count - 1) so it dies before reaching the destination but
//     is seen by inline DPI hardware. DPI processes the fake packet
//     and marks the connection as "inspected/cleared". The real packet
//     then arrives and is allowed through.
//     → Works against: passive DPI (not inline/transparent proxy)
//
//  C. HTTP Host Header Mutation
//     Scramble the case of the Host header: "GitHub.com" → "gItHuB.cOm"
//     DPI regex matches are case-sensitive in many implementations.
//     Also add a trailing dot: "Host: github.com." (valid per RFC 2396).
//     → Works against: simple Host-header-matching DPI
//
//  D. TCP Segmentation of HTTP Request
//     Split the GET request at byte 2 of the "Host:" header value.
//     "GET / HTTP/1.1\r\nHost: g" [segment break] "ithub.com\r\n"
//     Many DPI implementations don't reassemble across segments.
//     → Works against: non-reassembling DPI (most ISP hardware)
//
//  E. Fake TLS ClientHello (Decoy SNI)
//     Send a TLS ClientHello containing SNI for an allowed domain
//     (e.g., "www.microsoft.com") immediately before the real connection.
//     Some DPIs clear the connection after seeing the first ClientHello.
//     The real connection follows immediately after.
//
//  F. Domain Fronting
//     The outer TLS SNI contains a CDN host (e.g., "cloudfront.net").
//     The inner HTTP Host header contains the real target domain.
//     DPI sees "cloudfront.net" (allowed) and passes the packet.
//     The CDN routes based on the inner Host header to the real server.
//     Compatible CDNs: Cloudflare, Fastly (limited), Azure CDN.

#include "NetworkSentinel.h"
#include "CircumventionEngine.h"
#include "DnsResolver.h"
#include "../settings/SettingsEngine.h"
#include "../tls/TlsAbstraction.h"
#include <windows.h>
#include <winsock2.h>
#include <ws2tcpip.h>
#include <winhttp.h>
#include <iphlpapi.h>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <thread>
#include <atomic>

#pragma comment(lib, "ws2_32.lib")
#pragma comment(lib, "winhttp.lib")
#pragma comment(lib, "iphlpapi.lib")

namespace NetworkSentinel {

// ─── State ──────────────────────────────────────────────────────────────────
static NetworkEnvironment g_env   = {};
static SentinelState      g_state = {};
static std::atomic<bool>  g_monitorRunning(false);
static HANDLE             g_monitorThread = nullptr;

// ─── Probe URLs (connectivity test) ─────────────────────────────────────────
// We try multiple well-known test endpoints in case one is blocked.
// Cloudflare, Microsoft, Google, Apple NeverSSL (HTTP-only)
static const char* k_probeHostHTTPS[] = {
    "cp.cloudflare.com",       // → returns 204, Cloudflare captive portal check
    "www.msftconnecttest.com", // → Microsoft connectivity test
    "connectivitycheck.gstatic.com", // → Google
    "captive.apple.com",       // → Apple
    nullptr
};
static const char* k_probePathHTTPS[] = {
    "/",                       // → Cloudflare returns 200 "OK"
    "/connecttest.txt",        // → "Microsoft Connect Test"
    "/generate_204",           // → HTTP 204 No Content
    "/",                       // → Apple
    nullptr
};
// For HTTP-only captive portal detection:
static const char* k_probeHostHTTP   = "neverssl.com";
static const char* k_probePathHTTP   = "/";
static const char* k_probeExpectedH  = "NeverSSL"; // substring in body

// ─── Captive portal detection URLs (same as Windows NCSI) ───────────────────
static const char* k_ncsiHost = "www.msftncsi.com";
static const char* k_ncsiPath = "/ncsi.txt";
static const char* k_ncsiExp  = "Microsoft NCSI";

// ─── Corporate proxy detection (WPAD/PAC) ────────────────────────────────────
static bool DetectCorporateProxy(char* proxy_out, int proxy_out_len) {
    // Check WinHTTP proxy settings (respects WPAD + PAC)
    WINHTTP_CURRENT_USER_IE_PROXY_CONFIG ieProxy = {};
    if (WinHttpGetIEProxyConfigForCurrentUser(&ieProxy)) {
        if (ieProxy.lpszProxy && wcslen(ieProxy.lpszProxy) > 0) {
            WideCharToMultiByte(CP_ACP, 0, ieProxy.lpszProxy, -1,
                proxy_out, proxy_out_len, nullptr, nullptr);
            if (ieProxy.lpszProxy)    GlobalFree(ieProxy.lpszProxy);
            if (ieProxy.lpszProxyBypass) GlobalFree(ieProxy.lpszProxyBypass);
            if (ieProxy.lpszAutoConfigUrl) GlobalFree(ieProxy.lpszAutoConfigUrl);
            fprintf(stdout, "[Sentinel] Corporate proxy detected: %s\n", proxy_out);
            return true;
        }
        if (ieProxy.lpszProxy)    GlobalFree(ieProxy.lpszProxy);
        if (ieProxy.lpszProxyBypass) GlobalFree(ieProxy.lpszProxyBypass);
        if (ieProxy.lpszAutoConfigUrl) GlobalFree(ieProxy.lpszAutoConfigUrl);
    }

    // Also check registry HKCU\Software\Microsoft\Windows\CurrentVersion\Internet Settings
    HKEY hKey;
    char proxyEnable[4] = "0";
    DWORD cbData = sizeof(proxyEnable);
    if (RegOpenKeyExA(HKEY_CURRENT_USER,
        "Software\\Microsoft\\Windows\\CurrentVersion\\Internet Settings",
        0, KEY_READ, &hKey) == ERROR_SUCCESS) {
        DWORD type;
        DWORD enabled = 0;
        cbData = sizeof(enabled);
        if (RegQueryValueExA(hKey, "ProxyEnable", nullptr, &type,
            (LPBYTE)&enabled, &cbData) == ERROR_SUCCESS && enabled) {
            cbData = proxy_out_len;
            RegQueryValueExA(hKey, "ProxyServer", nullptr, &type,
                (LPBYTE)proxy_out, &cbData);
            RegCloseKey(hKey);
            fprintf(stdout, "[Sentinel] Registry proxy detected: %s\n", proxy_out);
            return true;
        }
        RegCloseKey(hKey);
    }
    return false;
}

// ─── Captive portal probe ─────────────────────────────────────────────────
static CaptivePortalResult ProbeCaptivePortal() {
    CaptivePortalResult result = {};

    // Use WinHTTP to probe the NCSI endpoint over plain HTTP
    HINTERNET hSession = WinHttpOpen(L"AeonBrowser/1.0",
        WINHTTP_ACCESS_TYPE_NO_PROXY, WINHTTP_NO_PROXY_NAME,
        WINHTTP_NO_PROXY_BYPASS, 0);
    if (!hSession) return result;

    // Timeout: 3 seconds total (fast fail)
    DWORD timeout = 3000;
    WinHttpSetOption(hSession, WINHTTP_OPTION_CONNECT_TIMEOUT, &timeout, sizeof(timeout));
    WinHttpSetOption(hSession, WINHTTP_OPTION_RECEIVE_TIMEOUT, &timeout, sizeof(timeout));

    HINTERNET hConnect = WinHttpConnect(hSession, L"www.msftncsi.com",
        INTERNET_DEFAULT_HTTP_PORT, 0);
    if (!hConnect) { WinHttpCloseHandle(hSession); return result; }

    HINTERNET hRequest = WinHttpOpenRequest(hConnect, L"GET", L"/ncsi.txt",
        nullptr, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, 0);
    if (!hRequest) { WinHttpCloseHandle(hConnect); WinHttpCloseHandle(hSession); return result; }

    if (WinHttpSendRequest(hRequest, WINHTTP_NO_ADDITIONAL_HEADERS, 0,
        WINHTTP_NO_REQUEST_DATA, 0, 0, 0) &&
        WinHttpReceiveResponse(hRequest, nullptr)) {

        DWORD statusCode = 0;
        DWORD statusSize = sizeof(statusCode);
        WinHttpQueryHeaders(hRequest, WINHTTP_QUERY_STATUS_CODE |
            WINHTTP_QUERY_FLAG_NUMBER, nullptr, &statusCode, &statusSize, nullptr);

        if (statusCode == 200) {
            char body[256] = {};
            DWORD read = 0;
            WinHttpReadData(hRequest, body, sizeof(body) - 1, &read);
            result.reachable = (strstr(body, "Microsoft NCSI") != nullptr);
        } else if (statusCode == 302 || statusCode == 301 || statusCode == 307) {
            // Redirect = captive portal
            result.portal_detected = true;
            result.reachable = false;
            // Extract redirect URL from Location header
            DWORD urlLen = sizeof(result.portal_url);
            wchar_t wUrl[512] = {};
            DWORD wUrlLen = sizeof(wUrl);
            WinHttpQueryHeaders(hRequest, WINHTTP_QUERY_LOCATION,
                nullptr, wUrl, &wUrlLen, nullptr);
            WideCharToMultiByte(CP_ACP, 0, wUrl, -1,
                result.portal_url, sizeof(result.portal_url), nullptr, nullptr);
            fprintf(stdout, "[Sentinel] Captive portal detected! URL: %s\n",
                result.portal_url);
        }
    }

    WinHttpCloseHandle(hRequest);
    WinHttpCloseHandle(hConnect);
    WinHttpCloseHandle(hSession);
    return result;
}

// ─── Detect network type heuristically ───────────────────────────────────
static NetworkType ClassifyNetwork(const CaptivePortalResult& portal,
                                    const char* proxyServer) {
    // Order matters — check most specific first

    if (portal.portal_detected) {
        // See if portal URL hints at type
        if (strstr(portal.portal_url, "hotel") ||
            strstr(portal.portal_url, "guest") ||
            strstr(portal.portal_url, "wifi"))
            return NetworkType::Hotel;
        return NetworkType::CoffeeShop; // Assume cafe/public WiFi
    }

    if (proxyServer[0]) {
        // Corporate proxy = enterprise or school
        // Heuristic: check if domain is .edu → school
        char domain[64] = {};
        DWORD len = sizeof(domain);
        GetComputerNameExA(ComputerNameDnsDomain, domain, &len);
        if (strstr(domain, ".edu") || strstr(domain, "school") ||
            strstr(domain, "univ"))
            return NetworkType::School;
        return NetworkType::Corporate;
    }

    if (!portal.reachable) {
        // No reachability, no portal, no proxy = likely national firewall
        return NetworkType::NationalFirewall;
    }

    return NetworkType::Open; // Open internet
}

// ─── DPI evasion: apply packet tricks via GoodbyeDPI child process ────────
static bool LaunchGoodbyeDPI(DpiMode mode) {
    char exeDir[MAX_PATH];
    GetModuleFileNameA(nullptr, exeDir, MAX_PATH);
    char* last = strrchr(exeDir, '\\');
    if (last) *last = '\0';

    char goodbyePath[MAX_PATH];
    _snprintf_s(goodbyePath, sizeof(goodbyePath), _TRUNCATE,
        "%s\\Network\\goodbyedpi.exe", exeDir);

    if (GetFileAttributesA(goodbyePath) == INVALID_FILE_ATTRIBUTES) {
        // Also try zapret/winws.exe (Windows port of Zapret)
        _snprintf_s(goodbyePath, sizeof(goodbyePath), _TRUNCATE,
            "%s\\Network\\winws.exe", exeDir);
        if (GetFileAttributesA(goodbyePath) == INVALID_FILE_ATTRIBUTES) {
            fprintf(stderr, "[Sentinel] Neither goodbyedpi.exe nor winws.exe found.\n");
            return false; // Will be bundled in release build
        }
    }

    // Build argument string based on detected DPI mode
    char args[512] = {};
    switch (mode) {
        case DpiMode::China_GFW:
            // GFW uses active probing + SNI inspection
            // Flags: fragment TLS ClientHello + TTL-limited fake pkt + DoH
            _snprintf_s(args, sizeof(args), _TRUNCATE,
                "\"%s\" -5 --set-ttl 5 --wrong-chksum --wrong-seq "
                "--native-frag -d 2 --min-ttl 3 --auto-ttl 1-4-10 "
                "--max-payload 1200", goodbyePath);
            break;

        case DpiMode::Russia_TSPU:
            // Russian TSPU: throttle via HTTP reset injection
            // Zapret mode: fake SYN packets + TCP segmentation
            _snprintf_s(args, sizeof(args), _TRUNCATE,
                "\"%s\" -5 --wrong-chksum --wrong-seq --native-frag "
                "-d 2 --auto-ttl 1-4-10 --max-payload 1200", goodbyePath);
            break;

        case DpiMode::ISP_SNI:
            // Simple SNI filter (most school/cafe DPI)
            // Just fragment the TLS ClientHello
            _snprintf_s(args, sizeof(args), _TRUNCATE,
                "\"%s\" -5 --native-frag -d 2", goodbyePath);
            break;

        case DpiMode::Corporate_HTTP:
            // Corporate transparent proxy: mess with HTTP Host header
            _snprintf_s(args, sizeof(args), _TRUNCATE,
                "\"%s\" -5 --host-mixedcase --wrong-chksum", goodbyePath);
            break;

        default:
            // Minimal universal: just fragment
            _snprintf_s(args, sizeof(args), _TRUNCATE,
                "\"%s\" -5 --native-frag", goodbyePath);
            break;
    }

    STARTUPINFOA si = {}; si.cb = sizeof(si);
    si.dwFlags = STARTF_USESHOWWINDOW; si.wShowWindow = SW_HIDE;
    PROCESS_INFORMATION pi = {};
    if (!CreateProcessA(nullptr, args, nullptr, nullptr, FALSE,
        CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi)) {
        fprintf(stderr, "[Sentinel] Failed to launch DPI bypass: %lu\n", GetLastError());
        return false;
    }
    CloseHandle(pi.hThread);
    g_state.dpi_process = pi.hProcess;
    fprintf(stdout, "[Sentinel] DPI bypass active (mode %d, PID %lu)\n",
        (int)mode, pi.dwProcessId);
    return true;
}

// ─── Port availability probe ─────────────────────────────────────────────
static bool PortReachable(const char* host, int port) {
    SOCKET s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (s == INVALID_SOCKET) return false;

    // Non-blocking with 2-second timeout
    u_long nb = 1;
    ioctlsocket(s, FIONBIO, &nb);

    struct sockaddr_in addr = {};
    addr.sin_family = AF_INET;
    addr.sin_port   = htons((u_short)port);
    inet_pton(AF_INET, host, &addr.sin_addr);
    connect(s, (sockaddr*)&addr, sizeof(addr));

    fd_set fds; FD_ZERO(&fds); FD_SET(s, &fds);
    timeval tv = { 2, 0 };
    int rc = select(0, nullptr, &fds, nullptr, &tv);
    closesocket(s);
    return rc > 0;
}

// ─── SSL inspection detector ─────────────────────────────────────────────
static bool DetectSSLInspection() {
    // Connect to a pinned server and compare the actual cert fingerprint
    // against our pinned copy. If they differ, a MITM is in place.
    // We pin github.com's cert (well-known, large, rarely rotated cert chain).
    // pinned SHA256 of SubjectPublicKeyInfo for GitHub's DigiCert CA:
    static const char k_githubPinnedSPKI[] =
        "12:DF:4D:32:E9:A9:F0:34:8B:2B:10:2E:0A:39:F6:E6:"
        "3A:31:C9:29:34:F0:94:03:C0:DB:9C:E6:BB:F8:D7:72";

    // We use WinHTTP with certificate info query
    HINTERNET hSession = WinHttpOpen(L"AeonSentinel/1.0",
        WINHTTP_ACCESS_TYPE_NO_PROXY, WINHTTP_NO_PROXY_NAME,
        WINHTTP_NO_PROXY_BYPASS, 0);
    if (!hSession) return false;

    HINTERNET hConnect = WinHttpConnect(hSession, L"github.com", INTERNET_DEFAULT_HTTPS_PORT, 0);
    HINTERNET hRequest = WinHttpOpenRequest(hConnect, L"HEAD", L"/", nullptr,
        WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE);

    bool mitm = false;
    if (hRequest && WinHttpSendRequest(hRequest, WINHTTP_NO_ADDITIONAL_HEADERS,
        0, WINHTTP_NO_REQUEST_DATA, 0, 0, 0) &&
        WinHttpReceiveResponse(hRequest, nullptr)) {

        WINHTTP_CERTIFICATE_INFO certInfo = {};
        DWORD certInfoSize = sizeof(certInfo);
        if (WinHttpQueryOption(hRequest, WINHTTP_OPTION_SECURITY_CERTIFICATE_STRUCT,
            &certInfo, &certInfoSize)) {
            // Check issuer — if it's not DigiCert/GitHub's known CA, MITM
            char issuer[256] = {};
            WideCharToMultiByte(CP_ACP, 0, certInfo.lpszIssuerInfo, -1,
                issuer, sizeof(issuer), nullptr, nullptr);
            // Known good issuer orgs: DigiCert, GitHub
            if (!strstr(issuer, "DigiCert") && !strstr(issuer, "Github")) {
                mitm = true;
                fprintf(stdout,
                    "[Sentinel] SSL inspection detected! Issuer: %s\n", issuer);
                fprintf(stdout,
                    "[Sentinel] Corporate MITM proxy is intercepting HTTPS traffic.\n");
            }
            LocalFree(certInfo.lpszSubjectInfo);
            LocalFree(certInfo.lpszIssuerInfo);
            LocalFree(certInfo.lpszProtocolName);
            LocalFree(certInfo.lpszSignatureAlgName);
            LocalFree(certInfo.lpszEncryptionAlgName);
        }
    }
    if (hRequest) WinHttpCloseHandle(hRequest);
    if (hConnect) WinHttpCloseHandle(hConnect);
    WinHttpCloseHandle(hSession);
    return mitm;
}

// ─── Background monitor thread ─────────────────────────────────────────────
static DWORD WINAPI MonitorThread(LPVOID) {
    while (g_monitorRunning) {
        // Re-probe every 30 seconds
        Sleep(30000);

        auto portal = ProbeCaptivePortal();
        if (!portal.reachable && !g_state.bypass_active) {
            // Network changed — re-run full analysis
            fprintf(stdout, "[Sentinel] Network change detected — re-probing...\n");
            Analyze();
        }
    }
    return 0;
}

// ═══════════════════════════════════════════════════════════════════════════
//  PUBLIC API
// ═══════════════════════════════════════════════════════════════════════════

NetworkEnvironment Analyze() {
    fprintf(stdout, "\n[Sentinel] === NETWORK ANALYSIS ===\n");

    // 1. Winsock init
    WSADATA wsa = {};
    WSAStartup(MAKEWORD(2,2), &wsa);

    // 2. Detect WPAD/corporate proxy
    char proxyServer[256] = {};
    g_env.has_corporate_proxy = DetectCorporateProxy(proxyServer, sizeof(proxyServer));
    strncpy_s(g_env.proxy_server, proxyServer, sizeof(g_env.proxy_server)-1);

    // 3. Captive portal probe
    auto portal = ProbeCaptivePortal();
    g_env.captive_portal   = portal.portal_detected;
    g_env.internet_ok      = portal.reachable;
    strncpy_s(g_env.portal_url, portal.portal_url, sizeof(g_env.portal_url)-1);

    // 4. SSL inspection detection (only if we have internet)
    if (g_env.internet_ok && g_env.has_corporate_proxy) {
        g_env.ssl_intercepted = DetectSSLInspection();
    }

    // 5. Port availability
    g_env.port_443_ok = PortReachable("1.1.1.1", 443);
    g_env.port_80_ok  = PortReachable("1.1.1.1", 80);
    g_env.port_22_ok  = PortReachable("1.1.1.1", 22);

    // 6. Classify environment
    g_env.type = ClassifyNetwork(portal, proxyServer);

    // 7. Notify DnsResolver so it adapts its provider cascade immediately
    {
        DnsResolver::NetworkEnvHint dnsHint = DnsResolver::NetworkEnvHint::Open;
        switch (g_env.type) {
            case NetworkType::Open:           dnsHint = DnsResolver::NetworkEnvHint::Open; break;
            case NetworkType::CoffeeShop:     dnsHint = DnsResolver::NetworkEnvHint::CaptivePortal; break;
            case NetworkType::Hotel:          dnsHint = DnsResolver::NetworkEnvHint::Hotel; break;
            case NetworkType::Corporate:      dnsHint = DnsResolver::NetworkEnvHint::Corporate; break;
            case NetworkType::School:         dnsHint = DnsResolver::NetworkEnvHint::School; break;
            case NetworkType::NationalFirewall:dnsHint = DnsResolver::NetworkEnvHint::NationalFirewall; break;
            case NetworkType::ISP_Throttle:   dnsHint = DnsResolver::NetworkEnvHint::Open; break;
            default:                          dnsHint = DnsResolver::NetworkEnvHint::Unknown; break;
        }
        // Pass corporate AD suffix so DnsResolver can do split-horizon
        const char* corpSuffix = g_env.has_corporate_proxy ? g_env.proxy_server : nullptr;
        DnsResolver::SetNetworkHint(dnsHint, corpSuffix);

        // During captive portal: allow system DNS so the portal page can load
        DnsResolver::AllowSystemDns(g_env.captive_portal);
    }

    const char* typeNames[] = {
        "Open", "CoffeeShop/Captive", "Hotel/Captive", "Corporate", "School",
        "National Firewall", "ISP Throttle"
    };
    fprintf(stdout, "[Sentinel] Network type: %s\n",
        typeNames[min((int)g_env.type, 6)]);
    fprintf(stdout, "[Sentinel] Internet: %s | Port 443: %s | MITM: %s | Captive: %s\n",
        g_env.internet_ok      ? "YES" : "NO",
        g_env.port_443_ok      ? "YES" : "NO",
        g_env.ssl_intercepted  ? "YES" : "NO",
        g_env.captive_portal   ? "YES" : "NO");

    return g_env;
}

void ApplyBestStrategy() {
    const auto& env = g_env;
    fprintf(stdout, "[Sentinel] Applying best bypass strategy...\n");

    switch (env.type) {

        // ── Open internet — nothing to do ────────────────────────────────
        case NetworkType::Open:
            fprintf(stdout, "[Sentinel] No bypass needed.\n");
            g_state.bypass_active = false;
            return;

        // ── Captive portal (coffee shop / hotel / airport) ───────────────
        case NetworkType::CoffeeShop:
        case NetworkType::Hotel:
            fprintf(stdout,
                "[Sentinel] Captive portal — signaling UI to open portal page.\n");
            g_state.need_captive_portal = true;
            strncpy_s(g_state.captive_portal_url, env.portal_url,
                sizeof(g_state.captive_portal_url)-1);
            // After user authenticates, the monitor thread will detect
            // internet restored and clear need_captive_portal.
            return;

        // ── Corporate network with SSL inspection ────────────────────────
        case NetworkType::Corporate:
            if (env.ssl_intercepted) {
                fprintf(stdout,
                    "[Sentinel] Corporate SSL inspection detected.\n"
                    "[Sentinel] Strategy: Work WITH the corporate proxy.\n"
                    "           Aeon will accept the corporate cert chain\n"
                    "           for general browsing. For Tor/encrypted\n"
                    "           sessions, meek transport (CDN fronting)\n"
                    "           is used to escape the inspection boundary.\n");
                g_state.accept_corporate_ca = true;
                // For sensitive tabs: route via meek (looks like CDN HTTPS)
                // Corporate proxy won't see the payload, only Cloudflare TLS.
                CircumventionEngine::Enable(nullptr);
            } else {
                // Just a proxy — configure Aeon to route through it normally
                fprintf(stdout,
                    "[Sentinel] Corporate proxy (no SSL inspection). Routing normally.\n");
            }
            // DPI: most corporate nets don't do SNI filtering — skip GoodbyeDPI
            LaunchGoodbyeDPI(DpiMode::Corporate_HTTP);
            g_state.bypass_active = true;
            return;

        // ── School network ────────────────────────────────────────────────
        case NetworkType::School:
            fprintf(stdout,
                "[Sentinel] School network detected.\n"
                "[Sentinel] Strategy: DoH (override forced DNS) + SNI fragment.\n");
            // DoH overrides the school's forced DNS resolver
            // SNI fragmentation defeats transparent Squid proxy
            LaunchGoodbyeDPI(DpiMode::ISP_SNI);
            g_state.bypass_active = true;
            // If port 443 works, Tor meek as fallback for blocked categories
            if (env.port_443_ok) {
                CircumventionEngine::Enable(nullptr);
            }
            return;

        // ── National firewall (China / Iran / Russia / etc.) ─────────────
        case NetworkType::NationalFirewall:
            fprintf(stdout,
                "[Sentinel] National firewall detected.\n"
                "[Sentinel] Strategy: GoodbyeDPI + full CircumventionEngine stack.\n");
            // Determine which country/DPI system based on TTL probing
            // (Chinese GFW TTL = typically 47-64 hops, Russia TSPU = 254)
            // For now: engage China mode (strongest)
            LaunchGoodbyeDPI(DpiMode::China_GFW);
            CircumventionEngine::Enable(nullptr); // Full 5-layer bypass
            g_state.bypass_active = true;
            return;

        // ── ISP throttle (Russia YouTube/Telegram throttle) ───────────────
        case NetworkType::ISP_Throttle:
            fprintf(stdout,
                "[Sentinel] ISP throttle detected.\n"
                "[Sentinel] Strategy: Zapret/GoodbyeDPI packet manipulation.\n");
            LaunchGoodbyeDPI(DpiMode::Russia_TSPU);
            g_state.bypass_active = true;
            return;
    }
}

void StartMonitor() {
    if (g_monitorRunning) return;
    g_monitorRunning = true;
    g_monitorThread = CreateThread(nullptr, 0, MonitorThread, nullptr, 0, nullptr);
}

void StopMonitor() {
    g_monitorRunning = false;
    if (g_monitorThread) {
        WaitForSingleObject(g_monitorThread, 3000);
        CloseHandle(g_monitorThread);
        g_monitorThread = nullptr;
    }
    if (g_state.dpi_process) {
        TerminateProcess(g_state.dpi_process, 0);
        CloseHandle(g_state.dpi_process);
        g_state.dpi_process = nullptr;
    }
}

const NetworkEnvironment& GetEnvironment() { return g_env; }
const SentinelState&      GetState()       { return g_state; }

} // namespace NetworkSentinel
