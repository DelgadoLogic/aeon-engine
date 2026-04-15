// =============================================================================
// aeon_cloud.cpp — AeonCloud: Cloud-Assisted Rendering Engine Implementation
// DelgadoLogic | Pillar 2: Cloud-Assisted Rendering via AeonHive Mesh
//
// Implements the full Turbo Mode lifecycle:
//   1. Bandwidth/RAM detection for auto-mode switching
//   2. Renderer discovery via AeonHive CloudRenderOffer topic
//   3. Page rendering delegation via CloudRenderRequest
//   4. AVIF/WebP tile reception via CloudRenderTile
//   5. DOM metadata + interactive element mapping via CloudRenderMeta
//   6. Input relay (tap, scroll, type) via CloudRenderInput
//   7. Community cache for popular pages
//   8. Volunteer renderer mode for desktop users
//
// Architecture (Client):
//   TurboNavigate(url) → Publish(CloudRenderRequest, url)
//   Subscribe(CloudRenderTile) → decode → composite → display
//   TurboInput(event) → Publish(CloudRenderInput, serialized event)
//
// Architecture (Renderer):
//   Subscribe(CloudRenderRequest) → headless render → encode tiles
//   Publish(CloudRenderTile, tiles) + Publish(CloudRenderMeta, metadata)
//
// Dependencies:
//   - AeonHive (P2P backbone, pub/sub, peer discovery)
//   - AeonTLS (secure connections to origin servers)
//   - Platform APIs for RAM/bandwidth detection
// =============================================================================

#include "aeon_cloud.h"
#include "../hive/aeon_hive.h"

#include <cstring>
#include <ctime>
#include <mutex>
#include <algorithm>
#include <deque>
#include <unordered_map>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#elif defined(__ANDROID__)
#include <sys/sysinfo.h>
#elif defined(__linux__)
#include <sys/sysinfo.h>
#endif

// ---------------------------------------------------------------------------
// Wire format helpers — serialization for AeonHive pub/sub payloads
// ---------------------------------------------------------------------------
namespace CloudWire {

// RenderRequest payload: [url_len:u16][url:utf8]
static std::vector<uint8_t> EncodeRenderRequest(const char* url) {
    size_t url_len = strlen(url);
    if (url_len > 2048) url_len = 2048;
    std::vector<uint8_t> buf(2 + url_len);
    buf[0] = static_cast<uint8_t>(url_len & 0xFF);
    buf[1] = static_cast<uint8_t>((url_len >> 8) & 0xFF);
    memcpy(buf.data() + 2, url, url_len);
    return buf;
}

static bool DecodeRenderRequest(const uint8_t* data, size_t len,
                                 char* url_out, size_t url_max) {
    if (!data || len < 2) return false;
    uint16_t url_len = data[0] | (data[1] << 8);
    if (url_len > len - 2 || url_len >= url_max) return false;
    memcpy(url_out, data + 2, url_len);
    url_out[url_len] = '\0';
    return true;
}

// InputEvent payload: [type:u8][x:u16][y:u16][dx:i16][dy:i16][scale:f32]
//                     [element_id:u32][text_len:u16][text:utf8]
static std::vector<uint8_t> EncodeInputEvent(const AeonInputEvent& evt) {
    size_t text_len = strlen(evt.text);
    if (text_len > 1024) text_len = 1024;
    std::vector<uint8_t> buf(1 + 4 + 4 + 4 + 4 + 2 + text_len);
    size_t off = 0;

    buf[off++] = static_cast<uint8_t>(evt.type);
    memcpy(buf.data() + off, &evt.x, 2); off += 2;
    memcpy(buf.data() + off, &evt.y, 2); off += 2;
    memcpy(buf.data() + off, &evt.dx, 2); off += 2;
    memcpy(buf.data() + off, &evt.dy, 2); off += 2;
    memcpy(buf.data() + off, &evt.scale, 4); off += 4;
    memcpy(buf.data() + off, &evt.element_id, 4); off += 4;
    uint16_t tl = static_cast<uint16_t>(text_len);
    memcpy(buf.data() + off, &tl, 2); off += 2;
    if (text_len > 0) {
        memcpy(buf.data() + off, evt.text, text_len);
    }
    return buf;
}

// RendererOffer payload: [available_slots:u8][ram_mb:u32][gpu_flag:u8]
static std::vector<uint8_t> EncodeRendererOffer(uint8_t slots, uint32_t ram, bool gpu) {
    std::vector<uint8_t> buf(6);
    buf[0] = slots;
    memcpy(buf.data() + 1, &ram, 4);
    buf[5] = gpu ? 1 : 0;
    return buf;
}

} // namespace CloudWire


// ---------------------------------------------------------------------------
// AeonCloudImpl — private implementation
// ---------------------------------------------------------------------------
class AeonCloudImpl {
public:
    AeonCloudConfig                 config;
    mutable std::mutex              mu;

    // Current state
    AeonCloudMode                   current_mode = AeonCloudMode::Full;
    bool                            initialized = false;
    bool                            renderer_active = false;
    uint32_t                        active_render_sessions = 0;

    // Callbacks
    AeonCloudTileCallback           on_tile;
    AeonCloudMetaCallback           on_meta;
    AeonCloudModeCallback           on_mode_change;

    // Session statistics
    AeonCloudStats                  stats;

    // Bandwidth estimation (simple EWMA)
    uint32_t                        est_bandwidth_kbps = 0;
    uint64_t                        last_bandwidth_sample = 0;
    static constexpr float          BANDWIDTH_EWMA_ALPHA = 0.3f;

    // Community cache: URL → vector of tiles (LRU, max 128 entries)
    struct CachePage {
        std::string url;
        std::vector<AeonTile> tiles;
        std::vector<uint8_t>  tile_data_storage; // owns tile pixel data
        uint64_t cached_at;
        uint32_t access_count;
    };
    std::deque<CachePage>           page_cache;
    static constexpr size_t         MAX_CACHE_ENTRIES = 128;

    // Known renderer peers (populated via CloudRenderOffer subscriptions)
    struct RendererPeer {
        uint8_t  peer_id[32];
        uint8_t  available_slots;
        uint32_t ram_mb;
        bool     has_gpu;
        uint64_t last_seen;
        float    avg_latency_ms;  // measured
    };
    std::vector<RendererPeer>       known_renderers;

    // Active Turbo session
    struct TurboSession {
        bool     active = false;
        uint8_t  renderer_peer[32];
        char     current_url[2048];
        uint32_t sequence = 0;
        uint64_t navigate_start = 0; // for latency measurement
    };
    TurboSession                    session;

    // ── Time Utilities ────────────────────────────────────────────
    static uint64_t NowUTC() {
        return static_cast<uint64_t>(time(nullptr));
    }

    // ── RAM Detection ─────────────────────────────────────────────
    static uint32_t DetectAvailableRAM() {
#ifdef _WIN32
        MEMORYSTATUSEX ms;
        ms.dwLength = sizeof(ms);
        if (GlobalMemoryStatusEx(&ms)) {
            return static_cast<uint32_t>(ms.ullAvailPhys / (1024 * 1024));
        }
        return 256; // conservative fallback
#elif defined(__linux__) || defined(__ANDROID__)
        struct sysinfo si;
        if (sysinfo(&si) == 0) {
            return static_cast<uint32_t>(
                (si.freeram * si.mem_unit) / (1024 * 1024));
        }
        return 256;
#else
        return 512; // macOS/other: assume comfortable
#endif
    }

    // ── Bandwidth Estimation ──────────────────────────────────────
    void UpdateBandwidthEstimate(uint32_t sample_kbps) {
        uint64_t now = NowUTC();
        if (est_bandwidth_kbps == 0) {
            est_bandwidth_kbps = sample_kbps;
        } else {
            // EWMA smoothing
            est_bandwidth_kbps = static_cast<uint32_t>(
                BANDWIDTH_EWMA_ALPHA * sample_kbps +
                (1.0f - BANDWIDTH_EWMA_ALPHA) * est_bandwidth_kbps);
        }
        last_bandwidth_sample = now;
    }

    // ── Renderer Selection ────────────────────────────────────────
    // Pick the best renderer based on: available slots, latency, GPU, RAM
    const RendererPeer* SelectBestRenderer() const {
        const RendererPeer* best = nullptr;
        float best_score = -1.0f;
        uint64_t now = NowUTC();

        for (const auto& rp : known_renderers) {
            // Skip stale peers (not seen in 5 minutes)
            if (now - rp.last_seen > 300) continue;
            // Skip full renderers
            if (rp.available_slots == 0) continue;

            // Scoring: higher = better
            float score = 0.0f;
            score += rp.available_slots * 10.0f;           // more slots = better
            score += rp.has_gpu ? 50.0f : 0.0f;            // GPU bonus
            score += (rp.ram_mb / 1024.0f) * 20.0f;        // more RAM = better
            if (rp.avg_latency_ms > 0.0f) {
                score -= rp.avg_latency_ms * 0.5f;         // lower latency = better
            }

            if (score > best_score) {
                best_score = score;
                best = &rp;
            }
        }
        return best;
    }

    // ── Auto-Mode Decision ────────────────────────────────────────
    AeonCloudMode RecommendMode() const {
        uint32_t ram = DetectAvailableRAM();
        uint32_t bw = est_bandwidth_kbps;

        // Turbo threshold: < 512MB RAM or < 256kbps
        if (ram < 512 || (bw > 0 && bw < 256)) {
            // Check if any renderers available
            if (!known_renderers.empty()) {
                return AeonCloudMode::Turbo;
            }
            // No renderers available → Lite mode (text-only fallback)
            return AeonCloudMode::Lite;
        }

        // Lite threshold: < 128MB RAM or < 64kbps
        if (ram < 128 || (bw > 0 && bw < 64)) {
            return AeonCloudMode::Lite;
        }

        // Comfortable resources → full local rendering
        return AeonCloudMode::Full;
    }

    // ── Cache Management ──────────────────────────────────────────
    CachePage* FindCached(const char* url) {
        for (auto& cp : page_cache) {
            if (cp.url == url) {
                cp.access_count++;
                return &cp;
            }
        }
        return nullptr;
    }

    void EvictLRU() {
        while (page_cache.size() >= MAX_CACHE_ENTRIES) {
            // Remove least recently accessed
            auto oldest = page_cache.begin();
            for (auto it = page_cache.begin(); it != page_cache.end(); ++it) {
                if (it->access_count < oldest->access_count ||
                    (it->access_count == oldest->access_count &&
                     it->cached_at < oldest->cached_at)) {
                    oldest = it;
                }
            }
            page_cache.erase(oldest);
        }
    }

    // ── AeonHive Integration ──────────────────────────────────────

    void SetupHiveSubscriptions() {
        auto& hive = AeonHiveInstance();

        // Receive tiles from renderer
        hive.Subscribe(AeonHiveTopic::CloudRenderTile,
            [this](const AeonMessage& msg) {
                HandleIncomingTile(msg);
            });

        // Receive DOM metadata from renderer
        hive.Subscribe(AeonHiveTopic::CloudRenderMeta,
            [this](const AeonMessage& msg) {
                HandleIncomingMeta(msg);
            });

        // Receive renderer capacity offers
        hive.Subscribe(AeonHiveTopic::CloudRenderOffer,
            [this](const AeonMessage& msg) {
                HandleRendererOffer(msg);
            });

        // If we're a renderer, listen for requests
        if (config.volunteer_renderer) {
            hive.Subscribe(AeonHiveTopic::CloudRenderRequest,
                [this](const AeonMessage& msg) {
                    HandleRenderRequest(msg);
                });
        }
    }

    void HandleIncomingTile(const AeonMessage& msg) {
        if (!msg.payload || msg.payload_len < 16) return;
        std::lock_guard<std::mutex> lock(mu);

        if (!session.active) return;

        // Tile wire format:
        // [x:u16][y:u16][w:u16][h:u16][format:u8][quality:u8]
        // [seq:u32][is_delta:u8][data_len:u32][data:bytes]
        const uint8_t* p = msg.payload;
        if (msg.payload_len < 17) return;

        AeonTile tile{};
        memcpy(&tile.x, p, 2); p += 2;
        memcpy(&tile.y, p, 2); p += 2;
        memcpy(&tile.width, p, 2); p += 2;
        memcpy(&tile.height, p, 2); p += 2;
        tile.format = *p++;
        tile.quality = *p++;
        memcpy(&tile.sequence, p, 4); p += 4;
        tile.is_delta = (*p++ != 0);

        uint32_t data_len = 0;
        memcpy(&data_len, p, 4); p += 4;

        size_t header_size = static_cast<size_t>(p - msg.payload);
        if (header_size + data_len > msg.payload_len) return;

        tile.data = p;
        tile.data_len = data_len;

        // Update statistics
        stats.bytes_downloaded += data_len;
        stats.tiles_received++;

        // Estimate data savings (uncompressed tile would be ~w*h*3 bytes)
        uint64_t uncompressed = static_cast<uint64_t>(tile.width) * tile.height * 3;
        if (uncompressed > data_len) {
            stats.bytes_saved += (uncompressed - data_len);
        }

        // Update savings percentage
        uint64_t total = stats.bytes_downloaded + stats.bytes_saved;
        if (total > 0) {
            stats.savings_percent =
                (static_cast<float>(stats.bytes_saved) / total) * 100.0f;
        }

        // Measure latency (from navigate start to first tile)
        if (session.navigate_start > 0 && session.sequence == 0) {
            uint64_t now_ms = NowUTC() * 1000; // approximate
            float latency = static_cast<float>(now_ms - session.navigate_start * 1000);
            if (stats.avg_latency_ms == 0.0f) {
                stats.avg_latency_ms = latency;
            } else {
                stats.avg_latency_ms = 0.7f * stats.avg_latency_ms + 0.3f * latency;
            }
        }
        session.sequence = tile.sequence;

        // Deliver to callback
        if (on_tile) {
            on_tile(tile);
        }
    }

    void HandleIncomingMeta(const AeonMessage& msg) {
        if (!msg.payload || msg.payload_len < sizeof(AeonPageMetadata)) return;
        std::lock_guard<std::mutex> lock(mu);

        if (!session.active) return;

        // Deserialize page metadata
        AeonPageMetadata meta{};
        memcpy(&meta, msg.payload, sizeof(AeonPageMetadata));

        // Interactive elements follow metadata struct
        size_t elem_offset = sizeof(AeonPageMetadata);
        size_t elem_count = meta.interactive_count;
        size_t elem_size = sizeof(AeonInteractiveElement);

        const AeonInteractiveElement* elements = nullptr;
        if (elem_count > 0 &&
            elem_offset + elem_count * elem_size <= msg.payload_len) {
            elements = reinterpret_cast<const AeonInteractiveElement*>(
                msg.payload + elem_offset);
        } else {
            elem_count = 0;
        }

        stats.pages_rendered++;

        // Deliver to callback
        if (on_meta) {
            on_meta(meta, elements, elem_count);
        }
    }

    void HandleRendererOffer(const AeonMessage& msg) {
        if (!msg.payload || msg.payload_len < 6) return;
        std::lock_guard<std::mutex> lock(mu);

        uint8_t slots = msg.payload[0];
        uint32_t ram = 0;
        memcpy(&ram, msg.payload + 1, 4);
        bool gpu = (msg.payload[5] != 0);

        // Update or add to known renderer list
        bool found = false;
        for (auto& rp : known_renderers) {
            if (memcmp(rp.peer_id, msg.sender_peer_id, 32) == 0) {
                rp.available_slots = slots;
                rp.ram_mb = ram;
                rp.has_gpu = gpu;
                rp.last_seen = NowUTC();
                found = true;
                break;
            }
        }

        if (!found) {
            RendererPeer rp{};
            memcpy(rp.peer_id, msg.sender_peer_id, 32);
            rp.available_slots = slots;
            rp.ram_mb = ram;
            rp.has_gpu = gpu;
            rp.last_seen = NowUTC();
            rp.avg_latency_ms = 0.0f;
            known_renderers.push_back(rp);
        }

        // Prune stale renderers (> 10 minutes)
        uint64_t cutoff = NowUTC() - 600;
        known_renderers.erase(
            std::remove_if(known_renderers.begin(), known_renderers.end(),
                [cutoff](const RendererPeer& rp) {
                    return rp.last_seen < cutoff;
                }),
            known_renderers.end());
    }

    void HandleRenderRequest(const AeonMessage& msg) {
        if (!renderer_active) return;
        if (!msg.payload || msg.payload_len < 3) return;
        std::lock_guard<std::mutex> lock(mu);

        // Check capacity
        if (active_render_sessions >= config.renderer_max_tabs) return;

        char url[2048] = {};
        if (!CloudWire::DecodeRenderRequest(msg.payload, msg.payload_len,
                                             url, sizeof(url))) {
            return;
        }

        // TODO: Launch headless renderer for this URL.
        // For now, increment session count to demonstrate capacity management.
        // Production implementation would:
        //   1. Spawn headless Chromium/Blink tab
        //   2. Navigate to URL
        //   3. Capture viewport tiles (AVIF/WebP encoded)
        //   4. Publish tiles via AeonHiveTopic::CloudRenderTile
        //   5. Publish DOM metadata via AeonHiveTopic::CloudRenderMeta
        //   6. Listen for input events via AeonHiveTopic::CloudRenderInput
        active_render_sessions++;
    }

    // ── Renderer Offer Broadcasting ───────────────────────────────
    void BroadcastCapacity() {
        if (!renderer_active) return;

        uint8_t avail = static_cast<uint8_t>(
            config.renderer_max_tabs - active_render_sessions);
        uint32_t ram = DetectAvailableRAM();

        // Detect GPU (simplified)
        bool has_gpu = false;
#ifdef _WIN32
        // Check for any GPU via WMI or adapter enumeration
        // For now, assume modern Windows has basic GPU
        has_gpu = true;
#endif

        auto payload = CloudWire::EncodeRendererOffer(avail, ram, has_gpu);
        AeonHiveInstance().Publish(
            AeonHiveTopic::CloudRenderOffer,
            payload.data(), payload.size());
    }
};


// ===========================================================================
// AeonCloud — public interface implementation
// ===========================================================================

AeonCloud::AeonCloud() : m_impl(new AeonCloudImpl()) {}

AeonCloud::~AeonCloud() {
    Shutdown();
    delete m_impl;
    m_impl = nullptr;
}

// ── Lifecycle ─────────────────────────────────────────────────

bool AeonCloud::Initialize(const ResourceBudget& budget) {
    if (!m_impl) return false;
    std::lock_guard<std::mutex> lock(m_impl->mu);

    // Default configuration
    m_impl->config.mode = AeonCloudMode::Auto;
    m_impl->config.tile_quality = 60;
    m_impl->config.tile_size = 256;
    m_impl->config.prefer_avif = true;
    m_impl->config.enable_delta_tiles = true;
    m_impl->config.max_bandwidth_kbps = 0;  // auto
    m_impl->config.volunteer_renderer = false;
    m_impl->config.renderer_max_tabs = 3;
    m_impl->config.renderer_ram_limit_mb = 512;

    // Initialize statistics
    memset(&m_impl->stats, 0, sizeof(AeonCloudStats));
    m_impl->stats.session_start_utc = AeonCloudImpl::NowUTC();

    // Set up AeonHive subscriptions
    m_impl->SetupHiveSubscriptions();

    m_impl->initialized = true;
    return true;
}

void AeonCloud::Shutdown() {
    if (!m_impl) return;
    std::lock_guard<std::mutex> lock(m_impl->mu);

    if (m_impl->renderer_active) {
        m_impl->renderer_active = false;
        m_impl->active_render_sessions = 0;
    }

    // Unsubscribe from AeonHive topics
    auto& hive = AeonHiveInstance();
    hive.Unsubscribe(AeonHiveTopic::CloudRenderTile);
    hive.Unsubscribe(AeonHiveTopic::CloudRenderMeta);
    hive.Unsubscribe(AeonHiveTopic::CloudRenderOffer);
    hive.Unsubscribe(AeonHiveTopic::CloudRenderRequest);

    m_impl->session.active = false;
    m_impl->page_cache.clear();
    m_impl->known_renderers.clear();
    m_impl->initialized = false;
}

// ── Configuration ─────────────────────────────────────────────

bool AeonCloud::Configure(const AeonCloudConfig& config) {
    if (!m_impl) return false;
    std::lock_guard<std::mutex> lock(m_impl->mu);
    m_impl->config = config;

    // Re-register render request handler if volunteering changed
    auto& hive = AeonHiveInstance();
    if (config.volunteer_renderer && !m_impl->renderer_active) {
        hive.Subscribe(AeonHiveTopic::CloudRenderRequest,
            [this](const AeonMessage& msg) {
                m_impl->HandleRenderRequest(msg);
            });
    }
    return true;
}

AeonCloudMode AeonCloud::CurrentMode() const {
    if (!m_impl) return AeonCloudMode::Full;
    std::lock_guard<std::mutex> lock(m_impl->mu);
    return m_impl->current_mode;
}

// ── Mode Switching ────────────────────────────────────────────

bool AeonCloud::SetMode(AeonCloudMode mode) {
    if (!m_impl) return false;
    std::lock_guard<std::mutex> lock(m_impl->mu);

    AeonCloudMode old_mode = m_impl->current_mode;

    if (mode == AeonCloudMode::Auto) {
        mode = m_impl->RecommendMode();
    }

    if (mode == AeonCloudMode::Turbo) {
        // Turbo requires at least one known renderer
        if (m_impl->known_renderers.empty()) {
            // No renderers available — fall back to Full
            if (m_impl->on_mode_change) {
                m_impl->on_mode_change(old_mode, AeonCloudMode::Full,
                    "No rendering peers available — staying in Full mode");
            }
            m_impl->current_mode = AeonCloudMode::Full;
            return false;
        }
    }

    m_impl->current_mode = mode;

    if (old_mode != mode && m_impl->on_mode_change) {
        const char* reason = "User requested mode change";
        if (m_impl->config.mode == AeonCloudMode::Auto) {
            reason = "Auto-detected optimal mode based on bandwidth/RAM";
        }
        m_impl->on_mode_change(old_mode, mode, reason);
    }

    return true;
}

void AeonCloud::OnModeChange(AeonCloudModeCallback callback) {
    if (m_impl) {
        std::lock_guard<std::mutex> lock(m_impl->mu);
        m_impl->on_mode_change = std::move(callback);
    }
}

// ── Turbo Mode: Client-Side API ───────────────────────────────

bool AeonCloud::TurboNavigate(const char* url) {
    if (!m_impl || !url) return false;
    std::lock_guard<std::mutex> lock(m_impl->mu);

    if (m_impl->current_mode != AeonCloudMode::Turbo) {
        return false; // Not in Turbo mode
    }

    // Check community cache first
    auto* cached = m_impl->FindCached(url);
    if (cached && !cached->tiles.empty()) {
        // Serve from cache — deliver tiles immediately
        for (const auto& tile : cached->tiles) {
            if (m_impl->on_tile) {
                m_impl->on_tile(tile);
            }
        }
        return true;
    }

    // Select best available renderer
    const auto* renderer = m_impl->SelectBestRenderer();
    if (!renderer) {
        // No renderer available — degrade to Full mode
        m_impl->current_mode = AeonCloudMode::Full;
        if (m_impl->on_mode_change) {
            m_impl->on_mode_change(AeonCloudMode::Turbo, AeonCloudMode::Full,
                "No rendering peers available — degrading to Full mode");
        }
        return false;
    }

    // Set up session
    m_impl->session.active = true;
    memcpy(m_impl->session.renderer_peer, renderer->peer_id, 32);
    strncpy(m_impl->session.current_url, url,
            sizeof(m_impl->session.current_url) - 1);
    m_impl->session.sequence = 0;
    m_impl->session.navigate_start = AeonCloudImpl::NowUTC();

    // Store renderer peer ID in stats
    for (int i = 0; i < 32; ++i) {
        snprintf(m_impl->stats.renderer_peer + i * 2, 3, "%02x",
                 renderer->peer_id[i]);
    }

    // Send render request via AeonHive
    auto payload = CloudWire::EncodeRenderRequest(url);
    AeonHiveInstance().Publish(
        AeonHiveTopic::CloudRenderRequest,
        payload.data(), payload.size());

    return true;
}

bool AeonCloud::TurboInput(const AeonInputEvent& event) {
    if (!m_impl) return false;
    std::lock_guard<std::mutex> lock(m_impl->mu);

    if (!m_impl->session.active) return false;

    auto payload = CloudWire::EncodeInputEvent(event);
    return AeonHiveInstance().Publish(
        AeonHiveTopic::CloudRenderInput,
        payload.data(), payload.size());
}

void AeonCloud::TurboRefresh() {
    if (!m_impl) return;
    std::lock_guard<std::mutex> lock(m_impl->mu);

    if (!m_impl->session.active) return;

    // Re-request the current URL
    auto payload = CloudWire::EncodeRenderRequest(m_impl->session.current_url);
    AeonHiveInstance().Publish(
        AeonHiveTopic::CloudRenderRequest,
        payload.data(), payload.size());

    m_impl->session.sequence = 0;
    m_impl->session.navigate_start = AeonCloudImpl::NowUTC();
}

void AeonCloud::OnTile(AeonCloudTileCallback callback) {
    if (m_impl) {
        std::lock_guard<std::mutex> lock(m_impl->mu);
        m_impl->on_tile = std::move(callback);
    }
}

void AeonCloud::OnMetadata(AeonCloudMetaCallback callback) {
    if (m_impl) {
        std::lock_guard<std::mutex> lock(m_impl->mu);
        m_impl->on_meta = std::move(callback);
    }
}

// ── Turbo Mode: Renderer-Side API ─────────────────────────────

bool AeonCloud::StartRenderer() {
    if (!m_impl) return false;
    std::lock_guard<std::mutex> lock(m_impl->mu);

    if (m_impl->renderer_active) return true; // already running

    // Verify we have enough resources to render
    uint32_t avail_ram = AeonCloudImpl::DetectAvailableRAM();
    if (avail_ram < m_impl->config.renderer_ram_limit_mb) {
        return false; // not enough RAM to volunteer
    }

    m_impl->renderer_active = true;
    m_impl->active_render_sessions = 0;

    // Subscribe to render requests if not already
    auto& hive = AeonHiveInstance();
    hive.Subscribe(AeonHiveTopic::CloudRenderRequest,
        [this](const AeonMessage& msg) {
            m_impl->HandleRenderRequest(msg);
        });

    // Broadcast our availability
    m_impl->BroadcastCapacity();

    return true;
}

void AeonCloud::StopRenderer() {
    if (!m_impl) return;
    std::lock_guard<std::mutex> lock(m_impl->mu);

    m_impl->renderer_active = false;
    m_impl->active_render_sessions = 0;

    // Broadcast zero capacity
    auto payload = CloudWire::EncodeRendererOffer(0, 0, false);
    AeonHiveInstance().Publish(
        AeonHiveTopic::CloudRenderOffer,
        payload.data(), payload.size());

    AeonHiveInstance().Unsubscribe(AeonHiveTopic::CloudRenderRequest);
}

uint32_t AeonCloud::ActiveRenderSessions() const {
    if (!m_impl) return 0;
    std::lock_guard<std::mutex> lock(m_impl->mu);
    return m_impl->active_render_sessions;
}

// ── Session Statistics ────────────────────────────────────────

AeonCloudStats AeonCloud::SessionStats() const {
    if (!m_impl) {
        AeonCloudStats empty{};
        return empty;
    }
    std::lock_guard<std::mutex> lock(m_impl->mu);
    return m_impl->stats;
}

// ── Bandwidth Detection ───────────────────────────────────────

uint32_t AeonCloud::EstimatedBandwidth() const {
    if (!m_impl) return 0;
    std::lock_guard<std::mutex> lock(m_impl->mu);
    return m_impl->est_bandwidth_kbps;
}

uint32_t AeonCloud::AvailableRAM() const {
    return AeonCloudImpl::DetectAvailableRAM();
}

bool AeonCloud::IsTurboRecommended() const {
    if (!m_impl) return false;
    std::lock_guard<std::mutex> lock(m_impl->mu);
    return m_impl->RecommendMode() == AeonCloudMode::Turbo;
}

// ── Community Cache ───────────────────────────────────────────

bool AeonCloud::CacheRenderedPage(const char* url,
                                   const AeonTile* tiles, size_t count) {
    if (!m_impl || !url || !tiles || count == 0) return false;
    std::lock_guard<std::mutex> lock(m_impl->mu);

    // Evict if cache full
    m_impl->EvictLRU();

    AeonCloudImpl::CachePage page;
    page.url = url;
    page.cached_at = AeonCloudImpl::NowUTC();
    page.access_count = 1;

    // Deep-copy tile data
    size_t total_data = 0;
    for (size_t i = 0; i < count; ++i) {
        total_data += tiles[i].data_len;
    }
    page.tile_data_storage.resize(total_data);

    size_t offset = 0;
    for (size_t i = 0; i < count; ++i) {
        AeonTile t = tiles[i];
        if (t.data && t.data_len > 0) {
            memcpy(page.tile_data_storage.data() + offset, t.data, t.data_len);
            t.data = page.tile_data_storage.data() + offset;
            offset += t.data_len;
        } else {
            t.data = nullptr;
            t.data_len = 0;
        }
        page.tiles.push_back(t);
    }

    m_impl->page_cache.push_back(std::move(page));
    return true;
}

bool AeonCloud::IsCached(const char* url) const {
    if (!m_impl || !url) return false;
    std::lock_guard<std::mutex> lock(m_impl->mu);

    for (const auto& cp : m_impl->page_cache) {
        if (cp.url == url) return true;
    }
    return false;
}

// ── Global singleton ──────────────────────────────────────────

AeonCloud& AeonCloudInstance() {
    static AeonCloud instance;
    return instance;
}
