// AeonBrowser — EraChrome.cpp
// DelgadoLogic | Lead Systems Architect
//
// PURPOSE: Adaptive UI Engine — "Era-Chrome"
// Renders the browser chrome (not the rendering engine) using the best UI
// toolkit available for the detected OS tier.
//
// Modern Tier  → Mica/Acrylic DWM composition, vertical FlowBar sidebar,
//                rounded corners, LogicFlow integration toolbar button.
// Retro Tier   → Pure Win32 classic grey, beveled buttons, MS Sans Serif,
//                no DWM, no transparency.
//
// IT TROUBLESHOOTING:
//   - Black toolbar on Vista: DWM may be disabled (Aero off). EraChrome
//     detects this via DwmIsCompositionEnabled() and falls back to classic.
//   - Sidebar missing on XP: vertical tab bar requires Gecko engine which
//     has a native sidebar implementation. HTML4 tier has horizontal tabs only.
//   - LogicFlow button not responding: LogicFlowAgent.exe not running.
//     Button is greyed out when IPC pipe "\\.\\pipe\\LFAgent" is unavailable.

#include "EraChrome.h"
#include "BrowserChrome.h"
#include "../probe/HardwareProbe.h"
#include "../engine/AeonBridge.h"
#include "../agent/AeonAgentPipe.h"

#include <windows.h>
#include <dwmapi.h>    // DwmIsCompositionEnabled, DwmExtendFrameIntoClientArea
#include <cstdio>

#pragma comment(lib, "dwmapi.lib")

// ---------------------------------------------------------------------------
// Window Class Names
// ---------------------------------------------------------------------------
static constexpr WCHAR CLASS_MODERN[] = L"AeonBrowser_Modern";
static constexpr WCHAR CLASS_RETRO[]  = L"AeonBrowser_Retro";

// ---------------------------------------------------------------------------
// Modern Skin Helpers (Win8+)
// ---------------------------------------------------------------------------
static void ApplyMicaBackdrop(HWND hwnd) {
    // IT NOTE: DWMWA_SYSTEMBACKDROP_TYPE = 38, value 2 = Mica.
    // Available only on Win11 build 22000+. Silently fails on earlier builds.
    // We try it — if it fails, the plain black DWM frame remains (fine).
    const DWORD DWMWA_SYSTEMBACKDROP_TYPE = 38;
    const DWORD DWMSBT_MAINWINDOW = 2; // Mica
    DwmSetWindowAttribute(hwnd, DWMWA_SYSTEMBACKDROP_TYPE,
        &DWMSBT_MAINWINDOW, sizeof(DWMSBT_MAINWINDOW));

    // Extend frame to cover entire client area (needed for Mica to show)
    MARGINS m = {-1, -1, -1, -1};
    DwmExtendFrameIntoClientArea(hwnd, &m);
}

static void ApplyRoundedCorners(HWND hwnd) {
    // IT NOTE: DWMWA_WINDOW_CORNER_PREFERENCE = 33, value 2 = DWMWCP_ROUND.
    // Win11 only. On Win10/8 this is ignored — square corners remain.
    const DWORD DWMWA_WINDOW_CORNER_PREFERENCE = 33;
    const DWORD DWMWCP_ROUND = 2;
    DwmSetWindowAttribute(hwnd, DWMWA_WINDOW_CORNER_PREFERENCE,
        &DWMWCP_ROUND, sizeof(DWMWCP_ROUND));
}

// ---------------------------------------------------------------------------
// Retro Skin: Classic Win32 painting
// ---------------------------------------------------------------------------
static void PaintRetroChrome(HWND hwnd, HDC hdc, const RECT& rc) {
    // Fill background with classic Windows grey
    HBRUSH grey = CreateSolidBrush(RGB(192, 192, 192));
    FillRect(hdc, &rc, grey);
    DeleteObject(grey);

    // Draw beveled top toolbar (3D raised effect)
    // IT NOTE: DrawEdge is available on all Win32 platforms including Win9x.
    RECT toolbar = { rc.left, rc.top, rc.right, rc.top + 32 };
    DrawEdge(hdc, &toolbar, EDGE_RAISED, BF_RECT | BF_MIDDLE);

    // Address bar slot
    RECT addr = { rc.left + 80, rc.top + 6, rc.right - 80, rc.top + 26 };
    DrawEdge(hdc, &addr, EDGE_SUNKEN, BF_RECT);
    FillRect(hdc, &addr, (HBRUSH)GetStockObject(WHITE_BRUSH));

    // Status bar at bottom
    RECT status = { rc.left, rc.bottom - 20, rc.right, rc.bottom };
    DrawEdge(hdc, &status, EDGE_RAISED, BF_RECT | BF_MIDDLE);

    // Draw status text (MS Sans Serif — standard retro font)
    HFONT retro = CreateFontA(13, 0, 0, 0, FW_NORMAL, 0, 0, 0,
        ANSI_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
        DEFAULT_QUALITY, DEFAULT_PITCH | FF_SWISS, "MS Sans Serif");
    HFONT old = (HFONT)SelectObject(hdc, retro);
    SetBkMode(hdc, TRANSPARENT);
    TextOutA(hdc, rc.left + 4, rc.bottom - 17, "Ready", 5);
    SelectObject(hdc, old);
    DeleteObject(retro);
}

// ---------------------------------------------------------------------------
// Window Procedures — forward all relevant messages to BrowserChrome + IPC
// ---------------------------------------------------------------------------
static LRESULT CALLBACK ModernWndProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    switch (m) {
        case WM_DESTROY:
            AeonAgentPipe::Stop();
            PostQuitMessage(0);
            return 0;

        case WM_PAINT:
            BrowserChrome::OnPaint(h);
            // Also let DefWindowProc validate the paint region
            break;

        case WM_SIZE: {
            int width  = LOWORD(l);
            int height = HIWORD(l);
            BrowserChrome::OnSize(h, width, height);
            return 0;
        }

        case WM_LBUTTONDOWN: {
            int x = (short)LOWORD(l);
            int y = (short)HIWORD(l);

            // Hit-test: if click is on empty nav bar area, initiate window drag
            int hit = BrowserChrome::HitTest(h, x, y);
            if (hit == -1 && y < 40) {
                // Caption area drag — send to system
                ReleaseCapture();
                SendMessage(h, WM_NCLBUTTONDOWN, HTCAPTION, l);
                return 0;
            }
            BrowserChrome::OnLButtonDown(h, x, y);
            return 0;
        }

        case WM_LBUTTONUP: {
            int x = (short)LOWORD(l);
            int y = (short)HIWORD(l);
            BrowserChrome::OnLButtonUp(h, x, y);
            return 0;
        }

        case WM_MOUSEMOVE: {
            int x = (short)LOWORD(l);
            int y = (short)HIWORD(l);
            BrowserChrome::OnMouseMove(h, x, y);
            return 0;
        }

        case WM_COMMAND:
            BrowserChrome::OnCommand(h, LOWORD(w), HIWORD(w), (HWND)l);
            return 0;

        case WM_KEYDOWN:
        case WM_SYSKEYDOWN:
            if (BrowserChrome::OnKeyDown(h, w, l))
                return 0;
            break;

        case WM_AEON_AGENT:
            AeonAgentPipe::HandleCommand(w, l);
            return 0;

        case WM_AEONBRIDGE:
            AeonBridge::HandleWmAeonBridge(w, l);
            return 0;
    }
    return DefWindowProcW(h, m, w, l);
}

static LRESULT CALLBACK RetroWndProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    switch (m) {
        case WM_DESTROY:
            AeonAgentPipe::Stop();
            PostQuitMessage(0);
            return 0;

        case WM_PAINT: {
            PAINTSTRUCT ps;
            HDC hdc = BeginPaint(h, &ps);
            RECT rc; GetClientRect(h, &rc);
            PaintRetroChrome(h, hdc, rc);
            EndPaint(h, &ps);
            // Also paint the BrowserChrome tab strip over the retro base
            BrowserChrome::OnPaint(h);
            return 0;
        }

        case WM_SIZE: {
            int width  = LOWORD(l);
            int height = HIWORD(l);
            BrowserChrome::OnSize(h, width, height);
            return 0;
        }

        case WM_LBUTTONDOWN: {
            int x = (short)LOWORD(l);
            int y = (short)HIWORD(l);
            BrowserChrome::OnLButtonDown(h, x, y);
            return 0;
        }

        case WM_LBUTTONUP: {
            int x = (short)LOWORD(l);
            int y = (short)HIWORD(l);
            BrowserChrome::OnLButtonUp(h, x, y);
            return 0;
        }

        case WM_MOUSEMOVE: {
            int x = (short)LOWORD(l);
            int y = (short)HIWORD(l);
            BrowserChrome::OnMouseMove(h, x, y);
            return 0;
        }

        case WM_COMMAND:
            BrowserChrome::OnCommand(h, LOWORD(w), HIWORD(w), (HWND)l);
            return 0;

        case WM_KEYDOWN:
        case WM_SYSKEYDOWN:
            if (BrowserChrome::OnKeyDown(h, w, l))
                return 0;
            break;

        case WM_AEON_AGENT:
            AeonAgentPipe::HandleCommand(w, l);
            return 0;

        case WM_AEONBRIDGE:
            AeonBridge::HandleWmAeonBridge(w, l);
            return 0;
    }
    return DefWindowProcW(h, m, w, l);
}

// ---------------------------------------------------------------------------
// EraChrome Public API
// ---------------------------------------------------------------------------
bool EraChrome::IsModern() const {
    return m_Profile.tier >= AeonTier::Win8_Modern;
}

bool EraChrome::IsDwmAvailable() const {
    // DWM is present from Vista but can be disabled
    BOOL enabled = FALSE;
    if (DwmIsCompositionEnabled(&enabled) != S_OK) return false;
    return enabled == TRUE;
}

void EraChrome::CreateBrowserWindow() {
    // Select skin based on tier
    bool modern = IsModern();
    WNDCLASSEXW wc = {};
    wc.cbSize        = sizeof(wc);
    wc.hCursor       = LoadCursor(nullptr, IDC_ARROW);
    wc.hbrBackground = modern
        ? (HBRUSH)(COLOR_WINDOW + 1)
        : CreateSolidBrush(RGB(192, 192, 192));
    wc.lpfnWndProc   = modern ? ModernWndProc : RetroWndProc;
    wc.lpszClassName = modern ? CLASS_MODERN : CLASS_RETRO;
    wc.hInstance     = m_hInst;
    wc.hIcon         = LoadIcon(m_hInst, MAKEINTRESOURCE(1));
    RegisterClassExW(&wc);

    DWORD style = WS_OVERLAPPEDWINDOW;
    // Modern: start maximised; Retro: standard 800×600 (matches retro screens)
    int x = CW_USEDEFAULT, y = CW_USEDEFAULT;
    int w = modern ? CW_USEDEFAULT : 800;
    int h = modern ? CW_USEDEFAULT : 600;

    m_hwnd = CreateWindowExW(
        modern ? WS_EX_APPWINDOW : 0,
        modern ? CLASS_MODERN : CLASS_RETRO,
        L"Aeon Browser — by DelgadoLogic",
        style, x, y, w, h,
        nullptr, nullptr, m_hInst, nullptr);

    if (!m_hwnd) {
        fprintf(stderr, "[EraChrome] CreateWindowEx failed: %lu\n",
            GetLastError());
        return;
    }

    if (modern && IsDwmAvailable()) {
        ApplyMicaBackdrop(m_hwnd);
        ApplyRoundedCorners(m_hwnd);
    }

    // ── Wire BrowserChrome to the window ────────────────────────────────
    BrowserChrome::Create(m_hwnd, &m_Profile, m_engine);

    // ── Start agent control pipe ────────────────────────────────────────
    AeonAgentPipe::Start(m_hwnd);

    ShowWindow(m_hwnd, m_nCmdShow);
    UpdateWindow(m_hwnd);
    fprintf(stdout, "[EraChrome] %s skin applied.\n",
        modern ? "Modern (Mica/Acrylic)" : "Retro (Classic Win32)");
}

int EraChrome::Run(const char* cmdLine) {
    (void)cmdLine; // TODO: parse initial URL from cmdLine
    MSG msg = {};
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    return static_cast<int>(msg.wParam);
}
