// AeonBrowser — TierDispatcher.h
#pragma once
#include "../probe/HardwareProbe.h"
#include <windows.h>

class TierDispatcher {
public:
    TierDispatcher(const SystemProfile& p, HINSTANCE hInst);
    ~TierDispatcher();
    bool LoadEngine();
    AeonTier GetEffectiveTier() const { return m_effectiveTier; }

private:
    const SystemProfile& m_Profile;
    HINSTANCE            m_hInst;
    AeonTier             m_effectiveTier = AeonTier::Unknown;
    struct Impl;
    Impl* m_impl;
};
