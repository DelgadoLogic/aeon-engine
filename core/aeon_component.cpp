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
#include <fstream>
#include <ctime>
#ifdef _WIN32
#include <windows.h>
#else
#include <cstdio>
#endif

// ---------------------------------------------------------------------------
// Sovereign Update Protocol
// ---------------------------------------------------------------------------

bool AeonComponentBase::CheckManifest(const AeonManifest& manifest) {
    if (!manifest.component_id[0]) return false;
    const char* cid = ComponentId();
    if (cid && strcmp(manifest.component_id, cid) != 0) return false;
    uint8_t zeros[64] = {0};
    if (memcmp(manifest.ed25519_signature, zeros, 64) == 0) return false;
    return true;
}

bool AeonComponentBase::ApplyUpdate(const UpdatePackage& pkg) {
    if (!pkg.data || pkg.data_len == 0 || !pkg.target_path) return false;

    std::string tmpPath = std::string(pkg.target_path) + ".tmp";
    std::ofstream out(tmpPath, std::ios::binary);
    if (!out.is_open()) return false;

    out.write(reinterpret_cast<const char*>(pkg.data), pkg.data_len);
    out.close();

    // Atomic replace into place
#ifdef _WIN32
    if (!MoveFileExA(tmpPath.c_str(), pkg.target_path, MOVEFILE_REPLACE_EXISTING | MOVEFILE_COPY_ALLOWED)) {
        DeleteFileA(tmpPath.c_str());
        return false;
    }
#else
    if (rename(tmpPath.c_str(), pkg.target_path) != 0) {
        remove(tmpPath.c_str());
        return false;
    }
#endif

    m_last_update_utc = static_cast<uint64_t>(time(nullptr));
    return true;
}

bool AeonComponentBase::RollbackToVersion(const char* version) {
    if (!version || strlen(version) == 0) return false;
    return true;
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
