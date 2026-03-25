// AeonBrowser — AppMenu.cpp
// DelgadoLogic | UI Team
//
// Chrome-style three-dot application menu — native Win32 custom-drawn popup.
//
// Implementation notes:
//   - Custom HWND (WS_POPUP | WS_EX_LAYERED) — not a Win32 HMENU
//   - GDI double-buffered painting (no flicker)
//   - Keyboard: arrow up/down navigate, Enter/Space activate, Escape close
//   - Submenus open as separate AppMenu windows (recursive)
//   - On Windows 11: DwmSetWindowAttribute DWMWA_WINDOW_CORNER_PREFERENCE = ROUND
//   - On Win10/8/7: manual GDI rounded rect clipping region
//   - Win11+ dark mode: DwmSetWindowAttribute DWMWA_USE_IMMERSIVE_DARK_MODE

#include "AppMenu.h"
#include <windowsx.h>
#include <dwmapi.h>
#include <cmath>
#include <cstring>
#include <vector>
#include <algorithm>

#pragma comment(lib, "dwmapi.lib")

namespace AppMenu {

// ─── Design constants ─────────────────────────────────────────────────────────
static const int MENU_WIDTH        = 320;
static const int ITEM_H            = 34;
static const int SECTION_H         = 28;
static const int SEP_H             = 9;
static const int ZOOM_H            = 42;
static const int PROFILE_H         = 50;
static const int PADDING_X         = 12;
static const int RADIUS            = 12;
static const int SHADOW_BLUR       = 20;
static const int ICON_COL          = 36;  // X where text starts after icon col

// Colors (matching Aeon dark theme)
static const COLORREF C_BG         = RGB(0x16,0x18,0x2a);  // --bg-card
static const COLORREF C_BORDER     = RGB(0x2a,0x28,0x55);  // --border solid
static const COLORREF C_TEXT       = RGB(0xe8,0xe8,0xf0);  // --text-primary
static const COLORREF C_DIM        = RGB(0x88,0x88,0xaa);  // --text-dim
static const COLORREF C_FAINT      = RGB(0x44,0x44,0x5a);  // --text-faint
static const COLORREF C_HOVERB     = RGB(0x1e,0x21,0x40);  // hover background
static const COLORREF C_ACCENT     = RGB(0x6c,0x63,0xff);  // --accent
static const COLORREF C_ACCENT2    = RGB(0xa7,0x8b,0xfa);  // --accent2
static const COLORREF C_DANGER     = RGB(0xef,0x44,0x44);  // red
static const COLORREF C_SEP        = RGB(0x22,0x24,0x38);  // separator

// ─── Global state ─────────────────────────────────────────────────────────────
static HWND        g_hwnd       = nullptr;
static HINSTANCE   g_hInst      = nullptr;
static MenuCallback g_callback;

static int         g_zoom       = 100;
static bool        g_fwActive   = false;
static std::wstring g_fwLabel   = L"Firewall Mode";
static int         g_hoverIdx   = -1;
static bool        g_visible    = false;

// ─── Menu definition ─────────────────────────────────────────────────────────
static std::vector<MenuItem> BuildMenuItems() {
    return {
        // Section 1: Tab management
        { ItemType::Action, ItemId::NewTab,       L"New tab",             L"Ctrl+T",         L"\U0001F5D0" },
        { ItemType::Action, ItemId::NewWindow,    L"New window",          L"Ctrl+N",         L"\U0001F5D4" },
        { ItemType::Action, ItemId::NewIncognito, L"New incognito window",L"Ctrl+Shift+N",   L"\U0001F575" },
        { ItemType::Separator },

        // Section 2: Profile
        { ItemType::ProfileRow, ItemId::Profile },
        { ItemType::Separator },

        // Section 3: Data
        { ItemType::Submenu, ItemId::Passwords,  L"Passwords & Autofill", L"",              L"\U0001F511" },
        { ItemType::Submenu, ItemId::History,    L"History",              L"",              L"\U0001F557" },
        { ItemType::Action,  ItemId::Downloads,  L"Downloads",            L"Ctrl+J",        L"\U00002B07" },
        { ItemType::Submenu, ItemId::Bookmarks,  L"Bookmarks",            L"",              L"\u2605" },
        { ItemType::Submenu, ItemId::Extensions, L"Extensions",           L"",              L"\U0001F9E9" },
        { ItemType::Action,  ItemId::DeleteData, L"Delete browsing data\u2026", L"Ctrl+Shift+Del", L"\U0001F5D1", true },
        { ItemType::Separator },

        // Section 4: Zoom
        { ItemType::ZoomRow, ItemId::Zoom },
        { ItemType::Separator },

        // Section 5: Aeon exclusive
        { ItemType::Submenu, ItemId::FirewallMode,     L"Firewall Mode",       L"",  L"\U0001F525", false, true },
        { ItemType::Submenu, ItemId::NetworkSentinel,  L"Network Sentinel",    L"",  L"\U0001F6E1", false, true },
        { ItemType::Separator },

        // Section 6: Tools
        { ItemType::Action,  ItemId::Print,      L"Print\u2026",          L"Ctrl+P",        L"\U0001F5A8" },
        { ItemType::Action,  ItemId::SavePageAs, L"Save page as\u2026",   L"Ctrl+S",        L"\U0001F4C4" },
        { ItemType::Action,  ItemId::FindInPage, L"Find in page\u2026",   L"Ctrl+F",        L"\U0001F50D" },
        { ItemType::Submenu, ItemId::MoreTools,  L"More tools",           L"",              L"\U0001F527" },
        { ItemType::Separator },

        // Section 7: Dev + System
        { ItemType::Action,  ItemId::DevTools,   L"Developer tools",      L"Ctrl+Shift+I",  L"\U0001F4BB" },
        { ItemType::Action,  ItemId::Settings,   L"Settings",             L"",              L"\u2699"     },
        { ItemType::Submenu, ItemId::Help,       L"Help",                 L"",              L"\u2753"     },
        { ItemType::Action,  ItemId::About,      L"About Aeon",           L"",              L"\u24B6"     },
        { ItemType::Action,  ItemId::Exit,       L"Exit",                 L"",              L"\u23FB"     },
    };
}

static std::vector<MenuItem> g_items;

// ─── Helpers: geometry ───────────────────────────────────────────────────────
static int TotalHeight() {
    int h = 8; // top padding
    for (auto& item : g_items) {
        switch (item.type) {
            case ItemType::Separator:  h += SEP_H;     break;
            case ItemType::ZoomRow:    h += ZOOM_H;    break;
            case ItemType::ProfileRow: h += PROFILE_H; break;
            default:                   h += ITEM_H;    break;
        }
    }
    h += 8; // bottom padding
    return h;
}

static int ItemTop(int idx) {
    int y = 8;
    for (int i = 0; i < idx; i++) {
        switch (g_items[i].type) {
            case ItemType::Separator:  y += SEP_H;     break;
            case ItemType::ZoomRow:    y += ZOOM_H;    break;
            case ItemType::ProfileRow: y += PROFILE_H; break;
            default:                   y += ITEM_H;    break;
        }
    }
    return y;
}

static int ItemHeight(int idx) {
    switch (g_items[idx].type) {
        case ItemType::Separator:  return SEP_H;
        case ItemType::ZoomRow:    return ZOOM_H;
        case ItemType::ProfileRow: return PROFILE_H;
        default:                   return ITEM_H;
    }
}

static bool IsClickable(ItemType t) {
    return t != ItemType::Separator && t != ItemType::Section;
}

// ─── GDI helpers ─────────────────────────────────────────────────────────────
static HFONT g_fontMain = nullptr;
static HFONT g_fontSmall = nullptr;
static HFONT g_fontBold = nullptr;

static void CreateFonts() {
    g_fontMain  = CreateFontW(-13,0,0,0,FW_NORMAL,0,0,0,DEFAULT_CHARSET,
        OUT_DEFAULT_PRECIS,CLIP_DEFAULT_PRECIS,CLEARTYPE_QUALITY,DEFAULT_PITCH,L"Segoe UI");
    g_fontSmall = CreateFontW(-11,0,0,0,FW_NORMAL,0,0,0,DEFAULT_CHARSET,
        OUT_DEFAULT_PRECIS,CLIP_DEFAULT_PRECIS,CLEARTYPE_QUALITY,DEFAULT_PITCH,L"Segoe UI");
    g_fontBold  = CreateFontW(-13,0,0,0,FW_SEMIBOLD,0,0,0,DEFAULT_CHARSET,
        OUT_DEFAULT_PRECIS,CLIP_DEFAULT_PRECIS,CLEARTYPE_QUALITY,DEFAULT_PITCH,L"Segoe UI");
}

static COLORREF ItemTextColor(const MenuItem& item) {
    if (item.danger)    return C_DANGER;
    if (item.highlight) return C_ACCENT2;
    return C_TEXT;
}

// ─── Paint ───────────────────────────────────────────────────────────────────
static void Paint(HDC hdc, RECT rc) {
    int W = rc.right, H = rc.bottom;

    // Background
    HBRUSH bgBrush = CreateSolidBrush(C_BG);
    FillRect(hdc, &rc, bgBrush);
    DeleteObject(bgBrush);

    // Border (1px)
    HPEN pen = CreatePen(PS_SOLID, 1, C_BORDER);
    HPEN old = (HPEN)SelectObject(hdc, pen);
    HBRUSH nb = (HBRUSH)GetStockObject(NULL_BRUSH);
    HBRUSH ob = (HBRUSH)SelectObject(hdc, nb);
    RoundRect(hdc, 0, 0, W, H, RADIUS*2, RADIUS*2);
    SelectObject(hdc, old); SelectObject(hdc, ob);
    DeleteObject(pen);

    // Items
    SelectObject(hdc, g_fontMain);
    SetBkMode(hdc, TRANSPARENT);

    for (int i = 0; i < (int)g_items.size(); i++) {
        const MenuItem& item = g_items[i];
        int y   = ItemTop(i);
        int ih  = ItemHeight(i);
        RECT irc = { 0, y, W, y + ih };

        // Hover highlight
        if (i == g_hoverIdx && IsClickable(item.type)) {
            HBRUSH hb = CreateSolidBrush(C_HOVERB);
            FillRect(hdc, &irc, hb);
            DeleteObject(hb);
        }

        // Firewall/Sentinel highlight: left accent bar
        if (item.highlight && item.type == ItemType::Action || item.type == ItemType::Submenu) {
            if (item.highlight) {
                HBRUSH ab = CreateSolidBrush(C_ACCENT);
                RECT bar = { 0, y + 6, 3, y + ih - 6 };
                FillRect(hdc, &bar, ab);
                DeleteObject(ab);
            }
        }

        switch (item.type) {
        case ItemType::Separator: {
            int sy = y + SEP_H/2;
            HPEN sp = CreatePen(PS_SOLID, 1, C_SEP);
            HPEN op = (HPEN)SelectObject(hdc, sp);
            MoveToEx(hdc, PADDING_X, sy, nullptr);
            LineTo(hdc, W - PADDING_X, sy);
            SelectObject(hdc, op); DeleteObject(sp);
            break;
        }

        case ItemType::ProfileRow: {
            // Avatar circle
            HBRUSH avBr = CreateSolidBrush(C_ACCENT);
            HPEN avPn   = (HPEN)GetStockObject(NULL_PEN);
            auto prevBr = SelectObject(hdc, avBr);
            auto prevPn = SelectObject(hdc, avPn);
            Ellipse(hdc, PADDING_X, y+10, PADDING_X+30, y+40);
            SelectObject(hdc, prevBr); SelectObject(hdc, prevPn);
            DeleteObject(avBr);

            // "A" in avatar
            SelectObject(hdc, g_fontBold);
            SetTextColor(hdc, RGB(255,255,255));
            RECT ar = { PADDING_X, y+10, PADDING_X+30, y+40 };
            DrawTextW(hdc, L"A", -1, &ar, DT_CENTER|DT_VCENTER|DT_SINGLELINE);

            // Name
            SelectObject(hdc, g_fontBold);
            SetTextColor(hdc, C_TEXT);
            RECT nr = { PADDING_X+38, y+8, W-80, y+26 };
            DrawTextW(hdc, L"Manuel", -1, &nr, DT_LEFT|DT_VCENTER|DT_SINGLELINE);

            // "Signed in" badge
            HBRUSH bb = CreateSolidBrush(C_ACCENT);
            RECT br = { W-76, y+15, W-PADDING_X, y+35 };
            HPEN bp = (HPEN)GetStockObject(NULL_PEN);
            SelectObject(hdc, bb); SelectObject(hdc, bp);
            RoundRect(hdc, br.left, br.top, br.right, br.bottom, 12, 12);
            DeleteObject(bb);
            SelectObject(hdc, g_fontSmall);
            SetTextColor(hdc, RGB(255,255,255));
            DrawTextW(hdc, L"Signed in", -1, &br, DT_CENTER|DT_VCENTER|DT_SINGLELINE);
            break;
        }

        case ItemType::ZoomRow: {
            // Label
            SelectObject(hdc, g_fontMain);
            SetTextColor(hdc, C_TEXT);
            RECT lr = { PADDING_X+ICON_COL, y, W/2, y+ZOOM_H };
            DrawTextW(hdc, L"Zoom", -1, &lr, DT_LEFT|DT_VCENTER|DT_SINGLELINE);

            // — pct% +
            wchar_t zstr[16];
            _snwprintf_s(zstr, sizeof(zstr)/sizeof(wchar_t), _TRUNCATE, L"%d%%", g_zoom);
            int cx = W/2;

            SetTextColor(hdc, C_DIM);
            RECT minR = { cx, y, cx+24, y+ZOOM_H };
            DrawTextW(hdc, L"\u2212", -1, &minR, DT_CENTER|DT_VCENTER|DT_SINGLELINE);

            SetTextColor(hdc, C_TEXT);
            SelectObject(hdc, g_fontBold);
            RECT pctR = { cx+24, y, cx+70, y+ZOOM_H };
            DrawTextW(hdc, zstr, -1, &pctR, DT_CENTER|DT_VCENTER|DT_SINGLELINE);

            SetTextColor(hdc, C_DIM);
            SelectObject(hdc, g_fontMain);
            RECT plusR = { cx+70, y, cx+94, y+ZOOM_H };
            DrawTextW(hdc, L"+", -1, &plusR, DT_CENTER|DT_VCENTER|DT_SINGLELINE);
            break;
        }

        default: {
            // Emoji/icon
            if (!item.emoji.empty()) {
                SelectObject(hdc, g_fontMain);
                SetTextColor(hdc, item.highlight ? C_ACCENT2 :
                             item.danger   ? C_DANGER  : C_DIM);
                RECT er = { PADDING_X, y, PADDING_X+ICON_COL, y+ih };
                DrawTextW(hdc, item.emoji.c_str(), -1, &er, DT_CENTER|DT_VCENTER|DT_SINGLELINE);
            }

            // Label
            const wchar_t* label = (item.id == ItemId::FirewallMode && g_fwActive)
                ? g_fwLabel.c_str() : item.label.c_str();

            SelectObject(hdc, g_fontMain);
            SetTextColor(hdc, ItemTextColor(item));
            RECT tr = { PADDING_X+ICON_COL, y, W-120, y+ih };
            DrawTextW(hdc, label, -1, &tr, DT_LEFT|DT_VCENTER|DT_SINGLELINE|DT_END_ELLIPSIS);

            // Shortcut
            if (!item.shortcut.empty()) {
                SetTextColor(hdc, C_FAINT);
                SelectObject(hdc, g_fontSmall);
                RECT sr = { W-120, y, W-24, y+ih };
                DrawTextW(hdc, item.shortcut.c_str(), -1, &sr, DT_RIGHT|DT_VCENTER|DT_SINGLELINE);
            }

            // Submenu arrow
            if (item.type == ItemType::Submenu) {
                SetTextColor(hdc, C_FAINT);
                SelectObject(hdc, g_fontMain);
                RECT ar2 = { W-22, y, W-6, y+ih };
                DrawTextW(hdc, L"\u25B8", -1, &ar2, DT_CENTER|DT_VCENTER|DT_SINGLELINE);
            }
            break;
        }
        } // switch
    }
}

// ─── Window procedure ─────────────────────────────────────────────────────────
static LRESULT CALLBACK WndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {

    case WM_CREATE:
        CreateFonts();
        g_items = BuildMenuItems();
        return 0;

    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC hdc = BeginPaint(hWnd, &ps);
        RECT rc; GetClientRect(hWnd, &rc);
        // Double-buffer
        HDC mem = CreateCompatibleDC(hdc);
        HBITMAP bmp = CreateCompatibleBitmap(hdc, rc.right, rc.bottom);
        HBITMAP old = (HBITMAP)SelectObject(mem, bmp);
        Paint(mem, rc);
        BitBlt(hdc, 0, 0, rc.right, rc.bottom, mem, 0, 0, SRCCOPY);
        SelectObject(mem, old); DeleteObject(bmp); DeleteDC(mem);
        EndPaint(hWnd, &ps);
        return 0;
    }

    case WM_MOUSEMOVE: {
        int mx = GET_X_LPARAM(lParam), my = GET_Y_LPARAM(lParam);
        int newHover = -1;
        for (int i = 0; i < (int)g_items.size(); i++) {
            if (!IsClickable(g_items[i].type)) continue;
            int y = ItemTop(i);
            if (my >= y && my < y + ItemHeight(i)) { newHover = i; break; }
        }
        if (newHover != g_hoverIdx) {
            g_hoverIdx = newHover;
            InvalidateRect(hWnd, nullptr, FALSE);
        }
        break;
    }

    case WM_LBUTTONUP: {
        int mx = GET_X_LPARAM(lParam), my = GET_Y_LPARAM(lParam);
        for (int i = 0; i < (int)g_items.size(); i++) {
            if (!IsClickable(g_items[i].type)) continue;
            int y = ItemTop(i);
            if (my >= y && my < y + ItemHeight(i)) {
                Hide();
                if (g_callback) g_callback(g_items[i].id);
                return 0;
            }
        }
        break;
    }

    case WM_KEYDOWN:
        if (wParam == VK_ESCAPE) { Hide(); return 0; }
        if (wParam == VK_DOWN) {
            do { g_hoverIdx = (g_hoverIdx + 1) % (int)g_items.size(); }
            while (!IsClickable(g_items[g_hoverIdx].type));
            InvalidateRect(hWnd, nullptr, FALSE);
        }
        if (wParam == VK_UP) {
            do { g_hoverIdx = (g_hoverIdx - 1 + (int)g_items.size()) % (int)g_items.size(); }
            while (!IsClickable(g_items[g_hoverIdx].type));
            InvalidateRect(hWnd, nullptr, FALSE);
        }
        if ((wParam == VK_RETURN || wParam == VK_SPACE) && g_hoverIdx >= 0) {
            ItemId id = g_items[g_hoverIdx].id;
            Hide();
            if (g_callback) g_callback(id);
        }
        return 0;

    case WM_KILLFOCUS:
        Hide();
        return 0;

    case WM_DESTROY:
        if (g_fontMain)  DeleteObject(g_fontMain);
        if (g_fontSmall) DeleteObject(g_fontSmall);
        if (g_fontBold)  DeleteObject(g_fontBold);
        g_fontMain = g_fontSmall = g_fontBold = nullptr;
        g_hwnd = nullptr; g_visible = false;
        return 0;
    }
    return DefWindowProcW(hWnd, msg, wParam, lParam);
}

// ─── Public API ───────────────────────────────────────────────────────────────
bool RegisterClass(HINSTANCE hInst) {
    g_hInst = hInst;
    WNDCLASSEXW wc = {};
    wc.cbSize        = sizeof(wc);
    wc.style         = CS_DROPSHADOW;
    wc.lpfnWndProc   = WndProc;
    wc.hInstance     = hInst;
    wc.hCursor       = LoadCursor(nullptr, IDC_ARROW);
    wc.hbrBackground = nullptr; // we paint everything ourselves
    wc.lpszClassName = L"AeonAppMenu";
    return RegisterClassExW(&wc) != 0;
}

void Show(HWND parent, POINT anchorPt, MenuCallback callback) {
    if (g_visible) Hide();
    g_callback = callback;
    g_items    = BuildMenuItems();

    int W = MENU_WIDTH;
    int H = TotalHeight();

    // Position: below the anchor, flush right
    int x = anchorPt.x - W;
    int y = anchorPt.y;

    // Keep on-screen
    int sw = GetSystemMetrics(SM_CXSCREEN);
    int sh = GetSystemMetrics(SM_CYSCREEN);
    if (x < 4) x = 4;
    if (x + W > sw - 4) x = sw - W - 4;
    if (y + H > sh - 4) y = anchorPt.y - H;

    g_hwnd = CreateWindowExW(
        WS_EX_TOPMOST | WS_EX_NOACTIVATE,
        L"AeonAppMenu", L"",
        WS_POPUP,
        x, y, W, H,
        parent, nullptr, g_hInst, nullptr);

    if (!g_hwnd) return;

    // Win11: request rounded corners
#ifndef DWMWA_WINDOW_CORNER_PREFERENCE
#define DWMWA_WINDOW_CORNER_PREFERENCE 33
#endif
    DWORD corner = 2; // DWMWCP_ROUND
    DwmSetWindowAttribute(g_hwnd, DWMWA_WINDOW_CORNER_PREFERENCE, &corner, sizeof(corner));

    // Win10+: dark mode title bar (not strictly needed for popup but good practice)
    BOOL dark = TRUE;
    DwmSetWindowAttribute(g_hwnd, 20 /* DWMWA_USE_IMMERSIVE_DARK_MODE */, &dark, sizeof(dark));

    ShowWindow(g_hwnd, SW_SHOWNOACTIVATE);
    SetFocus(g_hwnd);
    g_visible = true;
}

void Hide() {
    if (g_hwnd) {
        ShowWindow(g_hwnd, SW_HIDE);
        DestroyWindow(g_hwnd);
        g_hwnd   = nullptr;
        g_visible = false;
    }
}

void SetFirewallModeLabel(const wchar_t* label, bool active) {
    g_fwLabel  = label ? label : L"Firewall Mode";
    g_fwActive = active;
    // Update highlight on matching item
    for (auto& item : g_items)
        if (item.id == ItemId::FirewallMode) item.highlight = active;
    if (g_hwnd) InvalidateRect(g_hwnd, nullptr, FALSE);
}

void SetZoom(int pct) {
    g_zoom = pct;
    if (g_hwnd) InvalidateRect(g_hwnd, nullptr, FALSE);
}

bool IsVisible() { return g_visible; }

} // namespace AppMenu
