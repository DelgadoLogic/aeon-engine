// =============================================================================
// topics.rs — Named publish/subscribe topics for AeonHive messaging
//
// Each topic is a GossipSub-style channel. Messages are signed by the sender
// and optionally countersigned by the sovereign key.
// =============================================================================

use serde::{Deserialize, Serialize};

/// Named topics for P2P message routing.
/// Maps to the C++ AeonHiveTopic enum in aeon_hive.h
#[derive(Debug, Clone, Copy, PartialEq, Eq, Hash, Serialize, Deserialize)]
#[repr(u32)]
pub enum HiveTopic {
    /// Ed25519-signed bridge configs for circumvention
    BridgeConfig      = 0x0001,
    /// Anonymized censorship intelligence reports
    CensorshipIntel   = 0x0002,
    /// Browser update manifests and chunk availability
    UpdateManifest    = 0x0003,
    /// DNS cache sharing (resolved domains)
    DnsCache          = 0x0004,
    /// Peer relay availability announcements
    RelayAnnounce     = 0x0005,
    /// Spell check word frequency data
    WordFrequency     = 0x0006,
    /// Cast device model registry
    DeviceRegistry    = 0x0007,
    /// Translation offload requests
    TranslateOffload  = 0x0008,
    /// Peer reputation score updates
    PeerScore         = 0x0009,
    /// Sovereign commands (signed by DelgadoLogic key)
    SovereignCommand  = 0x000A,
    /// Vote coordination for autonomous patches
    VoteCoordination  = 0x000B,
}

impl HiveTopic {
    /// Get the topic's wire name (used as iroh ALPN sub-channel identifier)
    pub fn wire_name(&self) -> &'static str {
        match self {
            Self::BridgeConfig     => "aeon/bridge/v1",
            Self::CensorshipIntel  => "aeon/intel/v1",
            Self::UpdateManifest   => "aeon/update/v1",
            Self::DnsCache         => "aeon/dns/v1",
            Self::RelayAnnounce    => "aeon/relay/v1",
            Self::WordFrequency    => "aeon/word/v1",
            Self::DeviceRegistry   => "aeon/cast/v1",
            Self::TranslateOffload => "aeon/translate/v1",
            Self::PeerScore        => "aeon/score/v1",
            Self::SovereignCommand => "aeon/sovereign/v1",
            Self::VoteCoordination => "aeon/vote/v1",
        }
    }

    /// Whether messages on this topic require sovereign key signature
    pub fn requires_sovereign_sig(&self) -> bool {
        matches!(self, Self::BridgeConfig | Self::SovereignCommand | Self::UpdateManifest)
    }
}

/// Ed25519 signature wrapper — hex-encoded for serde compatibility.
/// Raw [u8; 64] doesn't implement Serialize/Deserialize for arrays > 32.
#[derive(Debug, Clone)]
pub struct Sig64(pub [u8; 64]);

impl Sig64 {
    pub fn from_bytes(bytes: [u8; 64]) -> Self {
        Self(bytes)
    }

    pub fn as_bytes(&self) -> &[u8; 64] {
        &self.0
    }
}

impl Serialize for Sig64 {
    fn serialize<S: serde::Serializer>(&self, serializer: S) -> Result<S::Ok, S::Error> {
        serializer.serialize_str(&hex::encode(self.0))
    }
}

impl<'de> Deserialize<'de> for Sig64 {
    fn deserialize<D: serde::Deserializer<'de>>(deserializer: D) -> Result<Self, D::Error> {
        let s = String::deserialize(deserializer)?;
        let bytes = hex::decode(&s).map_err(serde::de::Error::custom)?;
        if bytes.len() != 64 {
            return Err(serde::de::Error::custom(
                format!("expected 64 bytes, got {}", bytes.len())
            ));
        }
        let mut arr = [0u8; 64];
        arr.copy_from_slice(&bytes);
        Ok(Sig64(arr))
    }
}

/// A signed message on a topic.
#[derive(Debug, Clone, Serialize, Deserialize)]
pub struct HiveMessage {
    /// Which topic this message belongs to
    pub topic: HiveTopic,
    /// Sender's Ed25519 public key (32 bytes, hex-encoded)
    pub sender: String,
    /// UTC timestamp (seconds since epoch)
    pub timestamp: u64,
    /// Actual payload (topic-specific, usually JSON or postcard bytes)
    pub payload: Vec<u8>,
    /// Ed25519 signature by sender over (topic || timestamp || payload)
    pub signature: Sig64,
    /// Optional sovereign countersignature (for BridgeConfig, UpdateManifest)
    pub sovereign_sig: Option<Sig64>,
    /// Message sequence number (monotonic per sender, prevents replay)
    pub seq: u64,
}

impl HiveMessage {
    /// Compute the bytes that get signed: topic_id || timestamp || seq || payload
    pub fn signable_bytes(&self) -> Vec<u8> {
        let mut buf = Vec::with_capacity(12 + self.payload.len());
        buf.extend_from_slice(&(self.topic as u32).to_le_bytes());
        buf.extend_from_slice(&self.timestamp.to_le_bytes());
        buf.extend_from_slice(&self.seq.to_le_bytes());
        buf.extend_from_slice(&self.payload);
        buf
    }
}
