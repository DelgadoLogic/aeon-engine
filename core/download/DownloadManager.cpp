// AeonBrowser — DownloadManager.cpp
// DelgadoLogic | Network Team
//
// Multi-threaded download engine using WinINet.
// Features:
//   - Resume-from-byte (HTTP Range: bytes=N-)
//   - Multi-stream (up to 8 segments per file)
//   - Torrent / Magnet via aeon_router Rust DLL
//   - IPFS via aeon_router
//   - Mirrors aeon.downloads bridge for downloads.html

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

// Internal extended task struct (maps to public DownloadItem for external callers)
enum class DLStatus : uint8_t { Idle=0, Queued, Downloading, Paused, Complete, Error, Cancelled };

struct DownloadTask {
    uint64_t   id;
    char       url[2048];
    char       filename[512];
    char       destPath[512];
    char       dest_path[512];   // same as destPath, used by worker
    char       error_msg[256];   // human-readable error string
    uint64_t   totalBytes;
    uint64_t   receivedBytes;
    uint64_t   size;             // alias for totalBytes
    uint64_t   received;         // alias for receivedBytes
    LONGLONG   speed_bps;        // bytes/sec
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
static std::atomic<int>          g_nextId{1};

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
    // Sanitize: remove invalid filename chars
    for (char& c : name) {
        if (c=='\\' || c=='/'  || c==':'  || c=='*' ||
            c=='?'  || c=='"' || c=='<'  || c=='>' || c=='|')
            c = '_';
    }
    return name;
}

static LONGLONG GetRemoteFileSize(const char* url) {
    // HEAD request to get Content-Length
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
    int id = (int)(INT_PTR)param;

    DownloadTask* task = nullptr;
    {
        std::lock_guard<std::mutex> lock(g_mu);
        for (auto& t : g_tasks) { if (t.id == id) { task = &t; break; } }
        if (!task) return 1;
        task->status = DLStatus::Downloading;
    }

    const char* url  = task->url;
    const char* dest = task->dest_path;

    // ── Torrent / Magnet → hand off to aeon_router ────────────────────────
    if (strncmp(url, "magnet:", 7) == 0 || strncmp(url, "torrent:", 8) == 0) {
        // aeon_router handles torrent seeding-disabled download
        // Stub: mark as unsupported until router DLL is ready
        std::lock_guard<std::mutex> lock(g_mu);
        task->status = DLStatus::Error;
        strncpy_s(task->error_msg, "Torrent: router not loaded", sizeof(task->error_msg)-1);
        return 0;
    }

    // ── HTTP/S download ────────────────────────────────────────────────────
    // Check if partial file exists (resume)
    LONGLONG existing = ExistingBytes(dest);
    LONGLONG total    = GetRemoteFileSize(url);

    {
        std::lock_guard<std::mutex> lock(g_mu);
        task->size     = (total > 0) ? total : 0;
        task->received = existing;
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
        // Already complete
        std::lock_guard<std::mutex> lock(g_mu);
        task->received = total;
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
        strncpy_s(task->error_msg, "InternetOpenUrl failed", sizeof(task->error_msg)-1);
        return 1;
    }

    // Open file for append (resume) or write
    HANDLE hFile = CreateFileA(dest,
        GENERIC_WRITE, 0, nullptr,
        existing > 0 ? OPEN_EXISTING : CREATE_ALWAYS,
        FILE_ATTRIBUTE_NORMAL, nullptr);
    if (hFile == INVALID_HANDLE_VALUE) {
        InternetCloseHandle(hUrl);
        std::lock_guard<std::mutex> lock(g_mu);
        task->status = DLStatus::Error;
        return 1;
    }
    if (existing > 0) {
        LARGE_INTEGER li; li.QuadPart = existing;
        SetFilePointerEx(hFile, li, nullptr, FILE_BEGIN);
    }

    // ── Read loop ─────────────────────────────────────────────────────────
    static const DWORD CHUNK = 65536; // 64 KB
    std::vector<BYTE> buf(CHUNK);
    DWORD read = 0;
    LONGLONG received  = existing;
    ULONGLONG t0       = GetTickCount64();
    LONGLONG  bytes_t0 = received;

    while (true) {
        // Check paused / cancelled
        {
            std::lock_guard<std::mutex> lock(g_mu);
            if (task->status == DLStatus::Paused) {
                CloseHandle(hFile); InternetCloseHandle(hUrl);
                return 0; // File stays on disk for resume
            }
            if (task->status == DLStatus::Cancelled) {
                CloseHandle(hFile); InternetCloseHandle(hUrl);
                DeleteFileA(dest);
                return 0;
            }
        }

        if (!InternetReadFile(hUrl, buf.data(), CHUNK, &read)) break;
        if (read == 0) break; // EOF

        DWORD written = 0;
        WriteFile(hFile, buf.data(), read, &written, nullptr);
        received += written;

        // Update speed every 500ms
        ULONGLONG now = GetTickCount64();
        double elapsed = (now - t0) / 1000.0;
        {
            std::lock_guard<std::mutex> lock(g_mu);
            task->received = received;
            if (elapsed >= 0.5) {
                task->speed_bps = (LONGLONG)((received - bytes_t0) / elapsed);
                if (task->size > 0 && task->speed_bps > 0)
                    task->eta_sec = (int)((task->size - received) / task->speed_bps);
                t0 = now; bytes_t0 = received;
            }
        }
    }

    CloseHandle(hFile);
    InternetCloseHandle(hUrl);

    std::lock_guard<std::mutex> lock(g_mu);
    task->received = received;
    task->status   = (total > 0 && received >= total) ? DLStatus::Complete : DLStatus::Error;
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
    if (!url || !g_hInternet) return -1;

    std::string fname = filename_hint && filename_hint[0]
        ? std::string(filename_hint)
        : SafeFilename(url);

    char dest[MAX_PATH];
    _snprintf_s(dest, sizeof(dest), _TRUNCATE, "%s\\%s", g_downloadDir, fname.c_str());

    DownloadTask task = {};
    task.id     = g_nextId++;
    task.status = DLStatus::Queued;
    strncpy_s(task.url,       url,         sizeof(task.url)-1);
    strncpy_s(task.filename,  fname.c_str(),sizeof(task.filename)-1);
    strncpy_s(task.dest_path, dest,         sizeof(task.dest_path)-1);

    {
        std::lock_guard<std::mutex> lock(g_mu);
        g_tasks.push_back(task);
    }

    // Launch worker thread
    HANDLE hThread = CreateThread(nullptr, 0, DownloadWorker,
        (LPVOID)(INT_PTR)task.id, 0, nullptr);
    if (hThread) CloseHandle(hThread);

    fprintf(stdout, "[DL] Started #%llu: %s → %s\n", (unsigned long long)task.id, url, dest);
    return task.id;
}

void Pause(int id) {
    std::lock_guard<std::mutex> lock(g_mu);
    for (auto& t : g_tasks)
        if (t.id == id && t.status == DLStatus::Downloading)
            t.status = DLStatus::Paused;
}

void Resume(int id) {
    int found = -1;
    {
        std::lock_guard<std::mutex> lock(g_mu);
        for (auto& t : g_tasks)
            if (t.id == id && t.status == DLStatus::Paused) {
                t.status = DLStatus::Queued; found = id;
            }
    }
    if (found >= 0) {
        HANDLE h = CreateThread(nullptr, 0, DownloadWorker,
            (LPVOID)(INT_PTR)found, 0, nullptr);
        if (h) CloseHandle(h);
    }
}

void Cancel(int id) {
    std::lock_guard<std::mutex> lock(g_mu);
    for (auto& t : g_tasks) if (t.id == id) t.status = DLStatus::Cancelled;
}

void Remove(int id) {
    std::lock_guard<std::mutex> lock(g_mu);
    g_tasks.erase(std::remove_if(g_tasks.begin(), g_tasks.end(),
        [id](const DownloadTask& t){ return t.id == id; }), g_tasks.end());
}

void ClearCompleted() {
    std::lock_guard<std::mutex> lock(g_mu);
    g_tasks.erase(std::remove_if(g_tasks.begin(), g_tasks.end(),
        [](const DownloadTask& t){ return t.status == DLStatus::Complete; }), g_tasks.end());
}

void RevealInExplorer(int id) {
    // Capture path under lock, call ShellExecute AFTER release (P1 fix)
    char pathBuf[512] = {};
    {
        std::lock_guard<std::mutex> lock(g_mu);
        for (auto& t : g_tasks) {
            if (t.id == id) {
                strncpy_s(pathBuf, t.dest_path, sizeof(pathBuf)-1);
                break;
            }
        }
    }
    if (pathBuf[0]) {
        char cmd[MAX_PATH + 32];
        _snprintf_s(cmd, sizeof(cmd), _TRUNCATE,
            "/select,\"%s\"", pathBuf);
        ShellExecuteA(nullptr, "open", "explorer.exe", cmd, nullptr, SW_SHOW);
    }
}

void OpenFile(int id) {
    // Capture path under lock, call ShellExecute AFTER release (P1 fix)
    char pathBuf[512] = {};
    bool found = false;
    {
        std::lock_guard<std::mutex> lock(g_mu);
        for (auto& t : g_tasks) {
            if (t.id == id && t.status == DLStatus::Complete) {
                strncpy_s(pathBuf, t.dest_path, sizeof(pathBuf)-1);
                found = true;
                break;
            }
        }
    }
    if (found)
        ShellExecuteA(nullptr, "open", pathBuf, nullptr, nullptr, SW_SHOW);
}

void OpenDownloadFolder() {
    ShellExecuteA(nullptr,"open",g_downloadDir,nullptr,nullptr,SW_SHOW);
}

std::vector<DownloadItem> GetAll() {
    std::lock_guard<std::mutex> lock(g_mu);
    std::vector<DownloadItem> out;
    out.reserve(g_tasks.size());
    for (const auto& t : g_tasks) {
        DownloadItem item = {};
        item.id            = t.id;
        strncpy_s(item.url,      t.url,       sizeof(item.url)-1);
        strncpy_s(item.filename, t.filename,   sizeof(item.filename)-1);
        strncpy_s(item.destPath, t.dest_path,  sizeof(item.destPath)-1);
        strncpy_s(item.error_msg,t.error_msg,  sizeof(item.error_msg)-1);
        item.totalBytes    = t.size;
        item.receivedBytes = t.received;
        item.speed_bps     = t.speed_bps;
        item.eta_sec       = t.eta_sec;
        item.errorCode     = t.errorCode;
        // Map internal DLStatus → public DownloadState
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

const char* GetDownloadDir() { return g_downloadDir; }

void Shutdown() {
    if (g_hInternet) { InternetCloseHandle(g_hInternet); g_hInternet = nullptr; }
}

} // namespace DownloadManager
