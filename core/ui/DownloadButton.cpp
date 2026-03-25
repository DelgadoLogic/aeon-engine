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
#include <windowsx.h>
#include <cmath>
#include <cstdio>

#pragma comment(lib, "Msimg32.lib")

namespace DownloadButton {

static const double PI = 3.14159265358979323846;

// Colors
static const COLORREF C_BG      = RGB(0x16,0x18,0x2a);
static const COLORREF C_HOVER   = RGB(0x1e,0x21,0x40);
static const COLORREF C_ACCENT  = RGB(0x6c,0x63,0xff);
static const COLORREF C_TEXT    = RGB(0xe8,0xe8,0xf0);
static const COLORREF C_ARC     = RGB(0x6c,0x63,0xff);
static const COLORREF C_ARC_BG  = RGB(0x2a,0x28,0x55);

static HWND      g_hwnd     = nullptr;
static HINSTANCE g_hInst    = nullptr;
static HFONT     g_fontIcon = nullptr;
static HFONT     g_fontBadge= nullptr;
static int       g_count    = 0;
static int       g_pct      = -1;   // -1 = indeterminate
static bool      g_hover    = false;
static float     g_spin     = 0.0f; // indeterminate spinner angle
static UINT_PTR  g_timer    = 0;

typedef HWND (*NavFunc)(const wchar_t*);
static NavFunc g_navFn = nullptr;

// ─── Paint ───────────────────────────────────────────────────────────────────
static void Paint(HDC hdc, RECT rc) {
    int CX = (rc.right  + rc.left) / 2;
    int CY = (rc.bottom + rc.top)  / 2;
    int R  = (rc.right  - rc.left) / 2 - 3; // progress arc radius

    // Background
    HBRUSH bg = CreateSolidBrush(g_hover ? C_HOVER : C_BG);
    FillRect(hdc, &rc, bg);
    DeleteObject(bg);

    // Progress arc background ring
    HPEN arcBgPen = CreatePen(PS_SOLID, 2, C_ARC_BG);
    HPEN oldPen   = (HPEN)SelectObject(hdc, arcBgPen);
    HBRUSH nb     = (HBRUSH)GetStockObject(NULL_BRUSH);
    HBRUSH ob     = (HBRUSH)SelectObject(hdc, nb);
    Ellipse(hdc, CX-R, CY-R, CX+R, CY+R);
    SelectObject(hdc, oldPen); DeleteObject(arcBgPen);

    // Progress arc (foreground) — GDI Arc
    if (g_pct > 0 || g_pct == -1) {
        HPEN arcPen = CreatePen(PS_SOLID, 2, C_ARC);
        SelectObject(hdc, arcPen);

        double startAngle, sweepAngle;
        if (g_pct < 0) {
            // Spinner: 270° sweep rotating
            startAngle = g_spin;
            sweepAngle = 270.0;
        } else {
            startAngle = -90.0;
            sweepAngle = (g_pct / 100.0) * 360.0;
        }

        // Convert to GDI Arc points (clockwise from start)
        double sa = startAngle * PI / 180.0;
        double ea = (startAngle + sweepAngle) * PI / 180.0;
        int x1 = CX + (int)(R * cos(sa));
        int y1 = CY + (int)(R * sin(sa));
        int x2 = CX + (int)(R * cos(ea));
        int y2 = CY + (int)(R * sin(ea));
        Arc(hdc, CX-R, CY-R, CX+R, CY+R, x1, y1, x2, y2);

        SelectObject(hdc, oldPen); DeleteObject(arcPen);
    }
    SelectObject(hdc, ob);

    // Download arrow icon ⬇
    SetBkMode(hdc, TRANSPARENT);
    SelectObject(hdc, g_fontIcon);
    SetTextColor(hdc, C_TEXT);
    DrawTextW(hdc, L"\u2B07", -1, (RECT*)&rc, DT_CENTER|DT_VCENTER|DT_SINGLELINE);

    // Badge (count)
    if (g_count > 0) {
        wchar_t badge[8]; _snwprintf_s(badge,8,_TRUNCATE,L"%d",g_count);
        int bx = rc.right  - 10;
        int by = rc.top    + 5;
        int br = 8;
        HBRUSH bb = CreateSolidBrush(C_ACCENT);
        HPEN   bp = (HPEN)GetStockObject(NULL_PEN);
        SelectObject(hdc, bb); SelectObject(hdc, bp);
        Ellipse(hdc, bx-br, by-br, bx+br, by+br);
        DeleteObject(bb);
        SelectObject(hdc, g_fontBadge);
        SetTextColor(hdc, RGB(255,255,255));
        RECT brc = { bx-br, by-br, bx+br, by+br };
        DrawTextW(hdc, badge, -1, &brc, DT_CENTER|DT_VCENTER|DT_SINGLELINE);
    }
}

// ─── Window Procedure ────────────────────────────────────────────────────────
static LRESULT CALLBACK WndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
    case WM_CREATE:
        g_fontIcon  = CreateFontW(-16,0,0,0,FW_NORMAL,0,0,0,DEFAULT_CHARSET,
            OUT_DEFAULT_PRECIS,CLIP_DEFAULT_PRECIS,CLEARTYPE_QUALITY,DEFAULT_PITCH,L"Segoe UI Emoji");
        g_fontBadge = CreateFontW(-9,0,0,0,FW_BOLD,0,0,0,DEFAULT_CHARSET,
            OUT_DEFAULT_PRECIS,CLIP_DEFAULT_PRECIS,CLEARTYPE_QUALITY,DEFAULT_PITCH,L"Segoe UI");
        // Tooltip
        {
            HWND tip = CreateWindowExW(0,TOOLTIPS_CLASS,nullptr,WS_POPUP|TTS_ALWAYSTIP,
                0,0,0,0,hWnd,nullptr,g_hInst,nullptr);
            TOOLINFOW ti = {}; ti.cbSize=sizeof(ti); ti.uFlags=TTF_SUBCLASS|TTF_IDISHWND;
            ti.hwnd=hWnd; ti.uId=(UINT_PTR)hWnd; ti.lpszText=(LPWSTR)L"Downloads (Ctrl+J)";
            SendMessageW(tip,TTM_ADDTOOLW,0,(LPARAM)&ti);
        }
        return 0;

    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC hdc = BeginPaint(hWnd,&ps);
        RECT rc; GetClientRect(hWnd,&rc);
        HDC mem = CreateCompatibleDC(hdc);
        HBITMAP bmp = CreateCompatibleBitmap(hdc,rc.right,rc.bottom);
        HBITMAP ob  = (HBITMAP)SelectObject(mem,bmp);
        Paint(mem,rc);
        BitBlt(hdc,0,0,rc.right,rc.bottom,mem,0,0,SRCCOPY);
        SelectObject(mem,ob); DeleteObject(bmp); DeleteDC(mem);
        EndPaint(hWnd,&ps);
        return 0;
    }

    case WM_TIMER:
        // Animate indeterminate spinner
        if (g_pct < 0 && g_count > 0) {
            g_spin = fmod(g_spin + 8.0f, 360.0f);
            InvalidateRect(hWnd,nullptr,FALSE);
        }
        break;

    case WM_MOUSEMOVE:
        if (!g_hover) {
            g_hover = true;
            InvalidateRect(hWnd,nullptr,FALSE);
            TRACKMOUSEEVENT tme={sizeof(tme),TME_LEAVE,hWnd,0};
            TrackMouseEvent(&tme);
        }
        break;

    case WM_MOUSELEAVE:
        g_hover = false; InvalidateRect(hWnd,nullptr,FALSE); break;

    case WM_LBUTTONUP:
        // Navigate to downloads page
        PostMessageW(GetParent(hWnd), WM_APP+100, 0, 0); // custom nav message
        break;

    case WM_DESTROY:
        if (g_timer)     { KillTimer(hWnd,1); g_timer=0; }
        if (g_fontIcon)  { DeleteObject(g_fontIcon);  g_fontIcon=nullptr; }
        if (g_fontBadge) { DeleteObject(g_fontBadge); g_fontBadge=nullptr; }
        g_hwnd=nullptr;
        return 0;
    }
    return DefWindowProcW(hWnd,msg,wParam,lParam);
}

// ─── Public API ──────────────────────────────────────────────────────────────
bool RegisterClass(HINSTANCE hInst) {
    g_hInst = hInst;
    WNDCLASSEXW wc = {};
    wc.cbSize      = sizeof(wc);
    wc.style       = CS_HREDRAW|CS_VREDRAW;
    wc.lpfnWndProc = WndProc;
    wc.hInstance   = hInst;
    wc.hCursor     = LoadCursor(nullptr,IDC_HAND);
    wc.hbrBackground = nullptr;
    wc.lpszClassName = L"AeonDownloadBtn";
    return RegisterClassExW(&wc) != 0;
}

HWND Create(HWND parent, int x, int y, int size) {
    g_hwnd = CreateWindowExW(0, L"AeonDownloadBtn", L"",
        WS_CHILD,   // starts hidden — shown by Update()
        x, y, size, size,
        parent, (HMENU)IDC_DOWNLOAD_BTN, g_hInst, nullptr);
    return g_hwnd;
}

void Update(int activeCount, int totalPct) {
    g_count = activeCount;
    g_pct   = totalPct;

    if (!g_hwnd) return;

    if (activeCount > 0) {
        ShowWindow(g_hwnd, SW_SHOW);
        if (!g_timer) g_timer = SetTimer(g_hwnd, 1, 200, nullptr);
    } else {
        // Keep visible briefly after completion (2 seconds), then hide
        if (!g_timer) SetTimer(g_hwnd, 2, 2000, nullptr);
    }
    InvalidateRect(g_hwnd, nullptr, FALSE);
}

void SetVisible(bool visible) {
    if (g_hwnd) ShowWindow(g_hwnd, visible ? SW_SHOW : SW_HIDE);
}

void Move(int x, int y, int size) {
    if (g_hwnd) SetWindowPos(g_hwnd,nullptr,x,y,size,size,SWP_NOZORDER);
}

} // namespace DownloadButton
