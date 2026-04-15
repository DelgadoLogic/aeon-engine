// =============================================================================
// node.rs — The AeonHive Node
//
// This is the main orchestrator that:
//   1. Creates an iroh QUIC endpoint with this node's Ed25519 identity
//   2. Connects to bootstrap relay (Cloud Run initially, peers later)
//   3. Discovers other peers via DHT (pkarr)
//   4. Handles incoming/outgoing protocol connections
//   5. Manages all sub-systems (DNS cache, bridge registry, relay, reputation)
//
// Every Aeon browser runs one HiveNode instance.
// The node starts as a Leaf and can graduate to Relay/Seeder/Builder/Sentinel
// based on capabilities and opt-in settings.
// =============================================================================

use std::path::PathBuf;
use std::sync::Arc;
use std::time::Duration;
use tokio::sync::{broadcast, mpsc};
use tracing;

#[allow(unused_imports)]
use crate::alpn;
use crate::bridge::BridgeRegistry;
use crate::dns_cache::DnsCache;
use crate::error::{HiveError, HiveResult};
use crate::identity::{HiveIdentity, PeerId};
use crate::protocols::*;
use crate::relay::{RelayConfig, RelayDirectory, RelayState};
use crate::reputation::ReputationDB;
use crate::topics::{HiveMessage, HiveTopic, Sig64};

/// Configuration for starting the HiveNode
#[derive(Debug, Clone)]
pub struct HiveNodeConfig {
    /// Data directory for persistent state
    pub data_dir: PathBuf,
    /// Bootstrap relay URLs (iroh relay servers)
    pub bootstrap_relays: Vec<String>,
    /// Bootstrap peer addresses for initial discovery
    pub bootstrap_peers: Vec<String>,
    /// Relay configuration
    pub relay_config: RelayConfig,
    /// Whether to accept offload tasks (translation, spell check)
    pub accept_offload: bool,
    /// Maximum peer connections
    pub max_peers: usize,
    /// Listen port (0 = random ephemeral)
    pub listen_port: u16,
}

impl Default for HiveNodeConfig {
    fn default() -> Self {
        let data_dir = dirs_next::data_local_dir()
            .unwrap_or_else(|| PathBuf::from("."))
            .join("Aeon");

        Self {
            data_dir,
            bootstrap_relays: vec![
                // AeonShield Cloud Run relay (Phase 1 bootstrap)
                "https://aeon-relay-y2r5ogip6q-ue.a.run.app".to_string(),
            ],
            bootstrap_peers: Vec::new(),
            relay_config: RelayConfig::default(),
            accept_offload: false,
            max_peers: 64,
            listen_port: 0,
        }
    }
}

/// Events emitted by the HiveNode
#[derive(Debug, Clone)]
pub enum HiveEvent {
    /// A new peer connected
    PeerConnected(PeerId),
    /// A peer disconnected
    PeerDisconnected(PeerId),
    /// Received a bridge config update
    BridgeUpdate(BridgeConfig),
    /// Received a DNS cache entry
    DnsCacheUpdate(DnsCacheEntry),
    /// Received a censorship intel report
    IntelReport(IntelReport),
    /// Relay directory changed
    RelayUpdate(RelayAnnouncement),
    /// Node status changed
    StatusChange(NodeStatus),
}

/// Current status of the HiveNode
#[derive(Debug, Clone)]
pub enum NodeStatus {
    Starting,
    Bootstrapping,
    Connected { peer_count: usize },
    Degraded { reason: String },
    Shutdown,
}

/// The main AeonHive node.
///
/// This wraps an iroh Endpoint and manages all P2P sub-systems.
/// Once `start()` is called, the node runs continuously in the background,
/// discovering peers, sharing data, and relaying traffic.
pub struct HiveNode {
    /// This node's persistent identity
    identity: HiveIdentity,
    /// Node configuration (used in Phase 2 for iroh Endpoint binding)
    #[allow(dead_code)]
    config: HiveNodeConfig,
    /// DNS cache (distributed)
    pub dns_cache: Arc<DnsCache>,
    /// Bridge registry (sovereign-signed)
    pub bridges: Arc<BridgeRegistry>,
    /// Relay state (local relay mode)
    pub relay: Arc<RelayState>,
    /// Known relay peers
    pub relay_directory: Arc<RelayDirectory>,
    /// Peer reputation tracking
    pub reputation: Arc<ReputationDB>,
    /// Event broadcast channel
    event_tx: broadcast::Sender<HiveEvent>,
    /// Shutdown signal
    shutdown_tx: Option<mpsc::Sender<()>>,
}

impl HiveNode {
    /// Create a new HiveNode with the given configuration.
    /// This initializes all sub-systems but does NOT start networking.
    /// Call `start()` to begin P2P operations.
    pub fn new(config: HiveNodeConfig) -> HiveResult<Self> {
        // Load or create persistent identity
        let identity = HiveIdentity::load_or_create(&config.data_dir)?;
        tracing::info!(
            peer_id = %identity.peer_id(),
            data_dir = %config.data_dir.display(),
            "AeonHive node initialized"
        );

        let (event_tx, _) = broadcast::channel(256);

        Ok(Self {
            identity,
            dns_cache: Arc::new(DnsCache::new()),
            bridges: Arc::new(BridgeRegistry::new()),
            relay: Arc::new(RelayState::new(config.relay_config.clone())),
            relay_directory: Arc::new(RelayDirectory::new()),
            reputation: Arc::new(ReputationDB::new()),
            config,
            event_tx,
            shutdown_tx: None,
        })
    }

    /// Get this node's peer ID
    pub fn peer_id(&self) -> PeerId {
        self.identity.peer_id()
    }

    /// Get this node's identity
    pub fn identity(&self) -> &HiveIdentity {
        &self.identity
    }

    /// Subscribe to node events
    pub fn subscribe(&self) -> broadcast::Receiver<HiveEvent> {
        self.event_tx.subscribe()
    }

    /// Start the HiveNode — begins P2P networking.
    ///
    /// This spawns background tasks for:
    ///   - iroh endpoint listening (incoming QUIC connections)
    ///   - Bootstrap peer discovery
    ///   - Periodic gossip rounds
    ///   - DNS cache eviction
    ///   - Relay announcements
    ///
    /// Returns a handle that can be used to shut down the node.
    pub async fn start(&mut self) -> HiveResult<()> {
        let (shutdown_tx, _shutdown_rx) = mpsc::channel::<()>(1);
        self.shutdown_tx = Some(shutdown_tx);

        let _ = self.event_tx.send(HiveEvent::StatusChange(NodeStatus::Starting));

        tracing::info!(
            peer_id = %self.peer_id(),
            "Starting AeonHive node..."
        );

        // ── Create iroh Endpoint ─────────────────────────────────────────
        //
        // The iroh Endpoint handles:
        //   - QUIC transport over UDP (looks like normal internet traffic)
        //   - NAT hole-punching (connects peers behind home/corporate NATs)
        //   - Relay fallback (when direct connection isn't possible)
        //   - Ed25519 identity (peer ID = public key)
        //
        // For Phase 1, we connect to our Cloud Run relay as bootstrap.
        // As peers join, direct QUIC connections replace relay traffic.
        //
        // NOTE: In production, this will use iroh::Endpoint directly.
        // For now, we simulate the endpoint lifecycle with the sub-systems
        // we've built (DNS cache, bridge registry, relay directory).

        let _ = self.event_tx.send(HiveEvent::StatusChange(NodeStatus::Bootstrapping));

        // Spawn background maintenance tasks
        let dns_cache = self.dns_cache.clone();
        let _bridges = self.bridges.clone();
        let relay_dir = self.relay_directory.clone();
        let _reputation = self.reputation.clone();
        let _event_tx = self.event_tx.clone();

        // Background: DNS cache eviction (every 60 seconds)
        let dns_cache_bg = dns_cache.clone();
        tokio::spawn(async move {
            let mut interval = tokio::time::interval(Duration::from_secs(60));
            loop {
                interval.tick().await;
                let evicted = dns_cache_bg.evict_expired();
                if evicted > 0 {
                    tracing::debug!(evicted, cache_size = dns_cache_bg.len(), "DNS cache eviction");
                }
            }
        });

        // Background: Relay directory cleanup (every 5 minutes)
        let relay_dir_bg = relay_dir.clone();
        tokio::spawn(async move {
            let mut interval = tokio::time::interval(Duration::from_secs(300));
            loop {
                interval.tick().await;
                relay_dir_bg.evict_stale();
                tracing::debug!(relays = relay_dir_bg.count(), "Relay directory cleanup");
            }
        });

        tracing::info!(
            peer_id = %self.peer_id(),
            dns_cache = self.dns_cache.len(),
            bridges = self.bridges.count(),
            relays = self.relay_directory.count(),
            "AeonHive node ONLINE"
        );

        let _ = self.event_tx.send(HiveEvent::StatusChange(
            NodeStatus::Connected { peer_count: 0 }
        ));

        Ok(())
    }

    /// Process an incoming gossip message.
    /// Verifies signature, checks reputation, routes to appropriate sub-system.
    pub fn handle_message(&self, msg: HiveMessage) -> HiveResult<()> {
        let sender = PeerId::from_hex(&msg.sender)?;

        // Check if peer is isolated
        if self.reputation.is_isolated(&sender) {
            tracing::warn!(peer = %sender, "Ignoring message from isolated peer");
            return Ok(());
        }

        // Verify message signature
        let signable = msg.signable_bytes();
        if HiveIdentity::verify(&sender, &signable, msg.signature.as_bytes()).is_err() {
            self.reputation.record_invalid_sig(&sender);
            return Err(HiveError::InvalidSignature);
        }

        // If topic requires sovereign sig, verify it
        if msg.topic.requires_sovereign_sig() {
            if let Some(ref sov_sig) = msg.sovereign_sig {
                HiveIdentity::verify_sovereign(&signable, sov_sig.as_bytes())?;
            } else {
                return Err(HiveError::Protocol(
                    "Missing sovereign signature on protected topic".into()
                ));
            }
        }

        // Record valid message for reputation
        self.reputation.record_valid_message(&sender);

        // Route to appropriate handler
        match msg.topic {
            HiveTopic::BridgeConfig => {
                let config: BridgeConfig = serde_json::from_slice(&msg.payload)?;
                if self.bridges.insert_verified(config.clone()) {
                    let _ = self.event_tx.send(HiveEvent::BridgeUpdate(config));
                }
            }
            HiveTopic::DnsCache => {
                let entry: DnsCacheEntry = serde_json::from_slice(&msg.payload)?;
                if self.dns_cache.insert_from_peer(entry.clone()) {
                    let _ = self.event_tx.send(HiveEvent::DnsCacheUpdate(entry));
                }
            }
            HiveTopic::CensorshipIntel => {
                let report: IntelReport = serde_json::from_slice(&msg.payload)?;
                // Update bridge health based on intel
                if !report.strategy.is_empty() {
                    let success = report.result == "success";
                    // Find bridges matching this strategy and update health
                    for bridge in self.bridges.all_active() {
                        if bridge.protocol == report.strategy
                            && bridge.target_countries.contains(&report.country) {
                            self.bridges.update_health(&bridge.bridge_id, success);
                        }
                    }
                }
                let _ = self.event_tx.send(HiveEvent::IntelReport(report));
            }
            HiveTopic::RelayAnnounce => {
                let announcement: RelayAnnouncement = serde_json::from_slice(&msg.payload)?;
                self.relay_directory.upsert(announcement.clone());
                let _ = self.event_tx.send(HiveEvent::RelayUpdate(announcement));
            }
            _ => {
                tracing::trace!(topic = ?msg.topic, "Unhandled topic");
            }
        }

        Ok(())
    }

    /// Publish a message to the mesh on a given topic.
    pub fn publish(&self, topic: HiveTopic, payload: &[u8]) -> HiveResult<HiveMessage> {
        let now = std::time::SystemTime::now()
            .duration_since(std::time::UNIX_EPOCH)
            .unwrap_or_default()
            .as_secs();

        let mut msg = HiveMessage {
            topic,
            sender: self.peer_id().to_hex(),
            timestamp: now,
            payload: payload.to_vec(),
            signature: Sig64::from_bytes([0u8; 64]),
            sovereign_sig: None,
            seq: now, // Use timestamp as monotonic seq for simplicity
        };

        // Sign the message
        let signable = msg.signable_bytes();
        msg.signature = Sig64::from_bytes(self.identity.sign(&signable));

        Ok(msg)
    }

    /// Publish a DNS cache entry to the mesh.
    pub fn share_dns(&self, entry: &DnsCacheEntry) -> HiveResult<()> {
        if !self.dns_cache.should_share(&entry.domain) {
            return Ok(()); // Already shared recently
        }

        let payload = serde_json::to_vec(entry)?;
        let _msg = self.publish(HiveTopic::DnsCache, &payload)?;
        self.dns_cache.mark_shared(&entry.domain);

        tracing::debug!(domain = %entry.domain, "DNS entry shared to mesh");
        Ok(())
    }

    /// Submit an intel report to the mesh.
    pub fn report_intel(&self, report: &IntelReport) -> HiveResult<()> {
        let payload = serde_json::to_vec(report)?;
        let _msg = self.publish(HiveTopic::CensorshipIntel, &payload)?;

        tracing::debug!(
            country = %report.country,
            strategy = %report.strategy,
            result = %report.result,
            "Intel report submitted to mesh"
        );
        Ok(())
    }

    /// Announce relay availability to the mesh.
    pub fn announce_relay(&self) -> HiveResult<()> {
        let announcement = self.relay.make_announcement(&self.peer_id());
        let payload = serde_json::to_vec(&announcement)?;
        let _msg = self.publish(HiveTopic::RelayAnnounce, &payload)?;

        tracing::info!(
            available = announcement.available,
            load = ?announcement.load,
            "Relay announcement published"
        );
        Ok(())
    }

    /// Get the best available relay for a censored user.
    pub fn find_relay(&self, user_country: &str) -> Option<RelayAnnouncement> {
        self.relay_directory.find_best_relay(Some(user_country))
    }

    /// Get a summary of this node's current state.
    pub fn status(&self) -> HiveNodeStatus {
        HiveNodeStatus {
            peer_id: self.peer_id().to_hex(),
            dns_cache_size: self.dns_cache.len(),
            bridge_count: self.bridges.count(),
            relay_count: self.relay_directory.count(),
            reputation_db_size: self.reputation.count(),
            relay_stats: self.relay.stats(),
        }
    }

    /// Shutdown the node gracefully.
    pub async fn shutdown(&mut self) {
        tracing::info!("AeonHive node shutting down...");
        let _ = self.event_tx.send(HiveEvent::StatusChange(NodeStatus::Shutdown));

        if let Some(tx) = self.shutdown_tx.take() {
            let _ = tx.send(()).await;
        }
    }
}

/// Summary of the node's current state
#[derive(Debug, Clone, serde::Serialize)]
pub struct HiveNodeStatus {
    pub peer_id: String,
    pub dns_cache_size: usize,
    pub bridge_count: usize,
    pub relay_count: usize,
    pub reputation_db_size: usize,
    pub relay_stats: crate::relay::RelayStats,
}
