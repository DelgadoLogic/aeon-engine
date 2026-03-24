// AeonBrowser — NetworkSentinel.h
#pragma once
#include <windows.h>

namespace NetworkSentinel {

    enum class NetworkType {
        Open = 0,         // Normal internet — nothing to bypass
        CoffeeShop,       // Captive portal (café/public WiFi)
        Hotel,            // Captive portal (hotel/airport)
        Corporate,        // Enterprise network (proxy + possible SSL inspection)
        School,           // School/university (transparent proxy + category filter)
        NationalFirewall, // China GFW / Iran / Russia / etc.
        ISP_Throttle      // ISP-level throttle (Russia YouTube/Telegram TSPU)
    };

    enum class DpiMode {
        Universal = 0,    // Minimal — just fragment TLS ClientHello
        ISP_SNI,          // SNI-based filter (school, café router)
        Corporate_HTTP,   // Corporate transparent proxy (HTTP Host scramble)
        Russia_TSPU,      // Russian TSPU boxes (Zapret mode)
        China_GFW         // China Great Firewall (max aggression)
    };

    struct CaptivePortalResult {
        bool reachable       = false;
        bool portal_detected = false;
        char portal_url[512] = {};
    };

    struct NetworkEnvironment {
        NetworkType type             = NetworkType::Open;
        bool        internet_ok      = false;
        bool        captive_portal   = false;
        bool        has_corporate_proxy = false;
        bool        ssl_intercepted  = false;   // Corporate MITM detected
        bool        port_443_ok      = false;
        bool        port_80_ok       = false;
        bool        port_22_ok       = false;
        char        proxy_server[256] = {};
        char        portal_url[512]  = {};
    };

    struct SentinelState {
        bool        bypass_active       = false;
        bool        need_captive_portal = false; // UI must open portal page
        bool        accept_corporate_ca = false; // Accept corp MITM cert
        char        captive_portal_url[512] = {};
        HANDLE      dpi_process         = nullptr; // goodbyedpi.exe / winws.exe
    };

    // Run full network probe and classify the environment.
    // Call once at startup, or after a network change event.
    NetworkEnvironment Analyze();

    // Apply the best bypass strategy based on Analyze() result.
    // Call immediately after Analyze().
    void ApplyBestStrategy();

    // Start a background thread that re-probes every 30s and re-applies
    // strategy if the network environment changes (e.g., moved from café to VPN).
    void StartMonitor();
    void StopMonitor();

    // Accessors for current state
    const NetworkEnvironment& GetEnvironment();
    const SentinelState&      GetState();
}
