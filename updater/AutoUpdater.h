// AeonBrowser — AutoUpdater.h v2.1
// Sovereign Ed25519 + SHA-256 Update System (api.delgadologic.tech)
#pragma once

#include <string>
#include <vector>
#include <cstdint>

namespace AutoUpdater {

    // Canonical sovereign update endpoint (matches OmniCore.Engine & aeon-update-server)
    inline constexpr const char* MANIFEST_URL =
        "https://api.delgadologic.tech/v1/update/aeon/stable";

    // Pinned DelgadoLogic sovereign Ed25519 public key (SPKI DER Base64)
    inline constexpr const char* SOVEREIGN_ED25519_PUBKEY_B64 =
        "MCowBQYDK2VwAyEABfoHnS014mgux9fNHwe0zzRPEqVoy0OOJ+5tVvfu3a4=";

    // ── State machine ──────────────────────────────────────────────────────
    enum class UpdateState : int {
        Idle            = 0,  // No update activity
        Checking        = 1,  // Polling manifest
        Downloading     = 2,  // Downloading chunks (P2P or HTTPS)
        ReadyToInstall  = 3,  // Staged, waiting for cold start
        Failed          = 4   // Last attempt failed
    };

    // ── Verified Manifest from api.delgadologic.tech ───────────────────────
    struct UpdateManifest {
        std::string product = "aeon";
        std::string channel = "stable";
        std::string version;
        std::string status;
        std::string download_url;
        std::string release_notes;
        std::string sha256;
        int64_t     size_bytes = 0;
        std::string signature;  // Base64 (or hex) 64-byte Ed25519 signature over canonical sorted-key JSON
        std::vector<std::string> chunk_urls;
    };

    // ── Lightweight info (reported to UI) ──────────────────────────────────
    struct UpdateInfo {
        bool        update_available = false;
        std::string version;
        std::string download_url;
        std::string sha256;
    };

    // ── Canonical JSON & Ed25519 Verification Helpers (Matching OmniCore.Engine) ──

    // Produces canonical sorted-key JSON identical to Python json.dumps(obj, sort_keys=True, separators=(",", ":"))
    // and OmniCore.Engine.AutoUpdateEngine.BuildCanonicalManifestJson.
    std::string CanonicalizeJsonString(const std::string& rawManifestObjectJson);

    // Parses an envelope {"manifest": {...}, "signature": "<base64>"} and verifies its Ed25519 signature
    // against canonical sorted-key JSON before populating outManifest. Fails closed on any tampering.
    bool TryParseAndVerifyEnvelope(
        const std::string& rawEnvelopeJson,
        UpdateManifest& outManifest,
        const std::string& pubKeyOverrideB64 = "");

    // Validates a 64-character hexadecimal SHA-256 string.
    bool IsValidSha256Hex(const std::string& hash);

    // Validates that a URL is non-empty and strictly uses the https:// scheme.
    bool IsTrustedHttpsUrl(const std::string& url);

    // ── Public API ─────────────────────────────────────────────────────────

    // Call BEFORE painting any windows — applies staged update atomically
    void CheckAndInstallStagedUpdate();

    // Start the background poller (runs every 6h, P2P-first with HTTPS fallback)
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
