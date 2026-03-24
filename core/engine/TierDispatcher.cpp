// AeonBrowser — TierDispatcher.cpp
// DelgadoLogic | Lead Systems Architect
//
// PURPOSE: Loads the correct rendering engine DLL based on AeonTier.
// Each engine is a separate DLL (aeon_blink.dll, aeon_gecko.dll,
// aeon_html4.dll) with a standardised AeonEngine C interface.
//
// IT TROUBLESHOOTING:
//   - "Engine DLL not found" → check installation dir for right DLL set.
//   - "Entry point missing" → DLL was built with wrong API version.
//   - XP Blink crash immediately → SSE2 missing, wrong tier detected.
//     Force WinXP_LowSpec by adding reg: HKLM\SOFTWARE\DelgadoLogic\Aeon\ForceTier=3

#include "TierDispatcher.h"
#include "../probe/HardwareProbe.h"
#include <windows.h>
#include <cstdio>

// Standard engine DLL entry point signature (all tiers implement this)
typedef bool (__cdecl *AeonEngineInit_t)(const SystemProfile*, HINSTANCE);
typedef void (__cdecl *AeonEngineShutdown_t)();

struct TierDispatcher::Impl {
    HMODULE    engineDll    = nullptr;
    AeonEngineInit_t     fnInit     = nullptr;
    AeonEngineShutdown_t fnShutdown = nullptr;
};

static const char* SelectEngineDll(AeonTier tier) {
    // IT NOTE: DLL names are deliberate — aeon_html4 for anything pre-XP,
    // aeon_gecko for the Gecko-based lightweight build (no SSE2 XP + Vista/7),
    // aeon_blink for the Chromium-core modern builds.
    switch (tier) {
        case AeonTier::Win16_Retro:
        case AeonTier::Win9x_Retro:
        case AeonTier::Win2000_Compat:
        case AeonTier::WinXP_LowSpec:  return "aeon_html4.dll";   // Our HTML4/CSS2 renderer
        case AeonTier::WinXP_HiSpec:
        case AeonTier::WinVista_7:     return "aeon_gecko.dll";   // Gecko lightweight
        case AeonTier::Win8_Modern:
        case AeonTier::Win10_11_Pro:   return "aeon_blink.dll";   // Blink/Chromium core
        default:                       return nullptr;
    }
}

TierDispatcher::TierDispatcher(const SystemProfile& p, HINSTANCE hInst)
    : m_Profile(p), m_hInst(hInst), m_impl(new Impl) {}

TierDispatcher::~TierDispatcher() {
    if (m_impl->engineDll) {
        if (m_impl->fnShutdown) m_impl->fnShutdown();
        FreeLibrary(m_impl->engineDll);
    }
    delete m_impl;
}

bool TierDispatcher::LoadEngine() {
    // Check for forced tier override (for IT troubleshooting)
    // HKLM\SOFTWARE\DelgadoLogic\Aeon\ForceTier (REG_DWORD)
    HKEY hk;
    if (RegOpenKeyExA(HKEY_LOCAL_MACHINE,
            "SOFTWARE\\DelgadoLogic\\Aeon", 0, KEY_READ, &hk) == ERROR_SUCCESS) {
        DWORD forced = 0xFF, sz = sizeof(forced);
        RegQueryValueExA(hk, "ForceTier", nullptr, nullptr,
            reinterpret_cast<BYTE*>(&forced), &sz);
        RegCloseKey(hk);
        if (forced != 0xFF) {
            m_effectiveTier = static_cast<AeonTier>(forced);
#ifdef AEON_DEBUG
            printf("[TierDispatcher] Tier overridden to %u\n", forced);
#endif
        }
    } else {
        m_effectiveTier = m_Profile.tier;
    }

    const char* dllName = SelectEngineDll(m_effectiveTier);
    if (!dllName) {
        fprintf(stderr, "[TierDispatcher] No engine defined for tier %u\n",
            static_cast<unsigned>(m_effectiveTier));
        return false;
    }

    m_impl->engineDll = LoadLibraryA(dllName);
    if (!m_impl->engineDll) {
        fprintf(stderr, "[TierDispatcher] LoadLibrary(%s) failed: %lu\n",
            dllName, GetLastError());
        return false;
    }

    m_impl->fnInit = reinterpret_cast<AeonEngineInit_t>(
        GetProcAddress(m_impl->engineDll, "AeonEngine_Init"));
    m_impl->fnShutdown = reinterpret_cast<AeonEngineShutdown_t>(
        GetProcAddress(m_impl->engineDll, "AeonEngine_Shutdown"));

    if (!m_impl->fnInit) {
        fprintf(stderr, "[TierDispatcher] %s missing AeonEngine_Init\n", dllName);
        return false;
    }

    return m_impl->fnInit(&m_Profile, m_hInst);
}
