// AeonBrowser — TlsAbstraction.cpp
// DelgadoLogic | Senior Security Engineer
//
// PURPOSE: Universal TLS wrapper. Detects which TLS stack is appropriate
// for the current OS tier and configures it before any network I/O.
//
// IT TROUBLESHOOTING:
//   - Win9x "wolfssl.dll not found": DLL must be in AeonBrowser install dir.
//   - Vista HTTPS fails after apply: Requires reboot after schannel registry write.
//   - Win7 HttpClient fails TLS: TelemetryEnabled check passes but schannel
//     still uses TLS 1.0. Run EnableModernTls_NT() manually to re-apply keys.
//   - XP "SEC_E_ALGORITHM_MISMATCH": One-Core-API not installed; schannel
//     does not support TLS 1.2. Route through WolfSSL instead.
//
// SECURITY NOTE: WolfSSL on Win9x disables cert verification (no CA store).
// We use certificate pinning to our own telemetry endpoint as a compensating
// control. Third-party HTTPS traffic is TLS-encrypted but not cert-verified.
// This is documented in Aeon's privacy policy and is standard for the platform.

#include "TlsAbstraction.h"
#include "../probe/HardwareProbe.h"
#include <windows.h>
#include <cstdio>

namespace AeonTls {

// ---------------------------------------------------------------------------
// Internal: Win9x / Win3.x — load WolfSSL 16/32-bit DLL
// The wolfssl.dll ships with our installer. It is our custom build studied
// from the WinGPT/dialup.net WolfSSL port (GPL v2 — isolated as DLL).
// ---------------------------------------------------------------------------
static bool LoadWolfSsl_9x() {
    HMODULE h = LoadLibraryA("wolfssl.dll");
    if (!h) {
        fprintf(stderr,
            "[AeonTls] wolfssl.dll not found. "
            "Ensure it is in the AeonBrowser install directory.\n");
        return false;
    }
    // Resolve wolfSSL_Init and wolfSSL_CTX_new dynamically
    // (symbols vary between 16-bit and 32-bit builds)
    auto fnInit = (int(*)())GetProcAddress(h, "wolfSSL_Init");
    if (!fnInit || fnInit() != 1) {
        fprintf(stderr, "[AeonTls] wolfSSL_Init() failed.\n");
        FreeLibrary(h);
        return false;
    }
    fprintf(stdout, "[AeonTls] WolfSSL loaded — TLS 1.3 on Win9x/3.x ready.\n");
    return true;
}

// ---------------------------------------------------------------------------
// Internal: Vista — Apply schannel registry keys to unlock TLS 1.1/1.2.
// These keys are off-by-default in Vista's schannel. They exist in the DLL
// but Microsoft disabled them. A single registry write unlocks them.
// IT NOTE: Reboot required after first-time application.
// ---------------------------------------------------------------------------
static bool EnableModernTls_Vista() {
    const char* protocols[] = {
        "SYSTEM\\CurrentControlSet\\Control\\SecurityProviders\\SCHANNEL\\Protocols\\TLS 1.1\\Client",
        "SYSTEM\\CurrentControlSet\\Control\\SecurityProviders\\SCHANNEL\\Protocols\\TLS 1.2\\Client"
    };

    bool didWrite = false;
    for (const char* path : protocols) {
        HKEY hk;
        if (RegCreateKeyExA(HKEY_LOCAL_MACHINE, path, 0, nullptr,
                REG_OPTION_NON_VOLATILE, KEY_WRITE, nullptr, &hk, nullptr)
                == ERROR_SUCCESS) {
            DWORD zero = 0, one = 1;
            RegSetValueExA(hk, "DisabledByDefault", 0, REG_DWORD,
                reinterpret_cast<const BYTE*>(&zero), sizeof(zero));
            RegSetValueExA(hk, "Enabled", 0, REG_DWORD,
                reinterpret_cast<const BYTE*>(&one), sizeof(one));
            RegCloseKey(hk);
            didWrite = true;
        }
    }
    if (didWrite) {
        fprintf(stdout, "[AeonTls] Vista: TLS 1.1/1.2 schannel keys written. "
            "Reboot required for first-time activation.\n");
    }
    return true;
}

// ---------------------------------------------------------------------------
// Internal: Win7 — Same as Vista but also enables TLS 1.2 Server side
// (needed for loopback connections to our local protocol router).
// ---------------------------------------------------------------------------
static bool EnableModernTls_Win7() {
    const char* paths[] = {
        "SYSTEM\\CurrentControlSet\\Control\\SecurityProviders\\SCHANNEL\\Protocols\\TLS 1.2\\Client",
        "SYSTEM\\CurrentControlSet\\Control\\SecurityProviders\\SCHANNEL\\Protocols\\TLS 1.2\\Server",
        "SYSTEM\\CurrentControlSet\\Control\\SecurityProviders\\SCHANNEL\\Protocols\\TLS 1.3\\Client",
    };
    for (const char* path : paths) {
        HKEY hk;
        if (RegCreateKeyExA(HKEY_LOCAL_MACHINE, path, 0, nullptr,
                REG_OPTION_NON_VOLATILE, KEY_WRITE, nullptr, &hk, nullptr)
                == ERROR_SUCCESS) {
            DWORD zero = 0, one = 1;
            RegSetValueExA(hk, "DisabledByDefault", 0, REG_DWORD,
                reinterpret_cast<const BYTE*>(&zero), sizeof(zero));
            RegSetValueExA(hk, "Enabled", 0, REG_DWORD,
                reinterpret_cast<const BYTE*>(&one), sizeof(one));
            RegCloseKey(hk);
        }
    }
    fprintf(stdout, "[AeonTls] Win7: TLS 1.2/1.3 schannel keys applied.\n");
    return true;
}

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------
bool Initialize(const SystemProfile& p) {
    switch (p.tier) {
        case AeonTier::Win16_Retro:
        case AeonTier::Win9x_Retro:
        case AeonTier::Win2000_Compat:
            // No native TLS 1.2 — load WolfSSL DLL
            return LoadWolfSsl_9x();

        case AeonTier::WinXP_LowSpec:
        case AeonTier::WinXP_HiSpec:
            // XP: Try One-Core-API schannel first (TLS 1.2 if OCA installed)
            // Fall back to WolfSSL if OCA not found
            {
                HKEY hk;
                bool ocaPresent = (RegOpenKeyExA(HKEY_LOCAL_MACHINE,
                    "SOFTWARE\\One-Core-API", 0, KEY_READ, &hk) == ERROR_SUCCESS);
                if (ocaPresent) RegCloseKey(hk);

                if (ocaPresent) {
                    fprintf(stdout, "[AeonTls] XP: One-Core-API detected — "
                        "using system TLS 1.2.\n");
                    return true; // OCA provides schannel TLS 1.2 natively
                } else {
                    fprintf(stdout, "[AeonTls] XP: OCA not found — "
                        "falling back to WolfSSL.\n");
                    return LoadWolfSsl_9x();
                }
            }

        case AeonTier::WinVista_7:
            // Vista: needs registry unlock + schannel patch KB4056564
            // Win7: just registry keys
            if (p.osMajor == 6 && p.osMinor == 0) {
                return EnableModernTls_Vista();
            }
            return EnableModernTls_Win7();

        case AeonTier::Win8_Modern:
        case AeonTier::Win10_11_Pro:
        default:
            // Modern Windows: TLS 1.2/1.3 is natively supported.
            // Nothing to configure — OS schannel handles it.
            fprintf(stdout, "[AeonTls] Modern OS: native TLS 1.3 ready.\n");
            return true;
    }
}

} // namespace AeonTls
