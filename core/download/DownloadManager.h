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
        uint64_t      id;
        char          url[2048];
        char          filename[512];
        char          destPath[512];
        char          error_msg[256];
        uint64_t      totalBytes;
        uint64_t      receivedBytes;
        int64_t       speed_bps;         // bytes per second
        double        speed_mbps;        // megabytes per second (speed_bps / (1024 * 1024))
        int           eta_sec;           // estimated seconds remaining
        DownloadState state;
        int           errorCode;         // 0 = no error
        double        progress_percent;  // 0.0 to 100.0
    };

    // Initialize with download directory (nullptr = default ~/Documents/Downloads/Aeon)
    bool Init(const char* downloadDir = nullptr);

    // Start a download; returns item ID (0 on failure)
    uint64_t StartDownload(const char* url, const char* suggestedFilename = nullptr);

    // Control — both int and int64_t signatures + uint64_t helper
    void Pause(int id);
    void Resume(int id);
    void Cancel(int id);
    void Remove(int id);

    void Pause(int64_t id);
    void Resume(int64_t id);
    void Cancel(int64_t id);
    void Remove(int64_t id);
    void RemoveDownload(int64_t id);

    inline void Pause(uint64_t id)          { Pause(static_cast<int64_t>(id)); }
    inline void Resume(uint64_t id)         { Resume(static_cast<int64_t>(id)); }
    inline void Cancel(uint64_t id)         { Cancel(static_cast<int64_t>(id)); }
    inline void Remove(uint64_t id)         { Remove(static_cast<int64_t>(id)); }
    inline void RemoveDownload(uint64_t id) { RemoveDownload(static_cast<int64_t>(id)); }

    // Query — returns full snapshot of all tasks with complete telemetry
    std::vector<DownloadItem> GetAll();
    std::vector<DownloadItem> GetAllDownloads();

    // Housekeeping
    void ClearCompleted();

    // Explorer integration
    void RevealInExplorer(int id);
    void OpenFile(int id);
    void RevealInExplorer(int64_t id);
    void OpenFile(int64_t id);

    inline void RevealInExplorer(uint64_t id) { RevealInExplorer(static_cast<int64_t>(id)); }
    inline void OpenFile(uint64_t id)         { OpenFile(static_cast<int64_t>(id)); }

    void OpenDownloadFolder();

    // Directory
    const char* GetDownloadDir();

    void Shutdown();

} // namespace DownloadManager
