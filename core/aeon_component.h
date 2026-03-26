// =============================================================================
// aeon_component.h — AeonDNA Base Interface
// Project: Aeon Browser (DelgadoLogic)
//
// REQUIRED: Every native component that ships in Aeon must inherit IAeonComponent.
// This ensures all components are:
//   • Updatable via Sovereign Key (Ed25519-signed manifests)
//   • Improvable by the AI Hive (anonymous metrics → targeted patches)
//   • XP/Low-RAM aware (resource budgets + peer offload)
//   • Rollback-capable (atomic updates with fallback)
//
// See: research/STANDARDS.md for full contract + graduation checklist
// =============================================================================

#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>

// ---------------------------------------------------------------------------
// Forward declarations
// ---------------------------------------------------------------------------
struct AeonManifest;
struct UpdatePackage;
struct HiveInsight;
class  AeonHive;

// ---------------------------------------------------------------------------
// AeonManifest — sovereign-signed component update descriptor
// ---------------------------------------------------------------------------
struct AeonManifest {
    char     component_id[64];      // e.g. "aeon.spell"
    char     version[32];           // e.g. "1.3.0"
    char     upstream_ref[128];     // e.g. "hunspell/hunspell@2.1.0"
    uint8_t  ed25519_signature[64]; // Sovereign Key signature over SHA-256(payload)
    uint8_t  payload_sha256[32];    // hash of the update payload
    uint64_t timestamp_utc;         // unix timestamp of signing
    uint32_t flags;                 // AEON_MANIFEST_FLAG_*
};

#define AEON_MANIFEST_FLAG_AUTO_APPLY  0x01  // safe to apply without restart
#define AEON_MANIFEST_FLAG_INSIGHT     0x02  // AI Hive-proposed improvement
#define AEON_MANIFEST_FLAG_EMERGENCY   0x04  // emergency rollback manifest

// ---------------------------------------------------------------------------
// UpdatePackage — the actual patch payload
// ---------------------------------------------------------------------------
struct UpdatePackage {
    const uint8_t* data;
    size_t         data_len;
    const char*    target_path;     // relative path inside install dir
    bool           is_delta;        // true = bsdiff delta, false = full file
};

// ---------------------------------------------------------------------------
// HiveInsight — AI-proposed optimization from the Aeon network
// ---------------------------------------------------------------------------
struct HiveInsight {
    char     insight_id[64];
    char     description[256];
    uint8_t  ed25519_signature[64];
    int      risk_level;            // AEON_INSIGHT_RISK_*
    // Payload is a JSON config delta or a sovereign-signed micro-patch
    const char* payload_json;
    size_t      payload_len;
};

#define AEON_INSIGHT_RISK_CONFIG  1   // config tuning — auto-apply
#define AEON_INSIGHT_RISK_PATCH   2   // code path change — auto-apply after CI
#define AEON_INSIGHT_RISK_MODEL   3   // model weight update — sovereign review
#define AEON_INSIGHT_RISK_ARCH    4   // architectural change — human review

// ---------------------------------------------------------------------------
// ComponentMetrics — anonymous performance counters for Hive reporting
// ---------------------------------------------------------------------------
struct ComponentMetrics {
    const char* component_id;
    uint64_t    invocations_total;
    uint64_t    latency_p50_us;     // microseconds
    uint64_t    latency_p99_us;
    uint64_t    failure_count;
    uint64_t    hive_offload_count; // times we offloaded to a peer
    uint64_t    ram_peak_bytes;
    uint32_t    cpu_class;          // 0=SSE2/XP, 1=SSE4, 2=AVX2, 3=AVX512
    // NO user data, NO URLs, NO content — counters only
};

// ---------------------------------------------------------------------------
// ResourceBudget — set by the browser at startup based on detected hardware
// ---------------------------------------------------------------------------
struct ResourceBudget {
    size_t max_ram_bytes;      // hard RAM ceiling for this component
    int    cpu_class;          // AEON_CPU_CLASS_*
    bool   hive_offload_ok;   // true = can send heavy work to Hive peer
    int    target_latency_ms;  // expected max response time
};

#define AEON_CPU_CLASS_SSE2   0   // Pentium 4 / XP baseline
#define AEON_CPU_CLASS_SSE4   1   // Core 2 / Win 7 common
#define AEON_CPU_CLASS_AVX2   2   // Haswell+ / Win 10 modern
#define AEON_CPU_CLASS_AVX512 3   // Skylake-X+ / Gaming builds

// ---------------------------------------------------------------------------
// IAeonComponent — the base contract
// ---------------------------------------------------------------------------
class IAeonComponent {
public:
    // ── Identity ──────────────────────────────────────────────────────────
    virtual const char* ComponentId()      const = 0; // "aeon.spell"
    virtual const char* ComponentVersion() const = 0; // "1.0.0"
    virtual const char* UpstreamRef()      const = 0; // "hunspell/hunspell@2.1.0"

    // ── Lifecycle ─────────────────────────────────────────────────────────
    virtual bool Initialize(const ResourceBudget& budget) = 0;
    virtual void Shutdown()                                = 0;

    // ── Sovereign Update Protocol ─────────────────────────────────────────
    // Called at startup and when AeonHive broadcasts a new manifest
    virtual bool CheckManifest(const AeonManifest& manifest)   = 0; // sig verify
    virtual bool ApplyUpdate(const UpdatePackage& pkg)          = 0; // atomic patch
    virtual bool RollbackToVersion(const char* version)         = 0; // emergency

    // ── AI Hive Improvement Loop ──────────────────────────────────────────
    virtual void ReportMetrics(AeonHive& hive)                  = 0; // opt-in
    virtual void ReceiveInsight(const HiveInsight& insight)     = 0; // AI proposal
    virtual bool ApplyInsight(const HiveInsight& insight)       = 0; // auto-apply

    // ── Resource Awareness (XP / Low-RAM) ────────────────────────────────
    virtual void SetResourceBudget(const ResourceBudget& budget) = 0;
    virtual bool CanOffloadToHive()                         const = 0;
    virtual ResourceBudget GetCurrentBudget()               const = 0;

    // ── Status ────────────────────────────────────────────────────────────
    virtual bool     IsHealthy()       const = 0;
    virtual uint64_t LastUpdateTime()  const = 0; // unix UTC

    virtual ~IAeonComponent() = default;
};

// ---------------------------------------------------------------------------
// AeonComponentBase — convenience mixin with default implementations
// for Sovereign Update and Hive boilerplate, so concrete classes only
// override what they need.
// ---------------------------------------------------------------------------
class AeonComponentBase : public IAeonComponent {
public:
    bool CheckManifest(const AeonManifest& m) override;  // verifies Ed25519
    bool ApplyUpdate(const UpdatePackage& pkg) override; // atomic write + verify
    bool RollbackToVersion(const char* version) override;
    void ReportMetrics(AeonHive& hive) override;         // sends m_metrics
    void ReceiveInsight(const HiveInsight& insight) override;
    bool ApplyInsight(const HiveInsight& insight) override;
    bool IsHealthy() const override { return m_healthy; }
    uint64_t LastUpdateTime() const override { return m_last_update_utc; }
    ResourceBudget GetCurrentBudget() const override { return m_budget; }
    void SetResourceBudget(const ResourceBudget& b) override { m_budget = b; }

protected:
    ComponentMetrics m_metrics{};
    ResourceBudget   m_budget{};
    bool             m_healthy       = false;
    uint64_t         m_last_update_utc = 0;

    // Subclasses call this to record invocations
    void TrackInvocation(uint64_t latency_us, bool failed = false);
};
