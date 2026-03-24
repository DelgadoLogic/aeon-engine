// AeonBrowser — BrowserChrome.h
#pragma once
#include <windows.h>
#include "../../core/engine/AeonEngine_Interface.h"
#include "../../core/probe/HardwareProbe.h"

namespace BrowserChrome {
    // Call once after window creation to set up the chrome.
    void Create(HWND parent, const SystemProfile* profile, AeonEngineVTable* engine);

    // Forward WM_PAINT to draw the chrome.
    void OnPaint(HWND hwnd);

    // Forward WM_SIZE to reposition content area.
    void OnSize(HWND hwnd, int w, int h);
}
