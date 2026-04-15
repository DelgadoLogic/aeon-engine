// AeonBrowser — CrashKeys.h
// DelgadoLogic | Senior Security Engineer
//
// Thread-safe crash key/value store. Call Set() throughout your code to
// attach runtime context to crash reports. When a crash occurs, the
// CrashHandler reads all keys and writes them into the JSON sidecar.
//
// Keys are fixed-capacity (64 slots × 64-char key × 256-char value).
// No heap allocation — safe to read from inside a crash handler.

#pragma once
#include <cstdint>

namespace AeonCrash {

// Maximum dimensions — all stack/static allocated for crash safety.
static constexpr int kMaxKeys    = 64;
static constexpr int kMaxKeyLen  = 64;
static constexpr int kMaxValLen  = 256;

struct CrashKeyEntry {
    char key[kMaxKeyLen];
    char value[kMaxValLen];
    volatile bool occupied;
};

// Set a crash key. Thread-safe (uses InterlockedCompareExchange).
// Overwrites if key already exists. Silently drops if all 64 slots full.
void SetKey(const char* key, const char* value);

// Set a crash key with an integer value.
void SetKeyInt(const char* key, int64_t value);

// Remove a crash key by name.
void ClearKey(const char* key);

// Read-only access for the crash handler. Returns pointer to the static
// array of kMaxKeys entries. Caller must check entry.occupied.
const CrashKeyEntry* GetAllKeys();

// Count of currently occupied keys.
int GetKeyCount();

} // namespace AeonCrash
