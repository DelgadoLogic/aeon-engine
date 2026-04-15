// =============================================================================
// aeon_portal.cpp — AeonPortal: Zero-Rating & MNO Partnership Engine
// Implementation of the sovereign zero-rating, data budget, and compression
// proxy system for emerging market connectivity.
//
// Architecture:
//   - PIMPL pattern for ABI stability
//   - AeonHive integration for sponsored content delivery + MNO manifest sync
//   - Platform-specific MNO detection (Android TelephonyManager, IP geo fallback)
//   - Thread-safe data budget accounting
//   - LRU image compression cache to avoid re-compressing same assets
//
// Note: This is the sovereign implementation. It does NOT depend on any
// third-party MNO SDK. Zero-rating is achieved via HTTP header injection
// using partner-specific X-headers negotiated at the business level.
// =============================================================================

#include "aeon_portal.h"
#include "../hive/aeon_hive.h"

#include <algorithm>
#include <cstring>
#include <ctime>
#include <mutex>
#include <unordered_map>
#include <cmath>

#ifdef _WIN32
    #include <windows.h>
#elif defined(__ANDROID__)
    // Android SIM/carrier detection stubs
#elif defined(__linux__)
    #include <unistd.h>
    #include <sys/sysinfo.h>
#endif

// =============================================================================
// Known MNO Partners — pre-registered zero-rating configurations
// =============================================================================
static const AeonMNO KNOWN_MNO_PARTNERS[] = {
    // Nigeria — MTN
    { 621, 30, "MTN Nigeria", "MTN", "NG",
      "X-MTN-Free-Zone", "aeon-partner-ng-001", "*.aeon.delgadologic.tech",
      true },
    // Nigeria — Airtel
    { 621, 60, "Airtel Nigeria", "Airtel", "NG",
      "X-Airtel-Zero", "aeon-partner-ng-002", "*.aeon.delgadologic.tech",
      true },
    // India — Jio
    { 405, 872, "Reliance Jio", "Jio", "IN",
      "X-Jio-Partner", "aeon-partner-in-001", "*.aeon.delgadologic.tech",
      true },
    // India — Airtel
    { 404, 10, "Airtel India", "Airtel", "IN",
      "X-Airtel-Zero", "aeon-partner-in-002", "*.aeon.delgadologic.tech",
      true },
    // Kenya — Safaricom
    { 639, 2, "Safaricom", "Safaricom", "KE",
      "X-Safaricom-Free", "aeon-partner-ke-001", "*.aeon.delgadologic.tech",
      true },
    // South Africa — Vodacom
    { 655, 1, "Vodacom SA", "Vodacom", "ZA",
      "X-Vodacom-Zero", "aeon-partner-za-001", "*.aeon.delgadologic.tech",
      true },
    // Indonesia — Telkomsel
    { 510, 10, "Telkomsel", "Telkomsel", "ID",
      "X-Telkomsel-Free", "aeon-partner-id-001", "*.aeon.delgadologic.tech",
      true },
    // Brazil — Claro
    { 724, 5, "Claro Brasil", "Claro", "BR",
      "X-Claro-Zero", "aeon-partner-br-001", "*.aeon.delgadologic.tech",
      true },
    // Philippines — Globe
    { 515, 2, "Globe Telecom", "Globe", "PH",
      "X-Globe-Free", "aeon-partner-ph-001", "*.aeon.delgadologic.tech",
      true },
    // Bangladesh — Grameenphone
    { 470, 1, "Grameenphone", "GP", "BD",
      "X-GP-Free-Zone", "aeon-partner-bd-001", "*.aeon.delgadologic.tech",
      true },
};
static constexpr size_t KNOWN_MNO_COUNT =
    sizeof(KNOWN_MNO_PARTNERS) / sizeof(KNOWN_MNO_PARTNERS[0]);

// Local data rates by country (USD per GB, approximate)
static const std::unordered_map<std::string, float> DATA_RATES_USD = {
    {"NG", 1.20f}, {"IN", 0.17f}, {"KE", 2.41f}, {"ZA", 3.87f},
    {"ID", 0.85f}, {"BR", 1.58f}, {"PH", 0.96f}, {"BD", 0.65f},
    {"GH", 2.15f}, {"TZ", 2.78f}, {"UG", 3.21f}, {"PK", 0.52f},
    {"EG", 0.88f}, {"ET", 1.65f}, {"CD", 8.30f}, {"CM", 2.33f},
    {"DEFAULT", 2.67f},  // Global average
};


// =============================================================================
// AeonPortalImpl (PIMPL)
// =============================================================================

class AeonPortalImpl {
public:
    mutable std::mutex          mtx;
    AeonPortalConfig            config;
    AeonDataBudget              budget;
    AeonPortalStats             stats;
    AeonMNO                     current_mno;
    bool                        mno_detected = false;
    bool                        initialized  = false;

    // Registered MNO partners (known + dynamically added via manifest)
    std::vector<AeonMNO>                    mno_partners;

    // Sponsored content pool
    std::vector<AeonSponsoredContent>       sponsored_content;

    // Image compression cache: hash -> compressed bytes
    struct CompressedEntry {
        std::vector<uint8_t> data;
        uint64_t             original_size;
        uint64_t             last_used;
    };
    std::unordered_map<uint64_t, CompressedEntry> compression_cache;
    static constexpr size_t MAX_CACHE_ENTRIES = 256;

    // Per-page stats (reset on navigate)
    struct {
        uint64_t original_bytes    = 0;
        uint64_t compressed_bytes  = 0;
        uint32_t images_compressed = 0;
    } page_stats;

    // Callbacks
    AeonBudgetWarningCallback           on_budget_warning;
    AeonMNODetectedCallback             on_mno_detected;
    AeonSponsoredContentCallback        on_sponsored_content;

    // AeonHive subscription IDs
    int sub_mno_manifest  = -1;
    int sub_sponsored     = -1;

    // ── Helpers ──────────────────────────────────────────────────────────

    float GetDataRate() const {
        std::string country(current_mno.country, 2);
        auto it = DATA_RATES_USD.find(country);
        if (it != DATA_RATES_USD.end()) return it->second;
        return DATA_RATES_USD.at("DEFAULT");
    }

    uint64_t SimpleHash(const uint8_t* data, size_t len) const {
        // FNV-1a 64-bit
        uint64_t hash = 0xcbf29ce484222325ULL;
        for (size_t i = 0; i < len; ++i) {
            hash ^= data[i];
            hash *= 0x100000001b3ULL;
        }
        return hash;
    }

    void EvictOldestCache() {
        if (compression_cache.size() < MAX_CACHE_ENTRIES) return;
        uint64_t oldest_time = UINT64_MAX;
        uint64_t oldest_key  = 0;
        for (auto& [k, v] : compression_cache) {
            if (v.last_used < oldest_time) {
                oldest_time = v.last_used;
                oldest_key  = k;
            }
        }
        compression_cache.erase(oldest_key);
    }

    void CheckBudgetThresholds() {
        if (budget.cycle_budget_bytes == 0) return;  // No budget set

        budget.usage_percent =
            (float)budget.cycle_bytes_down / (float)budget.cycle_budget_bytes * 100.0f;

        if (on_budget_warning) {
            uint32_t pct = (uint32_t)budget.usage_percent;
            if (pct >= config.budget_critical_percent) {
                on_budget_warning(budget, pct);
                // Auto-compress if configured
                if (config.auto_compress_on_critical) {
                    config.compression = AeonCompressionMode::Aggressive;
                }
            } else if (pct >= config.budget_warning_percent) {
                on_budget_warning(budget, pct);
            }
        }
    }

    // Detect MNO by MCC/MNC pair from known partners
    bool TryMatchMNO(uint16_t mcc, uint16_t mnc) {
        for (const auto& partner : mno_partners) {
            if (partner.mcc == mcc && partner.mnc == mnc) {
                current_mno  = partner;
                mno_detected = true;
                if (on_mno_detected) {
                    on_mno_detected(current_mno);
                }
                return true;
            }
        }
        return false;
    }
};


// =============================================================================
// AeonPortal Implementation
// =============================================================================

AeonPortal::AeonPortal() : m_impl(new AeonPortalImpl()) {}

AeonPortal::~AeonPortal() {
    if (m_impl) {
        Shutdown();
        delete m_impl;
        m_impl = nullptr;
    }
}

bool AeonPortal::Initialize() {
    std::lock_guard<std::mutex> lock(m_impl->mtx);
    if (m_impl->initialized) return true;

    // Load known MNO partners
    for (size_t i = 0; i < KNOWN_MNO_COUNT; ++i) {
        m_impl->mno_partners.push_back(KNOWN_MNO_PARTNERS[i]);
    }

    // Set defaults
    m_impl->config.compression          = AeonCompressionMode::Standard;
    m_impl->config.proxy_route          = AeonProxyRoute::Direct;
    m_impl->config.budget_bytes         = 0;  // Unlimited by default
    m_impl->config.budget_warning_percent  = 80;
    m_impl->config.budget_critical_percent = 95;
    m_impl->config.auto_compress_on_critical = true;
    m_impl->config.enable_zero_rating   = true;
    m_impl->config.enable_sponsored     = true;
    m_impl->config.auto_detect_mno      = true;
    m_impl->config.proxy_tls            = true;
    m_impl->config.proxy_port           = 8443;
    strncpy(m_impl->config.proxy_host, "proxy.aeon.delgadologic.tech",
            sizeof(m_impl->config.proxy_host) - 1);

    // Initialize budget
    memset(&m_impl->budget, 0, sizeof(AeonDataBudget));
    m_impl->budget.session_start_utc = (uint64_t)time(nullptr);
    m_impl->budget.cycle_start_utc   = (uint64_t)time(nullptr);
    m_impl->budget.cycle_end_utc     = m_impl->budget.cycle_start_utc + (30 * 86400);

    // Initialize stats
    memset(&m_impl->stats, 0, sizeof(AeonPortalStats));

    // Subscribe to AeonHive for MNO manifest updates
    // Topic would be something like AeonHiveTopic::SovereignManifest
    // For now, we initialize without HiveInstance dependency at startup

    m_impl->initialized = true;
    return true;
}

void AeonPortal::Shutdown() {
    std::lock_guard<std::mutex> lock(m_impl->mtx);
    if (!m_impl->initialized) return;

    // Unsubscribe from AeonHive
    // AeonHiveInstance().Unsubscribe(m_impl->sub_mno_manifest);
    // AeonHiveInstance().Unsubscribe(m_impl->sub_sponsored);

    m_impl->compression_cache.clear();
    m_impl->sponsored_content.clear();
    m_impl->initialized = false;
}

void AeonPortal::Configure(const AeonPortalConfig& cfg) {
    std::lock_guard<std::mutex> lock(m_impl->mtx);
    m_impl->config = cfg;
}

AeonPortalConfig AeonPortal::GetConfig() const {
    std::lock_guard<std::mutex> lock(m_impl->mtx);
    return m_impl->config;
}


// ── MNO Detection ────────────────────────────────────────────────────────────

bool AeonPortal::DetectMNO() {
    std::lock_guard<std::mutex> lock(m_impl->mtx);

#ifdef __ANDROID__
    // Android: Use JNI to call TelephonyManager.getNetworkOperator()
    // Returns MCC+MNC as string (e.g., "62130" for MTN Nigeria)
    // Stub: In production, this calls through JNI bridge
    //
    // JNIEnv* env = GetJNIEnv();
    // jclass clazz = env->FindClass("android/telephony/TelephonyManager");
    // jmethodID getNetworkOperator = env->GetMethodID(clazz, "getNetworkOperator", "()Ljava/lang/String;");
    // jstring result = (jstring)env->CallObjectMethod(telephonyManager, getNetworkOperator);
    // const char* mccmnc = env->GetStringUTFChars(result, nullptr);
    // uint16_t mcc = atoi(std::string(mccmnc, 3).c_str());
    // uint16_t mnc = atoi(std::string(mccmnc + 3).c_str());
    // return m_impl->TryMatchMNO(mcc, mnc);

    // For now, try all known partners (will be replaced by real JNI)
    return false;
#elif defined(_WIN32)
    // Windows: No SIM card, use IP geolocation as fallback
    // In production: query a lightweight IP->country service
    // For now, return false (desktop doesn't need zero-rating)
    return false;
#else
    // Linux/other: IP geolocation fallback
    return false;
#endif
}

AeonMNO AeonPortal::GetCurrentMNO() const {
    std::lock_guard<std::mutex> lock(m_impl->mtx);
    return m_impl->current_mno;
}

void AeonPortal::RegisterMNOPartner(const AeonMNO& mno) {
    std::lock_guard<std::mutex> lock(m_impl->mtx);

    // Check if already registered (by MCC+MNC)
    for (auto& existing : m_impl->mno_partners) {
        if (existing.mcc == mno.mcc && existing.mnc == mno.mnc) {
            existing = mno;  // Update
            return;
        }
    }
    m_impl->mno_partners.push_back(mno);
}

bool AeonPortal::IsZeroRated() const {
    std::lock_guard<std::mutex> lock(m_impl->mtx);
    return m_impl->mno_detected && m_impl->current_mno.zero_rate_enabled;
}

std::vector<std::pair<std::string, std::string>> AeonPortal::GetZeroRateHeaders() const {
    std::lock_guard<std::mutex> lock(m_impl->mtx);
    std::vector<std::pair<std::string, std::string>> headers;

    if (!m_impl->mno_detected || !m_impl->current_mno.zero_rate_enabled) {
        return headers;
    }

    // Primary zero-rating header
    headers.emplace_back(
        std::string(m_impl->current_mno.zero_rate_header),
        std::string(m_impl->current_mno.zero_rate_value)
    );

    // Aeon partner identification header
    headers.emplace_back("X-Aeon-Partner", "aeon-browser-v1");

    // Device class header (helps MNO prioritize traffic)
    headers.emplace_back("X-Aeon-Device-Class",
        m_impl->config.compression == AeonCompressionMode::Aggressive ? "low"
        : m_impl->config.compression == AeonCompressionMode::TextOnly ? "minimal"
        : "standard");

    return headers;
}


// ── Data Budget ──────────────────────────────────────────────────────────────

void AeonPortal::SetBudget(uint64_t bytes_per_cycle, uint32_t cycle_days) {
    std::lock_guard<std::mutex> lock(m_impl->mtx);
    m_impl->budget.cycle_budget_bytes = bytes_per_cycle;
    m_impl->config.budget_bytes = bytes_per_cycle;

    // Reset cycle timing
    uint64_t now = (uint64_t)time(nullptr);
    m_impl->budget.cycle_start_utc = now;
    m_impl->budget.cycle_end_utc   = now + ((uint64_t)cycle_days * 86400);
}

void AeonPortal::RecordUsage(uint64_t bytes_up, uint64_t bytes_down, bool zero_rated) {
    std::lock_guard<std::mutex> lock(m_impl->mtx);

    // Session tracking
    m_impl->budget.session_bytes_up   += bytes_up;
    m_impl->budget.session_bytes_down += bytes_down;

    if (zero_rated) {
        m_impl->budget.zero_rated_bytes += bytes_down;
        // Zero-rated traffic doesn't count against budget
    } else {
        // Cycle tracking (non-zero-rated only)
        m_impl->budget.cycle_bytes_up   += bytes_up;
        m_impl->budget.cycle_bytes_down += bytes_down;
    }

    // Check thresholds
    m_impl->CheckBudgetThresholds();
}

AeonDataBudget AeonPortal::GetBudget() const {
    std::lock_guard<std::mutex> lock(m_impl->mtx);
    return m_impl->budget;
}

void AeonPortal::ResetCycle() {
    std::lock_guard<std::mutex> lock(m_impl->mtx);

    m_impl->budget.cycle_bytes_up   = 0;
    m_impl->budget.cycle_bytes_down = 0;
    m_impl->budget.cycle_bytes_saved = 0;

    uint64_t now = (uint64_t)time(nullptr);
    m_impl->budget.cycle_start_utc = now;
    m_impl->budget.cycle_end_utc   = now + (30 * 86400);  // Default 30-day cycle
    m_impl->budget.usage_percent   = 0.0f;
}

void AeonPortal::OnBudgetWarning(AeonBudgetWarningCallback cb) {
    std::lock_guard<std::mutex> lock(m_impl->mtx);
    m_impl->on_budget_warning = std::move(cb);
}


// ── Compression ──────────────────────────────────────────────────────────────

void AeonPortal::SetCompressionMode(AeonCompressionMode mode) {
    std::lock_guard<std::mutex> lock(m_impl->mtx);
    m_impl->config.compression = mode;
}

AeonCompressionMode AeonPortal::GetCompressionMode() const {
    std::lock_guard<std::mutex> lock(m_impl->mtx);
    return m_impl->config.compression;
}

std::vector<uint8_t> AeonPortal::CompressImage(
    const uint8_t* data, size_t len,
    const char* mime_type,
    AeonCompressionMode mode)
{
    std::lock_guard<std::mutex> lock(m_impl->mtx);

    // Check cache first
    uint64_t hash = m_impl->SimpleHash(data, len);
    auto it = m_impl->compression_cache.find(hash);
    if (it != m_impl->compression_cache.end()) {
        it->second.last_used = (uint64_t)time(nullptr);
        return it->second.data;
    }

    // Determine target quality based on mode
    int quality = 85;
    switch (mode) {
        case AeonCompressionMode::None:       return std::vector<uint8_t>(data, data + len);
        case AeonCompressionMode::Light:      quality = 80; break;
        case AeonCompressionMode::Standard:   quality = 50; break;
        case AeonCompressionMode::Aggressive: quality = 20; break;
        case AeonCompressionMode::TextOnly:   return {};  // No images in text-only mode
    }

    // === Image Compression Logic ===
    // In production, this uses stb_image + stb_image_write for JPEG recompression
    // or WebP encoding via libwebp for maximum compression.
    //
    // For now, we simulate compression by truncating to quality%
    // This is a placeholder — real implementation will:
    //   1. Decode JPEG/PNG/WebP via stb_image
    //   2. Resize if dimensions > viewport (common on mobile)
    //   3. Re-encode as WebP at target quality
    //   4. If WebP is larger than JPEG, keep JPEG

    size_t compressed_size = (size_t)((float)len * ((float)quality / 100.0f));
    if (compressed_size >= len) compressed_size = len;  // Don't expand

    std::vector<uint8_t> compressed(data, data + compressed_size);

    // Update stats
    m_impl->page_stats.original_bytes    += len;
    m_impl->page_stats.compressed_bytes  += compressed_size;
    m_impl->page_stats.images_compressed += 1;

    m_impl->stats.total_bytes_saved         += (len - compressed_size);
    m_impl->stats.total_images_compressed   += 1;

    // Cache the result
    m_impl->EvictOldestCache();
    m_impl->compression_cache[hash] = {
        compressed,
        (uint64_t)len,
        (uint64_t)time(nullptr)
    };

    // Update data savings in budget
    m_impl->budget.session_bytes_saved += (len - compressed_size);
    m_impl->budget.cycle_bytes_saved   += (len - compressed_size);

    return compressed;
}

AeonPortal::PageCompressionStats AeonPortal::GetPageStats() const {
    std::lock_guard<std::mutex> lock(m_impl->mtx);
    PageCompressionStats stats;
    stats.original_bytes    = m_impl->page_stats.original_bytes;
    stats.compressed_bytes  = m_impl->page_stats.compressed_bytes;
    stats.images_compressed = m_impl->page_stats.images_compressed;
    stats.ratio = (stats.original_bytes > 0)
        ? (float)stats.compressed_bytes / (float)stats.original_bytes
        : 1.0f;
    return stats;
}


// ── Proxy Routing ────────────────────────────────────────────────────────────

void AeonPortal::SetProxyRoute(AeonProxyRoute route) {
    std::lock_guard<std::mutex> lock(m_impl->mtx);
    m_impl->config.proxy_route = route;
}

AeonProxyRoute AeonPortal::GetProxyRoute() const {
    std::lock_guard<std::mutex> lock(m_impl->mtx);
    return m_impl->config.proxy_route;
}

bool AeonPortal::ShouldProxy(const char* url) const {
    std::lock_guard<std::mutex> lock(m_impl->mtx);

    if (m_impl->config.proxy_route == AeonProxyRoute::Direct) return false;
    if (!url || url[0] == '\0') return false;

    // Don't proxy these:
    //   - localhost / 127.0.0.1 / [::1]
    //   - Already-proxied URLs
    //   - Aeon internal URLs
    std::string u(url);

    if (u.find("localhost") != std::string::npos ||
        u.find("127.0.0.1") != std::string::npos ||
        u.find("[::1]")     != std::string::npos) {
        return false;
    }

    if (u.find("proxy.aeon") != std::string::npos) {
        return false;  // Don't double-proxy
    }

    // Proxy everything else when compression is enabled
    return true;
}

std::string AeonPortal::GetProxyURL(const char* target_url) const {
    std::lock_guard<std::mutex> lock(m_impl->mtx);

    if (!target_url || target_url[0] == '\0') return "";

    // Construct proxy URL:
    // https://proxy.aeon.delgadologic.tech:8443/compress?url=<target>&q=<quality>
    std::string proxy_url;
    proxy_url.reserve(512);

    if (m_impl->config.proxy_tls) {
        proxy_url += "https://";
    } else {
        proxy_url += "http://";
    }

    proxy_url += m_impl->config.proxy_host;
    proxy_url += ":";
    proxy_url += std::to_string(m_impl->config.proxy_port);
    proxy_url += "/compress?url=";
    proxy_url += target_url;  // In production: URL-encode this
    proxy_url += "&q=";

    switch (m_impl->config.compression) {
        case AeonCompressionMode::Light:      proxy_url += "80"; break;
        case AeonCompressionMode::Standard:   proxy_url += "50"; break;
        case AeonCompressionMode::Aggressive: proxy_url += "20"; break;
        case AeonCompressionMode::TextOnly:   proxy_url += "0";  break;
        default:                              proxy_url += "85"; break;
    }

    return proxy_url;
}


// ── Sponsored Content ────────────────────────────────────────────────────────

void AeonPortal::RefreshSponsoredContent() {
    // In production: Subscribe to AeonHive topic for sponsored content manifests
    // The content is signed by AEON_MANIFEST_SIGNER_PUBKEY to prevent injection
    //
    // AeonHiveInstance().Subscribe(AeonHiveTopic::SponsoredContent,
    //     [this](const uint8_t* data, size_t len, const AeonHivePeer& peer) {
    //         // Deserialize and verify signature
    //         // Update m_impl->sponsored_content
    //     });
}

std::vector<AeonSponsoredContent> AeonPortal::GetSponsoredContent() const {
    std::lock_guard<std::mutex> lock(m_impl->mtx);
    std::vector<AeonSponsoredContent> active;

    uint64_t now = (uint64_t)time(nullptr);
    for (const auto& item : m_impl->sponsored_content) {
        if (!item.user_dismissed &&
            now >= item.impression_start_utc &&
            now <= item.impression_end_utc) {
            active.push_back(item);
        }
    }

    return active;
}

void AeonPortal::RecordImpression(const char* content_id) {
    std::lock_guard<std::mutex> lock(m_impl->mtx);
    m_impl->stats.total_sponsored_shown += 1;
    // Privacy: We record aggregate counts only, no user tracking
}

void AeonPortal::RecordClick(const char* content_id) {
    std::lock_guard<std::mutex> lock(m_impl->mtx);
    m_impl->stats.total_sponsored_clicked += 1;
}

void AeonPortal::DismissContent(const char* content_id) {
    std::lock_guard<std::mutex> lock(m_impl->mtx);
    for (auto& item : m_impl->sponsored_content) {
        if (strncmp(item.id, content_id, sizeof(item.id)) == 0) {
            item.user_dismissed = true;
            break;
        }
    }
}

void AeonPortal::OnSponsoredContent(AeonSponsoredContentCallback cb) {
    std::lock_guard<std::mutex> lock(m_impl->mtx);
    m_impl->on_sponsored_content = std::move(cb);
}


// ── Portal Landing Page ──────────────────────────────────────────────────────

std::string AeonPortal::GeneratePortalHTML(const AeonMNO& mno) const {
    // Generate a brandable HTML landing page for zero-rated entry
    // This is served when users access the internet through the MNO's
    // zero-rated domain (e.g., free.aeon.delgadologic.tech)

    std::string html;
    html.reserve(4096);

    html += R"(<!DOCTYPE html>
<html lang="en">
<head>
    <meta charset="UTF-8">
    <meta name="viewport" content="width=device-width, initial-scale=1.0">
    <title>Aeon Browser - Free Internet Access</title>
    <style>
        * { margin: 0; padding: 0; box-sizing: border-box; }
        body {
            font-family: 'Inter', -apple-system, sans-serif;
            background: linear-gradient(135deg, #0a0e1a 0%, #1a1f35 50%, #0d1117 100%);
            color: #e0e0e0;
            min-height: 100vh;
            display: flex;
            flex-direction: column;
            align-items: center;
            justify-content: center;
        }
        .portal-container {
            max-width: 420px;
            width: 90%;
            padding: 2rem;
            text-align: center;
        }
        .logo {
            width: 80px;
            height: 80px;
            margin: 0 auto 1.5rem;
            background: linear-gradient(135deg, #6366f1, #818cf8);
            border-radius: 20px;
            display: flex;
            align-items: center;
            justify-content: center;
            font-size: 2rem;
            font-weight: 700;
            color: white;
            box-shadow: 0 8px 32px rgba(99, 102, 241, 0.3);
        }
        .partner-badge {
            display: inline-block;
            padding: 0.4rem 1rem;
            background: rgba(255, 255, 255, 0.08);
            border: 1px solid rgba(255, 255, 255, 0.15);
            border-radius: 100px;
            font-size: 0.8rem;
            color: #a0a0a0;
            margin-bottom: 1.5rem;
        }
        h1 {
            font-size: 1.6rem;
            font-weight: 600;
            margin-bottom: 0.5rem;
            background: linear-gradient(90deg, #818cf8, #a78bfa);
            -webkit-background-clip: text;
            -webkit-text-fill-color: transparent;
        }
        .subtitle {
            color: #888;
            font-size: 0.95rem;
            margin-bottom: 2rem;
            line-height: 1.5;
        }
        .search-bar {
            width: 100%;
            padding: 0.9rem 1.2rem;
            background: rgba(255, 255, 255, 0.06);
            border: 1px solid rgba(255, 255, 255, 0.12);
            border-radius: 12px;
            color: white;
            font-size: 1rem;
            outline: none;
            transition: all 0.2s;
            margin-bottom: 1.5rem;
        }
        .search-bar:focus {
            border-color: #6366f1;
            box-shadow: 0 0 0 3px rgba(99, 102, 241, 0.15);
        }
        .quick-links {
            display: grid;
            grid-template-columns: repeat(3, 1fr);
            gap: 0.8rem;
            margin-bottom: 2rem;
        }
        .quick-link {
            padding: 1rem 0.5rem;
            background: rgba(255, 255, 255, 0.04);
            border: 1px solid rgba(255, 255, 255, 0.08);
            border-radius: 12px;
            text-decoration: none;
            color: #ccc;
            font-size: 0.75rem;
            transition: all 0.2s;
        }
        .quick-link:hover {
            background: rgba(99, 102, 241, 0.1);
            border-color: rgba(99, 102, 241, 0.3);
            transform: translateY(-2px);
        }
        .quick-link-icon {
            font-size: 1.5rem;
            margin-bottom: 0.4rem;
        }
        .data-banner {
            padding: 1rem;
            background: rgba(34, 197, 94, 0.08);
            border: 1px solid rgba(34, 197, 94, 0.2);
            border-radius: 12px;
            font-size: 0.85rem;
            color: #4ade80;
        }
        .footer {
            margin-top: 2rem;
            font-size: 0.7rem;
            color: #555;
        }
    </style>
</head>
<body>
    <div class="portal-container">
        <div class="logo">A</div>
)";

    // Dynamic MNO branding
    html += "        <div class=\"partner-badge\">Powered by ";
    html += mno.brand;
    html += " + Aeon Browser</div>\n";

    html += R"(        <h1>Free Internet Access</h1>
        <p class="subtitle">Browse the web for free on )";
    html += mno.brand;
    html += R"( network. No data charges apply.</p>
        <input type="text" class="search-bar" placeholder="Search or enter URL..." autofocus>
        <div class="quick-links">
            <a href="https://wikipedia.org" class="quick-link">
                <div class="quick-link-icon">📚</div>
                Wikipedia
            </a>
            <a href="https://news.google.com" class="quick-link">
                <div class="quick-link-icon">📰</div>
                News
            </a>
            <a href="https://translate.google.com" class="quick-link">
                <div class="quick-link-icon">🌍</div>
                Translate
            </a>
            <a href="https://weather.com" class="quick-link">
                <div class="quick-link-icon">🌤</div>
                Weather
            </a>
            <a href="https://mail.google.com" class="quick-link">
                <div class="quick-link-icon">✉️</div>
                Email
            </a>
            <a href="https://youtube.com" class="quick-link">
                <div class="quick-link-icon">▶️</div>
                Video
            </a>
        </div>
        <div class="data-banner">
            🎉 This session is <strong>zero-rated</strong> — browse without using your data plan!
        </div>
        <div class="footer">
            Aeon Browser &copy; 2026 DelgadoLogic | Privacy First
        </div>
    </div>
</body>
</html>)";

    return html;
}


// ── Statistics ───────────────────────────────────────────────────────────────

AeonPortalStats AeonPortal::GetStats() const {
    std::lock_guard<std::mutex> lock(m_impl->mtx);

    AeonPortalStats stats = m_impl->stats;

    // Calculate average compression ratio
    if (m_impl->page_stats.original_bytes > 0) {
        stats.avg_compression_ratio =
            (float)m_impl->page_stats.compressed_bytes /
            (float)m_impl->page_stats.original_bytes;
    }

    // Calculate data savings in USD
    float rate = m_impl->GetDataRate();
    float gb_saved = (float)stats.total_bytes_saved / (1024.0f * 1024.0f * 1024.0f);
    stats.data_savings_usd = gb_saved * rate;

    return stats;
}

std::string AeonPortal::GetSavingsSummary() const {
    auto stats = GetStats();
    auto budget = GetBudget();

    std::string summary;
    summary.reserve(256);

    // Format saved bytes
    float saved_mb = (float)stats.total_bytes_saved / (1024.0f * 1024.0f);
    float saved_gb = saved_mb / 1024.0f;

    if (saved_gb >= 1.0f) {
        // Truncate to 1 decimal
        int gb_int = (int)(saved_gb * 10.0f);
        summary += "You saved ";
        summary += std::to_string(gb_int / 10);
        summary += ".";
        summary += std::to_string(gb_int % 10);
        summary += " GB";
    } else {
        int mb_int = (int)saved_mb;
        summary += "You saved ";
        summary += std::to_string(mb_int);
        summary += " MB";
    }

    // Add estimated cost savings
    if (stats.data_savings_usd > 0.01f) {
        int usd_cents = (int)(stats.data_savings_usd * 100.0f);
        summary += " ($";
        summary += std::to_string(usd_cents / 100);
        summary += ".";
        int cents = usd_cents % 100;
        if (cents < 10) summary += "0";
        summary += std::to_string(cents);
        summary += ")";
    }

    summary += " this month";

    // Zero-rated bonus
    if (budget.zero_rated_bytes > 0) {
        float zr_mb = (float)budget.zero_rated_bytes / (1024.0f * 1024.0f);
        int zr_int = (int)zr_mb;
        summary += " + ";
        summary += std::to_string(zr_int);
        summary += " MB free (zero-rated)";
    }

    return summary;
}


// ── Diagnostics ─────────────────────────────────────────────────────────────

std::string AeonPortal::DiagnosticReport() const {
    std::lock_guard<std::mutex> lock(m_impl->mtx);

    auto stats = m_impl->stats;
    auto budget = m_impl->budget;
    auto config = m_impl->config;

    std::string report;
    report.reserve(2048);

    report += "=== AeonPortal Diagnostic Report ===\n\n";

    // MNO Status
    report += "MNO Detection:\n";
    if (m_impl->mno_detected) {
        report += "  Carrier: ";
        report += m_impl->current_mno.name;
        report += " (MCC: ";
        report += std::to_string(m_impl->current_mno.mcc);
        report += " MNC: ";
        report += std::to_string(m_impl->current_mno.mnc);
        report += ")\n";
        report += "  Zero-Rated: ";
        report += m_impl->current_mno.zero_rate_enabled ? "YES" : "NO";
        report += "\n";
    } else {
        report += "  No carrier detected\n";
    }
    report += "  Known Partners: ";
    report += std::to_string(m_impl->mno_partners.size());
    report += "\n\n";

    // Data Budget
    report += "Data Budget:\n";
    if (budget.cycle_budget_bytes > 0) {
        float usage_mb = (float)budget.cycle_bytes_down / (1024.0f * 1024.0f);
        float budget_mb = (float)budget.cycle_budget_bytes / (1024.0f * 1024.0f);
        report += "  Usage: ";
        report += std::to_string((int)usage_mb);
        report += " / ";
        report += std::to_string((int)budget_mb);
        report += " MB (";
        report += std::to_string((int)budget.usage_percent);
        report += "%)\n";
    } else {
        report += "  Budget: Unlimited\n";
    }
    report += "  Session: ";
    float session_mb = (float)budget.session_bytes_down / (1024.0f * 1024.0f);
    report += std::to_string((int)session_mb);
    report += " MB down\n";
    report += "  Zero-Rated: ";
    float zr_mb = (float)budget.zero_rated_bytes / (1024.0f * 1024.0f);
    report += std::to_string((int)zr_mb);
    report += " MB\n\n";

    // Compression
    report += "Compression:\n";
    const char* mode_names[] = { "None", "Light", "Standard", "Aggressive", "TextOnly" };
    report += "  Mode: ";
    report += mode_names[(int)config.compression];
    report += "\n";
    report += "  Images Compressed: ";
    report += std::to_string(stats.total_images_compressed);
    report += "\n";
    report += "  Total Saved: ";
    float saved_mb = (float)stats.total_bytes_saved / (1024.0f * 1024.0f);
    report += std::to_string((int)saved_mb);
    report += " MB\n";
    report += "  Estimated Savings: $";
    float rate = m_impl->GetDataRate();
    float gb_saved = (float)stats.total_bytes_saved / (1024.0f * 1024.0f * 1024.0f);
    int cents = (int)(gb_saved * rate * 100.0f);
    report += std::to_string(cents / 100);
    report += ".";
    int c = cents % 100;
    if (c < 10) report += "0";
    report += std::to_string(c);
    report += "\n\n";

    // Proxy
    const char* route_names[] = { "Direct", "AeonProxy", "PeerProxy", "MNOProxy" };
    report += "Proxy Route: ";
    report += route_names[(int)config.proxy_route];
    report += "\n";
    report += "Proxy Host: ";
    report += config.proxy_host;
    report += ":";
    report += std::to_string(config.proxy_port);
    report += "\n\n";

    // Sponsored
    report += "Sponsored Content:\n";
    report += "  Active Items: ";
    report += std::to_string(m_impl->sponsored_content.size());
    report += "\n";
    report += "  Impressions: ";
    report += std::to_string(stats.total_sponsored_shown);
    report += "\n";
    report += "  Clicks: ";
    report += std::to_string(stats.total_sponsored_clicked);
    report += "\n";

    report += "\n=== End Report ===\n";

    return report;
}


// ── Callbacks ────────────────────────────────────────────────────────────────

void AeonPortal::OnMNODetected(AeonMNODetectedCallback cb) {
    std::lock_guard<std::mutex> lock(m_impl->mtx);
    m_impl->on_mno_detected = std::move(cb);
}


// ── Global Singleton ─────────────────────────────────────────────────────────

AeonPortal& AeonPortalInstance() {
    static AeonPortal instance;
    return instance;
}
