// =============================================================================
// aeon_offline_installer.h — AeonOfflineInstaller: Internet-Free Bootstrap
// Graduated from: SHAREit/Xender distribution concepts (clean-room)
//
// The Offline-First Installer enables Aeon Browser to be distributed and
// installed on devices that may never have internet access. This is critical
// for emerging markets where:
//
//   - Users share APKs via SHAREit, Xender, Bluetooth, USB drives
//   - Internet cafes distribute software via LAN or USB sticks
//   - NGOs deploy browsers on tablets/phones in schools without WiFi
//   - Feature phones with SD card slots receive APKs via card swap
//
// The installer package is a self-contained archive that includes:
//   1. The Aeon Browser binary (platform-specific)
//   2. A minimal trust store bundle (sovereign CA roots)
//   3. AeonHive bootstrap peers (hardcoded local discovery endpoints)
//   4. Offline content pack (Wikipedia Vital Articles, emergency info)
//   5. Ed25519 signature for integrity (signed by AEON_UPDATE_SIGNER_PUBKEY)
//
// Package format: .aeon (custom archive)
//   [8 bytes]  Magic:     "AEONPKG\x00"
//   [4 bytes]  Version:   Package format version (uint32 LE)
//   [4 bytes]  Platform:  AeonPlatform enum
//   [32 bytes] Signature: Ed25519 signature of payload
//   [32 bytes] PubKey:    Signer public key
//   [64 bytes] Reserved:  Future use
//   [N bytes]  Payload:   Compressed archive (zstd or deflate)
//
// What we improve over .apk sideloading:
//   [+] Self-verifying: Ed25519 signature prevents tampering
//   [+] Cross-platform: Same format for Android, Windows, Linux
//   [+] Includes trust store: Works even if device CAs are expired
//   [+] Includes offline content: Useful even without internet
//   [+] Minimal size: Target <15MB for Android, <25MB for Windows
//   [+] Auto-update ready: Registers with AeonP2PUpdate on first mesh contact
// =============================================================================

#pragma once

#include "aeon_component.h"
#include "../updater/aeon_p2p_update.h"
#include <cstdint>
#include <cstddef>
#include <functional>
#include <string>
#include <vector>

// Forward declarations
class AeonOfflineInstallerImpl;

// ---------------------------------------------------------------------------
// Package magic and version
// ---------------------------------------------------------------------------
static constexpr uint8_t  AEON_PKG_MAGIC[8] = {'A','E','O','N','P','K','G','\0'};
static constexpr uint32_t AEON_PKG_VERSION   = 1;

// ---------------------------------------------------------------------------
// AeonPackageHeader — binary header for .aeon package files
// ---------------------------------------------------------------------------
#pragma pack(push, 1)
struct AeonPackageHeader {
    uint8_t  magic[8];              // "AEONPKG\0"
    uint32_t version;               // Package format version
    uint32_t platform;              // AeonPlatform enum
    uint8_t  signature[64];         // Ed25519 signature of payload
    uint32_t signer_key_index;      // Index into trusted key ring (0 = primary)
    uint8_t  signer_pubkey[32];     // Signer's public key for verification
    uint32_t payload_size;          // Size of compressed payload in bytes
    uint32_t payload_hash[8];       // SHA-256 of uncompressed payload
    uint32_t compression;           // 0 = none, 1 = deflate, 2 = zstd
    uint8_t  browser_version[16];   // Version string (null-terminated)
    uint8_t  reserved[28];          // Future use (zero-filled)
};
#pragma pack(pop)

static_assert(sizeof(AeonPackageHeader) == 192,
    "AeonPackageHeader must be exactly 192 bytes");

// ---------------------------------------------------------------------------
// AeonPackageManifest — what's inside the package
// ---------------------------------------------------------------------------
struct AeonPackageManifest {
    // Browser binary
    char     binary_path[256];      // Path to main executable inside archive
    uint64_t binary_size;           // Uncompressed size of browser binary
    uint8_t  binary_sha256[32];     // SHA-256 of browser binary

    // Trust store
    char     trust_bundle_path[256];// Path to trust bundle inside archive
    uint32_t trust_bundle_version;  // Trust bundle version number
    uint32_t trust_anchor_count;    // Number of CA roots in bundle

    // Offline content
    uint32_t offline_content_count; // Number of offline content items
    uint64_t offline_content_size;  // Total size of offline content

    // Bootstrap peers
    uint32_t bootstrap_peer_count;  // Number of hardcoded bootstrap peers
};

// ---------------------------------------------------------------------------
// AeonOfflineContent — a single offline content item
// ---------------------------------------------------------------------------
struct AeonOfflineContent {
    char     title[128];            // Human-readable title
    char     url[512];              // Original URL (for cache key)
    char     path_in_archive[256];  // Path inside .aeon archive
    char     mime_type[32];         // MIME type (text/html, etc.)
    char     language[8];           // ISO 639-1 language code
    uint64_t size_bytes;            // Uncompressed size
    uint8_t  sha256[32];           // SHA-256 hash
};

// ---------------------------------------------------------------------------
// AeonBootstrapPeer — hardcoded peer for initial mesh discovery
// ---------------------------------------------------------------------------
struct AeonBootstrapPeer {
    char     hostname[128];         // DNS name or IP
    uint16_t port;                  // Port number
    uint8_t  pubkey[32];            // Peer's Ed25519 public key
    bool     is_dns_seed;           // True = DNS seed, false = direct IP
};

// ---------------------------------------------------------------------------
// Installation status
// ---------------------------------------------------------------------------
enum class AeonInstallStatus : uint8_t {
    NotStarted      = 0,
    Verifying       = 1,            // Checking Ed25519 signature
    Extracting      = 2,            // Decompressing payload
    Installing      = 3,            // Platform-specific install
    ConfiguringTrust = 4,           // Installing trust store
    LoadingContent  = 5,            // Loading offline content cache
    ConnectingMesh  = 6,            // Trying to join AeonHive mesh
    Complete        = 7,
    Failed          = 0xFF,
};

// ---------------------------------------------------------------------------
// Installation progress
// ---------------------------------------------------------------------------
struct AeonInstallProgress {
    AeonInstallStatus status;
    float             percent;      // 0.0 - 100.0
    char              step_name[64]; // Human-readable step name
    char              error[256];   // Error message (if Failed)
    uint64_t          bytes_written; // Bytes written to disk
    uint64_t          bytes_total;  // Total bytes to write
};

// ---------------------------------------------------------------------------
// Callbacks
// ---------------------------------------------------------------------------
using AeonInstallProgressCallback = std::function<void(
    const AeonInstallProgress& progress)>;

using AeonInstallCompleteCallback = std::function<void(
    bool success, const char* error)>;

// ---------------------------------------------------------------------------
// AeonOfflineInstaller — Internet-Free Bootstrap Engine
// ---------------------------------------------------------------------------
class AeonOfflineInstaller : public IAeonComponent {
public:
    AeonOfflineInstaller();
    ~AeonOfflineInstaller() override;

    // Non-copyable
    AeonOfflineInstaller(const AeonOfflineInstaller&) = delete;
    AeonOfflineInstaller& operator=(const AeonOfflineInstaller&) = delete;

    // ── IAeonComponent ───────────────────────────────────────────────────

    const char* Name() const override { return "AeonOfflineInstaller"; }
    const char* Version() const override { return "1.0.0"; }
    bool Initialize() override;
    void Shutdown() override;

    // ── Package Building (developer tool) ────────────────────────────────

    // Build a .aeon package from source files
    // This is a build-time tool, not used at runtime on user devices
    struct BuildConfig {
        const char*   browser_binary_path;    // Path to compiled browser binary
        const char*   trust_bundle_path;      // Path to trust bundle
        const char*   offline_content_dir;    // Directory of offline content
        const char*   signing_key_path;       // Path to Ed25519 private key
        AeonPlatform  target_platform;        // Target platform
        const char*   browser_version;        // Version string
        const char*   output_path;            // Output .aeon file path
        uint32_t      compression;            // 0=none, 1=deflate, 2=zstd
    };

    bool BuildPackage(const BuildConfig& cfg);

    // ── Package Verification ─────────────────────────────────────────────

    // Verify a .aeon package without installing it
    // Returns true if signature is valid and contents are intact
    bool VerifyPackage(const char* package_path);

    // Read package header without full verification
    bool ReadPackageHeader(const char* package_path, AeonPackageHeader& header);

    // Read package manifest (requires decompression)
    bool ReadPackageManifest(const char* package_path, AeonPackageManifest& manifest);

    // ── Installation ─────────────────────────────────────────────────────

    // Install from a .aeon package file
    // install_dir: Where to extract the browser (platform-specific default if null)
    bool InstallFromPackage(
        const char* package_path,
        const char* install_dir = nullptr);

    // Install from raw bytes (for receiving via Bluetooth/USB/AeonHive)
    bool InstallFromMemory(
        const uint8_t* data, size_t len,
        const char* install_dir = nullptr);

    // Get current installation progress
    AeonInstallProgress GetProgress() const;

    // Abort installation
    void AbortInstall();

    // ── Offline Content ──────────────────────────────────────────────────

    // List offline content available in an installed package
    std::vector<AeonOfflineContent> ListOfflineContent() const;

    // Get offline content by URL (returns the cached version)
    // If the URL matches an offline content item, returns the cached HTML
    std::vector<uint8_t> GetOfflineContent(const char* url) const;

    // Check if a URL has offline content available
    bool HasOfflineContent(const char* url) const;

    // ── Bootstrap Peers ──────────────────────────────────────────────────

    // Get the list of hardcoded bootstrap peers
    std::vector<AeonBootstrapPeer> GetBootstrapPeers() const;

    // Try to connect to bootstrap peers (attempt mesh join)
    bool TryBootstrapMesh();

    // ── Package Distribution ─────────────────────────────────────────────

    // Export the currently installed browser as a .aeon package
    // This allows users to share their browser installation with others
    bool ExportAsPackage(const char* output_path);

    // Get the expected package size for a given platform
    uint64_t EstimatePackageSize(AeonPlatform platform) const;

    // ── Callbacks ────────────────────────────────────────────────────────

    void OnProgress(AeonInstallProgressCallback cb);
    void OnComplete(AeonInstallCompleteCallback cb);

    // ── Diagnostics ─────────────────────────────────────────────────────

    std::string DiagnosticReport() const;

    // ── Resource Awareness ────────────────────────────────────────────────
    bool CanOffloadToHive() const override { return false; }

private:
    AeonOfflineInstallerImpl* m_impl = nullptr;
};

// ── Global singleton ──────────────────────────────────────────────────────────
AeonOfflineInstaller& AeonOfflineInstallerInstance();
