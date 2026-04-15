// =============================================================================
// dns_cache.rs — Distributed DNS cache sharing via DHT
//
// When a peer resolves a domain via DoH, it publishes the result to the mesh.
// Other peers query the mesh BEFORE hitting cloud DNS.
//
// This is AeonShield Step 2: DNS resolution graduates from cloud to P2P.
// At 10K+ users, enough cached records exist that cloud DNS is optional.
// =============================================================================

use std::collections::HashMap;
use parking_lot::RwLock;
use tracing;

use crate::protocols::DnsCacheEntry;

/// Maximum entries in the local DNS cache
const MAX_CACHE_SIZE: usize = 10_000;

/// Thread-safe distributed DNS cache
pub struct DnsCache {
    /// Local cache: domain -> entry
    entries: RwLock<HashMap<String, DnsCacheEntry>>,
    /// Domains we've shared to the mesh (to avoid re-publishing)
    shared: RwLock<HashMap<String, u64>>, // domain -> last_shared_timestamp
}

impl DnsCache {
    pub fn new() -> Self {
        Self {
            entries: RwLock::new(HashMap::with_capacity(1024)),
            shared: RwLock::new(HashMap::new()),
        }
    }

    /// Lookup a domain in the local cache.
    /// Returns None if not found or expired.
    pub fn lookup(&self, domain: &str) -> Option<DnsCacheEntry> {
        let cache = self.entries.read();
        cache.get(domain).and_then(|entry| {
            if entry.is_valid() {
                Some(entry.clone())
            } else {
                None
            }
        })
    }

    /// Insert a DNS resolution result into the local cache.
    /// Called after a successful DoH lookup.
    pub fn insert(&self, entry: DnsCacheEntry) {
        let mut cache = self.entries.write();

        // Evict oldest if at capacity
        if cache.len() >= MAX_CACHE_SIZE {
            // Find and remove the oldest entry
            if let Some(oldest_domain) = cache.iter()
                .min_by_key(|(_, e)| e.resolved_at)
                .map(|(d, _)| d.clone())
            {
                cache.remove(&oldest_domain);
            }
        }

        tracing::debug!(domain = %entry.domain, addrs = ?entry.addresses, "DNS cache insert");
        cache.insert(entry.domain.clone(), entry);
    }

    /// Mark a domain as shared to the mesh.
    /// Returns true if it should be shared (hasn't been shared recently).
    pub fn should_share(&self, domain: &str) -> bool {
        let now = std::time::SystemTime::now()
            .duration_since(std::time::UNIX_EPOCH)
            .unwrap_or_default()
            .as_secs();

        let shared = self.shared.read();
        match shared.get(domain) {
            Some(&last) => now - last > 300, // Re-share every 5 minutes max
            None => true,
        }
    }

    /// Record that a domain was shared to the mesh.
    pub fn mark_shared(&self, domain: &str) {
        let now = std::time::SystemTime::now()
            .duration_since(std::time::UNIX_EPOCH)
            .unwrap_or_default()
            .as_secs();

        let mut shared = self.shared.write();
        shared.insert(domain.to_string(), now);
    }

    /// Insert an entry received from a peer (via gossip/DHT).
    /// Only accepted if we don't already have a fresher entry.
    pub fn insert_from_peer(&self, entry: DnsCacheEntry) -> bool {
        if !entry.is_valid() {
            return false;
        }

        let mut cache = self.entries.write();

        // Only accept if we don't have a newer entry
        if let Some(existing) = cache.get(&entry.domain) {
            if existing.resolved_at >= entry.resolved_at && existing.is_valid() {
                return false; // We have a fresher copy
            }
        }

        // Evict if at capacity
        if cache.len() >= MAX_CACHE_SIZE {
            if let Some(oldest_domain) = cache.iter()
                .min_by_key(|(_, e)| e.resolved_at)
                .map(|(d, _)| d.clone())
            {
                cache.remove(&oldest_domain);
            }
        }

        tracing::debug!(
            domain = %entry.domain,
            addrs = ?entry.addresses,
            "DNS cache insert from peer"
        );
        cache.insert(entry.domain.clone(), entry);
        true
    }

    /// Get all valid entries (for bulk sharing with a newly connected peer).
    pub fn all_valid(&self) -> Vec<DnsCacheEntry> {
        let cache = self.entries.read();
        cache.values()
            .filter(|e| e.is_valid())
            .cloned()
            .collect()
    }

    /// Remove expired entries.
    pub fn evict_expired(&self) -> usize {
        let mut cache = self.entries.write();
        let before = cache.len();
        cache.retain(|_, e| e.is_valid());
        before - cache.len()
    }

    /// Cache size
    pub fn len(&self) -> usize {
        self.entries.read().len()
    }

    pub fn is_empty(&self) -> bool {
        self.entries.read().is_empty()
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    fn make_entry(domain: &str, ttl: u32) -> DnsCacheEntry {
        let now = std::time::SystemTime::now()
            .duration_since(std::time::UNIX_EPOCH)
            .unwrap()
            .as_secs();

        DnsCacheEntry {
            domain: domain.to_string(),
            addresses: vec!["93.184.216.34".to_string()],
            record_type: "A".to_string(),
            ttl,
            resolved_at: now,
            resolver: "cloudflare-doh".to_string(),
        }
    }

    #[test]
    fn test_insert_and_lookup() {
        let cache = DnsCache::new();
        let entry = make_entry("youtube.com", 300);
        cache.insert(entry);
        assert!(cache.lookup("youtube.com").is_some());
        assert!(cache.lookup("google.com").is_none());
    }

    #[test]
    fn test_expired_entry() {
        let cache = DnsCache::new();
        let mut entry = make_entry("expired.com", 1);
        // Make it already expired
        entry.resolved_at = entry.resolved_at.saturating_sub(10);
        entry.ttl = 1;
        cache.insert(entry);
        // Should not return expired entries
        assert!(cache.lookup("expired.com").is_none());
    }

    #[test]
    fn test_peer_insert_fresher() {
        let cache = DnsCache::new();
        let entry1 = make_entry("test.com", 300);
        cache.insert(entry1);

        let now = std::time::SystemTime::now()
            .duration_since(std::time::UNIX_EPOCH)
            .unwrap()
            .as_secs();

        let entry2 = DnsCacheEntry {
            domain: "test.com".to_string(),
            addresses: vec!["1.2.3.4".to_string()],
            record_type: "A".to_string(),
            ttl: 300,
            resolved_at: now + 10, // Fresher
            resolver: "peer".to_string(),
        };

        assert!(cache.insert_from_peer(entry2));
        let result = cache.lookup("test.com").unwrap();
        assert_eq!(result.addresses[0], "1.2.3.4");
    }
}
