// =============================================================================
// aeon_cast.h — AeonCast Public API
// Graduated from: mjansson/mdns (Public Domain / Unlicense)
//
// What we improve over upstream mDNS:
//   [+] Non-blocking async I/O: IOCP on Vista+, WSAAsyncSelect on XP
//       (upstream is entirely synchronous — blocks the calling thread)
//   [+] Persistent device cache — re-uses last-known device list on resume
//   [+] Multi-interface scanning — enumerates all network adapters, not just one
//   [+] Native Chromecast V2 protocol — CASTV2/TLS over socket (our IP)
//   [+] Hive device registry — crowd-sourced device model DB
//   [+] Sovereign protocol updates — Cast V2 rule changes pushed via manifest
//   [+] XP-safe: no IOCP fallback path uses WSAAsyncSelect + window message queue
// =============================================================================

#pragma once

#include "../core/aeon_component.h"
#include <functional>
#include <string>
#include <vector>

// ---------------------------------------------------------------------------
// AeonCastDeviceType — what kind of device was found
// ---------------------------------------------------------------------------
enum class AeonCastDeviceType {
    ChromecastGen1 = 0,
    ChromecastGen2,
    ChromecastUltra,
    ChromecastAudio,
    AndroidTV,
    SmartTV,      // generic DIAL-compatible
    AirPlay,      // Apple AirPlay target
    Unknown
};

// ---------------------------------------------------------------------------
// AeonCastDevice — a discovered Cast-capable device
// ---------------------------------------------------------------------------
struct AeonCastDevice {
    char   name[128];           // friendly name ("Living Room TV")
    char   id[64];              // Chromecast UUID
    char   model[64];           // "Chromecast Ultra", etc.
    char   ip[46];              // IPv4 or IPv6 string
    char   mac[18];             // "aa:bb:cc:dd:ee:ff" (if known)
    uint16_t port;              // default 8009 for Cast
    AeonCastDeviceType type;
    float    signal_strength;   // 0.0–1.0 (estimated from mDNS TTL)
    bool     is_cached;         // true if from persistent cache, not live scan
    bool     supports_hdr;
    bool     supports_4k;
    uint64_t last_seen_utc;     // unix timestamp
};

// ---------------------------------------------------------------------------
// AeonCastState — current state of a Cast session
// ---------------------------------------------------------------------------
enum class AeonCastState {
    Idle = 0,
    Connecting,
    Connected,
    Casting,
    Paused,
    Error,
    Disconnected
};

// ---------------------------------------------------------------------------
// Callbacks
// ---------------------------------------------------------------------------
using AeonCastDiscoveryCallback = std::function<void(
    const AeonCastDevice& device,
    bool                  appeared   // false = device disappeared
)>;

using AeonCastSessionCallback = std::function<void(
    AeonCastState state,
    const char*   message  // optional detail string
)>;

// ---------------------------------------------------------------------------
// AeonCastSession — active Cast session to one device
// ---------------------------------------------------------------------------
class AeonCastSession {
public:
    virtual ~AeonCastSession() = default;
    virtual bool CastURL(const char* url)            = 0;
    virtual bool CastTab(uint32_t tab_id)            = 0;
    virtual bool Pause()                             = 0;
    virtual bool Resume()                            = 0;
    virtual bool Stop()                              = 0;
    virtual bool SetVolume(float vol)                = 0; // 0.0–1.0
    virtual AeonCastState GetState() const           = 0;
    virtual const AeonCastDevice& GetDevice() const  = 0;
    virtual void SetCallback(AeonCastSessionCallback cb) = 0;
};

// ---------------------------------------------------------------------------
// AeonCast — main discovery + session manager
// ---------------------------------------------------------------------------
class AeonCastImpl;

class AeonCast final : public AeonComponentBase {
public:
    AeonCast();
    ~AeonCast() override;

    // ── IAeonComponent identity ───────────────────────────────────────────
    const char* ComponentId()      const override { return "aeon.cast"; }
    const char* ComponentVersion() const override { return "1.0.0"; }
    const char* UpstreamRef()      const override { return "mjansson/mdns@public-domain"; }

    // ── Lifecycle ─────────────────────────────────────────────────────────
    bool Initialize(const ResourceBudget& budget) override;
    void Shutdown() override;

    // ── Discovery ─────────────────────────────────────────────────────────

    // Begin async mDNS discovery on all network interfaces.
    // Fires callback each time a device appears or disappears.
    // Returns false if networking unavailable (XP without WinSock2).
    bool StartDiscovery(AeonCastDiscoveryCallback callback);

    // Stop discovery (does not disconnect active sessions)
    void StopDiscovery();

    // Force a single immediate scan (useful on tab open)
    void ScanNow();

    // Return all devices seen in last scan (includes cached)
    std::vector<AeonCastDevice> GetKnownDevices() const;

    // Clear device cache (forces fresh discovery on next ScanNow)
    void ClearCache();

    // ── Session Management ────────────────────────────────────────────────

    // Connect to a device and return a session handle.
    // Returns nullptr if connection fails.
    std::unique_ptr<AeonCastSession> Connect(
        const AeonCastDevice& device,
        AeonCastSessionCallback state_callback
    );

    // ── Hive Device Registry ──────────────────────────────────────────────

    // Map Chromecast model string to AeonCastDeviceType using crowd-sourced DB.
    // Hive peers contribute model strings → DB updated via sovereign manifest.
    AeonCastDeviceType ResolveDeviceType(const char* model_string) const;

    // ── Resource Awareness ────────────────────────────────────────────────
    bool CanOffloadToHive() const override { return false; } // discovery is local

private:
    AeonCastImpl* m_impl = nullptr;
};

// ── Global singleton ──────────────────────────────────────────────────────────
AeonCast& AeonCastInstance();
