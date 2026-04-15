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
    TierDispatcher(const SystemProfile& p, HINSTANCE hInst);
    ~TierDispatcher();
    bool LoadEngine();
    AeonTier GetEffectiveTier() const { return m_effectiveTier; }

    // Returns the loaded engine vtable (nullptr if LoadEngine() failed).
    AeonEngineVTable* GetEngine() const { return m_engine; }

private:
    const SystemProfile& m_Profile;
    HINSTANCE            m_hInst;
    AeonTier             m_effectiveTier = AeonTier::Unknown;
    AeonEngineVTable*    m_engine = nullptr;
    struct Impl;
    Impl* m_impl;
};
