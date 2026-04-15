// =============================================================================
// aeon_cloud.h — AeonCloud: Cloud-Assisted Rendering Service
// Graduated from: Opera Mini server-side rendering concept (clean-room)
//
// AeonCloud provides server-side rendering for resource-constrained devices.
// The "Turbo Mode" turns any AeonHive mesh node with a GPU or decent CPU
// into a rendering proxy that:
//   1. Fetches and renders the target page via headless Chromium/Blink
//   2. Compresses the visual output into AVIF/WebP tiles
//   3. Streams tiles + interactive DOM metadata to the thin client
//   4. Relays user input (tap, scroll, type) back to the renderer
//
// Target: Devices with <512MB RAM, <256kbps connections
// Result: ~30MB RAM on client, ~90% data savings
//
// Architecture:
//   Client (thin shell) <-- QUIC/WebSocket --> AeonHive Node (renderer)
//
// What we improve over Opera Mini / UCBrowser proxy:
//   [+] Fully P2P — no central proxy servers (AeonHive mesh nodes)
//   [+] E2E encrypted (Noise protocol via AeonHive)
//   [+] Community-volunteered rendering (desktop users help mobile users)
//   [+] Open-source, user-controllable, no data mining
//   [+] Graceful degradation: Turbo -> Lite -> Full (based on bandwidth)
//   [+] AVIF tiles for superior compression (vs. old server-side JPEG)
// =============================================================================

#pragma once

#include "aeon_component.h"
#include <cstdint>
#include <cstddef>
#include <functional>
#include <string>
#include <vector>

// Forward declarations
class AeonCloudImpl;

// ---------------------------------------------------------------------------
// AeonCloudMode — rendering strategy
// ---------------------------------------------------------------------------
enum class AeonCloudMode : uint8_t {
    Full    = 0,    // Local rendering (normal browser mode)
    Turbo   = 1,    // Server-side rendering via AeonHive proxy
    Lite    = 2,    // Text-only mode (absolute minimum bandwidth)
    Auto    = 3,    // Auto-detect based on bandwidth and RAM
};

// ---------------------------------------------------------------------------
// AeonTile — a compressed visual tile from the renderer
// ---------------------------------------------------------------------------
struct AeonTile {
    uint16_t x, y;              // Position in the virtual viewport
    uint16_t width, height;     // Tile dimensions (typically 256x256)
    uint8_t  format;            // 0=AVIF, 1=WebP, 2=JPEG, 3=PNG
    uint8_t  quality;           // 0-100 (adaptive based on bandwidth)
    uint32_t sequence;          // Monotonic frame sequence number
    const uint8_t* data;        // Compressed image data
    size_t   data_len;          // Image data length
    bool     is_delta;          // true = only changed pixels from previous frame
};

// ---------------------------------------------------------------------------
// AeonInteractiveElement — clickable/tappable region from DOM metadata
// ---------------------------------------------------------------------------
struct AeonInteractiveElement {
    uint32_t element_id;        // Unique ID for input relay
    uint16_t x, y, w, h;       // Bounding box in viewport coords
    uint8_t  type;              // 0=link, 1=button, 2=input, 3=select, 4=textarea
    char     hint[256];         // Accessible name / placeholder / href
    bool     is_focused;        // Currently focused element
};

// ---------------------------------------------------------------------------
// AeonPageMetadata — DOM-level info sent alongside tiles
// ---------------------------------------------------------------------------
struct AeonPageMetadata {
    char     title[512];        // Page <title>
    char     url[2048];         // Current URL (after redirects)
    char     favicon_url[512];  // Favicon URL
    uint32_t doc_width;         // Total document width (px)
    uint32_t doc_height;        // Total document height (px)
    uint32_t viewport_x;       // Current scroll position X
    uint32_t viewport_y;       // Current scroll position Y
    uint32_t interactive_count; // Number of interactive elements
    float    load_progress;     // 0.0 - 1.0
    bool     is_secure;         // HTTPS + valid cert
};

// ---------------------------------------------------------------------------
// AeonInputEvent — user interaction relayed to renderer
// ---------------------------------------------------------------------------
struct AeonInputEvent {
    enum Type : uint8_t {
        Tap         = 0,
        DoubleTap   = 1,
        LongPress   = 2,
        Scroll      = 3,
        Pinch       = 4,
        KeyPress    = 5,
        TextInput   = 6,
        Back        = 7,
        Forward     = 8,
        Navigate    = 9,    // URL navigation
    };

    Type     type;
    uint16_t x, y;              // Position (for touch events)
    int16_t  dx, dy;            // Delta (for scroll/pinch)
    float    scale;             // Scale factor (for pinch)
    uint32_t element_id;        // Target element (0 = coordinate-based)
    char     text[1024];        // Text/URL for TextInput/Navigate types
};

// ---------------------------------------------------------------------------
// AeonCloudStats — session statistics
// ---------------------------------------------------------------------------
struct AeonCloudStats {
    uint64_t bytes_downloaded;      // Total compressed data received
    uint64_t bytes_saved;           // Estimated savings vs. full page load
    float    savings_percent;       // (saved / (downloaded + saved)) * 100
    uint32_t tiles_received;        // Total tiles rendered
    uint32_t pages_rendered;        // Total pages loaded
    float    avg_latency_ms;        // Average tile delivery latency
    uint64_t session_start_utc;     // When Turbo was activated
    char     renderer_peer[65];     // Hex peer ID of rendering node
};

// ---------------------------------------------------------------------------
// AeonCloudConfig — session configuration
// ---------------------------------------------------------------------------
struct AeonCloudConfig {
    AeonCloudMode mode;               // Default rendering mode
    uint8_t       tile_quality;        // Image quality 0-100 (default: 60)
    uint16_t      tile_size;           // Tile dimensions (default: 256)
    bool          prefer_avif;         // true = AVIF, false = WebP (default: true)
    bool          enable_delta_tiles;  // true = send only changed regions
    uint32_t      max_bandwidth_kbps;  // Bandwidth cap for tile streaming (0=auto)
    bool          volunteer_renderer;  // true = this node offers rendering to others
    uint32_t      renderer_max_tabs;   // Max concurrent tabs to render (default: 3)
    uint32_t      renderer_ram_limit_mb; // RAM cap for headless renderer (default: 512)
};

// ---------------------------------------------------------------------------
// Callbacks
// ---------------------------------------------------------------------------
using AeonCloudTileCallback    = std::function<void(const AeonTile& tile)>;
using AeonCloudMetaCallback    = std::function<void(const AeonPageMetadata& meta,
                                                     const AeonInteractiveElement* elements,
                                                     size_t count)>;
using AeonCloudModeCallback    = std::function<void(AeonCloudMode old_mode,
                                                     AeonCloudMode new_mode,
                                                     const char* reason)>;

// ---------------------------------------------------------------------------
// AeonCloud — cloud-assisted rendering engine
// ---------------------------------------------------------------------------
class AeonCloud final : public AeonComponentBase {
public:
    AeonCloud();
    ~AeonCloud() override;

    // ── IAeonComponent identity ───────────────────────────────────────────
    const char* ComponentId()      const override { return "aeon.cloud"; }
    const char* ComponentVersion() const override { return "1.0.0"; }
    const char* UpstreamRef()      const override {
        return "clean-room-design (no upstream)";
    }

    // ── Lifecycle ─────────────────────────────────────────────────────────
    bool Initialize(const ResourceBudget& budget) override;
    void Shutdown() override;

    // ── Configuration ─────────────────────────────────────────────────────
    bool Configure(const AeonCloudConfig& config);
    AeonCloudMode CurrentMode() const;

    // ── Mode Switching ────────────────────────────────────────────────────

    // Switch rendering mode. Turbo requires an active AeonHive connection.
    bool SetMode(AeonCloudMode mode);

    // Register callback for automatic mode changes (e.g., Auto detection)
    void OnModeChange(AeonCloudModeCallback callback);

    // ── Turbo Mode: Client-Side API ───────────────────────────────────────

    // Navigate to a URL via the rendering proxy
    bool TurboNavigate(const char* url);

    // Send a user input event to the renderer
    bool TurboInput(const AeonInputEvent& event);

    // Request a full viewport refresh (e.g., after resize)
    void TurboRefresh();

    // Register tile reception callback (called for each visual tile)
    void OnTile(AeonCloudTileCallback callback);

    // Register metadata callback (called on page load, DOM changes)
    void OnMetadata(AeonCloudMetaCallback callback);

    // ── Turbo Mode: Renderer-Side API (for volunteer nodes) ───────────────

    // Start accepting rendering requests from peers
    bool StartRenderer();

    // Stop rendering for peers
    void StopRenderer();

    // Get current renderer load
    uint32_t ActiveRenderSessions() const;

    // ── Session Statistics ─────────────────────────────────────────────────
    AeonCloudStats SessionStats() const;

    // ── Bandwidth Detection ───────────────────────────────────────────────

    // Get estimated current bandwidth (kbps)
    uint32_t EstimatedBandwidth() const;

    // Get estimated device available RAM (MB)
    uint32_t AvailableRAM() const;

    // Check if Turbo mode is recommended for current conditions
    bool IsTurboRecommended() const;

    // ── Community Cache ───────────────────────────────────────────────────

    // Cache a rendered page for community access (shared within local mesh)
    bool CacheRenderedPage(const char* url, const AeonTile* tiles, size_t count);

    // Check if a URL is available in community cache
    bool IsCached(const char* url) const;

    // ── Resource Awareness ────────────────────────────────────────────────
    bool CanOffloadToHive() const override { return true; } // Cloud IS offload

private:
    AeonCloudImpl* m_impl = nullptr;
};

// ── Global singleton ──────────────────────────────────────────────────────────
AeonCloud& AeonCloudInstance();
