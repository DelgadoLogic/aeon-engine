// AeonBrowser — HardwareProbe.h
// DelgadoLogic | Lead Systems Architect
//
// PURPOSE: Declares the OS/CPU interrogation interface used at startup.
// Every decision downstream (rendering engine, TLS stack, UI skin) flows
// from the AeonTier this module produces. Change NOTHING here without
// updating TierDispatcher.cpp accordingly.

#pragma once

#include <cstdint>

// ---------------------------------------------------------------------------
// Tier Enumeration
// ---------------------------------------------------------------------------
// These values must stay stable — they are serialised to registry/config.
enum class AeonTier : uint8_t {
    Win16_Retro   = 0,  // Windows 3.x / 16-bit (Open Watcom HTML4 renderer)
    Win9x_Retro   = 1,  // Windows 95 / 98 / ME  (KernelEx + BearSSL)
    Win2000_Compat= 2,  // Windows 2000 / BlackWingCat kernel
    WinXP_LowSpec = 3,  // Windows XP, no SSE2   (Gecko lightweight)
    WinXP_HiSpec  = 4,  // Windows XP + SSE2     (Blink-compatible)
    WinVista_7    = 5,  // Vista / 7              (Gecko, TLS registry fix)
    Win8_Modern   = 6,  // Windows 8 / 8.1        (Blink + Rust router)
    Win10_11_Pro  = 7,  // Windows 10/11           (Full Pro stack, Mica UI)
    Unknown       = 0xFF
};

// CPU capabilities bitmask
struct CpuCaps {
    bool is64Bit    : 1;
    bool hasSSE2    : 1;
    bool hasSSE4    : 1;
    bool hasAVX     : 1;
    bool hasAVX2    : 1;
    uint8_t cores;
};

// Full system snapshot produced by probe
struct SystemProfile {
    AeonTier   tier;
    CpuCaps    cpu;
    uint64_t   ramBytes;        // Physical RAM
    uint32_t   osMajor;
    uint32_t   osMinor;
    uint32_t   osBuild;
    bool       isNT;            // NT kernel (vs Win9x/Win16)
    bool       is64BitOS;
    char       osDescription[64];
};

// ---------------------------------------------------------------------------
// Main API
// ---------------------------------------------------------------------------
namespace AeonProbe {

    // Call once at startup. Fills 'out' and returns the tier.
    AeonTier RunProbe(SystemProfile& out);

    // Human-readable tier name for logging / crash reports
    const char* TierName(AeonTier t);

    // Print profile to stdout (debug builds only)
    void DumpProfile(const SystemProfile& p);

} // namespace AeonProbe
