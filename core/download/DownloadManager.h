// AeonBrowser — DownloadManager.h
// DelgadoLogic | Download Team
#pragma once
#include <cstdint>
#include <cstddef>

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
        uint64_t     totalBytes;
        uint64_t     receivedBytes;
        DownloadState state;
        int          errorCode;    // 0 = no error
    };

    // Initialize with download directory
    bool Init(const char* downloadDir);

    // Start a download; returns item ID (0 on failure)
    uint64_t StartDownload(const char* url, const char* suggestedFilename);

    // Control
    bool Pause(uint64_t id);
    bool Resume(uint64_t id);
    bool Cancel(uint64_t id);

    // Query
    int  GetAll(DownloadItem* out, int maxCount);
    bool GetItem(uint64_t id, DownloadItem* out);

    // Housekeeping
    void ClearCompleted();

    // Open folder in Explorer
    void ShowInFolder(uint64_t id);
    void OpenDownloadFolder();

    void Shutdown();

} // namespace DownloadManager
