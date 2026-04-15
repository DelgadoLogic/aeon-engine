// AeonBrowser — AutoUpdater.h v2
// Autonomous P2P + GCS Update System
#pragma once

#include <string>
#include <vector>

namespace AutoUpdater {

    // ── State machine ──────────────────────────────────────────────────────
    enum class UpdateState : int {
        Idle            = 0,  // No update activity
        Checking        = 1,  // Polling manifest
        Downloading     = 2,  // Downloading chunks (P2P or GCS)
        ReadyToInstall  = 3,  // Staged, waiting for cold start
        Failed          = 4   // Last attempt failed
    };

    // ── Manifest from GCS ──────────────────────────────────────────────────
    struct UpdateManifest {
        std::string version;
        std::string sha256;
        std::string signature;  // Ed25519 signature of the manifest
        std::vector<std::string> chunk_urls;
    };

    // ── Lightweight info (reported to UI) ──────────────────────────────────
    struct UpdateInfo {
        bool        update_available = false;
        std::string version;
        std::string download_url;
        std::string sha256;
    };

    // ── Public API ─────────────────────────────────────────────────────────

    // Call BEFORE painting any windows — applies staged update atomically
    void CheckAndInstallStagedUpdate();

    // Start the background poller (runs every 6h, P2P-first downloads)
    void Start(std::vector<std::string> hive_peers);

    // Graceful shutdown
    void Shutdown();

    // Current state of the update system
    UpdateState GetState();

    // Download progress (0.0 – 1.0)
    float GetProgress();

    // Version string of the staged update (if any)
    std::string GetStagedVersion();

    // ── Legacy compatibility aliases ───────────────────────────────────────
    inline void StartPoller() { Start({}); }
    inline void CheckNow()   { /* triggers on next poll cycle */ }

} // namespace AutoUpdater
