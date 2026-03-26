// AeonBrowser — AutoUpdater.h
#pragma once

namespace AutoUpdater {

    struct UpdateInfo {
        bool  update_available = false;
        char  version[32]      = {};
        char  download_url[512]= {};
        char  sha256[70]       = {};
    };

    // Check update server synchronously.
    bool CheckForUpdate(const char* channel, UpdateInfo* outInfo);

    // Check asynchronously (fire-and-forget background thread).
    void CheckAsync(const char* channel);

    // Long-running background poller — checks every 24h.
    void StartPoller();

    // Download installer to dest_path and verify Authenticode signature.
    bool DownloadAndVerify(const UpdateInfo& info, const char* dest_path);

    // Launch installer with UAC prompt.
    // Alias for immediate async check (used by AeonBridge)\n    inline void CheckNow(const char* channel) { CheckAsync(channel); }\n\n    // Zero-argument version used by AeonBridge
    void CheckNow();

    void LaunchInstaller(const char* installer_path);
}
