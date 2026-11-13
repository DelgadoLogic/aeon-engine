// AeonBrowser — HistoryEngine.cpp
// DelgadoLogic | Storage Team
//
// SQLite3 WAL-mode history + full-text search.
// Private/incognito: all writes are no-ops.
//
// Tables:
//   history   — id, url, title, visit_time (Unix ms), visit_count, favicon_b64
//   bookmarks — id, url, title, folder, added_time
//   reading   — id, url, title, saved_time, read (0/1)
//
// FTS5 virtual table 'history_fts' mirrors history for fast substring search.

#include "HistoryEngine.h"
extern "C" {
    #include "sqlite3.h"
}
#include <windows.h>
#include <cstdio>
#include <cstring>
#include <vector>
#include <string>
#include <sstream>
#include <algorithm>
#include <ctime>
#include <mutex>

namespace HistoryEngine {

static sqlite3*   g_db         = nullptr;
static bool       g_incognito  = false;
static std::mutex g_mu;

// ─── JSON / HTML Helpers ─────────────────────────────────────────────────────
static std::string JsonEscape(const char* s) {
    if (!s) return "";
    std::string out;
    out.reserve(strlen(s) + 16);
    for (const char* p = s; *p; ++p) {
        switch (*p) {
            case '\\': out += "\\\\"; break;
            case '"':  out += "\\\""; break;
            case '\n': out += "\\n";  break;
            case '\r': out += "\\r";  break;
            case '\t': out += "\\t";  break;
            case '\b': out += "\\b";  break;
            case '\f': out += "\\f";  break;
            default:
                if (static_cast<unsigned char>(*p) < 0x20) {
                    char buf[8];
                    snprintf(buf, sizeof(buf), "\\u%04x", static_cast<unsigned char>(*p));
                    out += buf;
                } else {
                    out += *p;
                }
                break;
        }
    }
    return out;
}

static std::string HtmlEscape(const char* s) {
    if (!s) return "";
    std::string out;
    for (const char* p = s; *p; ++p) {
        switch (*p) {
            case '&':  out += "&amp;"; break;
            case '<':  out += "&lt;"; break;
            case '>':  out += "&gt;"; break;
            case '"':  out += "&quot;"; break;
            case '\'': out += "&#39;"; break;
            default:   out += *p; break;
        }
    }
    return out;
}

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

    FILETIME ft; GetSystemTimeAsFileTime(&ft);
    ULONGLONG ms100 = ((ULONGLONG)ft.dwHighDateTime << 32) | ft.dwLowDateTime;
    ULONGLONG ms = (ms100 - 116444736000000000ULL) / 10000ULL; // FILETIME -> Unix ms

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
    if (sqlite3_prepare_v2(g_db, sql, -1, &stmt, nullptr) == SQLITE_OK) {
        sqlite3_bind_text(stmt, 1, url,         -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(stmt, 2, title,        -1, SQLITE_TRANSIENT);
        sqlite3_bind_int64(stmt, 3, (sqlite3_int64)ms);
        sqlite3_bind_text(stmt, 4, favicon_b64 ? favicon_b64 : nullptr, -1, SQLITE_TRANSIENT);
        sqlite3_step(stmt);
        sqlite3_finalize(stmt);
    }
}

std::vector<HistoryEntry> GetRecent(int limit) {
    std::vector<HistoryEntry> out;
    if (!g_db) return out;
    std::lock_guard<std::mutex> lock(g_mu);

    const char* sql =
        "SELECT url, title, visit_time, visit_count FROM history "
        "ORDER BY visit_time DESC LIMIT ?;";
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(g_db, sql, -1, &stmt, nullptr) == SQLITE_OK) {
        sqlite3_bind_int(stmt, 1, limit > 0 ? limit : 100);

        while (sqlite3_step(stmt) == SQLITE_ROW) {
            HistoryEntry e = {};
            auto col = [&](int i) -> const char* {
                const unsigned char* t = sqlite3_column_text(stmt, i);
                return t ? reinterpret_cast<const char*>(t) : "";
            };
            strncpy_s(e.url,        col(0), sizeof(e.url)-1);
            strncpy_s(e.title,      col(1), sizeof(e.title)-1);
            e.visitTime  = (LONGLONG)sqlite3_column_int64(stmt, 2);
            e.visitCount = sqlite3_column_int(stmt, 3);
            out.push_back(e);
        }
        sqlite3_finalize(stmt);
    }
    return out;
}

std::vector<HistoryEntry> Search(const char* query) {
    return Search(query, 50);
}

std::vector<HistoryEntry> Search(const char* query, int limit) {
    std::vector<HistoryEntry> out;
    if (!g_db || !query || !query[0]) return out;
    if (limit <= 0) limit = 50;
    std::lock_guard<std::mutex> lock(g_mu);

    // 1. Try FTS5 literal search first
    std::string ftsQuery = "\"";
    for (const char* p = query; *p; p++) {
        if (*p == '"') ftsQuery += "\"\""; else ftsQuery += *p;
    }
    ftsQuery += "\"";

    const char* sqlFts =
        "SELECT h.url, h.title, h.visit_time, h.visit_count "
        "FROM history h "
        "JOIN history_fts f ON h.id = f.rowid "
        "WHERE history_fts MATCH ? "
        "ORDER BY h.visit_time DESC LIMIT ?;";

    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(g_db, sqlFts, -1, &stmt, nullptr) == SQLITE_OK) {
        sqlite3_bind_text(stmt, 1, ftsQuery.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_int(stmt, 2, limit);

        while (sqlite3_step(stmt) == SQLITE_ROW) {
            HistoryEntry e = {};
            auto col = [&](int i) -> const char* {
                auto* t = sqlite3_column_text(stmt, i);
                return t ? reinterpret_cast<const char*>(t) : "";
            };
            strncpy_s(e.url,   col(0), sizeof(e.url)-1);
            strncpy_s(e.title, col(1), sizeof(e.title)-1);
            e.visitTime  = (LONGLONG)sqlite3_column_int64(stmt, 2);
            e.visitCount = sqlite3_column_int(stmt, 3);
            out.push_back(e);
        }
        sqlite3_finalize(stmt);
    }

    // 2. If FTS yielded no results (e.g. substring match or special characters in URL), fallback to LIKE
    if (out.empty()) {
        const char* sqlLike =
            "SELECT url, title, visit_time, visit_count FROM history "
            "WHERE url LIKE ? OR title LIKE ? "
            "ORDER BY visit_time DESC LIMIT ?;";
        if (sqlite3_prepare_v2(g_db, sqlLike, -1, &stmt, nullptr) == SQLITE_OK) {
            std::string pattern = "%" + std::string(query) + "%";
            sqlite3_bind_text(stmt, 1, pattern.c_str(), -1, SQLITE_TRANSIENT);
            sqlite3_bind_text(stmt, 2, pattern.c_str(), -1, SQLITE_TRANSIENT);
            sqlite3_bind_int(stmt, 3, limit);

            while (sqlite3_step(stmt) == SQLITE_ROW) {
                HistoryEntry e = {};
                auto col = [&](int i) -> const char* {
                    auto* t = sqlite3_column_text(stmt, i);
                    return t ? reinterpret_cast<const char*>(t) : "";
                };
                strncpy_s(e.url,   col(0), sizeof(e.url)-1);
                strncpy_s(e.title, col(1), sizeof(e.title)-1);
                e.visitTime  = (LONGLONG)sqlite3_column_int64(stmt, 2);
                e.visitCount = sqlite3_column_int(stmt, 3);
                out.push_back(e);
            }
            sqlite3_finalize(stmt);
        }
    }

    return out;
}

void DeleteEntry(const char* url) {
    if (!g_db || !url) return;
    std::lock_guard<std::mutex> lock(g_mu);
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(g_db, "DELETE FROM history WHERE url = ?;", -1, &stmt, nullptr) == SQLITE_OK) {
        sqlite3_bind_text(stmt, 1, url, -1, SQLITE_TRANSIENT);
        sqlite3_step(stmt);
        sqlite3_finalize(stmt);
    }
}

void DeleteUrls(const std::vector<std::string>& urls) {
    if (!g_db || urls.empty()) return;
    std::lock_guard<std::mutex> lock(g_mu);
    sqlite3_exec(g_db, "BEGIN TRANSACTION;", nullptr, nullptr, nullptr);
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(g_db, "DELETE FROM history WHERE url = ?;", -1, &stmt, nullptr) == SQLITE_OK) {
        for (const auto& url : urls) {
            sqlite3_bind_text(stmt, 1, url.c_str(), -1, SQLITE_TRANSIENT);
            sqlite3_step(stmt);
            sqlite3_reset(stmt);
        }
        sqlite3_finalize(stmt);
    }
    sqlite3_exec(g_db, "COMMIT;", nullptr, nullptr, nullptr);
}

void DeleteEntries(const std::vector<std::string>& urls) {
    DeleteUrls(urls);
}

void ClearRange(uint64_t start_time, uint64_t end_time) {
    if (!g_db) return;
    std::lock_guard<std::mutex> lock(g_mu);
    if (start_time == 0 && end_time == 0) {
        sqlite3_exec(g_db, "DELETE FROM history; DELETE FROM history_fts; VACUUM;",
            nullptr, nullptr, nullptr);
        return;
    }
    sqlite3_stmt* stmt = nullptr;
    const char* sql = "DELETE FROM history WHERE visit_time >= ? AND visit_time <= ?;";
    if (sqlite3_prepare_v2(g_db, sql, -1, &stmt, nullptr) == SQLITE_OK) {
        sqlite3_bind_int64(stmt, 1, (sqlite3_int64)start_time);
        sqlite3_bind_int64(stmt, 2, (sqlite3_int64)end_time);
        sqlite3_step(stmt);
        sqlite3_finalize(stmt);
    }
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
bool AddBookmark(const char* url, const char* title, const char* folder) {
    if (!g_db || !url) return false;
    std::lock_guard<std::mutex> lock(g_mu);

    FILETIME ft; GetSystemTimeAsFileTime(&ft);
    ULONGLONG ms100 = ((ULONGLONG)ft.dwHighDateTime << 32) | ft.dwLowDateTime;
    ULONGLONG ms = (ms100 - 116444736000000000ULL) / 10000ULL;

    const char* sql =
        "INSERT OR REPLACE INTO bookmarks (url, title, folder, added_time) "
        "VALUES (?,?,?,?);";
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(g_db, sql, -1, &stmt, nullptr) != SQLITE_OK) return false;
    sqlite3_bind_text(stmt, 1, url,    -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 2, title ? title : url,  -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 3, (folder && folder[0]) ? folder : "Bookmarks", -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(stmt, 4, (sqlite3_int64)ms);
    int rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    return rc == SQLITE_DONE;
}

bool UpdateBookmark(uint64_t id, const char* url, const char* title, const char* folder) {
    if (!g_db || !url) return false;
    std::lock_guard<std::mutex> lock(g_mu);

    const char* sql = "UPDATE bookmarks SET url = ?, title = ?, folder = ? WHERE id = ?;";
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(g_db, sql, -1, &stmt, nullptr) != SQLITE_OK) return false;
    sqlite3_bind_text(stmt, 1, url, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 2, title ? title : url, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 3, (folder && folder[0]) ? folder : "Bookmarks", -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(stmt, 4, (sqlite3_int64)id);
    int rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    return rc == SQLITE_DONE;
}

void DeleteBookmark(uint64_t id) {
    if (!g_db) return;
    std::lock_guard<std::mutex> lock(g_mu);
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(g_db, "DELETE FROM bookmarks WHERE id = ?;", -1, &stmt, nullptr) == SQLITE_OK) {
        sqlite3_bind_int64(stmt, 1, (sqlite3_int64)id);
        sqlite3_step(stmt);
        sqlite3_finalize(stmt);
    }
}

void DeleteBookmark(const char* url) {
    if (!g_db || !url) return;
    std::lock_guard<std::mutex> lock(g_mu);
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(g_db, "DELETE FROM bookmarks WHERE url = ?;", -1, &stmt, nullptr) == SQLITE_OK) {
        sqlite3_bind_text(stmt, 1, url, -1, SQLITE_TRANSIENT);
        sqlite3_step(stmt);
        sqlite3_finalize(stmt);
    }
}

std::vector<Bookmark> GetBookmarks() {
    return GetBookmarks(nullptr);
}

std::vector<Bookmark> GetBookmarks(const char* folder) {
    std::vector<Bookmark> out;
    if (!g_db) return out;
    std::lock_guard<std::mutex> lock(g_mu);
    sqlite3_stmt* stmt = nullptr;

    if (folder && folder[0]) {
        const char* sql = "SELECT id, url, title, folder, added_time FROM bookmarks WHERE folder = ? ORDER BY added_time DESC;";
        if (sqlite3_prepare_v2(g_db, sql, -1, &stmt, nullptr) == SQLITE_OK) {
            sqlite3_bind_text(stmt, 1, folder, -1, SQLITE_TRANSIENT);
        }
    } else {
        const char* sql = "SELECT id, url, title, folder, added_time FROM bookmarks ORDER BY added_time DESC;";
        sqlite3_prepare_v2(g_db, sql, -1, &stmt, nullptr);
    }

    if (!stmt) return out;

    while (sqlite3_step(stmt) == SQLITE_ROW) {
        Bookmark b = {};
        b.id = sqlite3_column_int64(stmt, 0);
        auto col = [&](int i) -> const char* {
            auto* t = sqlite3_column_text(stmt, i);
            return t ? reinterpret_cast<const char*>(t) : "";
        };
        strncpy_s(b.url,    col(1), sizeof(b.url)-1);
        strncpy_s(b.title,  col(2), sizeof(b.title)-1);
        strncpy_s(b.folder, col(3), sizeof(b.folder)-1);
        b.added_time = sqlite3_column_int64(stmt, 4);
        out.push_back(b);
    }
    sqlite3_finalize(stmt);
    return out;
}

bool IsBookmarked(const char* url) {
    if (!g_db || !url) return false;
    std::lock_guard<std::mutex> lock(g_mu);
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(g_db, "SELECT 1 FROM bookmarks WHERE url = ? LIMIT 1;", -1, &stmt, nullptr) != SQLITE_OK)
        return false;
    sqlite3_bind_text(stmt, 1, url, -1, SQLITE_TRANSIENT);
    bool found = (sqlite3_step(stmt) == SQLITE_ROW);
    sqlite3_finalize(stmt);
    return found;
}

std::string ExportBookmarksJson() {
    auto bks = GetBookmarks();
    std::string json = "[\n";
    for (size_t i = 0; i < bks.size(); i++) {
        if (i > 0) json += ",\n";
        json += "  {\n";
        json += "    \"id\": " + std::to_string(bks[i].id) + ",\n";
        json += "    \"url\": \"" + JsonEscape(bks[i].url) + "\",\n";
        json += "    \"title\": \"" + JsonEscape(bks[i].title) + "\",\n";
        json += "    \"folder\": \"" + JsonEscape(bks[i].folder) + "\",\n";
        json += "    \"added_time\": " + std::to_string(bks[i].added_time) + "\n";
        json += "  }";
    }
    json += "\n]";
    return json;
}

std::string ExportBookmarksHtml() {
    auto bks = GetBookmarks();
    std::string html;
    html += "<!DOCTYPE NETSCAPE-Bookmark-file-1>\n";
    html += "<!-- This is an automatically generated file.\n";
    html += "     It will be read and overwritten.\n";
    html += "     DO NOT EDIT! -->\n";
    html += "<META HTTP-EQUIV=\"Content-Type\" CONTENT=\"text/html; charset=UTF-8\">\n";
    html += "<TITLE>Bookmarks</TITLE>\n";
    html += "<H1>Bookmarks</H1>\n";
    html += "<DL><p>\n";

    // Collect folders
    std::vector<std::string> folders;
    for (const auto& b : bks) {
        std::string f = b.folder[0] ? b.folder : "Bookmarks";
        if (std::find(folders.begin(), folders.end(), f) == folders.end()) {
            folders.push_back(f);
        }
    }

    for (const auto& folder : folders) {
        html += "    <DT><H3 ADD_DATE=\"0\" LAST_MODIFIED=\"0\">" + HtmlEscape(folder.c_str()) + "</H3>\n";
        html += "    <DL><p>\n";
        for (const auto& b : bks) {
            std::string f = b.folder[0] ? b.folder : "Bookmarks";
            if (f == folder) {
                uint64_t sec = b.added_time / 1000;
                html += "        <DT><A HREF=\"" + HtmlEscape(b.url) + "\" ADD_DATE=\"" + std::to_string(sec) + "\">" + HtmlEscape(b.title) + "</A>\n";
            }
        }
        html += "    </DL><p>\n";
    }

    html += "</DL><p>\n";
    return html;
}

int ImportBookmarksJson(const std::string& json) {
    if (json.empty()) return 0;
    int importedCount = 0;

    size_t pos = 0;
    while ((pos = json.find('{', pos)) != std::string::npos) {
        size_t endObj = json.find('}', pos);
        if (endObj == std::string::npos) break;
        std::string obj = json.substr(pos, endObj - pos + 1);

        auto extractField = [&](const std::string& key) -> std::string {
            std::string search = "\"" + key + "\"";
            size_t kpos = obj.find(search);
            if (kpos == std::string::npos) return "";
            size_t colon = obj.find(':', kpos + search.length());
            if (colon == std::string::npos) return "";
            size_t q1 = obj.find('"', colon + 1);
            if (q1 == std::string::npos) return "";
            size_t q2 = obj.find('"', q1 + 1);
            if (q2 == std::string::npos) return "";
            return obj.substr(q1 + 1, q2 - q1 - 1);
        };

        std::string url = extractField("url");
        std::string title = extractField("title");
        std::string folder = extractField("folder");

        if (!url.empty()) {
            if (AddBookmark(url.c_str(), title.empty() ? url.c_str() : title.c_str(), folder.empty() ? "Bookmarks" : folder.c_str())) {
                importedCount++;
            }
        }
        pos = endObj + 1;
    }
    return importedCount;
}

int ImportBookmarksHtml(const std::string& html) {
    if (html.empty()) return 0;
    int importedCount = 0;

    std::string currentFolder = "Bookmarks";
    size_t pos = 0;

    while (pos < html.length()) {
        size_t h3Pos = html.find("<H3", pos);
        if (h3Pos == std::string::npos) h3Pos = html.find("<h3", pos);

        size_t aPos = html.find("<A", pos);
        if (aPos == std::string::npos) aPos = html.find("<a", pos);

        if (h3Pos != std::string::npos && (aPos == std::string::npos || h3Pos < aPos)) {
            size_t closeH3Tag = html.find('>', h3Pos);
            size_t endH3 = html.find("</H3>", closeH3Tag);
            if (endH3 == std::string::npos) endH3 = html.find("</h3>", closeH3Tag);
            if (closeH3Tag != std::string::npos && endH3 != std::string::npos) {
                currentFolder = html.substr(closeH3Tag + 1, endH3 - closeH3Tag - 1);
                pos = endH3 + 5;
                continue;
            }
        }

        if (aPos != std::string::npos) {
            size_t hrefPos = html.find("HREF=\"", aPos);
            if (hrefPos == std::string::npos) hrefPos = html.find("href=\"", aPos);
            if (hrefPos != std::string::npos) {
                size_t urlStart = hrefPos + 6;
                size_t urlEnd = html.find('"', urlStart);
                size_t closeATag = html.find('>', aPos);
                size_t endA = html.find("</A>", closeATag);
                if (endA == std::string::npos) endA = html.find("</a>", closeATag);

                if (urlEnd != std::string::npos && closeATag != std::string::npos && endA != std::string::npos) {
                    std::string url = html.substr(urlStart, urlEnd - urlStart);
                    std::string title = html.substr(closeATag + 1, endA - closeATag - 1);
                    if (!url.empty()) {
                        if (AddBookmark(url.c_str(), title.empty() ? url.c_str() : title.c_str(), currentFolder.c_str())) {
                            importedCount++;
                        }
                    }
                    pos = endA + 4;
                    continue;
                }
            }
        }

        break;
    }
    return importedCount;
}

// ─── Reading List ─────────────────────────────────────────────────────────────
void AddToReadingList(const char* url, const char* title) {
    if (!g_db || !url) return;
    std::lock_guard<std::mutex> lock(g_mu);
    FILETIME ft; GetSystemTimeAsFileTime(&ft);
    ULONGLONG ms100 = ((ULONGLONG)ft.dwHighDateTime << 32) | ft.dwLowDateTime;
    ULONGLONG ms = (ms100 - 116444736000000000ULL) / 10000ULL;
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(g_db,
        "INSERT OR IGNORE INTO reading_list (url,title,saved_time,read) VALUES (?,?,?,0);",
        -1, &stmt, nullptr) == SQLITE_OK) {
        sqlite3_bind_text(stmt, 1, url, -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(stmt, 2, title ? title : url, -1, SQLITE_TRANSIENT);
        sqlite3_bind_int64(stmt, 3, (sqlite3_int64)ms);
        sqlite3_step(stmt);
        sqlite3_finalize(stmt);
    }
}

void MarkRead(const char* url) {
    if (!g_db || !url) return;
    std::lock_guard<std::mutex> lock(g_mu);
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(g_db,
        "UPDATE reading_list SET read=1 WHERE url=?;", -1, &stmt, nullptr) == SQLITE_OK) {
        sqlite3_bind_text(stmt, 1, url, -1, SQLITE_TRANSIENT);
        sqlite3_step(stmt);
        sqlite3_finalize(stmt);
    }
}

std::vector<ReadingEntry> GetReadingList() {
    std::vector<ReadingEntry> out;
    if (!g_db) return out;
    std::lock_guard<std::mutex> lock(g_mu);
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(g_db,
        "SELECT url,title,saved_time,read FROM reading_list ORDER BY saved_time DESC;",
        -1, &stmt, nullptr) == SQLITE_OK) {
        while (sqlite3_step(stmt) == SQLITE_ROW) {
            ReadingEntry e = {};
            auto col = [&](int i) -> const char* {
                auto* t = sqlite3_column_text(stmt, i);
                return t ? reinterpret_cast<const char*>(t) : "";
            };
            strncpy_s(e.url,   col(0), sizeof(e.url)-1);
            strncpy_s(e.title, col(1), sizeof(e.title)-1);
            e.saved_time = sqlite3_column_int64(stmt, 2);
            e.read       = sqlite3_column_int(stmt, 3) != 0;
            out.push_back(e);
        }
        sqlite3_finalize(stmt);
    }
    return out;
}

void Shutdown() {
    std::lock_guard<std::mutex> lock(g_mu);
    if (g_db) { sqlite3_close_v2(g_db); g_db = nullptr; }
}

} // namespace HistoryEngine
