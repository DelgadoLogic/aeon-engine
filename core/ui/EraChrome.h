// AeonBrowser — EraChrome.h
#pragma once
#include "../probe/HardwareProbe.h"
#include <windows.h>

class EraChrome {
public:
    EraChrome(const SystemProfile& p, HINSTANCE hInst, int nCmdShow)
        : m_Profile(p), m_hInst(hInst), m_nCmdShow(nCmdShow) {}

    bool IsModern()       const;
    bool IsDwmAvailable() const;
    void CreateBrowserWindow();
    int  Run(const char* cmdLine);

    HWND GetHwnd() const { return m_hwnd; }

private:
    const SystemProfile& m_Profile;
    HINSTANCE            m_hInst;
    int                  m_nCmdShow;
    HWND                 m_hwnd = nullptr;
};
