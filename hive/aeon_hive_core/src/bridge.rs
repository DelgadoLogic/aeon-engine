// =============================================================================
// bridge.rs — Sovereign-signed bridge config distribution
//
// Bridge configs are the most critical P2P data in AeonShield.
// They tell censored users HOW to connect (VLESS+REALITY, ws_tunnel, etc.)
//
// Distribution: Sovereign key signs → publish to gossip → all peers receive.
// Self-sustainable at ~1K users (tiny data, gossip propagates instantly).
// =============================================================================

use std::collections::HashMap;
use parking_lot::RwLock;
use tracing;

use crate::identity::HiveIdentity;
use crate::protocols::BridgeConfig;
use crate::error::HiveResult;

/// Bridge registry — stores verified bridge configs
pub struct BridgeRegistry {
    /// Active bridges: bridge_id -> config
    bridges: RwLock<HashMap<String, BridgeConfig>>,
    /// Country-optimized index: country_code -> list of bridge_ids
    country_index: RwLock<HashMap<String, Vec<String>>>,
}

impl BridgeRegistry {
    pub fn new() -> Self {
        Self {
            bridges: RwLock::new(HashMap::new()),
            country_index: RwLock::new(HashMap::new()),
        }
    }

    /// Insert a bridge config that has been verified (sovereign sig checked).
    /// Returns true if this is new or updated, false if duplicate.
    pub fn insert_verified(&self, config: BridgeConfig) -> bool {
        let now = std::time::SystemTime::now()
            .duration_since(std::time::UNIX_EPOCH)
            .unwrap_or_default()
            .as_secs();

        // Reject expired bridges
        if config.expires_at > 0 && now > config.expires_at {
            tracing::debug!(bridge_id = %config.bridge_id, "Rejected expired bridge");
            return false;
        }

        let bridge_id = config.bridge_id.clone();
        let countries = config.target_countries.clone();

        let mut bridges = self.bridges.write();

        // Check if we already have a newer version
        if let Some(existing) = bridges.get(&bridge_id) {
            if existing.created_at >= config.created_at {
                return false;
            }
        }

        tracing::info!(
            bridge_id = %bridge_id,
            protocol = %config.protocol,
            countries = ?countries,
            "Bridge config registered"
        );

        bridges.insert(bridge_id.clone(), config);
        drop(bridges);

        // Update country index
        let mut idx = self.country_index.write();
        for country in &countries {
            let entry = idx.entry(country.clone()).or_default();
            if !entry.contains(&bridge_id) {
                entry.push(bridge_id.clone());
            }
        }

        true
    }

    /// Get the best bridges for a specific country, ordered by health.
    pub fn get_for_country(&self, country: &str) -> Vec<BridgeConfig> {
        let idx = self.country_index.read();
        let bridge_ids = match idx.get(country) {
            Some(ids) => ids.clone(),
            None => return Vec::new(),
        };
        drop(idx);

        let now = std::time::SystemTime::now()
            .duration_since(std::time::UNIX_EPOCH)
            .unwrap_or_default()
            .as_secs();

        let bridges = self.bridges.read();
        let mut result: Vec<_> = bridge_ids.iter()
            .filter_map(|id| bridges.get(id))
            .filter(|b| b.expires_at == 0 || now < b.expires_at) // Not expired
            .cloned()
            .collect();

        // Sort by health descending (healthiest first)
        result.sort_by(|a, b| b.health.partial_cmp(&a.health).unwrap_or(std::cmp::Ordering::Equal));
        result
    }

    /// Get all active bridge configs (for sharing with a new peer).
    pub fn all_active(&self) -> Vec<BridgeConfig> {
        let now = std::time::SystemTime::now()
            .duration_since(std::time::UNIX_EPOCH)
            .unwrap_or_default()
            .as_secs();

        let bridges = self.bridges.read();
        bridges.values()
            .filter(|b| b.expires_at == 0 || now < b.expires_at)
            .cloned()
            .collect()
    }

    /// Update bridge health based on intelligence reports.
    pub fn update_health(&self, bridge_id: &str, success: bool) {
        let mut bridges = self.bridges.write();
        if let Some(bridge) = bridges.get_mut(bridge_id) {
            if success {
                bridge.health = (bridge.health + 0.01).min(1.0);
            } else {
                bridge.health = (bridge.health - 0.05).max(0.0);
            }
        }
    }

    /// Remove a bridge (sovereign revocation).
    pub fn remove(&self, bridge_id: &str) {
        let mut bridges = self.bridges.write();
        bridges.remove(bridge_id);

        let mut idx = self.country_index.write();
        for ids in idx.values_mut() {
            ids.retain(|id| id != bridge_id);
        }

        tracing::warn!(bridge_id = %bridge_id, "Bridge REVOKED");
    }

    /// Verify a bridge config's sovereign signature before inserting.
    pub fn verify_and_insert(
        &self,
        config: BridgeConfig,
        payload_bytes: &[u8],
        signature: &[u8; 64],
    ) -> HiveResult<bool> {
        HiveIdentity::verify_sovereign(payload_bytes, signature)?;
        Ok(self.insert_verified(config))
    }

    /// Bridge count
    pub fn count(&self) -> usize {
        self.bridges.read().len()
    }
}
