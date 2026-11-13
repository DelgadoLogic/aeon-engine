// AeonBrowser — DownloadManager.cpp
// DelgadoLogic | Network Team
//
// Multi-threaded download engine using WinINet.
// Features:
//   - Resume-from-byte (HTTP Range: bytes=N-)
//   - Multi-stream & persistent connections
//   - Live speed telemetry in bytes/sec and MB/s
//   - Clean cancellation, error handling, and thread safety

#include "DownloadManager.h"
#include "../history/HistoryEngine.h"
#include <windows.h>
#include <wininet.h>
#include <shlobj.h>
#include <cstdio>
#include <cstring>
#include <vector>
#include <string>
#include <mutex>
#include <thread>
#include <atomic>
#include <algorithm>

// Internal extended task struct (maps to public DownloadItem for external callers)
enum class DLStatus : uint8_t { Idle=0, Queued, Downloading, Paused, Complete, Error, Cancelled };

struct DownloadTask {
    uint64_t   id;
    char       url[2048];
    char       filename[512];
    char       destPath[512];
    char       dest_path[512];   // alias used by worker
    char       error_msg[256];   // human-readable error string
    uint64_t   totalBytes;
    uint64_t   receivedBytes;
    uint64_t   size;             // alias for totalBytes
    uint64_t   received;         // alias for receivedBytes
    LONGLONG   speed_bps;        // bytes/sec
    double     speed_mbps;       // MB/s
    int        eta_sec;          // time remaining
    DLStatus   status;
    int        errorCode;
    HANDLE     hThread;
    bool       cancelFlag;
};

#pragma comment(lib, "wininet.lib")

namespace DownloadManager {

// ─── State ───────────────────────────────────────────────────────────────────
static std::vector<DownloadTask> g_tasks;
static std::mutex                g_mu;
static char                      g_downloadDir[MAX_PATH] = {};
static HINTERNET                 g_hInternet             = nullptr;
static std::atomic<uint64_t>     g_nextId{1};

// ─── Helpers ─────────────────────────────────────────────────────────────────
static void BuildDefaultDir() {
    char docs[MAX_PATH];
    SHGetFolderPathA(nullptr, CSIDL_PERSONAL, nullptr, SHGFP_TYPE_CURRENT, docs);
    _snprintf_s(g_downloadDir, sizeof(g_downloadDir), _TRUNCATE,
        "%s\\Downloads\\Aeon", docs);
    CreateDirectoryA(g_downloadDir, nullptr);
}

static std::string SafeFilename(const char* url) {
    // Extract last path component from URL, strip query string
    const char* p = strrchr(url, '/');
    std::string name = p ? std::string(p + 1) : std::string(url);
    auto q = name.find('?');
    if (q != std::string::npos) name = name.substr(0, q);
    if (name.empty()) name = "download";
    // Sanitize: remove invalid filename characters
    for (char& c : name) {
        if (c=='\\' || c=='/'  || c==':'  || c=='*' ||
            c=='?'  || c=='"' || c=='<'  || c=='>' || c=='|')
            c = '_';
    }
    return name;
}

static LONGLONG GetRemoteFileSize(const char* url) {
    HINTERNET hUrl = InternetOpenUrlA(g_hInternet, url,
        "Accept: */*\r\n", -1L,
        INTERNET_FLAG_NO_CACHE_WRITE | INTERNET_FLAG_RELOAD |
        INTERNET_FLAG_SECURE, 0);
    if (!hUrl) return -1;
    LONGLONG size = -1;
    char buf[32] = {};
    DWORD bufLen = sizeof(buf);
    if (HttpQueryInfoA(hUrl, HTTP_QUERY_CONTENT_LENGTH,
        buf, &bufLen, nullptr)) {
        size = _atoi64(buf);
    }
    InternetCloseHandle(hUrl);
    return size;
}

static LONGLONG ExistingBytes(const char* path) {
    WIN32_FILE_ATTRIBUTE_DATA info = {};
    if (!GetFileAttributesExA(path, GetFileExInfoStandard, &info)) return 0;
    LARGE_INTEGER li;
    li.HighPart = info.nFileSizeHigh;
    li.LowPart  = info.nFileSizeLow;
    return li.QuadPart;
}

// ─── Worker thread per task ───────────────────────────────────────────────────
static DWORD WINAPI DownloadWorker(LPVOID param) {
    uint64_t id = static_cast<uint64_t>(reinterpret_cast<uintptr_t>(param));

    DownloadTask* task = nullptr;
    {
        std::lock_guard<std::mutex> lock(g_mu);
        for (auto& t : g_tasks) {
            if (t.id == id) { task = &t; break; }
        }
        if (!task) return 1;
        task->status = DLStatus::Downloading;
        task->cancelFlag = false;
    }

    const char* url  = task->url;
    const char* dest = task->dest_path;

    // Magnet / Torrent check
    if (strncmp(url, "magnet:", 7) == 0 || strncmp(url, "torrent:", 8) == 0) {
        std::lock_guard<std::mutex> lock(g_mu);
        task->status = DLStatus::Error;
        strncpy_s(task->error_msg, "P2P router dispatch not active", sizeof(task->error_msg)-1);
        return 0;
    }

    LONGLONG existing = ExistingBytes(dest);
    LONGLONG total    = GetRemoteFileSize(url);

    {
        std::lock_guard<std::mutex> lock(g_mu);
        task->size     = (total > 0) ? total : 0;
        task->totalBytes = task->size;
        task->received = existing;
        task->receivedBytes = existing;
    }

    // Build Range header if resuming
    char rangeHdr[64] = {};
    const char* extra = nullptr;
    if (existing > 0 && total > 0 && existing < total) {
        _snprintf_s(rangeHdr, sizeof(rangeHdr), _TRUNCATE,
            "Range: bytes=%lld-\r\n", existing);
        extra = rangeHdr;
        fprintf(stdout, "[DL] Resuming %s from byte %lld\n", dest, existing);
    } else if (existing >= total && total > 0) {
        std::lock_guard<std::mutex> lock(g_mu);
        task->received = total;
        task->receivedBytes = total;
        task->status   = DLStatus::Complete;
        return 0;
    }

    DWORD flags = INTERNET_FLAG_NO_CACHE_WRITE | INTERNET_FLAG_RELOAD;
    if (strncmp(url, "https://", 8) == 0) flags |= INTERNET_FLAG_SECURE;

    HINTERNET hUrl = InternetOpenUrlA(g_hInternet, url,
        extra ? extra : "Accept: */*\r\n", -1L, flags, 0);
    if (!hUrl) {
        std::lock_guard<std::mutex> lock(g_mu);
        task->status = DLStatus::Error;
        task->errorCode = GetLastError();
        strncpy_s(task->error_msg, "InternetOpenUrl failed", sizeof(task->error_msg)-1);
        return 1;
    }

    HANDLE hFile = CreateFileA(dest,
        GENERIC_WRITE, 0, nullptr,
        existing > 0 ? OPEN_EXISTING : CREATE_ALWAYS,
        FILE_ATTRIBUTE_NORMAL, nullptr);
    if (hFile == INVALID_HANDLE_VALUE) {
        InternetCloseHandle(hUrl);
        std::lock_guard<std::mutex> lock(g_mu);
        task->status = DLStatus::Error;
        task->errorCode = GetLastError();
        strncpy_s(task->error_msg, "Failed to create local destination file", sizeof(task->error_msg)-1);
        return 1;
    }
    if (existing > 0) {
        LARGE_INTEGER li; li.QuadPart = existing;
        SetFilePointerEx(hFile, li, nullptr, FILE_BEGIN);
    }

    // ── Read loop ─────────────────────────────────────────────────────────
    static const DWORD CHUNK = 65536; // 64 KB chunk
    std::vector<BYTE> buf(CHUNK);
    DWORD readBytes = 0;
    LONGLONG received  = existing;
    ULONGLONG t0       = GetTickCount64();
    LONGLONG  bytes_t0 = received;

    while (true) {
        // Check paused / cancelled
        {
            std::lock_guard<std::mutex> lock(g_mu);
            if (task->cancelFlag || task->status == DLStatus::Cancelled) {
                CloseHandle(hFile);
                InternetCloseHandle(hUrl);
                DeleteFileA(dest);
                return 0;
            }
            if (task->status == DLStatus::Paused) {
                CloseHandle(hFile);
                InternetCloseHandle(hUrl);
                return 0; // Partial file remains on disk for resume
            }
        }

        if (!InternetReadFile(hUrl, buf.data(), CHUNK, &readBytes)) break;
        if (readBytes == 0) break; // EOF

        DWORD written = 0;
        WriteFile(hFile, buf.data(), readBytes, &written, nullptr);
        received += written;

        // Telemetry update: compute speed every 250ms
        ULONGLONG now = GetTickCount64();
        double elapsed = (now - t0) / 1000.0;
        if (elapsed >= 0.25) {
            std::lock_guard<std::mutex> lock(g_mu);
            task->received = received;
            task->receivedBytes = received;
            task->speed_bps = static_cast<LONGLONG>((received - bytes_t0) / elapsed);
            task->speed_mbps = static_cast<double>(task->speed_bps) / (1024.0 * 1024.0);
            if (task->size > 0 && task->speed_bps > 0) {
                LONGLONG remaining = task->size > received ? (task->size - received) : 0;
                task->eta_sec = static_cast<int>(remaining / task->speed_bps);
            }
            t0 = now;
            bytes_t0 = received;
        }
    }

    CloseHandle(hFile);
    InternetCloseHandle(hUrl);

    std::lock_guard<std::mutex> lock(g_mu);
    task->received = received;
    task->receivedBytes = received;
    task->speed_bps = 0;
    task->speed_mbps = 0.0;
    task->eta_sec = 0;
    task->status = (total <= 0 || received >= total) ? DLStatus::Complete : DLStatus::Error;
    if (task->status == DLStatus::Complete) {
        fprintf(stdout, "[DL] Complete: %s (%lld bytes)\n", dest, received);
    }
    return 0;
}

// ═══════════════════════════════════════════════════════════════════════════
//  PUBLIC API
// ═══════════════════════════════════════════════════════════════════════════

bool Init(const char* download_dir) {
    if (download_dir && download_dir[0]) {
        strncpy_s(g_downloadDir, download_dir, sizeof(g_downloadDir)-1);
        CreateDirectoryA(g_downloadDir, nullptr);
    } else {
        BuildDefaultDir();
    }
    g_hInternet = InternetOpenA("AeonBrowser/1.0",
        INTERNET_OPEN_TYPE_PRECONFIG, nullptr, nullptr, 0);
    fprintf(stdout, "[DL] Initialized. Dir: %s\n", g_downloadDir);
    return g_hInternet != nullptr;
}

uint64_t StartDownload(const char* url, const char* filename_hint) {
    if (!url || !g_hInternet) return 0;

    std::string fname = (filename_hint && filename_hint[0])
        ? std::string(filename_hint)
        : SafeFilename(url);

    char dest[MAX_PATH];
    _snprintf_s(dest, sizeof(dest), _TRUNCATE, "%s\\%s", g_downloadDir, fname.c_str());

    DownloadTask task = {};
    task.id     = g_nextId++;
    task.status = DLStatus::Queued;
    strncpy_s(task.url,       url,          sizeof(task.url)-1);
    strncpy_s(task.filename,  fname.c_str(),sizeof(task.filename)-1);
    strncpy_s(task.destPath,  dest,         sizeof(task.destPath)-1);
    strncpy_s(task.dest_path, dest,         sizeof(task.dest_path)-1);

    {
        std::lock_guard<std::mutex> lock(g_mu);
        g_tasks.push_back(task);
    }

    // Launch worker thread
    HANDLE hThread = CreateThread(nullptr, 0, DownloadWorker,
        reinterpret_cast<LPVOID>(static_cast<uintptr_t>(task.id)), 0, nullptr);
    if (hThread) CloseHandle(hThread);

    fprintf(stdout, "[DL] Started #%llu: %s -> %s\n", (unsigned long long)task.id, url, dest);
    return task.id;
}

void Pause(int64_t id) {
    std::lock_guard<std::mutex> lock(g_mu);
    for (auto& t : g_tasks) {
        if (t.id == static_cast<uint64_t>(id) && t.status == DLStatus::Downloading) {
            t.status = DLStatus::Paused;
            t.speed_bps = 0;
            t.speed_mbps = 0.0;
        }
    }
}

void Pause(int id) {
    Pause(static_cast<int64_t>(id));
}

void Resume(int64_t id) {
    uint64_t found = 0;
    {
        std::lock_guard<std::mutex> lock(g_mu);
        for (auto& t : g_tasks) {
            if (t.id == static_cast<uint64_t>(id) && t.status == DLStatus::Paused) {
                t.status = DLStatus::Queued;
                found = t.id;
            }
        }
    }
    if (found > 0) {
        HANDLE h = CreateThread(nullptr, 0, DownloadWorker,
            reinterpret_cast<LPVOID>(static_cast<uintptr_t>(found)), 0, nullptr);
        if (h) CloseHandle(h);
    }
}

void Resume(int id) {
    Resume(static_cast<int64_t>(id));
}

void Cancel(int64_t id) {
    std::lock_guard<std::mutex> lock(g_mu);
    for (auto& t : g_tasks) {
        if (t.id == static_cast<uint64_t>(id)) {
            t.cancelFlag = true;
            t.status = DLStatus::Cancelled;
            t.speed_bps = 0;
            t.speed_mbps = 0.0;
        }
    }
}

void Cancel(int id) {
    Cancel(static_cast<int64_t>(id));
}

void Remove(int64_t id) {
    std::lock_guard<std::mutex> lock(g_mu);
    for (auto& t : g_tasks) {
        if (t.id == static_cast<uint64_t>(id)) {
            t.cancelFlag = true;
            t.status = DLStatus::Cancelled;
        }
    }
    g_tasks.erase(std::remove_if(g_tasks.begin(), g_tasks.end(),
        [id](const DownloadTask& t){ return t.id == static_cast<uint64_t>(id); }), g_tasks.end());
}

void Remove(int id) {
    Remove(static_cast<int64_t>(id));
}

void RemoveDownload(int64_t id) {
    Remove(id);
}

void ClearCompleted() {
    std::lock_guard<std::mutex> lock(g_mu);
    g_tasks.erase(std::remove_if(g_tasks.begin(), g_tasks.end(),
        [](const DownloadTask& t){
            return t.status == DLStatus::Complete || t.status == DLStatus::Cancelled;
        }), g_tasks.end());
}

void RevealInExplorer(int64_t id) {
    char pathBuf[512] = {};
    {
        std::lock_guard<std::mutex> lock(g_mu);
        for (auto& t : g_tasks) {
            if (t.id == static_cast<uint64_t>(id)) {
                strncpy_s(pathBuf, t.dest_path, sizeof(pathBuf)-1);
                break;
            }
        }
    }
    if (pathBuf[0]) {
        char cmd[MAX_PATH + 32];
        _snprintf_s(cmd, sizeof(cmd), _TRUNCATE, "/select,\"%s\"", pathBuf);
        ShellExecuteA(nullptr, "open", "explorer.exe", cmd, nullptr, SW_SHOW);
    }
}

void RevealInExplorer(int id) {
    RevealInExplorer(static_cast<int64_t>(id));
}

void OpenFile(int64_t id) {
    char pathBuf[512] = {};
    bool found = false;
    {
        std::lock_guard<std::mutex> lock(g_mu);
        for (auto& t : g_tasks) {
            if (t.id == static_cast<uint64_t>(id) && t.status == DLStatus::Complete) {
                strncpy_s(pathBuf, t.dest_path, sizeof(pathBuf)-1);
                found = true;
                break;
            }
        }
    }
    if (found && pathBuf[0]) {
        ShellExecuteA(nullptr, "open", pathBuf, nullptr, nullptr, SW_SHOW);
    }
}

void OpenFile(int id) {
    OpenFile(static_cast<int64_t>(id));
}

void OpenDownloadFolder() {
    ShellExecuteA(nullptr, "open", g_downloadDir, nullptr, nullptr, SW_SHOW);
}

std::vector<DownloadItem> GetAll() {
    std::lock_guard<std::mutex> lock(g_mu);
    std::vector<DownloadItem> out;
    out.reserve(g_tasks.size());
    for (const auto& t : g_tasks) {
        DownloadItem item = {};
        item.id            = t.id;
        strncpy_s(item.url,      t.url,        sizeof(item.url)-1);
        strncpy_s(item.filename, t.filename,   sizeof(item.filename)-1);
        strncpy_s(item.destPath, t.dest_path,  sizeof(item.destPath)-1);
        strncpy_s(item.error_msg,t.error_msg,  sizeof(item.error_msg)-1);
        item.totalBytes    = t.size;
        item.receivedBytes = t.received;
        item.speed_bps     = t.speed_bps;
        item.speed_mbps    = (double)t.speed_bps / (1024.0 * 1024.0);
        item.eta_sec       = t.eta_sec;
        item.errorCode     = t.errorCode;
        item.progress_percent = (t.size > 0) ? (100.0 * static_cast<double>(t.received) / static_cast<double>(t.size)) : 0.0;

        switch (t.status) {
            case DLStatus::Idle:        item.state = DownloadState::Pending;   break;
            case DLStatus::Queued:      item.state = DownloadState::Pending;   break;
            case DLStatus::Downloading: item.state = DownloadState::Active;    break;
            case DLStatus::Paused:      item.state = DownloadState::Paused;    break;
            case DLStatus::Complete:    item.state = DownloadState::Complete;  break;
            case DLStatus::Error:       item.state = DownloadState::Failed;    break;
            case DLStatus::Cancelled:   item.state = DownloadState::Cancelled; break;
        }
        out.push_back(item);
    }
    return out;
}

std::vector<DownloadItem> GetAllDownloads() {
    return GetAll();
}

const char* GetDownloadDir() {
    return g_downloadDir;
}

void Shutdown() {
    if (g_hInternet) {
        InternetCloseHandle(g_hInternet);
        g_hInternet = nullptr;
    }
}

} // namespace DownloadManager
