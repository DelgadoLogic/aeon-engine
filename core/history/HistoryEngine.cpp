// AeonBrowser — HistoryEngine.cpp
// DelgadoLogic | Storage Team
//
// SQLite3 WAL-mode history + full-text search.
// Private/incognito: all writes are no-ops.
//
// Tables:
//   history   — url, title, visit_time (Unix ms), visit_count, favicon_b64
//   bookmarks — url, title, folder, added_time
//   reading   — url, title, saved_time, read (0/1)
//
// FTS5 virtual table 'history_fts' mirrors history for fast substring search.

#include "HistoryEngine.h"
extern "C" {
    #include "sqlite3.h"
}
#include <windows.h>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <mutex>

namespace HistoryEngine {

static sqlite3*  g_db         = nullptr;
static bool      g_incognito  = false;
static std::mutex g_mu;

// ─── Schema ──────────────────────────────────────────────────────────────────
static const char* k_schema = R"SQL(
PRAGMA journal_mode = WAL;
PRAGMA synchronous  = NORMAL;
PRAGMA foreign_keys = ON;

CREATE TABLE IF NOT EXISTS history (
    id          INTEGER PRIMARY KEY AUTOINCREMENT,
    url         TEXT    NOT NULL,
    title       TEXT,
    visit_time  INTEGER NOT NULL,   -- Unix milliseconds
    visit_count INTEGER DEFAULT 1,
    favicon_b64 TEXT,
    UNIQUE(url) ON CONFLICT IGNORE
);

CREATE TABLE IF NOT EXISTS bookmarks (
    id          INTEGER PRIMARY KEY AUTOINCREMENT,
    url         TEXT    NOT NULL UNIQUE,
    title       TEXT,
    folder      TEXT    DEFAULT 'Bookmarks',
    added_time  INTEGER NOT NULL
);

CREATE TABLE IF NOT EXISTS reading_list (
    id          INTEGER PRIMARY KEY AUTOINCREMENT,
    url         TEXT    NOT NULL UNIQUE,
    title       TEXT,
    saved_time  INTEGER NOT NULL,
    read        INTEGER DEFAULT 0
);

-- Full-text search
CREATE VIRTUAL TABLE IF NOT EXISTS history_fts USING fts5(
    url, title,
    content='history', content_rowid='id'
);

-- Keep FTS in sync
CREATE TRIGGER IF NOT EXISTS history_fts_insert
    AFTER INSERT ON history
    BEGIN INSERT INTO history_fts(rowid,url,title) VALUES(new.id,new.url,new.title); END;

CREATE TRIGGER IF NOT EXISTS history_fts_update
    AFTER UPDATE ON history
    BEGIN
        DELETE FROM history_fts WHERE rowid = old.id;
        INSERT INTO history_fts(rowid,url,title) VALUES(new.id,new.url,new.title);
    END;

CREATE TRIGGER IF NOT EXISTS history_fts_delete
    AFTER DELETE ON history
    BEGIN DELETE FROM history_fts WHERE rowid = old.id; END;
)SQL";

// ─── Init ─────────────────────────────────────────────────────────────────────
bool Init(const char* db_path, bool incognito) {
    g_incognito = incognito;

    if (incognito) {
        // In-memory database — no disk writes ever
        if (sqlite3_open(":memory:", &g_db) != SQLITE_OK) return false;
    } else {
        if (sqlite3_open(db_path, &g_db) != SQLITE_OK) {
            fprintf(stderr, "[History] Cannot open: %s\n", sqlite3_errmsg(g_db));
            return false;
        }
    }

    char* err = nullptr;
    sqlite3_exec(g_db, k_schema, nullptr, nullptr, &err);
    if (err) { fprintf(stderr, "[History] Schema error: %s\n", err); sqlite3_free(err); }

    fprintf(stdout, "[History] Initialized (%s)\n", incognito ? "incognito/:memory:" : db_path);
    return true;
}

// ─── History ──────────────────────────────────────────────────────────────────
void RecordVisit(const char* url, const char* title, const char* favicon_b64) {
    if (!g_db || !url) return;
    std::lock_guard<std::mutex> lock(g_mu);

    ULONGLONG ms = GetTickCount64(); // Rough; production uses SystemTime
    FILETIME ft; GetSystemTimeAsFileTime(&ft);
    ULONGLONG ms100 = ((ULONGLONG)ft.dwHighDateTime << 32) | ft.dwLowDateTime;
    ms = (ms100 - 116444736000000000ULL) / 10000ULL; // FILETIME → Unix ms

    // Upsert: if URL exists, increment visit_count and update title/time
    const char* sql =
        "INSERT INTO history (url, title, visit_time, visit_count, favicon_b64) "
        "VALUES (?1, ?2, ?3, 1, ?4) "
        "ON CONFLICT(url) DO UPDATE SET "
        "  title = excluded.title, "
        "  visit_time = excluded.visit_time, "
        "  visit_count = history.visit_count + 1, "
        "  favicon_b64 = excluded.favicon_b64;";

    sqlite3_stmt* stmt = nullptr;
    sqlite3_prepare_v2(g_db, sql, -1, &stmt, nullptr);
    sqlite3_bind_text(stmt, 1, url,         -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 2, title,        -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(stmt, 3, (sqlite3_int64)ms);
    sqlite3_bind_text(stmt, 4, favicon_b64 ? favicon_b64 : nullptr, -1, SQLITE_TRANSIENT);
    sqlite3_step(stmt);
    sqlite3_finalize(stmt);
}

std::vector<HistoryEntry> GetRecent(int limit) {
    std::vector<HistoryEntry> out;
    if (!g_db) return out;
    std::lock_guard<std::mutex> lock(g_mu);

    const char* sql =
        "SELECT url, title, visit_time, visit_count FROM history "
        "ORDER BY visit_time DESC LIMIT ?;";
    sqlite3_stmt* stmt = nullptr;
    sqlite3_prepare_v2(g_db, sql, -1, &stmt, nullptr);
    sqlite3_bind_int(stmt, 1, limit > 0 ? limit : 100);

    while (sqlite3_step(stmt) == SQLITE_ROW) {
        HistoryEntry e = {};
        auto col = [&](int i) -> const char* {
            const unsigned char* t = sqlite3_column_text(stmt, i);
            return t ? reinterpret_cast<const char*>(t) : "";
        };
        strncpy_s(e.url,        col(0), sizeof(e.url)-1);
        strncpy_s(e.title,      col(1), sizeof(e.title)-1);
        e.visit_time  = (LONGLONG)sqlite3_column_int64(stmt, 2);
        e.visit_count = sqlite3_column_int(stmt, 3);
        out.push_back(e);
    }
    sqlite3_finalize(stmt);
    return out;
}

std::vector<HistoryEntry> Search(const char* query) {
    std::vector<HistoryEntry> out;
    if (!g_db || !query) return out;
    std::lock_guard<std::mutex> lock(g_mu);

    // FTS5 query — wrap in quotes for literal search
    std::string ftsQuery = "\"";
    for (const char* p = query; *p; p++) {
        if (*p == '"') ftsQuery += "\"\""; else ftsQuery += *p;
    }
    ftsQuery += "\"";

    const char* sql =
        "SELECT h.url, h.title, h.visit_time, h.visit_count "
        "FROM history h "
        "JOIN history_fts f ON h.id = f.rowid "
        "WHERE history_fts MATCH ? "
        "ORDER BY h.visit_time DESC LIMIT 50;";

    sqlite3_stmt* stmt = nullptr;
    sqlite3_prepare_v2(g_db, sql, -1, &stmt, nullptr);
    sqlite3_bind_text(stmt, 1, ftsQuery.c_str(), -1, SQLITE_TRANSIENT);

    while (sqlite3_step(stmt) == SQLITE_ROW) {
        HistoryEntry e = {};
        auto col = [&](int i) -> const char* {
            auto* t = sqlite3_column_text(stmt, i);
            return t ? reinterpret_cast<const char*>(t) : "";
        };
        strncpy_s(e.url,   col(0), sizeof(e.url)-1);
        strncpy_s(e.title, col(1), sizeof(e.title)-1);
        e.visit_time  = (LONGLONG)sqlite3_column_int64(stmt, 2);
        e.visit_count = sqlite3_column_int(stmt, 3);
        out.push_back(e);
    }
    sqlite3_finalize(stmt);
    return out;
}

void DeleteEntry(const char* url) {
    if (!g_db || !url) return;
    std::lock_guard<std::mutex> lock(g_mu);
    sqlite3_stmt* stmt = nullptr;
    sqlite3_prepare_v2(g_db, "DELETE FROM history WHERE url = ?;", -1, &stmt, nullptr);
    sqlite3_bind_text(stmt, 1, url, -1, SQLITE_TRANSIENT);
    sqlite3_step(stmt); sqlite3_finalize(stmt);
}

void WipeAll() {
    if (!g_db) return;
    std::lock_guard<std::mutex> lock(g_mu);
    sqlite3_exec(g_db, "DELETE FROM history; DELETE FROM history_fts; VACUUM;",
        nullptr, nullptr, nullptr);
    fprintf(stdout, "[History] All history wiped.\n");
}

bool IsPrivate() { return g_incognito; }

// ─── Bookmarks ────────────────────────────────────────────────────────────────
void AddBookmark(const char* url, const char* title, const char* folder) {
    if (!g_db || !url) return;
    std::lock_guard<std::mutex> lock(g_mu);

    FILETIME ft; GetSystemTimeAsFileTime(&ft);
    ULONGLONG ms = (((ULONGLONG)ft.dwHighDateTime<<32)|ft.dwLowDateTime
        - 116444736000000000ULL) / 10000ULL;

    const char* sql =
        "INSERT OR REPLACE INTO bookmarks (url, title, folder, added_time) "
        "VALUES (?,?,?,?);";
    sqlite3_stmt* stmt = nullptr;
    sqlite3_prepare_v2(g_db, sql, -1, &stmt, nullptr);
    sqlite3_bind_text(stmt, 1, url,    -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 2, title,  -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 3, folder ? folder : "Bookmarks", -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(stmt, 4, (sqlite3_int64)ms);
    sqlite3_step(stmt); sqlite3_finalize(stmt);
}

void DeleteBookmark(const char* url) {
    if (!g_db || !url) return;
    std::lock_guard<std::mutex> lock(g_mu);
    sqlite3_stmt* stmt = nullptr;
    sqlite3_prepare_v2(g_db, "DELETE FROM bookmarks WHERE url = ?;", -1, &stmt, nullptr);
    sqlite3_bind_text(stmt, 1, url, -1, SQLITE_TRANSIENT);
    sqlite3_step(stmt); sqlite3_finalize(stmt);
}

std::vector<BookmarkEntry> GetBookmarks() {
    std::vector<BookmarkEntry> out;
    if (!g_db) return out;
    std::lock_guard<std::mutex> lock(g_mu);
    sqlite3_stmt* stmt = nullptr;
    sqlite3_prepare_v2(g_db,
        "SELECT url, title, folder, added_time FROM bookmarks ORDER BY added_time DESC;",
        -1, &stmt, nullptr);
    while (sqlite3_step(stmt) == SQLITE_ROW) {
        BookmarkEntry e = {};
        auto col = [&](int i) -> const char* {
            auto* t = sqlite3_column_text(stmt,i);
            return t ? reinterpret_cast<const char*>(t) : "";
        };
        strncpy_s(e.url,    col(0), sizeof(e.url)-1);
        strncpy_s(e.title,  col(1), sizeof(e.title)-1);
        strncpy_s(e.folder, col(2), sizeof(e.folder)-1);
        e.added_time = sqlite3_column_int64(stmt, 3);
        out.push_back(e);
    }
    sqlite3_finalize(stmt);
    return out;
}

bool IsBookmarked(const char* url) {
    if (!g_db || !url) return false;
    std::lock_guard<std::mutex> lock(g_mu);
    sqlite3_stmt* stmt = nullptr;
    sqlite3_prepare_v2(g_db,
        "SELECT 1 FROM bookmarks WHERE url = ? LIMIT 1;", -1, &stmt, nullptr);
    sqlite3_bind_text(stmt, 1, url, -1, SQLITE_TRANSIENT);
    bool found = sqlite3_step(stmt) == SQLITE_ROW;
    sqlite3_finalize(stmt);
    return found;
}

// ─── Reading List ─────────────────────────────────────────────────────────────
void AddToReadingList(const char* url, const char* title) {
    if (!g_db || !url) return;
    std::lock_guard<std::mutex> lock(g_mu);
    FILETIME ft; GetSystemTimeAsFileTime(&ft);
    ULONGLONG ms = (((ULONGLONG)ft.dwHighDateTime<<32)|ft.dwLowDateTime
        - 116444736000000000ULL) / 10000ULL;
    sqlite3_stmt* stmt = nullptr;
    sqlite3_prepare_v2(g_db,
        "INSERT OR IGNORE INTO reading_list (url,title,saved_time,read) VALUES (?,?,?,0);",
        -1, &stmt, nullptr);
    sqlite3_bind_text(stmt,1,url,-1,SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt,2,title,-1,SQLITE_TRANSIENT);
    sqlite3_bind_int64(stmt,3,(sqlite3_int64)ms);
    sqlite3_step(stmt); sqlite3_finalize(stmt);
}

void MarkRead(const char* url) {
    if (!g_db || !url) return;
    std::lock_guard<std::mutex> lock(g_mu);
    sqlite3_stmt* stmt = nullptr;
    sqlite3_prepare_v2(g_db,
        "UPDATE reading_list SET read=1 WHERE url=?;", -1, &stmt, nullptr);
    sqlite3_bind_text(stmt,1,url,-1,SQLITE_TRANSIENT);
    sqlite3_step(stmt); sqlite3_finalize(stmt);
}

std::vector<ReadingEntry> GetReadingList() {
    std::vector<ReadingEntry> out;
    if (!g_db) return out;
    std::lock_guard<std::mutex> lock(g_mu);
    sqlite3_stmt* stmt = nullptr;
    sqlite3_prepare_v2(g_db,
        "SELECT url,title,saved_time,read FROM reading_list ORDER BY saved_time DESC;",
        -1, &stmt, nullptr);
    while (sqlite3_step(stmt) == SQLITE_ROW) {
        ReadingEntry e = {};
        auto col = [&](int i) -> const char* {
            auto* t = sqlite3_column_text(stmt,i);
            return t ? reinterpret_cast<const char*>(t) : "";
        };
        strncpy_s(e.url,   col(0), sizeof(e.url)-1);
        strncpy_s(e.title, col(1), sizeof(e.title)-1);
        e.saved_time = sqlite3_column_int64(stmt,2);
        e.read       = sqlite3_column_int(stmt,3) != 0;
        out.push_back(e);
    }
    sqlite3_finalize(stmt);
    return out;
}

void Shutdown() {
    std::lock_guard<std::mutex> lock(g_mu);
    if (g_db) { sqlite3_close(g_db); g_db = nullptr; }
}

} // namespace HistoryEngine
