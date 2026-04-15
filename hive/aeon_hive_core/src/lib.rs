// =============================================================================
// AeonHive Core — lib.rs
// The sovereign P2P mesh node powered by iroh.
//
// This replaces the raw UDP gossip in aeon_hive.py with proper:
//   - QUIC encrypted transport (iroh)
//   - NAT hole-punching (iroh relay)
//   - Ed25519 persistent identity
//   - Topic-based messaging (protocol channels)
//   - DHT peer discovery (pkarr)
//
// Architecture: async Rust core → C FFI → Python/C++ consumers
// =============================================================================

pub mod identity;
pub mod node;
pub mod protocols;
pub mod topics;
pub mod bridge;
pub mod dns_cache;
pub mod relay;
pub mod reputation;
pub mod error;

pub use node::HiveNode;
pub use identity::HiveIdentity;
pub use error::HiveError;

/// Library version
pub const VERSION: &str = env!("CARGO_PKG_VERSION");

/// Protocol ALPNs — unique identifiers for each AeonHive sub-protocol
pub mod alpn {
    /// Main gossip channel for bridge configs, intelligence, and peer metadata
    pub const GOSSIP: &[u8] = b"aeon-hive/gossip/1";
    /// DNS cache sharing — peers exchange cached DNS resolutions
    pub const DNS_CACHE: &[u8] = b"aeon-hive/dns/1";
    /// Traffic relay — censored peers tunnel traffic through free peers
    pub const RELAY: &[u8] = b"aeon-hive/relay/1";
    /// Update distribution — iroh-blobs-style chunked binary transfer
    pub const UPDATE: &[u8] = b"aeon-hive/update/1";
    /// Intelligence — anonymized censorship status reports
    pub const INTEL: &[u8] = b"aeon-hive/intel/1";
}
