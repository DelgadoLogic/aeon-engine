// =============================================================================
// aeon_tls.h — AeonTLS: Sovereign Trust Store & TLS Engine
// Graduated from: BearSSL (MIT) + Mozilla CA Program (MPL 2.0)
//
// AeonTLS is Aeon's built-in cryptographic layer, completely independent of
// the host OS certificate store. This solves two critical problems:
//
//   1. "Root Expiry" trap: Legacy Android (<7.1) and Windows (<7 SP1) have
//      outdated root CAs that fail on modern HTTPS. AeonTLS carries its own.
//
//   2. TLS 1.3 mandate: Windows 7/8 don't support TLS 1.3 natively.
//      AeonTLS wraps BearSSL to provide TLS 1.3 on any Windows version.
//
// Trust store updates are delivered sovereignly via AeonHive mesh:
//   - Ed25519-signed CA bundles broadcast on HiveTopic::TrustStoreUpdate
//   - Signature verification before any trust anchor changes
//   - Rollback protection via monotonic version counter
//   - Zero dependency on OS update mechanisms
//
// What we improve over upstream:
//   [+] Built-in Mozilla CA bundle (no OS dependency)
//   [+] Sovereign trust store updater via P2P mesh (no HTTP fetch needed)
//   [+] Ed25519 signature verification for all bundle updates
//   [+] Certificate lifetime tracking (200-day rotation awareness)
//   [+] Automatic fallback: AeonTLS -> OS store -> hardcoded emergency CAs
//   [+] Compiled trust anchors for zero-parse startup on constrained devices
//   [+] Win95-safe: uses WinSock 1.1 via bearssl_bridge when needed
// =============================================================================

#pragma once

#include "aeon_component.h"
#include <cstdint>
#include <cstddef>
#include <functional>
#include <string>
#include <vector>

// Forward declarations
class AeonTLSImpl;
struct br_x509_trust_anchor;

// ---------------------------------------------------------------------------
// AeonCertInfo — metadata about a trust anchor
// ---------------------------------------------------------------------------
struct AeonCertInfo {
    char     cn[128];               // Common Name
    char     org[128];              // Organization
    uint8_t  sha256_fingerprint[32];// SHA-256 of DER-encoded certificate
    uint64_t not_before_utc;        // Validity start (Unix timestamp)
    uint64_t not_after_utc;         // Validity end (Unix timestamp)
    uint32_t key_type;              // BR_KEYTYPE_RSA or BR_KEYTYPE_EC
    uint16_t key_bits;              // RSA bit length or EC curve size
    bool     is_active;             // Currently trusted
    bool     is_expired;            // Past not_after
    bool     is_revoked;            // Explicitly revoked via update
};

// ---------------------------------------------------------------------------
// AeonTrustBundle — a signed collection of trust anchors
// ---------------------------------------------------------------------------
struct AeonTrustBundle {
    uint32_t version;               // Monotonic version counter (rollback protection)
    uint64_t timestamp_utc;         // When this bundle was created
    uint32_t anchor_count;          // Number of trust anchors in this bundle
    uint8_t  signer_pubkey[32];     // Ed25519 public key of the bundle signer
    uint8_t  signature[64];         // Ed25519 signature over the bundle payload
    const uint8_t* payload;         // Serialized trust anchors
    size_t   payload_len;
};

// ---------------------------------------------------------------------------
// AeonTLSConfig — startup configuration
// ---------------------------------------------------------------------------
struct AeonTLSConfig {
    bool     use_builtin_store;     // true = use AeonTLS built-in CAs (default: true)
    bool     use_os_store;          // true = also consult OS cert store (default: false)
    bool     auto_update;           // true = accept trust store updates via AeonHive
    bool     enforce_tls13;         // true = require TLS 1.3 minimum (default: false)
    uint16_t min_tls_version;       // Minimum TLS version: 0x0301=1.0, 0x0303=1.2, 0x0304=1.3
    uint32_t cert_expiry_warn_days; // Warn when root cert expires within N days (default: 90)

    // Sovereign trust store signing key (Ed25519 public key)
    // Only bundles signed by this key (or its successors) are accepted.
    uint8_t  trusted_signer[32];

    // Emergency fallback CAs (hardcoded, never removed)
    // Used if both built-in and OS stores fail validation
    bool     enable_emergency_cas;  // default: true
};

// ---------------------------------------------------------------------------
// Certificate validation result
// ---------------------------------------------------------------------------
enum class AeonCertStatus : uint32_t {
    Valid           = 0x0000,   // Chain validates against a trusted anchor
    Expired         = 0x0001,   // Certificate has expired
    NotYetValid     = 0x0002,   // Certificate not yet valid
    Revoked         = 0x0003,   // Certificate explicitly revoked
    UntrustedRoot   = 0x0004,   // Root CA not in any trust store
    ChainError      = 0x0005,   // Chain construction failed
    SignatureError  = 0x0006,   // Signature verification failed
    NameMismatch    = 0x0007,   // CN/SAN doesn't match requested hostname
    WeakCrypto      = 0x0008,   // Key too weak (RSA <2048, SHA-1 leaf)
    CTMissing       = 0x0009,   // Certificate Transparency log missing
    EmergencyCA     = 0x000A,   // Validated but only via emergency CA fallback
};

// ---------------------------------------------------------------------------
// Callbacks
// ---------------------------------------------------------------------------
using AeonTLSTrustUpdateCallback = std::function<void(
    const AeonTrustBundle& bundle, bool accepted, const char* reason)>;

using AeonTLSCertWarningCallback = std::function<void(
    const AeonCertInfo& cert, uint32_t days_until_expiry)>;

// ---------------------------------------------------------------------------
// AeonTLS — sovereign TLS engine and trust store manager
// ---------------------------------------------------------------------------
class AeonTLS final : public AeonComponentBase {
public:
    AeonTLS();
    ~AeonTLS() override;

    // ── IAeonComponent identity ───────────────────────────────────────────
    const char* ComponentId()      const override { return "aeon.tls"; }
    const char* ComponentVersion() const override { return "1.0.0"; }
    const char* UpstreamRef()      const override {
        return "BearSSL@MIT + Mozilla-CA-Program@MPL2";
    }

    // ── Lifecycle ─────────────────────────────────────────────────────────
    bool Initialize(const ResourceBudget& budget) override;
    void Shutdown() override;

    // ── Configuration ─────────────────────────────────────────────────────
    bool Configure(const AeonTLSConfig& config);

    // ── Trust Store Management ────────────────────────────────────────────

    // Get the current trust store version and anchor count
    uint32_t TrustStoreVersion() const;
    uint32_t TrustAnchorCount() const;

    // Enumerate all trust anchors
    std::vector<AeonCertInfo> ListTrustAnchors() const;

    // Get BearSSL-compatible trust anchor array (for direct BearSSL usage)
    const br_x509_trust_anchor* GetBearSSLAnchors(size_t* out_count) const;

    // Check if a specific root CA is trusted (by SHA-256 fingerprint)
    bool IsTrustedRoot(const uint8_t sha256[32]) const;

    // ── Sovereign Trust Updates (via AeonHive) ────────────────────────────

    // Apply a signed trust bundle received from the mesh network.
    // Returns true if the bundle was accepted and applied.
    // Rejects if: wrong signer, version <= current, invalid signature.
    bool ApplyTrustBundle(const AeonTrustBundle& bundle);

    // Register callback for trust store update events
    void OnTrustUpdate(AeonTLSTrustUpdateCallback callback);

    // Register callback for cert expiry warnings
    void OnCertWarning(AeonTLSCertWarningCallback callback);

    // Force refresh of trust store from AeonHive mesh
    // (broadcasts a request on HiveTopic::TrustStoreRequest)
    void RequestTrustUpdate();

    // ── Certificate Validation ────────────────────────────────────────────

    // Validate a server certificate chain against the trust store.
    // chain/chain_lens are DER-encoded certificates (leaf first, root last).
    AeonCertStatus ValidateChain(
        const char* hostname,
        const uint8_t** chain,
        const size_t* chain_lens,
        size_t chain_count
    ) const;

    // Quick check: is this hostname's cert about to expire?
    // Returns days until expiry, or -1 if not cached.
    int32_t CertExpiryDays(const char* hostname) const;

    // ── TLS Session Creation ──────────────────────────────────────────────

    // Create a TLS client context for a connection.
    // Returns an opaque handle, or nullptr on failure.
    // The handle wraps a BearSSL client context configured with AeonTLS
    // trust anchors and the negotiated TLS version.
    void* CreateTLSContext(const char* hostname);

    // Destroy a TLS context created by CreateTLSContext
    void DestroyTLSContext(void* ctx);

    // Get the negotiated TLS version for a context (e.g., 0x0303 = TLS 1.2)
    uint16_t NegotiatedVersion(void* ctx) const;

    // ── Certificate Lifetime Tracking ─────────────────────────────────────

    // Scan all anchors and report those expiring within warn_days.
    // Critical for the October 2026 200-day cert rotation deadline.
    std::vector<AeonCertInfo> ExpiringAnchors(uint32_t warn_days = 90) const;

    // Get the earliest expiry date among all active root CAs (Unix timestamp)
    uint64_t EarliestRootExpiry() const;

    // ── Diagnostics ───────────────────────────────────────────────────────

    // Human-readable trust store status for debug/about page
    std::string DiagnosticReport() const;

    // ── Resource Awareness ────────────────────────────────────────────────
    bool CanOffloadToHive() const override { return false; } // Crypto runs local

private:
    AeonTLSImpl* m_impl = nullptr;
};

// ── Global singleton ──────────────────────────────────────────────────────────
AeonTLS& AeonTLSInstance();

// ── Sovereign Trust Store Signing ─────────────────────────────────────────────
// The official DelgadoLogic trust store signing public key.
// Only bundles signed by this key are accepted by production builds.
// Dev/test builds may use AEON_TLS_ALLOW_ANY_SIGNER=1 to bypass.
// Generated: 2026-04-12 by sovereign-keys/aeon_keygen.py
// SHA-256: 1290794ab733c5e3324a2ff30016ec19771cd33fb466361523886f5c5e6dc5cb
static const uint8_t AEON_TRUST_SIGNER_PUBKEY[32] = {
    0x3d, 0x2a, 0x5e, 0x4f, 0xea, 0xbe, 0xa6, 0x69,
    0x72, 0x16, 0xb0, 0xc0, 0x04, 0x4c, 0xe4, 0xe2,
    0x74, 0x08, 0xac, 0x77, 0x43, 0x24, 0x7b, 0x3b,
    0xb3, 0xbd, 0xe1, 0xcb, 0xc9, 0x74, 0x37, 0xe5,
};

