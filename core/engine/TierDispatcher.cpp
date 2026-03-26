// AeonBrowser — TierDispatcher.cpp
// DelgadoLogic | Engine Team

#include "TierDispatcher.h"
#include "AeonEngine_Interface.h"
#include "../probe/HardwareProbe.h"
#include <windows.h>
#include <cstdio>

struct EngineCandidate { const char* dllName; const char* desc; };

static const EngineCandidate k_proTier[] = {
    { "aeon_blink.dll",       "Blink (Chromium-compatible)" },
    { "aeon_blink_stub.dll",  "Blink stub (WebView2 host)" },
    { "aeon_gecko.dll",       "Gecko lightweight" },
    { "aeon_html4.dll",       "HTML4 fallback" },
    { nullptr, nullptr }
};
static const EngineCandidate k_extendedTier[] = {
    { "aeon_gecko.dll", "Gecko (Vista/7)" },
    { "aeon_html4.dll", "HTML4 fallback"  },
    { nullptr, nullptr }
};
static const EngineCandidate k_xpHiTier[] = {
    { "aeon_blink_xp.dll", "Blink XP (SSE2)" },
    { "aeon_gecko.dll",    "Gecko" },
    { "aeon_html4.dll",    "HTML4 fallback" },
    { nullptr, nullptr }
};
static const EngineCandidate k_xpLoTier[] = {
    { "aeon_gecko_nsse.dll", "Gecko no-SSE2" },
    { "aeon_html4.dll",      "HTML4 fallback" },
    { nullptr, nullptr }
};
static const EngineCandidate k_retroTier[] = {
    { "aeon_html4.dll", "HTML4/CSS2 GDI" },
    { nullptr, nullptr }
};

static const EngineCandidate* CandidatesFor(AeonTier tier) {
    switch (tier) {
        case AeonTier::Win10_11_Pro:
        case AeonTier::Win8_Modern:   return k_proTier;
        case AeonTier::WinVista_7:    return k_extendedTier;
        case AeonTier::WinXP_HiSpec:  return k_xpHiTier;
        case AeonTier::WinXP_LowSpec: return k_xpLoTier;
        default:                       return k_retroTier;
    }
}

// Internal helper used by AeonMain
AeonEngineVTable* TierDispatcher_LoadEngine(const SystemProfile* profile) {
    if (!profile) return nullptr;
    if (profile->tier == AeonTier::Win16_Retro) return nullptr;

    char exeDir[MAX_PATH];
    GetModuleFileNameA(nullptr, exeDir, MAX_PATH);
    if (char* s = strrchr(exeDir, '\\')) *s = '\0';

    const EngineCandidate* candidates = CandidatesFor(profile->tier);
    for (int i = 0; candidates[i].dllName; i++) {
        char path[MAX_PATH];
        _snprintf_s(path, sizeof(path), _TRUNCATE, "%s\\%s", exeDir, candidates[i].dllName);

        HMODULE hMod = LoadLibraryA(path);
        if (!hMod) { fprintf(stdout, "[Tier] Missing: %s\n", candidates[i].dllName); continue; }

        auto createFn = reinterpret_cast<AeonEngineVTable*(*)()>(
            GetProcAddress(hMod, "AeonEngine_Create"));
        if (!createFn) { FreeLibrary(hMod); continue; }

        AeonEngineVTable* e = createFn();
        if (!e) { FreeLibrary(hMod); continue; }

        fprintf(stdout, "[Tier] Engine: %s (%s)\n", candidates[i].dllName, candidates[i].desc);
        return e;
    }
    fprintf(stderr, "[Tier] FATAL: no engine for tier %d\n", (int)profile->tier);
    return nullptr;
}

// TierDispatcher class (declared in TierDispatcher.h)
TierDispatcher::TierDispatcher(const SystemProfile& p, HINSTANCE hInst)
    : m_Profile(p), m_hInst(hInst), m_impl(nullptr) {}
TierDispatcher::~TierDispatcher() {}
bool TierDispatcher::LoadEngine() {
    auto* e = TierDispatcher_LoadEngine(&m_Profile);
    m_effectiveTier = m_Profile.tier;
    return (e != nullptr);
}
