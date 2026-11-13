// AeonBrowser — TierDispatcher.h
// DelgadoLogic | Engine Team
//
// PURPOSE: Selects and loads the correct rendering engine DLL for the
// detected OS tier. Exposes the engine vtable for downstream consumers.
#pragma once
#include "../probe/HardwareProbe.h"
#include "AeonEngine_Interface.h"
#include <windows.h>

class TierDispatcher {
public:
    static TierDispatcher& GetInstance();
    TierDispatcher(const SystemProfile& p, HINSTANCE hInst);
    ~TierDispatcher();
    bool LoadEngine();
    AeonTier GetEffectiveTier() const { return m_effectiveTier; }

    void SetDefaultEngine(AeonEngineVTable* engine) { m_engine = engine; }

    // Returns the default loaded engine vtable (nullptr if LoadEngine() failed).
    AeonEngineVTable* GetEngine() const { return m_engine; }

    // Dynamic Domain Routing: Inspects URL and returns appropriate engine
    // Returns aeon_gecko_stealth.dll vtable for anti-bot protected domains,
    // and standard engine (aeon_blink.dll) for normal navigation.
    AeonEngineVTable* GetEngineForUrl(const char* url);

    // Checks if the given URL belongs to a known anti-bot protected domain
    static bool IsProtectedDomain(const char* url);

private:
    AeonEngineVTable* LoadEngineByName(const char* dllName);

    const SystemProfile& m_Profile;
    HINSTANCE            m_hInst;
    AeonTier             m_effectiveTier = AeonTier::Unknown;
    AeonEngineVTable*    m_engine = nullptr;
    AeonEngineVTable*    m_stealthEngine = nullptr;
};

