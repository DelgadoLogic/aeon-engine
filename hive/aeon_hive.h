// =============================================================================
// aeon_hive.h — AeonHive Public API
// Graduated from: go-libp2p + yjs + iroh (all MIT/Apache 2.0)
//
// AeonHive is the sovereign P2P backbone of Aeon Browser.
// EVERYTHING depends on it:
//   • AeonSpell  → word frequency data from peers
//   • AeonCast   → device registry from peers
//   • AeonTranslate → offload translation to capable peers
//   • AeonUpdate → sovereign manifest broadcast + delivery
//   • AeonMind   → AI improvement loop (metrics in, insights out)
//   • AeonCE     → circumvention bridge discovery
//   • AeonPredict→ shared click/resource prediction models
//
// What we improve over upstream:
//   [+] C++ reimplementation of libp2p protocols (no Go runtime dependency)
//   [+] Noise protocol XX handshake for E2E encryption (like WireGuard, lighter)
//   [+] Kademlia DHT for peer discovery without central bootstrap server
//   [+] GossipSub for efficient manifest/metric broadcast
//   [+] CRDT sync (YATA algorithm from Yjs) for distributed state
//   [+] Peer scoring — bad actors get isolated, good peers get promoted
//   [+] Sovereign manifest relay — nodes relay signed updates they can't apply
//   [+] XP-safe: IOCP transport on Vista+, WSAAsyncSelect on XP
//   [+] Zero telemetry to any external server — purely P2P, user-controlled
// =============================================================================

#pragma once

#include "aeon_component.h"
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

// ---------------------------------------------------------------------------
// Forward declarations
// ---------------------------------------------------------------------------
class AeonHiveImpl;

// ---------------------------------------------------------------------------
// AeonPeer — a known peer in the Hive
// ---------------------------------------------------------------------------
struct AeonPeer {
    uint8_t  peer_id[32];       // Ed25519 public key = peer identity
    char     addr[64];          // "quic://1.2.3.4:9000" or "tcp://..."
    uint32_t protocol_version;
    float    reputation;        // 0.0–1.0 (starts at 0.5, adjusts with behavior)
    uint64_t last_seen_utc;
    bool     is_relay;          // true = can relay sovereign updates
    bool     has_gpu;           // true = can offload translation tasks
    uint32_t ram_mb;            // self-reported (unverified, for offload routing)
    uint8_t  cpu_class;         // AEON_CPU_CLASS_*
};

// ---------------------------------------------------------------------------
// AeonHiveTopic — named pubsub channel
// ---------------------------------------------------------------------------
enum class AeonHiveTopic : uint32_t {
    SovereignManifest  = 0x0001, // signed update broadcasts
    WordFrequency      = 0x0002, // spell check frequency data
    MetricReport       = 0x0003, // anonymous performance counters
    HiveInsight        = 0x0004, // AI-proposed improvements
    TranslateOffload   = 0x0005, // translation task requests
    DeviceRegistry     = 0x0006, // Cast device model DB
    CircumventBridge   = 0x0007, // bridge config for censored regions
    PredictHints       = 0x0008, // resource prefetch hints
    PeerScore          = 0x0009, // peer reputation updates

    // ── Pillar 1: Sovereign Crypto ────────────────────────────────────
    TrustStoreUpdate   = 0x0100, // Ed25519-signed CA bundle updates (AeonTLS)
    TrustStoreRequest  = 0x0101, // Request latest trust store from mesh

    // ── Pillar 2: Cloud-Assisted Rendering ────────────────────────────
    CloudRenderRequest = 0x0200, // Turbo Mode: client requests page render
    CloudRenderTile    = 0x0201, // Turbo Mode: renderer sends visual tiles
    CloudRenderMeta    = 0x0202, // Turbo Mode: DOM metadata + interactive map
    CloudRenderInput   = 0x0203, // Turbo Mode: client relays user input
    CloudRenderOffer   = 0x0204, // Renderer advertises available capacity

    // ── Pillar 3: P2P Distribution ────────────────────────────────────
    P2PUpdateManifest  = 0x0300, // Signed update manifest broadcast
    P2PUpdateChunk     = 0x0301, // Update binary chunk transfer
    P2PUpdateRequest   = 0x0302, // Request specific chunks from peers

    // ── Pillar 4: Zero-Rating & MNO ──────────────────────────────────
    PortalMNOManifest  = 0x0400, // MNO partner config broadcasts (signed)
    PortalSponsored    = 0x0401, // Sponsored content manifest (signed)
    PortalDataReport   = 0x0402, // Aggregated anonymous data usage stats
    PortalZeroRateCfg  = 0x0403, // Zero-rating header config updates

    // ── Pillar 5: Cross-Platform Sync ─────────────────────────────────
    SyncBookmarks      = 0x0500, // Encrypted bookmark CRDT sync
    SyncTabs           = 0x0501, // Open tab state across devices
    SyncPasswords      = 0x0502, // E2E encrypted password vault sync
    SyncHistory        = 0x0503, // Browsing history sync (opt-in)
};

// ---------------------------------------------------------------------------
// AeonMessage — a message received on a topic
// ---------------------------------------------------------------------------
struct AeonMessage {
    AeonHiveTopic topic;
    uint8_t       sender_peer_id[32];
    uint64_t      timestamp_utc;
    uint8_t       signature[64];    // Ed25519 sig by sender's key
    const uint8_t* payload;
    size_t         payload_len;
    bool           signature_valid; // verified by AeonHive before delivery
};

// ---------------------------------------------------------------------------
// Callbacks
// ---------------------------------------------------------------------------
using AeonHiveMessageCallback  = std::function<void(const AeonMessage& msg)>;
using AeonHivePeerCallback     = std::function<void(const AeonPeer& peer, bool joined)>;

// ---------------------------------------------------------------------------
// AeonHiveConfig — startup configuration
// ---------------------------------------------------------------------------
struct AeonHiveConfig {
    uint16_t    listen_port;         // 0 = random ephemeral port
    const char* bootstrap_peers[8];  // well-known peer addrs for initial discovery
    int         bootstrap_count;
    bool        relay_mode;          // true = relay manifests for others (good citizen)
    bool        accept_offload;      // true = accept translation/compute offload tasks
    bool        private_mode;        // true = only connect to explicitly trusted peers
    uint32_t    max_peers;           // peer table size (default 64, min 8)
    size_t      max_bandwidth_kbps;  // 0 = unlimited
};

// ---------------------------------------------------------------------------
// AeonOffloadRequest — request to perform work for a peer
// ---------------------------------------------------------------------------
struct AeonOffloadRequest {
    uint8_t  request_id[16];
    uint8_t  requester_peer_id[32];
    uint32_t task_type;             // AEON_OFFLOAD_TASK_*
    const uint8_t* payload;
    size_t   payload_len;
};

#define AEON_OFFLOAD_TASK_TRANSLATE 0x01
#define AEON_OFFLOAD_TASK_SPELL     0x02
#define AEON_OFFLOAD_TASK_INFERENCE 0x03

using AeonOffloadHandler = std::function<
    std::vector<uint8_t>(const AeonOffloadRequest& req)>;

// ---------------------------------------------------------------------------
// AeonHive — main P2P network node
// ---------------------------------------------------------------------------
class AeonHive final : public AeonComponentBase {
public:
    AeonHive();
    ~AeonHive() override;

    // ── IAeonComponent identity ───────────────────────────────────────────
    const char* ComponentId()      const override { return "aeon.hive"; }
    const char* ComponentVersion() const override { return "1.0.0"; }
    const char* UpstreamRef()      const override {
        return "go-libp2p@MIT + yjs@MIT + iroh@Apache2";
    }

    // ── Lifecycle ─────────────────────────────────────────────────────────
    bool Initialize(const ResourceBudget& budget) override;
    void Shutdown() override;

    // ── Node Identity ─────────────────────────────────────────────────────

    // Start the Hive node with config. Generates a key pair on first run,
    // persists it to %APPDATA%\Aeon\hive_identity.key
    bool Start(const AeonHiveConfig& config);
    void Stop();

    // Returns this node's peer ID (Ed25519 public key)
    const uint8_t* LocalPeerId() const;

    // Returns this node's reachable address (after NAT traversal)
    std::string LocalAddr() const;

    // ── Peer Discovery (Kademlia DHT) ─────────────────────────────────────

    // Connect to a specific peer by address
    bool Connect(const char* addr);

    // Disconnect from a peer
    void Disconnect(const uint8_t* peer_id);

    // Get all currently connected peers
    std::vector<AeonPeer> ConnectedPeers() const;

    // Find peers that can handle a specific offload task
    std::vector<AeonPeer> FindCapablePeers(uint32_t task_type, int count = 3) const;

    // Register callback for peer join/leave events
    void OnPeerEvent(AeonHivePeerCallback callback);

    // ── Pub/Sub (GossipSub) ───────────────────────────────────────────────

    // Subscribe to a topic — callback fires for each verified message
    void Subscribe(AeonHiveTopic topic, AeonHiveMessageCallback callback);
    void Unsubscribe(AeonHiveTopic topic);

    // Publish a signed message to all peers subscribed to a topic
    bool Publish(AeonHiveTopic topic, const uint8_t* payload, size_t len);

    // ── Sovereign Manifest Relay ──────────────────────────────────────────

    // Broadcast a sovereign manifest to the entire network.
    // manifest_bytes must be a fully signed AeonManifest blob.
    // Even nodes that can't apply this update will relay it.
    bool BroadcastManifest(const uint8_t* manifest_bytes, size_t len);

    // ── Metric Reporting (AI Hive loop) ──────────────────────────────────

    // Called by components to submit their metrics.
    // AeonHive aggregates with differential privacy noise before publishing.
    void SubmitMetrics(const ComponentMetrics& metrics);

    // Force a metric flush to the network (normally automatic every 5 min)
    void FlushMetrics();

    // ── Offload Protocol ─────────────────────────────────────────────────

    // Register a handler for inbound offload requests from peers
    // (only called if config.accept_offload == true)
    void RegisterOffloadHandler(uint32_t task_type, AeonOffloadHandler handler);

    // Send an offload request to a specific peer, get result via callback
    bool RequestOffload(
        const AeonPeer&   peer,
        const AeonOffloadRequest& req,
        std::function<void(const std::vector<uint8_t>& result, bool ok)> callback
    );

    // ── CRDT Shared State (Yjs-inspired YATA algorithm) ─────────────────

    // Get/set a shared CRDT document key.
    // Used for: shared device registry, shared word-freq index, etc.
    // Merges are handled automatically — concurrent updates always converge.
    std::vector<uint8_t> CrdtGet(const char* doc_key) const;
    bool CrdtApplyUpdate(const char* doc_key,
                         const uint8_t* update, size_t len);
    void OnCrdtUpdate(const char* doc_key,
                      std::function<void(const uint8_t* update, size_t len)> cb);

    // ── Peer Scoring ──────────────────────────────────────────────────────

    // Penalize a peer (bad manifest, failed offload, invalid message)
    void PenalizePeer(const uint8_t* peer_id, float amount);

    // Reward a peer (delivered valid manifest, good offload result)
    void RewardPeer(const uint8_t* peer_id, float amount);

    // ── Resource Awareness ────────────────────────────────────────────────
    bool CanOffloadToHive() const override { return false; } // Hive IS the offload layer

private:
    AeonHiveImpl* m_impl = nullptr;
};

// ── Global singleton ──────────────────────────────────────────────────────────
AeonHive& AeonHiveInstance();

// ── Sovereign Manifest Signing ───────────────────────────────────────────────
// The official DelgadoLogic manifest signing public key.
// Used to verify AeonHive sovereign manifests (version broadcasts, config).
// Generated: 2026-04-12 by sovereign-keys/aeon_keygen.py
// SHA-256: 843aef5e8040163028809be384b8dea93d12ed4f39de8fca462deaa366b3acf0
static const uint8_t AEON_MANIFEST_SIGNER_PUBKEY[32] = {
    0x24, 0x4c, 0xe1, 0x50, 0x36, 0x1f, 0x09, 0xdd,
    0xa4, 0x55, 0x74, 0x66, 0x56, 0xc0, 0xd4, 0xf9,
    0xf6, 0x48, 0xfb, 0x71, 0x61, 0x04, 0x28, 0x5d,
    0xa5, 0x39, 0xdb, 0x56, 0xc5, 0x6a, 0x80, 0x62,
};
