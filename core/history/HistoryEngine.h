// AeonBrowser — HistoryEngine.h
// DelgadoLogic | Data Team
#pragma once
#include <cstdint>
#include <vector>
#include <string>

namespace HistoryEngine {

    // ── Visit History ─────────────────────────────────────────────────────────
    struct HistoryEntry {
        char     url[2048];
        char     title[256];
        uint64_t visitTime;     // Unix milliseconds
        int      visitCount;
    };

    struct Bookmark {
        char     url[2048];
        char     title[256];
        char     folder[128];   // "Bookmarks" = root
        uint64_t added_time;    // Unix milliseconds
        uint64_t id;            // SQLite rowid
        char     favicon_b64[4096];
        uint32_t sort_order;
    };

    struct ReadingEntry {
        char     url[2048];
        char     title[256];
        uint64_t saved_time;
        bool     read;
    };

    // Initialize — opens/creates SQLite DB at dbPath
    bool Init(const char* dbPath, bool incognito = false);

    // Record a page visit
    void RecordVisit(const char* url, const char* title,
                     const char* favicon_b64 = nullptr);

    // Retrieve recent history (returns vector, limit 0 = default 100)
    std::vector<HistoryEntry> GetRecent(int limit = 100);

    // Full-text search
    std::vector<HistoryEntry> Search(const char* query);

    // Delete a single entry by URL
    void DeleteEntry(const char* url);

    // Clear all history
    void WipeAll();

    bool IsPrivate();

    // ── Bookmark API ──────────────────────────────────────────────────────────
    bool AddBookmark(const char* url, const char* title, const char* folder);
    void DeleteBookmark(uint64_t id);
    void DeleteBookmark(const char* url);   // overload for URL-based delete
    std::vector<Bookmark> GetBookmarks();
    bool IsBookmarked(const char* url);

    // ── Array-based wrappers for C callers ───────────────────────────────────
    inline int GetBookmarks(Bookmark* out, int maxCount) {
        auto v = GetBookmarks();
        int n = (int)v.size() < maxCount ? (int)v.size() : maxCount;
        for (int i = 0; i < n; i++) out[i] = v[i];
        return n;
    }

    // ── Reading List ──────────────────────────────────────────────────────────
    void AddToReadingList(const char* url, const char* title);
    void MarkRead(const char* url);
    std::vector<ReadingEntry> GetReadingList();

    // Shutdown / flush
    void Shutdown();

} // namespace HistoryEngine
