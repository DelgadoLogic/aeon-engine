// tests/test_omni_license.cpp
// AeonBrowser — Production RSA-2048 License Validation & Cryptographic Verification Test Harness
// DelgadoLogic | Security & Cryptography Track 3

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <bcrypt.h>
#include <wincrypt.h>
#include <shlobj.h>
#include <iostream>
#include <iomanip>
#include <string>
#include <vector>
#include <cassert>
#include <fstream>
#include <sstream>

#include "../core/billing/OmniLicense.h"
#include "../updater/AutoUpdater.h"
#include "../core/crypto/Ed25519.h"
#include "../core/session/nlohmann/json.hpp"

#pragma comment(lib, "bcrypt.lib")
#pragma comment(lib, "crypt32.lib")
#pragma comment(lib, "wbemuuid.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "oleaut32.lib")
#pragma comment(lib, "advapi32.lib")

#ifndef BCRYPT_SUCCESS
#define BCRYPT_SUCCESS(Status) (((NTSTATUS)(Status)) >= 0)
#endif

namespace {
    int g_testsPassed = 0;
    int g_testsFailed = 0;

    void LogResult(const std::string& testName, bool passed, const std::string& details = "") {
        if (passed) {
            std::cout << "  [PASS] " << testName;
            if (!details.empty()) std::cout << " (" << details << ")";
            std::cout << std::endl;
            g_testsPassed++;
        } else {
            std::cout << "  [FAIL] " << testName;
            if (!details.empty()) std::cout << " — " << details;
            std::cout << std::endl;
            g_testsFailed++;
        }
    }

    std::string Base64Encode(const BYTE* data, DWORD len) {
        DWORD outLen = 0;
        if (!CryptBinaryToStringA(data, len, CRYPT_STRING_BASE64 | CRYPT_STRING_NOCRLF, NULL, &outLen)) {
            return "";
        }
        std::string str(outLen, '\0');
        if (!CryptBinaryToStringA(data, len, CRYPT_STRING_BASE64 | CRYPT_STRING_NOCRLF, &str[0], &outLen)) {
            return "";
        }
        while (!str.empty() && (str.back() == '\0' || str.back() == '\r' || str.back() == '\n')) {
            str.pop_back();
        }
        return str;
    }

    std::string SignPayloadWithCNG(BCRYPT_KEY_HANDLE hPrivKey, const std::string& payload) {
        // Hash payload with SHA-256
        BCRYPT_ALG_HANDLE hSha256 = NULL;
        if (!BCRYPT_SUCCESS(BCryptOpenAlgorithmProvider(&hSha256, BCRYPT_SHA256_ALGORITHM, NULL, 0))) {
            return "";
        }

        BCRYPT_HASH_HANDLE hHash = NULL;
        DWORD hashObjSize = 0, cbData = 0;
        BCryptGetProperty(hSha256, BCRYPT_OBJECT_LENGTH, (PBYTE)&hashObjSize, sizeof(DWORD), &cbData, 0);
        std::vector<BYTE> hashObj(hashObjSize);
        BCryptCreateHash(hSha256, &hHash, hashObj.data(), hashObjSize, NULL, 0, 0);
        BCryptHashData(hHash, (PBYTE)payload.data(), (ULONG)payload.size(), 0);
        BYTE hashVal[32];
        BCryptFinishHash(hHash, hashVal, sizeof(hashVal), 0);
        BCryptDestroyHash(hHash);
        BCryptCloseAlgorithmProvider(hSha256, 0);

        // Sign hash using private key with PKCS#1 padding
        BCRYPT_PKCS1_PADDING_INFO padInfo = {};
        padInfo.pszAlgId = BCRYPT_SHA256_ALGORITHM;

        DWORD sigLen = 0;
        if (!BCRYPT_SUCCESS(BCryptSignHash(hPrivKey, &padInfo, hashVal, sizeof(hashVal), NULL, 0, &sigLen, BCRYPT_PAD_PKCS1))) {
            return "";
        }
        std::vector<BYTE> sigBuf(sigLen);
        if (!BCRYPT_SUCCESS(BCryptSignHash(hPrivKey, &padInfo, hashVal, sizeof(hashVal), sigBuf.data(), sigLen, &sigLen, BCRYPT_PAD_PKCS1))) {
            return "";
        }
        return Base64Encode(sigBuf.data(), sigLen);
    }
}

int main() {
    std::cout << "====================================================================" << std::endl;
    std::cout << " AeonBrowser — Production RSA-2048 Cryptographic Verification Suite" << std::endl;
    std::cout << " Track 3: Sovereign Cryptography & Offline License Validation" << std::endl;
    std::cout << " DelgadoLogic Internal Test Framework" << std::endl;
    std::cout << "====================================================================" << std::endl;

    BCRYPT_ALG_HANDLE hRsaAlg = NULL;
    if (!BCRYPT_SUCCESS(BCryptOpenAlgorithmProvider(&hRsaAlg, BCRYPT_RSA_ALGORITHM, NULL, 0))) {
        std::cerr << "[FATAL] Failed to open CNG RSA provider." << std::endl;
        return 1;
    }

    BCRYPT_KEY_HANDLE hTestPrivKey = NULL;
    if (!BCRYPT_SUCCESS(BCryptGenerateKeyPair(hRsaAlg, &hTestPrivKey, 2048, 0)) ||
        !BCRYPT_SUCCESS(BCryptFinalizeKeyPair(hTestPrivKey, 0))) {
        std::cerr << "[FATAL] Failed to generate test RSA-2048 keypair." << std::endl;
        BCryptCloseAlgorithmProvider(hRsaAlg, 0);
        return 1;
    }
    std::cout << "[INFO] Ephemeral RSA-2048 keypair generated successfully." << std::endl;

    std::string testPubKeyB64 = OmniLicense::DELGADOLOGIC_PUBLIC_KEY;
    std::cout << "[INFO] Public key blob length: " << testPubKeyB64.size() << " bytes." << std::endl;
    std::cout << "[INFO] Pinned sovereign public key present: " << (!testPubKeyB64.empty() ? "YES" : "NO") << std::endl;

    std::string currentHWID = OmniLicense::GenerateHWID();
    std::cout << "[INFO] Machine HWID: " << currentHWID << " (len=" << currentHWID.length() << ")" << std::endl;
    std::cout << "--------------------------------------------------------------------" << std::endl;

    DWORD keyBlobLen = 0;
    BCryptExportKey(hTestPrivKey, NULL, BCRYPT_RSAPUBLIC_BLOB, NULL, 0, &keyBlobLen, 0);
    std::vector<BYTE> keyBlob(keyBlobLen);
    BCryptExportKey(hTestPrivKey, NULL, BCRYPT_RSAPUBLIC_BLOB, keyBlob.data(), keyBlobLen, &keyBlobLen, 0);

    std::string testPrivPubKeyB64 = Base64Encode(keyBlob.data(), keyBlobLen);

    // -------------------------------------------------------------------------
    // TEST 1: Valid RSA-2048 signature (Pro tier, unbounded HWID)
    // -------------------------------------------------------------------------
    {
        nlohmann::json claims;
        claims["Tier"] = "Pro";
        claims["ExpiresAt"] = 2524608000LL; // 2050
        claims["BoundHwid"] = "";
        claims["OrderId"] = "TEST-ORDER-001";
        std::string payload = claims.dump();
        std::string sig = SignPayloadWithCNG(hTestPrivKey, payload);

        nlohmann::json token;
        token["Payload"] = payload;
        token["Signature"] = sig;

        OmniTierLevel tier = OmniTierLevel::Free;
        bool ok = OmniLicense::ValidateOfflineKey(token.dump(), &tier, testPrivPubKeyB64);
        LogResult("Test 1: Valid RSA-2048 signature (Pro tier, unbounded HWID)", ok && tier == OmniTierLevel::Pro, "Tier=1");
    }

    // -------------------------------------------------------------------------
    // TEST 2: Valid RSA-2048 signature (Enterprise tier extraction)
    // -------------------------------------------------------------------------
    {
        nlohmann::json claims;
        claims["Tier"] = "Enterprise";
        claims["ExpiresAt"] = 2524608000LL;
        claims["BoundHwid"] = "";
        std::string payload = claims.dump();
        std::string sig = SignPayloadWithCNG(hTestPrivKey, payload);

        nlohmann::json token;
        token["Payload"] = payload;
        token["Signature"] = sig;

        OmniTierLevel tier = OmniTierLevel::Free;
        bool ok = OmniLicense::ValidateOfflineKey(token.dump(), &tier, testPrivPubKeyB64);
        LogResult("Test 2: Valid RSA-2048 signature (Enterprise tier extraction)", ok && tier == OmniTierLevel::Enterprise, "Tier=2");
    }

    // -------------------------------------------------------------------------
    // TEST 3: Base64-encoded LicenseToken verification
    // -------------------------------------------------------------------------
    {
        nlohmann::json claims;
        claims["Tier"] = "Pro";
        claims["ExpiresAt"] = 2524608000LL;
        claims["BoundHwid"] = "";
        std::string payload = claims.dump();
        std::string sig = SignPayloadWithCNG(hTestPrivKey, payload);

        nlohmann::json token;
        token["Payload"] = payload;
        token["Signature"] = sig;
        std::string tokenStr = token.dump();
        std::string b64Token = Base64Encode((const BYTE*)tokenStr.data(), (DWORD)tokenStr.size());

        OmniTierLevel tier = OmniTierLevel::Free;
        bool ok = OmniLicense::ValidateOfflineKey(b64Token, &tier, testPrivPubKeyB64);
        LogResult("Test 3: Base64-encoded LicenseToken verification", ok && tier == OmniTierLevel::Pro);
    }

    // -------------------------------------------------------------------------
    // TEST 4: Hardware-bound license matching current HWID
    // -------------------------------------------------------------------------
    {
        nlohmann::json claims;
        claims["Tier"] = "Enterprise";
        claims["ExpiresAt"] = 2524608000LL;
        claims["BoundHwid"] = currentHWID;
        std::string payload = claims.dump();
        std::string sig = SignPayloadWithCNG(hTestPrivKey, payload);

        nlohmann::json token;
        token["Payload"] = payload;
        token["Signature"] = sig;

        OmniTierLevel tier = OmniTierLevel::Free;
        bool ok = OmniLicense::ValidateOfflineKey(token.dump(), &tier, testPrivPubKeyB64);
        LogResult("Test 4: Hardware-bound license matching current HWID", ok && tier == OmniTierLevel::Enterprise);
    }

    // -------------------------------------------------------------------------
    // TEST 5: Direct payload with embedded signature field
    // -------------------------------------------------------------------------
    {
        nlohmann::json claims;
        claims["Tier"] = "Pro";
        claims["ExpiresAt"] = 2524608000LL;
        claims["BoundHwid"] = "";
        std::string payload = claims.dump();
        std::string sig = SignPayloadWithCNG(hTestPrivKey, payload);

        nlohmann::json direct = claims;
        direct["Signature"] = sig;

        OmniTierLevel tier = OmniTierLevel::Free;
        bool ok = OmniLicense::ValidateOfflineKey(direct.dump(), &tier, testPrivPubKeyB64);
        LogResult("Test 5: Direct payload with embedded signature field", ok && tier == OmniTierLevel::Pro);
    }

    // -------------------------------------------------------------------------
    // TEST 6: Tampered signature rejection
    // -------------------------------------------------------------------------
    {
        nlohmann::json claims;
        claims["Tier"] = "Pro";
        claims["ExpiresAt"] = 2524608000LL;
        claims["BoundHwid"] = "";
        std::string payload = claims.dump();
        std::string sig = SignPayloadWithCNG(hTestPrivKey, payload);

        // Tamper signature
        if (!sig.empty()) sig[sig.size() - 2] = (sig[sig.size() - 2] == 'A') ? 'B' : 'A';

        nlohmann::json token;
        token["Payload"] = payload;
        token["Signature"] = sig;

        OmniTierLevel tier = OmniTierLevel::Free;
        bool ok = OmniLicense::ValidateOfflineKey(token.dump(), &tier, testPrivPubKeyB64);
        LogResult("Test 6: Tampered signature rejection", !ok, "Correctly rejected");
    }

    // -------------------------------------------------------------------------
    // TEST 7: Tampered payload content rejection
    // -------------------------------------------------------------------------
    {
        nlohmann::json claims;
        claims["Tier"] = "Pro";
        claims["ExpiresAt"] = 2524608000LL;
        claims["BoundHwid"] = "";
        std::string payload = claims.dump();
        std::string sig = SignPayloadWithCNG(hTestPrivKey, payload);

        // Tamper payload: change Pro to Enterprise without resigning
        claims["Tier"] = "Enterprise";
        std::string tamperedPayload = claims.dump();

        nlohmann::json token;
        token["Payload"] = tamperedPayload;
        token["Signature"] = sig;

        OmniTierLevel tier = OmniTierLevel::Free;
        bool ok = OmniLicense::ValidateOfflineKey(token.dump(), &tier, testPrivPubKeyB64);
        LogResult("Test 7: Tampered payload content rejection", !ok, "Correctly rejected");
    }

    // -------------------------------------------------------------------------
    // TEST 8: Expired license rejection (Valid sig, expired timestamp)
    // -------------------------------------------------------------------------
    {
        nlohmann::json claims;
        claims["Tier"] = "Pro";
        claims["ExpiresAt"] = 1577836800LL; // 2020-01-01 (expired)
        claims["BoundHwid"] = "";
        std::string payload = claims.dump();
        std::string sig = SignPayloadWithCNG(hTestPrivKey, payload);

        nlohmann::json token;
        token["Payload"] = payload;
        token["Signature"] = sig;

        OmniTierLevel tier = OmniTierLevel::Free;
        bool ok = OmniLicense::ValidateOfflineKey(token.dump(), &tier, testPrivPubKeyB64);
        LogResult("Test 8: Expired license rejection (Valid sig, expired timestamp)", !ok, "Correctly rejected");
    }

    // -------------------------------------------------------------------------
    // TEST 9: Mismatched HWID rejection (Valid sig, foreign HWID)
    // -------------------------------------------------------------------------
    {
        nlohmann::json claims;
        claims["Tier"] = "Pro";
        claims["ExpiresAt"] = 2524608000LL;
        claims["BoundHwid"] = "FOREIGN_MACHINE_HWID_ABC1234567890=";
        std::string payload = claims.dump();
        std::string sig = SignPayloadWithCNG(hTestPrivKey, payload);

        nlohmann::json token;
        token["Payload"] = payload;
        token["Signature"] = sig;

        OmniTierLevel tier = OmniTierLevel::Free;
        bool ok = OmniLicense::ValidateOfflineKey(token.dump(), &tier, testPrivPubKeyB64);
        LogResult("Test 9: Mismatched HWID rejection (Valid sig, foreign HWID)", !ok, "Correctly rejected");
    }

    // -------------------------------------------------------------------------
    // TEST 10: Untrusted RSA key rejected against pinned DelgadoLogic key
    // -------------------------------------------------------------------------
    {
        nlohmann::json claims;
        claims["Tier"] = "Pro";
        claims["ExpiresAt"] = 2524608000LL;
        claims["BoundHwid"] = "";
        std::string payload = claims.dump();
        std::string sig = SignPayloadWithCNG(hTestPrivKey, payload);

        nlohmann::json token;
        token["Payload"] = payload;
        token["Signature"] = sig;

        // Validated against pinned DelgadoLogic public key (without passing custom test key)
        OmniTierLevel tier = OmniTierLevel::Free;
        bool ok = OmniLicense::ValidateOfflineKey(token.dump(), &tier, "");
        LogResult("Test 10: Untrusted RSA key rejected against pinned DelgadoLogic key", !ok, "Correctly rejected");
    }

    // -------------------------------------------------------------------------
    // TEST 11: Malformed Base64 safely rejected with zero crashes
    // -------------------------------------------------------------------------
    {
        std::string garbage = "!!@@##$$%%^^&&**(()__++INVALID_BASE64";
        OmniTierLevel tier = OmniTierLevel::Free;
        bool ok = OmniLicense::ValidateOfflineKey(garbage, &tier, testPrivPubKeyB64);
        LogResult("Test 11: Malformed Base64 safely rejected with zero crashes", !ok);
    }

    // -------------------------------------------------------------------------
    // TEST 12: Corrupted JSON safely rejected with zero crashes
    // -------------------------------------------------------------------------
    {
        std::string brokenJson = "{\"Payload\": { unclosed json string ...";
        OmniTierLevel tier = OmniTierLevel::Free;
        bool ok = OmniLicense::ValidateOfflineKey(brokenJson, &tier, testPrivPubKeyB64);
        LogResult("Test 12: Corrupted JSON safely rejected with zero crashes", !ok);
    }

    // -------------------------------------------------------------------------
    // TEST 13: Empty & Whitespace Input Resilience
    // -------------------------------------------------------------------------
    {
        bool okEmpty = OmniLicense::ValidateOfflineKey("");
        bool okSpace = OmniLicense::ValidateOfflineKey("   \t\r\n  ");
        LogResult("Test 13: Empty & whitespace strings safely rejected", !okEmpty && !okSpace);
    }

    // -------------------------------------------------------------------------
    // TEST 14: Direct Payload Missing Signature Rejection
    // -------------------------------------------------------------------------
    {
        std::string unsignedPayload = "{\"Tier\":\"Pro\",\"ExpiresAt\":2524608000,\"BoundHwid\":\"\"}";
        bool ok = OmniLicense::ValidateOfflineKey(unsignedPayload, nullptr, testPrivPubKeyB64);
        LogResult("Test 14: Unsigned payload safely rejected", !ok);
    }

    // -------------------------------------------------------------------------
    // TEST 15: Offline License File Integration (%APPDATA%\DelgadoLogic\Aeon\license.key)
    // -------------------------------------------------------------------------
    {
        char appDataPath[MAX_PATH] = {0};
        GetEnvironmentVariableA("APPDATA", appDataPath, MAX_PATH);
        std::string baseDir = std::string(appDataPath) + "\\DelgadoLogic\\Aeon";
        std::string keyPath = baseDir + "\\license.key";

        // Backup existing license file if present
        std::string backupPath = keyPath + ".bak_test";
        bool hadExisting = (GetFileAttributesA(keyPath.c_str()) != INVALID_FILE_ATTRIBUTES);
        if (hadExisting) {
            CopyFileA(keyPath.c_str(), backupPath.c_str(), FALSE);
        }

        // Create directory
        CreateDirectoryA((std::string(appDataPath) + "\\DelgadoLogic").c_str(), NULL);
        CreateDirectoryA(baseDir.c_str(), NULL);

        // First test: corrupted key in file -> GetCurrentContext should be Free and invalid
        {
            std::ofstream f(keyPath, std::ios::binary | std::ios::trunc);
            f << "CORRUPTED_KEY_CONTENTS";
            f.close();

            LicenseContext ctx = OmniLicense::GetCurrentContext();
            bool passCorrupt = (!ctx.IsOfflineKeyValid && ctx.Tier == OmniTierLevel::Free && ctx.HardwareID == currentHWID);
            LogResult("Test 15a: Offline file with corrupted content defaults to Free", passCorrupt);
        }

        // Second test: non-existent file -> GetCurrentContext should be Free and invalid
        {
            DeleteFileA(keyPath.c_str());
            LicenseContext ctx = OmniLicense::GetCurrentContext();
            bool passNone = (!ctx.IsOfflineKeyValid && ctx.Tier == OmniTierLevel::Free && ctx.HardwareID == currentHWID);
            LogResult("Test 15b: Missing offline file defaults to Free with valid HWID", passNone);
        }

        // Restore backup if it existed
        if (hadExisting) {
            CopyFileA(backupPath.c_str(), keyPath.c_str(), FALSE);
            DeleteFileA(backupPath.c_str());
        }
    }

    // -------------------------------------------------------------------------
    // TEST 16: Retired SKUs (FounderPass, SovereignSuite, Family) rejected fail-closed
    // -------------------------------------------------------------------------
    {
        bool allRetiredRejected = true;
        for (const char* retiredTier : { "FounderPass", "SovereignSuite", "Family", "UnknownSKU" }) {
            nlohmann::json claims;
            claims["Tier"] = retiredTier;
            claims["ExpiresAt"] = 2524608000LL;
            claims["BoundHwid"] = "";
            std::string payload = claims.dump();
            std::string sig = SignPayloadWithCNG(hTestPrivKey, payload);

            nlohmann::json token;
            token["Payload"] = payload;
            token["Signature"] = sig;

            OmniTierLevel tier = OmniTierLevel::Pro;
            if (OmniLicense::ValidateOfflineKey(token.dump(), &tier, testPrivPubKeyB64)) {
                allRetiredRejected = false;
            }
        }
        LogResult("Test 16: Retired SKUs (FounderPass, SovereignSuite, Family) fail closed", allRetiredRejected);
    }

    // -------------------------------------------------------------------------
    // TEST 17: Valid Community tier extraction
    // -------------------------------------------------------------------------
    {
        nlohmann::json claims;
        claims["Tier"] = "Community";
        claims["ExpiresAt"] = 2524608000LL;
        claims["BoundHwid"] = currentHWID;
        std::string payload = claims.dump();
        std::string sig = SignPayloadWithCNG(hTestPrivKey, payload);

        nlohmann::json token;
        token["Payload"] = payload;
        token["Signature"] = sig;

        OmniTierLevel tier = OmniTierLevel::Free;
        bool ok = OmniLicense::ValidateOfflineKey(token.dump(), &tier, testPrivPubKeyB64);
        LogResult("Test 17: Valid RSA-2048 Community tier extraction", ok && tier == OmniTierLevel::Community);
    }

    // -------------------------------------------------------------------------
    // TEST 18: AutoUpdater sovereign endpoint & canonical sorted-JSON serializer
    // -------------------------------------------------------------------------
    {
        bool endpointOk = (std::string(AutoUpdater::MANIFEST_URL) == "https://api.delgadologic.tech/v1/update/aeon/stable");
        std::string unsorted = "{ \"version\": \"0.22.1\", \"channel\": \"stable\", \"product\": \"aeon\", \"size_bytes\": 22090000 }";
        std::string canonical = AutoUpdater::CanonicalizeJsonString(unsorted);
        std::string expected = "{\"channel\":\"stable\",\"product\":\"aeon\",\"size_bytes\":22090000,\"version\":\"0.22.1\"}";
        bool shaOk = AutoUpdater::IsValidSha256Hex("7fc3fac70fba7964c9a87038a160fb6d20e59d85f37623e7e2cfb02b8b487f74")
                  && !AutoUpdater::IsValidSha256Hex("short")
                  && AutoUpdater::IsTrustedHttpsUrl("https://api.delgadologic.tech/v1/update/aeon/stable")
                  && !AutoUpdater::IsTrustedHttpsUrl("http://insecure.example.com/aeon.exe");
        LogResult("Test 18: AutoUpdater endpoint, canonical sorted JSON & SHA-256/HTTPS gates",
                  endpointOk && (canonical == expected) && shaOk);
    }

    // -------------------------------------------------------------------------
    // TEST 19: RFC 8032 Ed25519 Test Vector + AutoUpdater Envelope & OmniLicense Ed25519
    // -------------------------------------------------------------------------
    {
        // RFC 8032 Section 7.1 TEST 2 (1-byte message 0x72)
        const uint8_t rfcSeed[32] = {
            0x4c,0xcd,0x08,0x9b,0x28,0xff,0x96,0xda,0x9d,0xb6,0xc3,0x46,0xec,0x11,0x4e,0x0f,
            0x5b,0x8a,0x31,0x9f,0x35,0xab,0xa6,0x24,0xda,0x8c,0xf6,0xed,0x4f,0xb8,0xa6,0xfb
        };
        const uint8_t rfcExpectedPub[32] = {
            0x3d,0x40,0x17,0xc3,0xe8,0x43,0x89,0x5a,0x92,0xb7,0x0a,0xa7,0x4d,0x1b,0x7e,0xbc,
            0x9c,0x98,0x2c,0xcf,0x2e,0xc4,0x96,0x8c,0xc0,0xcd,0x55,0xf1,0x2a,0xf4,0x66,0x0c
        };
        const uint8_t rfcExpectedSig[64] = {
            0x92,0xa0,0x09,0xa9,0xf0,0xd4,0xca,0xb8,0x72,0x0e,0x82,0x0b,0x5f,0x64,0x25,0x40,
            0xa2,0xb2,0x7b,0x54,0x16,0x50,0x3f,0x8f,0xb3,0x76,0x22,0x23,0xeb,0xdb,0x69,0xda,
            0x08,0x5a,0xc1,0xe4,0x3e,0x15,0x99,0x6e,0x45,0x8f,0x36,0x13,0xd0,0xf1,0x1d,0x8c,
            0x38,0x7b,0x2e,0xae,0xb4,0x30,0x2a,0xee,0xb0,0x0d,0x29,0x16,0x12,0xbb,0x0c,0x00
        };
        const uint8_t rfcMsg[1] = { 0x72 };

        uint8_t derivedPub[32] = {};
        uint8_t computedSig[64] = {};
        bool rfcPubOk = AeonCrypto::Ed25519::DerivePublicKey(rfcSeed, derivedPub) &&
                        (memcmp(derivedPub, rfcExpectedPub, 32) == 0);
        bool rfcSignOk = AeonCrypto::Ed25519::SignDetached(rfcMsg, 1, rfcSeed, derivedPub, computedSig) &&
                         (memcmp(computedSig, rfcExpectedSig, 64) == 0);
        bool rfcVerifyOk = AeonCrypto::Ed25519::VerifyDetached(rfcMsg, 1, rfcExpectedSig, rfcExpectedPub);

        // Build SPKI 44-byte public key (prefix + 32-byte pubkey) matching SovereignPublicKeyPem format
        BYTE spkiKey[44] = { 0x30, 0x2A, 0x30, 0x05, 0x06, 0x03, 0x2B, 0x65, 0x70, 0x03, 0x21, 0x00 };
        memcpy(spkiKey + 12, derivedPub, 32);
        std::string edSpkiPubB64 = Base64Encode(spkiKey, 44);

        // Sign & verify AutoUpdater canonical JSON manifest envelope
        nlohmann::json manifest;
        manifest["product"] = "aeon";
        manifest["channel"] = "stable";
        manifest["version"] = "0.23.0";
        manifest["status"] = "update_available";
        manifest["download_url"] = "https://api.delgadologic.tech/downloads/AeonSetup.exe";
        manifest["sha256"] = "7fc3fac70fba7964c9a87038a160fb6d20e59d85f37623e7e2cfb02b8b487f74";
        manifest["size_bytes"] = 22091264;

        std::string canonicalManifest = AutoUpdater::CanonicalizeJsonString(manifest.dump());
        uint8_t envSig[64] = {};
        AeonCrypto::Ed25519::SignDetached(
            reinterpret_cast<const uint8_t*>(canonicalManifest.data()),
            canonicalManifest.size(),
            rfcSeed,
            derivedPub,
            envSig);

        nlohmann::json env;
        env["manifest"] = manifest;
        env["signature"] = Base64Encode(envSig, 64);

        AutoUpdater::UpdateManifest verified{};
        bool okValid = AutoUpdater::TryParseAndVerifyEnvelope(env.dump(), verified, edSpkiPubB64);

        // Tamper version after signing -> must fail closed
        env["manifest"]["version"] = "9.9.9";
        AutoUpdater::UpdateManifest tamperedOut{};
        bool okTamperedRejected = !AutoUpdater::TryParseAndVerifyEnvelope(env.dump(), tamperedOut, edSpkiPubB64);

        // Sign & verify OmniLicense Ed25519 offline token
        nlohmann::json edClaims;
        edClaims["Tier"] = "Pro";
        edClaims["ExpiresAt"] = 2524608000LL;
        edClaims["BoundHwid"] = currentHWID;
        std::string edPayload = edClaims.dump();
        uint8_t licSig[64] = {};
        AeonCrypto::Ed25519::SignDetached(
            reinterpret_cast<const uint8_t*>(edPayload.data()),
            edPayload.size(),
            rfcSeed,
            derivedPub,
            licSig);

        nlohmann::json edLicToken;
        edLicToken["Payload"] = edPayload;
        edLicToken["Signature"] = Base64Encode(licSig, 64);
        edLicToken["Alg"] = "Ed25519";
        OmniTierLevel edTier = OmniTierLevel::Free;
        bool okEdLicense = OmniLicense::ValidateOfflineKey(edLicToken.dump(), &edTier, edSpkiPubB64) && (edTier == OmniTierLevel::Pro);

        LogResult("Test 19: RFC 8032 Ed25519 + AutoUpdater canonical envelope + OmniLicense Ed25519",
                  rfcPubOk && rfcSignOk && rfcVerifyOk && okValid && (verified.version == "0.23.0") && okTamperedRejected && okEdLicense);
    }

    // -------------------------------------------------------------------------
    // Summary
    // -------------------------------------------------------------------------
    std::cout << "====================================================================" << std::endl;
    std::cout << " TEST RESULTS: " << g_testsPassed << " PASSED, " << g_testsFailed << " FAILED." << std::endl;
    std::cout << "====================================================================" << std::endl;

    BCryptDestroyKey(hTestPrivKey);
    BCryptCloseAlgorithmProvider(hRsaAlg, 0);

    return (g_testsFailed == 0) ? 0 : 1;
}
