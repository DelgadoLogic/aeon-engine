// AeonBrowser — PasswordVault.h
// DelgadoLogic | Security Team
//
// All credentials are encrypted with Windows DPAPI (CryptProtectData).
// Plaintext never touches disk. Vault file is in %APPDATA%\DelgadoLogic\Aeon\vault.db
#pragma once
#include <cstddef>

namespace PasswordVault {

    struct Credential {
        char url[512]      = {};
        char username[256] = {};
        char password[256] = {};  // plaintext — only in RAM, never serialized
        char display_url[128] = {}; // origin only (e.g. github.com)
    };

    // Initialize vault from disk. Returns false if vault corrupt / first run.
    bool Init(const char* vault_path);

    // Save (or update) a credential. Password is DPAPI-encrypted before write.
    bool Save(const Credential& cred);

    // Look up credential for an exact origin URL. Decrypts on the fly.
    bool Find(const char* origin_url, Credential* out);

    // List all stored origins (no passwords — for UI display).
    int  ListOrigins(char out[][128], int max_count);

    // Delete a stored credential.
    bool Delete(const char* origin_url);

    // Wipe all credentials from disk (used by "Clear All Data").
    void WipeAll();

    // Lock vault: zero all in-memory decrypted buffers.
    void Lock();
}
