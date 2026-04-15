// =============================================================================
// aeon_component.cpp — AeonComponentBase default implementations
// Project: Aeon Browser (DelgadoLogic)
//
// Provides default (stub) implementations for the Sovereign Update Protocol
// and AI Hive Improvement Loop. Concrete components override these when they
// are ready to graduate to full sovereign-update capability.
// =============================================================================

#include "aeon_component.h"
#include <cstring>

// ---------------------------------------------------------------------------
// Sovereign Update Protocol — stubs until components graduate
// ---------------------------------------------------------------------------

bool AeonComponentBase::CheckManifest(const AeonManifest& manifest) {
    // TODO(aeon): Implement Ed25519 signature verification against
    // the Sovereign Key stored in core/security/sovereign_key.pub
    // For now, reject all manifests — no updates until crypto is wired.
    (void)manifest;
    return false;
}

bool AeonComponentBase::ApplyUpdate(const UpdatePackage& pkg) {
    // TODO(aeon): Implement atomic write + SHA-256 verification
    // 1. Write pkg.data to a temp file next to pkg.target_path
    // 2. Verify SHA-256 matches the manifest
    // 3. Atomic rename into place
    // 4. Record m_last_update_utc
    (void)pkg;
    return false;
}

bool AeonComponentBase::RollbackToVersion(const char* version) {
    // TODO(aeon): Restore the previous version from the rollback cache
    // at %APPDATA%/DelgadoLogic/Aeon/rollback/{component_id}/{version}
    (void)version;
    return false;
}

// ---------------------------------------------------------------------------
// AI Hive Improvement Loop — stubs until Hive mesh is active
// ---------------------------------------------------------------------------

void AeonComponentBase::ReportMetrics(AeonHive& hive) {
    // When Hive is active, this sends m_metrics (anonymous counters only)
    // to the nearest peer for aggregation. No user data leaves the device.
    (void)hive;
    // Default: no-op — components opt in by overriding
}

void AeonComponentBase::ReceiveInsight(const HiveInsight& insight) {
    // Store the insight for later evaluation. In production, this queues
    // insights for a review pass during idle time.
    (void)insight;
}

bool AeonComponentBase::ApplyInsight(const HiveInsight& insight) {
    // Auto-apply is disabled by default. Components that want AI-driven
    // tuning override this and check insight.risk_level.
    (void)insight;
    return false;
}

// ---------------------------------------------------------------------------
// Internal helpers
// ---------------------------------------------------------------------------

void AeonComponentBase::TrackInvocation(uint64_t latency_us, bool failed) {
    m_metrics.invocations_total++;
    if (failed) {
        m_metrics.failure_count++;
    }

    // Update P50/P99 with exponential moving average (simplified)
    // In production this would use a proper histogram/reservoir
    const uint64_t alpha = 8;  // 1/8 weight for new samples
    m_metrics.latency_p50_us =
        m_metrics.latency_p50_us
            ? m_metrics.latency_p50_us + (latency_us - m_metrics.latency_p50_us) / alpha
            : latency_us;

    // P99 only updates if this sample exceeds current estimate
    if (latency_us > m_metrics.latency_p99_us) {
        m_metrics.latency_p99_us =
            m_metrics.latency_p99_us
                ? m_metrics.latency_p99_us + (latency_us - m_metrics.latency_p99_us) / 2
                : latency_us;
    }
}
