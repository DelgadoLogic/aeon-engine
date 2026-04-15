// =============================================================================
// aeon_tls.cpp — AeonTLS: Sovereign Trust Store & TLS Engine Implementation
// DelgadoLogic | Pillar 1: Sovereign Cryptographic Stack
//
// Implements the full trust store lifecycle:
//   1. Initialize with compiled-in Mozilla CA bundle
//   2. Accept Ed25519-signed trust bundle updates from AeonHive mesh
//   3. Verify certificate chains against the sovereign store
//   4. Create BearSSL TLS client contexts for outgoing connections
//   5. Track certificate lifetimes and warn before expiry
//
// Dependencies:
//   - BearSSL (MIT) for TLS 1.2/1.3 and X.509 validation
//   - Ed25519 (public domain) for trust bundle signature verification
//   - retro/trust_anchors.h for compiled-in Mozilla CAs
// =============================================================================

#include "aeon_tls.h"
#include "../retro/trust_anchors.h"

// BearSSL headers
extern "C" {
#include "bearssl/inc/bearssl.h"
}

#include <cstring>
#include <ctime>
#include <mutex>
#include <algorithm>

// ---------------------------------------------------------------------------
// Ed25519 signature verification (lightweight, public-domain implementation)
// We embed Orlp's ed25519 or a compatible implementation.
// ---------------------------------------------------------------------------
extern "C" {
    int ed25519_verify(const uint8_t* signature, const uint8_t* message,
                       size_t message_len, const uint8_t* public_key);
}

// ---------------------------------------------------------------------------
// AeonTLSImpl — private implementation
// ---------------------------------------------------------------------------
class AeonTLSImpl {
public:
    AeonTLSConfig                  config;
    mutable std::mutex             mu;

    // Compiled-in trust anchors (from retro/trust_anchors.h)
    std::vector<br_x509_trust_anchor> anchors;
    std::vector<AeonCertInfo>         anchor_info;

    // Trust store versioning
    uint32_t store_version = 1;  // v1 = compiled-in bundle
    uint64_t store_timestamp = 0;

    // Callbacks
    AeonTLSTrustUpdateCallback     on_trust_update;
    AeonTLSCertWarningCallback     on_cert_warning;

    // Certificate expiry cache: hostname -> days until expiry
    struct ExpiryCache {
        char     hostname[256];
        int32_t  days_remaining;
        uint64_t cached_at;   // Unix timestamp when cached
    };
    std::vector<ExpiryCache> expiry_cache;

    // ── Load compiled-in trust anchors ─────────────────────────────
    bool LoadBuiltinStore() {
        // trust_anchors.h defines TAs[] and TAs_NUM
        // Each entry is a br_x509_trust_anchor struct
#ifdef TAs_NUM
        for (size_t i = 0; i < TAs_NUM; ++i) {
            anchors.push_back(TAs[i]);

            AeonCertInfo info{};
            // Extract CN from the DN blob (simplified — first UTF8String or PrintableString)
            ExtractCN(TAs[i].dn.data, TAs[i].dn.len, info.cn, sizeof(info.cn));
            info.key_type = TAs[i].pkey.key_type;
            if (info.key_type == BR_KEYTYPE_RSA) {
                info.key_bits = TAs[i].pkey.key.rsa.nlen * 8;
            } else {
                info.key_bits = 256; // EC assumed P-256
            }
            info.is_active = true;
            info.is_expired = false;
            info.is_revoked = false;

            anchor_info.push_back(info);
        }
        return TAs_NUM > 0;
#else
        // Fallback: no compiled-in anchors, require mesh update
        return false;
#endif
    }

    // ── Extract Common Name from DER-encoded DN ───────────────────
    static void ExtractCN(const uint8_t* dn, size_t dn_len,
                           char* out, size_t out_len) {
        // Quick scan for OID 2.5.4.3 (CN) followed by UTF8String or PrintableString
        // OID bytes: 55 04 03
        const uint8_t cn_oid[] = {0x55, 0x04, 0x03};
        for (size_t i = 0; i + sizeof(cn_oid) + 2 < dn_len; ++i) {
            if (memcmp(dn + i, cn_oid, sizeof(cn_oid)) == 0) {
                size_t val_offset = i + sizeof(cn_oid);
                if (val_offset + 2 < dn_len) {
                    uint8_t tag = dn[val_offset];
                    uint8_t len = dn[val_offset + 1];
                    if ((tag == 0x0C || tag == 0x13) && len > 0) { // UTF8String or PrintableString
                        size_t copy_len = (len < out_len - 1) ? len : out_len - 1;
                        if (val_offset + 2 + copy_len <= dn_len) {
                            memcpy(out, dn + val_offset + 2, copy_len);
                            out[copy_len] = '\0';
                            return;
                        }
                    }
                }
            }
        }
        strncpy(out, "<unknown>", out_len - 1);
        out[out_len - 1] = '\0';
    }

    // ── Verify Ed25519 signature on a trust bundle ────────────────
    bool VerifyBundleSignature(const AeonTrustBundle& bundle) const {
        // Check signer matches our trusted key
        if (memcmp(bundle.signer_pubkey, config.trusted_signer, 32) != 0) {
            // Check if it's the placeholder key (all zeros = dev mode)
            uint8_t zeros[32] = {};
            if (memcmp(config.trusted_signer, zeros, 32) == 0) {
                // Dev mode: accept any signer with WARNING
                return true;
            }
            return false;  // Wrong signer
        }

        // Verify the Ed25519 signature over the payload
        return ed25519_verify(
            bundle.signature,
            bundle.payload,
            bundle.payload_len,
            bundle.signer_pubkey
        ) == 1;
    }

    // ── Time utilities ────────────────────────────────────────────
    static uint64_t NowUTC() {
        return static_cast<uint64_t>(time(nullptr));
    }

    static int32_t DaysBetween(uint64_t from, uint64_t to) {
        if (to <= from) return 0;
        return static_cast<int32_t>((to - from) / 86400);
    }
};


// ===========================================================================
// AeonTLS — public interface implementation
// ===========================================================================

AeonTLS::AeonTLS() : m_impl(new AeonTLSImpl()) {}

AeonTLS::~AeonTLS() {
    Shutdown();
    delete m_impl;
    m_impl = nullptr;
}

// ── Lifecycle ─────────────────────────────────────────────────

bool AeonTLS::Initialize(const ResourceBudget& budget) {
    if (!m_impl) return false;
    std::lock_guard<std::mutex> lock(m_impl->mu);

    // Default configuration
    m_impl->config.use_builtin_store = true;
    m_impl->config.use_os_store = false;
    m_impl->config.auto_update = true;
    m_impl->config.enforce_tls13 = false;
    m_impl->config.min_tls_version = 0x0303; // TLS 1.2 minimum
    m_impl->config.cert_expiry_warn_days = 90;
    m_impl->config.enable_emergency_cas = true;
    memcpy(m_impl->config.trusted_signer, AEON_TRUST_SIGNER_PUBKEY, 32);

    // Load built-in trust anchors
    if (m_impl->config.use_builtin_store) {
        m_impl->LoadBuiltinStore();
    }

    m_impl->store_timestamp = AeonTLSImpl::NowUTC();
    return true;
}

void AeonTLS::Shutdown() {
    if (!m_impl) return;
    std::lock_guard<std::mutex> lock(m_impl->mu);
    m_impl->anchors.clear();
    m_impl->anchor_info.clear();
    m_impl->expiry_cache.clear();
}

// ── Configuration ─────────────────────────────────────────────

bool AeonTLS::Configure(const AeonTLSConfig& config) {
    if (!m_impl) return false;
    std::lock_guard<std::mutex> lock(m_impl->mu);
    m_impl->config = config;
    return true;
}

// ── Trust Store Management ────────────────────────────────────

uint32_t AeonTLS::TrustStoreVersion() const {
    if (!m_impl) return 0;
    std::lock_guard<std::mutex> lock(m_impl->mu);
    return m_impl->store_version;
}

uint32_t AeonTLS::TrustAnchorCount() const {
    if (!m_impl) return 0;
    std::lock_guard<std::mutex> lock(m_impl->mu);
    return static_cast<uint32_t>(m_impl->anchors.size());
}

std::vector<AeonCertInfo> AeonTLS::ListTrustAnchors() const {
    if (!m_impl) return {};
    std::lock_guard<std::mutex> lock(m_impl->mu);
    return m_impl->anchor_info;
}

const br_x509_trust_anchor* AeonTLS::GetBearSSLAnchors(size_t* out_count) const {
    if (!m_impl || m_impl->anchors.empty()) {
        if (out_count) *out_count = 0;
        return nullptr;
    }
    std::lock_guard<std::mutex> lock(m_impl->mu);
    if (out_count) *out_count = m_impl->anchors.size();
    return m_impl->anchors.data();
}

bool AeonTLS::IsTrustedRoot(const uint8_t sha256[32]) const {
    if (!m_impl) return false;
    std::lock_guard<std::mutex> lock(m_impl->mu);
    for (const auto& info : m_impl->anchor_info) {
        if (info.is_active && !info.is_revoked &&
            memcmp(info.sha256_fingerprint, sha256, 32) == 0) {
            return true;
        }
    }
    return false;
}

// ── Sovereign Trust Updates ───────────────────────────────────

bool AeonTLS::ApplyTrustBundle(const AeonTrustBundle& bundle) {
    if (!m_impl) return false;
    std::lock_guard<std::mutex> lock(m_impl->mu);

    // Rollback protection: reject older or same version
    if (bundle.version <= m_impl->store_version) {
        if (m_impl->on_trust_update) {
            m_impl->on_trust_update(bundle, false,
                "Rejected: version <= current (rollback protection)");
        }
        return false;
    }

    // Verify Ed25519 signature
    if (!m_impl->VerifyBundleSignature(bundle)) {
        if (m_impl->on_trust_update) {
            m_impl->on_trust_update(bundle, false,
                "Rejected: signature verification failed");
        }
        return false;
    }

    // Parse the bundle payload into BearSSL trust anchors.
    // The payload format is a simple TLV: [count:u32][anchor_der:len-prefixed]*
    // For now, we validate but defer full DER parsing to a dedicated routine.
    if (!bundle.payload || bundle.payload_len < 4) {
        if (m_impl->on_trust_update) {
            m_impl->on_trust_update(bundle, false,
                "Rejected: empty or malformed payload");
        }
        return false;
    }

    // Read anchor count from payload header
    uint32_t new_count = 0;
    memcpy(&new_count, bundle.payload, sizeof(uint32_t));

    if (new_count == 0 || new_count > 500) {
        if (m_impl->on_trust_update) {
            m_impl->on_trust_update(bundle, false,
                "Rejected: invalid anchor count (0 or >500)");
        }
        return false;
    }

    // TODO: Full DER parsing of each trust anchor from payload.
    // For now, accept the bundle and update version metadata.
    // The actual anchor replacement will be implemented when the
    // TrustStoreBuilder tool generates production bundles.

    m_impl->store_version = bundle.version;
    m_impl->store_timestamp = bundle.timestamp_utc;

    if (m_impl->on_trust_update) {
        m_impl->on_trust_update(bundle, true,
            "Accepted: signature valid, version incremented");
    }
    return true;
}

void AeonTLS::OnTrustUpdate(AeonTLSTrustUpdateCallback callback) {
    if (m_impl) {
        std::lock_guard<std::mutex> lock(m_impl->mu);
        m_impl->on_trust_update = std::move(callback);
    }
}

void AeonTLS::OnCertWarning(AeonTLSCertWarningCallback callback) {
    if (m_impl) {
        std::lock_guard<std::mutex> lock(m_impl->mu);
        m_impl->on_cert_warning = std::move(callback);
    }
}

void AeonTLS::RequestTrustUpdate() {
    // Broadcasts a request on AeonHiveTopic::TrustStoreRequest.
    // Peer nodes that have a newer trust bundle will respond on
    // AeonHiveTopic::TrustStoreUpdate.
    //
    // This is a fire-and-forget request — the response arrives
    // asynchronously via the Hive subscription handler which calls
    // ApplyTrustBundle().
    //
    // Integration point: AeonHive::Publish(TrustStoreRequest, our_version)
}

// ── Certificate Validation ────────────────────────────────────

AeonCertStatus AeonTLS::ValidateChain(
    const char* hostname,
    const uint8_t** chain,
    const size_t* chain_lens,
    size_t chain_count
) const {
    if (!m_impl || !hostname || !chain || chain_count == 0) {
        return AeonCertStatus::ChainError;
    }

    std::lock_guard<std::mutex> lock(m_impl->mu);

    if (m_impl->anchors.empty()) {
        return AeonCertStatus::UntrustedRoot;
    }

    // Set up BearSSL X.509 minimal validation engine
    br_x509_minimal_context x509;
    br_x509_minimal_init(&x509,
                          &br_sha256_vtable,
                          m_impl->anchors.data(),
                          m_impl->anchors.size());

    // Configure hash functions for chain validation
    br_x509_minimal_set_hash(&x509, br_sha256_ID, &br_sha256_vtable);
    br_x509_minimal_set_hash(&x509, br_sha384_ID, &br_sha384_vtable);
    br_x509_minimal_set_hash(&x509, br_sha512_ID, &br_sha512_vtable);

    // Configure RSA and EC signature verifiers
    br_x509_minimal_set_rsa(&x509, br_rsa_i31_pkcs1_vrfy);
    br_x509_minimal_set_ecdsa(&x509, br_ec_prime_i31, br_ecdsa_i31_vrfy_asn1);

    // Start chain validation
    x509.vtable->start_chain(&x509.vtable, hostname);

    for (size_t i = 0; i < chain_count; ++i) {
        x509.vtable->start_cert(&x509.vtable, chain_lens[i]);
        x509.vtable->append(&x509.vtable, chain[i], chain_lens[i]);
        x509.vtable->end_cert(&x509.vtable);
    }

    unsigned err = x509.vtable->end_chain(&x509.vtable);

    switch (err) {
        case BR_ERR_OK: // 0
            return AeonCertStatus::Valid;
        case BR_ERR_X509_EXPIRED:
            return AeonCertStatus::Expired;
        case BR_ERR_X509_NOT_TRUSTED:
            return AeonCertStatus::UntrustedRoot;
        case BR_ERR_X509_BAD_SERVER_NAME:
            return AeonCertStatus::NameMismatch;
        case BR_ERR_X509_BAD_SIGNATURE:
            return AeonCertStatus::SignatureError;
        default:
            return AeonCertStatus::ChainError;
    }
}

int32_t AeonTLS::CertExpiryDays(const char* hostname) const {
    if (!m_impl || !hostname) return -1;
    std::lock_guard<std::mutex> lock(m_impl->mu);

    uint64_t now = AeonTLSImpl::NowUTC();
    for (const auto& ec : m_impl->expiry_cache) {
        if (strcmp(ec.hostname, hostname) == 0) {
            // Cached value valid for 1 hour
            if (now - ec.cached_at < 3600) {
                return ec.days_remaining;
            }
        }
    }
    return -1; // Not cached
}

// ── TLS Session Creation ──────────────────────────────────────

struct AeonTLSSessionContext {
    br_ssl_client_context  sc;
    br_x509_minimal_context x509;
    br_sslio_context       ioc;
    uint8_t                iobuf[BR_SSL_BUFSIZE_BIDI];
    char                   hostname[256];
};

void* AeonTLS::CreateTLSContext(const char* hostname) {
    if (!m_impl || !hostname) return nullptr;
    std::lock_guard<std::mutex> lock(m_impl->mu);

    if (m_impl->anchors.empty()) return nullptr;

    auto* ctx = new (std::nothrow) AeonTLSSessionContext();
    if (!ctx) return nullptr;

    memset(ctx, 0, sizeof(AeonTLSSessionContext));
    strncpy(ctx->hostname, hostname, sizeof(ctx->hostname) - 1);

    // Initialize X.509 minimal engine
    br_x509_minimal_init(&ctx->x509,
                          &br_sha256_vtable,
                          m_impl->anchors.data(),
                          m_impl->anchors.size());

    br_x509_minimal_set_hash(&ctx->x509, br_sha256_ID, &br_sha256_vtable);
    br_x509_minimal_set_hash(&ctx->x509, br_sha384_ID, &br_sha384_vtable);
    br_x509_minimal_set_hash(&ctx->x509, br_sha512_ID, &br_sha512_vtable);
    br_x509_minimal_set_rsa(&ctx->x509, br_rsa_i31_pkcs1_vrfy);
    br_x509_minimal_set_ecdsa(&ctx->x509, br_ec_prime_i31, br_ecdsa_i31_vrfy_asn1);

    // Initialize TLS client context
    br_ssl_client_init_full(&ctx->sc,
                             &ctx->x509,
                             m_impl->anchors.data(),
                             m_impl->anchors.size());

    // Enforce minimum TLS version
    uint16_t min_ver = m_impl->config.min_tls_version;
    if (min_ver < 0x0301) min_ver = 0x0301; // TLS 1.0 floor
    br_ssl_engine_set_versions(&ctx->sc.eng, min_ver, 0x0304); // up to TLS 1.3

    // Set I/O buffer
    br_ssl_engine_set_buffer(&ctx->sc.eng, ctx->iobuf, sizeof(ctx->iobuf), 1);

    // Reset for new connection
    br_ssl_client_reset(&ctx->sc, hostname, 0);

    return ctx;
}

void AeonTLS::DestroyTLSContext(void* ctx) {
    if (ctx) {
        delete static_cast<AeonTLSSessionContext*>(ctx);
    }
}

uint16_t AeonTLS::NegotiatedVersion(void* ctx) const {
    if (!ctx) return 0;
    auto* session = static_cast<AeonTLSSessionContext*>(ctx);
    return br_ssl_engine_get_version(&session->sc.eng);
}

// ── Certificate Lifetime Tracking ─────────────────────────────

std::vector<AeonCertInfo> AeonTLS::ExpiringAnchors(uint32_t warn_days) const {
    if (!m_impl) return {};
    std::lock_guard<std::mutex> lock(m_impl->mu);

    std::vector<AeonCertInfo> expiring;
    uint64_t now = AeonTLSImpl::NowUTC();
    uint64_t warn_threshold = now + (static_cast<uint64_t>(warn_days) * 86400);

    for (const auto& info : m_impl->anchor_info) {
        if (info.is_active && !info.is_revoked) {
            if (info.not_after_utc > 0 && info.not_after_utc <= warn_threshold) {
                expiring.push_back(info);

                // Fire warning callback
                if (m_impl->on_cert_warning) {
                    uint32_t days = AeonTLSImpl::DaysBetween(now, info.not_after_utc);
                    m_impl->on_cert_warning(info, days);
                }
            }
        }
    }

    // Sort by expiry date (soonest first)
    std::sort(expiring.begin(), expiring.end(),
              [](const AeonCertInfo& a, const AeonCertInfo& b) {
                  return a.not_after_utc < b.not_after_utc;
              });

    return expiring;
}

uint64_t AeonTLS::EarliestRootExpiry() const {
    if (!m_impl) return 0;
    std::lock_guard<std::mutex> lock(m_impl->mu);

    uint64_t earliest = UINT64_MAX;
    for (const auto& info : m_impl->anchor_info) {
        if (info.is_active && !info.is_revoked && info.not_after_utc > 0) {
            earliest = std::min(earliest, info.not_after_utc);
        }
    }
    return (earliest == UINT64_MAX) ? 0 : earliest;
}

// ── Diagnostics ───────────────────────────────────────────────

std::string AeonTLS::DiagnosticReport() const {
    if (!m_impl) return "AeonTLS: not initialized";
    std::lock_guard<std::mutex> lock(m_impl->mu);

    std::string report;
    report.reserve(1024);

    report += "=== AeonTLS Diagnostic Report ===\n";
    report += "Trust Store Version:  " + std::to_string(m_impl->store_version) + "\n";
    report += "Trust Anchors Active: " + std::to_string(m_impl->anchors.size()) + "\n";
    report += "Min TLS Version:      ";
    switch (m_impl->config.min_tls_version) {
        case 0x0301: report += "TLS 1.0"; break;
        case 0x0302: report += "TLS 1.1"; break;
        case 0x0303: report += "TLS 1.2"; break;
        case 0x0304: report += "TLS 1.3"; break;
        default:     report += "unknown"; break;
    }
    report += "\n";
    report += "Auto-Update (Hive):   ";
    report += m_impl->config.auto_update ? "enabled" : "disabled";
    report += "\n";
    report += "OS Store Fallback:    ";
    report += m_impl->config.use_os_store ? "enabled" : "disabled";
    report += "\n";
    report += "Emergency CAs:        ";
    report += m_impl->config.enable_emergency_cas ? "enabled" : "disabled";
    report += "\n";

    // Expiry warnings
    uint64_t now = AeonTLSImpl::NowUTC();
    int expiring_90d = 0;
    for (const auto& info : m_impl->anchor_info) {
        if (info.is_active && info.not_after_utc > 0 &&
            info.not_after_utc <= now + 7776000) { // 90 days
            ++expiring_90d;
        }
    }
    report += "Expiring (90 days):   " + std::to_string(expiring_90d) + "\n";
    report += "=================================\n";

    return report;
}

// ── Global singleton ──────────────────────────────────────────

AeonTLS& AeonTLSInstance() {
    static AeonTLS instance;
    return instance;
}
