// AeonBrowser — DownloadManager.h
// DelgadoLogic | Download Team
#pragma once
#include <cstdint>
#include <cstddef>
#include <vector>

namespace DownloadManager {

    enum class DownloadState : uint8_t {
        Pending = 0,
        Active,
        Paused,
        Complete,
        Failed,
        Cancelled
    };

    struct DownloadItem {
        uint64_t     id;
        char         url[2048];
        char         filename[512];
        char         destPath[512];
        char         error_msg[256];
        uint64_t     totalBytes;
        uint64_t     receivedBytes;
        int64_t      speed_bps;
        int          eta_sec;
        DownloadState state;
        int          errorCode;    // 0 = no error
    };

    // Initialize with download directory (nullptr = default ~/Documents/Downloads/Aeon)
    bool Init(const char* downloadDir = nullptr);

    // Start a download; returns item ID (0 on failure)
    uint64_t StartDownload(const char* url, const char* suggestedFilename = nullptr);

    // Control — all accept int id to match .cpp implementation
    void Pause(int id);
    void Resume(int id);
    void Cancel(int id);
    void Remove(int id);

    // Query — returns full snapshot of all tasks
    std::vector<DownloadItem> GetAll();

    // Housekeeping
    void ClearCompleted();

    // Explorer integration
    void RevealInExplorer(int id);
    void OpenFile(int id);
    void OpenDownloadFolder();

    // Directory
    const char* GetDownloadDir();

    void Shutdown();

} // namespace DownloadManager
