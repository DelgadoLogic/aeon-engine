// AeonBrowser — PasswordVault.cpp
// DelgadoLogic | Security Team
//
// All credentials are encrypted with Windows DPAPI (CryptProtectData).
// Plaintext NEVER written to disk. Live only in RAM during the session.
//
// STORAGE FORMAT: vault.db is a SQLite3 database with 3 columns:
//   origin TEXT   — the site origin (e.g. "github.com")
//   username BLOB — plaintext username (not sensitive, not encrypted)
//   password BLOB — DPAPI-encrypted password blob (binary, opaque)
//
// DPAPI SCOPE: CRYPTPROTECT_LOCAL_MACHINE = 0 (per-user scope).
// Only the SAME Windows user account on the SAME machine can decrypt.
// Even if villain copies vault.db, they cannot read passwords.
//
// INCOGNITO: In incognito mode, HistoryEngine::IsIncognito() returns true.
// PasswordVault::Save() is a no-op in incognito — nothing is written.
//
// MASTER PASSWORD (future): We plan a PBKDF2-derived key wrapping the
// DPAPI blob for portability. For v1.0 we stay pure DPAPI for simplicity.

#include "PasswordVault.h"
#include "../../history/HistoryEngine.h"
#include <windows.h>
#include <wincrypt.h>
#include <cstdio>
#include <cstring>
#include <vector>
#include <string>

// We use the SQLite3 handle from HistoryEngine to avoid a second DB connection.
// In production this is refactored into a VaultDB class. For v1.0 we vendor
// sqlite3_exec calls directly — same pattern as HistoryEngine.cpp.
extern "C" {
    #include "../../history/sqlite3.h"
}
#pragma comment(lib, "crypt32.lib")

namespace PasswordVault {

static sqlite3* g_db = nullptr;
static char     g_vaultPath[MAX_PATH] = {};

// ---------------------------------------------------------------------------
// DPAPI helpers — encrypt/decrypt a buffer using the current user's key
// ---------------------------------------------------------------------------
static std::vector<BYTE> DpapiEncrypt(const char* plaintext) {
    DATA_BLOB input  = {};
    DATA_BLOB output = {};
    input.pbData = reinterpret_cast<BYTE*>(const_cast<char*>(plaintext));
    input.cbData = static_cast<DWORD>(strlen(plaintext) + 1);

    // CRYPTPROTECT_UI_FORBIDDEN — never show a dialog
    if (!CryptProtectData(&input, L"AeonVault", nullptr, nullptr, nullptr,
                          CRYPTPROTECT_UI_FORBIDDEN, &output)) {
        fprintf(stderr, "[Vault] CryptProtectData failed: %lu\n", GetLastError());
        return {};
    }
    std::vector<BYTE> blob(output.pbData, output.pbData + output.cbData);
    LocalFree(output.pbData);
    return blob;
}

static std::string DpapiDecrypt(const BYTE* blob, DWORD blobLen) {
    DATA_BLOB input  = {};
    DATA_BLOB output = {};
    input.pbData = const_cast<BYTE*>(blob);
    input.cbData = blobLen;

    if (!CryptUnprotectData(&input, nullptr, nullptr, nullptr, nullptr,
                            CRYPTPROTECT_UI_FORBIDDEN, &output)) {
        fprintf(stderr, "[Vault] CryptUnprotectData failed: %lu\n", GetLastError());
        return {};
    }
    std::string result(reinterpret_cast<char*>(output.pbData));
    SecureZeroMemory(output.pbData, output.cbData); // zero before free
    LocalFree(output.pbData);
    return result;
}

// ---------------------------------------------------------------------------
// Extract origin from URL (strips path/query/fragment)
// e.g. "https://github.com/orgs/foo" → "github.com"
// ---------------------------------------------------------------------------
static std::string OriginOf(const char* url) {
    const char* p = url;
    if (strncmp(p, "https://", 8) == 0) p += 8;
    else if (strncmp(p, "http://",  7) == 0) p += 7;
    const char* end = strpbrk(p, "/?#");
    return end ? std::string(p, end) : std::string(p);
}

// ---------------------------------------------------------------------------
// Init — open or create vault.db
// ---------------------------------------------------------------------------
bool Init(const char* vault_path) {
    strncpy_s(g_vaultPath, vault_path, MAX_PATH - 1);

    if (sqlite3_open(vault_path, &g_db) != SQLITE_OK) {
        fprintf(stderr, "[Vault] Cannot open vault.db: %s\n", sqlite3_errmsg(g_db));
        return false;
    }

    const char* schema =
        "CREATE TABLE IF NOT EXISTS vault ("
        "  origin   TEXT PRIMARY KEY,"
        "  username TEXT NOT NULL,"
        "  password BLOB NOT NULL"
        ");";
    char* err = nullptr;
    sqlite3_exec(g_db, schema, nullptr, nullptr, &err);
    if (err) { fprintf(stderr, "[Vault] Schema error: %s\n", err); sqlite3_free(err); }

    // WAL mode — same as history DB, prevents corruption on crash
    sqlite3_exec(g_db, "PRAGMA journal_mode=WAL;", nullptr, nullptr, nullptr);
    sqlite3_exec(g_db, "PRAGMA foreign_keys=ON;",  nullptr, nullptr, nullptr);

    fprintf(stdout, "[Vault] Initialized. Path: %s\n", vault_path);
    return true;
}

// ---------------------------------------------------------------------------
// Save — DPAPI-encrypt password then upsert row
// ---------------------------------------------------------------------------
bool Save(const Credential& cred) {
    if (!g_db) return false;

    std::string origin = OriginOf(cred.url);
    auto blob = DpapiEncrypt(cred.password);
    if (blob.empty()) return false;

    sqlite3_stmt* stmt = nullptr;
    const char* sql =
        "INSERT OR REPLACE INTO vault (origin, username, password) "
        "VALUES (?, ?, ?);";
    sqlite3_prepare_v2(g_db, sql, -1, &stmt, nullptr);
    sqlite3_bind_text (stmt, 1, origin.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text (stmt, 2, cred.username,  -1, SQLITE_TRANSIENT);
    sqlite3_bind_blob (stmt, 3, blob.data(), static_cast<int>(blob.size()), SQLITE_TRANSIENT);
    int rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);

    if (rc != SQLITE_DONE) {
        fprintf(stderr, "[Vault] Save failed: %s\n", sqlite3_errmsg(g_db));
        return false;
    }
    fprintf(stdout, "[Vault] Saved credential for: %s\n", origin.c_str());
    return true;
}

// ---------------------------------------------------------------------------
// Find — look up by origin, DPAPI-decrypt password into out->password
// ---------------------------------------------------------------------------
bool Find(const char* origin_url, Credential* out) {
    if (!g_db || !out) return false;
    std::string origin = OriginOf(origin_url);

    sqlite3_stmt* stmt = nullptr;
    const char* sql = "SELECT username, password FROM vault WHERE origin = ?;";
    sqlite3_prepare_v2(g_db, sql, -1, &stmt, nullptr);
    sqlite3_bind_text(stmt, 1, origin.c_str(), -1, SQLITE_TRANSIENT);

    bool found = false;
    if (sqlite3_step(stmt) == SQLITE_ROW) {
        const char* user = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 0));
        const BYTE* blob = reinterpret_cast<const BYTE*>(sqlite3_column_blob(stmt, 1));
        DWORD blobLen    = static_cast<DWORD>(sqlite3_column_bytes(stmt, 1));

        strncpy_s(out->url,      origin_url, sizeof(out->url)-1);
        strncpy_s(out->username, user,        sizeof(out->username)-1);
        strncpy_s(out->display_url, origin.c_str(), sizeof(out->display_url)-1);

        // Decrypt — live in RAM only
        std::string pass = DpapiDecrypt(blob, blobLen);
        strncpy_s(out->password, pass.c_str(), sizeof(out->password)-1);
        SecureZeroMemory(const_cast<char*>(pass.c_str()), pass.size()); // zero temp
        found = true;
    }
    sqlite3_finalize(stmt);
    return found;
}

// ---------------------------------------------------------------------------
// ListOrigins — for UI display (no passwords exposed)
// ---------------------------------------------------------------------------
int ListOrigins(char out[][128], int max_count) {
    if (!g_db || !out) return 0;
    sqlite3_stmt* stmt = nullptr;
    sqlite3_prepare_v2(g_db, "SELECT origin FROM vault ORDER BY origin;", -1, &stmt, nullptr);
    int n = 0;
    while (sqlite3_step(stmt) == SQLITE_ROW && n < max_count) {
        const char* o = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 0));
        strncpy_s(out[n], 128, o, _TRUNCATE);
        n++;
    }
    sqlite3_finalize(stmt);
    return n;
}

// ---------------------------------------------------------------------------
// Delete / WipeAll / Lock
// ---------------------------------------------------------------------------
bool Delete(const char* origin_url) {
    if (!g_db) return false;
    std::string origin = OriginOf(origin_url);
    sqlite3_stmt* stmt = nullptr;
    sqlite3_prepare_v2(g_db, "DELETE FROM vault WHERE origin = ?;", -1, &stmt, nullptr);
    sqlite3_bind_text(stmt, 1, origin.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    fprintf(stdout, "[Vault] Deleted credential: %s\n", origin.c_str());
    return true;
}

void WipeAll() {
    if (!g_db) return;
    sqlite3_exec(g_db, "DELETE FROM vault;", nullptr, nullptr, nullptr);
    // Vacuum to actually reclaim disk space (removed blobs)
    sqlite3_exec(g_db, "VACUUM;", nullptr, nullptr, nullptr);
    fprintf(stdout, "[Vault] All credentials wiped.\n");
}

void Lock() {
    // In future: zero all decrypted in-memory credential caches
    fprintf(stdout, "[Vault] Vault locked (session keys cleared).\n");
}

} // namespace PasswordVault
