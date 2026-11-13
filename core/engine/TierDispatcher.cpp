// AeonBrowser — TierDispatcher.cpp
// DelgadoLogic | Engine Team

#include "TierDispatcher.h"
#include "AeonEngine_Interface.h"
#include "../probe/HardwareProbe.h"
#include <windows.h>
#include <cstdio>
#include <string>
#include <cctype>

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

// Singleton state
static TierDispatcher* s_dispatcherInstance = nullptr;

TierDispatcher& TierDispatcher::GetInstance() {
    static SystemProfile defaultProfile = {};
    static TierDispatcher defaultInstance(defaultProfile, nullptr);
    if (s_dispatcherInstance) return *s_dispatcherInstance;
    return defaultInstance;
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

        // ── ABI version check (REQUIRED before touching vtable) ──────────
        auto abiFn = reinterpret_cast<AeonEngine_AbiVersion_t>(
            GetProcAddress(hMod, "AeonEngine_AbiVersion"));
        if (!abiFn) {
            fprintf(stderr, "[Tier] %s: no AbiVersion export — rejecting\n", candidates[i].dllName);
            FreeLibrary(hMod); continue;
        }
        int dllAbi = abiFn();
        if (dllAbi != AEON_ENGINE_ABI_VERSION) {
            fprintf(stderr, "[Tier] %s: ABI mismatch (DLL=%d, core=%d) — rejecting\n",
                    candidates[i].dllName, dllAbi, AEON_ENGINE_ABI_VERSION);
            FreeLibrary(hMod); continue;
        }

        auto createFn = reinterpret_cast<AeonEngineVTable*(*)()>(
            GetProcAddress(hMod, "AeonEngine_Create"));
        if (!createFn) { FreeLibrary(hMod); continue; }

        AeonEngineVTable* e = createFn();
        if (!e) { FreeLibrary(hMod); continue; }

        fprintf(stdout, "[Tier] Engine: %s (%s) [ABI v%d]\n",
                candidates[i].dllName, candidates[i].desc, dllAbi);
        TierDispatcher::GetInstance().SetDefaultEngine(e);
        return e;
    }
    fprintf(stderr, "[Tier] FATAL: no engine for tier %d\n", (int)profile->tier);
    return nullptr;
}

// TierDispatcher class (declared in TierDispatcher.h)
TierDispatcher::TierDispatcher(const SystemProfile& p, HINSTANCE hInst)
    : m_Profile(p), m_hInst(hInst) {
    s_dispatcherInstance = this;
}

TierDispatcher::~TierDispatcher() {
    if (s_dispatcherInstance == this) s_dispatcherInstance = nullptr;
    if (m_stealthEngine && m_stealthEngine->Shutdown)
        m_stealthEngine->Shutdown();
    if (m_engine && m_engine->Shutdown)
        m_engine->Shutdown();
}

bool TierDispatcher::IsProtectedDomain(const char* url) {
    if (!url || !*url) return false;

    // 1. Extract authority (strip scheme, stop before path '/', query '?', or fragment '#')
    std::string str(url);
    size_t schemePos = str.find("://");
    size_t start = (schemePos != std::string::npos) ? schemePos + 3 : 0;
    size_t authEnd = str.find_first_of("/?#", start);
    std::string auth = (authEnd != std::string::npos) ? str.substr(start, authEnd - start) : str.substr(start);

    // 2. Strip RFC 3986 userinfo (e.g. user:pass@) if present
    size_t atPos = auth.rfind('@');
    if (atPos != std::string::npos) {
        auth = auth.substr(atPos + 1);
    }

    // 3. Strip port if present
    size_t portPos = auth.find(':');
    std::string host = (portPos != std::string::npos) ? auth.substr(0, portPos) : auth;

    // 4. Convert hostname to lowercase
    for (char& c : host) {
        c = (char)tolower(static_cast<unsigned char>(c));
    }

    if (host.empty()) return false;

    // 3. Check exact domain match or suffix match with leading dot
    static const char* protectedDomains[] = {
        "accounts.google.com",
        "challenges.cloudflare.com",
        "nowsecure.nl",
        "bot.sannysoft.com",
        "datadome.com",
        "datadome.co",
        nullptr
    };

    for (int i = 0; protectedDomains[i] != nullptr; ++i) {
        std::string d(protectedDomains[i]);
        if (host == d) {
            return true;
        }
        std::string suffix = "." + d;
        if (host.size() > suffix.size() &&
            host.compare(host.size() - suffix.size(), suffix.size(), suffix) == 0) {
            return true;
        }
    }
    return false;
}

AeonEngineVTable* TierDispatcher::LoadEngineByName(const char* dllName) {
    if (!dllName) return nullptr;

    char exeDir[MAX_PATH];
    GetModuleFileNameA(nullptr, exeDir, MAX_PATH);
    if (char* s = strrchr(exeDir, '\\')) *s = '\0';

    char path[MAX_PATH];
    _snprintf_s(path, sizeof(path), _TRUNCATE, "%s\\%s", exeDir, dllName);

    HMODULE hMod = LoadLibraryA(path);
    if (!hMod) {
        fprintf(stderr, "[Tier] Failed to load engine DLL: %s (path: %s)\n", dllName, path);
        return nullptr;
    }

    auto abiFn = reinterpret_cast<AeonEngine_AbiVersion_t>(GetProcAddress(hMod, "AeonEngine_AbiVersion"));
    if (!abiFn) {
        fprintf(stderr, "[Tier] %s: no AbiVersion export\n", dllName);
        FreeLibrary(hMod);
        return nullptr;
    }
    int dllAbi = abiFn();
    if (dllAbi != AEON_ENGINE_ABI_VERSION) {
        fprintf(stderr, "[Tier] %s: ABI mismatch (DLL=%d, core=%d)\n", dllName, dllAbi, AEON_ENGINE_ABI_VERSION);
        FreeLibrary(hMod);
        return nullptr;
    }

    auto createFn = reinterpret_cast<AeonEngineVTable*(*)()>(GetProcAddress(hMod, "AeonEngine_Create"));
    if (!createFn) {
        fprintf(stderr, "[Tier] %s: no AeonEngine_Create export\n", dllName);
        FreeLibrary(hMod);
        return nullptr;
    }

    AeonEngineVTable* e = createFn();
    if (e && e->Init) {
        if (!e->Init(&m_Profile, m_hInst)) {
            fprintf(stderr, "[Tier] Engine %s Init() failed\n", dllName);
            if (e->Shutdown) e->Shutdown();
            return nullptr;
        }
    }

    fprintf(stdout, "[Tier] Dynamically loaded engine DLL: %s [ABI v%d]\n", dllName, dllAbi);
    return e;
}

AeonEngineVTable* TierDispatcher::GetEngineForUrl(const char* url) {
    if (IsProtectedDomain(url)) {
        if (!m_stealthEngine) {
            fprintf(stdout, "[Tier] Protected domain detected for '%s' — initializing Camoufox C++ Stealth Engine (aeon_gecko_stealth.dll)\n", url ? url : "(null)");
            m_stealthEngine = LoadEngineByName("aeon_gecko_stealth.dll");
        }
        if (m_stealthEngine) {
            return m_stealthEngine;
        }
        fprintf(stderr, "[Tier] WARNING: Failed to load aeon_gecko_stealth.dll, falling back to default engine\n");
    }
    return m_engine;
}

bool TierDispatcher::LoadEngine() {
    auto* e = TierDispatcher_LoadEngine(&m_Profile);
    m_effectiveTier = m_Profile.tier;
    if (!e) return false;

    // Initialize the engine DLL — must happen before any other vtable calls
    if (e->Init) {
        int result = e->Init(&m_Profile, m_hInst);
        if (!result) {
            fprintf(stderr, "[Tier] Engine Init() returned failure\n");
            if (e->Shutdown) e->Shutdown();
            return false;
        }
        fprintf(stdout, "[Tier] Engine Init() succeeded.\n");
    }

    m_engine = e;
    return true;
}

