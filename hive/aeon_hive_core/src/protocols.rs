// =============================================================================
// protocols.rs — Wire protocol definitions for AeonHive P2P messages
//
// Defines the serializable message types that flow between peers.
// All messages are postcard-encoded for compact wire format.
// =============================================================================

use serde::{Deserialize, Serialize};

// ── Bridge Config Protocol ───────────────────────────────────────────────────

/// A circumvention bridge configuration distributed via gossip.
/// Must be signed by sovereign key to be accepted.
#[derive(Debug, Clone, Serialize, Deserialize)]
pub struct BridgeConfig {
    /// Unique bridge ID
    pub bridge_id: String,
    /// Protocol type: "vless_reality", "ws_tunnel", "shadowsocks", etc.
    pub protocol: String,
    /// Server address (IP or hostname)
    pub server: String,
    /// Server port
    pub port: u16,
    /// Protocol-specific config (JSON blob)
    pub config: String,
    /// Countries where this bridge is effective (ISO 3166-1 alpha-2)
    pub target_countries: Vec<String>,
    /// Health: 0.0-1.0 (updated by intel reports)
    pub health: f64,
    /// When this config was generated (epoch seconds)
    pub created_at: u64,
    /// When this config expires (epoch seconds, 0 = never)
    pub expires_at: u64,
}

// ── DNS Cache Protocol ───────────────────────────────────────────────────────

/// A cached DNS resolution shared between peers.
#[derive(Debug, Clone, Serialize, Deserialize)]
pub struct DnsCacheEntry {
    /// Domain name (e.g., "youtube.com")
    pub domain: String,
    /// Resolved IP addresses
    pub addresses: Vec<String>,
    /// DNS record type (A, AAAA, CNAME)
    pub record_type: String,
    /// TTL in seconds
    pub ttl: u32,
    /// When this was resolved (epoch seconds)
    pub resolved_at: u64,
    /// Which DoH resolver was used
    pub resolver: String,
}

impl DnsCacheEntry {
    /// Check if this entry is still valid
    pub fn is_valid(&self) -> bool {
        let now = std::time::SystemTime::now()
            .duration_since(std::time::UNIX_EPOCH)
            .unwrap_or_default()
            .as_secs();
        now < self.resolved_at + self.ttl as u64
    }
}

// ── Censorship Intelligence Protocol ─────────────────────────────────────────

/// An anonymized censorship status report from a peer.
/// No identifying information — just what works and what doesn't.
#[derive(Debug, Clone, Serialize, Deserialize)]
pub struct IntelReport {
    /// Country code where this was tested (ISO 3166-1 alpha-2)
    pub country: String,
    /// Strategy that was attempted
    pub strategy: String,
    /// Result: "success", "blocked", "timeout", "degraded"
    pub result: String,
    /// Latency in milliseconds (0 if blocked)
    pub latency_ms: u32,
    /// When this was tested (epoch seconds)
    pub tested_at: u64,
    /// Target domain category (not the actual domain for privacy)
    /// e.g., "video_streaming", "social_media", "news"
    pub domain_category: String,
}

// ── Relay Announce Protocol ──────────────────────────────────────────────────

/// A peer announcing its availability as a traffic relay.
#[derive(Debug, Clone, Serialize, Deserialize)]
pub struct RelayAnnouncement {
    /// Relay's public key (hex)
    pub peer_id: String,
    /// Whether this peer is accepting new connections
    pub available: bool,
    /// Current load: 0.0 (idle) to 1.0 (full)
    pub load: f64,
    /// Max concurrent tunnel connections
    pub max_tunnels: u32,
    /// Current active tunnels
    pub active_tunnels: u32,
    /// Country code where this relay is located
    pub country: String,
    /// Bandwidth cap in kbps (0 = unlimited)
    pub bandwidth_cap_kbps: u64,
    /// Uptime in seconds
    pub uptime_secs: u64,
}

// ── Update Manifest Protocol ─────────────────────────────────────────────────

/// A sovereign-signed update manifest for browser binary distribution.
#[derive(Debug, Clone, Serialize, Deserialize)]
pub struct UpdateManifest {
    /// Release version (semver)
    pub version: String,
    /// Chromium base version
    pub chromium_base: String,
    /// UTC timestamp of release
    pub timestamp: u64,
    /// Chunks that make up the binary
    pub chunks: Vec<UpdateChunk>,
    /// Total binary size
    pub total_size: u64,
    /// Whether this is an emergency security update
    pub emergency: bool,
    /// Rollback window in hours (0 = no rollback)
    pub rollback_window_hours: u32,
}

/// A single chunk of a browser update.
#[derive(Debug, Clone, Serialize, Deserialize)]
pub struct UpdateChunk {
    /// Chunk index (0-based)
    pub index: u32,
    /// BLAKE3 hash of chunk contents
    pub blake3: String,
    /// Size in bytes
    pub size: u64,
}

// ── Peer Hello Protocol ──────────────────────────────────────────────────────

/// Initial handshake message when two peers connect.
#[derive(Debug, Clone, Serialize, Deserialize)]
pub struct PeerHello {
    /// Peer's public key (hex)
    pub peer_id: String,
    /// AeonHive protocol version
    pub protocol_version: u32,
    /// Aeon Browser version
    pub browser_version: String,
    /// Capabilities this peer offers
    pub capabilities: Vec<PeerCapability>,
    /// Country (self-reported, for relay routing)
    pub country: String,
}

/// What a peer can do for the network.
#[derive(Debug, Clone, Serialize, Deserialize, PartialEq, Eq)]
pub enum PeerCapability {
    /// Can relay traffic for censored peers
    Relay,
    /// Can seed browser update chunks
    Seeder,
    /// Can compile source chunks (build worker)
    Builder,
    /// Can offload translation tasks
    Translator,
    /// Reports censorship intelligence
    Sentinel,
}
