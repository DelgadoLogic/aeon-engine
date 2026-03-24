// AeonBrowser — HistoryEngine.h
// DelgadoLogic | Lead Systems Architect
//
// PURPOSE: Bookmark and browsing history engine backed by SQLite (amalgamation).
// We ship sqlite3.c in-tree so there is zero system dependency.
// Database: %APPDATA%\DelgadoLogic\Aeon\history.db
//
// SCHEMA:
//   visits  (id, url, title, timestamp, visit_count)
//   bookmarks (id, url, title, folder, favicon_b64, created_at, modified_at)
//   bookmark_folders (id, name, parent_id, sort_order)
//   form_history (id, field_name, value, use_count, last_used)       [optional]
//
// PRIVACY: History is stored only locally. Never transmitted.
//   Clear-on-exit is an option in SettingsEngine.
//   Private/Incognito mode uses an in-memory SQLite database (no disk write).
//
// IT TROUBLESHOOTING:
//   - DB locked: Another Aeon instance may have it open. Only one instance
//     accesses the DB at a time via write_lock.
//   - History file corrupted: Delete history.db. Aeon recreates schema on start.
//   - Bookmarks missing after reinstall: DB is in %APPDATA%, not install dir.
//     It survives uninstall (UninstallDelete in .iss does NOT touch %APPDATA%).

#pragma once
#include <windows.h>
#include <cstdint>
#include <cstddef>

struct HistoryEntry {
    int64_t id;
    char    url[512];
    char    title[256];
    int64_t timestamp;     // Unix UTC epoch seconds
    int     visit_count;
};

struct BookmarkEntry {
    int64_t id;
    char    url[512];
    char    title[256];
    char    folder[128];
    int64_t created_at;
    int64_t modified_at;
};

namespace HistoryEngine {

    // Open (or create) the SQLite database. Call once at startup.
    // Pass private_mode = true for in-memory DB (incognito / private window).
    bool Initialize(bool private_mode = false);

    // Close the database. Call at shutdown.
    void Shutdown();

    // Record a page visit. Auto-increments visit_count if URL seen before.
    void RecordVisit(const char* url, const char* title);

    // Query history by partial URL or title. Returns count of results written
    // into out array (up to max_results).
    int  SearchHistory(const char* query,
                       HistoryEntry* out, int max_results);

    // Delete all history entries.
    void ClearHistory();

    // Delete a single history entry by ID.
    void DeleteEntry(int64_t id);

    // --- Bookmarks ---
    int64_t AddBookmark(const char* url, const char* title,
                        const char* folder = "Bookmarks bar");

    bool    RemoveBookmark(int64_t id);

    bool    UpdateBookmark(int64_t id, const char* new_title,
                           const char* new_folder);

    // Get all bookmarks in a folder (nullptr = root).
    // Returns count written to out (up to max_results).
    int     GetBookmarks(const char* folder,
                         BookmarkEntry* out, int max_results);

    // Check if a URL is bookmarked. Returns bookmark ID or -1.
    int64_t IsBookmarked(const char* url);

    // Export bookmarks as Netscape HTML Bookmarks format (importable by all browsers)
    bool    ExportBookmarks(const char* output_path);

    // Import Netscape HTML bookmarks file.
    int     ImportBookmarks(const char* input_path);
}
