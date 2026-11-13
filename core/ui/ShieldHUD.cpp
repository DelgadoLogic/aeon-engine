// AeonBrowser — ShieldHUD.cpp
// DelgadoLogic | UI & Privacy Team
//
// Implements the Sovereign Shield Dropdown HUD using double-buffered GDI/GDI+.

#include "ShieldHUD.h"
#include "DwmHelper.h"
#include "../../privacy/ContentBlocker.h"
#include "../../core/network/DnsResolver.h"
#include "../../engines/gecko_stealth/aeon_gecko_stealth.h"
#include <windowsx.h>
#include <objidl.h>
#include <gdiplus.h>
#include <dwmapi.h>
#include <algorithm>
#include <unordered_set>
#include <string>
#include <vector>
#include <cstdio>
#include <cstring>

#pragma comment(lib, "dwmapi.lib")
#pragma comment(lib, "gdiplus.lib")

namespace ShieldHUD {

// ─── Design Dimensions & Metrics (96-DPI logical base) ─────────────────────
static const int HUD_W      = 344;
static const int HUD_H      = 444;
static const int RADIUS     = 12;
static const int PAD_X      = 16;

// ─── Colors (Obsidian Neon Palette) ─────────────────────────────────────────
static const COLORREF C_BG          = RGB(11,  6,  22);    // #0B0616 Deep Obsidian
static const COLORREF C_CARD        = RGB(17,  21,  40);   // #111528 Obsidian Glass Card
static const COLORREF C_CARD_HOVER  = RGB(28,  33,  62);   // #1C213E Card Hover
static const COLORREF C_BORDER      = RGB(45,  52,  92);   // #2D345C Border
static const COLORREF C_TEXT        = RGB(248, 250, 252);  // #F8FAFC Text Primary
static const COLORREF C_MUTED       = RGB(148, 163, 184);  // #94A3B8 Text Muted
static const COLORREF C_FAINT       = RGB(100, 116, 139);  // #64748B Text Faint
static const COLORREF C_GREEN       = RGB(34,  197, 94);   // #22C55E Neon Green
static const COLORREF C_CYAN        = RGB(0,   229, 255);  // #00E5FF Neon Cyan
static const COLORREF C_PURPLE      = RGB(168, 85,  247);  // #A855F7 Electric Purple
static const COLORREF C_ACCENT      = RGB(147, 51,  234);  // #9333EA Obsidian Violet
static const COLORREF C_ORANGE      = RGB(249, 115, 22);   // #F97316 Shield Amber
static const COLORREF C_RED         = RGB(239, 68,  68);   // #EF4444 Danger Red

// ─── State ───────────────────────────────────────────────────────────────────
static HWND                 g_hwnd        = nullptr;
static HINSTANCE            g_hInst       = nullptr;
static bool                 g_visible     = false;
static int                  g_dpi         = 96;
static std::string          g_currentUrl  = "";
static std::string          g_currentHost = "";
static bool                 g_shieldsUp   = true;
static int                  g_hoverId     = -1;

static ShieldToggleCallback g_toggleCb;
static ShieldActionCallback g_actionCb;

static std::unordered_set<std::string> g_DisabledDomains;

static inline int S(int px) {
    return MulDiv(px, g_dpi > 0 ? g_dpi : 96, 96);
}

static inline int UnS(int physPx) {
    return MulDiv(physPx, 96, g_dpi > 0 ? g_dpi : 96);
}

// Fonts
static HFONT g_fontTitle  = nullptr;
static HFONT g_fontBold   = nullptr;
static HFONT g_fontMain   = nullptr;
static HFONT g_fontSmall  = nullptr;
static HFONT g_fontLarge  = nullptr;
static HFONT g_fontIcon   = nullptr;

// ─── Hostname Utility ───────────────────────────────────────────────────────
static std::string ExtractHostFromUrl(const std::string& url) {
    if (url.empty()) return "aeon://newtab";
    if (url.find("aeon://") == 0) return url;
    if (url.find("about:") == 0) return url;
    if (url.find("file://") == 0) return "local file";

    size_t start = url.find("://");
    start = (start == std::string::npos) ? 0 : start + 3;
    size_t end = url.find_first_of("/?#:", start);
    if (end == std::string::npos) end = url.length();
    return url.substr(start, end - start);
}

bool IsShieldActiveForUrl(const char* url) {
    if (!url || !url[0]) return true;
    std::string host = ExtractHostFromUrl(url);
    if (host.empty() || host.find("aeon://") == 0 || host.find("about:") == 0 || host == "local file")
        return true;
    return g_DisabledDomains.find(host) == g_DisabledDomains.end();
}

void SetShieldActiveForUrl(const char* url, bool active) {
    if (!url || !url[0]) return;
    std::string host = ExtractHostFromUrl(url);
    if (host.empty()) return;
    if (active) g_DisabledDomains.erase(host);
    else g_DisabledDomains.insert(host);
}

// ─── GDI / GDI+ Helpers ──────────────────────────────────────────────────────
static void CreateFonts() {
    if (!g_fontTitle)
        g_fontTitle = CreateFontW(-S(14), 0, 0, 0, FW_BOLD, 0, 0, 0, DEFAULT_CHARSET,
            OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Segoe UI");
    if (!g_fontBold)
        g_fontBold = CreateFontW(-S(12), 0, 0, 0, FW_SEMIBOLD, 0, 0, 0, DEFAULT_CHARSET,
            OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Segoe UI");
    if (!g_fontMain)
        g_fontMain = CreateFontW(-S(12), 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET,
            OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Segoe UI");
    if (!g_fontSmall)
        g_fontSmall = CreateFontW(-S(10), 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET,
            OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Segoe UI");
    if (!g_fontLarge)
        g_fontLarge = CreateFontW(-S(18), 0, 0, 0, FW_BOLD, 0, 0, 0, DEFAULT_CHARSET,
            OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Segoe UI");
    if (!g_fontIcon)
        g_fontIcon = CreateFontW(-S(14), 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET,
            OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Segoe MDL2 Assets");
}

static void FreeFonts() {
    if (g_fontTitle) { DeleteObject(g_fontTitle); g_fontTitle = nullptr; }
    if (g_fontBold)  { DeleteObject(g_fontBold);  g_fontBold  = nullptr; }
    if (g_fontMain)  { DeleteObject(g_fontMain);  g_fontMain  = nullptr; }
    if (g_fontSmall) { DeleteObject(g_fontSmall); g_fontSmall = nullptr; }
    if (g_fontLarge) { DeleteObject(g_fontLarge); g_fontLarge = nullptr; }
    if (g_fontIcon)  { DeleteObject(g_fontIcon);  g_fontIcon  = nullptr; }
}

static void FillRoundRect(Gdiplus::Graphics& g, const RECT& r, int radius, Gdiplus::Brush& brush) {
    Gdiplus::GraphicsPath path;
    float rx = (float)radius;
    float x = (float)r.left;
    float y = (float)r.top;
    float w = (float)(r.right - r.left);
    float h = (float)(r.bottom - r.top);
    path.AddArc(x, y, rx * 2, rx * 2, 180, 90);
    path.AddArc(x + w - rx * 2, y, rx * 2, rx * 2, 270, 90);
    path.AddArc(x + w - rx * 2, y + h - rx * 2, rx * 2, rx * 2, 0, 90);
    path.AddArc(x, y + h - rx * 2, rx * 2, rx * 2, 90, 90);
    path.CloseFigure();
    g.FillPath(&brush, &path);
}

static void DrawRoundRect(Gdiplus::Graphics& g, const RECT& r, int radius, Gdiplus::Pen& pen) {
    Gdiplus::GraphicsPath path;
    float rx = (float)radius;
    float x = (float)r.left;
    float y = (float)r.top;
    float w = (float)(r.right - r.left);
    float h = (float)(r.bottom - r.top);
    path.AddArc(x, y, rx * 2, rx * 2, 180, 90);
    path.AddArc(x + w - rx * 2, y, rx * 2, rx * 2, 270, 90);
    path.AddArc(x + w - rx * 2, y + h - rx * 2, rx * 2, rx * 2, 0, 90);
    path.AddArc(x, y + h - rx * 2, rx * 2, rx * 2, 90, 90);
    path.CloseFigure();
    g.DrawPath(&pen, &path);
}

// ─── Painting ────────────────────────────────────────────────────────────────
static void Paint(HDC hdc, RECT rc) {
    int W = rc.right;
    int H = rc.bottom;
    if (W <= 0 || H <= 0) return;

    int padX = S(PAD_X);
    int rad  = S(RADIUS);

    Gdiplus::Graphics g(hdc);
    g.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
    g.SetTextRenderingHint(Gdiplus::TextRenderingHintClearTypeGridFit);

    // 1. Background Fill (#0B0616 Deep Obsidian)
    Gdiplus::SolidBrush bgBrush(Gdiplus::Color(255, GetRValue(C_BG), GetGValue(C_BG), GetBValue(C_BG)));
    FillRoundRect(g, rc, rad, bgBrush);

    // 1b. Subtle Obsidian-Purple Border Outline
    Gdiplus::Pen borderPen(Gdiplus::Color(255, 110, 62, 198), 1.5f);
    DrawRoundRect(g, rc, rad, borderPen);

    // 2. Header Section (y = 12..56 logical)
    Gdiplus::SolidBrush shieldIconBg(g_shieldsUp ? Gdiplus::Color(255, 0, 229, 255) : Gdiplus::Color(255, 239, 68, 68));
    g.FillEllipse(&shieldIconBg, padX, S(16), S(28), S(28));

    SelectObject(hdc, g_fontIcon);
    SetBkMode(hdc, TRANSPARENT);
    SetTextColor(hdc, g_shieldsUp ? RGB(7, 9, 19) : RGB(255, 255, 255));
    RECT shieldGlyphR = { padX, S(16), padX + S(28), S(44) };
    DrawTextW(hdc, L"\uE83D", -1, &shieldGlyphR, DT_CENTER | DT_VCENTER | DT_SINGLELINE);

    SelectObject(hdc, g_fontTitle);
    SetTextColor(hdc, C_TEXT);
    RECT titleR = { padX + S(36), S(12), W - S(70), S(32) };
    DrawTextW(hdc, L"Sovereign Shield", -1, &titleR, DT_LEFT | DT_VCENTER | DT_SINGLELINE);

    SelectObject(hdc, g_fontSmall);
    SetTextColor(hdc, C_MUTED);
    std::wstring wHost(g_currentHost.begin(), g_currentHost.end());
    RECT hostR = { padX + S(36), S(32), W - S(70), S(50) };
    DrawTextW(hdc, wHost.c_str(), -1, &hostR, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);

    // 1-Click Master Toggle Switch (x = W - 62, y = 18, w = 46, h = 24 logical)
    RECT toggleR = { W - S(62), S(18), W - S(16), S(42) };
    bool isHoverToggle = (g_hoverId == 10);
    Gdiplus::SolidBrush toggleTrackBrush(g_shieldsUp
        ? (isHoverToggle ? Gdiplus::Color(255, 40, 215, 105) : Gdiplus::Color(255, 34, 197, 94))
        : (isHoverToggle ? Gdiplus::Color(255, 75, 80, 110)  : Gdiplus::Color(255, 55, 60, 85)));
    FillRoundRect(g, toggleR, S(12), toggleTrackBrush);

    int knobX = g_shieldsUp ? (W - S(38)) : (W - S(60));
    Gdiplus::SolidBrush knobBrush(Gdiplus::Color(255, 255, 255, 255));
    g.FillEllipse(&knobBrush, knobX, S(20), S(20), S(20));

    // 3. Hero Status Banner (y = 58..102 logical)
    RECT heroR = { padX, S(58), W - padX, S(102) };
    Gdiplus::SolidBrush heroBrush(g_shieldsUp ? Gdiplus::Color(36, 0, 229, 255) : Gdiplus::Color(40, 239, 68, 68));
    FillRoundRect(g, heroR, S(8), heroBrush);
    Gdiplus::Pen heroPen(g_shieldsUp ? Gdiplus::Color(130, 0, 229, 255) : Gdiplus::Color(130, 239, 68, 68), 1.0f);
    DrawRoundRect(g, heroR, S(8), heroPen);

    SelectObject(hdc, g_fontBold);
    SetTextColor(hdc, g_shieldsUp ? C_CYAN : C_RED);
    RECT heroStatusR = { padX + S(12), S(64), W - padX - S(12), S(82) };
    DrawTextW(hdc, g_shieldsUp ? L"SHIELDS ARE UP" : L"SHIELDS ARE DOWN", -1, &heroStatusR, DT_LEFT | DT_VCENTER | DT_SINGLELINE);

    SelectObject(hdc, g_fontSmall);
    SetTextColor(hdc, C_MUTED);
    RECT heroSubR = { padX + S(12), S(82), W - padX - S(12), S(98) };
    DrawTextW(hdc, g_shieldsUp ? L"Trackers blocked \u00B7 Google pings stripped" : L"Protection paused for this website", -1, &heroSubR, DT_LEFT | DT_VCENTER | DT_SINGLELINE);

    // Query Telemetry Data
    ContentBlocker::Stats cbStats = ContentBlocker::GetStats();
    unsigned long long totalBlocked = cbStats.ads_blocked + cbStats.trackers_blocked;
    std::string dohProvider = DnsResolver::ActiveProvider();
    if (dohProvider.empty()) dohProvider = "Cloudflare DoH (1.1.1.1)";
    DnsResolver::Stats dnsStats = DnsResolver::GetStats();
    size_t peerCount = 4;

    // 4. Telemetry Dashboard (4 Cards, y = 110..370 logical)
    struct MetricCard {
        int id;
        const wchar_t* icon;
        const wchar_t* title;
        std::wstring value;
        std::wstring subtext;
        COLORREF accent;
    };

    wchar_t blockVal[64], blockSub[64], dnsSub[64], hiveVal[64];
    _snwprintf_s(blockVal, _countof(blockVal), _TRUNCATE, L"%llu Blocked", totalBlocked);
    _snwprintf_s(blockSub, _countof(blockSub), _TRUNCATE, L"%.1f MB saved", cbStats.bandwidth_saved_mb);
    _snwprintf_s(dnsSub, _countof(dnsSub), _TRUNCATE, L"%llu encrypted queries", dnsStats.dohQueries);
    _snwprintf_s(hiveVal, _countof(hiveVal), _TRUNCATE, L"%zu Connected Peers", peerCount);

    std::wstring wDoh(dohProvider.begin(), dohProvider.end());

    MetricCard cards[] = {
        { 1, L"\uE83D", L"Trackers & Ads", blockVal, blockSub, C_CYAN },
        { 2, L"\uE81C", L"Fingerprint Guard", L"Active (Randomized)", L"Canvas \u00B7 WebGL \u00B7 AudioContext", C_PURPLE },
        { 3, L"\uE72E", L"Encrypted DoH DNS", wDoh, dnsSub, C_GREEN },
        { 4, L"\uE774", L"P2P AeonHive Mesh", hiveVal, L"Kademlia DHT \u00B7 GossipSub", C_ACCENT }
    };

    int cardY = 110;
    for (const auto& mc : cards) {
        RECT cR = { padX, S(cardY), W - padX, S(cardY + 58) };
        bool isHover = (g_hoverId == mc.id);
        Gdiplus::SolidBrush cBrush(isHover
            ? Gdiplus::Color(255, GetRValue(C_CARD_HOVER), GetGValue(C_CARD_HOVER), GetBValue(C_CARD_HOVER))
            : Gdiplus::Color(255, GetRValue(C_CARD), GetGValue(C_CARD), GetBValue(C_CARD)));
        FillRoundRect(g, cR, S(6), cBrush);
        Gdiplus::Pen cPen(Gdiplus::Color(255, GetRValue(C_BORDER), GetGValue(C_BORDER), GetBValue(C_BORDER)), 1.0f);
        DrawRoundRect(g, cR, S(6), cPen);

        Gdiplus::SolidBrush barBrush(Gdiplus::Color(255, GetRValue(mc.accent), GetGValue(mc.accent), GetBValue(mc.accent)));
        RECT barR = { padX + S(2), S(cardY + 8), padX + S(5), S(cardY + 50) };
        FillRoundRect(g, barR, S(2), barBrush);

        SelectObject(hdc, g_fontIcon);
        SetTextColor(hdc, mc.accent);
        RECT icR = { padX + S(10), S(cardY + 6), padX + S(28), S(cardY + 24) };
        DrawTextW(hdc, mc.icon, -1, &icR, DT_LEFT | DT_VCENTER | DT_SINGLELINE);

        SelectObject(hdc, g_fontSmall);
        SetTextColor(hdc, C_MUTED);
        RECT tR = { padX + S(30), S(cardY + 6), W - padX - S(12), S(cardY + 22) };
        DrawTextW(hdc, mc.title, -1, &tR, DT_LEFT | DT_VCENTER | DT_SINGLELINE);

        SelectObject(hdc, g_fontBold);
        SetTextColor(hdc, C_TEXT);
        RECT vR = { padX + S(12), S(cardY + 22), W - padX - S(12), S(cardY + 40) };
        DrawTextW(hdc, mc.value.c_str(), -1, &vR, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);

        SelectObject(hdc, g_fontSmall);
        SetTextColor(hdc, C_FAINT);
        RECT sR = { padX + S(12), S(cardY + 40), W - padX - S(12), S(cardY + 54) };
        DrawTextW(hdc, mc.subtext.c_str(), -1, &sR, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);

        cardY += 64;
    }

    // 5. Footer Buttons (y = 380..418 logical)
    RECT btnSettingsR = { padX, S(380), (W / 2) - S(4), S(418) };
    bool isHoverSettings = (g_hoverId == 20);
    Gdiplus::SolidBrush setBrush(isHoverSettings ? Gdiplus::Color(255, 43, 47, 86) : Gdiplus::Color(255, 17, 21, 40));
    FillRoundRect(g, btnSettingsR, S(6), setBrush);
    Gdiplus::Pen btnPen(Gdiplus::Color(255, GetRValue(C_BORDER), GetGValue(C_BORDER), GetBValue(C_BORDER)), 1.0f);
    DrawRoundRect(g, btnSettingsR, S(6), btnPen);
    SelectObject(hdc, g_fontSmall);
    SetTextColor(hdc, isHoverSettings ? C_CYAN : C_TEXT);
    DrawTextW(hdc, L"Shield Center", -1, &btnSettingsR, DT_CENTER | DT_VCENTER | DT_SINGLELINE);

    RECT btnClearR = { (W / 2) + S(4), S(380), W - padX, S(418) };
    bool isHoverClear = (g_hoverId == 21);
    Gdiplus::SolidBrush clrBrush(isHoverClear ? Gdiplus::Color(255, 55, 25, 35) : Gdiplus::Color(255, 17, 21, 40));
    FillRoundRect(g, btnClearR, S(6), clrBrush);
    DrawRoundRect(g, btnClearR, S(6), btnPen);
    SetTextColor(hdc, isHoverClear ? C_RED : C_MUTED);
    DrawTextW(hdc, L"Clear Site Data", -1, &btnClearR, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
}

// ─── Window Procedure ────────────────────────────────────────────────────────
static LRESULT CALLBACK WndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
    case WM_CREATE:
        CreateFonts();
        return 0;

    case WM_ERASEBKGND:
        return 1;

    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC hdc = BeginPaint(hWnd, &ps);
        RECT rc; GetClientRect(hWnd, &rc);
        int w = (std::max)(1L, rc.right);
        int h = (std::max)(1L, rc.bottom);

        HDC memDC = CreateCompatibleDC(hdc);
        HBITMAP memBmp = CreateCompatibleBitmap(hdc, w, h);
        HBITMAP oldBmp = (HBITMAP)SelectObject(memDC, memBmp);

        Paint(memDC, rc);

        BitBlt(hdc, 0, 0, w, h, memDC, 0, 0, SRCCOPY);
        SelectObject(memDC, oldBmp);
        DeleteObject(memBmp);
        DeleteDC(memDC);

        EndPaint(hWnd, &ps);
        return 0;
    }

    case WM_MOUSEMOVE: {
        int lx = UnS(GET_X_LPARAM(lParam));
        int ly = UnS(GET_Y_LPARAM(lParam));
        int newHover = -1;

        if (lx >= HUD_W - 62 && lx <= HUD_W - 16 && ly >= 18 && ly <= 42) {
            newHover = 10;
        } else if (ly >= 380 && ly <= 418) {
            if (lx >= PAD_X && lx <= (HUD_W / 2) - 4) newHover = 20;
            else if (lx >= (HUD_W / 2) + 4 && lx <= HUD_W - PAD_X) newHover = 21;
        } else {
            int cy = 110;
            for (int id = 1; id <= 4; id++) {
                if (ly >= cy && ly <= cy + 58 && lx >= PAD_X && lx <= HUD_W - PAD_X) {
                    newHover = id;
                    break;
                }
                cy += 64;
            }
        }

        if (newHover != g_hoverId) {
            g_hoverId = newHover;
            InvalidateRect(hWnd, nullptr, FALSE);
        }
        break;
    }

    case WM_LBUTTONDOWN: {
        int mx = GET_X_LPARAM(lParam);
        int my = GET_Y_LPARAM(lParam);
        RECT rc; GetClientRect(hWnd, &rc);
        POINT pt = { mx, my };
        if (!PtInRect(&rc, pt)) {
            ReleaseCapture();
            Hide();
            return 0;
        }
        return 0;
    }

    case WM_LBUTTONUP: {
        int mx = GET_X_LPARAM(lParam);
        int my = GET_Y_LPARAM(lParam);
        RECT rc; GetClientRect(hWnd, &rc);
        POINT pt = { mx, my };
        if (!PtInRect(&rc, pt)) {
            return 0;
        }

        int lx = UnS(mx);
        int ly = UnS(my);

        // Toggle Switch Click
        if (lx >= HUD_W - 62 && lx <= HUD_W - 16 && ly >= 18 && ly <= 42) {
            g_shieldsUp = !g_shieldsUp;
            SetShieldActiveForUrl(g_currentUrl.c_str(), g_shieldsUp);
            InvalidateRect(hWnd, nullptr, FALSE);
            if (g_toggleCb) g_toggleCb(g_shieldsUp, g_currentHost);
            return 0;
        }

        // Shield Center / Privacy Settings Button Click
        if (ly >= 380 && ly <= 418 && lx >= PAD_X && lx <= (HUD_W / 2) - 4) {
            ReleaseCapture();
            Hide();
            if (g_actionCb) g_actionCb("settings", g_currentHost);
            return 0;
        }

        // Clear Site Data Button Click
        if (ly >= 380 && ly <= 418 && lx >= (HUD_W / 2) + 4 && lx <= HUD_W - PAD_X) {
            ReleaseCapture();
            Hide();
            if (g_actionCb) g_actionCb("clear_data", g_currentHost);
            return 0;
        }
        break;
    }

    case WM_KEYDOWN:
        if (wParam == VK_ESCAPE) {
            ReleaseCapture();
            Hide();
            return 0;
        }
        break;

    case WM_KILLFOCUS:
    case WM_ACTIVATE:
        if (LOWORD(wParam) == WA_INACTIVE && GetCapture() != hWnd) {
            Hide();
        }
        return 0;

    case WM_DESTROY:
        FreeFonts();
        g_hwnd = nullptr;
        g_visible = false;
        return 0;
    }

    return DefWindowProcW(hWnd, msg, wParam, lParam);
}

// ─── Public API ──────────────────────────────────────────────────────────────
bool RegisterClass(HINSTANCE hInst) {
    g_hInst = hInst;
    WNDCLASSEXW wc = {};
    wc.cbSize        = sizeof(wc);
    wc.style         = CS_DROPSHADOW;
    wc.lpfnWndProc   = WndProc;
    wc.hInstance     = hInst;
    wc.hCursor       = LoadCursor(nullptr, IDC_ARROW);
    wc.hbrBackground = nullptr;
    wc.lpszClassName = L"AeonShieldHUD";
    return RegisterClassExW(&wc) != 0 || GetLastError() == ERROR_CLASS_ALREADY_EXISTS;
}

void Show(HWND parent, POINT anchorPt, const char* currentUrl,
          ShieldToggleCallback toggleCb,
          ShieldActionCallback actionCb) {
    if (g_visible) Hide();

    if (!g_hInst) {
        RegisterClass(GetModuleHandle(nullptr));
    }

    int targetDpi = (int)AeonDwm::GetWindowDpiSafe(parent);
    if (targetDpi > 0 && targetDpi != g_dpi) {
        g_dpi = targetDpi;
        FreeFonts();
    }

    g_toggleCb    = toggleCb;
    g_actionCb    = actionCb;
    g_currentUrl  = currentUrl ? currentUrl : "aeon://newtab";
    g_currentHost = ExtractHostFromUrl(g_currentUrl);
    g_shieldsUp   = IsShieldActiveForUrl(g_currentUrl.c_str());
    g_hoverId     = -1;

    int W = S(HUD_W);
    int H = S(HUD_H);

    int x = anchorPt.x - (W / 2);
    int y = anchorPt.y + S(4);

    HMONITOR hMon = MonitorFromPoint(anchorPt, MONITOR_DEFAULTTONEAREST);
    MONITORINFO mi = { sizeof(mi) };
    if (GetMonitorInfoW(hMon, &mi)) {
        if (x < mi.rcWork.left + S(8)) x = mi.rcWork.left + S(8);
        if (x + W > mi.rcWork.right - S(8)) x = mi.rcWork.right - W - S(8);
        if (y + H > mi.rcWork.bottom - S(8)) y = anchorPt.y - H - S(4);
    }

    g_hwnd = CreateWindowExW(
        WS_EX_TOPMOST | WS_EX_TOOLWINDOW,
        L"AeonShieldHUD", L"",
        WS_POPUP | WS_CLIPCHILDREN | WS_CLIPSIBLINGS,
        x, y, W, H,
        parent, nullptr, g_hInst, nullptr);

    if (!g_hwnd) return;

    AeonDwm::ApplyObsidianTransientPopupDwm(g_hwnd, false);

    ShowWindow(g_hwnd, SW_SHOW);
    SetForegroundWindow(g_hwnd);
    SetFocus(g_hwnd);
    SetCapture(g_hwnd);
    g_visible = true;
}

void Hide() {
    if (g_hwnd) {
        ShowWindow(g_hwnd, SW_HIDE);
        DestroyWindow(g_hwnd);
        g_hwnd = nullptr;
        g_visible = false;
    }
}

bool IsVisible() {
    return g_visible && (g_hwnd != nullptr);
}

void OnDpiChanged(int newDpi) {
    if (newDpi <= 0 || newDpi == g_dpi) return;
    g_dpi = newDpi;
    FreeFonts();
    if (g_hwnd && IsWindow(g_hwnd)) {
        CreateFonts();
        SetWindowPos(g_hwnd, nullptr, 0, 0, S(HUD_W), S(HUD_H), SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
        InvalidateRect(g_hwnd, nullptr, FALSE);
    }
}

} // namespace ShieldHUD
