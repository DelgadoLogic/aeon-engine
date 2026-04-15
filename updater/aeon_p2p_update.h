// =============================================================================
// aeon_p2p_update.h — AeonP2PUpdate: Peer-to-Peer Distribution & Updates
// Graduated from: BitTorrent protocol concepts (clean-room)
//
// AeonP2PUpdate enables Aeon Browser to update itself WITHOUT internet access.
// In emerging markets where data is expensive and shared via SHAREit/Xender,
// this component allows browsers to distribute updates peer-to-peer:
//
//   1. Device A downloads update 1.2.0 on WiFi
//   2. Device B (no internet) connects to local mesh
//   3. A transfers the signed update binary to B via AeonHive
//   4. B verifies Ed25519 signature against trusted signer key
//   5. B applies the update and becomes a relay for Device C
//
// This creates a "wave propagation" effect where a single download can
// update thousands of devices across a community network.
//
// What we improve over Play Store / traditional update channels:
//   [+] Zero internet requirement after initial seed download
//   [+] P2P relay — each updated device becomes a distributor
//   [+] Ed25519 signed manifests — cryptographic authenticity
//   [+] Chunk-based transfer — resume after interruption, parallel chunks
//   [+] Size-optimized: Aeon Lite APK target <15MB (vs. Chrome 200MB+)
//   [+] Platform-agnostic: works on Windows, Android, Linux
//   [+] No Google Play Services dependency
//   [+] Works on local-only networks (WiFi Direct, Bluetooth PAN)
// =============================================================================

#pragma once

#include "aeon_component.h"
#include <cstdint>
#include <cstddef>
#include <functional>
#include <string>
#include <vector>

// Forward declarations
class AeonP2PUpdateImpl;

// ---------------------------------------------------------------------------
// AeonPlatform — target platform for update packages
// ---------------------------------------------------------------------------
enum class AeonPlatform : uint8_t {
    WindowsX86      = 0x01,     // Win32 (Win7+)
    WindowsX64      = 0x02,     // Win64 (Win7+)
    WindowsLegacy   = 0x03,     // Win95-Vista (retro build)
    AndroidARM      = 0x10,     // ARMv7 (Android 5.0+)
    AndroidARM64    = 0x11,     // AArch64 (Android 8.0+)
    AndroidX86      = 0x12,     // x86 Android
    LinuxX64        = 0x20,     // Linux amd64
    LinuxARM64      = 0x21,     // Linux aarch64
    Universal       = 0xFF,     // Applies to all platforms
};

// ---------------------------------------------------------------------------
// AeonUpdateManifest — describes an available update
// ---------------------------------------------------------------------------
struct AeonUpdateManifest {
    char     version[32];           // Semantic version (e.g., "1.2.0")
    uint32_t version_code;          // Monotonic integer version
    AeonPlatform platform;          // Target platform
    uint64_t timestamp_utc;         // Build timestamp
    uint64_t total_size;            // Total update size (bytes)
    uint32_t chunk_count;           // Number of transfer chunks
    uint32_t chunk_size;            // Size per chunk (default: 256KB)
    uint8_t  sha256_hash[32];       // SHA-256 of complete update binary
    uint8_t  signer_pubkey[32];     // Ed25519 signer public key
    uint8_t  signature[64];         // Ed25519 signature over manifest fields
    char     changelog[4096];       // Human-readable release notes
    char     download_url[2048];    // HTTPS fallback URL (if internet available)
    char     ipfs_cid[128];         // IPFS content identifier (censorship-resistant)
    bool     is_critical;           // true = security update, force-prompt
    bool     is_delta;              // true = delta update (patch from prev version)
    char     delta_base_version[32];// Base version for delta (if is_delta)
};

// ---------------------------------------------------------------------------
// AeonUpdateChunk — a single transferable piece of the update
// ---------------------------------------------------------------------------
struct AeonUpdateChunk {
    uint32_t index;                 // Chunk index (0-based)
    uint32_t offset;                // Byte offset in the complete binary
    uint32_t length;                // Actual bytes in this chunk
    uint8_t  sha256_hash[32];       // SHA-256 of this chunk
    const uint8_t* data;            // Chunk payload
    bool     verified;              // Hash verified after receipt
};

// ---------------------------------------------------------------------------
// AeonUpdateProgress — transfer state
// ---------------------------------------------------------------------------
struct AeonUpdateProgress {
    char     version[32];           // Version being downloaded
    uint32_t chunks_total;          // Total chunks
    uint32_t chunks_received;       // Chunks successfully received
    uint32_t chunks_verified;       // Chunks hash-verified
    float    progress_percent;      // 0.0 - 100.0
    uint32_t peers_serving;         // Number of peers providing chunks
    uint32_t transfer_rate_kbps;    // Current download speed
    uint64_t eta_seconds;           // Estimated time remaining
    bool     is_complete;           // All chunks received and verified
    bool     is_applying;           // Update is being applied
};

// ---------------------------------------------------------------------------
// AeonP2PUpdateConfig — startup configuration
// ---------------------------------------------------------------------------
struct AeonP2PUpdateConfig {
    bool     auto_check;            // true = check for updates on startup
    bool     auto_download;         // true = download updates automatically
    bool     auto_apply;            // true = apply updates silently (danger!)
    bool     relay_updates;         // true = relay updates to other peers
    uint32_t relay_max_bandwidth_kbps; // Bandwidth cap for relaying (0=unlimited)
    uint32_t check_interval_hours;  // How often to check (default: 24)
    AeonPlatform platform;          // This device's platform

    // Trusted signer key (must match manifest signature)
    uint8_t  trusted_signer[32];
};

// ---------------------------------------------------------------------------
// Callbacks
// ---------------------------------------------------------------------------
using AeonUpdateAvailableCallback = std::function<void(
    const AeonUpdateManifest& manifest)>;

using AeonUpdateProgressCallback = std::function<void(
    const AeonUpdateProgress& progress)>;

using AeonUpdateAppliedCallback = std::function<void(
    const char* new_version, bool success, const char* error)>;

// ---------------------------------------------------------------------------
// AeonP2PUpdate — peer-to-peer update distribution engine
// ---------------------------------------------------------------------------
class AeonP2PUpdate final : public AeonComponentBase {
public:
    AeonP2PUpdate();
    ~AeonP2PUpdate() override;

    // ── IAeonComponent identity ───────────────────────────────────────────
    const char* ComponentId()      const override { return "aeon.p2pupdate"; }
    const char* ComponentVersion() const override { return "1.0.0"; }
    const char* UpstreamRef()      const override {
        return "clean-room-design (no upstream)";
    }

    // ── Lifecycle ─────────────────────────────────────────────────────────
    bool Initialize(const ResourceBudget& budget) override;
    void Shutdown() override;

    // ── Configuration ─────────────────────────────────────────────────────
    bool Configure(const AeonP2PUpdateConfig& config);

    // ── Update Discovery ──────────────────────────────────────────────────

    // Check for available updates (queries AeonHive mesh)
    bool CheckForUpdates();

    // Get the latest available manifest (null if none)
    const AeonUpdateManifest* LatestManifest() const;

    // Register callback for new update availability
    void OnUpdateAvailable(AeonUpdateAvailableCallback callback);

    // ── Download & Transfer ───────────────────────────────────────────────

    // Start downloading an update (from mesh peers and/or HTTPS fallback)
    bool StartDownload(const AeonUpdateManifest& manifest);

    // Pause/resume download
    void PauseDownload();
    void ResumeDownload();

    // Cancel download and discard chunks
    void CancelDownload();

    // Register progress callback
    void OnProgress(AeonUpdateProgressCallback callback);

    // Get current transfer state
    AeonUpdateProgress CurrentProgress() const;

    // ── Update Application ────────────────────────────────────────────────

    // Verify all chunks and prepare for installation
    bool Verify();

    // Apply the update (platform-specific)
    // Windows: replace EXE + restart via updater stub
    // Android: install APK via PackageManager intent
    bool Apply();

    // Register callback for update completion
    void OnApplied(AeonUpdateAppliedCallback callback);

    // ── Relay Mode (Good Citizen) ─────────────────────────────────────────

    // Start relaying update chunks to requesting peers
    bool StartRelay();

    // Stop relaying
    void StopRelay();

    // How many peers we're currently serving
    uint32_t RelayingToPeers() const;

    // Total bytes relayed in this session
    uint64_t BytesRelayed() const;

    // ── IPFS Distribution ─────────────────────────────────────────────────

    // Pin an update binary to IPFS (if IPFS node available)
    bool PinToIPFS(const AeonUpdateManifest& manifest);

    // Fetch an update from IPFS by CID
    bool FetchFromIPFS(const char* cid);

    // ── Diagnostics ───────────────────────────────────────────────────────

    // Get update history
    struct UpdateHistoryEntry {
        char     version[32];
        uint64_t applied_utc;
        bool     via_p2p;           // true = received from mesh, false = HTTPS
        uint32_t peers_served;      // How many peers we relayed this to
    };
    std::vector<UpdateHistoryEntry> UpdateHistory() const;

    // ── Resource Awareness ────────────────────────────────────────────────
    bool CanOffloadToHive() const override { return true; }

private:
    AeonP2PUpdateImpl* m_impl = nullptr;
};

// ── Global singleton ──────────────────────────────────────────────────────────
AeonP2PUpdate& AeonP2PUpdateInstance();

// ── Sovereign Update Signing ─────────────────────────────────────────────────
// The official DelgadoLogic P2P update signing public key.
// Only manifests signed by this key are accepted by production builds.
// Dev mode: if trusted_signer in AeonP2PUpdateConfig is all zeros,
//           any manifest signer is accepted (for testing only).
// Generated: 2026-04-12 by sovereign-keys/aeon_keygen.py
// SHA-256: 17514704c1f61cabb83c8c720df3dd0f0b598283135cd86797ae7e07709a2a0f
static const uint8_t AEON_UPDATE_SIGNER_PUBKEY[32] = {
    0x19, 0x07, 0xf3, 0x34, 0x3a, 0xab, 0xbb, 0xae,
    0xa4, 0xb2, 0xb3, 0x14, 0x8d, 0x36, 0x86, 0x94,
    0x60, 0x58, 0xc3, 0x96, 0x65, 0x0c, 0xcf, 0x3f,
    0x6b, 0x1d, 0xc7, 0xb0, 0xbb, 0x19, 0x53, 0xfa,
};
