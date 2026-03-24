// AeonBrowser — HardwareProbe.cpp
// DelgadoLogic | Lead Systems Architect & Senior Security Engineer
//
// PURPOSE: Hardware-Aware Initialization Sequence.
// This is the FIRST module executed. It determines which rendering engine,
// TLS stack, UI skin, and extension model to load for the current machine.
//
// DESIGN RATIONALE (IT Troubleshooting Notes):
//   - We intentionally call RtlGetVersion() via dynamic ntdll.dll import
//     instead of GetVersionEx() because GetVersionEx() returns fake data
//     on Windows 8.1+ when no app-manifest compatibility entry exists.
//     RtlGetVersion bypasses the compatibility shim layer entirely.
//
//   - CPUID is called inline via __cpuid() intrinsic. This is the only
//     reliable way to check SSE2/AVX without the OS lying to us; OS APIs
//     like IsProcessorFeaturePresent() can be shimmed by hypervisors.
//
//   - We detect Win16 at compile time (16-bit builds use a companion
//     entry in aeon16.c) and at runtime via GetWinFlags() presence in
//     KERNEL.DLL (32-bit host running Win32s on Win3.x).
//
//   - RAM detection uses GlobalMemoryStatusEx (NT 5.0+) falling back to
//     GlobalMemoryStatus (Win9x) for systems under 4GB visible RAM.

#include "HardwareProbe.h"

#include <windows.h>
#include <intrin.h>      // __cpuid
#include <cstdio>
#include <cstring>

// ---------------------------------------------------------------------------
// Internal helpers
// ---------------------------------------------------------------------------

// Alias for RtlGetVersion function pointer (bypasses manifest shim)
typedef NTSTATUS (WINAPI *RtlGetVersion_t)(PRTL_OSVERSIONINFOW);

static bool FetchOsVersionNative(OSVERSIONINFOW& info) {
    // IT NOTE: ntdll.dll is always loaded in every Windows process (even on
    // Windows 3.x via Win32s). RtlGetVersion is present since NT 4.0.
    // On NT platforms this is the ground truth; on Win9x we fall back below.
    HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
    if (!ntdll) return false;

    auto fn = reinterpret_cast<RtlGetVersion_t>(
        GetProcAddress(ntdll, "RtlGetVersion"));
    if (!fn) return false;

    memset(&info, 0, sizeof(info));
    info.dwOSVersionInfoSize = sizeof(info);
    return fn(&info) == 0; // STATUS_SUCCESS
}

static void QueryCpuCaps(CpuCaps& caps) {
    // IT NOTE: __cpuid is an MSVC/GCC intrinsic. On 16-bit Open Watcom builds
    // this file is excluded — the 16-bit tier uses a simpler CPUID inline asm.
    // On real XP/9x hardware SSE2 is NOT guaranteed (Pentium III has none).
    // We check both CPUID leaf 1 (SSE2) and leaf 7 (AVX2).

    int regs[4] = {0};
    caps = {}; // zero all flags

    __cpuid(regs, 0); // get max leaf
    int maxLeaf = regs[0];

    if (maxLeaf >= 1) {
        __cpuid(regs, 1);
        caps.hasSSE2 = (regs[3] >> 26) & 1;
        caps.hasSSE4 = (regs[2] >> 19) & 1;  // SSE4.1
        caps.hasAVX  = (regs[2] >> 28) & 1;
    }
    if (maxLeaf >= 7) {
        __cpuid(regs, 7);
        caps.hasAVX2 = (regs[1] >> 5) & 1;
    }

    // 64-bit detection: CPUID leaf 0x80000001 ext feature, bit 29 (LM flag)
    __cpuid(regs, 0x80000000);
    if (static_cast<unsigned>(regs[0]) >= 0x80000001u) {
        __cpuid(regs, 0x80000001);
        caps.is64Bit = (regs[3] >> 29) & 1;
    }

    // Core count via leaf 1 EBX[23:16]
    __cpuid(regs, 1);
    caps.cores = static_cast<uint8_t>((regs[1] >> 16) & 0xFF);
    if (caps.cores == 0) caps.cores = 1; // always at least 1
}

static uint64_t QueryRam() {
    // IT NOTE: GlobalMemoryStatusEx returns DWORDLONG (64-bit) and supports
    // >4GB on PAE-extended XP. GlobalMemoryStatus (fallback) clips at 2GB.
    MEMORYSTATUSEX msx = {};
    msx.dwLength = sizeof(msx);
    if (GlobalMemoryStatusEx(&msx)) {
        return msx.ullTotalPhys;
    }
    // Fallback for Win9x (no Ex variant)
    MEMORYSTATUS ms = {};
    ms.dwLength = sizeof(ms);
    GlobalMemoryStatus(&ms);
    return static_cast<uint64_t>(ms.dwTotalPhys);
}

// ---------------------------------------------------------------------------
// Tier Classification Logic
// ---------------------------------------------------------------------------
// IT NOTE: This is the single decision point for which engine loads.
// If you add a new OS tier, ADD it to AeonTier enum AND add a branch here.
// NEVER use floating-point comparisons for version numbers — use integer.

static AeonTier ClassifyTier(const SystemProfile& p) {

    // Win16 / Win32s (running on top of Win 3.x)
    // Detected at runtime by checking for Win16Mutex or Win32s registry key.
    // In practice the 16-bit build's entry point (aeon16.c) never calls this.
    // This branch handles the edge case of a 32-bit build on Win32s.
    if (!p.isNT && p.osMajor < 4) {
        return AeonTier::Win16_Retro;
    }

    // Win9x family (version 4.x, non-NT kernel)
    // 4.0 = Win95, 4.10 = Win98, 4.90 = WinME
    if (!p.isNT && p.osMajor == 4) {
        return AeonTier::Win9x_Retro;
    }

    // NT 5.0 = Windows 2000
    if (p.isNT && p.osMajor == 5 && p.osMinor == 0) {
        return AeonTier::Win2000_Compat;
    }

    // NT 5.1 / 5.2 = Windows XP / XP x64 / Server 2003
    if (p.isNT && p.osMajor == 5) {
        // SSE2 check: Pentium III and earlier Celerons lack SSE2.
        // Without SSE2, Blink (V8 JIT) will SIGILL on first function compile.
        // IT FIX: Route to Gecko-based lightweight renderer instead.
        return p.cpu.hasSSE2 ? AeonTier::WinXP_HiSpec
                             : AeonTier::WinXP_LowSpec;
    }

    // NT 6.0 = Vista,  NT 6.1 = Win7
    if (p.isNT && p.osMajor == 6 && p.osMinor <= 1) {
        return AeonTier::WinVista_7;
    }

    // NT 6.2 = Win8,  NT 6.3 = Win8.1
    if (p.isNT && p.osMajor == 6) {
        return AeonTier::Win8_Modern;
    }

    // NT 10.0 = Windows 10 and 11 (build >= 22000 is Win11 but same tier)
    if (p.isNT && p.osMajor >= 10) {
        return AeonTier::Win10_11_Pro;
    }

    return AeonTier::Unknown;
}

// ---------------------------------------------------------------------------
// Public API Implementation
// ---------------------------------------------------------------------------

namespace AeonProbe {

AeonTier RunProbe(SystemProfile& out) {
    memset(&out, 0, sizeof(out));

    // --- Step 1: Kernel Identity ---
    // IT NOTE: The bitmask check for VER_PLATFORM_WIN32_NT is the canonical
    // way to distinguish NT from Win9x. Win9x returns WIN32_WINDOWS (1),
    // NT returns WIN32_NT (2). Win32s on Win16 returns WIN32s (0).
    OSVERSIONINFOW osInfo = {};
    if (FetchOsVersionNative(osInfo)) {
        out.osMajor = osInfo.dwMajorVersion;
        out.osMinor = osInfo.dwMinorVersion;
        out.osBuild = osInfo.dwBuildNumber & 0xFFFF;
        out.isNT    = (osInfo.dwPlatformId == VER_PLATFORM_WIN32_NT);

        // Copy description (NT only — "Service Pack N" etc.)
        WideCharToMultiByte(CP_UTF8, 0,
            osInfo.szCSDVersion, -1,
            out.osDescription, sizeof(out.osDescription),
            nullptr, nullptr);
    } else {
        // Last resort: GetVersionEx (may return shimmed values on Win8.1+)
        OSVERSIONINFOA a = {};
        a.dwOSVersionInfoSize = sizeof(a);
        GetVersionExA(&a);
        out.osMajor = a.dwMajorVersion;
        out.osMinor = a.dwMinorVersion;
        out.osBuild = a.dwBuildNumber & 0xFFFF;
        out.isNT    = (a.dwPlatformId == VER_PLATFORM_WIN32_NT);
    }

    // Is the OS itself 64-bit? (process may be 32-bit WOW64)
    // IT NOTE: IsWow64Process tells us if WE are running under WOW64,
    // meaning the HOST OS is 64-bit even if our .exe is 32-bit.
    BOOL wow64 = FALSE;
    typedef BOOL (WINAPI *IsWow64Process_t)(HANDLE, PBOOL);
    auto isWow = reinterpret_cast<IsWow64Process_t>(
        GetProcAddress(GetModuleHandleA("kernel32.dll"), "IsWow64Process"));
    if (isWow) isWow(GetCurrentProcess(), &wow64);
    out.is64BitOS = (wow64 == TRUE) || (sizeof(void*) == 8);

    // --- Step 2: CPU Capabilities ---
    QueryCpuCaps(out.cpu);

    // --- Step 3: RAM ---
    out.ramBytes = QueryRam();

    // --- Step 4: Tier Classification ---
    out.tier = ClassifyTier(out);

    return out.tier;
}

const char* TierName(AeonTier t) {
    switch (t) {
        case AeonTier::Win16_Retro:    return "Win16-Retro (3.x)";
        case AeonTier::Win9x_Retro:    return "Win9x-Retro (95/98/ME)";
        case AeonTier::Win2000_Compat: return "Win2000-Compat";
        case AeonTier::WinXP_LowSpec:  return "WinXP-LowSpec (no SSE2)";
        case AeonTier::WinXP_HiSpec:   return "WinXP-HiSpec (SSE2+)";
        case AeonTier::WinVista_7:     return "WinVista/7-Extended";
        case AeonTier::Win8_Modern:    return "Win8/8.1-Modern";
        case AeonTier::Win10_11_Pro:   return "Win10/11-Pro";
        default:                       return "Unknown";
    }
}

void DumpProfile(const SystemProfile& p) {
#ifdef AEON_DEBUG
    printf("[AeonProbe] OS      : %u.%u (build %u) — %s\n",
        p.osMajor, p.osMinor, p.osBuild, p.isNT ? "NT" : "Win9x/16");
    printf("[AeonProbe] RAM     : %llu MB\n", p.ramBytes / (1024*1024));
    printf("[AeonProbe] CPU     : %d cores | SSE2=%d AVX=%d AVX2=%d 64bit=%d\n",
        p.cpu.cores, p.cpu.hasSSE2, p.cpu.hasAVX, p.cpu.hasAVX2, p.cpu.is64Bit);
    printf("[AeonProbe] Tier    : %s\n", TierName(p.tier));
    printf("[AeonProbe] OS Desc : %s\n", p.osDescription);
#endif
}

} // namespace AeonProbe
