// =============================================================================
// identity.rs — Persistent Ed25519 identity for each AeonHive node
//
// First install: generates a keypair, saves to %APPDATA%\Aeon\hive_identity.key
// Every subsequent launch: loads the same identity.
//
// The public key IS the peer ID. No separate ID scheme needed.
// This is the same approach as iroh's native NodeId system.
// =============================================================================

use std::path::{Path, PathBuf};
use std::fs;

use ed25519_dalek::{SigningKey, VerifyingKey, Signer, Verifier, Signature};
use serde::{Deserialize, Serialize};

use crate::error::{HiveError, HiveResult};

/// Persistent identity for this AeonHive node.
/// Wraps an Ed25519 signing key with load/save capabilities.
#[derive(Clone)]
pub struct HiveIdentity {
    signing_key: SigningKey,
    data_dir: PathBuf,
}

/// Public peer identity — can be shared freely.
#[derive(Debug, Clone, Serialize, Deserialize, PartialEq, Eq, Hash)]
pub struct PeerId(pub [u8; 32]);

impl std::fmt::Display for PeerId {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        write!(f, "{}", hex::encode(&self.0[..8])) // short display
    }
}

impl PeerId {
    /// Full hex string of the peer ID (64 chars)
    pub fn to_hex(&self) -> String {
        hex::encode(self.0)
    }

    /// Parse from hex string
    pub fn from_hex(s: &str) -> HiveResult<Self> {
        let bytes = hex::decode(s).map_err(|e| HiveError::Identity(e.to_string()))?;
        if bytes.len() != 32 {
            return Err(HiveError::Identity("PeerId must be 32 bytes".into()));
        }
        let mut arr = [0u8; 32];
        arr.copy_from_slice(&bytes);
        Ok(PeerId(arr))
    }
}

impl HiveIdentity {
    /// Load existing identity from disk, or generate a new one.
    pub fn load_or_create(data_dir: &Path) -> HiveResult<Self> {
        let key_path = data_dir.join("hive_identity.key");

        if key_path.exists() {
            tracing::info!("Loading existing identity from {}", key_path.display());
            let bytes = fs::read(&key_path)
                .map_err(|e| HiveError::Identity(format!("Failed to read key: {e}")))?;

            if bytes.len() != 32 {
                return Err(HiveError::Identity(
                    format!("Key file corrupt: expected 32 bytes, got {}", bytes.len())
                ));
            }

            let mut seed = [0u8; 32];
            seed.copy_from_slice(&bytes);
            let signing_key = SigningKey::from_bytes(&seed);

            Ok(Self { signing_key, data_dir: data_dir.to_path_buf() })
        } else {
            tracing::info!("Generating new AeonHive identity");
            let signing_key = SigningKey::generate(&mut rand::rng());

            // Ensure data directory exists
            fs::create_dir_all(data_dir)
                .map_err(|e| HiveError::Identity(format!("Failed to create dir: {e}")))?;

            // Save the secret key bytes (32 bytes)
            fs::write(&key_path, signing_key.to_bytes())
                .map_err(|e| HiveError::Identity(format!("Failed to write key: {e}")))?;

            tracing::info!("Identity saved to {}", key_path.display());

            Ok(Self { signing_key, data_dir: data_dir.to_path_buf() })
        }
    }

    /// Get this node's PeerId (Ed25519 public key)
    pub fn peer_id(&self) -> PeerId {
        PeerId(self.verifying_key().to_bytes())
    }

    /// Get the Ed25519 verifying (public) key
    pub fn verifying_key(&self) -> VerifyingKey {
        self.signing_key.verifying_key()
    }

    /// Sign arbitrary data with this node's identity
    pub fn sign(&self, data: &[u8]) -> [u8; 64] {
        self.signing_key.sign(data).to_bytes()
    }

    /// Verify a signature from any peer
    pub fn verify(peer_id: &PeerId, data: &[u8], signature: &[u8; 64]) -> HiveResult<()> {
        let vk = VerifyingKey::from_bytes(&peer_id.0)
            .map_err(|_| HiveError::InvalidSignature)?;
        let sig = Signature::from_bytes(signature);
        vk.verify(data, &sig)
            .map_err(|_| HiveError::InvalidSignature)
    }

    /// Verify a signature specifically from the sovereign key.
    /// The sovereign public key is embedded at compile time.
    pub fn verify_sovereign(data: &[u8], signature: &[u8; 64]) -> HiveResult<()> {
        let sovereign_hex = option_env!("AEON_SOVEREIGN_PUBKEY")
            .unwrap_or("0000000000000000000000000000000000000000000000000000000000000000");

        let sovereign_bytes = hex::decode(sovereign_hex)
            .map_err(|e| HiveError::Identity(format!("Bad sovereign key: {e}")))?;

        let mut key = [0u8; 32];
        key.copy_from_slice(&sovereign_bytes);

        let vk = VerifyingKey::from_bytes(&key)
            .map_err(|_| HiveError::InvalidSignature)?;
        let sig = Signature::from_bytes(signature);

        vk.verify(data, &sig)
            .map_err(|_| HiveError::InvalidSignature)
    }

    /// Get the data directory path
    pub fn data_dir(&self) -> &Path {
        &self.data_dir
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use tempfile::tempdir;

    #[test]
    fn test_identity_persistence() {
        let dir = tempdir().unwrap();

        // Create new identity
        let id1 = HiveIdentity::load_or_create(dir.path()).unwrap();
        let pid1 = id1.peer_id();

        // Reload — should be the same
        let id2 = HiveIdentity::load_or_create(dir.path()).unwrap();
        let pid2 = id2.peer_id();

        assert_eq!(pid1, pid2, "Identity must persist across loads");
    }

    #[test]
    fn test_sign_verify() {
        let dir = tempdir().unwrap();
        let id = HiveIdentity::load_or_create(dir.path()).unwrap();

        let data = b"AeonHive lives";
        let sig = id.sign(data);

        // Verify with correct key
        assert!(HiveIdentity::verify(&id.peer_id(), data, &sig).is_ok());

        // Verify with wrong data
        assert!(HiveIdentity::verify(&id.peer_id(), b"tampered", &sig).is_err());
    }

    #[test]
    fn test_peer_id_hex_roundtrip() {
        let dir = tempdir().unwrap();
        let id = HiveIdentity::load_or_create(dir.path()).unwrap();
        let pid = id.peer_id();

        let hex_str = pid.to_hex();
        let pid2 = PeerId::from_hex(&hex_str).unwrap();
        assert_eq!(pid, pid2);
    }
}
