// =============================================================================
// reputation.rs — Peer reputation scoring
//
// Every peer starts at 0.5 reputation. Good behavior (successful relay,
// valid messages, uptime) increases reputation. Bad behavior (invalid sigs,
// failed relay, spam) decreases it.
//
// Peers below 0.1 are isolated (no messages forwarded to/from them).
// Peers above 0.8 become eligible for Sentinel role (validates bridges).
// =============================================================================

use std::collections::HashMap;
use parking_lot::RwLock;
use serde::{Deserialize, Serialize};
use chrono::{DateTime, Utc};

use crate::identity::PeerId;

/// Reputation record for a single peer
#[derive(Debug, Clone, Serialize, Deserialize)]
pub struct PeerReputation {
    pub peer_id: PeerId,
    pub score: f64, // 0.0 to 1.0
    pub total_relayed: u64,
    pub total_failed: u64,
    pub messages_sent: u64,
    pub invalid_sigs: u64,
    pub first_seen: DateTime<Utc>,
    pub last_seen: DateTime<Utc>,
    pub banned: bool,
}

impl PeerReputation {
    pub fn new(peer_id: PeerId) -> Self {
        let now = Utc::now();
        Self {
            peer_id,
            score: 0.5,
            total_relayed: 0,
            total_failed: 0,
            messages_sent: 0,
            invalid_sigs: 0,
            first_seen: now,
            last_seen: now,
            banned: false,
        }
    }

    /// Reward for good behavior
    pub fn reward(&mut self, amount: f64) {
        self.score = (self.score + amount).min(1.0);
        self.last_seen = Utc::now();
    }

    /// Penalize for bad behavior
    pub fn penalize(&mut self, amount: f64) {
        self.score = (self.score - amount).max(0.0);
        self.last_seen = Utc::now();
    }

    /// Whether this peer should be isolated
    pub fn is_isolated(&self) -> bool {
        self.banned || self.score < 0.1
    }

    /// Whether this peer is eligible for Sentinel role
    pub fn is_sentinel_eligible(&self) -> bool {
        !self.banned
            && self.score >= 0.8
            && self.messages_sent >= 100
            && self.invalid_sigs == 0
    }

    /// Age of this peer in hours
    pub fn age_hours(&self) -> f64 {
        (Utc::now() - self.first_seen).num_seconds() as f64 / 3600.0
    }
}

/// Thread-safe reputation database
pub struct ReputationDB {
    peers: RwLock<HashMap<PeerId, PeerReputation>>,
}

impl ReputationDB {
    pub fn new() -> Self {
        Self {
            peers: RwLock::new(HashMap::new()),
        }
    }

    /// Get or create a reputation record for a peer
    pub fn get_or_create(&self, peer_id: &PeerId) -> PeerReputation {
        let read = self.peers.read();
        if let Some(rep) = read.get(peer_id) {
            return rep.clone();
        }
        drop(read);

        let mut write = self.peers.write();
        write
            .entry(peer_id.clone())
            .or_insert_with(|| PeerReputation::new(peer_id.clone()))
            .clone()
    }

    /// Record a successful relay
    pub fn record_relay_success(&self, peer_id: &PeerId) {
        let mut write = self.peers.write();
        let rep = write.entry(peer_id.clone())
            .or_insert_with(|| PeerReputation::new(peer_id.clone()));
        rep.total_relayed += 1;
        rep.reward(0.01); // Small reward per relay
    }

    /// Record a relay failure
    pub fn record_relay_failure(&self, peer_id: &PeerId) {
        let mut write = self.peers.write();
        let rep = write.entry(peer_id.clone())
            .or_insert_with(|| PeerReputation::new(peer_id.clone()));
        rep.total_failed += 1;
        rep.penalize(0.05); // Larger penalty for failure
    }

    /// Record a valid message received
    pub fn record_valid_message(&self, peer_id: &PeerId) {
        let mut write = self.peers.write();
        let rep = write.entry(peer_id.clone())
            .or_insert_with(|| PeerReputation::new(peer_id.clone()));
        rep.messages_sent += 1;
        rep.reward(0.001); // Tiny reward per message
    }

    /// Record an invalid signature (serious offense)
    pub fn record_invalid_sig(&self, peer_id: &PeerId) {
        let mut write = self.peers.write();
        let rep = write.entry(peer_id.clone())
            .or_insert_with(|| PeerReputation::new(peer_id.clone()));
        rep.invalid_sigs += 1;
        rep.penalize(0.2); // Heavy penalty
    }

    /// Sovereign ban a peer permanently
    pub fn ban_peer(&self, peer_id: &PeerId) {
        let mut write = self.peers.write();
        let rep = write.entry(peer_id.clone())
            .or_insert_with(|| PeerReputation::new(peer_id.clone()));
        rep.banned = true;
        rep.score = 0.0;
    }

    /// Check if a peer is isolated (should not interact with)
    pub fn is_isolated(&self, peer_id: &PeerId) -> bool {
        let read = self.peers.read();
        read.get(peer_id).map(|r| r.is_isolated()).unwrap_or(false)
    }

    /// Get all peers sorted by reputation (highest first)
    pub fn ranked_peers(&self) -> Vec<PeerReputation> {
        let read = self.peers.read();
        let mut peers: Vec<_> = read.values().filter(|r| !r.is_isolated()).cloned().collect();
        peers.sort_by(|a, b| b.score.partial_cmp(&a.score).unwrap_or(std::cmp::Ordering::Equal));
        peers
    }

    /// Get peers eligible for relay role (score >= 0.5, not banned)
    pub fn relay_candidates(&self) -> Vec<PeerReputation> {
        let read = self.peers.read();
        read.values()
            .filter(|r| !r.banned && r.score >= 0.5)
            .cloned()
            .collect()
    }

    /// Peer count
    pub fn count(&self) -> usize {
        self.peers.read().len()
    }
}
