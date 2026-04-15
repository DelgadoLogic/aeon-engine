# Aeon Sovereign Key Inventory

**Generated:** 2026-04-12 21:18:58 UTC  
**Generator:** `sovereign-keys/aeon_keygen.py`  

> [!CAUTION]
> Private keys (`.key` files) must NEVER be committed to version control.
> Store them in a secure offline location. Loss of private keys means
> inability to sign trust bundles or update manifests.

## Keys

| Purpose | Constant | Public Key SHA-256 | Consumers |
|---------|----------|--------------------|-----------|
| Trust Store Bundle Signing | `AEON_TRUST_SIGNER_PUBKEY` | `1290794ab733c5e3324a2ff30016ec19...` | `AeonTLS::ApplyTrustBundle()`, `AeonTLSImpl::VerifyBundleSignature()` |
| P2P Update Manifest Signing | `AEON_UPDATE_SIGNER_PUBKEY` | `17514704c1f61cabb83c8c720df3dd0f...` | `AeonP2PUpdateImpl::VerifyManifest()` |
| Sovereign Manifest Signing | `AEON_MANIFEST_SIGNER_PUBKEY` | `843aef5e8040163028809be384b8dea9...` | `AeonHive::BroadcastManifest()` |

## File Layout

```
sovereign-keys/
├── aeon_keygen.py           # This generator script
├── pubkeys.h                # C header (safe to commit)
├── KEY_INVENTORY.md         # This file (safe to commit)
├── .gitignore               # Excludes .key files
├── aeon_trust_signing.key   # 🔒 SECRET - trust store signing
├── aeon_trust_signing.pub   # Public key
├── aeon_update_signing.key  # 🔒 SECRET - P2P update signing
├── aeon_update_signing.pub  # Public key
├── aeon_manifest_signing.key # 🔒 SECRET - manifest signing
└── aeon_manifest_signing.pub # Public key
```

## Integration

After generating keys, update the following files:

### Trust Store Bundle Signing
- Replace `AEON_TRUST_SIGNER_PUBKEY` in `sovereign/aeon_tls.h`
  with the values from `pubkeys.h`

### P2P Update Manifest Signing
- Replace `AEON_UPDATE_SIGNER_PUBKEY` in `updater/aeon_p2p_update.h`
  with the values from `pubkeys.h`

### Sovereign Manifest Signing
- Replace `AEON_MANIFEST_SIGNER_PUBKEY` in `hive/aeon_hive.h`
  with the values from `pubkeys.h`

## Rotation Policy

- **Trust Store Keys:** Rotate annually. Old key must co-sign new key's first bundle.
- **Update Signing Keys:** Rotate every 6 months. Include transition period.
- **Manifest Keys:** Rotate every 6 months. Old manifests remain valid until expiry.

## Emergency Key Revocation

If a private key is compromised:
1. Generate new keypair immediately
2. Sign a revocation notice with the old key (if still available)
3. Broadcast revocation via AeonHive `SovereignManifest` topic
4. Push emergency update with new public key embedded
5. Update `pubkeys.h` and all consuming headers
