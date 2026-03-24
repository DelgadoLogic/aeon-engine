// AeonBrowser — HistoryEngine.cpp
// DelgadoLogic | Lead Systems Architect
//
// SQLite3 amalgamation must be placed at: history/sqlite3.c + sqlite3.h
// Download: https://sqlite.org/amalgamation.html (public domain)
// We vendor sqlite3.c directly — no need to install SQLite system-wide.
// This is the same pattern used by Firefox (firefox: places.sqlite).

#include "HistoryEngine.h"
#include "sqlite3.h"       // SQLite3 amalgamation header
#include <windows.h>
#include <shlobj.h>
#include <cstdio>
#include <cstring>
#include <ctime>

static sqlite3* g_Db  = nullptr;
static bool     g_Private = false;

// ---------------------------------------------------------------------------
// Schema SQL — created on first run
// ---------------------------------------------------------------------------
static const char* SCHEMA_SQL = R"SQL(
CREATE TABLE IF NOT EXISTS visits (
    id          INTEGER PRIMARY KEY AUTOINCREMENT,
    url         TEXT    NOT NULL,
    title       TEXT,
    timestamp   INTEGER NOT NULL DEFAULT (strftime('%s','now')),
    visit_count INTEGER NOT NULL DEFAULT 1
);
CREATE INDEX IF NOT EXISTS idx_visits_url       ON visits(url);
CREATE INDEX IF NOT EXISTS idx_visits_timestamp ON visits(timestamp DESC);

CREATE TABLE IF NOT EXISTS bookmarks (
    id          INTEGER PRIMARY KEY AUTOINCREMENT,
    url         TEXT    NOT NULL,
    title       TEXT,
    folder      TEXT    NOT NULL DEFAULT 'Bookmarks bar',
    favicon_b64 TEXT,
    created_at  INTEGER NOT NULL DEFAULT (strftime('%s','now')),
    modified_at INTEGER NOT NULL DEFAULT (strftime('%s','now'))
);
CREATE INDEX IF NOT EXISTS idx_bm_folder ON bookmarks(folder);

CREATE TABLE IF NOT EXISTS bookmark_folders (
    id         INTEGER PRIMARY KEY AUTOINCREMENT,
    name       TEXT    NOT NULL,
    parent_id  INTEGER,
    sort_order INTEGER NOT NULL DEFAULT 0
);
)SQL";

namespace HistoryEngine {

bool Initialize(bool private_mode) {
    g_Private = private_mode;

    const char* dbPath;
    char pathBuf[MAX_PATH] = {};

    if (private_mode) {
        dbPath = ":memory:"; // in-memory = truly private (no disk trace)
    } else {
        char appData[MAX_PATH];
        SHGetFolderPathA(nullptr, CSIDL_APPDATA, nullptr, 0, appData);
        char dir[MAX_PATH];
        _snprintf_s(dir, sizeof(dir), _TRUNCATE,
            "%s\\DelgadoLogic\\Aeon", appData);
        CreateDirectoryA(dir, nullptr);
        _snprintf_s(pathBuf, sizeof(pathBuf), _TRUNCATE,
            "%s\\history.db", dir);
        dbPath = pathBuf;
    }

    int rc = sqlite3_open(dbPath, &g_Db);
    if (rc != SQLITE_OK) {
        fprintf(stderr, "[HistoryEngine] Failed to open DB: %s\n",
            sqlite3_errmsg(g_Db));
        return false;
    }

    // Enable WAL mode for better concurrent read performance
    sqlite3_exec(g_Db, "PRAGMA journal_mode=WAL;", nullptr, nullptr, nullptr);
    sqlite3_exec(g_Db, "PRAGMA synchronous=NORMAL;", nullptr, nullptr, nullptr);

    // Create schema
    char* errMsg = nullptr;
    rc = sqlite3_exec(g_Db, SCHEMA_SQL, nullptr, nullptr, &errMsg);
    if (rc != SQLITE_OK) {
        fprintf(stderr, "[HistoryEngine] Schema error: %s\n", errMsg);
        sqlite3_free(errMsg);
        return false;
    }

    fprintf(stdout, "[HistoryEngine] Database open: %s\n",
        private_mode ? ":memory: (private)" : pathBuf);
    return true;
}

void Shutdown() {
    if (g_Db) {
        sqlite3_close(g_Db);
        g_Db = nullptr;
    }
}

void RecordVisit(const char* url, const char* title) {
    if (!g_Db || !url) return;

    // Upsert: increment visit_count if URL exists, else insert new
    const char* sql = R"SQL(
        INSERT INTO visits (url, title, timestamp, visit_count)
        VALUES (?, ?, strftime('%s','now'), 1)
        ON CONFLICT(url) DO UPDATE SET
            visit_count = visit_count + 1,
            title       = excluded.title,
            timestamp   = excluded.timestamp;
    )SQL";

    // SQLite doesn't support ON CONFLICT(url) without a UNIQUE index fix
    // Use two-step upsert for compat with SQLite < 3.24
    sqlite3_stmt* stmt;
    const char* checkSql = "SELECT id FROM visits WHERE url=? LIMIT 1;";
    sqlite3_prepare_v2(g_Db, checkSql, -1, &stmt, nullptr);
    sqlite3_bind_text(stmt, 1, url, -1, SQLITE_STATIC);
    int found = (sqlite3_step(stmt) == SQLITE_ROW);
    sqlite3_int64 existing_id = found ? sqlite3_column_int64(stmt, 0) : 0;
    sqlite3_finalize(stmt);

    if (found) {
        const char* upd = "UPDATE visits SET visit_count=visit_count+1, "
            "title=?, timestamp=? WHERE id=?;";
        sqlite3_prepare_v2(g_Db, upd, -1, &stmt, nullptr);
        sqlite3_bind_text(stmt, 1, title ? title : url, -1, SQLITE_STATIC);
        sqlite3_bind_int64(stmt, 2, (sqlite3_int64)time(nullptr));
        sqlite3_bind_int64(stmt, 3, existing_id);
    } else {
        const char* ins = "INSERT INTO visits (url,title,timestamp,visit_count) "
            "VALUES(?,?,?,1);";
        sqlite3_prepare_v2(g_Db, ins, -1, &stmt, nullptr);
        sqlite3_bind_text(stmt, 1, url,                -1, SQLITE_STATIC);
        sqlite3_bind_text(stmt, 2, title ? title : url,-1, SQLITE_STATIC);
        sqlite3_bind_int64(stmt, 3, (sqlite3_int64)time(nullptr));
    }
    sqlite3_step(stmt);
    sqlite3_finalize(stmt);
}

int SearchHistory(const char* query, HistoryEntry* out, int max_results) {
    if (!g_Db || !query || !out) return 0;

    const char* sql = "SELECT id,url,title,timestamp,visit_count FROM visits "
        "WHERE url LIKE ? OR title LIKE ? ORDER BY timestamp DESC LIMIT ?;";

    char pattern[520];
    _snprintf_s(pattern, sizeof(pattern), _TRUNCATE, "%%%s%%", query);

    sqlite3_stmt* stmt;
    sqlite3_prepare_v2(g_Db, sql, -1, &stmt, nullptr);
    sqlite3_bind_text(stmt, 1, pattern, -1, SQLITE_STATIC);
    sqlite3_bind_text(stmt, 2, pattern, -1, SQLITE_STATIC);
    sqlite3_bind_int(stmt,  3, max_results);

    int count = 0;
    while (sqlite3_step(stmt) == SQLITE_ROW && count < max_results) {
        HistoryEntry& e = out[count++];
        e.id          = sqlite3_column_int64(stmt, 0);
        strncpy_s(e.url,   (const char*)sqlite3_column_text(stmt, 1), _TRUNCATE);
        const char* t = (const char*)sqlite3_column_text(stmt, 2);
        strncpy_s(e.title, t ? t : "", _TRUNCATE);
        e.timestamp   = sqlite3_column_int64(stmt, 3);
        e.visit_count = sqlite3_column_int(stmt,  4);
    }
    sqlite3_finalize(stmt);
    return count;
}

void ClearHistory() {
    if (!g_Db) return;
    sqlite3_exec(g_Db, "DELETE FROM visits;", nullptr, nullptr, nullptr);
    sqlite3_exec(g_Db, "VACUUM;", nullptr, nullptr, nullptr);
}

void DeleteEntry(int64_t id) {
    if (!g_Db) return;
    sqlite3_stmt* stmt;
    sqlite3_prepare_v2(g_Db, "DELETE FROM visits WHERE id=?;", -1, &stmt, nullptr);
    sqlite3_bind_int64(stmt, 1, id);
    sqlite3_step(stmt);
    sqlite3_finalize(stmt);
}

int64_t AddBookmark(const char* url, const char* title, const char* folder) {
    if (!g_Db || !url) return -1;
    sqlite3_stmt* stmt;
    sqlite3_prepare_v2(g_Db,
        "INSERT INTO bookmarks(url,title,folder) VALUES(?,?,?);",
        -1, &stmt, nullptr);
    sqlite3_bind_text(stmt, 1, url,    -1, SQLITE_STATIC);
    sqlite3_bind_text(stmt, 2, title ? title : url, -1, SQLITE_STATIC);
    sqlite3_bind_text(stmt, 3, folder ? folder : "Bookmarks bar", -1, SQLITE_STATIC);
    sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    return sqlite3_last_insert_rowid(g_Db);
}

bool RemoveBookmark(int64_t id) {
    if (!g_Db) return false;
    sqlite3_stmt* stmt;
    sqlite3_prepare_v2(g_Db, "DELETE FROM bookmarks WHERE id=?;", -1, &stmt, nullptr);
    sqlite3_bind_int64(stmt, 1, id);
    int rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    return rc == SQLITE_DONE;
}

int64_t IsBookmarked(const char* url) {
    if (!g_Db || !url) return -1;
    sqlite3_stmt* stmt;
    sqlite3_prepare_v2(g_Db,
        "SELECT id FROM bookmarks WHERE url=? LIMIT 1;",
        -1, &stmt, nullptr);
    sqlite3_bind_text(stmt, 1, url, -1, SQLITE_STATIC);
    int64_t id = -1;
    if (sqlite3_step(stmt) == SQLITE_ROW)
        id = sqlite3_column_int64(stmt, 0);
    sqlite3_finalize(stmt);
    return id;
}

bool ExportBookmarks(const char* output_path) {
    if (!g_Db || !output_path) return false;
    FILE* f = nullptr;
    fopen_s(&f, output_path, "w");
    if (!f) return false;

    fprintf(f, "<!DOCTYPE NETSCAPE-Bookmark-file-1>\n");
    fprintf(f, "<!-- Exported from Aeon Browser by DelgadoLogic -->\n");
    fprintf(f, "<META HTTP-EQUIV=\"Content-Type\" CONTENT=\"text/html; charset=UTF-8\">\n");
    fprintf(f, "<TITLE>Bookmarks</TITLE>\n");
    fprintf(f, "<H1>Bookmarks</H1>\n<DL><p>\n");

    sqlite3_stmt* stmt;
    sqlite3_prepare_v2(g_Db,
        "SELECT url,title,created_at FROM bookmarks ORDER BY folder,id;",
        -1, &stmt, nullptr);
    while (sqlite3_step(stmt) == SQLITE_ROW) {
        const char* url   = (const char*)sqlite3_column_text(stmt, 0);
        const char* title = (const char*)sqlite3_column_text(stmt, 1);
        int64_t     ts    = sqlite3_column_int64(stmt, 2);
        fprintf(f, "    <DT><A HREF=\"%s\" ADD_DATE=\"%lld\">%s</A>\n",
            url, (long long)ts, title ? title : url);
    }
    sqlite3_finalize(stmt);
    fprintf(f, "</DL><p>\n");
    fclose(f);
    return true;
}

} // namespace HistoryEngine
