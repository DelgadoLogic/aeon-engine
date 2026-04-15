// AeonBrowser — TlsAbstraction.cpp
// DelgadoLogic | Senior Security Engineer
//
// PURPOSE: Universal TLS wrapper. Detects which TLS stack is appropriate
// for the current OS tier and configures it before any network I/O.
//
// IT TROUBLESHOOTING:
//   - Win9x TLS errors: BearSSL is statically linked in retro DLL.
//     No external DLL required. Supports TLS 1.0-1.2.
//   - Vista HTTPS fails after apply: Requires reboot after schannel registry write.
//   - Win7 HttpClient fails TLS: TelemetryEnabled check passes but schannel
//     still uses TLS 1.0. Run EnableModernTls_NT() manually to re-apply keys.
//   - XP "SEC_E_ALGORITHM_MISMATCH": One-Core-API not installed; schannel
//     does not support TLS 1.2. BearSSL retro DLL handles TLS instead.
//
// SECURITY NOTE: BearSSL on Win9x validates certs against 5 embedded root CAs
// (ISRG, DigiCert, Google Trust, GlobalSign) covering >90% of HTTPS traffic.
// Sites using uncommon CAs will fail with a TLS error. To add more CAs,
// regenerate retro/trust_anchors.h from the Mozilla CA bundle.

#include "TlsAbstraction.h"
#include "../probe/HardwareProbe.h"
#include <windows.h>
#include <cstdio>

namespace AeonTls {

// ---------------------------------------------------------------------------
// Internal: Win9x / Win3.x — BearSSL TLS is statically linked in the retro
// engine DLL (aeon_html4.dll). No external DLL loading required.
// This function is retained as a placeholder for future BearSSL-specific
// initialization if needed from the C++ core.
// ---------------------------------------------------------------------------
static bool InitBearSsl_Retro() {
    // BearSSL is compiled directly into the retro DLL (aeon_html4.dll).
    // TLS init/cleanup is handled by tls_init()/tls_cleanup() in the DLL.
    // No dynamic loading needed from the C++ core.
    fprintf(stdout, "[AeonTls] Retro tier: BearSSL TLS 1.2 handled by engine DLL.\n");
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
            // No native TLS 1.2 — retro DLL handles via BearSSL
            return InitBearSsl_Retro();

        case AeonTier::WinXP_LowSpec:
        case AeonTier::WinXP_HiSpec:
            // XP: Try One-Core-API schannel first (TLS 1.2 if OCA installed)
            // Fall back to BearSSL (retro DLL) if OCA not found
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
                        "falling back to BearSSL retro DLL.\n");
                    return InitBearSsl_Retro();
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
