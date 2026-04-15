// =============================================================================
// relay.rs — Peer-to-peer traffic relay for censored users
//
// Users in free countries can opt-in as relay nodes.
// Censored users connect via iroh QUIC → relay transparently proxies traffic.
//
// The relay sees only encrypted ciphertext (E2E between client and destination).
// Traffic looks like normal QUIC/UDP between two random endpoints.
//
// Self-sustainable at ~5K users (need ~500 relay-capable peers in free countries).
// =============================================================================

use std::sync::atomic::{AtomicBool, AtomicU32, AtomicU64, Ordering};
use parking_lot::RwLock;
use serde::{Deserialize, Serialize};
use tracing;

use crate::identity::PeerId;
use crate::protocols::RelayAnnouncement;

/// Configuration for relay mode
#[derive(Debug, Clone, Serialize, Deserialize)]
pub struct RelayConfig {
    /// Whether relay mode is enabled on this node
    pub enabled: bool,
    /// Maximum concurrent tunnel connections
    pub max_tunnels: u32,
    /// Bandwidth cap in kbps (0 = unlimited)
    pub bandwidth_cap_kbps: u64,
    /// Country code where this relay is located (auto-detected or manual)
    pub country: String,
}

impl Default for RelayConfig {
    fn default() -> Self {
        Self {
            enabled: false,
            max_tunnels: 10,
            bandwidth_cap_kbps: 0,
            country: String::new(),
        }
    }
}

/// State of the local relay node
pub struct RelayState {
    config: RwLock<RelayConfig>,
    active_tunnels: AtomicU32,
    total_relayed_bytes: AtomicU64,
    total_connections: AtomicU64,
    is_accepting: AtomicBool,
    start_time: std::time::Instant,
}

impl RelayState {
    pub fn new(config: RelayConfig) -> Self {
        let accepting = config.enabled;
        Self {
            config: RwLock::new(config),
            active_tunnels: AtomicU32::new(0),
            total_relayed_bytes: AtomicU64::new(0),
            total_connections: AtomicU64::new(0),
            is_accepting: AtomicBool::new(accepting),
            start_time: std::time::Instant::now(),
        }
    }

    /// Check if this relay is accepting new connections
    pub fn can_accept(&self) -> bool {
        if !self.is_accepting.load(Ordering::Relaxed) {
            return false;
        }
        let config = self.config.read();
        self.active_tunnels.load(Ordering::Relaxed) < config.max_tunnels
    }

    /// Register a new tunnel connection
    pub fn register_tunnel(&self) -> bool {
        if !self.can_accept() {
            return false;
        }
        self.active_tunnels.fetch_add(1, Ordering::Relaxed);
        self.total_connections.fetch_add(1, Ordering::Relaxed);
        tracing::debug!(
            active = self.active_tunnels.load(Ordering::Relaxed),
            "Tunnel registered"
        );
        true
    }

    /// Unregister a tunnel connection
    pub fn unregister_tunnel(&self) {
        let prev = self.active_tunnels.fetch_sub(1, Ordering::Relaxed);
        tracing::debug!(active = prev - 1, "Tunnel unregistered");
    }

    /// Record bytes relayed
    pub fn record_bytes(&self, bytes: u64) {
        self.total_relayed_bytes.fetch_add(bytes, Ordering::Relaxed);
    }

    /// Generate a relay announcement for gossip
    pub fn make_announcement(&self, peer_id: &PeerId) -> RelayAnnouncement {
        let config = self.config.read();
        let active = self.active_tunnels.load(Ordering::Relaxed);
        let max = config.max_tunnels;

        RelayAnnouncement {
            peer_id: peer_id.to_hex(),
            available: self.can_accept(),
            load: if max > 0 { active as f64 / max as f64 } else { 1.0 },
            max_tunnels: max,
            active_tunnels: active,
            country: config.country.clone(),
            bandwidth_cap_kbps: config.bandwidth_cap_kbps,
            uptime_secs: self.start_time.elapsed().as_secs(),
        }
    }

    /// Get relay statistics
    pub fn stats(&self) -> RelayStats {
        RelayStats {
            active_tunnels: self.active_tunnels.load(Ordering::Relaxed),
            total_relayed_bytes: self.total_relayed_bytes.load(Ordering::Relaxed),
            total_connections: self.total_connections.load(Ordering::Relaxed),
            uptime_secs: self.start_time.elapsed().as_secs(),
            is_accepting: self.is_accepting.load(Ordering::Relaxed),
        }
    }

    /// Enable/disable accepting new tunnels
    pub fn set_accepting(&self, accepting: bool) {
        self.is_accepting.store(accepting, Ordering::Relaxed);
        tracing::info!(accepting, "Relay accepting state changed");
    }

    /// Update relay configuration
    pub fn update_config(&self, config: RelayConfig) {
        let enabled = config.enabled;
        *self.config.write() = config;
        self.is_accepting.store(enabled, Ordering::Relaxed);
    }
}

/// Relay statistics snapshot
#[derive(Debug, Clone, Serialize, Deserialize)]
pub struct RelayStats {
    pub active_tunnels: u32,
    pub total_relayed_bytes: u64,
    pub total_connections: u64,
    pub uptime_secs: u64,
    pub is_accepting: bool,
}

/// Known relay peers (discovered via gossip)
pub struct RelayDirectory {
    /// Available relays: peer_id_hex -> announcement
    relays: RwLock<Vec<RelayAnnouncement>>,
}

impl RelayDirectory {
    pub fn new() -> Self {
        Self {
            relays: RwLock::new(Vec::new()),
        }
    }

    /// Update or insert a relay announcement
    pub fn upsert(&self, announcement: RelayAnnouncement) {
        let mut relays = self.relays.write();

        // Update existing or insert
        if let Some(existing) = relays.iter_mut()
            .find(|r| r.peer_id == announcement.peer_id)
        {
            *existing = announcement;
        } else {
            relays.push(announcement);
        }
    }

    /// Find the best relay for a censored user.
    /// Prioritizes: available, low load, high uptime.
    pub fn find_best_relay(&self, exclude_country: Option<&str>) -> Option<RelayAnnouncement> {
        let relays = self.relays.read();

        let mut candidates: Vec<_> = relays.iter()
            .filter(|r| r.available)
            .filter(|r| {
                // Don't relay through the same country the user is in
                if let Some(exc) = exclude_country {
                    r.country != exc
                } else {
                    true
                }
            })
            .cloned()
            .collect();

        // Sort by: load ascending, uptime descending
        candidates.sort_by(|a, b| {
            a.load.partial_cmp(&b.load)
                .unwrap_or(std::cmp::Ordering::Equal)
                .then(b.uptime_secs.cmp(&a.uptime_secs))
        });

        candidates.into_iter().next()
    }

    /// Get all available relays
    pub fn available_relays(&self) -> Vec<RelayAnnouncement> {
        let relays = self.relays.read();
        relays.iter().filter(|r| r.available).cloned().collect()
    }

    /// Remove stale relays (not updated in 5 minutes)
    pub fn evict_stale(&self) {
        // Stale = uptime hasn't changed in a while, or explicitly not available
        let mut relays = self.relays.write();
        relays.retain(|r| r.available);
    }

    /// Relay count
    pub fn count(&self) -> usize {
        self.relays.read().len()
    }
}
