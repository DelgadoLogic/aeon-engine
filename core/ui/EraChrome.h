// AeonBrowser — EraChrome.h
// DelgadoLogic | Lead UI Engineer
//
// PURPOSE: Adaptive UI Engine — selects Modern (Mica/DWM) or Retro (Win32)
// skin based on OS tier, creates the browser window, and runs the message loop.
#pragma once
#include "../probe/HardwareProbe.h"
#include "../engine/AeonEngine_Interface.h"
#include <windows.h>

class EraChrome {
public:
    EraChrome(const SystemProfile& p, HINSTANCE hInst, int nCmdShow)
        : m_Profile(p), m_hInst(hInst), m_nCmdShow(nCmdShow) {}

    bool IsModern()       const;
    bool IsDwmAvailable() const;
    void CreateBrowserWindow();
    int  Run(const char* cmdLine);

    // Set the engine vtable — must be called before CreateBrowserWindow().
    void SetEngine(AeonEngineVTable* engine) { m_engine = engine; }

    HWND GetHwnd() const { return m_hwnd; }

private:
    const SystemProfile& m_Profile;
    HINSTANCE            m_hInst;
    int                  m_nCmdShow;
    HWND                 m_hwnd = nullptr;
    AeonEngineVTable*    m_engine = nullptr;
};
