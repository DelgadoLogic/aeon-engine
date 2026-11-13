// AeonBrowser — DownloadButton.cpp
// DelgadoLogic | UI Team
//
// Toolbar download indicator — same concept as Chrome's ⬇ button.
//
// Design:
//   - 36×36 owner-drawn button in the right side of the address toolbar
//   - Invisible when idle (no active or recent downloads)
//   - Icon: ⬇ arrow with a thin circular progress arc around it
//   - Badge: small violet circle with count, e.g. "3"
//   - Progress arc animates via a 200ms timer
//   - Hover: slight glow + tooltip "Downloads (Ctrl+J)"
//   - Click: navigates to aeon://downloads

#include "DownloadButton.h"
#include "DwmHelper.h"
#include <commctrl.h>
#pragma comment(lib, "comctl32.lib")
#include <windowsx.h>
#include <objidl.h>
#include <gdiplus.h>
#pragma comment(lib, "gdiplus.lib")
#include <algorithm>
#include <cmath>
#include <cstdio>

#pragma comment(lib, "Msimg32.lib")

namespace DownloadButton {

// Obsidian Design System Tokens
static const COLORREF C_PARENT_BG = RGB(14, 17, 34);    // #0E1122 Nav bar base
static const COLORREF C_ACCENT    = RGB(147, 51, 234);  // #9333EA Electric Purple

static HWND      g_hwnd         = nullptr;
static HINSTANCE g_hInst        = nullptr;
static HFONT     g_fontBadge    = nullptr;
static ULONG_PTR g_gdiplusToken = 0;
static int       g_dpi          = 96;
static int       g_count        = 0;
static int       g_pct          = -1;   // -1 = indeterminate
static bool      g_hover        = false;
static float     g_spin         = 0.0f; // indeterminate spinner angle
static UINT_PTR  g_timer        = 0;
static UINT_PTR  g_hideTimer    = 0;

static inline int S(int px) {
    return MulDiv(px, g_dpi > 0 ? g_dpi : 96, 96);
}

static void EnsureGdiplus() {
    if (!g_gdiplusToken) {
        Gdiplus::GdiplusStartupInput input;
        Gdiplus::GdiplusStartup(&g_gdiplusToken, &input, nullptr);
    }
}

static void RebuildFonts() {
    if (g_fontBadge) {
        DeleteObject(g_fontBadge);
        g_fontBadge = nullptr;
    }
    g_fontBadge = CreateFontW(-S(9), 0, 0, 0, FW_BOLD, 0, 0, 0, DEFAULT_CHARSET,
        OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Segoe UI");
}

// ─── Paint (Double-Buffered Anti-Aliased GDI+ Progress Ring & Vector Glyph) ──
static void Paint(HDC hdc, RECT rc) {
    int W = rc.right - rc.left;
    int H = rc.bottom - rc.top;
    if (W <= 0 || H <= 0) return;

    EnsureGdiplus();

    // Seamless nav-bar background fill so rounded pill corners blend cleanly
    HBRUSH parentBg = CreateSolidBrush(C_PARENT_BG);
    FillRect(hdc, &rc, parentBg);
    DeleteObject(parentBg);

    float cx = (rc.left + rc.right) * 0.5f;
    float cy = (rc.top + rc.bottom) * 0.5f;
    float pillPad = (float)(std::max)(1, S(2));

    {
        Gdiplus::Graphics g(hdc);
        g.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
        g.SetPixelOffsetMode(Gdiplus::PixelOffsetModeHighQuality);

        // Rounded Obsidian pill container
        float pw = (float)W - pillPad * 2.0f;
        float ph = (float)H - pillPad * 2.0f;
        float rad = (std::min)(pw, ph) * 0.28f;
        Gdiplus::GraphicsPath pillPath;
        pillPath.AddArc(pillPad, pillPad, rad * 2.0f, rad * 2.0f, 180.0f, 90.0f);
        pillPath.AddArc(pillPad + pw - rad * 2.0f, pillPad, rad * 2.0f, rad * 2.0f, 270.0f, 90.0f);
        pillPath.AddArc(pillPad + pw - rad * 2.0f, pillPad + ph - rad * 2.0f, rad * 2.0f, rad * 2.0f, 0.0f, 90.0f);
        pillPath.AddArc(pillPad, pillPad + ph - rad * 2.0f, rad * 2.0f, rad * 2.0f, 90.0f, 90.0f);
        pillPath.CloseFigure();

        Gdiplus::SolidBrush pillBrush(g_hover
            ? Gdiplus::Color(255, 29, 34, 66)
            : Gdiplus::Color(255, 19, 23, 45));
        g.FillPath(&pillBrush, &pillPath);

        Gdiplus::Pen borderPen(g_hover
            ? Gdiplus::Color(180, 0, 229, 255)
            : Gdiplus::Color(110, 148, 163, 184), 1.0f);
        g.DrawPath(&borderPen, &pillPath);

        // Circular progress track + active arc
        float strokeW = (std::max)(1.8f, (float)S(2));
        float ringR = ((std::min)(W, H) * 0.5f) - (float)S(5);
        if (ringR > 4.0f) {
            Gdiplus::RectF arcRect(cx - ringR, cy - ringR, ringR * 2.0f, ringR * 2.0f);
            Gdiplus::Pen trackPen(Gdiplus::Color(140, 42, 46, 84), strokeW);
            g.DrawEllipse(&trackPen, arcRect);

            if (g_count > 0 || g_pct > 0) {
                float startDeg = -90.0f;
                float sweepDeg = 0.0f;
                if (g_pct < 0) {
                    startDeg = g_spin;
                    sweepDeg = 250.0f;
                } else {
                    int clamped = (std::max)(0, (std::min)(100, g_pct));
                    sweepDeg = (clamped / 100.0f) * 360.0f;
                }
                Gdiplus::Color arcCol = (g_count == 0 && g_pct >= 100)
                    ? Gdiplus::Color(255, 34, 197, 94)    // Emerald complete
                    : Gdiplus::Color(255, 0, 229, 255);   // Neon Cyan active
                Gdiplus::Pen fgPen(arcCol, strokeW);
                fgPen.SetStartCap(Gdiplus::LineCapRound);
                fgPen.SetEndCap(Gdiplus::LineCapRound);
                g.DrawArc(&fgPen, arcRect, startDeg, sweepDeg);
            }
        }

        // Crisp vector download arrow + tray glyph
        float iconScale = (float)g_dpi / 96.0f;
        float glyphStroke = (std::max)(1.5f, 1.6f * iconScale);
        Gdiplus::Color glyphColor = (g_count == 0 && g_pct >= 100)
            ? Gdiplus::Color(255, 34, 197, 94)
            : Gdiplus::Color(255, 248, 250, 252);
        Gdiplus::Pen glyphPen(glyphColor, glyphStroke);
        glyphPen.SetStartCap(Gdiplus::LineCapRound);
        glyphPen.SetEndCap(Gdiplus::LineCapRound);
        glyphPen.SetLineJoin(Gdiplus::LineJoinRound);

        // Shaft & chevron
        g.DrawLine(&glyphPen, cx, cy - 4.5f * iconScale, cx, cy + 2.2f * iconScale);
        g.DrawLine(&glyphPen, cx - 3.2f * iconScale, cy - 0.8f * iconScale, cx, cy + 2.5f * iconScale);
        g.DrawLine(&glyphPen, cx + 3.2f * iconScale, cy - 0.8f * iconScale, cx, cy + 2.5f * iconScale);
        // Bottom tray baseline
        g.DrawLine(&glyphPen, cx - 4.2f * iconScale, cy + 4.8f * iconScale, cx + 4.2f * iconScale, cy + 4.8f * iconScale);

        // Count badge pill (top-right)
        if (g_count > 0) {
            float br = (float)(std::max)(7, S(7));
            float bx = (float)rc.right - br - (float)S(2);
            float by = (float)rc.top + br + (float)S(1);
            Gdiplus::SolidBrush badgeFill(Gdiplus::Color(255, GetRValue(C_ACCENT), GetGValue(C_ACCENT), GetBValue(C_ACCENT)));
            Gdiplus::Pen badgeRing(Gdiplus::Color(255, 14, 17, 34), 1.2f);
            g.FillEllipse(&badgeFill, bx - br, by - br, br * 2.0f, br * 2.0f);
            g.DrawEllipse(&badgeRing, bx - br, by - br, br * 2.0f, br * 2.0f);
        }
    }

    if (g_count > 0) {
        if (!g_fontBadge) RebuildFonts();
        wchar_t badge[8];
        if (g_count > 99) wcscpy_s(badge, L"99+");
        else _snwprintf_s(badge, _countof(badge), _TRUNCATE, L"%d", g_count);

        int br = (std::max)(7, S(7));
        int bx = rc.right - br - S(2);
        int by = rc.top + br + S(1);
        RECT brc = { bx - br, by - br, bx + br, by + br };
        SetBkMode(hdc, TRANSPARENT);
        HFONT oldFont = (HFONT)SelectObject(hdc, g_fontBadge);
        SetTextColor(hdc, RGB(255, 255, 255));
        DrawTextW(hdc, badge, -1, &brc, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        SelectObject(hdc, oldFont);
    }
}

// ─── Window Procedure ────────────────────────────────────────────────────────
static LRESULT CALLBACK WndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
    case WM_CREATE:
        g_dpi = (int)AeonDwm::GetWindowDpiSafe(hWnd);
        EnsureGdiplus();
        RebuildFonts();
        {
            HWND tip = CreateWindowExW(0, L"tooltips_class32", nullptr, WS_POPUP | TTS_ALWAYSTIP,
                0, 0, 0, 0, hWnd, nullptr, g_hInst, nullptr);
            if (tip) {
                TOOLINFOW ti = {};
                ti.cbSize = sizeof(ti);
                ti.uFlags = TTF_SUBCLASS | TTF_IDISHWND;
                ti.hwnd = hWnd;
                ti.uId = (UINT_PTR)hWnd;
                ti.lpszText = (LPWSTR)L"Downloads (Ctrl+J)";
                SendMessageW(tip, TTM_ADDTOOLW, 0, (LPARAM)&ti);
            }
        }
        return 0;

    case WM_ERASEBKGND:
        return 1;

    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC hdc = BeginPaint(hWnd, &ps);
        RECT rc;
        GetClientRect(hWnd, &rc);
        int w = (std::max)(1L, rc.right);
        int h = (std::max)(1L, rc.bottom);
        HDC mem = CreateCompatibleDC(hdc);
        HBITMAP bmp = CreateCompatibleBitmap(hdc, w, h);
        HBITMAP ob  = (HBITMAP)SelectObject(mem, bmp);
        Paint(mem, rc);
        BitBlt(hdc, 0, 0, w, h, mem, 0, 0, SRCCOPY);
        SelectObject(mem, ob);
        DeleteObject(bmp);
        DeleteDC(mem);
        EndPaint(hWnd, &ps);
        return 0;
    }

    case WM_TIMER:
        if (wParam == 1) {
            if (g_pct < 0 && g_count > 0) {
                g_spin = (float)fmod(g_spin + 10.0f, 360.0f);
                InvalidateRect(hWnd, nullptr, FALSE);
            }
        } else if (wParam == 2) {
            KillTimer(hWnd, 2);
            g_hideTimer = 0;
            if (g_count <= 0) {
                ShowWindow(hWnd, SW_HIDE);
            }
        }
        return 0;

    case WM_MOUSEMOVE:
        if (!g_hover) {
            g_hover = true;
            InvalidateRect(hWnd, nullptr, FALSE);
            TRACKMOUSEEVENT tme = { sizeof(tme), TME_LEAVE, hWnd, 0 };
            TrackMouseEvent(&tme);
        }
        return 0;

    case WM_MOUSELEAVE:
        g_hover = false;
        InvalidateRect(hWnd, nullptr, FALSE);
        return 0;

    case WM_LBUTTONUP:
        PostMessageW(GetParent(hWnd), WM_APP + 100, 0, 0);
        return 0;

    case WM_DESTROY:
        if (g_timer)     { KillTimer(hWnd, 1); g_timer = 0; }
        if (g_hideTimer) { KillTimer(hWnd, 2); g_hideTimer = 0; }
        if (g_fontBadge) { DeleteObject(g_fontBadge); g_fontBadge = nullptr; }
        g_hwnd = nullptr;
        return 0;
    }
    return DefWindowProcW(hWnd, msg, wParam, lParam);
}

// ─── Public API ──────────────────────────────────────────────────────────────
bool RegisterClass(HINSTANCE hInst) {
    g_hInst = hInst;
    WNDCLASSEXW wc = {};
    wc.cbSize        = sizeof(wc);
    wc.style         = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc   = WndProc;
    wc.hInstance     = hInst;
    wc.hCursor       = LoadCursor(nullptr, IDC_HAND);
    wc.hbrBackground = nullptr;
    wc.lpszClassName = L"AeonDownloadBtn";
    return RegisterClassExW(&wc) != 0 || GetLastError() == ERROR_CLASS_ALREADY_EXISTS;
}

HWND Create(HWND parent, int x, int y, int size) {
    g_dpi = (int)AeonDwm::GetWindowDpiSafe(parent);
    g_hwnd = CreateWindowExW(0, L"AeonDownloadBtn", L"",
        WS_CHILD | WS_CLIPSIBLINGS,   // starts hidden — shown by Update()
        x, y, size, size,
        parent, (HMENU)IDC_DOWNLOAD_BTN, g_hInst, nullptr);
    return g_hwnd;
}

void Update(int activeCount, int totalPct) {
    g_count = activeCount;
    g_pct   = totalPct;

    if (!g_hwnd) return;

    if (activeCount > 0) {
        if (g_hideTimer) {
            KillTimer(g_hwnd, 2);
            g_hideTimer = 0;
        }
        ShowWindow(g_hwnd, SW_SHOW);
        if (!g_timer) g_timer = SetTimer(g_hwnd, 1, 80, nullptr);
    } else {
        if (g_timer) {
            KillTimer(g_hwnd, 1);
            g_timer = 0;
        }
        g_pct = 100;
        if (g_hideTimer) KillTimer(g_hwnd, 2);
        g_hideTimer = SetTimer(g_hwnd, 2, 2000, nullptr);
    }
    InvalidateRect(g_hwnd, nullptr, FALSE);
}

void SetVisible(bool visible) {
    if (g_hwnd) ShowWindow(g_hwnd, visible ? SW_SHOW : SW_HIDE);
}

void Move(int x, int y, int size) {
    if (g_hwnd) SetWindowPos(g_hwnd, nullptr, x, y, size, size, SWP_NOZORDER | SWP_NOACTIVATE);
}

void OnDpiChanged(int newDpi) {
    if (newDpi <= 0) return;
    g_dpi = newDpi;
    RebuildFonts();
    if (g_hwnd && IsWindow(g_hwnd)) {
        InvalidateRect(g_hwnd, nullptr, FALSE);
    }
}

} // namespace DownloadButton
