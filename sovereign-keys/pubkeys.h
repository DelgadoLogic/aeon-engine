// =============================================================================
// pubkeys.h — Aeon Sovereign Signing Public Keys (AUTO-GENERATED)
// Generated: 2026-04-12 21:18:58 UTC
// Tool: sovereign-keys/aeon_keygen.py
//
// This file contains ONLY public keys. It is safe to commit to version control.
// The corresponding private keys (.key files) must NEVER be committed.
// =============================================================================

#pragma once

#include <cstdint>

// ── Trust Store Bundle Signing ──────────────────────────────────
// Used by: AeonTLS::ApplyTrustBundle(), AeonTLSImpl::VerifyBundleSignature()
// SHA-256: 1290794ab733c5e3324a2ff30016ec19771cd33fb466361523886f5c5e6dc5cb
static const uint8_t AEON_TRUST_SIGNER_PUBKEY[32] = {
    0x3d, 0x2a, 0x5e, 0x4f, 0xea, 0xbe, 0xa6, 0x69,
    0x72, 0x16, 0xb0, 0xc0, 0x04, 0x4c, 0xe4, 0xe2,
    0x74, 0x08, 0xac, 0x77, 0x43, 0x24, 0x7b, 0x3b,
    0xb3, 0xbd, 0xe1, 0xcb, 0xc9, 0x74, 0x37, 0xe5,
};

// ── P2P Update Manifest Signing ─────────────────────────────────
// Used by: AeonP2PUpdateImpl::VerifyManifest()
// SHA-256: 17514704c1f61cabb83c8c720df3dd0f0b598283135cd86797ae7e07709a2a0f
static const uint8_t AEON_UPDATE_SIGNER_PUBKEY[32] = {
    0x19, 0x07, 0xf3, 0x34, 0x3a, 0xab, 0xbb, 0xae,
    0xa4, 0xb2, 0xb3, 0x14, 0x8d, 0x36, 0x86, 0x94,
    0x60, 0x58, 0xc3, 0x96, 0x65, 0x0c, 0xcf, 0x3f,
    0x6b, 0x1d, 0xc7, 0xb0, 0xbb, 0x19, 0x53, 0xfa,
};

// ── Sovereign Manifest Signing ──────────────────────────────────
// Used by: AeonHive::BroadcastManifest()
// SHA-256: 843aef5e8040163028809be384b8dea93d12ed4f39de8fca462deaa366b3acf0
static const uint8_t AEON_MANIFEST_SIGNER_PUBKEY[32] = {
    0x24, 0x4c, 0xe1, 0x50, 0x36, 0x1f, 0x09, 0xdd,
    0xa4, 0x55, 0x74, 0x66, 0x56, 0xc0, 0xd4, 0xf9,
    0xf6, 0x48, 0xfb, 0x71, 0x61, 0x04, 0x28, 0x5d,
    0xa5, 0x39, 0xdb, 0x56, 0xc5, 0x6a, 0x80, 0x62,
};
