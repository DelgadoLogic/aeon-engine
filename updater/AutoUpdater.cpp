#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#define _WINSOCKAPI_
// AeonBrowser — AutoUpdater.cpp v2.1
// Sovereign Autonomous Update System (Canonical Sorted-JSON Ed25519 + SHA-256)
// Matches OmniCore.Engine.AutoUpdateEngine and Infrastructure/aeon-update-server/main.py.

#include "AutoUpdater.h"
#include "../core/AeonVersion.h"
#include "../core/settings/SettingsEngine.h"
#include "../core/session/nlohmann/json.hpp"
#include "../core/crypto/Ed25519.h"
#include <windows.h>
#include <shellapi.h>
#include <wininet.h>
#include <wincrypt.h>
#include <wintrust.h>
#include <softpub.h>
#include <winsock2.h>
#include <ws2tcpip.h>
#include <shlobj.h>
#include <bcrypt.h>
#include <cstdio>
#include <cstring>
#include <cctype>
#include <algorithm>
#include <string>
#include <vector>
#include <thread>
#include <atomic>
#include <mutex>
#include <filesystem>

#pragma comment(lib, "wininet.lib")
#pragma comment(lib, "crypt32.lib")
#pragma comment(lib, "wintrust.lib")
#pragma comment(lib, "ws2_32.lib")
#pragma comment(lib, "bcrypt.lib")

#ifndef BCRYPT_SUCCESS
#define BCRYPT_SUCCESS(Status) (((NTSTATUS)(Status)) >= 0)
#endif

#ifndef BCRYPT_EDDSA_PUBLIC_GENERIC_MAGIC
#define BCRYPT_EDDSA_PUBLIC_GENERIC_MAGIC 0x47444445  // 'EDDG'
#endif

namespace AutoUpdater {

namespace fs = std::filesystem;

static constexpr char  CURRENT_VERSION[]  = AEON_VERSION;
static constexpr DWORD CHUNK_SIZE         = 4 * 1024 * 1024;  // 4MB
static constexpr int   HIVE_PORT          = 9797;
static constexpr DWORD MAX_BW_ACTIVE_KBS  = 64;    // KB/s when user active
static constexpr DWORD MAX_BW_IDLE_KBS    = 10240; // KB/s when user idle (10MB/s)
static constexpr DWORD IDLE_THRESHOLD_MS  = 300000; // 5 minutes = "idle"

static const BYTE kEd25519SpkiPrefix[12] = {
    0x30, 0x2A, 0x30, 0x05, 0x06, 0x03, 0x2B, 0x65, 0x70, 0x03, 0x21, 0x00
};

static std::atomic<bool>        g_running{false};
static std::atomic<float>       g_download_progress{0.0f};
static std::atomic<UpdateState> g_state{UpdateState::Idle};
static std::mutex               g_mutex;
static UpdateInfo               g_pending_info{};
static std::string              g_staging_dir;

// ── Idle detection ────────────────────────────────────────────────────────────

static DWORD GetUserIdleMs() {
    LASTINPUTINFO lii = {};
    lii.cbSize = sizeof(LASTINPUTINFO);
    if (GetLastInputInfo(&lii))
        return GetTickCount() - lii.dwTime;
    return 0;
}

static bool IsUserActive() {
    return GetUserIdleMs() < IDLE_THRESHOLD_MS;
}

static DWORD GetBandwidthCapBytesPerSec() {
    DWORD kbps = IsUserActive() ? MAX_BW_ACTIVE_KBS : MAX_BW_IDLE_KBS;
    return kbps * 1024;
}

// ── Integrity & URL Helpers (Fail-Closed) ─────────────────────────────────────

bool IsTrustedHttpsUrl(const std::string& url) {
    if (url.size() <= 8) return false;
    std::string prefix = url.substr(0, 8);
    std::transform(prefix.begin(), prefix.end(), prefix.begin(), [](unsigned char c) {
        return (char)std::tolower(c);
    });
    if (prefix != "https://") return false;
    return url[8] != '/' && url[8] != ' ' && url[8] != '\t';
}

bool IsValidSha256Hex(const std::string& hash) {
    if (hash.size() != 64) return false;
    for (unsigned char c : hash) {
        bool ok = (c >= '0' && c <= '9') ||
                  (c >= 'a' && c <= 'f') ||
                  (c >= 'A' && c <= 'F');
        if (!ok) return false;
    }
    return true;
}

static bool EqualsHexCaseInsensitive(const std::string& a, const std::string& b) {
    if (a.size() != b.size()) return false;
    unsigned char diff = 0;
    for (size_t i = 0; i < a.size(); i++) {
        unsigned char ca = (unsigned char)std::tolower((unsigned char)a[i]);
        unsigned char cb = (unsigned char)std::tolower((unsigned char)b[i]);
        diff |= (ca ^ cb);
    }
    return diff == 0;
}

static bool ComputeFileSHA256(const std::wstring& path, std::string& out_hex) {
    BCRYPT_ALG_HANDLE hAlg = nullptr;
    BCRYPT_HASH_HANDLE hHash = nullptr;
    DWORD hashLen = 0, resultLen = 0;

    if (!BCRYPT_SUCCESS(BCryptOpenAlgorithmProvider(&hAlg, BCRYPT_SHA256_ALGORITHM, nullptr, 0)))
        return false;

    BCryptGetProperty(hAlg, BCRYPT_HASH_LENGTH, (PUCHAR)&hashLen, sizeof(hashLen), &resultLen, 0);
    std::vector<BYTE> hashBuf(hashLen);

    if (!BCRYPT_SUCCESS(BCryptCreateHash(hAlg, &hHash, nullptr, 0, nullptr, 0, 0))) {
        BCryptCloseAlgorithmProvider(hAlg, 0);
        return false;
    }

    FILE* f = nullptr;
    _wfopen_s(&f, path.c_str(), L"rb");
    if (!f) {
        BCryptDestroyHash(hHash);
        BCryptCloseAlgorithmProvider(hAlg, 0);
        return false;
    }

    BYTE buf[65536];
    size_t n;
    while ((n = fread(buf, 1, sizeof(buf), f)) > 0)
        BCryptHashData(hHash, buf, (ULONG)n, 0);
    fclose(f);

    BCryptFinishHash(hHash, hashBuf.data(), hashLen, 0);
    BCryptDestroyHash(hHash);
    BCryptCloseAlgorithmProvider(hAlg, 0);

    char hex[65] = {};
    for (DWORD i = 0; i < hashLen; i++)
        sprintf_s(hex + i * 2, 3, "%02x", hashBuf[i]);
    out_hex = hex;
    return true;
}

// ── Canonical Sorted-Key JSON & Ed25519 Verification ──────────────────────────
// Matches Python json.dumps(obj, sort_keys=True, separators=(",", ":"))
// and OmniCore.Engine.AutoUpdateEngine.BuildCanonicalManifestJson.

static void AppendCanonicalEscapedString(std::string& out, const std::string& utf8) {
    out.push_back('"');
    size_t i = 0;
    while (i < utf8.size()) {
        unsigned char c = (unsigned char)utf8[i];
        if (c == '"') { out += "\\\""; i++; }
        else if (c == '\\') { out += "\\\\"; i++; }
        else if (c == '\b') { out += "\\b"; i++; }
        else if (c == '\f') { out += "\\f"; i++; }
        else if (c == '\n') { out += "\\n"; i++; }
        else if (c == '\r') { out += "\\r"; i++; }
        else if (c == '\t') { out += "\\t"; i++; }
        else if (c < 0x20) {
            char esc[7];
            sprintf_s(esc, sizeof(esc), "\\u%04x", (unsigned)c);
            out += esc;
            i++;
        } else if (c <= 0x7E) {
            out.push_back((char)c);
            i++;
        } else {
            // Decode UTF-8 multi-byte sequence to Unicode codepoint and escape as \uXXXX
            uint32_t cp = 0;
            size_t adv = 1;
            if ((c & 0xE0) == 0xC0 && i + 1 < utf8.size()) {
                cp = ((c & 0x1F) << 6) | ((unsigned char)utf8[i + 1] & 0x3F);
                adv = 2;
            } else if ((c & 0xF0) == 0xE0 && i + 2 < utf8.size()) {
                cp = ((c & 0x0F) << 12) | (((unsigned char)utf8[i + 1] & 0x3F) << 6) | ((unsigned char)utf8[i + 2] & 0x3F);
                adv = 3;
            } else if ((c & 0xF8) == 0xF0 && i + 3 < utf8.size()) {
                cp = ((c & 0x07) << 18) | (((unsigned char)utf8[i + 1] & 0x3F) << 12) |
                     (((unsigned char)utf8[i + 2] & 0x3F) << 6) | ((unsigned char)utf8[i + 3] & 0x3F);
                adv = 4;
            } else {
                cp = c;
                adv = 1;
            }
            if (cp <= 0xFFFF) {
                char esc[7];
                sprintf_s(esc, sizeof(esc), "\\u%04x", cp);
                out += esc;
            } else {
                cp -= 0x10000;
                uint32_t high = 0xD800 + ((cp >> 10) & 0x3FF);
                uint32_t low  = 0xDC00 + (cp & 0x3FF);
                char esc[13];
                sprintf_s(esc, sizeof(esc), "\\u%04x\\u%04x", high, low);
                out += esc;
            }
            i += adv;
        }
    }
    out.push_back('"');
}

static void AppendCanonicalElement(std::string& out, const nlohmann::json& elem) {
    if (elem.is_object()) {
        out.push_back('{');
        // nlohmann::json object uses std::map<std::string, json>, which iterates in sorted key order
        bool first = true;
        for (auto it = elem.begin(); it != elem.end(); ++it) {
            if (!first) out.push_back(',');
            first = false;
            AppendCanonicalEscapedString(out, it.key());
            out.push_back(':');
            AppendCanonicalElement(out, it.value());
        }
        out.push_back('}');
    } else if (elem.is_array()) {
        out.push_back('[');
        for (size_t i = 0; i < elem.size(); i++) {
            if (i > 0) out.push_back(',');
            AppendCanonicalElement(out, elem[i]);
        }
        out.push_back(']');
    } else if (elem.is_string()) {
        AppendCanonicalEscapedString(out, elem.get_ref<const std::string&>());
    } else if (elem.is_boolean()) {
        out += elem.get<bool>() ? "true" : "false";
    } else if (elem.is_number_integer()) {
        out += std::to_string(elem.get<int64_t>());
    } else if (elem.is_number_unsigned()) {
        out += std::to_string(elem.get<uint64_t>());
    } else if (elem.is_number_float()) {
        out += elem.dump();
    } else {
        out += "null";
    }
}

std::string CanonicalizeJsonString(const std::string& rawManifestObjectJson) {
    try {
        nlohmann::json j = nlohmann::json::parse(rawManifestObjectJson);
        if (!j.is_object()) return "";
        std::string canonical;
        canonical.reserve(rawManifestObjectJson.size());
        AppendCanonicalElement(canonical, j);
        return canonical;
    } catch (...) {
        return "";
    }
}

static bool Base64DecodeBytes(const std::string& input, std::vector<BYTE>& out) {
    if (input.empty()) return false;
    DWORD outLen = 0;
    if (!CryptStringToBinaryA(input.c_str(), (DWORD)input.size(), CRYPT_STRING_BASE64_ANY, nullptr, &outLen, nullptr, nullptr) || outLen == 0) {
        return false;
    }
    out.resize(outLen);
    if (!CryptStringToBinaryA(input.c_str(), (DWORD)input.size(), CRYPT_STRING_BASE64_ANY, out.data(), &outLen, nullptr, nullptr)) {
        return false;
    }
    out.resize(outLen);
    return true;
}

static bool HexDecodeBytes(const std::string& hex, std::vector<BYTE>& out) {
    if (hex.empty() || (hex.size() % 2) != 0) return false;
    out.resize(hex.size() / 2);
    for (size_t i = 0; i < hex.size(); i += 2) {
        char byte_str[3] = { hex[i], hex[i + 1], '\0' };
        char* end = nullptr;
        unsigned long val = strtoul(byte_str, &end, 16);
        if (end != byte_str + 2) return false;
        out[i / 2] = (BYTE)val;
    }
    return true;
}

static bool DecodePublicKey32(const std::string& keyMaterial, BYTE outKey[32]) {
    std::vector<BYTE> bytes;
    if (keyMaterial.size() == 64 && HexDecodeBytes(keyMaterial, bytes) && bytes.size() == 32) {
        memcpy(outKey, bytes.data(), 32);
        return true;
    }
    if (Base64DecodeBytes(keyMaterial, bytes)) {
        if (bytes.size() == 32) {
            memcpy(outKey, bytes.data(), 32);
            return true;
        }
        if (bytes.size() == 44 && memcmp(bytes.data(), kEd25519SpkiPrefix, 12) == 0) {
            memcpy(outKey, bytes.data() + 12, 32);
            return true;
        }
    }
    return false;
}

static bool DecodeSignature64(const std::string& sigText, std::vector<BYTE>& outSig) {
    if (sigText.size() == 128 && HexDecodeBytes(sigText, outSig) && outSig.size() == 64) {
        return true;
    }
    if (Base64DecodeBytes(sigText, outSig) && outSig.size() == 64) {
        return true;
    }
    return false;
}

static bool VerifyEd25519Payload(const std::string& canonicalJson, const std::string& sigText, const std::string& pubKeyMaterial) {
    BYTE pubKey32[32] = {};
    if (!DecodePublicKey32(pubKeyMaterial, pubKey32)) {
        fprintf(stderr, "[AutoUpdater] Invalid Ed25519 public key format — REJECTED\n");
        return false;
    }

    std::vector<BYTE> sigRaw;
    if (!DecodeSignature64(sigText, sigRaw)) {
        fprintf(stderr, "[AutoUpdater] Invalid Ed25519 signature encoding — REJECTED\n");
        return false;
    }

    return AeonCrypto::Ed25519::VerifyDetached(
        reinterpret_cast<const uint8_t*>(canonicalJson.data()),
        canonicalJson.size(),
        sigRaw.data(),
        pubKey32);
}

bool TryParseAndVerifyEnvelope(
    const std::string& rawEnvelopeJson,
    UpdateManifest& outManifest,
    const std::string& pubKeyOverrideB64)
{
    outManifest = UpdateManifest{};
    if (rawEnvelopeJson.empty()) return false;

    try {
        // Strip UTF-8 BOM if present
        std::string cleaned = rawEnvelopeJson;
        if (cleaned.size() >= 3 &&
            (unsigned char)cleaned[0] == 0xEF &&
            (unsigned char)cleaned[1] == 0xBB &&
            (unsigned char)cleaned[2] == 0xBF) {
            cleaned.erase(0, 3);
        }

        nlohmann::json root = nlohmann::json::parse(cleaned);
        if (!root.is_object()) return false;

        if (!root.contains("manifest") || !root["manifest"].is_object()) return false;
        if (!root.contains("signature") || !root["signature"].is_string()) return false;

        const nlohmann::json& mObj = root["manifest"];
        std::string sigText = root["signature"].get<std::string>();
        if (sigText.empty()) return false;

        std::string canonicalPayload;
        AppendCanonicalElement(canonicalPayload, mObj);

        std::string keyMat = pubKeyOverrideB64.empty() ? SOVEREIGN_ED25519_PUBKEY_B64 : pubKeyOverrideB64;
        if (!VerifyEd25519Payload(canonicalPayload, sigText, keyMat)) {
            fprintf(stderr, "[AutoUpdater] Ed25519 envelope signature INVALID — REJECTING manifest\n");
            return false;
        }

        if (!mObj.contains("version") || !mObj["version"].is_string()) return false;
        outManifest.version = mObj["version"].get<std::string>();
        if (outManifest.version.empty()) return false;

        if (mObj.contains("product") && mObj["product"].is_string())
            outManifest.product = mObj["product"].get<std::string>();
        if (mObj.contains("channel") && mObj["channel"].is_string())
            outManifest.channel = mObj["channel"].get<std::string>();
        if (mObj.contains("status") && mObj["status"].is_string())
            outManifest.status = mObj["status"].get<std::string>();
        if (mObj.contains("download_url") && mObj["download_url"].is_string())
            outManifest.download_url = mObj["download_url"].get<std::string>();
        if (mObj.contains("release_notes") && mObj["release_notes"].is_string())
            outManifest.release_notes = mObj["release_notes"].get<std::string>();
        if (mObj.contains("sha256") && mObj["sha256"].is_string())
            outManifest.sha256 = mObj["sha256"].get<std::string>();
        if (mObj.contains("size_bytes") && mObj["size_bytes"].is_number_integer())
            outManifest.size_bytes = mObj["size_bytes"].get<int64_t>();
        outManifest.signature = sigText;

        if (mObj.contains("chunks") && mObj["chunks"].is_array()) {
            for (const auto& item : mObj["chunks"]) {
                if (item.is_string() && !item.get_ref<const std::string&>().empty()) {
                    outManifest.chunk_urls.push_back(item.get<std::string>());
                }
            }
        } else if (mObj.contains("chunk_urls") && mObj["chunk_urls"].is_array()) {
            for (const auto& item : mObj["chunk_urls"]) {
                if (item.is_string() && !item.get_ref<const std::string&>().empty()) {
                    outManifest.chunk_urls.push_back(item.get<std::string>());
                }
            }
        }

        // If single HTTPS binary URL is provided without explicit P2P chunk array, treat download_url as chunk 0
        if (outManifest.chunk_urls.empty() && !outManifest.download_url.empty()) {
            outManifest.chunk_urls.push_back(outManifest.download_url);
        }

        // When an actual update is advertised, enforce HTTPS URLs + valid 64-hex SHA-256 (fail-closed)
        if (outManifest.status != "no_update" && outManifest.version != "0.0.0") {
            if (!IsValidSha256Hex(outManifest.sha256)) {
                fprintf(stderr, "[AutoUpdater] Manifest for v%s rejected: missing valid 64-hex sha256\n",
                        outManifest.version.c_str());
                return false;
            }
            if (outManifest.chunk_urls.empty()) {
                fprintf(stderr, "[AutoUpdater] Manifest for v%s rejected: missing download_url\n",
                        outManifest.version.c_str());
                return false;
            }
            for (const auto& u : outManifest.chunk_urls) {
                if (!IsTrustedHttpsUrl(u)) {
                    fprintf(stderr, "[AutoUpdater] Manifest for v%s rejected: untrusted non-HTTPS URL\n",
                            outManifest.version.c_str());
                    return false;
                }
            }
        }

        return true;
    } catch (...) {
        return false;
    }
}

// ── Manifest fetch (HTTPS via api.delgadologic.tech, silent) ──────────────────

static bool FetchAndVerifyManifest(UpdateManifest& out) {
    HINTERNET hNet = InternetOpenA(
        "AeonUpdater/2.1 (Windows; DelgadoLogic)",
        INTERNET_OPEN_TYPE_PRECONFIG, nullptr, nullptr, 0);
    if (!hNet) return false;

    const char* headers = "Accept: application/json\r\nX-Client-Product: aeon\r\n";
    HINTERNET hReq = InternetOpenUrlA(
        hNet, MANIFEST_URL, headers, (DWORD)strlen(headers),
        INTERNET_FLAG_SECURE | INTERNET_FLAG_RELOAD |
        INTERNET_FLAG_NO_CACHE_WRITE | INTERNET_FLAG_NO_UI, 0);

    if (!hReq) {
        InternetCloseHandle(hNet);
        return false;
    }

    std::string rawResp;
    char buf[4096];
    DWORD bytesRead = 0;
    while (InternetReadFile(hReq, buf, sizeof(buf), &bytesRead) && bytesRead > 0) {
        rawResp.append(buf, bytesRead);
        if (rawResp.size() > 262144) break; // 256 KB safety cap on manifest JSON
    }

    InternetCloseHandle(hReq);
    InternetCloseHandle(hNet);

    if (rawResp.empty()) return false;
    return TryParseAndVerifyEnvelope(rawResp, out);
}

// ── P2P chunk download from AeonHive peers ────────────────────────────────────

static bool DownloadChunkFromPeer(const std::string& peer_ip,
                                  int chunk_index, int chunk_total,
                                  std::vector<BYTE>& out_data) {
    SOCKET s = socket(AF_INET, SOCK_STREAM, 0);
    if (s == INVALID_SOCKET) return false;

    DWORD timeout = 3000;
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, (char*)&timeout, sizeof(timeout));
    setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, (char*)&timeout, sizeof(timeout));

    int prio = 0;
    setsockopt(s, IPPROTO_IP, IP_TOS, (char*)&prio, sizeof(prio));

    sockaddr_in addr = {};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(9798);
    if (inet_pton(AF_INET, peer_ip.c_str(), &addr.sin_addr) != 1) {
        closesocket(s);
        return false;
    }

    if (connect(s, (sockaddr*)&addr, sizeof(addr)) != 0) {
        closesocket(s);
        return false;
    }

    char req[128];
    _snprintf_s(req, sizeof(req), _TRUNCATE,
        "GET_CHUNK %s %d %d\n", CURRENT_VERSION, chunk_index, chunk_total);
    send(s, req, (int)strlen(req), 0);

    out_data.clear();
    out_data.reserve(CHUNK_SIZE);
    char buf[16384];
    int n;
    DWORD cap = GetBandwidthCapBytesPerSec();
    DWORD bytes_this_sec = 0;
    DWORD sec_start = GetTickCount();

    while ((n = recv(s, buf, sizeof(buf), 0)) > 0) {
        out_data.insert(out_data.end(), buf, buf + n);

        bytes_this_sec += n;
        DWORD elapsed = GetTickCount() - sec_start;
        if (elapsed < 1000 && bytes_this_sec > cap) {
            Sleep(1000 - elapsed);
            sec_start = GetTickCount();
            bytes_this_sec = 0;
        }

        if (out_data.size() >= CHUNK_SIZE * 2) break;
    }

    closesocket(s);
    return out_data.size() > 1024;
}

// ── HTTPS fallback chunk/binary download ──────────────────────────────────────

static bool DownloadChunkFromHttps(const std::string& url,
                                   std::vector<BYTE>& out_data) {
    if (!IsTrustedHttpsUrl(url)) return false;

    HINTERNET hNet = InternetOpenA("AeonUpdater/2.1 (Windows; DelgadoLogic)",
        INTERNET_OPEN_TYPE_PRECONFIG, nullptr, nullptr, 0);
    if (!hNet) return false;

    HINTERNET hReq = InternetOpenUrlA(hNet, url.c_str(), nullptr, 0,
        INTERNET_FLAG_SECURE | INTERNET_FLAG_RELOAD | INTERNET_FLAG_NO_UI, 0);
    if (!hReq) { InternetCloseHandle(hNet); return false; }

    out_data.clear();
    char buf[65536];
    DWORD read = 0;
    DWORD cap = GetBandwidthCapBytesPerSec();
    DWORD bytes_this_sec = 0, sec_start = GetTickCount();

    while (InternetReadFile(hReq, buf, sizeof(buf), &read) && read > 0) {
        out_data.insert(out_data.end(), buf, buf + read);

        bytes_this_sec += read;
        DWORD elapsed = GetTickCount() - sec_start;
        if (elapsed < 1000 && bytes_this_sec > cap) {
            Sleep(1000 - elapsed);
            sec_start = GetTickCount();
            bytes_this_sec = 0;
        }
    }

    InternetCloseHandle(hReq);
    InternetCloseHandle(hNet);
    return !out_data.empty();
}

// ── Staging directory ─────────────────────────────────────────────────────────

static std::string GetStagingDir() {
    wchar_t* appdata = nullptr;
    if (FAILED(SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, nullptr, &appdata)) || !appdata) {
        return "";
    }
    std::wstring path = std::wstring(appdata) + L"\\Aeon\\staging";
    CoTaskMemFree(appdata);
    fs::create_directories(path);
    char narrow[MAX_PATH] = {};
    WideCharToMultiByte(CP_UTF8, 0, path.c_str(), -1, narrow, MAX_PATH, nullptr, nullptr);
    return std::string(narrow);
}

// ── Main download loop ────────────────────────────────────────────────────────

static bool DownloadUpdate(const UpdateManifest& manifest,
                           const std::vector<std::string>& known_peers) {
    g_staging_dir = GetStagingDir();
    if (g_staging_dir.empty() || manifest.chunk_urls.empty() || !IsValidSha256Hex(manifest.sha256)) {
        return false;
    }

    int total_chunks = (int)manifest.chunk_urls.size();
    fprintf(stdout, "[AutoUpdater] Starting verified download: v%s (%d segment(s))\n",
            manifest.version.c_str(), total_chunks);

    std::string assembled_path = g_staging_dir + "\\aeon_next.tmp";
    FILE* out = nullptr;
    fopen_s(&out, assembled_path.c_str(), "wb");
    if (!out) return false;

    for (int i = 0; i < total_chunks; i++) {
        while (IsUserActive()) {
            Sleep(30000);
        }

        g_download_progress = (float)i / (float)total_chunks;

        std::vector<BYTE> chunk_data;
        bool got_chunk = false;

        if (total_chunks > 1) {
            for (const auto& peer : known_peers) {
                if (DownloadChunkFromPeer(peer, i, total_chunks, chunk_data)) {
                    got_chunk = true;
                    break;
                }
            }
        }

        if (!got_chunk) {
            if (!DownloadChunkFromHttps(manifest.chunk_urls[i], chunk_data)) {
                fclose(out);
                DeleteFileA(assembled_path.c_str());
                return false;
            }
        }

        fwrite(chunk_data.data(), 1, chunk_data.size(), out);
    }

    fclose(out);
    g_download_progress = 1.0f;

    // Verify full binary SHA-256 (constant-time / case-insensitive 64-hex comparison)
    int wideLen = MultiByteToWideChar(CP_UTF8, 0, assembled_path.c_str(), -1, nullptr, 0);
    std::wstring w_path(wideLen > 0 ? wideLen - 1 : 0, L'\0');
    if (wideLen > 1) {
        MultiByteToWideChar(CP_UTF8, 0, assembled_path.c_str(), -1, &w_path[0], wideLen);
    }

    std::string actual_sha;
    if (!ComputeFileSHA256(w_path, actual_sha)) {
        fprintf(stderr, "[AutoUpdater] SHA-256 computation failed\n");
        DeleteFileA(assembled_path.c_str());
        return false;
    }

    if (!EqualsHexCaseInsensitive(actual_sha, manifest.sha256)) {
        fprintf(stderr, "[AutoUpdater] SHA-256 MISMATCH — update tampered! Deleting payload.\n");
        DeleteFileA(assembled_path.c_str());
        return false;
    }

    fprintf(stdout, "[AutoUpdater] ✓ SHA-256 verified: %s\n", actual_sha.c_str());

    std::string final_path = g_staging_dir + "\\aeon_" + manifest.version + ".exe";
    MoveFileExA(assembled_path.c_str(), final_path.c_str(), MOVEFILE_REPLACE_EXISTING);

    {
        std::lock_guard<std::mutex> lock(g_mutex);
        g_pending_info.update_available = true;
        g_pending_info.version = manifest.version;
        g_pending_info.download_url = manifest.download_url;
        g_pending_info.sha256 = manifest.sha256;
    }

    std::string marker = g_staging_dir + "\\update_ready.txt";
    FILE* mf = nullptr;
    fopen_s(&mf, marker.c_str(), "w");
    if (mf) {
        fprintf(mf, "%s\n%s\n", manifest.version.c_str(), final_path.c_str());
        fclose(mf);
    }

    g_state = UpdateState::ReadyToInstall;
    fprintf(stdout, "[AutoUpdater] Update staged. Will install on next cold start.\n");
    return true;
}

// ── Cold start installer (call BEFORE creating any windows) ──────────────────

void CheckAndInstallStagedUpdate() {
    std::string staging = GetStagingDir();
    if (staging.empty()) return;
    std::string marker = staging + "\\update_ready.txt";

    FILE* mf = nullptr;
    fopen_s(&mf, marker.c_str(), "r");
    if (!mf) return;

    char version[64] = {}, path[MAX_PATH] = {};
    fscanf_s(mf, "%63s\n%259s", version, (unsigned)sizeof(version), path, (unsigned)sizeof(path));
    fclose(mf);

    if (!path[0]) return;

    wchar_t wPath[MAX_PATH] = {};
    MultiByteToWideChar(CP_UTF8, 0, path, -1, wPath, MAX_PATH);
    if (GetFileAttributesW(wPath) == INVALID_FILE_ATTRIBUTES) return;

    fprintf(stdout, "[AutoUpdater] Applying staged update %s on cold start\n", version);

    wchar_t current_exe[MAX_PATH] = {};
    GetModuleFileNameW(nullptr, current_exe, MAX_PATH);

    std::wstring backup = std::wstring(current_exe) + L".old";
    MoveFileExW(current_exe, backup.c_str(), MOVEFILE_REPLACE_EXISTING);
    MoveFileExW(wPath, current_exe, MOVEFILE_REPLACE_EXISTING);

    DeleteFileA(marker.c_str());
    DeleteFileW(backup.c_str());

    HKEY hKey = nullptr;
    if (RegOpenKeyExA(HKEY_LOCAL_MACHINE,
        "SOFTWARE\\DelgadoLogic\\Aeon", 0, KEY_SET_VALUE, &hKey) == ERROR_SUCCESS) {
        RegSetValueExA(hKey, "Version", 0, REG_SZ,
            (const BYTE*)version, (DWORD)strlen(version) + 1);
        RegCloseKey(hKey);
    }
}

// ── Background poller ─────────────────────────────────────────────────────────

void StartBackgroundPoller(std::vector<std::string> known_peers) {
    g_running = true;
    std::thread([peers = std::move(known_peers)]() mutable {
        SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_IDLE);
        SetThreadDescription(GetCurrentThread(), L"AeonUpdatePoller");

        Sleep(60000);

        while (g_running.load()) {
            if (g_state != UpdateState::Downloading) {
                g_state = UpdateState::Checking;
                UpdateManifest manifest;
                if (FetchAndVerifyManifest(manifest)) {
                    if (manifest.status == "no_update") {
                        g_state = UpdateState::Idle;
                    } else {
                        int cMaj = 0, cMin = 0, cPat = 0, nMaj = 0, nMin = 0, nPat = 0;
                        sscanf_s(CURRENT_VERSION, "%d.%d.%d", &cMaj, &cMin, &cPat);
                        sscanf_s(manifest.version.c_str(), "%d.%d.%d", &nMaj, &nMin, &nPat);
                        bool newer = (nMaj > cMaj) ||
                                     (nMaj == cMaj && nMin > cMin) ||
                                     (nMaj == cMaj && nMin == cMin && nPat > cPat);

                        if (newer && g_state != UpdateState::ReadyToInstall) {
                            g_state = UpdateState::Downloading;
                            while (IsUserActive() && g_running.load()) Sleep(60000);
                            if (g_running.load()) {
                                if (!DownloadUpdate(manifest, peers)) {
                                    g_state = UpdateState::Failed;
                                }
                            }
                        } else if (g_state != UpdateState::ReadyToInstall) {
                            g_state = UpdateState::Idle;
                        }
                    }
                } else if (g_state != UpdateState::ReadyToInstall) {
                    g_state = UpdateState::Idle;
                }
            }
            for (int i = 0; i < 360 && g_running.load(); i++)
                Sleep(60000);
        }
    }).detach();
}

UpdateState GetState()    { return g_state.load(); }
float       GetProgress() { return g_download_progress.load(); }
std::string GetStagedVersion() {
    std::lock_guard<std::mutex> lock(g_mutex);
    return g_pending_info.version;
}

void Start(std::vector<std::string> hive_peers) {
    StartBackgroundPoller(std::move(hive_peers));
}

void Shutdown() {
    g_running = false;
}

} // namespace AutoUpdater
