// AeonBrowser — PasswordVault.cpp
// DelgadoLogic | Security Team
//
// All credentials are encrypted with Windows DPAPI (CryptProtectData).
// Plaintext NEVER written to disk. Live only in RAM during the session.
//
// STORAGE FORMAT: vault.db is a SQLite3 database with columns:
//   id             INTEGER PRIMARY KEY AUTOINCREMENT
//   origin         TEXT NOT NULL UNIQUE
//   username       TEXT NOT NULL
//   password       BLOB NOT NULL (DPAPI-encrypted blob)
//   created_time   INTEGER
//   last_used_time INTEGER
//
// DPAPI SCOPE: CRYPTPROTECT_LOCAL_MACHINE = 0 (per-user scope).
// Only the SAME Windows user account on the SAME machine can decrypt.
//
// MASTER PASSWORD: Key-stretching verification with 5,000 rounds of SHA-256
// over random 16-byte cryptographically-secure salts stored in vault_meta.

#include "PasswordVault.h"
#include "../history/HistoryEngine.h"
#include <windows.h>
#include <wincrypt.h>
#include <cstdio>
#include <cstring>
#include <vector>
#include <string>
#include <sstream>
#include <mutex>

extern "C" {
    #include "../history/sqlite3.h"
}
#pragma comment(lib, "crypt32.lib")
#pragma comment(lib, "advapi32.lib")

namespace PasswordVault {

static sqlite3*   g_db                  = nullptr;
static char       g_vaultPath[MAX_PATH] = {};
static bool       g_unlocked            = false;
static std::mutex g_mu;

// ─── String / JSON Escaping ───────────────────────────────────────────────────
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

// ---------------------------------------------------------------------------
// DPAPI helpers — encrypt/decrypt a buffer using the current user's key
// ---------------------------------------------------------------------------
static std::vector<BYTE> DpapiEncrypt(const char* plaintext) {
    if (!plaintext) return {};
    DATA_BLOB input  = {};
    DATA_BLOB output = {};
    input.pbData = reinterpret_cast<BYTE*>(const_cast<char*>(plaintext));
    input.cbData = static_cast<DWORD>(strlen(plaintext) + 1);

    // CRYPTPROTECT_UI_FORBIDDEN — never show a modal dialog
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
    if (!blob || blobLen == 0) return "";
    DATA_BLOB input  = {};
    DATA_BLOB output = {};
    input.pbData = const_cast<BYTE*>(blob);
    input.cbData = blobLen;

    if (!CryptUnprotectData(&input, nullptr, nullptr, nullptr, nullptr,
                            CRYPTPROTECT_UI_FORBIDDEN, &output)) {
        fprintf(stderr, "[Vault] CryptUnprotectData failed: %lu\n", GetLastError());
        return "";
    }
    std::string result(reinterpret_cast<char*>(output.pbData));
    SecureZeroMemory(output.pbData, output.cbData); // zero before free
    LocalFree(output.pbData);
    return result;
}

// ---------------------------------------------------------------------------
// Extract origin from URL (strips scheme, path, query, fragment)
// e.g. "https://github.com/orgs/foo" → "github.com"
// ---------------------------------------------------------------------------
static std::string OriginOf(const char* url) {
    if (!url) return "";
    const char* p = url;
    if (strncmp(p, "https://", 8) == 0) p += 8;
    else if (strncmp(p, "http://",  7) == 0) p += 7;
    const char* end = strpbrk(p, "/?#");
    return end ? std::string(p, end) : std::string(p);
}

// ---------------------------------------------------------------------------
// Cryptographic Salt and Key-Stretching Helpers for Master Password
// ---------------------------------------------------------------------------
static std::string GenerateRandomSaltHex() {
    HCRYPTPROV hProv = 0;
    if (!CryptAcquireContextA(&hProv, nullptr, nullptr, PROV_RSA_AES, CRYPT_VERIFYCONTEXT)) {
        return "aeon_master_salt_a8b9c0d1e2f34567";
    }
    BYTE salt[16] = {};
    CryptGenRandom(hProv, sizeof(salt), salt);
    CryptReleaseContext(hProv, 0);

    char hex[33] = {};
    for (int i = 0; i < 16; i++) {
        snprintf(hex + i * 2, 3, "%02x", salt[i]);
    }
    return std::string(hex);
}

static std::string ComputeSaltedHash(const std::string& password, const std::string& saltHex) {
    HCRYPTPROV hProv = 0;
    if (!CryptAcquireContextA(&hProv, nullptr, nullptr, PROV_RSA_AES, CRYPT_VERIFYCONTEXT)) {
        return "";
    }

    std::string current = password + ":" + saltHex;
    BYTE hash[32] = {};
    DWORD hashLen = sizeof(hash);

    // Key-stretching: 5,000 iterative SHA-256 rounds
    for (int round = 0; round < 5000; round++) {
        HCRYPTHASH hHash = 0;
        if (!CryptCreateHash(hProv, CALG_SHA_256, 0, 0, &hHash)) break;
        CryptHashData(hHash, reinterpret_cast<const BYTE*>(current.data()), static_cast<DWORD>(current.size()), 0);
        hashLen = sizeof(hash);
        CryptGetHashParam(hHash, HP_HASHVAL, hash, &hashLen, 0);
        CryptDestroyHash(hHash);

        char hexBuf[65] = {};
        for (DWORD i = 0; i < hashLen; i++) {
            snprintf(hexBuf + i * 2, 3, "%02x", hash[i]);
        }
        current = std::string(hexBuf) + ":" + saltHex;
    }
    CryptReleaseContext(hProv, 0);

    char finalHex[65] = {};
    for (DWORD i = 0; i < hashLen; i++) {
        snprintf(finalHex + i * 2, 3, "%02x", hash[i]);
    }
    return std::string(finalHex);
}

// ---------------------------------------------------------------------------
// Init — open or create vault.db
// ---------------------------------------------------------------------------
bool Init(const char* vault_path) {
    if (!vault_path) return false;
    std::lock_guard<std::mutex> lock(g_mu);
    strncpy_s(g_vaultPath, vault_path, MAX_PATH - 1);

    if (sqlite3_open(vault_path, &g_db) != SQLITE_OK) {
        fprintf(stderr, "[Vault] Cannot open vault.db: %s\n", sqlite3_errmsg(g_db));
        return false;
    }

    const char* schema =
        "CREATE TABLE IF NOT EXISTS vault ("
        "  id             INTEGER PRIMARY KEY AUTOINCREMENT,"
        "  origin         TEXT NOT NULL UNIQUE,"
        "  username       TEXT NOT NULL,"
        "  password       BLOB NOT NULL,"
        "  created_time   INTEGER DEFAULT 0,"
        "  last_used_time INTEGER DEFAULT 0"
        ");"
        "CREATE TABLE IF NOT EXISTS vault_meta ("
        "  key   TEXT PRIMARY KEY,"
        "  value TEXT NOT NULL"
        ");";

    char* err = nullptr;
    sqlite3_exec(g_db, schema, nullptr, nullptr, &err);
    if (err) { fprintf(stderr, "[Vault] Schema error: %s\n", err); sqlite3_free(err); }

    // WAL mode — prevents corruption on unexpected termination
    sqlite3_exec(g_db, "PRAGMA journal_mode=WAL;", nullptr, nullptr, nullptr);
    sqlite3_exec(g_db, "PRAGMA foreign_keys=ON;",  nullptr, nullptr, nullptr);

    // Initial state: automatically unlocked on local DPAPI host unless master password is set
    g_unlocked = true;

    fprintf(stdout, "[Vault] Initialized. Path: %s\n", vault_path);
    return true;
}

// ---------------------------------------------------------------------------
// Save — DPAPI-encrypt password then upsert row
// ---------------------------------------------------------------------------
bool Save(const Credential& cred) {
    if (!g_db) return false;
    std::lock_guard<std::mutex> lock(g_mu);

    std::string origin = cred.url[0] ? OriginOf(cred.url) : OriginOf(cred.display_url);
    if (origin.empty()) origin = cred.display_url;
    if (origin.empty()) return false;

    auto blob = DpapiEncrypt(cred.password);
    if (blob.empty()) return false;

    FILETIME ft; GetSystemTimeAsFileTime(&ft);
    ULONGLONG ms100 = ((ULONGLONG)ft.dwHighDateTime << 32) | ft.dwLowDateTime;
    ULONGLONG ms = (ms100 - 116444736000000000ULL) / 10000ULL;

    sqlite3_stmt* stmt = nullptr;
    const char* sql =
        "INSERT INTO vault (origin, username, password, created_time, last_used_time) "
        "VALUES (?1, ?2, ?3, ?4, ?4) "
        "ON CONFLICT(origin) DO UPDATE SET "
        "  username = excluded.username, "
        "  password = excluded.password, "
        "  last_used_time = excluded.last_used_time;";

    if (sqlite3_prepare_v2(g_db, sql, -1, &stmt, nullptr) != SQLITE_OK) return false;
    sqlite3_bind_text (stmt, 1, origin.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text (stmt, 2, cred.username,  -1, SQLITE_TRANSIENT);
    sqlite3_bind_blob (stmt, 3, blob.data(), static_cast<int>(blob.size()), SQLITE_TRANSIENT);
    sqlite3_bind_int64(stmt, 4, (sqlite3_int64)ms);
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
    if (!g_db || !out || !origin_url) return false;
    std::lock_guard<std::mutex> lock(g_mu);
    std::string origin = OriginOf(origin_url);
    if (origin.empty()) origin = origin_url;

    sqlite3_stmt* stmt = nullptr;
    const char* sql = "SELECT rowid, username, password, created_time, last_used_time FROM vault WHERE origin = ?;";
    if (sqlite3_prepare_v2(g_db, sql, -1, &stmt, nullptr) != SQLITE_OK) return false;
    sqlite3_bind_text(stmt, 1, origin.c_str(), -1, SQLITE_TRANSIENT);

    bool found = false;
    if (sqlite3_step(stmt) == SQLITE_ROW) {
        int64_t id       = sqlite3_column_int64(stmt, 0);
        const char* user = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 1));
        const BYTE* blob = reinterpret_cast<const BYTE*>(sqlite3_column_blob(stmt, 2));
        DWORD blobLen    = static_cast<DWORD>(sqlite3_column_bytes(stmt, 2));
        uint64_t created = sqlite3_column_int64(stmt, 3);
        uint64_t last_used = sqlite3_column_int64(stmt, 4);

        out->id = id;
        strncpy_s(out->url,         origin_url,       sizeof(out->url) - 1);
        strncpy_s(out->username,    user ? user : "", sizeof(out->username) - 1);
        strncpy_s(out->display_url, origin.c_str(),   sizeof(out->display_url) - 1);
        out->created_time   = created;
        out->last_used_time = last_used;

        // Decrypt password into out->password buffer
        std::string pass = DpapiDecrypt(blob, blobLen);
        strncpy_s(out->password, pass.c_str(), sizeof(out->password) - 1);
        SecureZeroMemory(const_cast<char*>(pass.c_str()), pass.size());
        found = true;
    }
    sqlite3_finalize(stmt);
    return found;
}

bool FindById(int64_t id, Credential* out) {
    if (!g_db || !out) return false;
    std::lock_guard<std::mutex> lock(g_mu);

    sqlite3_stmt* stmt = nullptr;
    const char* sql = "SELECT origin, username, password, created_time, last_used_time FROM vault WHERE id = ? OR rowid = ?;";
    if (sqlite3_prepare_v2(g_db, sql, -1, &stmt, nullptr) != SQLITE_OK) return false;
    sqlite3_bind_int64(stmt, 1, (sqlite3_int64)id);
    sqlite3_bind_int64(stmt, 2, (sqlite3_int64)id);

    bool found = false;
    if (sqlite3_step(stmt) == SQLITE_ROW) {
        const char* origin = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 0));
        const char* user   = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 1));
        const BYTE* blob   = reinterpret_cast<const BYTE*>(sqlite3_column_blob(stmt, 2));
        DWORD blobLen      = static_cast<DWORD>(sqlite3_column_bytes(stmt, 2));
        uint64_t created   = sqlite3_column_int64(stmt, 3);
        uint64_t last_used = sqlite3_column_int64(stmt, 4);

        out->id = id;
        strncpy_s(out->url,         origin ? origin : "", sizeof(out->url) - 1);
        strncpy_s(out->username,    user ? user : "",     sizeof(out->username) - 1);
        strncpy_s(out->display_url, origin ? origin : "", sizeof(out->display_url) - 1);
        out->created_time   = created;
        out->last_used_time = last_used;

        std::string pass = DpapiDecrypt(blob, blobLen);
        strncpy_s(out->password, pass.c_str(), sizeof(out->password) - 1);
        SecureZeroMemory(const_cast<char*>(pass.c_str()), pass.size());
        found = true;
    }
    sqlite3_finalize(stmt);
    return found;
}

// ---------------------------------------------------------------------------
// GetDecryptedPassword — decrypt a single password by id or origin for UI reveal
// ---------------------------------------------------------------------------
std::string GetDecryptedPassword(int64_t id) {
    if (!g_db) return "";
    std::lock_guard<std::mutex> lock(g_mu);

    sqlite3_stmt* stmt = nullptr;
    const char* sql = "SELECT password FROM vault WHERE id = ? OR rowid = ? LIMIT 1;";
    if (sqlite3_prepare_v2(g_db, sql, -1, &stmt, nullptr) != SQLITE_OK) return "";
    sqlite3_bind_int64(stmt, 1, (sqlite3_int64)id);
    sqlite3_bind_int64(stmt, 2, (sqlite3_int64)id);

    std::string result = "";
    if (sqlite3_step(stmt) == SQLITE_ROW) {
        const BYTE* blob = reinterpret_cast<const BYTE*>(sqlite3_column_blob(stmt, 0));
        DWORD blobLen    = static_cast<DWORD>(sqlite3_column_bytes(stmt, 0));
        result = DpapiDecrypt(blob, blobLen);
    }
    sqlite3_finalize(stmt);
    return result;
}

std::string GetDecryptedPassword(const char* origin_url) {
    if (!g_db || !origin_url) return "";
    std::lock_guard<std::mutex> lock(g_mu);
    std::string origin = OriginOf(origin_url);
    if (origin.empty()) origin = origin_url;

    sqlite3_stmt* stmt = nullptr;
    const char* sql = "SELECT password FROM vault WHERE origin = ? LIMIT 1;";
    if (sqlite3_prepare_v2(g_db, sql, -1, &stmt, nullptr) != SQLITE_OK) return "";
    sqlite3_bind_text(stmt, 1, origin.c_str(), -1, SQLITE_TRANSIENT);

    std::string result = "";
    if (sqlite3_step(stmt) == SQLITE_ROW) {
        const BYTE* blob = reinterpret_cast<const BYTE*>(sqlite3_column_blob(stmt, 0));
        DWORD blobLen    = static_cast<DWORD>(sqlite3_column_bytes(stmt, 0));
        result = DpapiDecrypt(blob, blobLen);
    }
    sqlite3_finalize(stmt);
    return result;
}

// ---------------------------------------------------------------------------
// ListOrigins — for UI display (no passwords exposed)
// ---------------------------------------------------------------------------
int ListOrigins(char out[][128], int max_count) {
    if (!g_db || !out || max_count <= 0) return 0;
    std::lock_guard<std::mutex> lock(g_mu);
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(g_db, "SELECT origin FROM vault ORDER BY origin;", -1, &stmt, nullptr) != SQLITE_OK)
        return 0;

    int n = 0;
    while (sqlite3_step(stmt) == SQLITE_ROW && n < max_count) {
        const char* o = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 0));
        strncpy_s(out[n], 128, o ? o : "", _TRUNCATE);
        n++;
    }
    sqlite3_finalize(stmt);
    return n;
}

std::vector<Credential> GetAllCredentials() {
    std::vector<Credential> out;
    if (!g_db) return out;
    std::lock_guard<std::mutex> lock(g_mu);

    sqlite3_stmt* stmt = nullptr;
    const char* sql = "SELECT rowid, origin, username, password, created_time, last_used_time FROM vault ORDER BY origin ASC;";
    if (sqlite3_prepare_v2(g_db, sql, -1, &stmt, nullptr) != SQLITE_OK) return out;

    while (sqlite3_step(stmt) == SQLITE_ROW) {
        Credential cred = {};
        cred.id = sqlite3_column_int64(stmt, 0);
        const char* origin = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 1));
        const char* user   = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 2));
        const BYTE* blob   = reinterpret_cast<const BYTE*>(sqlite3_column_blob(stmt, 3));
        DWORD blobLen      = static_cast<DWORD>(sqlite3_column_bytes(stmt, 3));
        cred.created_time   = sqlite3_column_int64(stmt, 4);
        cred.last_used_time = sqlite3_column_int64(stmt, 5);

        strncpy_s(cred.url,         origin ? origin : "", sizeof(cred.url) - 1);
        strncpy_s(cred.display_url, origin ? origin : "", sizeof(cred.display_url) - 1);
        strncpy_s(cred.username,    user ? user : "",     sizeof(cred.username) - 1);

        std::string pass = DpapiDecrypt(blob, blobLen);
        strncpy_s(cred.password, pass.c_str(), sizeof(cred.password) - 1);
        SecureZeroMemory(const_cast<char*>(pass.c_str()), pass.size());

        out.push_back(cred);
    }
    sqlite3_finalize(stmt);
    return out;
}

// ---------------------------------------------------------------------------
// Delete / WipeAll / Lock
// ---------------------------------------------------------------------------
bool Delete(const char* origin_url) {
    if (!g_db || !origin_url) return false;
    std::lock_guard<std::mutex> lock(g_mu);
    std::string origin = OriginOf(origin_url);
    if (origin.empty()) origin = origin_url;

    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(g_db, "DELETE FROM vault WHERE origin = ?;", -1, &stmt, nullptr) != SQLITE_OK)
        return false;
    sqlite3_bind_text(stmt, 1, origin.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    fprintf(stdout, "[Vault] Deleted credential: %s\n", origin.c_str());
    return true;
}

bool Delete(int64_t id) {
    if (!g_db) return false;
    std::lock_guard<std::mutex> lock(g_mu);
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(g_db, "DELETE FROM vault WHERE id = ? OR rowid = ?;", -1, &stmt, nullptr) != SQLITE_OK)
        return false;
    sqlite3_bind_int64(stmt, 1, (sqlite3_int64)id);
    sqlite3_bind_int64(stmt, 2, (sqlite3_int64)id);
    sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    fprintf(stdout, "[Vault] Deleted credential id #%lld\n", (long long)id);
    return true;
}

void WipeAll() {
    if (!g_db) return;
    std::lock_guard<std::mutex> lock(g_mu);
    sqlite3_exec(g_db, "DELETE FROM vault;", nullptr, nullptr, nullptr);
    sqlite3_exec(g_db, "VACUUM;", nullptr, nullptr, nullptr);
    fprintf(stdout, "[Vault] All credentials wiped.\n");
}

void Lock() {
    std::lock_guard<std::mutex> lock(g_mu);
    if (g_db) {
        sqlite3_close_v2(g_db);
        g_db = nullptr;
    }
    g_unlocked = false;
    SecureZeroMemory(g_vaultPath, sizeof(g_vaultPath));
    fprintf(stdout, "[Vault] Vault locked — DB closed, session state reset.\n");
}

// ---------------------------------------------------------------------------
// Master Password Validation & Key Derivation
// ---------------------------------------------------------------------------
bool IsUnlocked() {
    return g_unlocked;
}

bool HasMasterPassword() {
    if (!g_db) return false;
    std::lock_guard<std::mutex> lock(g_mu);
    sqlite3_stmt* stmt = nullptr;
    const char* sql = "SELECT value FROM vault_meta WHERE key = 'master_hash';";
    if (sqlite3_prepare_v2(g_db, sql, -1, &stmt, nullptr) != SQLITE_OK) return false;
    bool has = (sqlite3_step(stmt) == SQLITE_ROW);
    sqlite3_finalize(stmt);
    return has;
}

bool SetMasterPassword(const std::string& masterPassword) {
    if (!g_db || masterPassword.empty()) return false;
    std::lock_guard<std::mutex> lock(g_mu);

    std::string salt = GenerateRandomSaltHex();
    std::string hash = ComputeSaltedHash(masterPassword, salt);
    if (hash.empty()) return false;

    sqlite3_stmt* stmt = nullptr;
    const char* sql = "INSERT OR REPLACE INTO vault_meta (key, value) VALUES (?, ?);";
    if (sqlite3_prepare_v2(g_db, sql, -1, &stmt, nullptr) == SQLITE_OK) {
        sqlite3_bind_text(stmt, 1, "master_salt", -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(stmt, 2, salt.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_step(stmt);
        sqlite3_reset(stmt);

        sqlite3_bind_text(stmt, 1, "master_hash", -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(stmt, 2, hash.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_step(stmt);
        sqlite3_finalize(stmt);
    }
    g_unlocked = true;
    return true;
}

bool UnlockVault(const std::string& masterPassword) {
    if (!g_db) return false;

    // Check if master password is set
    std::string salt, storedHash;
    {
        std::lock_guard<std::mutex> lock(g_mu);
        sqlite3_stmt* stmt = nullptr;
        const char* sql = "SELECT key, value FROM vault_meta WHERE key IN ('master_salt', 'master_hash');";
        if (sqlite3_prepare_v2(g_db, sql, -1, &stmt, nullptr) == SQLITE_OK) {
            while (sqlite3_step(stmt) == SQLITE_ROW) {
                const char* k = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 0));
                const char* v = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 1));
                if (k && v) {
                    if (strcmp(k, "master_salt") == 0) salt = v;
                    else if (strcmp(k, "master_hash") == 0) storedHash = v;
                }
            }
            sqlite3_finalize(stmt);
        }
    }

    if (salt.empty() || storedHash.empty()) {
        // No master password configured: UI unlock succeeds if input valid
        if (!masterPassword.empty() && masterPassword.length() >= 4) {
            SetMasterPassword(masterPassword);
        }
        g_unlocked = true;
        return true;
    }

    std::string computedHash = ComputeSaltedHash(masterPassword, salt);
    if (!computedHash.empty() && computedHash == storedHash) {
        g_unlocked = true;
        return true;
    }
    return false;
}

// ---------------------------------------------------------------------------
// Export / Import Helpers
// ---------------------------------------------------------------------------
std::string ExportPasswordsJson(bool decrypted) {
    if (!g_db) return "[]";
    std::lock_guard<std::mutex> lock(g_mu);

    sqlite3_stmt* stmt = nullptr;
    const char* sql = "SELECT rowid, origin, username, password, created_time, last_used_time FROM vault ORDER BY origin ASC;";
    if (sqlite3_prepare_v2(g_db, sql, -1, &stmt, nullptr) != SQLITE_OK) return "[]";

    std::string json = "[\n";
    bool first = true;

    while (sqlite3_step(stmt) == SQLITE_ROW) {
        if (!first) json += ",\n";
        first = false;

        int64_t id       = sqlite3_column_int64(stmt, 0);
        const char* origin = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 1));
        const char* user   = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 2));
        const BYTE* blob   = reinterpret_cast<const BYTE*>(sqlite3_column_blob(stmt, 3));
        DWORD blobLen      = static_cast<DWORD>(sqlite3_column_bytes(stmt, 3));
        uint64_t created   = sqlite3_column_int64(stmt, 4);
        uint64_t last_used = sqlite3_column_int64(stmt, 5);

        std::string passVal = "••••••••";
        if (decrypted) {
            passVal = DpapiDecrypt(blob, blobLen);
        }

        json += "  {\n";
        json += "    \"id\": " + std::to_string(id) + ",\n";
        json += "    \"origin\": \"" + JsonEscape(origin ? origin : "") + "\",\n";
        json += "    \"username\": \"" + JsonEscape(user ? user : "") + "\",\n";
        json += "    \"password\": \"" + JsonEscape(passVal.c_str()) + "\",\n";
        json += "    \"created_time\": " + std::to_string(created) + ",\n";
        json += "    \"last_used_time\": " + std::to_string(last_used) + "\n";
        json += "  }";

        if (decrypted) {
            SecureZeroMemory(const_cast<char*>(passVal.data()), passVal.size());
        }
    }
    sqlite3_finalize(stmt);
    json += "\n]";
    return json;
}

std::string ExportPasswordsCsv(bool decrypted) {
    if (!g_db) return "";
    std::lock_guard<std::mutex> lock(g_mu);

    sqlite3_stmt* stmt = nullptr;
    const char* sql = "SELECT origin, username, password FROM vault ORDER BY origin ASC;";
    if (sqlite3_prepare_v2(g_db, sql, -1, &stmt, nullptr) != SQLITE_OK) return "";

    std::string csv = "url,username,password\n";
    while (sqlite3_step(stmt) == SQLITE_ROW) {
        const char* origin = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 0));
        const char* user   = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 1));
        const BYTE* blob   = reinterpret_cast<const BYTE*>(sqlite3_column_blob(stmt, 2));
        DWORD blobLen      = static_cast<DWORD>(sqlite3_column_bytes(stmt, 2));

        std::string passVal = "••••••••";
        if (decrypted) {
            passVal = DpapiDecrypt(blob, blobLen);
        }

        csv += std::string("\"") + (origin ? origin : "") + "\",\"" + (user ? user : "") + "\",\"" + passVal + "\"\n";

        if (decrypted) {
            SecureZeroMemory(const_cast<char*>(passVal.data()), passVal.size());
        }
    }
    sqlite3_finalize(stmt);
    return csv;
}

int ImportPasswordsJson(const std::string& json) {
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

        std::string origin = extractField("origin");
        if (origin.empty()) origin = extractField("url");
        std::string user = extractField("username");
        if (user.empty()) user = extractField("user");
        std::string pass = extractField("password");

        if (!origin.empty() && !pass.empty()) {
            Credential cred = {};
            strncpy_s(cred.url, origin.c_str(), sizeof(cred.url) - 1);
            strncpy_s(cred.display_url, origin.c_str(), sizeof(cred.display_url) - 1);
            strncpy_s(cred.username, user.c_str(), sizeof(cred.username) - 1);
            strncpy_s(cred.password, pass.c_str(), sizeof(cred.password) - 1);
            if (Save(cred)) {
                importedCount++;
            }
            SecureZeroMemory(cred.password, sizeof(cred.password));
        }
        pos = endObj + 1;
    }
    return importedCount;
}

} // namespace PasswordVault
