// AeonBrowser — AutoUpdater.h
#pragma once

namespace AutoUpdater {

    struct UpdateInfo {
        bool  update_available = false;
        char  version[32]      = {};
        char  download_url[512]= {};
        char  sha256[70]       = {};  // SHA-256 hex string
    };

    // Check update server. Returns true if update available.
    bool CheckForUpdate(const char* tier, UpdateInfo* outInfo);

    // Download installer to dest_path and verify Authenticode signature.
    bool DownloadAndVerify(const UpdateInfo& info, const char* dest_path);

    // Launch installer with UAC prompt.
    void LaunchInstaller(const char* installer_path);
}
