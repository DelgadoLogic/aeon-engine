// AeonBrowser — PasswordVault.h
// DelgadoLogic | Security Team
//
// All credentials are encrypted with Windows DPAPI (CryptProtectData).
// Plaintext never touches disk. Vault file is in %APPDATA%\DelgadoLogic\Aeon\vault.db
#pragma once
#include <cstddef>
#include <cstdint>
#include <vector>
#include <string>

namespace PasswordVault {

    struct Credential {
        int64_t  id               = 0;
        char     url[512]         = {};
        char     username[256]    = {};
        char     password[256]    = {};  // plaintext — only in RAM, never serialized
        char     display_url[128] = {};  // origin only (e.g. github.com)
        uint64_t created_time     = 0;
        uint64_t last_used_time   = 0;
    };

    // Initialize vault from disk. Returns false if vault corrupt / first run.
    bool Init(const char* vault_path);

    // Save (or update) a credential. Password is DPAPI-encrypted before write.
    bool Save(const Credential& cred);

    // Look up credential for an exact origin URL. Decrypts on the fly.
    bool Find(const char* origin_url, Credential* out);

    // Look up credential by numeric ID / rowid.
    bool FindById(int64_t id, Credential* out);

    // Retrieve and decrypt a specific password via DPAPI for the UI reveal/copy action.
    std::string GetDecryptedPassword(int64_t id);
    std::string GetDecryptedPassword(const char* origin_url);

    // List all stored origins (no passwords — for UI display).
    int  ListOrigins(char out[][128], int max_count);

    // Return all credentials
    std::vector<Credential> GetAllCredentials();

    // Delete a stored credential by origin or numeric id.
    bool Delete(const char* origin_url);
    bool Delete(int64_t id);

    // Wipe all credentials from disk (used by "Clear All Data").
    void WipeAll();

    // Lock vault: zero all in-memory decrypted buffers and close DB handle.
    void Lock();

    // Master password verification and state
    bool IsUnlocked();
    bool HasMasterPassword();
    bool SetMasterPassword(const std::string& masterPassword);
    bool UnlockVault(const std::string& masterPassword);

    // Export / Import helpers
    std::string ExportPasswordsJson(bool decrypted = false);
    std::string ExportPasswordsCsv(bool decrypted = false);
    int ImportPasswordsJson(const std::string& json);
}
