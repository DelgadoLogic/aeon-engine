// AeonBrowser — TierDispatcher.cpp
// DelgadoLogic | Engine Team
//
// Loads the correct rendering engine DLL based on SystemProfile tier.
// Returns a populated AeonEngineVTable* ready for BrowserChrome::Create().
//
// ENGINE SEARCH ORDER (per tier):
//   Pro/Modern    → aeon_blink.dll   (Chromium Blink-compatible renderer)
//   Extended      → aeon_gecko.dll   (Gecko lightweight fork)
//   XPHi          → aeon_blink_xp.dll → fallback: aeon_gecko.dll
//   XPLo          → aeon_gecko_nsse.dll
//   Retro2000/9x  → aeon_html4.dll   (our custom HTML4/CSS2 GDI renderer)
//   Win16         → Not loaded here — aeon16.c handles everything natively
//
// If the preferred DLL is not found, TierDispatcher falls back down the chain
// until it hits aeon_html4.dll which is always bundled.

#include "TierDispatcher.h"
#include "AeonEngine_Interface.h"
#include "../probe/HardwareProbe.h"
#include <windows.h>
#include <cstdio>

namespace TierDispatcher {

struct EngineCandidate {
    const char* dllName;
    const char* description;
};

static const EngineCandidate k_proTier[] = {
    { "aeon_blink.dll",       "Blink shim (Chromium-compatible)" },
    { "aeon_blink_stub.dll",  "Blink stub (development)" },
    { "aeon_gecko.dll",       "Gecko lightweight fork" },
    { "aeon_html4.dll",       "HTML4 fallback" },
    { nullptr, nullptr }
};
static const EngineCandidate k_extendedTier[] = {
    { "aeon_gecko.dll",       "Gecko lightweight (Vista/7)" },
    { "aeon_html4.dll",       "HTML4 fallback" },
    { nullptr, nullptr }
};
static const EngineCandidate k_xpHiTier[] = {
    { "aeon_blink_xp.dll",   "Blink XP edition (SSE2)" },
    { "aeon_gecko.dll",      "Gecko" },
    { "aeon_html4.dll",      "HTML4 fallback" },
    { nullptr, nullptr }
};
static const EngineCandidate k_xpLoTier[] = {
    { "aeon_gecko_nsse.dll", "Gecko no-SSE2 edition" },
    { "aeon_html4.dll",      "HTML4 fallback" },
    { nullptr, nullptr }
};
static const EngineCandidate k_retroTier[] = {
    { "aeon_html4.dll",      "HTML4/CSS2 GDI renderer" },
    { nullptr, nullptr }
};

static const EngineCandidate* CandidatesFor(OSTier tier) {
    switch (tier) {
        case OSTier::Pro:
        case OSTier::Modern:   return k_proTier;
        case OSTier::Extended: return k_extendedTier;
        case OSTier::XPHi:    return k_xpHiTier;
        case OSTier::XPLo:    return k_xpLoTier;
        default:               return k_retroTier;
    }
}

AeonEngineVTable* LoadEngine(const SystemProfile* profile) {
    if (!profile) return nullptr;
    if (profile->tier == OSTier::Win16) return nullptr; // aeon16.c handles it

    char exeDir[MAX_PATH];
    GetModuleFileNameA(nullptr, exeDir, MAX_PATH);
    if (char* s = strrchr(exeDir, '\\')) *s = '\0';

    const EngineCandidate* candidates = CandidatesFor(profile->tier);

    for (int i = 0; candidates[i].dllName; i++) {
        char path[MAX_PATH];
        _snprintf_s(path, sizeof(path), _TRUNCATE, "%s\\%s", exeDir, candidates[i].dllName);

        HMODULE hMod = LoadLibraryA(path);
        if (!hMod) { fprintf(stdout,"[TierDispatcher] Not found: %s\n",candidates[i].dllName); continue; }

        auto createFn = reinterpret_cast<AeonEngine_CreateFn>(GetProcAddress(hMod,"AeonEngine_Create"));
        if (!createFn) { FreeLibrary(hMod); continue; }

        AeonEngineVTable* e = createFn();
        if (!e || e->api_version < AEON_ENGINE_API_VERSION) {
            if (e && e->Shutdown) e->Shutdown();
            FreeLibrary(hMod); continue;
        }

        fprintf(stdout,"[TierDispatcher] Engine: %s (%s) API v%u\n",
            candidates[i].dllName, candidates[i].description, e->api_version);
        return e;
    }

    fprintf(stderr,"[TierDispatcher] FATAL: no engine found for tier %d\n",(int)profile->tier);
    return nullptr;
}

} // namespace TierDispatcher
