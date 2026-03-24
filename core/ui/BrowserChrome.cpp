// AeonBrowser — BrowserChrome.cpp
// DelgadoLogic | Lead UI Engineer
//
// PURPOSE: Draws the Aeon browser chrome — the native Win32 toolbar that IS
// the browser UI. This replaces EraChrome's generic shell with the exact
// design seen in the UI mockup:
//
//   ┌─────────────────────────────────────────────────────────────────────┐
//   │ [A]  ←  →  ↻  │  🔒 https://delgadologic.tech  [⬇][★][🧅][⋮]  [_][□][✕] │  ← TITLEBAR+NAV (40px)
//   ├──────────────────────────────────────────────────────────────────────┤
//   │ [⚡ DelgadoLogic ×]  [🐙 GitHub ×]  [+ New Tab ×]          [+]    │  ← TAB STRIP (32px)
//   ├──────────────────────────────────────────────────────────────────────┤
//   │                                                                      │
//   │                     aeon://newtab                                    │  ← CONTENT AREA
//   │                                                                      │
//   └──────────────────────────────────────────────────────────────────────┘
//
// DESIGN TOKENS (from mockup):
//   --bg-primary:   #0d0e14   (titlebar, outer shell)
//   --bg-card:      #16182a   (URL bar, tab hover)
//   --bg-active:    #1e2140   (active tab bg)
//   --accent:       #6c63ff   (active tab underline, URL bar focus, A badge)
//   --accent-2:     #a78bfa   (hover states, A badge gradient end)
//   --text:         #e8e8f0   (primary text)
//   --text-dim:     #8888aa   (inactive tab text, toolbar icons)
//   --text-faint:   #44445a   (placeholder, separators)
//   --green:        #22c55e   (lock icon, AdBlock dot, HTTPS indicator)
//
// RENDERING PIPELINE:
//   1. BrowserChrome::Create()          — creates the host HWND
//   2. BrowserChrome::OnPaint()         — GDI drawing of all chrome elements
//   3. BrowserChrome::OnLButtonDown()   — hit-test buttons, tabs, URL bar
//   4. BrowserChrome::OnMouseMove()     — hover states + tooltip
//   5. BrowserChrome::SetEngine()       — bind to AeonEngineVTable
//
// LEGACY TIERS: On Win9x/XP, DWM APIs (glass, blur) are not called.
// The colors stay the same; we lose only the blur-behind effect.
// On Win16 (retro tier) this file is NOT compiled — aeon16.c handles UI.

#include "BrowserChrome.h"
#include "../../core/engine/AeonEngine_Interface.h"
#include "../../core/probe/HardwareProbe.h"
#include "../../core/settings/SettingsEngine.h"
#include <windows.h>
#include <dwmapi.h>
#include <cstring>
#include <cstdio>
#include <vector>
#include <string>

#pragma comment(lib, "dwmapi.lib")

// ---------------------------------------------------------------------------
// Design tokens as Win32 COLORREFs
// ---------------------------------------------------------------------------
#define CLR_BG_PRIMARY   RGB(13,  14,  20)   // #0d0e14
#define CLR_BG_CARD      RGB(22,  24,  42)   // #16182a
#define CLR_BG_ACTIVE    RGB(30,  33,  64)   // #1e2140
#define CLR_ACCENT       RGB(108, 99,  255)  // #6c63ff
#define CLR_ACCENT2      RGB(167, 139, 250)  // #a78bfa
#define CLR_TEXT         RGB(232, 232, 240)  // #e8e8f0
#define CLR_TEXT_DIM     RGB(136, 136, 170)  // #8888aa
#define CLR_TEXT_FAINT   RGB(68,  68,  90)   // #44445a
#define CLR_GREEN        RGB(34,  197, 94)   // #22c55e

// Chrome dimensions (pixels)
static const int NAV_HEIGHT  = 40;  // titlebar + navigation row
static const int TAB_HEIGHT  = 32;  // tab strip
static const int CHROME_H    = NAV_HEIGHT + TAB_HEIGHT; // 72px total chrome

// Button zones (nav bar, from left)
static const int BTN_LOGO_X  = 8;   static const int BTN_LOGO_W  = 36;
static const int BTN_BACK_X  = 52;  static const int BTN_BACK_W  = 28;
static const int BTN_FWD_X   = 82;  static const int BTN_FWD_W   = 28;
static const int BTN_REF_X   = 112; static const int BTN_REF_W   = 28;
static const int URLBAR_PAD  = 148; // URL bar starts here
static const int URLBAR_END  = 120; // pixels from right edge (for icon buttons)

// ---------------------------------------------------------------------------
// Per-tab state (mirrors AeonEngineVTable's tab model)
// ---------------------------------------------------------------------------
struct ChromeTab {
    unsigned int  id;
    std::string   url;
    std::string   title;
    bool          loading;
    RECT          tabRect;  // computed during paint
};

// ---------------------------------------------------------------------------
// BrowserChrome implementation
// ---------------------------------------------------------------------------

struct BrowserChrome {
    HWND                hwnd;
    HWND                hUrlBar;     // Edit control for URL entry
    HWND                hContent;    // child HWND for engine viewport

    std::vector<ChromeTab> tabs;
    int                 activeTab;
    int                 hoverTab;    // -1 = none
    int                 hoverBtn;    // 0=none 1=back 2=fwd 3=ref 4=tor 5=dl

    AeonEngineVTable*   engine;
    AeonSettings        settings;
    const SystemProfile* profile;

    bool                urlFocused;
    bool                settingsOpen;
};

// ---------------------------------------------------------------------------
// GDI helpers
// ---------------------------------------------------------------------------
static void FillRectColor(HDC hdc, const RECT& r, COLORREF c) {
    HBRUSH b = CreateSolidBrush(c);
    FillRect(hdc, &r, b);
    DeleteObject(b);
}

static void DrawRoundRect(HDC hdc, const RECT& r, int rx, COLORREF fill, COLORREF stroke) {
    HBRUSH br = CreateSolidBrush(fill);
    HPEN   pn = CreatePen(PS_SOLID, 1, stroke);
    SelectObject(hdc, br);
    SelectObject(hdc, pn);
    RoundRect(hdc, r.left, r.top, r.right, r.bottom, rx, rx);
    DeleteObject(br);
    DeleteObject(pn);
}

static void DrawText16(HDC hdc, const char* text, const RECT& r,
                       COLORREF c, int ptSize = 9, bool bold = false) {
    int ptH = -MulDiv(ptSize, GetDeviceCaps(hdc, LOGPIXELSY), 72);
    HFONT f = CreateFontA(ptH, 0, 0, 0, bold ? FW_SEMIBOLD : FW_NORMAL,
        0, 0, 0, ANSI_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
        CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_SWISS, "Segoe UI");
    HFONT old = (HFONT)SelectObject(hdc, f);
    SetTextColor(hdc, c);
    SetBkMode(hdc, TRANSPARENT);
    DrawTextA(hdc, text, -1, const_cast<LPRECT>(&r),
        DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
    SelectObject(hdc, old);
    DeleteObject(f);
}

// ---------------------------------------------------------------------------
// Paint the "A" logo badge — the signature element from the mockup
// ---------------------------------------------------------------------------
static void DrawLogoBadge(HDC hdc, int x, int y) {
    // Outer rounded square (gradient approximated as solid accent)
    RECT badge = { x, y, x + 28, y + 28 };
    DrawRoundRect(hdc, badge, 8, CLR_ACCENT, CLR_ACCENT2);

    // "A" lettermark — bold, white, slightly italic feel via offset
    RECT textR = { x + 2, y + 1, x + 28, y + 28 };
    int ptH = -MulDiv(16, GetDeviceCaps(hdc, LOGPIXELSY), 72);
    HFONT f = CreateFontA(ptH, 0, 0, 0, FW_BOLD, TRUE, 0, 0,
        ANSI_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
        CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_SWISS, "Segoe UI");
    HFONT old = (HFONT)SelectObject(hdc, f);
    SetTextColor(hdc, RGB(255, 255, 255));
    SetBkMode(hdc, TRANSPARENT);
    DrawTextA(hdc, "A", -1, &textR, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    SelectObject(hdc, old);
    DeleteObject(f);
}

// ---------------------------------------------------------------------------
// Paint nav bar (back/fwd/refresh + URL bar + right icons)
// ---------------------------------------------------------------------------
static void PaintNavBar(BrowserChrome* ch, HDC hdc, int width) {
    // Nav bar background
    RECT navR = { 0, 0, width, NAV_HEIGHT };
    FillRectColor(hdc, navR, CLR_BG_PRIMARY);

    // "A" logo badge — top-left, vertically centered
    DrawLogoBadge(hdc, BTN_LOGO_X, (NAV_HEIGHT - 28) / 2);

    // Nav buttons: ← → ↻
    struct { int x; const char* ch; } btns[] = {
        { BTN_BACK_X, "<" },
        { BTN_FWD_X,  ">" },
        { BTN_REF_X,  "R" }
    };
    for (auto& b : btns) {
        RECT br = { b.x, 6, b.x + BTN_BACK_W, NAV_HEIGHT - 6 };
        DrawRoundRect(hdc, br, 6, CLR_BG_CARD, CLR_TEXT_FAINT);
        DrawText16(hdc, b.ch, br, CLR_TEXT_DIM, 9, true);
    }

    // URL bar
    int urlLeft  = URLBAR_PAD;
    int urlRight = width - URLBAR_END;
    RECT urlR = { urlLeft, 6, urlRight, NAV_HEIGHT - 6 };
    COLORREF urlBorder = ch->urlFocused ? CLR_ACCENT : CLR_BG_CARD;
    DrawRoundRect(hdc, urlR, 12, CLR_BG_CARD, urlBorder);

    // Lock icon (green dot as placeholder)
    HBRUSH lockB = CreateSolidBrush(CLR_GREEN);
    RECT lockR = { urlLeft + 10, 16, urlLeft + 17, 24 };
    FillRect(hdc, &lockR, lockB);
    DeleteObject(lockB);

    // URL text
    const char* urlTxt = "about:blank";
    if (!ch->tabs.empty() && ch->activeTab >= 0 &&
        ch->activeTab < (int)ch->tabs.size()) {
        const auto& t = ch->tabs[ch->activeTab];
        urlTxt = t.url.c_str();
    }
    RECT urlTextR = { urlLeft + 22, 6, urlRight - 32, NAV_HEIGHT - 6 };
    DrawText16(hdc, urlTxt, urlTextR, CLR_TEXT, 9);

    // AdBlock shield — green dot
    HBRUSH shieldB = CreateSolidBrush(CLR_GREEN);
    RECT shieldR = { urlRight - 28, 16, urlRight - 21, 24 };
    HRGN shieldRgn = CreateEllipticRgn(shieldR.left, shieldR.top,
        shieldR.right, shieldR.bottom);
    FillRgn(hdc, shieldRgn, shieldB);
    DeleteObject(shieldRgn);
    DeleteObject(shieldB);

    // Right icon buttons: Downloads(⬇), Bookmarks(★), Tor(🧅), Menu(⋮)
    const char* rightIcons[] = { "v", "*", "o", "." };
    COLORREF torColor = ch->settings.tor_enabled ? CLR_GREEN : CLR_TEXT_DIM;
    COLORREF iconColors[] = { CLR_TEXT_DIM, CLR_TEXT_DIM, torColor, CLR_TEXT_DIM };
    for (int i = 0; i < 4; i++) {
        RECT ir = { width - URLBAR_END + 4 + i * 26, 8,
                    width - URLBAR_END + 4 + i * 26 + 22, NAV_HEIGHT - 8 };
        DrawText16(hdc, rightIcons[i], ir, iconColors[i], 10, true);
    }

    // Window control buttons: _ □ ✕
    RECT minR  = { width - 90, 0, width - 60, NAV_HEIGHT };
    RECT maxR  = { width - 60, 0, width - 30, NAV_HEIGHT };
    RECT clsR  = { width - 30, 0, width,      NAV_HEIGHT };
    DrawText16(hdc, "-", minR, CLR_TEXT_DIM, 9);
    DrawText16(hdc, "□", maxR, CLR_TEXT_DIM, 9);
    // Close button gets red on hover — for now always dim
    DrawText16(hdc, "x", clsR, CLR_TEXT_DIM, 9, true);
}

// ---------------------------------------------------------------------------
// Paint tab strip
// ---------------------------------------------------------------------------
static void PaintTabStrip(BrowserChrome* ch, HDC hdc, int width) {
    RECT tabStrip = { 0, NAV_HEIGHT, width, NAV_HEIGHT + TAB_HEIGHT };
    FillRectColor(hdc, tabStrip, RGB(10, 11, 17)); // slightly darker than nav

    // Bottom separator line
    RECT sep = { 0, NAV_HEIGHT + TAB_HEIGHT - 1, width, NAV_HEIGHT + TAB_HEIGHT };
    FillRectColor(hdc, sep, CLR_TEXT_FAINT);

    int tabX = 8;
    const int TAB_W_MAX  = 200;
    const int TAB_W_MIN  = 80;
    const int TAB_MARGIN = 4;

    int tabCount = max(1, (int)ch->tabs.size());
    int tabW = min(TAB_W_MAX, max(TAB_W_MIN, (width - 60) / tabCount));

    for (int i = 0; i < (int)ch->tabs.size(); i++) {
        ChromeTab& t = ch->tabs[i];
        bool active = (i == ch->activeTab);
        bool hover  = (i == ch->hoverTab);

        RECT tR = { tabX, NAV_HEIGHT + 2, tabX + tabW, NAV_HEIGHT + TAB_HEIGHT - 1 };
        t.tabRect = tR;

        // Tab background
        COLORREF tabBg  = active ? CLR_BG_ACTIVE : (hover ? CLR_BG_CARD : RGB(10,11,17));
        COLORREF tabBrd = active ? CLR_ACCENT     : CLR_TEXT_FAINT;
        DrawRoundRect(hdc, tR, 6, tabBg, tabBrd);

        // Active tab: bottom violet glow bar
        if (active) {
            RECT glow = { tR.left + 3, tR.bottom - 2, tR.right - 3, tR.bottom + 1 };
            FillRectColor(hdc, glow, CLR_ACCENT);
        }

        // Tab favicon (small colored dot as placeholder until favicon system is live)
        HBRUSH favB = CreateSolidBrush(active ? CLR_ACCENT2 : CLR_TEXT_FAINT);
        RECT favR = { tR.left + 8, tR.top + 9, tR.left + 16, tR.top + 17 };
        HRGN favRgn = CreateEllipticRgn(favR.left, favR.top, favR.right, favR.bottom);
        FillRgn(hdc, favRgn, favB);
        DeleteObject(favRgn);
        DeleteObject(favB);

        // Tab title
        RECT titleR = { tR.left + 20, tR.top, tR.right - 18, tR.bottom };
        DrawText16(hdc, t.title.empty() ? "New Tab" : t.title.c_str(),
            titleR, active ? CLR_TEXT : CLR_TEXT_DIM, 8);

        // Close ×
        RECT closeR = { tR.right - 18, tR.top + 2, tR.right - 2, tR.bottom - 2 };
        DrawText16(hdc, "x", closeR, CLR_TEXT_FAINT, 7);

        tabX += tabW + TAB_MARGIN;
    }

    // New tab (+) button
    RECT addR = { tabX + 4, NAV_HEIGHT + 6, tabX + 26, NAV_HEIGHT + TAB_HEIGHT - 6 };
    DrawRoundRect(hdc, addR, 5, CLR_BG_CARD, CLR_TEXT_FAINT);
    DrawText16(hdc, "+", addR, CLR_TEXT_DIM, 11, true);
}

// ---------------------------------------------------------------------------
// Full chrome paint
// ---------------------------------------------------------------------------
static void PaintChrome(BrowserChrome* ch) {
    RECT rc;
    GetClientRect(ch->hwnd, &rc);
    int W = rc.right;

    // Double-buffered paint
    HDC hdc    = GetDC(ch->hwnd);
    HDC memDC  = CreateCompatibleDC(hdc);
    HBITMAP bmp = CreateCompatibleBitmap(hdc, W, CHROME_H);
    HBITMAP old = (HBITMAP)SelectObject(memDC, bmp);

    PaintNavBar(ch, memDC, W);
    PaintTabStrip(ch, memDC, W);

    // Blit to screen
    BitBlt(hdc, 0, 0, W, CHROME_H, memDC, 0, 0, SRCCOPY);
    SelectObject(memDC, old);
    DeleteObject(bmp);
    DeleteDC(memDC);
    ReleaseDC(ch->hwnd, hdc);
}

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------
namespace BrowserChrome {

void Create(HWND parent, const SystemProfile* profile, AeonEngineVTable* engine) {
    BrowserChrome* ch = new BrowserChrome();
    ch->profile    = profile;
    ch->engine     = engine;
    ch->activeTab  = -1;
    ch->hoverTab   = -1;
    ch->hoverBtn   = 0;
    ch->urlFocused = false;
    ch->settings   = SettingsEngine::Load();

    // Add a default new tab
    if (engine) {
        ChromeTab t;
        t.id      = engine->NewTab(parent, "aeon://newtab");
        t.url     = "aeon://newtab";
        t.title   = "New Tab";
        t.loading = false;
        ch->tabs.push_back(t);
        ch->activeTab = 0;

        // Set engine viewport below the chrome
        RECT rc; GetClientRect(parent, &rc);
        engine->SetViewport(t.id, parent, 0, CHROME_H,
            rc.right, rc.bottom - CHROME_H);
    }

    ch->hwnd = parent;
    PaintChrome(ch);

    // Store in GWLP_USERDATA so WM_PAINT can find it
    SetWindowLongPtr(parent, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(ch));

    fprintf(stdout, "[Chrome] Browser chrome created. Logo badge: active. "
        "Tab strip: %d tab(s).\n", (int)ch->tabs.size());
}

void OnPaint(HWND hwnd) {
    BrowserChrome* ch = reinterpret_cast<BrowserChrome*>(
        GetWindowLongPtr(hwnd, GWLP_USERDATA));
    if (ch) PaintChrome(ch);
}

void OnSize(HWND hwnd, int w, int h) {
    BrowserChrome* ch = reinterpret_cast<BrowserChrome*>(
        GetWindowLongPtr(hwnd, GWLP_USERDATA));
    if (!ch) return;
    PaintChrome(ch);
    if (ch->engine && ch->activeTab >= 0) {
        auto& t = ch->tabs[ch->activeTab];
        ch->engine->SetViewport(t.id, hwnd, 0, CHROME_H, w, h - CHROME_H);
    }
}

} // namespace BrowserChrome
