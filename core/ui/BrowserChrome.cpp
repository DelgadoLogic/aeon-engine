// AeonBrowser -- BrowserChrome.cpp
// DelgadoLogic | Lead UI Engineer
//
// PURPOSE: Draws the Aeon browser chrome -- the native Win32 toolbar that IS
// the browser UI. This implements the unified top tab strip (36px) and
// navigation toolbar (38px) forming the 74px top chrome layer, integrated
// with Visual Tab Groups, In-Viewport Split View, Tab Sleep Manager,
// Sovereign Shield HUD, BookmarkToast, and AppMenu.

#include "BrowserChrome.h"
#include "DwmHelper.h"
#include "BookmarkBar.h"
#include "DownloadButton.h"
#include "../../core/download/DownloadManager.h"
#include "TabGroupManager.h"
#include "SplitViewController.h"
#include "ShieldHUD.h"
#include "BookmarkToast.h"
#include "AppMenu.h"
#include "OmniSuggestHUD.h"
#include "../../core/engine/AeonEngine_Interface.h"
#include "../../core/engine/TierDispatcher.h"
#include "../../core/probe/HardwareProbe.h"
#include "../../core/settings/SettingsEngine.h"
#include "../../core/history/HistoryEngine.h"
#include "../../core/memory/TabSleepManager.h"
#include "../../privacy/ContentBlocker.h"
#include "../../updater/AutoUpdater.h"
#include "../../core/network/NetworkSentinel.h"
#include "../../core/network/DnsResolver.h"
#include "../../core/engine/AeonBridge.h"
#include "../agent/AeonAgentPipe.h"
#include "../../ai/aeon_tab_intelligence.h"
#include "../../ai/aeon_journey_analytics.h"
#include "../../core/session/SessionManager.h"
#include <windows.h>
#include <gdiplus.h>
#include <dwmapi.h>
#include <cstring>
#include <cstdio>
#include <vector>
#include <string>
#include <thread>
#include <algorithm>
#include <set>
#include <cmath>

#pragma comment(lib, "dwmapi.lib")
#pragma comment(lib, "gdiplus.lib")

static std::wstring Utf8ToUtf16(const std::string& utf8) {
    if (utf8.empty()) return L"";
    int wLen = MultiByteToWideChar(CP_UTF8, 0, utf8.c_str(), -1, nullptr, 0);
    if (wLen <= 0) return L"";
    std::vector<wchar_t> wBuf(wLen);
    if (MultiByteToWideChar(CP_UTF8, 0, utf8.c_str(), -1, wBuf.data(), wLen) > 0) {
        return std::wstring(wBuf.data());
    }
    return L"";
}

static std::string Utf16ToUtf8(const std::wstring& utf16) {
    if (utf16.empty()) return "";
    int u8Len = WideCharToMultiByte(CP_UTF8, 0, utf16.c_str(), -1, nullptr, 0, nullptr, nullptr);
    if (u8Len <= 0) return "";
    std::vector<char> u8Buf(u8Len);
    if (WideCharToMultiByte(CP_UTF8, 0, utf16.c_str(), -1, u8Buf.data(), u8Len, nullptr, nullptr) > 0) {
        return std::string(u8Buf.data());
    }
    return "";
}

extern AeonTabIntelligence* g_TabIntel;
extern AeonJourneyAnalytics* g_JourneyAI;

#define CLR_BG_PRIMARY   RGB(11,  6,   22)   // #0b0616
#define CLR_BG_CARD      RGB(17,  9,   34)   // #110922
#define CLR_BG_ACTIVE    RGB(24,  13,  48)   // #180d30
#define CLR_TAB_HOVER    RGB(28,  15,  56)   // #1c0f38
#define CLR_ACCENT       RGB(147, 51,  234)  // #9333ea
#define CLR_ACCENT2      RGB(192, 132, 252)  // #c084fc
#define CLR_BRAVE_ORANGE RGB(255, 85,  0)    // #ff5500
#define CLR_TEXT         RGB(248, 250, 252)  // #f8fafc
#define CLR_TEXT_DIM     RGB(148, 163, 184)  // #94a3b8
#define CLR_TEXT_FAINT   RGB(76,  29,  149)  // #4c1d95
#define CLR_CLOSE_HOVER  RGB(59,  7,   100)  // #3b0764
#define CLR_CLOSE_DANGER RGB(232, 17,  35)   // #e81123
#define CLR_GREEN        RGB(34,  197, 94)   // #22c55e
#define CLR_STAR_GOLD    RGB(251, 191, 36)   // #fbbf24
#define CLR_CYAN         RGB(56,  189, 248)  // #38bdf8
#define CLR_AMBER        RGB(245, 158, 11)   // #f59e0b
#define CLR_DIVIDER      RGB(46,  16,  101)  // #2e1065
#define CLR_BASELINE     RGB(46,  16,  101)  // #2e1065

static const int TAB_STRIP_HEIGHT   = 36;
static const int NAV_TOOLBAR_HEIGHT = 38;
static const int CHROME_H           = 74;
static const int CTRL_W             = 46;
static const int CTRL_TOTAL         = CTRL_W * 3;
static const int PINNED_TAB_WIDTH = 36;

struct ChromeTab {
    unsigned int              id;
    std::string               url;
    std::string               title;
    bool                      loading;
    int                       progressPercent;
    float                     animProgress;
    RECT                      tabRect;
    RECT                      closeRect;
    std::vector<std::string>  history;
    int                       historyIndex;
    DWORD                     lastActiveTime;

    uint32_t                  groupId;
    bool                      isPinned;
    bool                      isSleeping;
    RECT                      audioRect;
    bool                      isPlayingAudio;
    bool                      isMuted;
    double                    zoomFactor;
};

struct ChromeState {
    HWND                hwnd;
    HWND                hUrlBar;
    HWND                hContent;

    HFONT               hUrlFont;
    HFONT               hTextFont;
    HFONT               hTextBold;
    HFONT               hTextTabFont;
    HFONT               hIconFont;
    HFONT               hIconFontSmall;
    HFONT               hIconFontLarge;
    HFONT               hLogoFont;

    std::vector<ChromeTab> tabs;
    int                 activeTab;
    int                 hoverTab;
    int                 hoverBtn;
    bool                trackingMouse;

    AeonEngineVTable*   engine;
    AeonSettings        settings;
    const SystemProfile* profile;

    bool                urlFocused;
    bool                isFullscreen;
    bool                settingsOpen;

    BrowserChrome::SplitViewController splitView;

    HBRUSH              hUrlBgBrush;

    double              hoverAlphas[700] = { 0.0 };

    // Drag and drop tab reordering
    int                 dragTabIndex = -1;
    int                 dragStartX = 0;
    int                 dragStartY = 0;
    bool                isDraggingTab = false;

    // Page zoom
    DWORD               zoomBadgeExpireTick = 0;
    int                 currentZoomPct = 100;

    // Find in Page (Ctrl+F)
    bool                findBarVisible = false;
    HWND                hFindBar = nullptr;
    HWND                hFindEdit = nullptr;
    std::string         findQuery = "";
    int                 findMatchCount = 0;

    // Bookmarks Bar (Ctrl+Shift+B)
    bool                bookmarkBarVisible = false;
    HWND                hBookmarkBar = nullptr;

    // Download Button
    HWND                hDownloadBtn = nullptr;

    // Loading & Progress Animation
    float               spinnerAngle = 0.0f;

    // PerMonitorV2 High-DPI & Arc/Obsidian Vertical Sidebar State
    int                 dpi = 96;
    float               dpiScale = 1.0f;
    bool                verticalSidebar = false;
    int                 sidebarWidth = 220;
    RECT                sidebarAddRect = { 0, 0, 0, 0 };
};

static inline int S(const ChromeState* ch, int px) {
    return MulDiv(px, (ch && ch->dpi > 0) ? ch->dpi : 96, 96);
}

static inline int GetEffectiveSidebarWidth(const ChromeState* ch) {
    if (!ch || ch->isFullscreen || !ch->verticalSidebar) return 0;
    return (ch->sidebarWidth > 0) ? ch->sidebarWidth : S(ch, 220);
}

static inline int GetTopChromeHeight(const ChromeState* ch) {
    if (!ch || ch->isFullscreen) return 0;
    return S(ch, CHROME_H) + (ch->bookmarkBarVisible ? S(ch, 32) : 0);
}

static std::string ReverseMapUrl(const std::string& url) {
    if (url.rfind("file:///", 0) != 0) return url;

    struct { const char* suffix; const char* aeonUrl; } mappings[] = {
        { "newtab/newtab.html",        "aeon://newtab" },
        { "pages/intelligence.html",  "aeon://intelligence" },
        { "pages/extensions.html",    "aeon://extensions" },
        { "pages/shields.html",       "aeon://shields" },
        { "pages/settings.html",      "aeon://settings" },
        { "pages/history.html",       "aeon://history" },
        { "pages/bookmarks.html",     "aeon://bookmarks" },
        { "pages/downloads.html",     "aeon://downloads" },
        { "pages/passwords.html",     "aeon://passwords" },
        { "pages/diagnostics.html",   "aeon://diagnostics" },
        { "pages/crash.html",         "aeon://crash" },
        { "pages/drm-info.html",      "aeon://drm-info" },
    };

    for (auto& m : mappings) {
        std::string lower = url;
        for (auto& c : lower) { if (c == '\\') c = '/'; c = (char)tolower(c); }
        size_t qPos = lower.find_first_of("?#");
        std::string pathPart = (qPos != std::string::npos) ? lower.substr(0, qPos) : lower;

        std::string suffLower = m.suffix;
        for (auto& c : suffLower) c = (char)tolower(c);
        std::string decoded = pathPart;
        size_t pos;
        while ((pos = decoded.find("%20")) != std::string::npos)
            decoded.replace(pos, 3, " ");
        if (decoded.size() >= suffLower.size() &&
            decoded.compare(decoded.size() - suffLower.size(),
                           suffLower.size(), suffLower) == 0) {
            return m.aeonUrl;
        }
    }
    return url;
}

static void FillGdiplusRoundRect(Gdiplus::Graphics& graphics, const RECT& r, int radius, Gdiplus::Brush& brush) {
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

    graphics.FillPath(&brush, &path);
}

static void DrawGdiplusRoundRect(Gdiplus::Graphics& graphics, const RECT& r, int radius, Gdiplus::Pen& pen) {
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

    graphics.DrawPath(&pen, &path);
}

static void DrawSplitPaneGlyph(Gdiplus::Graphics& g, float x, float y, bool isLeftPane, bool isActivePane, bool isHovered) {
    float w = 15.0f;
    float h = 11.0f;
    float r = 2.0f;

    Gdiplus::GraphicsPath path;
    path.AddArc(x, y, r * 2, r * 2, 180, 90);
    path.AddArc(x + w - r * 2, y, r * 2, r * 2, 270, 90);
    path.AddArc(x + w - r * 2, y + h - r * 2, r * 2, r * 2, 0, 90);
    path.AddArc(x, y + h - r * 2, r * 2, r * 2, 90, 90);
    path.CloseFigure();

    // Dark translucent glass background
    Gdiplus::SolidBrush bgBrush(Gdiplus::Color(200, 15, 18, 30));
    g.FillPath(&bgBrush, &path);

    // Sub-rect for the pane this tab controls
    float halfW = 5.5f;
    float fillY = y + 1.5f;
    float fillH = h - 3.0f;
    float fillX = isLeftPane ? (x + 1.5f) : (x + w - 1.5f - halfW);

    Gdiplus::GraphicsPath fillPath;
    float fillR = 1.0f;
    fillPath.AddArc(fillX, fillY, fillR * 2, fillR * 2, 180, 90);
    fillPath.AddArc(fillX + halfW - fillR * 2, fillY, fillR * 2, fillR * 2, 270, 90);
    fillPath.AddArc(fillX + halfW - fillR * 2, fillY + fillH - fillR * 2, fillR * 2, fillR * 2, 0, 90);
    fillPath.AddArc(fillX, fillY + fillH - fillR * 2, fillR * 2, fillR * 2, 90, 90);
    fillPath.CloseFigure();

    if (isActivePane) {
        Gdiplus::LinearGradientBrush fillBrush(
            Gdiplus::PointF(fillX, fillY),
            Gdiplus::PointF(fillX + halfW, fillY + fillH),
            Gdiplus::Color(255, 56, 189, 248),
            Gdiplus::Color(255, 14, 165, 233)
        );
        g.FillPath(&fillBrush, &fillPath);
    } else {
        Gdiplus::LinearGradientBrush fillBrush(
            Gdiplus::PointF(fillX, fillY),
            Gdiplus::PointF(fillX + halfW, fillY + fillH),
            Gdiplus::Color(200, 168, 85, 247),
            Gdiplus::Color(180, 126, 34, 206)
        );
        g.FillPath(&fillBrush, &fillPath);
    }

    // Micro center separator line
    float midX = x + w / 2.0f;
    Gdiplus::Pen sepPen(Gdiplus::Color(120, 255, 255, 255), 1.0f);
    g.DrawLine(&sepPen, midX, y + 1.5f, midX, y + h - 1.5f);

    // Outer frame border
    Gdiplus::Color borderCol = isActivePane
        ? Gdiplus::Color(240, 56, 189, 248)
        : (isHovered ? Gdiplus::Color(200, 192, 132, 252) : Gdiplus::Color(90, 148, 163, 184));
    Gdiplus::Pen borderPen(borderCol, 1.0f);
    g.DrawPath(&borderPen, &path);
}

static void FillRectColor(HDC hdc, const RECT& r, COLORREF c) {
    HBRUSH b = CreateSolidBrush(c);
    FillRect(hdc, &r, b);
    DeleteObject(b);
}

static void DrawRoundRect(HDC hdc, const RECT& r, int rx, COLORREF fill, COLORREF stroke) {
    HBRUSH br = CreateSolidBrush(fill);
    HPEN   pn = CreatePen(PS_SOLID, 1, stroke);
    HBRUSH oldBr = (HBRUSH)SelectObject(hdc, br);
    HPEN   oldPn = (HPEN)SelectObject(hdc, pn);
    RoundRect(hdc, r.left, r.top, r.right, r.bottom, rx, rx);
    SelectObject(hdc, oldBr);
    SelectObject(hdc, oldPn);
    DeleteObject(br);
    DeleteObject(pn);
}

static thread_local float g_PaintScale = 1.0f;

static inline RECT ScalePaintRect(const RECT& r) {
    if (g_PaintScale == 1.0f) return r;
    return {
        (int)(r.left   * g_PaintScale + 0.5f),
        (int)(r.top    * g_PaintScale + 0.5f),
        (int)(r.right  * g_PaintScale + 0.5f),
        (int)(r.bottom * g_PaintScale + 0.5f)
    };
}

static void DrawText16(HDC hdc, const char* text, const RECT& r,
                       COLORREF c, HFONT hFont) {
    if (!text) return;
    HFONT old = (HFONT)SelectObject(hdc, hFont);
    SetTextColor(hdc, c);
    SetBkMode(hdc, TRANSPARENT);
    std::wstring wStr = Utf8ToUtf16(text);
    if (!wStr.empty()) {
        RECT dr = ScalePaintRect(r);
        DrawTextW(hdc, wStr.c_str(), -1, &dr,
            DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
    }
    SelectObject(hdc, old);
}

static void DrawIcon(HDC hdc, const wchar_t* glyph, const RECT& r,
                     COLORREF c, HFONT hFont) {
    HFONT old = (HFONT)SelectObject(hdc, hFont);
    SetTextColor(hdc, c);
    SetBkMode(hdc, TRANSPARENT);
    RECT dr = ScalePaintRect(r);
    DrawTextW(hdc, glyph, -1, &dr,
        DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOCLIP);
    SelectObject(hdc, old);
}

#define ICON_BACK           L"\uE72B"
#define ICON_FORWARD        L"\uE72A"
#define ICON_REFRESH        L"\uE72C"
#define ICON_STOP           L"\uE711"
#define ICON_HOME           L"\uE80F"
#define ICON_DOWNLOAD       L"\uE896"
#define ICON_BOOKMARK       L"\uE734"
#define ICON_BOOKMARK_FILL  L"\uE735"
#define ICON_SHIELD         L"\uE83D"
#define ICON_AI_SUMMARY     L"\uF602"
#define ICON_SPLITVIEW      L"\uE740"
#define ICON_MORE           L"\uE712"
#define ICON_MINIMIZE       L"\uE921"
#define ICON_MAXIMIZE       L"\uE922"
#define ICON_RESTORE        L"\uE923"
#define ICON_CLOSE          L"\uE8BB"
#define ICON_ADD            L"\uE710"
#define ICON_LOCK           L"\uE72E"
#define ICON_CHEVRON_DOWN   L"\uE70D"
#define ICON_CHEVRON_RIGHT  L"\uE76C"
#define ICON_MOON_SLEEP     L"\uE708"

static void SyncViewportInternal(ChromeState* ch) {
    if (!ch || !ch->engine || !ch->hwnd) return;
    RECT rc;
    GetClientRect(ch->hwnd, &rc);
    int chromeH = S(ch, CHROME_H);
    int topH = GetTopChromeHeight(ch);
    int sidebarW = GetEffectiveSidebarWidth(ch);
    int contentH = max(0, rc.bottom - topH);
    int contentW = max(0, rc.right - sidebarW);

    if (ch->hBookmarkBar && ch->bookmarkBarVisible) {
        BookmarkBar::Reposition(0, chromeH, rc.right);
    }
    if (ch->hDownloadBtn) {
        DownloadButton::Move(rc.right - S(ch, 136), S(ch, TAB_STRIP_HEIGHT + 5), S(ch, 28));
    }
    if (ch->hFindBar && ch->findBarVisible) {
        int fbW = S(ch, 340), fbH = S(ch, 34);
        int fbX = max(sidebarW + S(ch, 10), rc.right - fbW - S(ch, 20));
        int fbY = (ch->isFullscreen ? 0 : topH) + S(ch, 6);
        SetWindowPos(ch->hFindBar, HWND_TOP, fbX, fbY, fbW, fbH, SWP_SHOWWINDOW);
    }

    if (ch->splitView.IsEnabled()) {
        int priIdx = ch->splitView.GetPrimaryTabIndex();
        int secIdx = ch->splitView.GetSecondaryTabIndex();

        if (priIdx >= 0 && priIdx < (int)ch->tabs.size() &&
            secIdx >= 0 && secIdx < (int)ch->tabs.size()) {

            RECT primaryRect, secondaryRect, splitterRect;
            ch->splitView.CalculateLayout(rc.right, rc.bottom, topH,
                                          primaryRect, secondaryRect, splitterRect,
                                          sidebarW, ch->dpiScale);

            unsigned int priId = ch->tabs[priIdx].id;
            unsigned int secId = ch->tabs[secIdx].id;

            int priW = max(0, primaryRect.right - primaryRect.left);
            int priH = max(0, primaryRect.bottom - primaryRect.top);
            int secW = max(0, secondaryRect.right - secondaryRect.left);
            int secH = max(0, secondaryRect.bottom - secondaryRect.top);

            ch->engine->SetViewport(priId, ch->hwnd, primaryRect.left, primaryRect.top, priW, priH);
            ch->engine->SetViewport(secId, ch->hwnd, secondaryRect.left, secondaryRect.top, secW, secH);

            // Hide all tabs not currently visible in the split view
            for (int i = 0; i < (int)ch->tabs.size(); ++i) {
                if (i != priIdx && i != secIdx) {
                    ch->engine->SetViewport(ch->tabs[i].id, ch->hwnd, 0, 0, 0, 0);
                }
            }
            return;
        }
    }

    if (ch->activeTab >= 0 && ch->activeTab < (int)ch->tabs.size()) {
        unsigned int tabId = ch->tabs[ch->activeTab].id;
        ch->engine->SetViewport(tabId, ch->hwnd, sidebarW, topH, contentW, contentH);

        // Hide all inactive background tabs
        for (int i = 0; i < (int)ch->tabs.size(); ++i) {
            if (i != ch->activeTab) {
                ch->engine->SetViewport(ch->tabs[i].id, ch->hwnd, 0, 0, 0, 0);
            }
        }
    }
}

static void PaintTabStrip(ChromeState* ch, HDC hdc, int width) {
    Gdiplus::Graphics graphics(hdc);
    graphics.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
    graphics.SetTextRenderingHint(Gdiplus::TextRenderingHintClearTypeGridFit);
    if (g_PaintScale != 1.0f) {
        graphics.ScaleTransform(g_PaintScale, g_PaintScale);
    }

    RECT stripRect = { 0, 0, width, TAB_STRIP_HEIGHT };
    Gdiplus::SolidBrush stripBg(Gdiplus::Color(160, 11, 6, 22));
    graphics.FillRectangle(&stripBg, 0, 0, width, TAB_STRIP_HEIGHT);

    Gdiplus::Pen basePen(Gdiplus::Color(255, 46, 16, 101), 1.0f);
    graphics.DrawLine(&basePen, 0.0f, (float)(TAB_STRIP_HEIGHT - 1), (float)width, (float)(TAB_STRIP_HEIGHT - 1));

    Gdiplus::GraphicsPath shieldPath;
    shieldPath.AddLine(21.0f, 6.0f, 11.0f, 9.0f);
    shieldPath.AddBezier(11.0f, 9.0f, 10.0f, 14.0f, 14.0f, 24.0f, 21.0f, 29.0f);
    shieldPath.AddBezier(21.0f, 29.0f, 28.0f, 24.0f, 32.0f, 14.0f, 31.0f, 9.0f);
    shieldPath.CloseFigure();

    Gdiplus::LinearGradientBrush shieldBg(
        Gdiplus::PointF(10.0f, 6.0f), Gdiplus::PointF(32.0f, 29.0f),
        Gdiplus::Color(60, 236, 72, 153), Gdiplus::Color(50, 56, 189, 248)
    );
    graphics.FillPath(&shieldBg, &shieldPath);

    Gdiplus::LinearGradientBrush shieldBorder(
        Gdiplus::PointF(10.0f, 6.0f), Gdiplus::PointF(32.0f, 29.0f),
        Gdiplus::Color(255, 236, 72, 153), Gdiplus::Color(255, 56, 189, 248)
    );
    Gdiplus::Pen shieldPen(&shieldBorder, 1.6f);
    shieldPen.SetLineJoin(Gdiplus::LineJoinRound);
    graphics.DrawPath(&shieldPen, &shieldPath);

    Gdiplus::Pen ribbonPen(Gdiplus::Color(255, 255, 255, 255), 1.5f);
    ribbonPen.SetLineCap(Gdiplus::LineCapRound, Gdiplus::LineCapRound, Gdiplus::DashCapRound);
    graphics.DrawLine(&ribbonPen, 15.0f, 23.0f, 21.0f, 11.0f);
    graphics.DrawLine(&ribbonPen, 21.0f, 11.0f, 27.0f, 23.0f);

    Gdiplus::Pen crossbarPen(Gdiplus::Color(230, 192, 132, 252), 1.3f);
    crossbarPen.SetLineCap(Gdiplus::LineCapRound, Gdiplus::LineCapRound, Gdiplus::DashCapRound);
    graphics.DrawBezier(&crossbarPen, 14.0f, 19.0f, 18.0f, 17.5f, 23.0f, 20.5f, 28.0f, 18.0f);

    const int TAB_START_X = 40;
    const int CTRL_TOTAL_W = 138;
    const int ADD_BTN_W = 28;
    const int TAB_MARGIN = 2;
    const int MAX_TAB_W = 220;
    const int MIN_TAB_W = 75;

    if (!ch->verticalSidebar) {
        int rightLimit = max(TAB_START_X + MIN_TAB_W, width - CTRL_TOTAL_W - ADD_BTN_W - 8);

        auto& groupMgr = BrowserChrome::TabGroupManager::GetInstance();
        std::set<uint32_t> presentGroupIds;
        for (auto& t : ch->tabs) {
            uint32_t gid = groupMgr.GetTabGroupId(t.id);
            if (gid != t.groupId) {
                t.groupId = gid;
            }
            if (t.groupId > 0) presentGroupIds.insert(t.groupId);
        }

        int totalGroupHeadersW = 0;
        for (uint32_t gid : presentGroupIds) {
            const auto* grp = groupMgr.GetGroup(gid);
            if (grp) {
                int grpW = 90 + static_cast<int>(grp->name.length() * 5);
                totalGroupHeadersW += grpW + TAB_MARGIN;
            }
        }

        int pinnedCount = 0;
        for (const auto& t : ch->tabs) {
            if (t.isPinned) pinnedCount++;
        }
        int totalPinnedW = pinnedCount * (PINNED_TAB_WIDTH + TAB_MARGIN);

        int unpinnedVisibleCount = 0;
        for (const auto& t : ch->tabs) {
            if (!t.isPinned && (t.groupId == 0 || !groupMgr.IsGroupCollapsed(t.groupId))) {
                unpinnedVisibleCount++;
            }
        }
        unpinnedVisibleCount = max(1, unpinnedVisibleCount);

        int availW = max(100, rightLimit - TAB_START_X - totalGroupHeadersW - totalPinnedW - 8);
        int tabW = min(MAX_TAB_W, max(MIN_TAB_W, (availW - (unpinnedVisibleCount - 1) * TAB_MARGIN) / unpinnedVisibleCount));

        int curX = TAB_START_X;
        std::set<uint32_t> renderedGroupHeaders;

        for (int i = 0; i < (int)ch->tabs.size(); i++) {
            ChromeTab& t = ch->tabs[i];

            if (t.groupId > 0 && renderedGroupHeaders.find(t.groupId) == renderedGroupHeaders.end()) {
                renderedGroupHeaders.insert(t.groupId);
                BrowserChrome::TabGroup* grpMut = groupMgr.GetGroupMut(t.groupId);
                if (grpMut) {
                    size_t grpTabCount = groupMgr.GetTabCountInGroup(t.groupId);
                    int grpW = 90 + static_cast<int>(grpMut->name.length() * 5);
                    RECT gRect = { curX, 5, curX + grpW, 32 };
                    grpMut->headerRect = gRect;
                    RECT chevR = { gRect.right - 18, gRect.top, gRect.right - 2, gRect.bottom };
                    grpMut->chevronRect = chevR;

                    bool grpHover = (ch->hoverBtn == 300 + (int)t.groupId || ch->hoverBtn == 400 + (int)t.groupId);

                    BYTE r = GetRValue(grpMut->color);
                    BYTE g = GetGValue(grpMut->color);
                    BYTE b = GetBValue(grpMut->color);
                    Gdiplus::SolidBrush chipBg(Gdiplus::Color(grpHover ? 78 : 42, r, g, b));
                    FillGdiplusRoundRect(graphics, gRect, 6, chipBg);

                    Gdiplus::Pen chipPen(Gdiplus::Color(grpHover ? 245 : 185, r, g, b), 1.2f);
                    DrawGdiplusRoundRect(graphics, gRect, 6, chipPen);

                    Gdiplus::SolidBrush dotBr(Gdiplus::Color(255, r, g, b));
                    graphics.FillEllipse(&dotBr, gRect.left + 7, gRect.top + 9, 8, 8);

                    // Group tab count pill badge
                    char countBuf[16];
                    snprintf(countBuf, sizeof(countBuf), "%zu", grpTabCount);
                    RECT badgeR = { gRect.right - 38, gRect.top + 5, gRect.right - 19, gRect.bottom - 5 };
                    Gdiplus::SolidBrush badgeBg(Gdiplus::Color(grpHover ? 130 : 85, r, g, b));
                    FillGdiplusRoundRect(graphics, badgeR, 4, badgeBg);
                    HFONT oldFont = (HFONT)SelectObject(hdc, ch->hTextTabFont);
                    SetTextColor(hdc, RGB(248, 250, 252));
                    SetBkMode(hdc, TRANSPARENT);
                    RECT scaledBadgeR = ScalePaintRect(badgeR);
                    DrawTextA(hdc, countBuf, -1, &scaledBadgeR, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
                    SelectObject(hdc, oldFont);

                    RECT nameR = { gRect.left + 19, gRect.top, badgeR.left - 3, gRect.bottom };
                    DrawText16(hdc, grpMut->name.c_str(), nameR, grpHover ? RGB(255, 255, 255) : CLR_TEXT, ch->hTextBold);

                    const wchar_t* chevGlyph = grpMut->collapsed ? ICON_CHEVRON_RIGHT : ICON_CHEVRON_DOWN;
                    DrawIcon(hdc, chevGlyph, chevR, grpHover ? RGB(248, 250, 252) : CLR_TEXT_DIM, ch->hIconFontSmall);

                    curX += grpW + TAB_MARGIN;
                }
            }

            if (t.groupId > 0 && groupMgr.IsGroupCollapsed(t.groupId)) {
                t.tabRect = { 0, 0, 0, 0 };
                t.closeRect = { 0, 0, 0, 0 };
                continue;
            }

            bool active = (i == ch->activeTab);
            bool hover  = (i == ch->hoverTab);

            bool isSplitPrimary = ch->splitView.IsEnabled() && (i == ch->splitView.GetPrimaryTabIndex());
            bool isSplitSecondary = ch->splitView.IsEnabled() && (i == ch->splitView.GetSecondaryTabIndex());
            bool isSplitMember = isSplitPrimary || isSplitSecondary;

            if (t.isPinned) {
                RECT tR = { curX, (active || isSplitMember) ? 4 : 5, curX + PINNED_TAB_WIDTH, (active || isSplitMember) ? (TAB_STRIP_HEIGHT - 3) : (TAB_STRIP_HEIGHT - 4) };
                t.tabRect = tR;
                t.closeRect = { 0, 0, 0, 0 };
                t.audioRect = { 0, 0, 0, 0 };

                if (active || isSplitMember) {
                    Gdiplus::LinearGradientBrush activeBrush(
                        Gdiplus::PointF((float)tR.left, (float)tR.top),
                        Gdiplus::PointF((float)tR.left, (float)tR.bottom),
                        Gdiplus::Color(250, 22, 26, 52),
                        Gdiplus::Color(255, 14, 17, 36)
                    );
                    FillGdiplusRoundRect(graphics, tR, 6, activeBrush);
                    Gdiplus::Pen activePen(Gdiplus::Color(195, 0, 229, 255), 1.2f);
                    DrawGdiplusRoundRect(graphics, tR, 6, activePen);
                } else {
                    Gdiplus::SolidBrush inactBrush(Gdiplus::Color(hover ? 125 : 68, 17, 21, 40));
                    FillGdiplusRoundRect(graphics, tR, 6, inactBrush);
                    Gdiplus::Pen inactBorder(Gdiplus::Color(hover ? 120 : 48, 148, 163, 184), 1.0f);
                    DrawGdiplusRoundRect(graphics, tR, 6, inactBorder);
                }

                int cx = tR.left + (PINNED_TAB_WIDTH - 16) / 2;
                int cy = tR.top + (tR.bottom - tR.top - 16) / 2;
                RECT pinR = { cx, cy, cx + 16, cy + 16 };
                DrawIcon(hdc, L"\uE718", pinR, active ? RGB(0, 229, 255) : CLR_TEXT_DIM, ch->hIconFontSmall);

                if (t.isPlayingAudio) {
                    Gdiplus::SolidBrush dotBrush(Gdiplus::Color(255, 0, 229, 255));
                    graphics.FillEllipse(&dotBrush, tR.right - 8, tR.top + 5, 5, 5);
                }

                curX += PINNED_TAB_WIDTH + TAB_MARGIN;
                continue;
            }

            if (active || isSplitMember) {
                RECT tR = { curX, 4, curX + tabW, TAB_STRIP_HEIGHT - 3 };
                t.tabRect = tR;

                Gdiplus::LinearGradientBrush activeBrush(
                    Gdiplus::PointF((float)tR.left, (float)tR.top),
                    Gdiplus::PointF((float)tR.left, (float)tR.bottom),
                    Gdiplus::Color(250, 22, 26, 52),
                    Gdiplus::Color(255, 14, 17, 36)
                );
                FillGdiplusRoundRect(graphics, tR, 6, activeBrush);

                COLORREF topAcc = CLR_ACCENT2;
                if (isSplitMember) {
                    bool isLeft = isSplitPrimary;
                    bool isPaneActive = (isLeft && ch->splitView.GetActivePane() == 0) ||
                                       (!isLeft && ch->splitView.GetActivePane() == 1);
                    topAcc = isPaneActive ? RGB(0, 229, 255) : RGB(147, 51, 234);
                } else if (t.groupId > 0) {
                    const auto* g = groupMgr.GetGroup(t.groupId);
                    if (g) topAcc = g->color;
                }

                Gdiplus::Pen activeBorder(Gdiplus::Color(190, GetRValue(topAcc), GetGValue(topAcc), GetBValue(topAcc)), 1.2f);
                DrawGdiplusRoundRect(graphics, tR, 6, activeBorder);

                // Crisp Obsidian neon cyan-to-purple pill indicator along bottom edge of active tab
                Gdiplus::LinearGradientBrush pillIndBrush(
                    Gdiplus::PointF((float)(tR.left + 10), (float)(tR.bottom - 2)),
                    Gdiplus::PointF((float)(tR.right - 10), (float)(tR.bottom - 2)),
                    Gdiplus::Color(255, 0, 229, 255),
                    Gdiplus::Color(255, 168, 85, 247)
                );
                Gdiplus::Pen pillIndPen(&pillIndBrush, 2.0f);
                pillIndPen.SetStartCap(Gdiplus::LineCapRound);
                pillIndPen.SetEndCap(Gdiplus::LineCapRound);
                graphics.DrawLine(&pillIndPen, (float)(tR.left + 10), (float)(tR.bottom - 1.5f), (float)(tR.right - 10), (float)(tR.bottom - 1.5f));

                Gdiplus::Pen specPen(Gdiplus::Color(70, 255, 255, 255), 1.0f);
                graphics.DrawLine(&specPen, (float)tR.left + 7, (float)tR.top + 1, (float)tR.right - 7, (float)tR.top + 1);
            } else {
                RECT tR = { curX, 5, curX + tabW, TAB_STRIP_HEIGHT - 4 };
                t.tabRect = tR;

                Gdiplus::SolidBrush inactBrush(Gdiplus::Color(hover ? 125 : 65, 17, 21, 40));
                FillGdiplusRoundRect(graphics, tR, 6, inactBrush);

                Gdiplus::Pen inactBorder(Gdiplus::Color(hover ? 115 : 42, 148, 163, 184), 1.0f);
                DrawGdiplusRoundRect(graphics, tR, 6, inactBorder);

                if (t.groupId > 0) {
                    const auto* g = groupMgr.GetGroup(t.groupId);
                    if (g) {
                        Gdiplus::Pen grpLinePen(Gdiplus::Color(175, GetRValue(g->color), GetGValue(g->color), GetBValue(g->color)), 1.5f);
                        graphics.DrawLine(&grpLinePen, (float)tR.left + 6, (float)tR.bottom - 2, (float)tR.right - 6, (float)tR.bottom - 2);
                    }
                }

                if (i < (int)ch->tabs.size() - 1 && (i + 1) != ch->activeTab && !isSplitMember) {
                    Gdiplus::Pen sepPen(Gdiplus::Color(60, 148, 163, 184), 1.0f);
                    float divX = (float)(curX + tabW + TAB_MARGIN / 2);
                    graphics.DrawLine(&sepPen, divX, 10.0f, divX, 26.0f);
                }
            }

            int iconY = t.tabRect.top + (t.tabRect.bottom - t.tabRect.top - 16) / 2;
            bool tabIsSleeping = t.isSleeping || TabSleepManager::IsTabSleeping(t.id);

            int titleLeft = t.tabRect.left + 26;

            if (tabIsSleeping) {
                RECT sleepR = { t.tabRect.left + 8, iconY, t.tabRect.left + 22, iconY + 16 };
                DrawIcon(hdc, ICON_MOON_SLEEP, sleepR, RGB(192, 132, 252), ch->hIconFontSmall);
            } else if (t.loading) {
                float cx = (float)(t.tabRect.left + 15);
                float cy = (float)(iconY + 8);
                float radius = 5.5f;

                Gdiplus::Pen trackPen(Gdiplus::Color(40, 147, 51, 234), 1.5f);
                graphics.DrawEllipse(&trackPen, cx - radius, cy - radius, radius * 2.0f, radius * 2.0f);

                Gdiplus::Pen spinPen(Gdiplus::Color(255, 0, 229, 255), 1.8f);
                spinPen.SetStartCap(Gdiplus::LineCapRound);
                spinPen.SetEndCap(Gdiplus::LineCapRound);
                graphics.DrawArc(&spinPen, cx - radius, cy - radius, radius * 2.0f, radius * 2.0f, ch->spinnerAngle, 260.0f);
            } else if (isSplitMember) {
                bool isLeft = isSplitPrimary;
                bool isPaneActive = (isLeft && ch->splitView.GetActivePane() == 0) ||
                                   (!isLeft && ch->splitView.GetActivePane() == 1);
                int glyphY = t.tabRect.top + (t.tabRect.bottom - t.tabRect.top - 11) / 2;
                DrawSplitPaneGlyph(graphics, (float)(t.tabRect.left + 9), (float)glyphY, isLeft, isPaneActive, ch->hoverBtn == 100 + i);
                titleLeft = t.tabRect.left + 29;
            } else {
                Gdiplus::SolidBrush favBrush(Gdiplus::Color(255, active ? 0 : 130, active ? 229 : 110, active ? 255 : 190));
                graphics.FillEllipse(&favBrush, t.tabRect.left + 10, iconY + 3, 10, 10);
            }

            RECT titleR = { titleLeft, t.tabRect.top, t.tabRect.right - 26, t.tabRect.bottom };
            const char* titleTxt = t.loading ? "Loading..." : (t.title.empty() ? "New Tab" : t.title.c_str());
            COLORREF titleClr = tabIsSleeping ? CLR_TEXT_DIM : (active ? RGB(248, 250, 252) : (hover ? RGB(241, 245, 249) : RGB(203, 213, 225)));
            DrawText16(hdc, titleTxt, titleR, titleClr, ch->hTextTabFont);

            int closeY = t.tabRect.top + (t.tabRect.bottom - t.tabRect.top - 18) / 2;
            RECT closeR = { t.tabRect.right - 24, closeY, t.tabRect.right - 6, closeY + 18 };
            t.closeRect = closeR;

            bool closeHover = (ch->hoverBtn == 200 + i);
            if (closeHover) {
                Gdiplus::SolidBrush closeBg(Gdiplus::Color(255, 62, 67, 104));
                graphics.FillEllipse(&closeBg, closeR.left, closeR.top, 18, 18);
            }
            COLORREF closeClr = closeHover ? RGB(255, 255, 255) : (active ? CLR_TEXT_DIM : CLR_TEXT_FAINT);
            DrawIcon(hdc, ICON_CLOSE, closeR, closeClr, ch->hIconFontSmall);

            if (t.isPlayingAudio) {
                RECT audioR = { closeR.left - 20, closeY, closeR.left - 2, closeY + 18 };
                t.audioRect = audioR;
                bool audioHover = (ch->hoverBtn == 600 + i);
                if (audioHover) {
                    Gdiplus::SolidBrush aBg(Gdiplus::Color(255, 62, 67, 104));
                    graphics.FillEllipse(&aBg, audioR.left, audioR.top, 18, 18);
                }
                COLORREF aClr = t.isMuted ? RGB(239, 68, 68) : (active ? RGB(0, 229, 255) : RGB(148, 163, 184));
                DrawIcon(hdc, t.isMuted ? L"\uE74F" : L"\uE767", audioR, aClr, ch->hIconFontSmall);
            } else {
                t.audioRect = { 0, 0, 0, 0 };
            }

            curX += tabW + TAB_MARGIN;
        }

        int addX = min(curX + 6, rightLimit);
        RECT addR = { addX, 6, addX + ADD_BTN_W, 29 };
        bool addHover = (ch->hoverBtn == 50);

        Gdiplus::SolidBrush addBg(Gdiplus::Color(addHover ? 110 : 50, 255, 255, 255));
        FillGdiplusRoundRect(graphics, addR, 6, addBg);
        Gdiplus::Pen addBorder(Gdiplus::Color(addHover ? 140 : 48, 0, 229, 255), 1.0f);
        DrawGdiplusRoundRect(graphics, addR, 6, addBorder);

        COLORREF addClr = addHover ? RGB(255, 255, 255) : RGB(203, 213, 225);
        DrawIcon(hdc, ICON_ADD, addR, addClr, ch->hIconFont);
    } else {
        // Arc / Obsidian Vertical Sidebar active: top strip shows workspace title badge + drag bar
        RECT wsPill = { 42, 6, min(width - 150, 260), 30 };
        if (wsPill.right > wsPill.left + 40) {
            Gdiplus::SolidBrush wsBg(Gdiplus::Color(110, 17, 21, 40));
            FillGdiplusRoundRect(graphics, wsPill, 6, wsBg);
            Gdiplus::Pen wsPen(Gdiplus::Color(90, 0, 229, 255), 1.0f);
            DrawGdiplusRoundRect(graphics, wsPill, 6, wsPen);
            RECT wsTextR = { wsPill.left + 10, wsPill.top, wsPill.right - 10, wsPill.bottom };
            DrawText16(hdc, "Aeon Sovereign  \xE2\x80\xA2  Vertical Tabs", wsTextR, CLR_TEXT_DIM, ch->hTextTabFont);
        }
    }

    struct CaptionBtn { int x; const wchar_t* glyph; int hitId; bool isClose; };
    CaptionBtn capBtns[] = {
        { width - 138, ICON_MINIMIZE, 10, false },
        { width - 92,  IsZoomed(ch->hwnd) ? ICON_RESTORE : ICON_MAXIMIZE, 11, false },
        { width - 46,  ICON_CLOSE,    12, true }
    };

    for (const auto& cb : capBtns) {
        RECT btnR = { cb.x, 0, cb.x + 46, TAB_STRIP_HEIGHT };
        bool btnHover = (ch->hoverBtn == cb.hitId);
        if (btnHover) {
            if (cb.isClose) {
                Gdiplus::SolidBrush dangerBrush(Gdiplus::Color(255, 232, 17, 35));
                graphics.FillRectangle(&dangerBrush, btnR.left, btnR.top, 46, TAB_STRIP_HEIGHT);
            } else {
                Gdiplus::SolidBrush capBrush(Gdiplus::Color(255, 29, 33, 59));
                graphics.FillRectangle(&capBrush, btnR.left, btnR.top, 46, TAB_STRIP_HEIGHT);
            }
        }
        COLORREF iconC = (btnHover && cb.isClose) ? RGB(255, 255, 255) : CLR_TEXT_DIM;
        DrawIcon(hdc, cb.glyph, btnR, iconC, ch->hIconFontSmall);
    }
}

static void PaintNavBar(ChromeState* ch, HDC hdc, int width) {
    Gdiplus::Graphics graphics(hdc);
    graphics.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
    graphics.SetTextRenderingHint(Gdiplus::TextRenderingHintClearTypeGridFit);
    if (g_PaintScale != 1.0f) {
        graphics.ScaleTransform(g_PaintScale, g_PaintScale);
    }

    RECT navRect = { 0, TAB_STRIP_HEIGHT, width, CHROME_H };
    Gdiplus::SolidBrush navBg(Gdiplus::Color(185, 14, 16, 28));
    graphics.FillRectangle(&navBg, 0, TAB_STRIP_HEIGHT, width, CHROME_H - TAB_STRIP_HEIGHT);

    Gdiplus::Pen botPen(Gdiplus::Color(255, 11, 13, 23), 1.0f);
    graphics.DrawLine(&botPen, 0.0f, (float)(CHROME_H - 1), (float)width, (float)(CHROME_H - 1));

    bool canGoBack = false;
    bool canGoForward = false;
    bool isLoading = false;
    std::string currentUrl = "aeon://newtab";
    std::string currentTitle = "New Tab";

    int effectiveActiveTab = ch->activeTab;
    if (ch->splitView.IsEnabled()) {
        effectiveActiveTab = ch->splitView.GetActiveTabIndex();
    }

    if (effectiveActiveTab >= 0 && effectiveActiveTab < (int)ch->tabs.size()) {
        const auto& t = ch->tabs[effectiveActiveTab];
        canGoBack = (t.historyIndex > 0);
        canGoForward = (t.historyIndex < (int)t.history.size() - 1);
        isLoading = t.loading;
        currentUrl = t.url;
        currentTitle = t.title;
    }

    std::string displayUrl = ReverseMapUrl(currentUrl);
    bool isNewTab = (currentUrl == "aeon://newtab" || displayUrl == "aeon://newtab" || currentUrl == "about:blank" || currentUrl.empty());

    float curAnimPct = 0.0f;
    if (effectiveActiveTab >= 0 && effectiveActiveTab < (int)ch->tabs.size()) {
        curAnimPct = ch->tabs[effectiveActiveTab].animProgress;
    }

    bool showLoadingBar = (isLoading && curAnimPct > 0.0f) ||
                          (!isLoading && !isNewTab && curAnimPct > 0.0f && curAnimPct <= 100.0f &&
                           effectiveActiveTab >= 0 && effectiveActiveTab < (int)ch->tabs.size() &&
                           ch->tabs[effectiveActiveTab].progressPercent >= 100);

    struct NavBtnDef {
        int x;
        const wchar_t* glyph;
        int hitId;
        bool enabled;
    };

    NavBtnDef navBtns[] = {
        { 8,   ICON_BACK,    1, canGoBack },
        { 38,  ICON_FORWARD, 2, canGoForward },
        { 68,  isLoading ? ICON_STOP : ICON_REFRESH, 3, true }
    };

    for (const auto& nb : navBtns) {
        RECT btnR = { nb.x, 40, nb.x + 26, 68 };
        bool btnHover = (ch->hoverBtn == nb.hitId) && nb.enabled;
        if (btnHover) {
            Gdiplus::SolidBrush hBrush(Gdiplus::Color(255, 29, 33, 59));
            FillGdiplusRoundRect(graphics, btnR, 4, hBrush);
        }
        COLORREF iconC = !nb.enabled ? CLR_TEXT_FAINT : (btnHover ? RGB(255, 255, 255) : CLR_TEXT_DIM);
        DrawIcon(hdc, nb.glyph, btnR, iconC, ch->hIconFontSmall);
    }

    struct ActionBtnDef { int x; const wchar_t* glyph; int hitId; COLORREF customColor; bool activeState; };
    ActionBtnDef actBtns[] = {
        { width - 136, ICON_DOWNLOAD,   20, 0,          false },
        { width - 104, ICON_SPLITVIEW,  24, CLR_CYAN,   ch->splitView.IsEnabled() },
        { width - 72,  ICON_AI_SUMMARY, 23, CLR_ACCENT, false },
        { width - 40,  ICON_MORE,       25, 0,          false }
    };

    for (const auto& ab : actBtns) {
        RECT btnR = { ab.x, 40, ab.x + 28, 68 };
        bool btnHover = (ch->hoverBtn == ab.hitId);
        if (btnHover || ab.activeState) {
            Gdiplus::SolidBrush hBrush(Gdiplus::Color(ab.activeState ? 60 : 40, 56, 189, 248));
            FillGdiplusRoundRect(graphics, btnR, 4, hBrush);
        }
        COLORREF iconC = btnHover ? RGB(255, 255, 255) : (ab.activeState ? CLR_CYAN : (ab.customColor ? ab.customColor : CLR_TEXT_DIM));
        DrawIcon(hdc, ab.glyph, btnR, iconC, ch->hIconFontSmall);
    }

    int urlLeft = 102;
    int urlRight = width - 144;
    if (urlRight > urlLeft + 80) {
        RECT urlCard = { urlLeft, 41, urlRight, 69 };
        Gdiplus::SolidBrush omniBg(Gdiplus::Color(ch->urlFocused ? 230 : 180, 18, 16, 36));
        FillGdiplusRoundRect(graphics, urlCard, 6, omniBg);

        // Ambient breathing neon backlight inside Omnibox while loading
        if (showLoadingBar) {
            DWORD tick = GetTickCount();
            float pulse = 0.5f + 0.5f * sinf((float)tick * 0.006f);
            BYTE glowA = (BYTE)(14 + pulse * 16);
            Gdiplus::SolidBrush glowBrush(Gdiplus::Color(glowA, 147, 51, 234));
            FillGdiplusRoundRect(graphics, urlCard, 6, glowBrush);
        }

        Gdiplus::Pen omniBorder(ch->urlFocused ? Gdiplus::Color(255, 56, 189, 248) : Gdiplus::Color(45, 255, 255, 255), 1.0f);
        DrawGdiplusRoundRect(graphics, urlCard, 6, omniBorder);

        // Omnibox bottom border progress bar (Safari / Arc style)
        if (showLoadingBar) {
            int cardW = urlRight - urlLeft;
            int trackW = cardW - 14;
            float clampedPct = min(100.0f, max(0.0f, curAnimPct));
            int progW = (int)((float)trackW * (clampedPct / 100.0f));

            if (progW > 0) {
                float barX = (float)(urlLeft + 7);
                float barY = 66.5f;
                float barH = 2.0f;

                // Subtle groove
                Gdiplus::SolidBrush trackBg(Gdiplus::Color(35, 255, 255, 255));
                graphics.FillRectangle(&trackBg, barX, barY + 0.5f, (float)trackW, 1.0f);

                // Gradient progress fill: Electric Purple -> Neon Cyan
                Gdiplus::LinearGradientBrush progBrush(
                    Gdiplus::PointF(barX, barY),
                    Gdiplus::PointF(barX + (float)trackW, barY),
                    Gdiplus::Color(255, 168, 85, 247),
                    Gdiplus::Color(255, 56, 189, 248)
                );
                graphics.FillRectangle(&progBrush, barX, barY, (float)progW, barH);

                // Radiant comet head with cyan glow halo
                Gdiplus::SolidBrush haloBrush(Gdiplus::Color(90, 56, 189, 248));
                graphics.FillEllipse(&haloBrush, barX + (float)progW - 4.5f, barY - 2.0f, 6.0f, 6.0f);

                Gdiplus::SolidBrush cometHead(Gdiplus::Color(255, 255, 255, 255));
                graphics.FillEllipse(&cometHead, barX + (float)progW - 2.0f, barY - 0.5f, 3.0f, 3.0f);
            }
        }

        COLORREF lockColor = CLR_GREEN;
        if (currentUrl.rfind("https://", 0) == 0) {
            lockColor = CLR_GREEN;
        } else if (currentUrl.rfind("aeon://", 0) == 0 || currentUrl.rfind("file://", 0) == 0 || currentUrl.rfind("about:", 0) == 0) {
            lockColor = CLR_ACCENT;
        } else {
            lockColor = CLR_AMBER;
        }
        RECT lockR = { urlLeft + 6, 41, urlLeft + 26, 69 };
        DrawIcon(hdc, ICON_LOCK, lockR, lockColor, ch->hIconFontSmall);

        if (ch->zoomBadgeExpireTick > GetTickCount()) {
            char zStr[16];
            snprintf(zStr, sizeof(zStr), "%d%%", ch->currentZoomPct);
            RECT zR = { urlRight - 105, 45, urlRight - 55, 65 };
            Gdiplus::SolidBrush zBg(Gdiplus::Color(220, 24, 18, 48));
            FillGdiplusRoundRect(graphics, zR, 4, zBg);
            Gdiplus::Pen zPen(Gdiplus::Color(255, 56, 189, 248), 1.0f);
            DrawGdiplusRoundRect(graphics, zR, 4, zPen);
            DrawText16(hdc, zStr, zR, RGB(56, 189, 248), ch->hTextTabFont);
        }

        bool isBookmarked = HistoryEngine::IsBookmarked(currentUrl.c_str());
        RECT starR = { urlRight - 48, 41, urlRight - 26, 69 };
        const wchar_t* starGlyph = isBookmarked ? ICON_BOOKMARK_FILL : ICON_BOOKMARK;
        COLORREF starC = isBookmarked ? CLR_STAR_GOLD : ((ch->hoverBtn == 21) ? RGB(255, 255, 255) : CLR_TEXT_DIM);
        DrawIcon(hdc, starGlyph, starR, starC, ch->hIconFontSmall);

        bool shieldActive = ShieldHUD::IsShieldActiveForUrl(currentUrl.c_str());
        RECT shieldR = { urlRight - 26, 41, urlRight - 6, 69 };
        bool shieldHover = (ch->hoverBtn == 22);
        if (shieldHover) {
            Gdiplus::SolidBrush shBrush(Gdiplus::Color(60, 255, 85, 0));
            FillGdiplusRoundRect(graphics, shieldR, 4, shBrush);
        }
        COLORREF shieldColor = shieldActive ? CLR_BRAVE_ORANGE : RGB(239, 68, 68);
        DrawIcon(hdc, ICON_SHIELD, shieldR, shieldColor, ch->hIconFontSmall);

        if (!ch->urlFocused && (!ch->hUrlBar || !IsWindowVisible(ch->hUrlBar))) {
            RECT textR = { urlLeft + 28, 41, urlRight - 52, 69 };
            if (displayUrl == "aeon://newtab") {
                DrawText16(hdc, "Search Google or type a URL", textR, CLR_TEXT_DIM, ch->hTextFont);
            } else {
                DrawText16(hdc, displayUrl.c_str(), textR, CLR_TEXT, ch->hTextFont);
            }
        }
    }

    if (showLoadingBar) {
        float clampedPct = min(100.0f, max(0.0f, curAnimPct));
        int progW = (int)((float)width * (clampedPct / 100.0f));
        if (progW > 0) {
            // Soft laser glow along viewport boundary
            Gdiplus::SolidBrush glowBar(Gdiplus::Color(65, 56, 189, 248));
            graphics.FillRectangle(&glowBar, 0, 71, min(width, progW + 6), 3);

            // 2px gradient laser line
            Gdiplus::LinearGradientBrush laserBrush(
                Gdiplus::PointF(0.0f, 72.0f), Gdiplus::PointF((float)width, 74.0f),
                Gdiplus::Color(255, 236, 72, 153),
                Gdiplus::Color(255, 56, 189, 248)
            );
            graphics.FillRectangle(&laserBrush, 0, 72, progW, 2);

            // Specular leading white tip
            Gdiplus::SolidBrush tipBrush(Gdiplus::Color(255, 255, 255, 255));
            graphics.FillEllipse(&tipBrush, (float)max(0, progW - 4), 71.0f, 4.0f, 4.0f);
        }
    }

    // Active Split-View pane top glowing indicator along bottom edge of NavBar (never clipped by WebView2)
    if (ch->splitView.IsEnabled() && !ch->isFullscreen) {
        RECT clientRc = {};
        GetClientRect(ch->hwnd, &clientRc);
        RECT primaryRect, secondaryRect, splitterRect;
        ch->splitView.CalculateLayout(clientRc.right, clientRc.bottom, GetTopChromeHeight(ch),
                                      primaryRect, secondaryRect, splitterRect,
                                      GetEffectiveSidebarWidth(ch), ch->dpiScale);
        const RECT& activeRect = (ch->splitView.GetActivePane() == 0) ? primaryRect : secondaryRect;
        if (activeRect.right > activeRect.left) {
            float invScale = (g_PaintScale > 0.01f) ? (1.0f / g_PaintScale) : 1.0f;
            float ax0 = (float)activeRect.left * invScale;
            float ax1 = (float)activeRect.right * invScale;
            Gdiplus::LinearGradientBrush activeAccBrush(
                Gdiplus::PointF(ax0, (float)(CHROME_H - 2)),
                Gdiplus::PointF(ax1, (float)(CHROME_H - 2)),
                Gdiplus::Color(255, 0, 229, 255),
                Gdiplus::Color(220, 147, 51, 234)
            );
            Gdiplus::Pen activeAccPen(&activeAccBrush, 2.0f);
            graphics.DrawLine(&activeAccPen, ax0, (float)(CHROME_H - 1.5f),
                              ax1, (float)(CHROME_H - 1.5f));
        }
    }
}

static void PaintVerticalSidebar(ChromeState* ch, HDC hdc, int sidebarW, int sideH, int topOffset) {
    Gdiplus::Graphics graphics(hdc);
    graphics.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
    graphics.SetTextRenderingHint(Gdiplus::TextRenderingHintClearTypeGridFit);

    // Deep Obsidian vertical gradient backdrop (#080A15 -> #0E1124)
    Gdiplus::LinearGradientBrush bgBrush(
        Gdiplus::PointF(0.0f, 0.0f),
        Gdiplus::PointF(0.0f, (float)max(1, sideH)),
        Gdiplus::Color(255, 8, 10, 21),
        Gdiplus::Color(255, 14, 17, 36)
    );
    graphics.FillRectangle(&bgBrush, 0, 0, sidebarW, sideH);

    // Crisp 1px right border rgba(148,163,184,0.16)
    Gdiplus::Pen borderPen(Gdiplus::Color(42, 148, 163, 184), 1.0f);
    graphics.DrawLine(&borderPen, (float)(sidebarW - 1), 0.0f, (float)(sidebarW - 1), (float)sideH);

    const int padX = S(ch, 10);
    int curY = S(ch, 10);

    // "+ New Tab" Obsidian capsule button at top of vertical sidebar
    int addH = S(ch, 30);
    RECT localAddR = { padX, curY, sidebarW - padX, curY + addH };
    ch->sidebarAddRect = { localAddR.left, topOffset + localAddR.top, localAddR.right, topOffset + localAddR.bottom };
    bool addHover = (ch->hoverBtn == 50);

    Gdiplus::LinearGradientBrush addBg(
        Gdiplus::PointF((float)localAddR.left, (float)localAddR.top),
        Gdiplus::PointF((float)localAddR.right, (float)localAddR.bottom),
        Gdiplus::Color(addHover ? 95 : 55, 147, 51, 234),
        Gdiplus::Color(addHover ? 85 : 40, 0, 229, 255)
    );
    FillGdiplusRoundRect(graphics, localAddR, 6, addBg);
    Gdiplus::Pen addPen(Gdiplus::Color(addHover ? 200 : 95, 0, 229, 255), 1.0f);
    DrawGdiplusRoundRect(graphics, localAddR, 6, addPen);

    RECT addIconR = { localAddR.left + S(ch, 8), localAddR.top, localAddR.left + S(ch, 26), localAddR.bottom };
    DrawIcon(hdc, ICON_ADD, addIconR, RGB(0, 229, 255), ch->hIconFontSmall);
    RECT addTextR = { localAddR.left + S(ch, 28), localAddR.top, localAddR.right - S(ch, 8), localAddR.bottom };
    DrawText16(hdc, "New Tab", addTextR, addHover ? RGB(255, 255, 255) : CLR_TEXT, ch->hTextBold);

    curY += addH + S(ch, 10);

    auto& groupMgr = BrowserChrome::TabGroupManager::GetInstance();
    for (auto& t : ch->tabs) {
        t.groupId = groupMgr.GetTabGroupId(t.id);
    }

    std::set<uint32_t> renderedGroups;
    int rowH = S(ch, 32);
    int rowGap = S(ch, 4);

    for (int i = 0; i < (int)ch->tabs.size(); ++i) {
        ChromeTab& t = ch->tabs[i];

        if (t.groupId > 0 && renderedGroups.find(t.groupId) == renderedGroups.end()) {
            renderedGroups.insert(t.groupId);
            BrowserChrome::TabGroup* grpMut = groupMgr.GetGroupMut(t.groupId);
            if (grpMut) {
                int gH = S(ch, 26);
                RECT localGR = { padX, curY, sidebarW - padX, curY + gH };
                grpMut->headerRect = { localGR.left, topOffset + localGR.top, localGR.right, topOffset + localGR.bottom };
                RECT localChev = { localGR.right - S(ch, 20), localGR.top, localGR.right - S(ch, 4), localGR.bottom };
                grpMut->chevronRect = { localChev.left, topOffset + localChev.top, localChev.right, topOffset + localChev.bottom };

                bool grpHover = (ch->hoverBtn == 300 + (int)t.groupId || ch->hoverBtn == 400 + (int)t.groupId);
                BYTE r = GetRValue(grpMut->color);
                BYTE g = GetGValue(grpMut->color);
                BYTE b = GetBValue(grpMut->color);

                Gdiplus::SolidBrush gBg(Gdiplus::Color(grpHover ? 75 : 38, r, g, b));
                FillGdiplusRoundRect(graphics, localGR, 5, gBg);
                Gdiplus::Pen gPen(Gdiplus::Color(grpHover ? 220 : 140, r, g, b), 1.0f);
                DrawGdiplusRoundRect(graphics, localGR, 5, gPen);

                Gdiplus::SolidBrush dotBr(Gdiplus::Color(255, r, g, b));
                graphics.FillEllipse(&dotBr, localGR.left + S(ch, 8), localGR.top + (gH - 8) / 2, 8, 8);

                size_t gCount = groupMgr.GetTabCountInGroup(t.groupId);
                char cntBuf[16];
                snprintf(cntBuf, sizeof(cntBuf), "%zu", gCount);
                RECT badgeR = { localChev.left - S(ch, 22), localGR.top + 4, localChev.left - 2, localGR.bottom - 4 };
                Gdiplus::SolidBrush bBg(Gdiplus::Color(95, r, g, b));
                FillGdiplusRoundRect(graphics, badgeR, 4, bBg);
                HFONT oldF = (HFONT)SelectObject(hdc, ch->hTextTabFont);
                SetTextColor(hdc, RGB(248, 250, 252));
                SetBkMode(hdc, TRANSPARENT);
                DrawTextA(hdc, cntBuf, -1, &badgeR, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
                SelectObject(hdc, oldF);

                RECT gNameR = { localGR.left + S(ch, 22), localGR.top, badgeR.left - 4, localGR.bottom };
                DrawText16(hdc, grpMut->name.c_str(), gNameR, CLR_TEXT, ch->hTextBold);

                DrawIcon(hdc, grpMut->collapsed ? ICON_CHEVRON_RIGHT : ICON_CHEVRON_DOWN,
                         localChev, CLR_TEXT_DIM, ch->hIconFontSmall);

                curY += gH + rowGap;
            }
        }

        if (t.groupId > 0 && groupMgr.IsGroupCollapsed(t.groupId)) {
            t.tabRect = { 0, 0, 0, 0 };
            t.closeRect = { 0, 0, 0, 0 };
            t.audioRect = { 0, 0, 0, 0 };
            continue;
        }

        if (curY + rowH > sideH - S(ch, 8)) {
            t.tabRect = { 0, 0, 0, 0 };
            t.closeRect = { 0, 0, 0, 0 };
            t.audioRect = { 0, 0, 0, 0 };
            continue;
        }

        int leftIndent = (t.groupId > 0) ? (padX + S(ch, 8)) : padX;
        RECT localTR = { leftIndent, curY, sidebarW - padX, curY + rowH };
        t.tabRect = { localTR.left, topOffset + localTR.top, localTR.right, topOffset + localTR.bottom };

        bool active = (i == ch->activeTab);
        bool hover  = (i == ch->hoverTab);
        bool isSplitPrimary = ch->splitView.IsEnabled() && (i == ch->splitView.GetPrimaryTabIndex());
        bool isSplitSecondary = ch->splitView.IsEnabled() && (i == ch->splitView.GetSecondaryTabIndex());
        bool isSplitMember = isSplitPrimary || isSplitSecondary;

        if (active || isSplitMember) {
            Gdiplus::SolidBrush actBg(Gdiplus::Color(235, 22, 27, 54));
            FillGdiplusRoundRect(graphics, localTR, 6, actBg);
            Gdiplus::Pen actPen(Gdiplus::Color(150, 0, 229, 255), 1.0f);
            DrawGdiplusRoundRect(graphics, localTR, 6, actPen);

            // Crisp 3px left vertical neon pill indicator
            Gdiplus::SolidBrush pillBr(Gdiplus::Color(255, 0, 229, 255));
            RECT pillR = { localTR.left + 2, localTR.top + 6, localTR.left + 5, localTR.bottom - 6 };
            FillGdiplusRoundRect(graphics, pillR, 1, pillBr);
        } else if (hover) {
            Gdiplus::SolidBrush hovBg(Gdiplus::Color(140, 17, 21, 40));
            FillGdiplusRoundRect(graphics, localTR, 6, hovBg);
        }

        Gdiplus::SolidBrush dotBr(Gdiplus::Color(255, active ? 0 : 148, active ? 229 : 163, active ? 255 : 184));
        graphics.FillEllipse(&dotBr, localTR.left + S(ch, 10), localTR.top + (rowH - 8) / 2, 8, 8);

        int closeSize = S(ch, 18);
        int closeLocalY = localTR.top + (rowH - closeSize) / 2;
        RECT localCloseR = { localTR.right - closeSize - S(ch, 6), closeLocalY, localTR.right - S(ch, 6), closeLocalY + closeSize };
        t.closeRect = { localCloseR.left, topOffset + localCloseR.top, localCloseR.right, topOffset + localCloseR.bottom };
        t.audioRect = { 0, 0, 0, 0 };

        bool closeHover = (ch->hoverBtn == 200 + i);
        if (closeHover || hover || active) {
            if (closeHover) {
                Gdiplus::SolidBrush cBg(Gdiplus::Color(235, 62, 67, 104));
                graphics.FillEllipse(&cBg, localCloseR.left, localCloseR.top, closeSize, closeSize);
            }
            DrawIcon(hdc, ICON_CLOSE, localCloseR, closeHover ? RGB(255, 255, 255) : CLR_TEXT_DIM, ch->hIconFontSmall);
        }

        RECT titleR = { localTR.left + S(ch, 24), localTR.top, localCloseR.left - 4, localTR.bottom };
        const char* titleTxt = t.loading ? "Loading..." : (t.title.empty() ? "New Tab" : t.title.c_str());
        DrawText16(hdc, titleTxt, titleR, active ? RGB(248, 250, 252) : CLR_TEXT_DIM, ch->hTextTabFont);

        curY += rowH + rowGap;
    }
}

static void PaintChrome(ChromeState* ch, HDC hdcOverride = nullptr) {
    if (!ch || !ch->hwnd) return;

    RECT rc;
    GetClientRect(ch->hwnd, &rc);
    int W = rc.right;
    int H = rc.bottom;
    if (W <= 0 || H <= 0) return;

    HDC targetDC = hdcOverride ? hdcOverride : GetDC(ch->hwnd);
    if (!targetDC) return;

    const int topChromeH = S(ch, CHROME_H);
    const int topContentY = GetTopChromeHeight(ch);
    const int sidebarW = GetEffectiveSidebarWidth(ch);

    // 1. Offscreen double-buffered Top Chrome Strip (0, 0, W, topChromeH)
    if (!ch->isFullscreen && topChromeH > 0) {
        HDC memDC = CreateCompatibleDC(targetDC);
        BITMAPINFO bmi = {};
        bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
        bmi.bmiHeader.biWidth = W;
        bmi.bmiHeader.biHeight = -topChromeH; // Top-down DIB
        bmi.bmiHeader.biPlanes = 1;
        bmi.bmiHeader.biBitCount = 32;
        bmi.bmiHeader.biCompression = BI_RGB;

        void* pBits = nullptr;
        HBITMAP memBmp = CreateDIBSection(targetDC, &bmi, DIB_RGB_COLORS, &pBits, nullptr, 0);
        if (memBmp) {
            if (pBits) memset(pBits, 0, (size_t)W * (size_t)topChromeH * 4);
            HBITMAP oldBmp = (HBITMAP)SelectObject(memDC, memBmp);

            // Solid Obsidian base (#070913) prevents semi-transparent alpha ghosts
            FillRectColor(memDC, { 0, 0, W, topChromeH }, RGB(7, 9, 19));
            int logicalW = max(1, MulDiv(W, 96, ch->dpi > 0 ? ch->dpi : 96));
            g_PaintScale = ch->dpiScale;
            PaintTabStrip(ch, memDC, logicalW);
            PaintNavBar(ch, memDC, logicalW);
            g_PaintScale = 1.0f;

            BitBlt(targetDC, 0, 0, W, topChromeH, memDC, 0, 0, SRCCOPY);
            SelectObject(memDC, oldBmp);
            DeleteObject(memBmp);
        }
        DeleteDC(memDC);
    }

    // 2. Offscreen double-buffered Arc/Obsidian Vertical Sidebar (0, topContentY, sidebarW, sideH)
    if (sidebarW > 0 && H > topContentY) {
        int sideH = H - topContentY;
        HDC sideDC = CreateCompatibleDC(targetDC);
        BITMAPINFO sBmi = {};
        sBmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
        sBmi.bmiHeader.biWidth = sidebarW;
        sBmi.bmiHeader.biHeight = -sideH;
        sBmi.bmiHeader.biPlanes = 1;
        sBmi.bmiHeader.biBitCount = 32;
        sBmi.bmiHeader.biCompression = BI_RGB;

        void* sBits = nullptr;
        HBITMAP sideBmp = CreateDIBSection(targetDC, &sBmi, DIB_RGB_COLORS, &sBits, nullptr, 0);
        if (sideBmp) {
            if (sBits) memset(sBits, 0, (size_t)sidebarW * (size_t)sideH * 4);
            HBITMAP oldSBmp = (HBITMAP)SelectObject(sideDC, sideBmp);

            PaintVerticalSidebar(ch, sideDC, sidebarW, sideH, topContentY);

            BitBlt(targetDC, 0, topContentY, sidebarW, sideH, sideDC, 0, 0, SRCCOPY);
            SelectObject(sideDC, oldSBmp);
            DeleteObject(sideBmp);
        }
        DeleteDC(sideDC);
    } else {
        ch->sidebarAddRect = { 0, 0, 0, 0 };
    }

    // 3. Offscreen double-buffered Split-View Divider Gutter (zero flicker during drag/resize)
    if (ch->splitView.IsEnabled() && !ch->isFullscreen && H > topContentY) {
        RECT primaryRect, secondaryRect, splitterRect;
        ch->splitView.CalculateLayout(W, H, topContentY, primaryRect, secondaryRect, splitterRect,
                                      sidebarW, ch->dpiScale);

        int divW = splitterRect.right - splitterRect.left;
        int divH = splitterRect.bottom - splitterRect.top;
        if (divW > 0 && divH > 0) {
            HDC divDC = CreateCompatibleDC(targetDC);
            BITMAPINFO dBmi = {};
            dBmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
            dBmi.bmiHeader.biWidth = divW;
            dBmi.bmiHeader.biHeight = -divH;
            dBmi.bmiHeader.biPlanes = 1;
            dBmi.bmiHeader.biBitCount = 32;
            dBmi.bmiHeader.biCompression = BI_RGB;

            void* dBits = nullptr;
            HBITMAP divBmp = CreateDIBSection(targetDC, &dBmi, DIB_RGB_COLORS, &dBits, nullptr, 0);
            if (divBmp) {
                if (dBits) memset(dBits, 0, (size_t)divW * (size_t)divH * 4);
                HBITMAP oldDBmp = (HBITMAP)SelectObject(divDC, divBmp);

                Gdiplus::Graphics gDiv(divDC);
                gDiv.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);

                bool splitHover = (ch->hoverBtn == 500 || ch->hoverBtn == 501 ||
                                   ch->splitView.IsHovered() || ch->splitView.IsDragging());

                Gdiplus::SolidBrush bgBr(Gdiplus::Color(255, 11, 13, 26));
                gDiv.FillRectangle(&bgBr, 0, 0, divW, divH);

                float localMidX = (float)divW / 2.0f;
                if (splitHover) {
                    Gdiplus::LinearGradientBrush spineBrush(
                        Gdiplus::PointF(localMidX, 0.0f),
                        Gdiplus::PointF(localMidX, (float)divH),
                        Gdiplus::Color(255, 0, 229, 255),
                        Gdiplus::Color(255, 168, 85, 247)
                    );
                    Gdiplus::Pen spinePen(&spineBrush, 2.0f);
                    gDiv.DrawLine(&spinePen, localMidX, 0.0f, localMidX, (float)divH);
                } else {
                    Gdiplus::Pen spinePen(Gdiplus::Color(135, 147, 51, 234), 1.2f);
                    gDiv.DrawLine(&spinePen, localMidX, 0.0f, localMidX, (float)divH);
                }

                // Center ergonomic 3-dot grip handle inside gutter
                float midY = (float)divH / 2.0f;
                Gdiplus::SolidBrush dotBrush(splitHover
                    ? Gdiplus::Color(255, 248, 250, 252)
                    : Gdiplus::Color(180, 192, 132, 252));
                for (int d = -1; d <= 1; ++d) {
                    gDiv.FillEllipse(&dotBrush, localMidX - 1.5f, midY + (float)(d * 6) - 1.5f, 3.0f, 3.0f);
                }

                // Top quick-swap indicator capsule inside gutter
                bool swapHover = (ch->hoverBtn == 501);
                Gdiplus::SolidBrush topCapBr(swapHover
                    ? Gdiplus::Color(255, 0, 229, 255)
                    : Gdiplus::Color(210, 147, 51, 234));
                gDiv.FillRectangle(&topCapBr, 1.0f, 6.0f, (float)max(2, divW - 2), 18.0f);

                BitBlt(targetDC, splitterRect.left, splitterRect.top, divW, divH, divDC, 0, 0, SRCCOPY);
                SelectObject(divDC, oldDBmp);
                DeleteObject(divBmp);
            }
            DeleteDC(divDC);
        }
    }

    if (!hdcOverride) {
        ReleaseDC(ch->hwnd, targetDC);
    }
}

#define IDC_URLBAR 9001
#define ID_LOADING_TIMER 9002
#define LOADING_TIMER_MS 30
static WNDPROC g_OrigUrlBarProc = nullptr;

static bool g_bProgrammaticUrlChange = false;
static std::string g_userTypedText = "";
static bool g_isBrowsingSuggestions = false;

static void ActivateUrlBar(ChromeState* ch, bool selectAll = true);
static void CommitUrlBar(ChromeState* ch);
static void NavigateActiveTab(ChromeState* ch, const char* url);
static void TriggerOmniSuggestions(ChromeState* ch, HWND hEdit);
static void SetUrlBarPreview(HWND hEdit, const std::string& text);
static void ExecuteOmniNavigation(ChromeState* ch, const std::string& rawInput);

static RECT GetOmniboxAnchorRect(ChromeState* ch) {
    RECT rc = {};
    if (!ch) return rc;
    GetClientRect(ch->hwnd, &rc);
    int urlLeft = S(ch, 102);
    int urlRight = rc.right - S(ch, 144);

    POINT ptTopLeft = { urlLeft, S(ch, 41) };
    POINT ptBottomRight = { urlRight, S(ch, 69) };
    ClientToScreen(ch->hwnd, &ptTopLeft);
    ClientToScreen(ch->hwnd, &ptBottomRight);

    RECT anchorRect = { ptTopLeft.x, ptTopLeft.y, ptBottomRight.x, ptBottomRight.y };
    return anchorRect;
}

static void SetUrlBarPreview(HWND hEdit, const std::string& text) {
    if (!hEdit) return;
    g_bProgrammaticUrlChange = true;
    std::wstring wText = Utf8ToUtf16(text);
    SetWindowTextW(hEdit, wText.c_str());
    SendMessageW(hEdit, EM_SETSEL, 0, -1);
    g_bProgrammaticUrlChange = false;
}

static void TriggerOmniSuggestions(ChromeState* ch, HWND hEdit) {
    if (!ch || !hEdit || g_bProgrammaticUrlChange) return;

    wchar_t wbuf[2048] = {};
    GetWindowTextW(hEdit, wbuf, 2048);
    std::string query = Utf16ToUtf8(wbuf);
    g_userTypedText = query;
    g_isBrowsingSuggestions = false;

    if (query.empty()) {
        AeonOmni::Hide();
        return;
    }

    RECT anchorRect = GetOmniboxAnchorRect(ch);
    AeonOmni::ShowSuggestions(query, anchorRect);
}

static LRESULT CALLBACK UrlBarSubclassProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    HWND parent = GetParent(hWnd);
    ChromeState* ch = reinterpret_cast<ChromeState*>(GetWindowLongPtr(parent, GWLP_USERDATA));

    switch (msg) {
        case WM_KEYDOWN: {
            if (wParam == VK_DOWN) {
                if (AeonOmni::IsVisible()) {
                    if (AeonOmni::HandleKeyDown(VK_DOWN)) {
                        std::string previewUrl = AeonOmni::GetSelectedUrl();
                        if (!previewUrl.empty()) {
                            g_isBrowsingSuggestions = true;
                            SetUrlBarPreview(hWnd, previewUrl);
                        } else {
                            g_isBrowsingSuggestions = false;
                            SetUrlBarPreview(hWnd, g_userTypedText);
                        }
                    }
                    return 0;
                } else if (ch) {
                    TriggerOmniSuggestions(ch, hWnd);
                    return 0;
                }
            }

            if (wParam == VK_UP) {
                if (AeonOmni::IsVisible()) {
                    if (AeonOmni::HandleKeyDown(VK_UP)) {
                        std::string previewUrl = AeonOmni::GetSelectedUrl();
                        if (!previewUrl.empty()) {
                            g_isBrowsingSuggestions = true;
                            SetUrlBarPreview(hWnd, previewUrl);
                        } else {
                            g_isBrowsingSuggestions = false;
                            SetUrlBarPreview(hWnd, g_userTypedText);
                        }
                    }
                    return 0;
                }
            }

            if (wParam == VK_TAB) {
                if (AeonOmni::IsVisible()) {
                    std::string completedUrl = AeonOmni::GetSelectedUrl();
                    if (completedUrl.empty() && AeonOmni::HandleKeyDown(VK_DOWN)) {
                        completedUrl = AeonOmni::GetSelectedUrl();
                    }
                    if (!completedUrl.empty()) {
                        g_userTypedText = completedUrl;
                        SetUrlBarPreview(hWnd, completedUrl);
                        if (ch) TriggerOmniSuggestions(ch, hWnd);
                    }
                    return 0;
                }
            }

            if (wParam == VK_RETURN) {
                std::string targetUrl;
                if (AeonOmni::IsVisible()) {
                    targetUrl = AeonOmni::GetSelectedUrl();
                    AeonOmni::Hide();
                }

                if (targetUrl.empty()) {
                    wchar_t wbuf[2048] = {};
                    GetWindowTextW(hWnd, wbuf, 2048);
                    targetUrl = Utf16ToUtf8(wbuf);
                }

                ShowWindow(hWnd, SW_HIDE);
                SetFocus(parent);
                if (ch) {
                    ch->urlFocused = false;
                    ExecuteOmniNavigation(ch, targetUrl);
                    PaintChrome(ch);
                }
                return 0;
            }

            if (wParam == VK_ESCAPE) {
                if (AeonOmni::IsVisible()) {
                    if (g_isBrowsingSuggestions) {
                        SetUrlBarPreview(hWnd, g_userTypedText);
                        g_isBrowsingSuggestions = false;
                    }
                    AeonOmni::Hide();
                    return 0;
                }

                ShowWindow(hWnd, SW_HIDE);
                SetFocus(parent);
                if (ch) {
                    ch->urlFocused = false;
                    PaintChrome(ch);
                }
                return 0;
            }
            break;
        }

        case WM_KILLFOCUS: {
            HWND newFocus = (HWND)wParam;
            if (newFocus != AeonOmni::GetHwnd()) {
                AeonOmni::Hide();
                ShowWindow(hWnd, SW_HIDE);
                if (ch) {
                    ch->urlFocused = false;
                    PaintChrome(ch);
                }
            }
            break;
        }

        case WM_CHAR: {
            if (wParam == VK_RETURN || wParam == VK_ESCAPE || wParam == VK_TAB) {
                return 0;
            }
            g_isBrowsingSuggestions = false;
            break;
        }
    }

    return CallWindowProcW(g_OrigUrlBarProc, hWnd, msg, wParam, lParam);
}

static void ActivateUrlBar(ChromeState* ch, bool selectAll) {
    RECT rc; GetClientRect(ch->hwnd, &rc);
    int urlLeft = S(ch, 102 + 28);
    int urlRight = rc.right - S(ch, 144 + 52);
    int urlW = max(S(ch, 40), urlRight - urlLeft);
    int urlY = S(ch, 44);
    int urlH = S(ch, 22);

    if (!ch->hUrlBar) {
        ch->hUrlBar = CreateWindowExW(
            0, L"EDIT", L"",
            WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL,
            urlLeft, urlY, urlW, urlH,
            ch->hwnd, (HMENU)(UINT_PTR)IDC_URLBAR, nullptr, nullptr);

        SendMessageW(ch->hUrlBar, WM_SETFONT, (WPARAM)ch->hUrlFont, TRUE);

        g_OrigUrlBarProc = (WNDPROC)SetWindowLongPtr(
            ch->hUrlBar, GWLP_WNDPROC, (LONG_PTR)UrlBarSubclassProc);
    }

    MoveWindow(ch->hUrlBar, urlLeft, urlY, urlW, urlH, TRUE);

    int effectiveActiveTab = ch->activeTab;
    if (ch->splitView.IsEnabled()) {
        effectiveActiveTab = ch->splitView.GetActiveTabIndex();
    }

    const char* url = "about:blank";
    if (effectiveActiveTab >= 0 && effectiveActiveTab < (int)ch->tabs.size())
        url = ch->tabs[effectiveActiveTab].url.c_str();

    std::string revUrl = ReverseMapUrl(url);
    g_bProgrammaticUrlChange = true;
    if (revUrl == "aeon://newtab") {
        SetWindowTextW(ch->hUrlBar, L"");
        g_userTypedText = "";
    } else {
        std::wstring wUrl = Utf8ToUtf16(revUrl);
        SetWindowTextW(ch->hUrlBar, wUrl.c_str());
        g_userTypedText = revUrl;
    }
    g_bProgrammaticUrlChange = false;
    g_isBrowsingSuggestions = false;

    ShowWindow(ch->hUrlBar, SW_SHOW);
    SetFocus(ch->hUrlBar);
    if (selectAll)
        SendMessageW(ch->hUrlBar, EM_SETSEL, 0, -1);

    ch->urlFocused = true;
    PaintChrome(ch);
}

static void ExecuteOmniNavigation(ChromeState* ch, const std::string& rawInput) {
    if (!ch) return;

    std::string input = rawInput;
    size_t start = input.find_first_not_of(" \t\r\n");
    if (start == std::string::npos) return;
    size_t end = input.find_last_not_of(" \t\r\n");
    std::string url = input.substr(start, end - start + 1);

    if (url.rfind("= ", 0) == 0) {
        url = url.substr(2);
    }

    if (url.rfind("@ai ", 0) == 0 || url.rfind("@ask ", 0) == 0) {
        size_t prefixLen = (url.rfind("@ai ", 0) == 0) ? 4 : 5;
        std::string prompt = url.substr(prefixLen);
        std::string encoded;
        for (char c : prompt) {
            if (c == ' ') encoded += "+";
            else if (c == '+') encoded += "%2B";
            else if (c == '&') encoded += "%26";
            else if (c == '?') encoded += "%3F";
            else if (c == '#') encoded += "%23";
            else if (c == '%') encoded += "%25";
            else encoded += c;
        }
        url = "aeon://intelligence?q=" + encoded;
        NavigateActiveTab(ch, url.c_str());
        return;
    }

    bool hasScheme = (url.find("://") != std::string::npos ||
                      url.rfind("aeon://", 0) == 0 ||
                      url.rfind("about:", 0) == 0 ||
                      url.rfind("file://", 0) == 0 ||
                      url.rfind("chrome://", 0) == 0 ||
                      url.rfind("edge://", 0) == 0 ||
                      url.rfind("view-source:", 0) == 0);

    if (!hasScheme) {
        bool hasSpaces = (url.find(' ') != std::string::npos);
        bool isLocal = (url.rfind("localhost", 0) == 0 || url.rfind("127.0.0.1", 0) == 0 || url.rfind("[::1]", 0) == 0);
        bool hasDot = (url.find('.') != std::string::npos);

        if (!hasSpaces && (isLocal || hasDot)) {
            url = "https://" + url;
        } else {
            std::string encoded;
            for (char c : url) {
                if (c == ' ') encoded += '+';
                else if (c == '+') encoded += "%2B";
                else if (c == '&') encoded += "%26";
                else if (c == '?') encoded += "%3F";
                else if (c == '#') encoded += "%23";
                else if (c == '%') encoded += "%25";
                else encoded += c;
            }

            std::string engine = ch->settings.search_engine;
            for (auto& c : engine) c = (char)tolower(c);

            if (engine == "google") {
                url = "https://www.google.com/search?q=" + encoded;
            } else if (engine == "bing") {
                url = "https://www.bing.com/search?q=" + encoded;
            } else if (engine == "brave") {
                url = "https://search.brave.com/search?q=" + encoded;
            } else if (engine == "ecosia") {
                url = "https://www.ecosia.org/search?q=" + encoded;
            } else if (engine == "searxng") {
                url = "https://searx.be/search?q=" + encoded;
            } else {
                url = "https://duckduckgo.com/?q=" + encoded;
            }
        }
    }

    NavigateActiveTab(ch, url.c_str());
}

static void CommitUrlBar(ChromeState* ch) {
    if (!ch->hUrlBar) return;
    wchar_t wbuf[2048] = {};
    GetWindowTextW(ch->hUrlBar, wbuf, 2048);

    ShowWindow(ch->hUrlBar, SW_HIDE);
    SetFocus(ch->hwnd);
    ch->urlFocused = false;

    if (wbuf[0] == L'\0') return;

    std::string input = Utf16ToUtf8(wbuf);
    ExecuteOmniNavigation(ch, input);
}

static void NavigateActiveTab(ChromeState* ch, const char* url) {
    int effectiveTab = ch->activeTab;
    if (ch->splitView.IsEnabled()) {
        effectiveTab = ch->splitView.GetActiveTabIndex();
    }

    if (effectiveTab >= 0 && effectiveTab < (int)ch->tabs.size()) {
        auto& t = ch->tabs[effectiveTab];
        t.url = url ? url : "";
        std::string mapped = ReverseMapUrl(t.url);
        if (mapped == "aeon://newtab" || t.url == "about:blank" || t.url.empty()) {
            t.loading = false;
            t.progressPercent = 0;
            t.animProgress = 0.0f;
        } else {
            t.loading = true;
            t.progressPercent = 25;
            t.animProgress = 10.0f;
            SetTimer(ch->hwnd, ID_LOADING_TIMER, LOADING_TIMER_MS, nullptr);
        }
        t.isSleeping = false;
        TabSleepManager::OnTabActivity(t.id);

        AeonEngineVTable* engineToUse = TierDispatcher::GetInstance().GetEngineForUrl(t.url.c_str());
        if (!engineToUse) engineToUse = ch->engine;
        if (engineToUse) engineToUse->Navigate(t.id, t.url.c_str(), nullptr);
        SyncViewportInternal(ch);
        PaintChrome(ch);
    }
}

namespace BrowserChrome {

static void OpenOrFocusInternalUrl(ChromeState* ch, HWND hwnd, const char* url);

int HitTest(HWND hwnd, int x, int y) {
    ChromeState* ch = reinterpret_cast<ChromeState*>(
        GetWindowLongPtr(hwnd, GWLP_USERDATA));
    if (!ch) return -1;

    RECT rc; GetClientRect(hwnd, &rc);
    int physW = rc.right;
    int physH = rc.bottom;
    int topChromeH = S(ch, CHROME_H);
    int topContentY = GetTopChromeHeight(ch);
    int sidebarW = GetEffectiveSidebarWidth(ch);

    if (ch->splitView.IsEnabled() && !ch->isFullscreen && y >= topContentY) {
        RECT primaryRect, secondaryRect, splitterRect;
        ch->splitView.CalculateLayout(physW, physH, topContentY, primaryRect, secondaryRect, splitterRect,
                                      sidebarW, ch->dpiScale);
        if (x >= splitterRect.left - S(ch, 6) && x <= splitterRect.right + S(ch, 6) &&
            y >= splitterRect.top && y <= splitterRect.top + S(ch, 26)) {
            return HIT_SPLIT_SWAP;
        }
        if (ch->splitView.HitTestSplitter(x, y)) {
            return HIT_SPLITTER;
        }
    }

    // Arc / Obsidian Vertical Sidebar hit-testing (physical client coordinates)
    if (sidebarW > 0 && !ch->isFullscreen && x >= 0 && x < sidebarW && y >= topContentY) {
        if (ch->sidebarAddRect.right > ch->sidebarAddRect.left &&
            x >= ch->sidebarAddRect.left && x <= ch->sidebarAddRect.right &&
            y >= ch->sidebarAddRect.top && y <= ch->sidebarAddRect.bottom) {
            return 50;
        }

        auto& groupMgr = BrowserChrome::TabGroupManager::GetInstance();
        for (const auto& grp : groupMgr.GetAllGroups()) {
            if (grp.chevronRect.right > grp.chevronRect.left &&
                x >= grp.chevronRect.left && x <= grp.chevronRect.right &&
                y >= grp.chevronRect.top && y <= grp.chevronRect.bottom) {
                return 400 + (int)grp.id;
            }
            if (grp.headerRect.right > grp.headerRect.left &&
                x >= grp.headerRect.left && x <= grp.headerRect.right &&
                y >= grp.headerRect.top && y <= grp.headerRect.bottom) {
                return 300 + (int)grp.id;
            }
        }

        for (int i = 0; i < (int)ch->tabs.size(); i++) {
            const RECT& tr = ch->tabs[i].tabRect;
            if (tr.right > tr.left && x >= tr.left && x <= tr.right && y >= tr.top && y <= tr.bottom) {
                const RECT& cr = ch->tabs[i].closeRect;
                if (cr.right > cr.left && x >= cr.left && x <= cr.right && y >= cr.top && y <= cr.bottom) {
                    return 200 + i;
                }
                return 100 + i;
            }
        }

        return -1;
    }

    // Convert physical top-strip coordinates to 96-DPI logical coordinates
    int dpiVal = (ch->dpi > 0) ? ch->dpi : 96;
    int lx = MulDiv(x, 96, dpiVal);
    int ly = MulDiv(y, 96, dpiVal);
    int W  = max(1, MulDiv(physW, 96, dpiVal));

    if (y >= 0 && y < topChromeH && ly < TAB_STRIP_HEIGHT) {
        if (lx >= W - CTRL_W)         return 12;
        if (lx >= W - CTRL_W * 2)     return 11;
        if (lx >= W - CTRL_TOTAL)     return 10;

        if (ch->verticalSidebar) {
            if (lx >= 42 && lx <= min(W - 150, 260) && ly >= 6 && ly <= 30) {
                return 51;
            }
            return -1;
        }

        auto& groupMgr = BrowserChrome::TabGroupManager::GetInstance();
        for (const auto& grp : groupMgr.GetAllGroups()) {
            if (lx >= grp.chevronRect.left && lx <= grp.chevronRect.right &&
                ly >= grp.chevronRect.top && ly <= grp.chevronRect.bottom) {
                return 400 + (int)grp.id;
            }
            if (lx >= grp.headerRect.left && lx <= grp.headerRect.right &&
                ly >= grp.headerRect.top && ly <= grp.headerRect.bottom) {
                return 300 + (int)grp.id;
            }
        }

        for (int i = 0; i < (int)ch->tabs.size(); i++) {
            const RECT& tr = ch->tabs[i].tabRect;
            if (tr.right > tr.left && lx >= tr.left && lx <= tr.right && ly >= tr.top && ly <= tr.bottom) {
                if (ch->tabs[i].isPinned) {
                    return 100 + i;
                }
                const RECT& cr = ch->tabs[i].closeRect;
                if (cr.right > cr.left && lx >= cr.left && lx <= cr.right && ly >= cr.top && ly <= cr.bottom) {
                    return 200 + i;
                }
                const RECT& ar = ch->tabs[i].audioRect;
                if (ar.right > ar.left && lx >= ar.left && lx <= ar.right && ly >= ar.top && ly <= ar.bottom) {
                    return 600 + i;
                }
                return 100 + i;
            }
        }

        const int TAB_START_X = 40;
        const int CTRL_TOTAL_W = 138;
        const int ADD_BTN_W = 28;
        const int MIN_TAB_W = 75;
        int rightLimit = max(TAB_START_X + MIN_TAB_W, W - CTRL_TOTAL_W - ADD_BTN_W - 8);

        int lastX = TAB_START_X;
        for (const auto& t : ch->tabs) {
            if (t.tabRect.right > lastX) lastX = t.tabRect.right;
        }
        for (const auto& g : groupMgr.GetAllGroups()) {
            if (g.headerRect.right > lastX) lastX = g.headerRect.right;
        }

        int addX = min(lastX + 6, rightLimit);
        if (lx >= addX && lx <= addX + ADD_BTN_W && ly >= 6 && ly <= 30) {
            return 50;
        }

        return -1;
    }

    if (y >= 0 && y < topChromeH && ly >= TAB_STRIP_HEIGHT && ly < CHROME_H) {
        if (lx >= 8 && lx <= 34)   return 1;
        if (lx >= 38 && lx <= 64)  return 2;
        if (lx >= 68 && lx <= 94)  return 3;

        if (lx >= W - 136 && lx <= W - 108) return 20;
        if (lx >= W - 104 && lx <= W - 76)  return 24;
        if (lx >= W - 72 && lx <= W - 44)   return 23;
        if (lx >= W - 40 && lx <= W - 12)   return 25;

        int urlLeft = 102;
        int urlRight = W - 144;
        if (lx >= urlLeft && lx <= urlRight) {
            if (lx >= urlRight - 26) return 22;
            if (lx >= urlRight - 48) return 21;
            if (lx <= urlLeft + 26)  return 6;
            return 5;
        }

        return 0;
    }

    return -1;
}

void TriggerBookmarkToast(HWND hwnd) {
    ChromeState* ch = reinterpret_cast<ChromeState*>(GetWindowLongPtr(hwnd, GWLP_USERDATA));
    if (!ch) return;
    int effectiveTab = ch->splitView.IsEnabled() ? ch->splitView.GetActiveTabIndex() : ch->activeTab;
    if (effectiveTab < 0 || effectiveTab >= (int)ch->tabs.size()) return;

    RECT rc; GetClientRect(hwnd, &rc);
    int urlRight = rc.right - S(ch, 144);
    POINT pt = { urlRight - S(ch, 37), S(ch, CHROME_H) };
    ClientToScreen(hwnd, &pt);
    auto& t = ch->tabs[effectiveTab];
    bool isBkm = HistoryEngine::IsBookmarked(t.url.c_str());
    std::wstring wTitle = Utf8ToUtf16(t.title.empty() ? t.url : t.title);
    BookmarkToast::Show(hwnd, pt, t.url.c_str(), wTitle.c_str(), isBkm,
        [ch](const char* u, const char* name, const char* folder) {
            HistoryEngine::AddBookmark(u, name, folder);
            PaintChrome(ch);
        },
        [ch](const char* u) {
            HistoryEngine::DeleteBookmark(u);
            PaintChrome(ch);
        }
    );
}

void TriggerShieldHUD(HWND hwnd) {
    ChromeState* ch = reinterpret_cast<ChromeState*>(GetWindowLongPtr(hwnd, GWLP_USERDATA));
    if (!ch) return;
    if (ShieldHUD::IsVisible()) {
        ShieldHUD::Hide();
        return;
    }
    RECT rc; GetClientRect(hwnd, &rc);
    int urlRight = rc.right - S(ch, 144);
    POINT pt = { urlRight - S(ch, 16), S(ch, CHROME_H) };
    ClientToScreen(hwnd, &pt);

    int effectiveTab = ch->splitView.IsEnabled() ? ch->splitView.GetActiveTabIndex() : ch->activeTab;
    const char* currentUrl = (effectiveTab >= 0 && effectiveTab < (int)ch->tabs.size())
        ? ch->tabs[effectiveTab].url.c_str() : "aeon://newtab";
    ShieldHUD::Show(hwnd, pt, currentUrl,
        [ch, effectiveTab](bool bShieldsActive, const std::string& domain) {
            (void)domain;
            (void)bShieldsActive;
            if (ch->engine && effectiveTab >= 0 && effectiveTab < (int)ch->tabs.size()) {
                ch->engine->Reload(ch->tabs[effectiveTab].id, 0);
            }
            PaintChrome(ch);
        },
        [ch, hwnd, effectiveTab](const std::string& action, const std::string& domain) {
            (void)domain;
            if (action == "settings") {
                OpenOrFocusInternalUrl(ch, hwnd, "aeon://shields");
            } else if (action == "clear_data") {
                if (ch->engine && effectiveTab >= 0 && effectiveTab < (int)ch->tabs.size()) {
                    ch->engine->Reload(ch->tabs[effectiveTab].id, 1);
                }
            }
        }
    );
}

void TriggerAppMenu(HWND hwnd) {
    ChromeState* ch = reinterpret_cast<ChromeState*>(GetWindowLongPtr(hwnd, GWLP_USERDATA));
    if (!ch) return;
    if (AppMenu::IsVisible()) {
        AppMenu::Hide();
        return;
    }
    RECT rc; GetClientRect(hwnd, &rc);
    POINT pt = { rc.right - S(ch, 26), S(ch, CHROME_H) };
    ClientToScreen(hwnd, &pt);
    auto callback = [ch, hwnd](AppMenu::ItemId id) {
        switch (id) {
            case AppMenu::ItemId::NewTab:          CreateTab(hwnd, "aeon://newtab"); break;
            case AppMenu::ItemId::NewWindow:
                {
                    char exePath[MAX_PATH];
                    GetModuleFileNameA(nullptr, exePath, MAX_PATH);
                    ShellExecuteA(nullptr, "open", exePath, nullptr, nullptr, SW_SHOW);
                }
                break;
            case AppMenu::ItemId::NewIncognito:    CreateTab(hwnd, "aeon://newtab?incognito=1"); break;
            case AppMenu::ItemId::Passwords:       OpenOrFocusInternalUrl(ch, hwnd, "aeon://passwords"); break;
            case AppMenu::ItemId::History:         OpenOrFocusInternalUrl(ch, hwnd, "aeon://history"); break;
            case AppMenu::ItemId::Downloads:       OpenOrFocusInternalUrl(ch, hwnd, "aeon://downloads"); break;
            case AppMenu::ItemId::Bookmarks:       OpenOrFocusInternalUrl(ch, hwnd, "aeon://bookmarks"); break;
            case AppMenu::ItemId::Extensions:      OpenOrFocusInternalUrl(ch, hwnd, "aeon://extensions"); break;
            case AppMenu::ItemId::DeleteData:      OpenOrFocusInternalUrl(ch, hwnd, "aeon://history?action=clear"); break;
            case AppMenu::ItemId::FirewallMode:    OpenOrFocusInternalUrl(ch, hwnd, "aeon://shields"); break;
            case AppMenu::ItemId::NetworkSentinel: OpenOrFocusInternalUrl(ch, hwnd, "aeon://diagnostics"); break;
            case AppMenu::ItemId::Print:           break;
            case AppMenu::ItemId::SavePageAs:      break;
            case AppMenu::ItemId::FindInPage:      ToggleFindBar(hwnd); break;
            case AppMenu::ItemId::MoreTools:       OpenOrFocusInternalUrl(ch, hwnd, "aeon://diagnostics"); break;
            case AppMenu::ItemId::DevTools:        OpenOrFocusInternalUrl(ch, hwnd, "aeon://diagnostics"); break;
            case AppMenu::ItemId::Settings:        OpenOrFocusInternalUrl(ch, hwnd, "aeon://settings"); break;
            case AppMenu::ItemId::Help:            OpenOrFocusInternalUrl(ch, hwnd, "aeon://settings#about"); break;
            case AppMenu::ItemId::About:           OpenOrFocusInternalUrl(ch, hwnd, "aeon://settings#about"); break;
            case AppMenu::ItemId::Exit:            PostMessage(hwnd, WM_CLOSE, 0, 0); break;
            default: break;
        }
    };
    AppMenu::Show(hwnd, pt, callback);
}

void SyncViewport(HWND hwnd) {
    ChromeState* ch = reinterpret_cast<ChromeState*>(GetWindowLongPtr(hwnd, GWLP_USERDATA));
    if (ch) SyncViewportInternal(ch);
}

void ShowGroupContextMenu(HWND hwnd, uint32_t groupId, int screenX, int screenY) {
    ChromeState* ch = reinterpret_cast<ChromeState*>(GetWindowLongPtr(hwnd, GWLP_USERDATA));
    if (!ch) return;

    auto& groupMgr = BrowserChrome::TabGroupManager::GetInstance();
    const auto* grp = groupMgr.GetGroup(groupId);
    if (!grp) return;

    HMENU hMenu = CreatePopupMenu();
    if (!hMenu) return;

    AppendMenuA(hMenu, MF_STRING, IDM_GROUP_NEW_TAB, "New Tab in Group");
    AppendMenuA(hMenu, MF_STRING, IDM_GROUP_RENAME, "Rename Group");

    HMENU hColorSub = CreatePopupMenu();
    AppendMenuA(hColorSub, MF_STRING, IDM_GROUP_COLOR_PURPLE, "Purple");
    AppendMenuA(hColorSub, MF_STRING, IDM_GROUP_COLOR_CYAN,   "Cyan");
    AppendMenuA(hColorSub, MF_STRING, IDM_GROUP_COLOR_GREEN,  "Green");
    AppendMenuA(hColorSub, MF_STRING, IDM_GROUP_COLOR_AMBER,  "Amber");
    AppendMenuA(hColorSub, MF_STRING, IDM_GROUP_COLOR_PINK,   "Pink");
    AppendMenuA(hColorSub, MF_STRING, IDM_GROUP_COLOR_RED,    "Red");
    AppendMenuA(hMenu, MF_POPUP, (UINT_PTR)hColorSub, "Change Color");

    AppendMenuA(hMenu, MF_SEPARATOR, 0, nullptr);
    AppendMenuA(hMenu, MF_STRING, IDM_GROUP_UNGROUP, "Ungroup Tabs");
    AppendMenuA(hMenu, MF_STRING, IDM_GROUP_CLOSE,   "Close Group");

    int cmd = TrackPopupMenu(hMenu, TPM_RETURNCMD | TPM_RIGHTBUTTON,
                             screenX, screenY, 0, hwnd, nullptr);
    DestroyMenu(hMenu);

    switch (cmd) {
        case IDM_GROUP_NEW_TAB: {
            unsigned int newId = CreateTab(hwnd, "aeon://newtab");
            groupMgr.AddTabToGroup(newId, groupId);
            for (auto& t : ch->tabs) {
                if (t.id == newId) { t.groupId = groupId; break; }
            }
            PaintChrome(ch);
            break;
        }
        case IDM_GROUP_RENAME: {
            static int renameCount = 1;
            std::string newName = grp->name + " " + std::to_string(++renameCount);
            groupMgr.RenameGroup(groupId, newName);
            PaintChrome(ch);
            break;
        }
        case IDM_GROUP_COLOR_PURPLE: groupMgr.SetGroupColor(groupId, BrowserChrome::GROUP_COLOR_PURPLE); PaintChrome(ch); break;
        case IDM_GROUP_COLOR_CYAN:   groupMgr.SetGroupColor(groupId, BrowserChrome::GROUP_COLOR_CYAN);   PaintChrome(ch); break;
        case IDM_GROUP_COLOR_GREEN:  groupMgr.SetGroupColor(groupId, BrowserChrome::GROUP_COLOR_GREEN);  PaintChrome(ch); break;
        case IDM_GROUP_COLOR_AMBER:  groupMgr.SetGroupColor(groupId, BrowserChrome::GROUP_COLOR_AMBER);  PaintChrome(ch); break;
        case IDM_GROUP_COLOR_PINK:   groupMgr.SetGroupColor(groupId, BrowserChrome::GROUP_COLOR_PINK);   PaintChrome(ch); break;
        case IDM_GROUP_COLOR_RED:    groupMgr.SetGroupColor(groupId, BrowserChrome::GROUP_COLOR_RED);    PaintChrome(ch); break;
        case IDM_GROUP_UNGROUP: {
            for (auto& t : ch->tabs) {
                if (t.groupId == groupId) t.groupId = 0;
            }
            groupMgr.DeleteGroup(groupId);
            PaintChrome(ch);
            break;
        }
        case IDM_GROUP_CLOSE: {
            std::vector<unsigned int> toClose;
            for (const auto& t : ch->tabs) {
                if (t.groupId == groupId) toClose.push_back(t.id);
            }
            groupMgr.DeleteGroup(groupId);
            for (unsigned int tid : toClose) {
                CloseTabById(hwnd, tid);
            }
            PaintChrome(ch);
            break;
        }
    }
}

void ShowTabContextMenu(HWND hwnd, int tabIndex, int screenX, int screenY) {
    ChromeState* ch = reinterpret_cast<ChromeState*>(GetWindowLongPtr(hwnd, GWLP_USERDATA));
    if (!ch || tabIndex < 0 || tabIndex >= (int)ch->tabs.size()) return;

    HMENU hMenu = CreatePopupMenu();
    if (!hMenu) return;

    auto& groupMgr = BrowserChrome::TabGroupManager::GetInstance();
    auto allGroups = groupMgr.GetAllGroups();

    HMENU hGroupSub = CreatePopupMenu();
    AppendMenuA(hGroupSub, MF_STRING, IDM_TAB_NEW_GROUP, "New Tab Group...");
    if (!allGroups.empty()) {
        AppendMenuA(hGroupSub, MF_SEPARATOR, 0, nullptr);
        for (size_t gIdx = 0; gIdx < allGroups.size(); gIdx++) {
            AppendMenuA(hGroupSub, MF_STRING, IDM_TAB_ADD_TO_GROUP_BASE + (UINT)gIdx, allGroups[gIdx].name.c_str());
        }
    }
    AppendMenuA(hMenu, MF_POPUP, (UINT_PTR)hGroupSub, "Add to Group");

    if (ch->tabs[tabIndex].groupId > 0) {
        AppendMenuA(hMenu, MF_STRING, IDM_TAB_REMOVE_GROUP, "Remove from Group");
    }

    AppendMenuA(hMenu, MF_SEPARATOR, 0, nullptr);
    if (!ch->splitView.IsEnabled()) {
        HMENU hSplitSub = CreatePopupMenu();
        AppendMenuA(hSplitSub, MF_STRING, IDM_SPLIT_NEW_TAB, "With New Tab");
        if (ch->tabs.size() > 1) {
            AppendMenuA(hSplitSub, MF_SEPARATOR, 0, nullptr);
            for (int i = 0; i < (int)ch->tabs.size(); ++i) {
                if (i == tabIndex) continue;
                std::string tTitle = ch->tabs[i].title.empty() ? "Tab " + std::to_string(i + 1) : ch->tabs[i].title;
                if (tTitle.length() > 30) tTitle = tTitle.substr(0, 27) + "...";
                AppendMenuA(hSplitSub, MF_STRING, IDM_SPLIT_WITH_BASE + i, tTitle.c_str());
            }
        }
        AppendMenuA(hMenu, MF_POPUP, (UINT_PTR)hSplitSub, "Split View (Side-by-Side)");
    } else {
        AppendMenuW(hMenu, MF_STRING, IDM_SPLIT_SWAP, L"\u21C4  Swap Split Panes\tCtrl+Alt+X");
        int pri = ch->splitView.GetPrimaryTabIndex();
        int sec = ch->splitView.GetSecondaryTabIndex();
        if (tabIndex != pri && tabIndex != sec) {
            AppendMenuA(hMenu, MF_STRING, IDM_SPLIT_SET_LEFT_BASE + tabIndex, "Put in Left Split Pane");
            AppendMenuA(hMenu, MF_STRING, IDM_SPLIT_SET_RIGHT_BASE + tabIndex, "Put in Right Split Pane");
        } else {
            AppendMenuW(hMenu, MF_STRING, IDM_SPLIT_SOLO_ACTIVE, L"\u2922  Expand to Full Width (Exit Split)");
        }
        AppendMenuW(hMenu, MF_STRING, IDM_TAB_SPLITVIEW, L"\u2715  Exit Split View");
    }
    AppendMenuA(hMenu, MF_STRING, IDM_TAB_DUPLICATE, "Duplicate Tab");
    AppendMenuA(hMenu, MF_STRING, IDM_TAB_PIN, ch->tabs[tabIndex].isPinned ? "Unpin Tab" : "Pin Tab");
    AppendMenuA(hMenu, MF_STRING | (ch->verticalSidebar ? MF_CHECKED : 0),
                IDM_TAB_TOGGLE_SIDEBAR, "Arc Vertical Sidebar\tCtrl+S");

    bool isSleeping = ch->tabs[tabIndex].isSleeping || TabSleepManager::IsTabSleeping(ch->tabs[tabIndex].id);
    AppendMenuA(hMenu, MF_STRING, IDM_TAB_SLEEP_NOW, isSleeping ? "Wake Tab" : "Sleep Tab Now (Save RAM)");

    AppendMenuA(hMenu, MF_SEPARATOR, 0, nullptr);
    AppendMenuA(hMenu, MF_STRING, IDM_TAB_CLOSE_OTHERS, "Close Other Tabs");
    AppendMenuA(hMenu, MF_STRING, IDM_TAB_CLOSE, "Close Tab\tCtrl+W");

    int cmd = TrackPopupMenu(hMenu, TPM_RETURNCMD | TPM_RIGHTBUTTON,
                             screenX, screenY, 0, hwnd, nullptr);
    DestroyMenu(hMenu);

    if (cmd >= IDM_TAB_ADD_TO_GROUP_BASE && cmd < IDM_TAB_ADD_TO_GROUP_BASE + (int)allGroups.size()) {
        size_t selectedGrpIdx = cmd - IDM_TAB_ADD_TO_GROUP_BASE;
        uint32_t gid = allGroups[selectedGrpIdx].id;
        AddTabToGroup(hwnd, tabIndex, gid);
        return;
    }
    if (cmd >= IDM_SPLIT_SET_LEFT_BASE && cmd < IDM_SPLIT_SET_LEFT_BASE + (int)ch->tabs.size()) {
        int targetIdx = cmd - IDM_SPLIT_SET_LEFT_BASE;
        ch->splitView.SetPrimaryTabIndex(targetIdx);
        SyncViewportInternal(ch);
        InvalidateRect(hwnd, nullptr, FALSE);
        PaintChrome(ch);
        return;
    }
    if (cmd >= IDM_SPLIT_SET_RIGHT_BASE && cmd < IDM_SPLIT_SET_RIGHT_BASE + (int)ch->tabs.size()) {
        int targetIdx = cmd - IDM_SPLIT_SET_RIGHT_BASE;
        ch->splitView.SetSecondaryTabIndex(targetIdx);
        SyncViewportInternal(ch);
        InvalidateRect(hwnd, nullptr, FALSE);
        PaintChrome(ch);
        return;
    }
    if (cmd >= IDM_SPLIT_WITH_BASE && cmd < IDM_SPLIT_WITH_BASE + (int)ch->tabs.size()) {
        int secIdx = cmd - IDM_SPLIT_WITH_BASE;
        ch->splitView.Enable(tabIndex, secIdx, 0.50f);
        ch->splitView.SetActivePane(0);
        SyncViewportInternal(ch);
        InvalidateRect(hwnd, nullptr, FALSE);
        PaintChrome(ch);
        return;
    }

    switch (cmd) {
        case IDM_TAB_NEW_GROUP: {
            uint32_t gid = CreateTabGroup(hwnd, ("Group " + std::to_string(groupMgr.GetGroupCount() + 1)).c_str());
            AddTabToGroup(hwnd, tabIndex, gid);
            break;
        }
        case IDM_TAB_REMOVE_GROUP:
            RemoveTabFromGroup(hwnd, tabIndex);
            break;
        case IDM_TAB_SPLITVIEW:
            ToggleSplitView(hwnd);
            break;
        case IDM_SPLIT_SWAP:
            SwapSplitPanes(hwnd);
            break;
        case IDM_SPLIT_SOLO_ACTIVE:
            SoloSplitPane(hwnd);
            break;
        case IDM_SPLIT_NEW_TAB: {
            int cur = tabIndex;
            CreateTab(hwnd, "aeon://newtab");
            ch->splitView.Enable(cur, ch->activeTab, 0.50f);
            ch->splitView.SetActivePane(0);
            SyncViewportInternal(ch);
            InvalidateRect(hwnd, nullptr, FALSE);
            PaintChrome(ch);
            break;
        }
        case IDM_TAB_DUPLICATE:
            CreateTab(hwnd, ch->tabs[tabIndex].url.c_str());
            break;
        case IDM_TAB_PIN: {
            ch->tabs[tabIndex].isPinned = !ch->tabs[tabIndex].isPinned;
            TabSleepManager::SetTabPinned(ch->tabs[tabIndex].id, ch->tabs[tabIndex].isPinned);
            PaintChrome(ch);
            break;
        }
        case IDM_TAB_TOGGLE_SIDEBAR:
            ToggleSidebar(hwnd);
            break;
        case IDM_TAB_SLEEP_NOW: {
            if (isSleeping) {
                WakeTab(hwnd, tabIndex);
            } else {
                SuspendTab(hwnd, tabIndex);
            }
            break;
        }
        case IDM_TAB_CLOSE_OTHERS: {
            unsigned int keepId = ch->tabs[tabIndex].id;
            std::vector<unsigned int> toClose;
            for (const auto& t : ch->tabs) {
                if (t.id != keepId) toClose.push_back(t.id);
            }
            for (unsigned int tid : toClose) {
                CloseTabById(hwnd, tid);
            }
            break;
        }
        case IDM_TAB_CLOSE:
            CloseTab(hwnd, tabIndex);
            break;
    }
}

void OnLButtonDown(HWND hwnd, int x, int y) {
    ChromeState* ch = reinterpret_cast<ChromeState*>(
        GetWindowLongPtr(hwnd, GWLP_USERDATA));
    if (!ch) return;

    if (AeonOmni::IsVisible()) {
        AeonOmni::Hide();
    }

    int hit = HitTest(hwnd, x, y);
    if (ch->splitView.IsEnabled() && !ch->isFullscreen) {
        if (hit == HIT_SPLIT_SWAP) {
            SwapSplitPanes(hwnd);
            return;
        }
        if (hit == HIT_SPLITTER || ch->splitView.HitTestSplitter(x, y)) {
            ch->splitView.StartDrag(x);
            SetCapture(hwnd);
            SetCursor(LoadCursor(nullptr, IDC_SIZEWE));
            PaintChrome(ch);
            return;
        }
    }

    if (hit != 5 && hit != 6 && ch->urlFocused && ch->hUrlBar) {
        ShowWindow(ch->hUrlBar, SW_HIDE);
        ch->urlFocused = false;
        PaintChrome(ch);
    }

    int topContentY = GetTopChromeHeight(ch);
    int sidebarW = GetEffectiveSidebarWidth(ch);
    if (hit == -1 && y >= topContentY && x >= sidebarW && ch->splitView.IsEnabled() && !ch->isFullscreen) {
        RECT rc; GetClientRect(hwnd, &rc);
        int pane = ch->splitView.HitTestPane(x, y, topContentY, rc.right, rc.bottom);
        if (pane == 0 && ch->splitView.GetActivePane() != 0) {
            ch->splitView.SetActivePane(0);
            SelectTab(hwnd, ch->splitView.GetPrimaryTabIndex());
        } else if (pane == 1 && ch->splitView.GetActivePane() != 1) {
            ch->splitView.SetActivePane(1);
            SelectTab(hwnd, ch->splitView.GetSecondaryTabIndex());
        }
        return;
    }

    int effectiveTab = ch->splitView.IsEnabled() ? ch->splitView.GetActiveTabIndex() : ch->activeTab;

    switch (hit) {
        case 1:
            if (ch->engine && effectiveTab >= 0 && effectiveTab < (int)ch->tabs.size()) {
                auto& t = ch->tabs[effectiveTab];
                if (t.historyIndex > 0) ch->engine->GoBack(t.id);
            }
            break;
        case 2:
            if (ch->engine && effectiveTab >= 0 && effectiveTab < (int)ch->tabs.size()) {
                auto& t = ch->tabs[effectiveTab];
                if (t.historyIndex < (int)t.history.size() - 1) ch->engine->GoForward(t.id);
            }
            break;
        case 3:
            if (ch->engine && effectiveTab >= 0 && effectiveTab < (int)ch->tabs.size()) {
                auto& t = ch->tabs[effectiveTab];
                if (t.loading) {
                    ch->engine->Stop(t.id);
                    t.loading = false;
                    t.progressPercent = 0;
                    t.animProgress = 0.0f;
                    bool anyStillActive = false;
                    for (const auto& tab : ch->tabs) {
                        if (tab.loading || tab.animProgress > 0.0f) {
                            anyStillActive = true;
                            break;
                        }
                    }
                    if (!anyStillActive) {
                        KillTimer(hwnd, ID_LOADING_TIMER);
                    }
                    PaintChrome(ch);
                } else {
                    std::string mapped = ReverseMapUrl(t.url);
                    if (mapped != "aeon://newtab" && t.url != "about:blank" && !t.url.empty()) {
                        t.loading = true;
                        t.progressPercent = 25;
                        t.animProgress = 10.0f;
                        SetTimer(hwnd, ID_LOADING_TIMER, LOADING_TIMER_MS, nullptr);
                    }
                    ch->engine->Reload(t.id, 0);
                    PaintChrome(ch);
                }
            }
            break;
        case 4:
            NavigateActiveTab(ch, "aeon://newtab");
            break;
        case 5:
        case 6:
            ActivateUrlBar(ch);
            break;
        case 10:
            ShowWindow(hwnd, SW_MINIMIZE);
            break;
        case 11:
            ShowWindow(hwnd, IsZoomed(hwnd) ? SW_RESTORE : SW_MAXIMIZE);
            break;
        case 12:
            PostMessage(hwnd, WM_CLOSE, 0, 0);
            break;
        case 50:
            CreateTab(hwnd, "aeon://newtab");
            break;
        case 51:
            ToggleSidebar(hwnd);
            break;
        case 20:
            OpenOrFocusInternalUrl(ch, hwnd, "aeon://downloads");
            break;
        case 21:
            TriggerBookmarkToast(hwnd);
            break;
        case 22:
            TriggerShieldHUD(hwnd);
            break;
        case 23: {
            std::string targetUrl = "aeon://intelligence";
            if (effectiveTab >= 0 && effectiveTab < (int)ch->tabs.size()) {
                const std::string& currentUrl = ch->tabs[effectiveTab].url;
                if (!currentUrl.empty() && currentUrl.find("aeon://") != 0) {
                    std::string encoded;
                    for (char c : currentUrl) {
                        if (c == ' ') encoded += "+";
                        else if (c == '+') encoded += "%2B";
                        else if (c == '&') encoded += "%26";
                        else if (c == '?') encoded += "%3F";
                        else if (c == '#') encoded += "%23";
                        else if (c == '%') encoded += "%25";
                        else encoded += c;
                    }
                    targetUrl = "aeon://intelligence?target=" + encoded;
                }
            }
            OpenOrFocusInternalUrl(ch, hwnd, targetUrl.c_str());
            break;
        }
        case 24: {
            if (ch->splitView.IsEnabled()) {
                RECT rc; GetClientRect(hwnd, &rc);
                POINT ptMenu = { rc.right - S(ch, 90), S(ch, CHROME_H) };
                ClientToScreen(hwnd, &ptMenu);
                ShowSplitViewMenu(hwnd, ptMenu.x, ptMenu.y);
            } else {
                ToggleSplitView(hwnd);
            }
            break;
        }
        case 25:
            TriggerAppMenu(hwnd);
            break;
        default:
            if (hit >= 600) {
                int idx = hit - 600;
                ToggleTabMute(hwnd, idx);
            } else if (hit >= 400) {
                uint32_t gid = (uint32_t)(hit - 400);
                ToggleGroupCollapsed(hwnd, gid);
            } else if (hit >= 300) {
                uint32_t gid = (uint32_t)(hit - 300);
                ToggleGroupCollapsed(hwnd, gid);
            } else if (hit >= 200) {
                int idx = hit - 200;
                CloseTab(hwnd, idx);
            } else if (hit >= 100) {
                int idx = hit - 100;
                ch->dragTabIndex = idx;
                ch->dragStartX = x;
                ch->dragStartY = y;
                ch->isDraggingTab = false;
                SetCapture(hwnd);
                SelectTab(hwnd, idx);
            }
            break;
    }
}

void OnLButtonUp(HWND hwnd, int x, int y) {
    ChromeState* ch = reinterpret_cast<ChromeState*>(
        GetWindowLongPtr(hwnd, GWLP_USERDATA));
    if (!ch) return;

    if (ch->isDraggingTab || ch->dragTabIndex >= 0) {
        ch->isDraggingTab = false;
        ch->dragTabIndex = -1;
        ReleaseCapture();
        PaintChrome(ch);
    }

    if (ch->splitView.IsDragging()) {
        ch->splitView.EndDrag();
        ReleaseCapture();
        SyncViewportInternal(ch);
        PaintChrome(ch);
    }
}

void OnMouseMove(HWND hwnd, int x, int y) {
    ChromeState* ch = reinterpret_cast<ChromeState*>(
        GetWindowLongPtr(hwnd, GWLP_USERDATA));
    if (!ch) return;

    if (ch->dragTabIndex >= 0 && (GetKeyState(VK_LBUTTON) & 0x8000)) {
        int delta = ch->verticalSidebar ? abs(y - ch->dragStartY) : abs(x - ch->dragStartX);
        if (!ch->isDraggingTab && delta > S(ch, 6)) {
            ch->isDraggingTab = true;
        }
        if (ch->isDraggingTab) {
            int dpiVal = (ch->dpi > 0) ? ch->dpi : 96;
            int testX = ch->verticalSidebar ? x : MulDiv(x, 96, dpiVal);
            int testY = y;
            for (int j = 0; j < (int)ch->tabs.size(); ++j) {
                if (j == ch->dragTabIndex) continue;
                const RECT& r = ch->tabs[j].tabRect;
                bool hitTarget = ch->verticalSidebar
                    ? (r.bottom > r.top && testY >= r.top && testY <= r.bottom)
                    : (r.right > r.left && testX >= r.left && testX <= r.right);
                if (hitTarget) {
                    std::swap(ch->tabs[ch->dragTabIndex], ch->tabs[j]);
                    if (ch->activeTab == ch->dragTabIndex) ch->activeTab = j;
                    else if (ch->activeTab == j) ch->activeTab = ch->dragTabIndex;
                    ch->dragTabIndex = j;
                    PaintChrome(ch);
                    break;
                }
            }
            return;
        }
    }

    if (ch->splitView.IsDragging()) {
        RECT rc; GetClientRect(hwnd, &rc);
        if (ch->splitView.UpdateDrag(x, rc.right, GetEffectiveSidebarWidth(ch))) {
            SyncViewportInternal(ch);
            PaintChrome(ch);
        }
        SetCursor(LoadCursor(nullptr, IDC_SIZEWE));
        return;
    }

    int hit = HitTest(hwnd, x, y);
    if (ch->splitView.IsEnabled() && !ch->isFullscreen) {
        bool overSplitter = (hit == HIT_SPLITTER || hit == HIT_SPLIT_SWAP || ch->splitView.HitTestSplitter(x, y));
        ch->splitView.SetHovered(overSplitter);
        if (hit == HIT_SPLIT_SWAP) {
            SetCursor(LoadCursor(nullptr, IDC_HAND));
        } else if (overSplitter) {
            SetCursor(LoadCursor(nullptr, IDC_SIZEWE));
        }
    }

    if (!ch->trackingMouse) {
        TRACKMOUSEEVENT tme = {};
        tme.cbSize = sizeof(tme);
        tme.dwFlags = TME_LEAVE;
        tme.hwndTrack = hwnd;
        TrackMouseEvent(&tme);
        ch->trackingMouse = true;
    }

    int oldHover = ch->hoverTab;
    int oldBtn   = ch->hoverBtn;
    ch->hoverTab = -1;
    ch->hoverBtn = -1;

    if (hit >= 200 && hit < 300) {
        ch->hoverBtn = hit;
        ch->hoverTab = hit - 200;
    } else if (hit >= 100 && hit < 200) {
        ch->hoverBtn = hit;
        ch->hoverTab = hit - 100;
    } else if (hit >= 0) {
        ch->hoverBtn = hit;
    }

    if (ch->hoverTab != oldHover || ch->hoverBtn != oldBtn) {
        PaintChrome(ch);
    }
}

void OnMouseLeave(HWND hwnd) {
    ChromeState* ch = reinterpret_cast<ChromeState*>(
        GetWindowLongPtr(hwnd, GWLP_USERDATA));
    if (!ch) return;

    ch->hoverTab = -1;
    ch->hoverBtn = -1;
    ch->trackingMouse = false;
    ch->splitView.SetHovered(false);
    PaintChrome(ch);
}

void OnCommand(HWND hwnd, int id, int notifyCode, HWND ctlHwnd) {
    ChromeState* ch = reinterpret_cast<ChromeState*>(
        GetWindowLongPtr(hwnd, GWLP_USERDATA));
    if (!ch) return;

    if (id == IDC_URLBAR) {
        if (notifyCode == EN_CHANGE) {
            if (!g_bProgrammaticUrlChange && ctlHwnd) {
                TriggerOmniSuggestions(ch, ctlHwnd);
            }
        } else if (notifyCode == EN_CHANGE + 100) {
            CommitUrlBar(ch);
        }
    }
}

bool OnKeyDown(HWND hwnd, WPARAM vk, LPARAM lParam) {
    ChromeState* ch = reinterpret_cast<ChromeState*>(
        GetWindowLongPtr(hwnd, GWLP_USERDATA));
    if (!ch) return false;

    bool ctrl  = (GetKeyState(VK_CONTROL) & 0x8000) != 0;
    bool shift = (GetKeyState(VK_SHIFT)   & 0x8000) != 0;
    bool alt   = (GetKeyState(VK_MENU)    & 0x8000) != 0;
    (void)lParam;

    if ((ctrl && alt && vk == 'S') || (ctrl && !shift && !alt && vk == VK_OEM_5)) {
        ToggleSplitView(hwnd);
        return true;
    }

    if (ctrl && alt && vk == 'X') {
        if (ch->splitView.IsEnabled()) {
            SwapSplitPanes(hwnd);
            return true;
        }
    }

    if (ctrl && alt && vk == VK_LEFT) {
        if (ch->splitView.IsEnabled()) {
            SetSplitViewActivePane(hwnd, 0);
            return true;
        }
    }

    if (ctrl && alt && vk == VK_RIGHT) {
        if (ch->splitView.IsEnabled()) {
            SetSplitViewActivePane(hwnd, 1);
            return true;
        }
    }

    if (ctrl && !shift && !alt && vk == 'T') {
        CreateTab(hwnd, "aeon://newtab");
        return true;
    }

    if (ctrl && !shift && !alt && vk == 'F') {
        ToggleFindBar(hwnd);
        return true;
    }

    if (vk == VK_ESCAPE && ch->findBarVisible) {
        CloseFindBar(hwnd);
        return true;
    }

    if (ctrl && !alt && (vk == VK_OEM_PLUS || vk == VK_ADD || vk == '=')) {
        ZoomIn(hwnd);
        return true;
    }

    if (ctrl && !alt && (vk == VK_OEM_MINUS || vk == VK_SUBTRACT || vk == '-')) {
        ZoomOut(hwnd);
        return true;
    }

    if (ctrl && !alt && (vk == '0' || vk == VK_NUMPAD0)) {
        ZoomReset(hwnd);
        return true;
    }

    if (ctrl && shift && !alt && vk == 'B') {
        ToggleBookmarkBar(hwnd);
        return true;
    }

    if (ctrl && !shift && !alt && vk == 'M') {
        int eff = ch->splitView.IsEnabled() ? ch->splitView.GetActiveTabIndex() : ch->activeTab;
        if (eff >= 0 && eff < (int)ch->tabs.size()) {
            ToggleTabMute(hwnd, eff);
        }
        return true;
    }

    if (ctrl && !shift && !alt && vk == 'D') {
        TriggerBookmarkToast(hwnd);
        return true;
    }

    if (ctrl && !shift && !alt && vk == 'K') {
        ToggleCommandBar(hwnd);
        return true;
    }

    if (ctrl && !shift && !alt && vk == 'S') {
        ToggleSidebar(hwnd);
        return true;
    }

    if (ctrl && !shift && !alt && vk == 'H') {
        OpenOrFocusInternalUrl(ch, hwnd, "aeon://history");
        return true;
    }

    if (ctrl && !shift && !alt && vk == 'J') {
        OpenOrFocusInternalUrl(ch, hwnd, "aeon://downloads");
        return true;
    }

    if (ctrl && shift && !alt && vk == 'O') {
        OpenOrFocusInternalUrl(ch, hwnd, "aeon://bookmarks");
        return true;
    }

    if ((ctrl && !shift && !alt && vk == VK_OEM_COMMA) || (alt && !ctrl && vk == 'P')) {
        OpenOrFocusInternalUrl(ch, hwnd, "aeon://settings");
        return true;
    }

    if (ctrl && !shift && !alt && vk == 'W') {
        int effectiveTab = ch->splitView.IsEnabled() ? ch->splitView.GetActiveTabIndex() : ch->activeTab;
        if (effectiveTab >= 0 && effectiveTab < (int)ch->tabs.size()) {
            CloseTab(hwnd, effectiveTab);
        }
        return true;
    }

    if ((ctrl && !shift && !alt && vk == 'L') || vk == VK_F6) {
        ActivateUrlBar(ch);
        return true;
    }

    if ((ctrl && !shift && !alt && vk == 'R') || vk == VK_F5) {
        int effectiveTab = ch->splitView.IsEnabled() ? ch->splitView.GetActiveTabIndex() : ch->activeTab;
        if (ch->engine && effectiveTab >= 0 && effectiveTab < (int)ch->tabs.size()) {
            std::string mapped = ReverseMapUrl(ch->tabs[effectiveTab].url);
            if (mapped != "aeon://newtab" && ch->tabs[effectiveTab].url != "about:blank" && !ch->tabs[effectiveTab].url.empty()) {
                ch->tabs[effectiveTab].loading = true;
                ch->tabs[effectiveTab].progressPercent = 25;
                ch->tabs[effectiveTab].animProgress = 10.0f;
                SetTimer(ch->hwnd, ID_LOADING_TIMER, LOADING_TIMER_MS, nullptr);
            }
            ch->engine->Reload(ch->tabs[effectiveTab].id, 0);
            PaintChrome(ch);
        }
        return true;
    }

    if ((ctrl && shift && !alt && vk == 'R') || (shift && vk == VK_F5)) {
        int effectiveTab = ch->splitView.IsEnabled() ? ch->splitView.GetActiveTabIndex() : ch->activeTab;
        if (ch->engine && effectiveTab >= 0 && effectiveTab < (int)ch->tabs.size()) {
            std::string mapped = ReverseMapUrl(ch->tabs[effectiveTab].url);
            if (mapped != "aeon://newtab" && ch->tabs[effectiveTab].url != "about:blank" && !ch->tabs[effectiveTab].url.empty()) {
                ch->tabs[effectiveTab].loading = true;
                ch->tabs[effectiveTab].progressPercent = 25;
                ch->tabs[effectiveTab].animProgress = 10.0f;
                SetTimer(ch->hwnd, ID_LOADING_TIMER, LOADING_TIMER_MS, nullptr);
            }
            ch->engine->Reload(ch->tabs[effectiveTab].id, 1);
            PaintChrome(ch);
        }
        return true;
    }

    if (ctrl && !shift && !alt && vk == VK_TAB) {
        NextTab(hwnd);
        return true;
    }

    if (ctrl && shift && !alt && vk == VK_TAB) {
        PrevTab(hwnd);
        return true;
    }

    if (ctrl && !shift && !alt && vk >= '1' && vk <= '8') {
        int idx = (int)(vk - '1');
        if (idx >= 0 && idx < (int)ch->tabs.size()) {
            SelectTab(hwnd, idx);
        }
        return true;
    }

    if (ctrl && !shift && !alt && vk == '9') {
        if (!ch->tabs.empty()) {
            SelectTab(hwnd, (int)ch->tabs.size() - 1);
        }
        return true;
    }

    if (alt && !ctrl && vk == VK_LEFT) {
        int effectiveTab = ch->splitView.IsEnabled() ? ch->splitView.GetActiveTabIndex() : ch->activeTab;
        if (ch->engine && effectiveTab >= 0 && effectiveTab < (int)ch->tabs.size())
            ch->engine->GoBack(ch->tabs[effectiveTab].id);
        return true;
    }

    if (alt && !ctrl && vk == VK_RIGHT) {
        int effectiveTab = ch->splitView.IsEnabled() ? ch->splitView.GetActiveTabIndex() : ch->activeTab;
        if (ch->engine && effectiveTab >= 0 && effectiveTab < (int)ch->tabs.size())
            ch->engine->GoForward(ch->tabs[effectiveTab].id);
        return true;
    }

    if (vk == VK_F11) {
        ch->isFullscreen = !ch->isFullscreen;
        RECT rc; GetClientRect(hwnd, &rc);
        OnSize(hwnd, rc.right, rc.bottom);
        return true;
    }

    return false;
}

HBRUSH OnCtlColorEdit(HWND hwnd, HDC hdc, HWND hEdit) {
    ChromeState* ch = reinterpret_cast<ChromeState*>(
        GetWindowLongPtr(hwnd, GWLP_USERDATA));
    if (!ch || hEdit != ch->hUrlBar) return nullptr;

    SetTextColor(hdc, RGB(232, 232, 240));
    SetBkColor(hdc, RGB(20, 23, 43));
    return ch->hUrlBgBrush;
}

#define IDM_CTX_BACK     40001
#define IDM_CTX_FORWARD  40002
#define IDM_CTX_RELOAD   40003
#define IDM_CTX_NEWTAB   40004
#define IDM_CTX_VIEWSRC  40005
#define IDM_CTX_INSPECT  40006

void OnContextMenu(HWND hwnd, int screenX, int screenY) {
    ChromeState* ch = reinterpret_cast<ChromeState*>(
        GetWindowLongPtr(hwnd, GWLP_USERDATA));
    if (!ch) return;

    POINT pt = { screenX, screenY };
    ScreenToClient(hwnd, &pt);

    int topChromeH = S(ch, CHROME_H);
    int topContentY = GetTopChromeHeight(ch);
    int sidebarW = GetEffectiveSidebarWidth(ch);

    if (!ch->isFullscreen) {
        int hit = HitTest(hwnd, pt.x, pt.y);
        if (hit >= 300 && hit < 500) {
            uint32_t gid = (hit >= 400) ? (uint32_t)(hit - 400) : (uint32_t)(hit - 300);
            ShowGroupContextMenu(hwnd, gid, screenX, screenY);
            return;
        } else if (hit >= 100 && hit < 200) {
            int tabIdx = hit - 100;
            ShowTabContextMenu(hwnd, tabIdx, screenX, screenY);
            return;
        } else if (hit >= 200 && hit < 300) {
            int tabIdx = hit - 200;
            ShowTabContextMenu(hwnd, tabIdx, screenX, screenY);
            return;
        } else if (hit == 24 || hit == HIT_SPLITTER || hit == HIT_SPLIT_SWAP) {
            ShowSplitViewMenu(hwnd, screenX, screenY);
            return;
        }

        if (pt.y < topChromeH || (sidebarW > 0 && pt.x < sidebarW && pt.y >= topContentY)) {
            return;
        }
    }

    if (ch->splitView.IsEnabled() && ch->splitView.HitTestSplitter(pt.x, pt.y)) {
        ShowSplitViewMenu(hwnd, screenX, screenY);
        return;
    }

    HMENU hMenu = CreatePopupMenu();
    if (!hMenu) return;

    int effectiveTab = ch->splitView.IsEnabled() ? ch->splitView.GetActiveTabIndex() : ch->activeTab;
    bool hasBack = (effectiveTab >= 0 && effectiveTab < (int)ch->tabs.size() &&
                    ch->tabs[effectiveTab].historyIndex > 0);
    bool hasFwd  = (effectiveTab >= 0 && effectiveTab < (int)ch->tabs.size() &&
                    ch->tabs[effectiveTab].historyIndex < (int)ch->tabs[effectiveTab].history.size() - 1);

    AppendMenuA(hMenu, MF_STRING | (hasBack ? 0 : MF_GRAYED), IDM_CTX_BACK, "&Back");
    AppendMenuA(hMenu, MF_STRING | (hasFwd ? 0 : MF_GRAYED), IDM_CTX_FORWARD, "&Forward");
    AppendMenuA(hMenu, MF_STRING, IDM_CTX_RELOAD, "&Reload\tF5");
    AppendMenuA(hMenu, MF_SEPARATOR, 0, nullptr);
    AppendMenuA(hMenu, MF_STRING, IDM_CTX_NEWTAB, "&New Tab\tCtrl+T");
    AppendMenuA(hMenu, MF_SEPARATOR, 0, nullptr);
    AppendMenuA(hMenu, MF_STRING, IDM_CTX_VIEWSRC, "View Page &Source");
    AppendMenuA(hMenu, MF_STRING, IDM_CTX_INSPECT, "&Inspect (DevTools)");

    int cmd = TrackPopupMenu(hMenu, TPM_RETURNCMD | TPM_RIGHTBUTTON,
                             screenX, screenY, 0, hwnd, nullptr);
    DestroyMenu(hMenu);

    switch (cmd) {
        case IDM_CTX_BACK:
            if (ch->engine && effectiveTab >= 0 && effectiveTab < (int)ch->tabs.size())
                ch->engine->GoBack(ch->tabs[effectiveTab].id);
            break;
        case IDM_CTX_FORWARD:
            if (ch->engine && effectiveTab >= 0 && effectiveTab < (int)ch->tabs.size())
                ch->engine->GoForward(ch->tabs[effectiveTab].id);
            break;
        case IDM_CTX_RELOAD:
            if (ch->engine && effectiveTab >= 0 && effectiveTab < (int)ch->tabs.size())
                ch->engine->Reload(ch->tabs[effectiveTab].id, 0);
            break;
        case IDM_CTX_NEWTAB:
            CreateTab(hwnd, "aeon://newtab");
            break;
        case IDM_CTX_VIEWSRC: {
            if (effectiveTab >= 0 && effectiveTab < (int)ch->tabs.size()) {
                std::string srcUrl = "view-source:" + ch->tabs[effectiveTab].url;
                CreateTab(hwnd, srcUrl.c_str());
            }
            break;
        }
        case IDM_CTX_INSPECT:
            CreateTab(hwnd, "http://localhost:9222");
            break;
    }
}

void UpdateTabTitle(HWND hwnd, unsigned int tab_id, const char* title) {
    ChromeState* ch = reinterpret_cast<ChromeState*>(
        GetWindowLongPtr(hwnd, GWLP_USERDATA));
    if (!ch) return;
    for (auto& t : ch->tabs) {
        if (t.id == tab_id) {
            t.title = title ? title : "";
            break;
        }
    }
    InvalidateRect(hwnd, nullptr, FALSE);
    PaintChrome(ch);
}

void UpdateTabUrl(HWND hwnd, unsigned int tab_id, const char* url) {
    ChromeState* ch = reinterpret_cast<ChromeState*>(
        GetWindowLongPtr(hwnd, GWLP_USERDATA));
    if (!ch) return;
    std::string mapped = ReverseMapUrl(url ? url : "");
    for (auto& t : ch->tabs) {
        if (t.id == tab_id) {
            t.url = mapped;
            if (t.history.empty()) {
                t.history.push_back(mapped);
                t.historyIndex = 0;
            } else {
                if (t.historyIndex - 1 >= 0 && t.history[t.historyIndex - 1] == mapped) {
                    t.historyIndex--;
                } else if (t.historyIndex + 1 < (int)t.history.size() && t.history[t.historyIndex + 1] == mapped) {
                    t.historyIndex++;
                } else if (t.historyIndex < (int)t.history.size() && t.history[t.historyIndex] != mapped) {
                    t.history.erase(t.history.begin() + t.historyIndex + 1, t.history.end());
                    t.history.push_back(mapped);
                    t.historyIndex = (int)t.history.size() - 1;
                }
            }
            break;
        }
    }
    InvalidateRect(hwnd, nullptr, FALSE);
    PaintChrome(ch);
}

void SetTabLoaded(HWND hwnd, unsigned int tab_id) {
    ChromeState* ch = reinterpret_cast<ChromeState*>(
        GetWindowLongPtr(hwnd, GWLP_USERDATA));
    if (!ch) return;
    for (auto& t : ch->tabs) {
        if (t.id == tab_id) {
            t.loading = false;
            std::string mapped = ReverseMapUrl(t.url);
            if (mapped == "aeon://newtab" || t.url == "about:blank" || t.url.empty() || t.animProgress <= 0.0f) {
                t.progressPercent = 0;
                t.animProgress = 0.0f;
            } else {
                t.progressPercent = 100;
                SetTimer(hwnd, ID_LOADING_TIMER, LOADING_TIMER_MS, nullptr);
            }
            break;
        }
    }
    PaintChrome(ch);
}

void SetTabProgress(HWND hwnd, unsigned int tab_id, int percent) {
    ChromeState* ch = reinterpret_cast<ChromeState*>(
        GetWindowLongPtr(hwnd, GWLP_USERDATA));
    if (!ch) return;
    for (auto& t : ch->tabs) {
        if (t.id == tab_id) {
            if (percent >= 100) {
                t.loading = false;
                if (t.animProgress <= 0.0f) {
                    t.animProgress = 0.0f;
                    t.progressPercent = 0;
                } else {
                    t.progressPercent = 100;
                    SetTimer(hwnd, ID_LOADING_TIMER, LOADING_TIMER_MS, nullptr);
                }
            } else if (percent > 0) {
                t.loading = true;
                t.progressPercent = percent;
                if (t.animProgress <= 0.0f) {
                    t.animProgress = 10.0f;
                }
                SetTimer(hwnd, ID_LOADING_TIMER, LOADING_TIMER_MS, nullptr);
            } else {
                t.loading = false;
                t.progressPercent = 0;
                t.animProgress = 0.0f;
            }
            break;
        }
    }
    PaintChrome(ch);
}

bool SelectTab(HWND hwnd, int index) {
    ChromeState* ch = reinterpret_cast<ChromeState*>(
        GetWindowLongPtr(hwnd, GWLP_USERDATA));
    if (!ch || index < 0 || index >= (int)ch->tabs.size()) return false;

    if (ch->tabs[index].isSleeping || TabSleepManager::IsTabSleeping(ch->tabs[index].id)) {
        ch->tabs[index].isSleeping = false;
        TabSleepManager::OnTabFocused(ch->tabs[index].id);
    }

    if (index == ch->activeTab && !ch->splitView.IsEnabled()) {
        SyncViewportInternal(ch);
        return true;
    }

    if (ch->splitView.IsEnabled()) {
        int pri = ch->splitView.GetPrimaryTabIndex();
        int sec = ch->splitView.GetSecondaryTabIndex();
        if (index == pri) {
            ch->splitView.SetActivePane(0);
        } else if (index == sec) {
            ch->splitView.SetActivePane(1);
        } else {
            if (ch->splitView.GetActivePane() == 0) {
                ch->splitView.SetPrimaryTabIndex(index);
            } else {
                ch->splitView.SetSecondaryTabIndex(index);
            }
        }
    }

    unsigned int prevTabId = (ch->activeTab >= 0 && ch->activeTab < (int)ch->tabs.size())
        ? ch->tabs[ch->activeTab].id : 0;

    ch->activeTab = index;
    unsigned int newTabId = ch->tabs[index].id;
    ch->tabs[index].lastActiveTime = GetTickCount();

    if (ch->engine) {
        ch->engine->FocusTab(newTabId);
        SyncViewportInternal(ch);
    }

    if (g_TabIntel) g_TabIntel->OnTabFocused((uint64_t)newTabId);
    if (g_JourneyAI && prevTabId != newTabId)
        g_JourneyAI->OnTabSwitch((uint64_t)prevTabId, (uint64_t)newTabId);

    if (ch->hUrlBar && !ch->urlFocused) {
        std::wstring wUrl = Utf8ToUtf16(ReverseMapUrl(ch->tabs[index].url));
        SetWindowTextW(ch->hUrlBar, wUrl.c_str());
    }

    if (ch->tabs[index].loading || ch->tabs[index].animProgress > 0.0f) {
        SetTimer(hwnd, ID_LOADING_TIMER, LOADING_TIMER_MS, nullptr);
    }

    InvalidateRect(hwnd, nullptr, FALSE);
    PaintChrome(ch);
    return true;
}

bool CloseTab(HWND hwnd, int index) {
    ChromeState* ch = reinterpret_cast<ChromeState*>(
        GetWindowLongPtr(hwnd, GWLP_USERDATA));
    if (!ch || index < 0 || index >= (int)ch->tabs.size()) return false;

    unsigned int tabId = ch->tabs[index].id;

    if (g_TabIntel) g_TabIntel->OnTabClosed((uint64_t)tabId);
    TabSleepManager::UnregisterTab(tabId);
    BrowserChrome::TabGroupManager::GetInstance().RemoveTabFromGroup(tabId);

    if (ch->engine) ch->engine->CloseTab(tabId);

    int remaining = (int)ch->tabs.size() - 1;
    ch->splitView.OnTabClosed(index, remaining);

    ch->tabs.erase(ch->tabs.begin() + index);

    if (ch->tabs.empty()) {
        CreateTab(hwnd, "aeon://newtab");
    } else {
        if (index == ch->activeTab) {
            if (ch->activeTab >= (int)ch->tabs.size()) {
                ch->activeTab = (int)ch->tabs.size() - 1;
            }
            unsigned int newActiveId = ch->tabs[ch->activeTab].id;
            ch->tabs[ch->activeTab].lastActiveTime = GetTickCount();

            if (ch->engine) {
                ch->engine->FocusTab(newActiveId);
                SyncViewportInternal(ch);
            }

            if (g_TabIntel) g_TabIntel->OnTabFocused((uint64_t)newActiveId);

            if (ch->hUrlBar && !ch->urlFocused) {
                std::wstring wUrl = Utf8ToUtf16(ReverseMapUrl(ch->tabs[ch->activeTab].url));
                SetWindowTextW(ch->hUrlBar, wUrl.c_str());
            }
        } else if (index < ch->activeTab) {
            ch->activeTab--;
        }
    }

    InvalidateRect(hwnd, nullptr, FALSE);
    PaintChrome(ch);
    return true;
}

void NextTab(HWND hwnd) {
    ChromeState* ch = reinterpret_cast<ChromeState*>(
        GetWindowLongPtr(hwnd, GWLP_USERDATA));
    if (!ch || ch->tabs.size() <= 1) return;
    int nextIdx = (ch->activeTab + 1) % (int)ch->tabs.size();
    SelectTab(hwnd, nextIdx);
}

void PrevTab(HWND hwnd) {
    ChromeState* ch = reinterpret_cast<ChromeState*>(
        GetWindowLongPtr(hwnd, GWLP_USERDATA));
    if (!ch || ch->tabs.size() <= 1) return;
    int prevIdx = (ch->activeTab - 1 + (int)ch->tabs.size()) % (int)ch->tabs.size();
    SelectTab(hwnd, prevIdx);
}

unsigned int GetActiveTabId(HWND hwnd) {
    ChromeState* ch = reinterpret_cast<ChromeState*>(
        GetWindowLongPtr(hwnd, GWLP_USERDATA));
    if (!ch || ch->activeTab < 0 || ch->activeTab >= (int)ch->tabs.size()) return 0;
    return ch->tabs[ch->activeTab].id;
}

uint32_t CreateTabGroup(HWND hwnd, const char* name, COLORREF color) {
    ChromeState* ch = reinterpret_cast<ChromeState*>(GetWindowLongPtr(hwnd, GWLP_USERDATA));
    auto& gm = BrowserChrome::TabGroupManager::GetInstance();
    uint32_t gid = gm.CreateGroup(name ? name : "", color);
    if (ch) PaintChrome(ch);
    return gid;
}

bool AddTabToGroup(HWND hwnd, int tabIndex, uint32_t groupId) {
    ChromeState* ch = reinterpret_cast<ChromeState*>(GetWindowLongPtr(hwnd, GWLP_USERDATA));
    if (!ch || tabIndex < 0 || tabIndex >= (int)ch->tabs.size()) return false;

    ch->tabs[tabIndex].groupId = groupId;
    BrowserChrome::TabGroupManager::GetInstance().AddTabToGroup(ch->tabs[tabIndex].id, groupId);
    PaintChrome(ch);
    return true;
}

bool RemoveTabFromGroup(HWND hwnd, int tabIndex) {
    ChromeState* ch = reinterpret_cast<ChromeState*>(GetWindowLongPtr(hwnd, GWLP_USERDATA));
    if (!ch || tabIndex < 0 || tabIndex >= (int)ch->tabs.size()) return false;

    uint32_t tabId = ch->tabs[tabIndex].id;
    ch->tabs[tabIndex].groupId = 0;
    BrowserChrome::TabGroupManager::GetInstance().RemoveTabFromGroup(tabId);
    PaintChrome(ch);
    return true;
}

bool ToggleGroupCollapsed(HWND hwnd, uint32_t groupId) {
    ChromeState* ch = reinterpret_cast<ChromeState*>(GetWindowLongPtr(hwnd, GWLP_USERDATA));
    bool res = BrowserChrome::TabGroupManager::GetInstance().ToggleGroupCollapsed(groupId);
    if (ch) {
        InvalidateRect(hwnd, nullptr, FALSE);
        PaintChrome(ch);
    }
    return res;
}

uint32_t GetTabGroupId(HWND hwnd, int tabIndex) {
    ChromeState* ch = reinterpret_cast<ChromeState*>(GetWindowLongPtr(hwnd, GWLP_USERDATA));
    if (!ch || tabIndex < 0 || tabIndex >= (int)ch->tabs.size()) return 0;
    return ch->tabs[tabIndex].groupId;
}

void ToggleSplitView(HWND hwnd) {
    ChromeState* ch = reinterpret_cast<ChromeState*>(GetWindowLongPtr(hwnd, GWLP_USERDATA));
    if (!ch) return;

    if (ch->splitView.IsEnabled()) {
        ch->splitView.Disable();
    } else {
        int pri = (ch->activeTab >= 0 && ch->activeTab < (int)ch->tabs.size()) ? ch->activeTab : 0;
        int sec = -1;

        if (ch->tabs.size() < 2) {
            unsigned int newTabId = CreateTab(hwnd, "aeon://newtab");
            for (int i = 0; i < (int)ch->tabs.size(); ++i) {
                if (ch->tabs[i].id == newTabId) { sec = i; break; }
            }
            // Keep original tab as primary
            ch->activeTab = pri;
        } else {
            sec = (pri + 1 < (int)ch->tabs.size()) ? (pri + 1) : (pri - 1 >= 0 ? pri - 1 : 0);
        }

        if (sec < 0) sec = 0;
        ch->splitView.Enable(pri, sec, 0.50f);
    }

    SyncViewportInternal(ch);
    InvalidateRect(hwnd, nullptr, FALSE);
    PaintChrome(ch);
    fprintf(stdout, "[SplitView] Split view %s (primary=%d, secondary=%d).\n",
        ch->splitView.IsEnabled() ? "ENABLED" : "DISABLED",
        ch->splitView.GetPrimaryTabIndex(), ch->splitView.GetSecondaryTabIndex());
}

bool IsSplitViewActive(HWND hwnd) {
    ChromeState* ch = reinterpret_cast<ChromeState*>(GetWindowLongPtr(hwnd, GWLP_USERDATA));
    return ch ? ch->splitView.IsEnabled() : false;
}

void SetSplitView(HWND hwnd, bool active, int primaryTab, int secondaryTab, float ratio) {
    ChromeState* ch = reinterpret_cast<ChromeState*>(GetWindowLongPtr(hwnd, GWLP_USERDATA));
    if (!ch) return;

    if (!active) {
        ch->splitView.Disable();
    } else {
        if (primaryTab < 0) primaryTab = ch->activeTab;
        if (secondaryTab < 0) secondaryTab = (primaryTab + 1 < (int)ch->tabs.size()) ? primaryTab + 1 : 0;
        ch->splitView.Enable(primaryTab, secondaryTab, ratio);
    }

    SyncViewportInternal(ch);
    InvalidateRect(hwnd, nullptr, FALSE);
    PaintChrome(ch);
}

void SetSplitViewActivePane(HWND hwnd, int pane) {
    ChromeState* ch = reinterpret_cast<ChromeState*>(GetWindowLongPtr(hwnd, GWLP_USERDATA));
    if (!ch || !ch->splitView.IsEnabled()) return;

    ch->splitView.SetActivePane(pane);
    int targetTab = ch->splitView.GetActiveTabIndex();
    if (targetTab >= 0 && targetTab < (int)ch->tabs.size()) {
        SelectTab(hwnd, targetTab);
    }
}

void SwapSplitPanes(HWND hwnd) {
    ChromeState* ch = reinterpret_cast<ChromeState*>(GetWindowLongPtr(hwnd, GWLP_USERDATA));
    if (!ch || !ch->splitView.IsEnabled()) return;

    ch->splitView.SwapPanes();
    ch->activeTab = ch->splitView.GetActiveTabIndex();
    if (ch->engine && ch->activeTab >= 0 && ch->activeTab < (int)ch->tabs.size()) {
        ch->engine->FocusTab(ch->tabs[ch->activeTab].id);
    }
    SyncViewportInternal(ch);
    InvalidateRect(hwnd, nullptr, FALSE);
    PaintChrome(ch);
}

void SoloSplitPane(HWND hwnd) {
    ChromeState* ch = reinterpret_cast<ChromeState*>(GetWindowLongPtr(hwnd, GWLP_USERDATA));
    if (!ch || !ch->splitView.IsEnabled()) return;

    ch->activeTab = ch->splitView.GetActiveTabIndex();
    ch->splitView.Disable();
    SyncViewportInternal(ch);
    InvalidateRect(hwnd, nullptr, FALSE);
    PaintChrome(ch);
}

void OnContentAreaClicked(HWND hwnd, int x, int y) {
    ChromeState* ch = reinterpret_cast<ChromeState*>(GetWindowLongPtr(hwnd, GWLP_USERDATA));
    if (!ch || !ch->splitView.IsEnabled()) return;

    RECT rc; GetClientRect(hwnd, &rc);
    int topContentY = GetTopChromeHeight(ch);
    int pane = ch->splitView.HitTestPane(x, y, topContentY, rc.right, rc.bottom);
    if (pane >= 0 && pane != ch->splitView.GetActivePane()) {
        SetSplitViewActivePane(hwnd, pane);
    }
}

void ShowSplitViewMenu(HWND hwnd, int screenX, int screenY) {
    ChromeState* ch = reinterpret_cast<ChromeState*>(GetWindowLongPtr(hwnd, GWLP_USERDATA));
    if (!ch) return;

    HMENU hMenu = CreatePopupMenu();
    if (!hMenu) return;

    if (ch->splitView.IsEnabled()) {
        int pri = ch->splitView.GetPrimaryTabIndex();
        int sec = ch->splitView.GetSecondaryTabIndex();
        int activePane = ch->splitView.GetActivePane();
        float curRatio = ch->splitView.GetSplitRatio();

        std::string priTitle = (pri >= 0 && pri < (int)ch->tabs.size()) ? ch->tabs[pri].title : "Left Pane";
        std::string secTitle = (sec >= 0 && sec < (int)ch->tabs.size()) ? ch->tabs[sec].title : "Right Pane";
        if (priTitle.length() > 25) priTitle = priTitle.substr(0, 22) + "...";
        if (secTitle.length() > 25) secTitle = secTitle.substr(0, 22) + "...";

        AppendMenuW(hMenu, MF_STRING, IDM_SPLIT_SWAP, L"\u21C4  Swap Left and Right Panes\tCtrl+Alt+X");
        AppendMenuA(hMenu, MF_SEPARATOR, 0, nullptr);
        AppendMenuW(hMenu, MF_STRING | (fabsf(curRatio - 0.50f) < 0.02f ? MF_CHECKED : 0),
                    IDM_SPLIT_PRESET_50_50, L"\u25EB  50 / 50 Balanced Split");
        AppendMenuW(hMenu, MF_STRING | (fabsf(curRatio - 0.60f) < 0.02f ? MF_CHECKED : 0),
                    IDM_SPLIT_PRESET_60_40, L"\u25EB  60 / 40 Primary Focus Split");
        AppendMenuW(hMenu, MF_STRING | (fabsf(curRatio - 0.70f) < 0.02f ? MF_CHECKED : 0),
                    IDM_SPLIT_PRESET_70_30, L"\u25EB  70 / 30 Deep Work Split");
        AppendMenuA(hMenu, MF_SEPARATOR, 0, nullptr);

        // Submenu: Change Left Pane Tab
        HMENU hLeftSub = CreatePopupMenu();
        for (int i = 0; i < (int)ch->tabs.size(); ++i) {
            std::string tTitle = ch->tabs[i].title.empty() ? "Tab " + std::to_string(i + 1) : ch->tabs[i].title;
            if (tTitle.length() > 30) tTitle = tTitle.substr(0, 27) + "...";
            UINT flags = MF_STRING;
            if (i == pri) flags |= MF_CHECKED;
            if (i == sec) tTitle += " (Right Pane)";
            AppendMenuA(hLeftSub, flags, IDM_SPLIT_SET_LEFT_BASE + i, tTitle.c_str());
        }
        std::string leftLabel = "Left Pane (" + (activePane == 0 ? std::string("Active: ") : std::string("")) + priTitle + ")";
        AppendMenuA(hMenu, MF_POPUP, (UINT_PTR)hLeftSub, leftLabel.c_str());

        // Submenu: Change Right Pane Tab
        HMENU hRightSub = CreatePopupMenu();
        for (int i = 0; i < (int)ch->tabs.size(); ++i) {
            std::string tTitle = ch->tabs[i].title.empty() ? "Tab " + std::to_string(i + 1) : ch->tabs[i].title;
            if (tTitle.length() > 30) tTitle = tTitle.substr(0, 27) + "...";
            UINT flags = MF_STRING;
            if (i == sec) flags |= MF_CHECKED;
            if (i == pri) tTitle += " (Left Pane)";
            AppendMenuA(hRightSub, flags, IDM_SPLIT_SET_RIGHT_BASE + i, tTitle.c_str());
        }
        std::string rightLabel = "Right Pane (" + (activePane == 1 ? std::string("Active: ") : std::string("")) + secTitle + ")";
        AppendMenuA(hMenu, MF_POPUP, (UINT_PTR)hRightSub, rightLabel.c_str());

        AppendMenuA(hMenu, MF_SEPARATOR, 0, nullptr);
        AppendMenuA(hMenu, MF_STRING, IDM_SPLIT_NEW_TAB, "+  Open New Tab in Inactive Pane");
        AppendMenuW(hMenu, MF_STRING, IDM_SPLIT_SOLO_ACTIVE, L"\u2922  Expand Active Pane (Full Width)");
        AppendMenuW(hMenu, MF_STRING, IDM_SPLIT_CLOSE_INACTIVE, L"\u2715  Close Inactive Pane");
        AppendMenuW(hMenu, MF_STRING, IDM_SPLIT_EXIT, L"\u2715  Exit Split View");
    } else {
        AppendMenuW(hMenu, MF_STRING, IDM_TAB_SPLITVIEW, L"\u25EB  Split with Adjacent Tab\tCtrl+Alt+S");
        AppendMenuA(hMenu, MF_STRING, IDM_SPLIT_NEW_TAB, "+  Split with New Tab");

        if (ch->tabs.size() > 1) {
            HMENU hSplitWithSub = CreatePopupMenu();
            for (int i = 0; i < (int)ch->tabs.size(); ++i) {
                if (i == ch->activeTab) continue;
                std::string tTitle = ch->tabs[i].title.empty() ? "Tab " + std::to_string(i + 1) : ch->tabs[i].title;
                if (tTitle.length() > 30) tTitle = tTitle.substr(0, 27) + "...";
                AppendMenuA(hSplitWithSub, MF_STRING, IDM_SPLIT_WITH_BASE + i, tTitle.c_str());
            }
            AppendMenuA(hMenu, MF_POPUP, (UINT_PTR)hSplitWithSub, "Split with Tab...");
        }
    }

    int cmd = TrackPopupMenu(hMenu, TPM_RETURNCMD | TPM_RIGHTBUTTON,
                             screenX, screenY, 0, hwnd, nullptr);
    DestroyMenu(hMenu);

    if (cmd == IDM_SPLIT_SWAP) {
        SwapSplitPanes(hwnd);
    } else if (cmd == IDM_SPLIT_RESET_RATIO || cmd == IDM_SPLIT_PRESET_50_50) {
        ch->splitView.SetPreset50_50();
        SyncViewportInternal(ch);
        InvalidateRect(hwnd, nullptr, FALSE);
        PaintChrome(ch);
    } else if (cmd == IDM_SPLIT_PRESET_60_40) {
        ch->splitView.SetPreset60_40();
        SyncViewportInternal(ch);
        InvalidateRect(hwnd, nullptr, FALSE);
        PaintChrome(ch);
    } else if (cmd == IDM_SPLIT_PRESET_70_30) {
        ch->splitView.SetPreset70_30();
        SyncViewportInternal(ch);
        InvalidateRect(hwnd, nullptr, FALSE);
        PaintChrome(ch);
    } else if (cmd == IDM_SPLIT_SOLO_ACTIVE) {
        SoloSplitPane(hwnd);
    } else if (cmd == IDM_SPLIT_EXIT || cmd == IDM_TAB_SPLITVIEW) {
        ToggleSplitView(hwnd);
    } else if (cmd == IDM_SPLIT_CLOSE_INACTIVE) {
        int inactivePane = 1 - ch->splitView.GetActivePane();
        int inactiveTab = (inactivePane == 0) ? ch->splitView.GetPrimaryTabIndex() : ch->splitView.GetSecondaryTabIndex();
        CloseTab(hwnd, inactiveTab);
    } else if (cmd == IDM_SPLIT_NEW_TAB) {
        if (ch->splitView.IsEnabled()) {
            int inactivePane = 1 - ch->splitView.GetActivePane();
            CreateTab(hwnd, "aeon://newtab");
            int newIdx = (int)ch->tabs.size() - 1;
            ch->splitView.SetPaneTab(inactivePane, newIdx);
            ch->splitView.SetActivePane(inactivePane);
            ch->activeTab = newIdx;
            SyncViewportInternal(ch);
            InvalidateRect(hwnd, nullptr, FALSE);
            PaintChrome(ch);
        } else {
            int cur = ch->activeTab;
            CreateTab(hwnd, "aeon://newtab");
            ch->splitView.Enable(cur, ch->activeTab, 0.50f);
            ch->splitView.SetActivePane(0);
            SyncViewportInternal(ch);
            InvalidateRect(hwnd, nullptr, FALSE);
            PaintChrome(ch);
        }
    } else if (cmd >= IDM_SPLIT_SET_LEFT_BASE && cmd < IDM_SPLIT_SET_LEFT_BASE + (int)ch->tabs.size()) {
        int targetIdx = cmd - IDM_SPLIT_SET_LEFT_BASE;
        ch->splitView.SetPrimaryTabIndex(targetIdx);
        if (ch->splitView.GetSecondaryTabIndex() == targetIdx) {
            ch->splitView.SetSecondaryTabIndex(ch->activeTab != targetIdx ? ch->activeTab : 0);
        }
        SyncViewportInternal(ch);
        InvalidateRect(hwnd, nullptr, FALSE);
        PaintChrome(ch);
    } else if (cmd >= IDM_SPLIT_SET_RIGHT_BASE && cmd < IDM_SPLIT_SET_RIGHT_BASE + (int)ch->tabs.size()) {
        int targetIdx = cmd - IDM_SPLIT_SET_RIGHT_BASE;
        ch->splitView.SetSecondaryTabIndex(targetIdx);
        if (ch->splitView.GetPrimaryTabIndex() == targetIdx) {
            ch->splitView.SetPrimaryTabIndex(ch->activeTab != targetIdx ? ch->activeTab : 0);
        }
        SyncViewportInternal(ch);
        InvalidateRect(hwnd, nullptr, FALSE);
        PaintChrome(ch);
    } else if (cmd >= IDM_SPLIT_WITH_BASE && cmd < IDM_SPLIT_WITH_BASE + (int)ch->tabs.size()) {
        int secIdx = cmd - IDM_SPLIT_WITH_BASE;
        ch->splitView.Enable(ch->activeTab, secIdx, 0.50f);
        ch->splitView.SetActivePane(0);
        SyncViewportInternal(ch);
        InvalidateRect(hwnd, nullptr, FALSE);
        PaintChrome(ch);
    }
}

void SuspendTab(HWND hwnd, int tabIndex) {
    ChromeState* ch = reinterpret_cast<ChromeState*>(GetWindowLongPtr(hwnd, GWLP_USERDATA));
    if (!ch || tabIndex < 0 || tabIndex >= (int)ch->tabs.size()) return;

    ch->tabs[tabIndex].isSleeping = true;
    TabSleepManager::SuspendTab(ch->tabs[tabIndex].id);
    InvalidateRect(hwnd, nullptr, FALSE);
    PaintChrome(ch);
}

void WakeTab(HWND hwnd, int tabIndex) {
    ChromeState* ch = reinterpret_cast<ChromeState*>(GetWindowLongPtr(hwnd, GWLP_USERDATA));
    if (!ch || tabIndex < 0 || tabIndex >= (int)ch->tabs.size()) return;

    ch->tabs[tabIndex].isSleeping = false;
    TabSleepManager::WakeTab(ch->tabs[tabIndex].id);
    InvalidateRect(hwnd, nullptr, FALSE);
    PaintChrome(ch);
}

int GetTabCount(HWND hwnd) {
    ChromeState* ch = reinterpret_cast<ChromeState*>(
        GetWindowLongPtr(hwnd, GWLP_USERDATA));
    if (!ch) return 0;
    return (int)ch->tabs.size();
}

bool GetTabInfo(HWND hwnd, int index,
                unsigned int* outId, char* outUrl, int urlLen,
                char* outTitle, int titleLen, bool* outActive) {
    ChromeState* ch = reinterpret_cast<ChromeState*>(
        GetWindowLongPtr(hwnd, GWLP_USERDATA));
    if (!ch || index < 0 || index >= (int)ch->tabs.size()) return false;

    const auto& t = ch->tabs[index];
    if (outId)    *outId = t.id;
    if (outUrl)   _snprintf_s(outUrl, urlLen, _TRUNCATE, "%s", t.url.c_str());
    if (outTitle) _snprintf_s(outTitle, titleLen, _TRUNCATE, "%s", t.title.c_str());
    if (outActive) *outActive = (index == ch->activeTab);
    return true;
}

int GetActiveTabIndex(HWND hwnd) {
    ChromeState* ch = reinterpret_cast<ChromeState*>(
        GetWindowLongPtr(hwnd, GWLP_USERDATA));
    if (!ch) return -1;
    return ch->activeTab;
}

unsigned int CreateTab(HWND hwnd, const char* url) {
    ChromeState* ch = reinterpret_cast<ChromeState*>(
        GetWindowLongPtr(hwnd, GWLP_USERDATA));
    if (!ch || !ch->engine) return 0;

    const char* targetUrl = (url && url[0]) ? url : "aeon://newtab";
    AeonEngineVTable* engineToUse = TierDispatcher::GetInstance().GetEngineForUrl(targetUrl);
    if (!engineToUse) engineToUse = ch->engine;

    ChromeTab t;
    t.id = engineToUse->NewTab(ch->hwnd, targetUrl);
    t.url = targetUrl;
    t.title = "New Tab";
    t.loading = false;
    t.progressPercent = 0;
    t.animProgress = 0.0f;
    t.lastActiveTime = GetTickCount();
    t.groupId = 0;
    t.isPinned = false;
    t.isSleeping = false;
    t.history.push_back(targetUrl);
    t.historyIndex = 0;

    int newIdx = (int)ch->tabs.size();
    ch->tabs.push_back(t);
    ch->splitView.OnTabInserted(newIdx);
    ch->activeTab = newIdx;

    TabSleepManager::RegisterTab(t.id, targetUrl);

    SyncViewportInternal(ch);
    engineToUse->FocusTab(t.id);

    std::string bridgeJs = AeonBridge::BuildInjectionScript();
    engineToUse->InjectEarlyJS(t.id, bridgeJs.c_str());

    if (g_TabIntel) {
        AeonTabInfo info = {};
        info.tab_id = (uint64_t)t.id;
        _snprintf_s(info.url, sizeof(info.url), _TRUNCATE, "%s", targetUrl);
        _snprintf_s(info.title, sizeof(info.title), _TRUNCATE, "New Tab");
        info.state = AeonTabState::Active;
        g_TabIntel->OnTabCreated(info);
    }

    PaintChrome(ch);
    fprintf(stdout, "[Agent] New tab #%u: %s\n", t.id, targetUrl);
    return t.id;
}

bool CloseTabById(HWND hwnd, unsigned int tabId) {
    ChromeState* ch = reinterpret_cast<ChromeState*>(
        GetWindowLongPtr(hwnd, GWLP_USERDATA));
    if (!ch) return false;

    for (int i = 0; i < (int)ch->tabs.size(); i++) {
        if (ch->tabs[i].id == tabId) {
            return CloseTab(hwnd, i);
        }
    }
    return false;
}

bool FocusTabById(HWND hwnd, unsigned int tabId) {
    ChromeState* ch = reinterpret_cast<ChromeState*>(
        GetWindowLongPtr(hwnd, GWLP_USERDATA));
    if (!ch) return false;

    for (int i = 0; i < (int)ch->tabs.size(); i++) {
        if (ch->tabs[i].id == tabId) {
            return SelectTab(hwnd, i);
        }
    }
    return false;
}

bool NavigateTab(HWND hwnd, unsigned int tabId, const char* url) {
    ChromeState* ch = reinterpret_cast<ChromeState*>(
        GetWindowLongPtr(hwnd, GWLP_USERDATA));
    if (!ch || !ch->engine || !url || !url[0]) return false;

    for (auto& t : ch->tabs) {
        if (t.id == tabId) {
            t.url = url;
            std::string mapped = ReverseMapUrl(t.url);
            if (mapped == "aeon://newtab" || t.url == "about:blank" || t.url.empty()) {
                t.loading = false;
                t.progressPercent = 0;
                t.animProgress = 0.0f;
            } else {
                t.loading = true;
                t.progressPercent = 25;
                t.animProgress = 10.0f;
                SetTimer(hwnd, ID_LOADING_TIMER, LOADING_TIMER_MS, nullptr);
            }
            t.isSleeping = false;
            TabSleepManager::OnTabActivity(tabId);

            AeonEngineVTable* engineToUse = TierDispatcher::GetInstance().GetEngineForUrl(url);
            if (!engineToUse) engineToUse = ch->engine;
            if (engineToUse) engineToUse->Navigate(tabId, url, nullptr);
            SyncViewportInternal(ch);
            PaintChrome(ch);
            fprintf(stdout, "[Agent] Navigate tab #%u: %s\n", tabId, url);
            return true;
        }
    }
    return false;
}

static void OpenOrFocusInternalUrl(ChromeState* ch, HWND hwnd, const char* url) {
    if (!ch || !hwnd || !url || !url[0]) return;

    int effectiveTab = ch->splitView.IsEnabled() ? ch->splitView.GetActiveTabIndex() : ch->activeTab;
    if (effectiveTab >= 0 && effectiveTab < (int)ch->tabs.size()) {
        const std::string& curUrl = ch->tabs[effectiveTab].url;
        if (curUrl == "aeon://newtab" || curUrl == "about:blank" || curUrl.empty()) {
            NavigateActiveTab(ch, url);
            return;
        }
    }

    std::string targetUrlStr = url;
    size_t targetHash = targetUrlStr.find('#');
    std::string targetBase = (targetHash != std::string::npos) ? targetUrlStr.substr(0, targetHash) : targetUrlStr;

    if (targetBase.rfind("aeon://", 0) == 0) {
        for (int i = 0; i < (int)ch->tabs.size(); i++) {
            const std::string& tabUrl = ch->tabs[i].url;
            size_t tabHash = tabUrl.find('#');
            std::string tabBase = (tabHash != std::string::npos) ? tabUrl.substr(0, tabHash) : tabUrl;

            if (tabBase == targetBase) {
                if (targetUrlStr != tabUrl) {
                    NavigateTab(hwnd, ch->tabs[i].id, url);
                }
                SelectTab(hwnd, i);
                return;
            }
        }
    }

    CreateTab(hwnd, url);
}

void OpenOrFocusUrl(HWND hwnd, const char* url) {
    ChromeState* ch = reinterpret_cast<ChromeState*>(
        GetWindowLongPtr(hwnd, GWLP_USERDATA));
    if (ch) OpenOrFocusInternalUrl(ch, hwnd, url);
}

void ToggleSidebar(HWND hwnd) {
    ChromeState* ch = reinterpret_cast<ChromeState*>(
        GetWindowLongPtr(hwnd, GWLP_USERDATA));
    if (!ch) return;

    ch->verticalSidebar = !ch->verticalSidebar;
    ch->sidebarWidth = S(ch, 220);
    ch->settings.vertical_tabs = ch->verticalSidebar;
    SettingsEngine::Save(ch->settings);
    SyncViewportInternal(ch);
    InvalidateRect(hwnd, nullptr, FALSE);
    PaintChrome(ch);
    fprintf(stdout, "[Arc UI] Toggled vertical sidebar state (%s).\n", ch->verticalSidebar ? "ON" : "OFF");
}

void SetLayoutMode(HWND hwnd, bool verticalSidebar) {
    ChromeState* ch = reinterpret_cast<ChromeState*>(
        GetWindowLongPtr(hwnd, GWLP_USERDATA));
    if (!ch) return;

    ch->verticalSidebar = verticalSidebar;
    ch->sidebarWidth = S(ch, 220);
    ch->settings.vertical_tabs = verticalSidebar;
    SettingsEngine::Save(ch->settings);
    SyncViewportInternal(ch);
    InvalidateRect(hwnd, nullptr, FALSE);
    PaintChrome(ch);
    fprintf(stdout, "[Arc UI] Set layout mode to %s.\n", verticalSidebar ? "Arc Vertical Sidebar" : "Horizontal Chrome");
}

bool IsVerticalSidebarActive(HWND hwnd) {
    ChromeState* ch = reinterpret_cast<ChromeState*>(
        GetWindowLongPtr(hwnd, GWLP_USERDATA));
    return ch ? ch->verticalSidebar : false;
}

int GetTabStripHeight(HWND hwnd) {
    ChromeState* ch = reinterpret_cast<ChromeState*>(
        GetWindowLongPtr(hwnd, GWLP_USERDATA));
    if (!ch) return TAB_STRIP_HEIGHT;
    return ch->isFullscreen ? 0 : S(ch, TAB_STRIP_HEIGHT);
}

int GetChromeHeight(HWND hwnd) {
    ChromeState* ch = reinterpret_cast<ChromeState*>(
        GetWindowLongPtr(hwnd, GWLP_USERDATA));
    if (!ch) return CHROME_H;
    return GetTopChromeHeight(ch);
}

bool IsCaptionDragPoint(HWND hwnd, int x, int y) {
    ChromeState* ch = reinterpret_cast<ChromeState*>(
        GetWindowLongPtr(hwnd, GWLP_USERDATA));
    if (!ch || ch->isFullscreen) return false;

    int hit = HitTest(hwnd, x, y);
    if (hit != -1) return false;

    int topStripH = S(ch, TAB_STRIP_HEIGHT);
    if (y >= 0 && y < topStripH) {
        return true;
    }

    int sidebarW = GetEffectiveSidebarWidth(ch);
    int topContentY = GetTopChromeHeight(ch);
    if (sidebarW > 0 && x >= 0 && x < sidebarW && y >= topContentY) {
        return true;
    }

    return false;
}

void ToggleCommandBar(HWND hwnd) {
    ChromeState* ch = reinterpret_cast<ChromeState*>(
        GetWindowLongPtr(hwnd, GWLP_USERDATA));
    if (!ch) return;

    ActivateUrlBar(ch);
    fprintf(stdout, "[Arc UI] Triggered Command Bar (Ctrl+K).\n");
}

void OnDpiChanged(HWND hwnd, int newDpi) {
    ChromeState* ch = reinterpret_cast<ChromeState*>(
        GetWindowLongPtr(hwnd, GWLP_USERDATA));
    if (!ch) return;

    if (newDpi <= 0) newDpi = 96;
    ch->dpi = newDpi;
    ch->dpiScale = (float)newDpi / 96.0f;
    ch->sidebarWidth = S(ch, 220);

    if (ch->hTextFont)      DeleteObject(ch->hTextFont);
    if (ch->hTextBold)      DeleteObject(ch->hTextBold);
    if (ch->hTextTabFont)   DeleteObject(ch->hTextTabFont);
    if (ch->hIconFont)      DeleteObject(ch->hIconFont);
    if (ch->hIconFontSmall) DeleteObject(ch->hIconFontSmall);
    if (ch->hIconFontLarge) DeleteObject(ch->hIconFontLarge);
    if (ch->hLogoFont)      DeleteObject(ch->hLogoFont);
    if (ch->hUrlFont)       DeleteObject(ch->hUrlFont);

    ch->hTextFont = CreateFontW(-MulDiv(9, newDpi, 72), 0, 0, 0, FW_NORMAL,
        0, 0, 0, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
        CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_SWISS, L"Segoe UI");
    ch->hTextBold = CreateFontW(-MulDiv(9, newDpi, 72), 0, 0, 0, FW_SEMIBOLD,
        0, 0, 0, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
        CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_SWISS, L"Segoe UI");
    ch->hTextTabFont = CreateFontW(-MulDiv(8, newDpi, 72), 0, 0, 0, FW_NORMAL,
        0, 0, 0, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
        CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_SWISS, L"Segoe UI");
    ch->hIconFont = CreateFontW(-MulDiv(10, newDpi, 72), 0, 0, 0, FW_NORMAL,
        0, 0, 0, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
        CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, L"Segoe MDL2 Assets");
    ch->hIconFontSmall = CreateFontW(-MulDiv(8, newDpi, 72), 0, 0, 0, FW_NORMAL,
        0, 0, 0, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
        CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, L"Segoe MDL2 Assets");
    ch->hIconFontLarge = CreateFontW(-MulDiv(11, newDpi, 72), 0, 0, 0, FW_NORMAL,
        0, 0, 0, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
        CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, L"Segoe MDL2 Assets");
    ch->hLogoFont = CreateFontW(-MulDiv(15, newDpi, 72), 0, 0, 0, FW_BOLD,
        0, 0, 0, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
        CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_SWISS, L"Segoe UI");
    ch->hUrlFont = CreateFontW(-MulDiv(10, newDpi, 72), 0, 0, 0, FW_NORMAL,
        0, 0, 0, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
        CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_SWISS, L"Segoe UI");

    if (ch->hUrlBar) {
        SendMessageW(ch->hUrlBar, WM_SETFONT, (WPARAM)ch->hUrlFont, TRUE);
    }
    if (ch->hFindEdit && ch->hTextTabFont) {
        SendMessageW(ch->hFindEdit, WM_SETFONT, (WPARAM)ch->hTextTabFont, TRUE);
    }
    DownloadButton::OnDpiChanged(newDpi);
    ShieldHUD::OnDpiChanged(newDpi);
    AeonOmni::OnDpiChanged(newDpi);

    RECT rc; GetClientRect(hwnd, &rc);
    OnSize(hwnd, rc.right, rc.bottom);
    PaintChrome(ch);
}

void Create(HWND parent, const SystemProfile* profile, AeonEngineVTable* engine) {
    ChromeState* ch = new ChromeState();
    ch->profile    = profile;
    ch->engine     = engine;
    ch->activeTab  = -1;
    ch->hoverTab   = -1;
    ch->hoverBtn   = 0;
    ch->urlFocused = false;
    ch->isFullscreen = false;
    ch->hUrlBgBrush = CreateSolidBrush(RGB(20, 23, 43));
    ch->settings   = SettingsEngine::Load();

    int dpiY = (int)AeonDwm::GetWindowDpiSafe(parent);
    if (dpiY <= 0) dpiY = 96;
    ch->dpi = dpiY;
    ch->dpiScale = (float)dpiY / 96.0f;
    ch->verticalSidebar = ch->settings.vertical_tabs;
    ch->sidebarWidth = S(ch, 220);

    HINSTANCE hInst = GetModuleHandle(nullptr);
    ShieldHUD::RegisterClass(hInst);
    BookmarkToast::RegisterClass(hInst);
    AppMenu::RegisterClass(hInst);
    AeonOmni::RegisterClass(hInst);
    BookmarkBar::RegisterClass(hInst);
    DownloadButton::RegisterClass(hInst);
    AeonOmni::Initialize(parent, hInst, [ch](const std::string& url) {
        NavigateActiveTab(ch, url.c_str());
    });
    AeonOmni::SetSearchEngine(ch->settings.search_engine);

    TabSleepManager::Initialize(0);
    TabSleepManager::SetEngine(engine, parent);

    RECT clr; GetClientRect(parent, &clr);
    ch->hBookmarkBar = BookmarkBar::Create(parent, 0, S(ch, CHROME_H), clr.right, [ch](const wchar_t* wUrl) {
        NavigateActiveTab(ch, Utf16ToUtf8(wUrl).c_str());
    });
    BookmarkBar::Show(false);

    ch->hDownloadBtn = DownloadButton::Create(parent, clr.right - S(ch, 136), S(ch, TAB_STRIP_HEIGHT + 5), S(ch, 28));
    DownloadButton::SetVisible(false);

    ch->hTextFont = CreateFontW(-MulDiv(9, dpiY, 72), 0, 0, 0, FW_NORMAL,
        0, 0, 0, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
        CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_SWISS, L"Segoe UI");
    ch->hTextBold = CreateFontW(-MulDiv(9, dpiY, 72), 0, 0, 0, FW_SEMIBOLD,
        0, 0, 0, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
        CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_SWISS, L"Segoe UI");
    ch->hTextTabFont = CreateFontW(-MulDiv(8, dpiY, 72), 0, 0, 0, FW_NORMAL,
        0, 0, 0, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
        CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_SWISS, L"Segoe UI");
    ch->hIconFont = CreateFontW(-MulDiv(10, dpiY, 72), 0, 0, 0, FW_NORMAL,
        0, 0, 0, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
        CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, L"Segoe MDL2 Assets");
    ch->hIconFontSmall = CreateFontW(-MulDiv(8, dpiY, 72), 0, 0, 0, FW_NORMAL,
        0, 0, 0, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
        CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, L"Segoe MDL2 Assets");
    ch->hIconFontLarge = CreateFontW(-MulDiv(11, dpiY, 72), 0, 0, 0, FW_NORMAL,
        0, 0, 0, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
        CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, L"Segoe MDL2 Assets");
    ch->hLogoFont = CreateFontW(-MulDiv(15, dpiY, 72), 0, 0, 0, FW_BOLD,
        0, 0, 0, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
        CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_SWISS, L"Segoe UI");
    ch->hUrlFont = CreateFontW(-MulDiv(10, dpiY, 72), 0, 0, 0, FW_NORMAL,
        0, 0, 0, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
        CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_SWISS, L"Segoe UI");

    if (engine) {
        ChromeTab t;
        t.id      = engine->NewTab(parent, "aeon://newtab");
        t.url     = "aeon://newtab";
        t.title   = "New Tab";
        t.loading = false;
        t.progressPercent = 0;
        t.animProgress = 0.0f;
        t.groupId = 0;
        t.isPinned = false;
        t.isSleeping = false;
        t.history.push_back(t.url);
        t.historyIndex = 0;
        ch->tabs.push_back(t);
        ch->activeTab = 0;

        TabSleepManager::RegisterTab(t.id, t.url.c_str());

        RECT rc; GetClientRect(parent, &rc);
        int initSidebarW = GetEffectiveSidebarWidth(ch);
        int initTopH = GetTopChromeHeight(ch);
        engine->SetViewport(t.id, parent, initSidebarW, initTopH,
            max(0, rc.right - initSidebarW), max(0, rc.bottom - initTopH));

        std::string bridgeJs = AeonBridge::BuildInjectionScript();
        engine->InjectEarlyJS(t.id, bridgeJs.c_str());

        AeonBridge::RegisterScriptEvaluator([engine](int tabId, const char* script) -> std::string {
            if (!engine || !engine->ExecuteScript) return "";
            char resultBuf[65536] = {0};
            int len = engine->ExecuteScript((unsigned int)tabId, script, resultBuf, sizeof(resultBuf));
            if (len > 0) return std::string(resultBuf);
            return "";
        });
    }

    ch->hwnd = parent;
    PaintChrome(ch);

    SetWindowLongPtr(parent, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(ch));

    AeonAgentPipe::Start(parent);

    fprintf(stdout, "[Chrome] Browser chrome created. Navigation Toolbar & Omnibox active. "
         "Tab strip: %d tab(s).\n", (int)ch->tabs.size());

    std::thread([]{
        DnsResolver::Initialize();
        NetworkSentinel::Analyze();
        NetworkSentinel::ApplyBestStrategy();
        NetworkSentinel::StartMonitor();
    }).detach();
}

void OnPaint(HWND hwnd) {
    ChromeState* ch = reinterpret_cast<ChromeState*>(
        GetWindowLongPtr(hwnd, GWLP_USERDATA));
    if (ch) {
        PAINTSTRUCT ps;
        HDC hdc = BeginPaint(hwnd, &ps);
        PaintChrome(ch, hdc);
        EndPaint(hwnd, &ps);
    }
}

void OnTimer(HWND hwnd, WPARAM wParam) {
    ChromeState* ch = reinterpret_cast<ChromeState*>(
        GetWindowLongPtr(hwnd, GWLP_USERDATA));
    if (!ch) return;

    static DWORD g_lastPollTick = 0;
    DWORD nowTick = GetTickCount();
    if (nowTick - g_lastPollTick > 1000) {
        g_lastPollTick = nowTick;
        // Poll downloads
        auto allDownloads = DownloadManager::GetAll();
        int activeCount = 0;
        double totalPct = 0;
        for (const auto& d : allDownloads) {
            if (d.state == DownloadManager::DownloadState::Active || d.state == DownloadManager::DownloadState::Pending) {
                activeCount++;
                totalPct += d.progress_percent;
            }
        }
        int avgPct = activeCount > 0 ? (int)(totalPct / activeCount) : -1;
        DownloadButton::Update(activeCount, avgPct);

        // Check active tab audio
        if (ch->engine && ch->engine->ExecuteScript && ch->activeTab >= 0 && ch->activeTab < (int)ch->tabs.size()) {
            char outBuf[64] = {};
            const char* checkJs = "(function(){ try { return Array.from(document.querySelectorAll('video,audio')).some(e => !e.paused && !e.ended && e.currentTime > 0) ? '1' : '0'; } catch(e){ return '0'; } })()";
            ch->engine->ExecuteScript(ch->tabs[ch->activeTab].id, checkJs, outBuf, sizeof(outBuf));
            bool isPlaying = (outBuf[0] == '1' || strstr(outBuf, "\"1\"") != nullptr);
            if (ch->tabs[ch->activeTab].isPlayingAudio != isPlaying) {
                ch->tabs[ch->activeTab].isPlayingAudio = isPlaying;
                PaintChrome(ch);
            }
        }
    }

    if (wParam == ID_LOADING_TIMER) {
        bool anyActive = false;
        ch->spinnerAngle += 15.0f;
        if (ch->spinnerAngle >= 360.0f) ch->spinnerAngle -= 360.0f;

        static DWORD lastCreepTick = 0;
        bool doCreep = (nowTick - lastCreepTick > 100);
        if (doCreep) lastCreepTick = nowTick;

        for (auto& t : ch->tabs) {
            std::string mapped = ReverseMapUrl(t.url);
            bool isNewTab = (t.url == "aeon://newtab" || mapped == "aeon://newtab" || t.url == "about:blank" || t.url.empty());
            if (isNewTab && !t.loading && t.progressPercent == 0) {
                t.animProgress = 0.0f;
                continue;
            }

            if (t.loading) {
                anyActive = true;
                if (doCreep && t.progressPercent < 85) {
                    t.progressPercent++;
                }
                float target = (float)max(t.progressPercent, 10);
                if (t.animProgress < target) {
                    t.animProgress += (target - t.animProgress) * 0.20f + 0.3f;
                    if (t.animProgress > target) t.animProgress = target;
                }
            } else {
                // Not loading (page finished or stopped)
                if (t.progressPercent >= 100 && t.animProgress > 0.0f) {
                    if (t.animProgress < 100.0f) {
                        float finishStep = (100.0f - t.animProgress) * 0.35f + 3.0f;
                        t.animProgress += finishStep;
                        if (t.animProgress >= 100.0f) {
                            t.animProgress = 100.0f;
                        }
                        anyActive = true;
                    } else {
                        // Displayed 100% on previous frame; now cleanly reset
                        t.animProgress = 0.0f;
                        t.progressPercent = 0;
                    }
                } else {
                    t.animProgress = 0.0f;
                    t.progressPercent = 0;
                }
            }
        }

        PaintChrome(ch);

        if (!anyActive) {
            KillTimer(hwnd, ID_LOADING_TIMER);
        }
        return;
    }

    if (wParam != 9005) return;

    bool anyActive = false;
    double delta = 0.15;

    for (int i = 0; i < 600; i++) {
        double target = (ch->hoverBtn == i) ? 1.0 : 0.0;
        if (ch->hoverAlphas[i] != target) {
            if (ch->hoverAlphas[i] < target) {
                ch->hoverAlphas[i] += delta;
                if (ch->hoverAlphas[i] > target) ch->hoverAlphas[i] = target;
            } else {
                ch->hoverAlphas[i] -= delta;
                if (ch->hoverAlphas[i] < target) ch->hoverAlphas[i] = target;
            }
            anyActive = true;
        }
    }

    if (anyActive) {
        PaintChrome(ch);
    } else {
        KillTimer(hwnd, 9005);
    }
}

void OnSize(HWND hwnd, int w, int h) {
    ChromeState* ch = reinterpret_cast<ChromeState*>(
        GetWindowLongPtr(hwnd, GWLP_USERDATA));
    if (!ch) return;

    if (AeonOmni::IsVisible()) {
        AeonOmni::Hide();
    }

    PaintChrome(ch);

    if (ch->hUrlBar && IsWindowVisible(ch->hUrlBar)) {
        int urlLeft = S(ch, 102 + 28);
        int urlRight = w - S(ch, 144 + 52);
        MoveWindow(ch->hUrlBar, urlLeft, S(ch, 44), max(S(ch, 40), urlRight - urlLeft), S(ch, 22), TRUE);
    }

    SyncViewportInternal(ch);
}

void Destroy(HWND hwnd) {
    KillTimer(hwnd, ID_LOADING_TIMER);
    KillTimer(hwnd, 9005 /* ID_HOVER_TIMER */);
    KillTimer(hwnd, SessionManager::AUTOSAVE_TIMER_ID);

    TabSleepManager::Shutdown();
    AeonOmni::Destroy();

    ChromeState* ch = reinterpret_cast<ChromeState*>(
        GetWindowLongPtr(hwnd, GWLP_USERDATA));
    if (ch) {
        if (ch->hTextFont)      DeleteObject(ch->hTextFont);
        if (ch->hTextBold)      DeleteObject(ch->hTextBold);
        if (ch->hTextTabFont)   DeleteObject(ch->hTextTabFont);
        if (ch->hIconFont)      DeleteObject(ch->hIconFont);
        if (ch->hIconFontSmall) DeleteObject(ch->hIconFontSmall);
        if (ch->hIconFontLarge) DeleteObject(ch->hIconFontLarge);
        if (ch->hLogoFont)      DeleteObject(ch->hLogoFont);
        if (ch->hUrlFont)       DeleteObject(ch->hUrlFont);
        if (ch->hUrlBgBrush)    DeleteObject(ch->hUrlBgBrush);

        SetWindowLongPtr(hwnd, GWLP_USERDATA, 0);
        delete ch;
        fprintf(stdout, "[Chrome] Browser chrome state and GDI resources cleaned up.\n");
    }
}

// -- Page Zoom Implementations ---------------------------------------
void SetZoom(HWND hwnd, double factor) {
    ChromeState* ch = reinterpret_cast<ChromeState*>(GetWindowLongPtr(hwnd, GWLP_USERDATA));
    if (!ch) return;
    int eff = ch->splitView.IsEnabled() ? ch->splitView.GetActiveTabIndex() : ch->activeTab;
    if (eff < 0 || eff >= (int)ch->tabs.size()) return;

    if (factor < 0.25) factor = 0.25;
    if (factor > 5.00) factor = 5.00;

    ch->tabs[eff].zoomFactor = factor;
    ch->currentZoomPct = (int)(factor * 100.0 + 0.5);
    ch->zoomBadgeExpireTick = GetTickCount() + 2500;

    if (ch->engine && ch->engine->ExecuteScript) {
        char js[256];
        snprintf(js, sizeof(js),
            "(function(z){ try { document.body.style.zoom = z; } catch(e){} })(%f);",
            factor);
        ch->engine->ExecuteScript(ch->tabs[eff].id, js, nullptr, 0);
    }
    PaintChrome(ch);
}

void ZoomIn(HWND hwnd) {
    ChromeState* ch = reinterpret_cast<ChromeState*>(GetWindowLongPtr(hwnd, GWLP_USERDATA));
    if (!ch) return;
    int eff = ch->splitView.IsEnabled() ? ch->splitView.GetActiveTabIndex() : ch->activeTab;
    if (eff < 0 || eff >= (int)ch->tabs.size()) return;
    double current = ch->tabs[eff].zoomFactor > 0.1 ? ch->tabs[eff].zoomFactor : 1.0;
    SetZoom(hwnd, current + 0.1);
}

void ZoomOut(HWND hwnd) {
    ChromeState* ch = reinterpret_cast<ChromeState*>(GetWindowLongPtr(hwnd, GWLP_USERDATA));
    if (!ch) return;
    int eff = ch->splitView.IsEnabled() ? ch->splitView.GetActiveTabIndex() : ch->activeTab;
    if (eff < 0 || eff >= (int)ch->tabs.size()) return;
    double current = ch->tabs[eff].zoomFactor > 0.1 ? ch->tabs[eff].zoomFactor : 1.0;
    SetZoom(hwnd, current - 0.1);
}

void ZoomReset(HWND hwnd) {
    SetZoom(hwnd, 1.0);
}

double GetZoom(HWND hwnd) {
    ChromeState* ch = reinterpret_cast<ChromeState*>(GetWindowLongPtr(hwnd, GWLP_USERDATA));
    if (!ch) return 1.0;
    int eff = ch->splitView.IsEnabled() ? ch->splitView.GetActiveTabIndex() : ch->activeTab;
    if (eff < 0 || eff >= (int)ch->tabs.size()) return 1.0;
    return ch->tabs[eff].zoomFactor > 0.1 ? ch->tabs[eff].zoomFactor : 1.0;
}

// -- Find in Page (Ctrl+F) Implementations ----------------------------
static LRESULT CALLBACK FindBarWndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    ChromeState* ch = reinterpret_cast<ChromeState*>(GetWindowLongPtr(hWnd, GWLP_USERDATA));
    switch (msg) {
    case WM_CREATE: {
        CREATESTRUCTW* cs = reinterpret_cast<CREATESTRUCTW*>(lParam);
        ch = reinterpret_cast<ChromeState*>(cs->lpCreateParams);
        SetWindowLongPtr(hWnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(ch));
        break;
    }
    case WM_ERASEBKGND:
        return 1;
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC hdc = BeginPaint(hWnd, &ps);
        RECT rc; GetClientRect(hWnd, &rc);
        int fW = max(1, (int)(rc.right - rc.left));
        int fH = max(1, (int)(rc.bottom - rc.top));

        HDC memDC = CreateCompatibleDC(hdc);
        HBITMAP memBmp = CreateCompatibleBitmap(hdc, fW, fH);
        HBITMAP oldBmp = (HBITMAP)SelectObject(memDC, memBmp);

        HBRUSH brBg = CreateSolidBrush(RGB(18, 20, 36));
        RECT localRc = { 0, 0, fW, fH };
        FillRect(memDC, &localRc, brBg);
        DeleteObject(brBg);

        HPEN penBorder = CreatePen(PS_SOLID, 1, RGB(147, 51, 234));
        HPEN oldPen = (HPEN)SelectObject(memDC, penBorder);
        HBRUSH oldBr = (HBRUSH)SelectObject(memDC, GetStockObject(NULL_BRUSH));
        RoundRect(memDC, 0, 0, fW, fH, 8, 8);
        SelectObject(memDC, oldBr);
        SelectObject(memDC, oldPen);
        DeleteObject(penBorder);

        if (ch && ch->hTextTabFont) {
            HFONT oldF = (HFONT)SelectObject(memDC, ch->hTextTabFont);
            SetTextColor(memDC, RGB(148, 163, 184));
            SetBkMode(memDC, TRANSPARENT);
            char cntStr[32] = {};
            if (!ch->findQuery.empty()) {
                snprintf(cntStr, sizeof(cntStr), "%d match%s", ch->findMatchCount, ch->findMatchCount == 1 ? "" : "es");
            }
            RECT cntR = { S(ch, 155), 0, S(ch, 235), fH };
            DrawTextA(memDC, cntStr, -1, &cntR, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
            SelectObject(memDC, oldF);
        }

        BitBlt(hdc, 0, 0, fW, fH, memDC, 0, 0, SRCCOPY);
        SelectObject(memDC, oldBmp);
        DeleteObject(memBmp);
        DeleteDC(memDC);
        EndPaint(hWnd, &ps);
        return 0;
    }
    case WM_CTLCOLOREDIT: {
        HDC hdc = (HDC)wParam;
        SetTextColor(hdc, RGB(248, 250, 252));
        SetBkColor(hdc, RGB(26, 29, 53));
        static HBRUSH hBr = CreateSolidBrush(RGB(26, 29, 53));
        return (LRESULT)hBr;
    }
    case WM_COMMAND: {
        int id = LOWORD(wParam);
        if (id == 9051 && HIWORD(wParam) == EN_CHANGE) {
            if (ch && ch->hFindEdit) {
                char buf[256] = {};
                GetWindowTextA(ch->hFindEdit, buf, sizeof(buf));
                ch->findQuery = buf;
                FindNext(ch->hwnd, false);
                InvalidateRect(hWnd, nullptr, FALSE);
            }
        } else if (id == 9052) {
            if (ch) FindNext(ch->hwnd, true);
        } else if (id == 9053) {
            if (ch) FindNext(ch->hwnd, false);
        } else if (id == 9054) {
            if (ch) CloseFindBar(ch->hwnd);
        }
        return 0;
    }
    case WM_DESTROY:
        return 0;
    }
    return DefWindowProcW(hWnd, msg, wParam, lParam);
}

void FindNext(HWND hwnd, bool reverse) {
    ChromeState* ch = reinterpret_cast<ChromeState*>(GetWindowLongPtr(hwnd, GWLP_USERDATA));
    if (!ch || !ch->engine || !ch->engine->ExecuteScript) return;
    int eff = ch->splitView.IsEnabled() ? ch->splitView.GetActiveTabIndex() : ch->activeTab;
    if (eff < 0 || eff >= (int)ch->tabs.size()) return;

    if (ch->findQuery.empty()) {
        ch->findMatchCount = 0;
        ch->engine->ExecuteScript(ch->tabs[eff].id, "try{if(window.getSelection)window.getSelection().removeAllRanges();}catch(e){}", nullptr, 0);
        if (ch->hFindBar) InvalidateRect(ch->hFindBar, nullptr, FALSE);
        return;
    }

    std::string safeQ = "";
    for (char c : ch->findQuery) {
        if (c == '\\') safeQ += "\\\\";
        else if (c == '"') { safeQ += '\\'; safeQ += '"'; }
        else if (c == '\n' || c == '\r') safeQ += ' ';
        else safeQ += c;
    }

    char script[1024];
    snprintf(script, sizeof(script),
        "(function(q, rev){"
        "  try {"
        "    var f = window.find(q, false, rev, true, false, true, false);"
        "    var txt = document.body ? document.body.innerText : '';"
        "    return JSON.stringify({count: f ? 1 : 0, found: f});"
        "  } catch(e) { return JSON.stringify({count: 0}); }"
        "})(\"%s\", %s);",
        safeQ.c_str(), reverse ? "true" : "false");

    char outBuf[256] = {};
    ch->engine->ExecuteScript(ch->tabs[eff].id, script, outBuf, sizeof(outBuf));

    const char* cPtr = strstr(outBuf, "\"count\":");
    if (cPtr) {
        ch->findMatchCount = atoi(cPtr + 8);
    }
    if (ch->hFindBar) InvalidateRect(ch->hFindBar, nullptr, FALSE);
}

void CloseFindBar(HWND hwnd) {
    ChromeState* ch = reinterpret_cast<ChromeState*>(GetWindowLongPtr(hwnd, GWLP_USERDATA));
    if (!ch) return;
    ch->findBarVisible = false;
    if (ch->hFindBar) ShowWindow(ch->hFindBar, SW_HIDE);
    if (ch->engine && ch->engine->ExecuteScript) {
        int eff = ch->splitView.IsEnabled() ? ch->splitView.GetActiveTabIndex() : ch->activeTab;
        if (eff >= 0 && eff < (int)ch->tabs.size()) {
            ch->engine->ExecuteScript(ch->tabs[eff].id, "try{if(window.getSelection)window.getSelection().removeAllRanges();}catch(e){}", nullptr, 0);
        }
    }
    SetFocus(hwnd);
}

void ToggleFindBar(HWND hwnd) {
    ChromeState* ch = reinterpret_cast<ChromeState*>(GetWindowLongPtr(hwnd, GWLP_USERDATA));
    if (!ch) return;
    ch->findBarVisible = !ch->findBarVisible;
    if (ch->findBarVisible) {
        RECT rc; GetClientRect(hwnd, &rc);
        int fbW = 340, fbH = 34;
        int fbX = max(10, rc.right - fbW - 20);
        int fbY = (ch->isFullscreen ? 0 : CHROME_H) + 6;

        if (!ch->hFindBar) {
            HINSTANCE hInst = GetModuleHandleW(nullptr);
            WNDCLASSEXW wc = { sizeof(wc) };
            wc.style = CS_HREDRAW | CS_VREDRAW;
            wc.lpfnWndProc = FindBarWndProc;
            wc.hInstance = hInst;
            wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
            wc.lpszClassName = L"AeonFindBar";
            RegisterClassExW(&wc);

            ch->hFindBar = CreateWindowExW(WS_EX_TOPMOST, L"AeonFindBar", L"",
                WS_CHILD | WS_CLIPCHILDREN, fbX, fbY, fbW, fbH,
                hwnd, nullptr, hInst, ch);

            ch->hFindEdit = CreateWindowExW(0, L"EDIT", L"",
                WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL,
                8, 6, 140, 22, ch->hFindBar, (HMENU)9051, hInst, nullptr);
            if (ch->hTextTabFont) SendMessageW(ch->hFindEdit, WM_SETFONT, (WPARAM)ch->hTextTabFont, TRUE);

            CreateWindowExW(0, L"BUTTON", L"\u25B2",
                WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON | BS_FLAT,
                240, 5, 24, 24, ch->hFindBar, (HMENU)9052, hInst, nullptr);

            CreateWindowExW(0, L"BUTTON", L"\u25BC",
                WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON | BS_FLAT,
                268, 5, 24, 24, ch->hFindBar, (HMENU)9053, hInst, nullptr);

            CreateWindowExW(0, L"BUTTON", L"\u2715",
                WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON | BS_FLAT,
                298, 5, 28, 24, ch->hFindBar, (HMENU)9054, hInst, nullptr);
        }
        SetWindowPos(ch->hFindBar, HWND_TOP, fbX, fbY, fbW, fbH, SWP_SHOWWINDOW);
        ShowWindow(ch->hFindBar, SW_SHOW);
        SetFocus(ch->hFindEdit);
        SendMessageW(ch->hFindEdit, EM_SETSEL, 0, -1);
    } else {
        CloseFindBar(hwnd);
    }
}

// -- Bookmarks Bar Implementations -----------------------------------
void ToggleBookmarkBar(HWND hwnd) {
    ChromeState* ch = reinterpret_cast<ChromeState*>(GetWindowLongPtr(hwnd, GWLP_USERDATA));
    if (!ch) return;
    ch->bookmarkBarVisible = !ch->bookmarkBarVisible;
    BookmarkBar::Show(ch->bookmarkBarVisible);
    SyncViewportInternal(ch);
    PaintChrome(ch);
}

bool IsBookmarkBarVisible(HWND hwnd) {
    ChromeState* ch = reinterpret_cast<ChromeState*>(GetWindowLongPtr(hwnd, GWLP_USERDATA));
    return ch ? ch->bookmarkBarVisible : false;
}

// -- Tab Audio & Muting Implementations ------------------------------
void ToggleTabMute(HWND hwnd, int tabIndex) {
    ChromeState* ch = reinterpret_cast<ChromeState*>(GetWindowLongPtr(hwnd, GWLP_USERDATA));
    if (!ch || tabIndex < 0 || tabIndex >= (int)ch->tabs.size()) return;

    ChromeTab& tab = ch->tabs[tabIndex];
    tab.isMuted = !tab.isMuted;

    if (ch->engine && ch->engine->ExecuteScript) {
        char js[256];
        snprintf(js, sizeof(js),
            "(function(m){ try { document.querySelectorAll('video,audio').forEach(e => e.muted = m); } catch(e){} })(%s);",
            tab.isMuted ? "true" : "false");
        ch->engine->ExecuteScript(tab.id, js, nullptr, 0);
    }
    PaintChrome(ch);
}

void SetTabAudioState(HWND hwnd, unsigned int tab_id, bool isPlaying, bool isMuted) {
    ChromeState* ch = reinterpret_cast<ChromeState*>(GetWindowLongPtr(hwnd, GWLP_USERDATA));
    if (!ch) return;
    for (auto& t : ch->tabs) {
        if (t.id == tab_id) {
            t.isPlayingAudio = isPlaying;
            t.isMuted = isMuted;
            PaintChrome(ch);
            break;
        }
    }
}

} // namespace BrowserChrome
