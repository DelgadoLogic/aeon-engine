// AeonBrowser — CircumventionEngine.h
#pragma once

namespace CircumventionEngine {

    enum class CircumventionLayer {
        None = 0,
        ECH,           // Layer 1: Encrypted Client Hello (SNI hiding)
        DoH,           // Layer 2: DNS-over-HTTPS only
        TorBridge,     // Layer 3: Tor + pluggable transport
        Shadowsocks,   // Layer 4: Shadowsocks + V2Ray obfuscation
        Psiphon        // Layer 5: Psiphon VPN (last resort)
    };

    enum class TransportType {
        obfs4 = 0,     // Randomizes byte patterns, no DPI fingerprint
        Meek,          // Traffic looks like Cloudflare/Azure CDN HTTPS
        Snowflake,     // Traffic looks like WebRTC video call
        Count
    };

    struct CircumventionState {
        bool                enabled          = false;
        CircumventionLayer  layer            = CircumventionLayer::None;
        char                active_transport[32] = {};  // "obfs4", "meek", etc.
        int                 local_socks_port = 0;       // 0 = Arti handles routing
        bool                doh_active       = false;
        bool                ech_active       = false;
    };

    // Enable Firewall Mode. Probes all 5 layers in sequence.
    // ssUri: optional "ss://..." URI for Layer 4 (Shadowsocks).
    // Returns true if at least one layer is active.
    bool Enable(const char* ssUri = nullptr);

    // Disable Firewall Mode and kill all child processes.
    void Disable();

    // Returns current state (which layer is active, which transport, etc.)
    const CircumventionState& GetState();
}
