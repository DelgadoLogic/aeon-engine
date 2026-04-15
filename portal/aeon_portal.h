// =============================================================================
// aeon_portal.h — AeonPortal: Zero-Rating & MNO Partnership Engine
// Graduated from: Opera Mini Data Saver concepts (clean-room)
//
// AeonPortal enables Aeon Browser to operate in zero-rated or data-capped
// environments common in emerging markets. It provides:
//
//   1. MNO Header Injection: Adds carrier-specific X-headers that trigger
//      zero-rating on partner networks (e.g., MTN Free Zone, Airtel Zero).
//
//   2. Data Budget Engine: Tracks data usage per session, per app, and per
//      billing cycle. Alerts users before exhausting prepaid data.
//
//   3. Compression Proxy: Routes traffic through Aeon's sovereign proxy
//      infrastructure (or P2P proxies via AeonHive) for image/text compression.
//
//   4. Portal Landing Page: Captive portal-style landing page that MNOs can
//      brand with their logo/colors, providing a zero-cost entry point.
//
//   5. Sponsored Content Rail: Non-intrusive sponsored content that finances
//      zero-rating agreements (user opt-in, transparent, no tracking).
//
// Market rationale:
//   - 73% of internet users in Sub-Saharan Africa use prepaid mobile data
//   - Average data cost: $2.67/GB (vs. $0.68 global average)
//   - Zero-rating partnerships directly unlock TAM in these markets
//   - Opera Mini demonstrated this model generates >$50M ARR
//
// What we improve over Opera Mini:
//   [+] Sovereign proxy: no single point of failure (AeonHive mesh as backup)
//   [+] Transparent data tracking: users see exactly what's used
//   [+] MNO SDK agnostic: works via HTTP header injection (no carrier SDK)
//   [+] P2P compression: nearby peers can serve cached pages for free
//   [+] No ad tracking: sponsored content uses context-only targeting
// =============================================================================

#pragma once

#include "aeon_component.h"
#include <cstdint>
#include <cstddef>
#include <functional>
#include <string>
#include <vector>

// Forward declarations
class AeonPortalImpl;

// ---------------------------------------------------------------------------
// AeonMNO — Mobile Network Operator identification
// ---------------------------------------------------------------------------
struct AeonMNO {
    uint16_t mcc;                       // Mobile Country Code (e.g., 621 = Nigeria)
    uint16_t mnc;                       // Mobile Network Code (e.g., 30 = MTN)
    char     name[64];                  // Human-readable name ("MTN Nigeria")
    char     brand[32];                 // Brand name for UI ("MTN")
    char     country[3];                // ISO 3166-1 alpha-2 ("NG")

    // Zero-rating configuration
    char     zero_rate_header[64];      // X-header name (e.g., "X-MTN-Free-Zone")
    char     zero_rate_value[128];      // Header value (Aeon partner ID)
    char     zero_rate_domain[128];     // Domain pattern for zero-rated traffic
    bool     zero_rate_enabled;         // Whether this MNO supports zero-rating
};

// ---------------------------------------------------------------------------
// AeonDataBudget — per-session and per-cycle data tracking
// ---------------------------------------------------------------------------
struct AeonDataBudget {
    // Current session
    uint64_t session_bytes_up;          // Bytes uploaded this session
    uint64_t session_bytes_down;        // Bytes downloaded this session
    uint64_t session_bytes_saved;       // Bytes saved by compression this session
    uint64_t session_start_utc;         // Session start timestamp

    // Billing cycle
    uint64_t cycle_bytes_up;            // Bytes uploaded this cycle
    uint64_t cycle_bytes_down;          // Bytes downloaded this cycle
    uint64_t cycle_bytes_saved;         // Bytes saved this cycle
    uint64_t cycle_start_utc;           // Cycle start timestamp
    uint64_t cycle_end_utc;             // Cycle end timestamp

    // Budget limits
    uint64_t cycle_budget_bytes;        // User-set data budget (0 = unlimited)
    float    usage_percent;             // cycle_bytes_down / cycle_budget_bytes * 100

    // Zero-rated traffic (doesn't count against budget)
    uint64_t zero_rated_bytes;          // Bytes received via zero-rating
};

// ---------------------------------------------------------------------------
// AeonCompressionMode — data compression levels
// ---------------------------------------------------------------------------
enum class AeonCompressionMode : uint8_t {
    None        = 0,                    // No compression (full quality)
    Light       = 1,                    // Images: 80% quality, text: gzip only
    Standard    = 2,                    // Images: 50% quality, JS: minified
    Aggressive  = 3,                    // Images: 20% quality, video: disabled
    TextOnly    = 4,                    // No images/video, text + basic CSS only
};

// ---------------------------------------------------------------------------
// AeonProxyRoute — how traffic is routed for compression
// ---------------------------------------------------------------------------
enum class AeonProxyRoute : uint8_t {
    Direct      = 0,                    // No proxy (standard browsing)
    AeonProxy   = 1,                    // Route through Aeon sovereign proxy
    PeerProxy   = 2,                    // Route through AeonHive peer (cached)
    MNOProxy    = 3,                    // Route through MNO-provided proxy
};

// ---------------------------------------------------------------------------
// AeonSponsoredContent — non-intrusive sponsored content item
// ---------------------------------------------------------------------------
struct AeonSponsoredContent {
    char     id[32];                    // Unique content ID
    char     title[128];               // Content title
    char     description[256];          // Short description
    char     url[512];                  // Landing URL
    char     image_url[512];            // Thumbnail URL
    char     sponsor_name[64];          // Sponsor name (transparent)
    char     category[32];              // Content category (context targeting)
    uint64_t impression_start_utc;      // Valid from
    uint64_t impression_end_utc;        // Valid until
    bool     user_dismissed;            // User dismissed this content
};

// ---------------------------------------------------------------------------
// AeonPortalConfig — configuration for the zero-rating engine
// ---------------------------------------------------------------------------
struct AeonPortalConfig {
    // Compression
    AeonCompressionMode compression;    // Default compression level
    AeonProxyRoute      proxy_route;    // Default routing

    // Data budget
    uint64_t budget_bytes;              // Monthly data budget (0 = unlimited)
    uint32_t budget_warning_percent;    // Warn at this usage % (default: 80)
    uint32_t budget_critical_percent;   // Critical alert at this % (default: 95)
    bool     auto_compress_on_critical; // Auto-switch to Aggressive at critical

    // Zero-rating
    bool     enable_zero_rating;        // Try to detect and use zero-rating
    bool     enable_sponsored;          // Show sponsored content rail

    // Proxy
    char     proxy_host[128];           // Aeon sovereign proxy hostname
    uint16_t proxy_port;                // Proxy port (default: 8443)
    bool     proxy_tls;                 // Use TLS for proxy connection

    // MNO detection
    bool     auto_detect_mno;           // Auto-detect carrier on Android
};

// ---------------------------------------------------------------------------
// AeonPortalStats — aggregated portal statistics
// ---------------------------------------------------------------------------
struct AeonPortalStats {
    uint64_t total_bytes_saved;         // Lifetime bytes saved by compression
    uint64_t total_zero_rated_bytes;    // Lifetime zero-rated bytes
    uint32_t total_pages_compressed;    // Pages served with compression
    uint32_t total_images_compressed;   // Images recompressed
    uint32_t total_sponsored_shown;     // Sponsored content impressions
    uint32_t total_sponsored_clicked;   // Sponsored content clicks
    float    avg_compression_ratio;     // Average compression ratio (0-1)
    float    data_savings_usd;          // Estimated money saved (based on local rates)
};

// ---------------------------------------------------------------------------
// Callbacks
// ---------------------------------------------------------------------------
using AeonBudgetWarningCallback = std::function<void(
    const AeonDataBudget& budget, uint32_t percent)>;

using AeonMNODetectedCallback = std::function<void(
    const AeonMNO& mno)>;

using AeonSponsoredContentCallback = std::function<void(
    const std::vector<AeonSponsoredContent>& content)>;

// ---------------------------------------------------------------------------
// AeonPortal — Zero-Rating & MNO Partnership Engine
// ---------------------------------------------------------------------------
class AeonPortal : public IAeonComponent {
public:
    AeonPortal();
    ~AeonPortal() override;

    // Non-copyable
    AeonPortal(const AeonPortal&) = delete;
    AeonPortal& operator=(const AeonPortal&) = delete;

    // ── IAeonComponent ───────────────────────────────────────────────────

    const char* Name() const override { return "AeonPortal"; }
    const char* Version() const override { return "1.0.0"; }
    bool Initialize() override;
    void Shutdown() override;

    // ── Configuration ────────────────────────────────────────────────────

    void Configure(const AeonPortalConfig& cfg);
    AeonPortalConfig GetConfig() const;

    // ── MNO Detection ────────────────────────────────────────────────────

    // Detect current mobile carrier (Android: TelephonyManager, others: IP geo)
    bool DetectMNO();

    // Get the currently detected MNO
    AeonMNO GetCurrentMNO() const;

    // Register known MNO partners (loaded from sovereign manifest)
    void RegisterMNOPartner(const AeonMNO& mno);

    // Check if current network supports zero-rating
    bool IsZeroRated() const;

    // Get zero-rating HTTP headers to inject into requests
    // Returns: vector of {header_name, header_value} pairs
    std::vector<std::pair<std::string, std::string>> GetZeroRateHeaders() const;

    // ── Data Budget ──────────────────────────────────────────────────────

    // Set monthly data budget
    void SetBudget(uint64_t bytes_per_cycle, uint32_t cycle_days = 30);

    // Record data usage (called by network layer on every request)
    void RecordUsage(uint64_t bytes_up, uint64_t bytes_down, bool zero_rated);

    // Get current budget status
    AeonDataBudget GetBudget() const;

    // Reset cycle (start new billing period)
    void ResetCycle();

    // Set budget warning callback
    void OnBudgetWarning(AeonBudgetWarningCallback cb);

    // ── Compression ──────────────────────────────────────────────────────

    // Set compression mode
    void SetCompressionMode(AeonCompressionMode mode);
    AeonCompressionMode GetCompressionMode() const;

    // Compress an image buffer (returns compressed buffer)
    std::vector<uint8_t> CompressImage(
        const uint8_t* data, size_t len,
        const char* mime_type,
        AeonCompressionMode mode = AeonCompressionMode::Standard);

    // Get compression stats for current page
    struct PageCompressionStats {
        uint64_t original_bytes;
        uint64_t compressed_bytes;
        uint32_t images_compressed;
        float    ratio;
    };
    PageCompressionStats GetPageStats() const;

    // ── Proxy Routing ────────────────────────────────────────────────────

    // Set the proxy route
    void SetProxyRoute(AeonProxyRoute route);
    AeonProxyRoute GetProxyRoute() const;

    // Check if a URL should use the compression proxy
    bool ShouldProxy(const char* url) const;

    // Get the proxy URL for a given target URL
    std::string GetProxyURL(const char* target_url) const;

    // ── Sponsored Content ────────────────────────────────────────────────

    // Fetch sponsored content from manifest (via AeonHive)
    void RefreshSponsoredContent();

    // Get available sponsored content
    std::vector<AeonSponsoredContent> GetSponsoredContent() const;

    // Record impression/click (privacy-respecting: no user tracking)
    void RecordImpression(const char* content_id);
    void RecordClick(const char* content_id);

    // Dismiss a sponsored content item
    void DismissContent(const char* content_id);

    // Set callback for new sponsored content
    void OnSponsoredContent(AeonSponsoredContentCallback cb);

    // ── Portal Landing Page ──────────────────────────────────────────────

    // Generate the zero-rated landing page HTML
    // This page is what users see when accessing the internet through
    // an MNO zero-rated entry point. It's brandable per MNO partner.
    std::string GeneratePortalHTML(const AeonMNO& mno) const;

    // ── Statistics ───────────────────────────────────────────────────────

    AeonPortalStats GetStats() const;

    // Get a human-readable savings summary
    // e.g., "You saved 1.2 GB (≈$3.21) this month"
    std::string GetSavingsSummary() const;

    // ── Diagnostics ─────────────────────────────────────────────────────

    std::string DiagnosticReport() const;

    // ── MNO Callback ────────────────────────────────────────────────────

    void OnMNODetected(AeonMNODetectedCallback cb);

    // ── Resource Awareness ────────────────────────────────────────────────
    bool CanOffloadToHive() const override { return true; }

private:
    AeonPortalImpl* m_impl = nullptr;
};

// ── Global singleton ──────────────────────────────────────────────────────────
AeonPortal& AeonPortalInstance();
