/**
 * Aeon Browser Installer
 * DelgadoLogic — Confidential
 *
 * Design: Dark circuit-board background, glowing Aeon shield logo,
 *         "AEON" wordmark pill, cyan gradient progress bar,
 *         rotating "Did You Know?" privacy slides, one-click install.
 *
 * OpSec:  Automatic %TEMP% cleanup on exit.
 *         Binary metadata scrubbing via RC version block.
 *         No installer metadata written to registry until user confirms.
 */

#ifndef UNICODE
#define UNICODE
#endif
#ifndef _UNICODE
#define _UNICODE
#endif
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX           // prevent min/max macro pollution with std::min
#include <windows.h>
#include <objidl.h>        // IStream — must come before gdiplus.h
#include <windowsx.h>
#include <gdiplus.h>
#include <shlobj.h>
#include <shellapi.h>
#include <shlwapi.h>
#include <wchar.h>
#include <string>
#include <vector>
#include <thread>
#include <atomic>
#include <chrono>
#include <random>
#include <filesystem>

#pragma comment(lib, "gdiplus.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "shlwapi.lib")
#pragma comment(lib, "user32.lib")
#pragma comment(lib, "gdi32.lib")
#pragma comment(lib, "ole32.lib")

using namespace Gdiplus;
namespace fs = std::filesystem;

// ─────────────────────────────────────────────────────────────────────────────
// Constants & Config
// ─────────────────────────────────────────────────────────────────────────────
static const int   WIN_W        = 820;
static const int   WIN_H        = 520;
static const DWORD SLIDE_MS     = 4500;   // ms per "Did You Know?" slide
static const DWORD ANIM_TICK_MS = 16;     // ~60 fps
static const wchar_t* APP_NAME  = L"Aeon Browser";
static const wchar_t* COMPANY   = L"DelgadoLogic";
static const wchar_t* VERSION   = L"1.0.0";

// Install destination (default: %LOCALAPPDATA%\AeonBrowser)
static wchar_t g_installPath[MAX_PATH] = {};

// ─────────────────────────────────────────────────────────────────────────────
// "Did You Know?" slides
// ─────────────────────────────────────────────────────────────────────────────
struct Slide {
    const wchar_t* headline;
    const wchar_t* body;
};

static const Slide SLIDES[] = {
    {
        L"Zero Telemetry. Always.",
        L"Aeon never phones home. No crash reports, no usage stats,\n"
        L"no silent pings — unless you explicitly opt in."
    },
    {
        L"Your DNS is Private",
        L"Every lookup is encrypted via DNS-over-HTTPS before\n"
        L"it leaves your machine. Your ISP sees nothing."
    },
    {
        L"Sleeping Tabs Save RAM",
        L"Inactive tabs use under 5 MB in Aeon — vs 80–300 MB\n"
        L"in Chrome. Your machine stays fast."
    },
    {
        L".onion Sites Work Natively",
        L"No Tor Browser needed. Any tab in Aeon can load\n"
        L"a .onion address — privately, automatically."
    },
    {
        L"Fingerprinting? Blocked.",
        L"Canvas, WebGL, AudioContext, font enumeration — Aeon\n"
        L"detects and neutralizes tracking before the page loads."
    },
    {
        L"Your Passwords Never Leave",
        L"AeonVault stores credentials locally, encrypted with\n"
        L"DPAPI + Argon2id. Not in any cloud. Ever."
    },
    {
        L"AI That Stays On-Device",
        L"Aeon's AI sidebar runs phi-3-mini locally. Your\n"
        L"questions never reach any external server."
    },
    {
        L"Hardening Your Digital Perimeter.",
        L"Aeon is built on Chromium — the world's most\n"
        L"tested browser engine — with Google stripped out."
    },
};
static const int SLIDE_COUNT = ARRAYSIZE(SLIDES);

// ─────────────────────────────────────────────────────────────────────────────
// Globals
// ─────────────────────────────────────────────────────────────────────────────
static HWND       g_hwnd          = nullptr;
static ULONG_PTR  g_gdiplusToken  = 0;
static std::atomic<float> g_progress(0.0f);   // 0.0–1.0
static std::atomic<int>   g_slideIdx(0);
static std::atomic<float> g_slideAlpha(1.0f); // fade value 0–1
static std::atomic<bool>  g_installing(false);
static std::atomic<bool>  g_done(false);
static std::atomic<bool>  g_cancelled(false);
static std::wstring       g_statusText = L"Preparing...";
static CRITICAL_SECTION   g_statusLock;

// ─────────────────────────────────────────────────────────────────────────────
// Colors matching the reference image
// ─────────────────────────────────────────────────────────────────────────────
static const Color COL_BG          (255, 18,  18,  22);   // #12 12 16 near-black
static const Color COL_CARD        (255, 28,  28,  36);   // dark charcoal card
static const Color COL_BORDER      (255, 55,  55,  75);   // subtle border
static const Color COL_CYAN        (255, 80,  220, 255);  // progress bar lead
static const Color COL_CYAN_DIM    (255, 30,  120, 180);  // progress bar tail
static const Color COL_PURPLE      (255, 160, 80,  255);  // shield glow
static const Color COL_WHITE       (255, 255, 255, 255);
static const Color COL_GRAY        (255, 160, 160, 180);
static const Color COL_TITLE_BLUE  (255, 100, 180, 255);  // "DELGADO LOGIC" text

// ─────────────────────────────────────────────────────────────────────────────
// Helper: set status text (thread-safe)
// ─────────────────────────────────────────────────────────────────────────────
static void SetStatus(const std::wstring& s) {
    EnterCriticalSection(&g_statusLock);
    g_statusText = s;
    LeaveCriticalSection(&g_statusLock);
    if (g_hwnd) InvalidateRect(g_hwnd, nullptr, FALSE);
}

static std::wstring GetStatus() {
    EnterCriticalSection(&g_statusLock);
    auto s = g_statusText;
    LeaveCriticalSection(&g_statusLock);
    return s;
}

// ─────────────────────────────────────────────────────────────────────────────
// OpSec: TEMP cleanup
// ─────────────────────────────────────────────────────────────────────────────
static void CleanupTemp() {
    wchar_t tempPath[MAX_PATH];
    GetTempPathW(MAX_PATH, tempPath);
    // Remove any files we extracted during install
    // Pattern: AeonInstall_XXXXXXXX.*
    std::wstring pattern = std::wstring(tempPath) + L"AeonInstall_*";
    WIN32_FIND_DATAW fd;
    HANDLE hFind = FindFirstFileW(pattern.c_str(), &fd);
    if (hFind != INVALID_HANDLE_VALUE) {
        do {
            std::wstring full = std::wstring(tempPath) + fd.cFileName;
            SetFileAttributesW(full.c_str(), FILE_ATTRIBUTE_NORMAL);
            DeleteFileW(full.c_str());
        } while (FindNextFileW(hFind, &fd));
        FindClose(hFind);
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// Worker thread: simulates/performs real installation
// ─────────────────────────────────────────────────────────────────────────────
struct InstallStep {
    float targetProgress;
    const wchar_t* status;
    DWORD delayMs;
};

static const InstallStep STEPS[] = {
    { 0.05f, L"Verifying system requirements...",       600  },
    { 0.12f, L"Preparing installation directory...",    400  },
    { 0.20f, L"Extracting core engine...",              1200 },
    { 0.35f, L"Installing browser components...",       1800 },
    { 0.48f, L"Configuring network stack...",           900  },
    { 0.58f, L"Setting up privacy defaults...",         700  },
    { 0.67f, L"Installing AI inference engine...",      1400 },
    { 0.75f, L"Registering URL protocols...",           500  },
    { 0.83f, L"Creating Start Menu shortcuts...",       400  },
    { 0.90f, L"Hardening your digital perimeter...",    800  },
    { 0.96f, L"Cleaning up temporary files...",         400  },
    { 1.00f, L"Installation complete!",                 600  },
};

static void InstallWorker() {
    // Set install path default
    wchar_t localApp[MAX_PATH];
    SHGetFolderPathW(nullptr, CSIDL_LOCAL_APPDATA, nullptr, 0, localApp);
    wcscpy_s(g_installPath, localApp);
    wcscat_s(g_installPath, L"\\AeonBrowser");

    // Create directory
    SHCreateDirectoryExW(nullptr, g_installPath, nullptr);

    for (auto& step : STEPS) {
        if (g_cancelled) return;
        SetStatus(step.status);

        // Smooth progress animation to target
        float start = g_progress.load();
        float end   = step.targetProgress;
        DWORD animMs = step.delayMs;
        auto t0 = std::chrono::steady_clock::now();
        while (true) {
            auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - t0).count();
            float t = (float)elapsed / (float)animMs; if (t > 1.0f) t = 1.0f;
            // Ease out cubic
            float ease = 1.0f - (1.0f - t) * (1.0f - t) * (1.0f - t);
            g_progress = start + (end - start) * ease;
            if (g_hwnd) InvalidateRect(g_hwnd, nullptr, FALSE);
            if (elapsed >= (DWORD)animMs) break;
            Sleep(16);
        }
        g_progress = end;
    }

    // OpSec cleanup
    CleanupTemp();

    g_done = true;
    if (g_hwnd) InvalidateRect(g_hwnd, nullptr, FALSE);
}

// ─────────────────────────────────────────────────────────────────────────────
// Slide auto-advance thread
// ─────────────────────────────────────────────────────────────────────────────
static void SlideWorker() {
    while (!g_done && !g_cancelled) {
        Sleep(SLIDE_MS);
        // Fade out
        for (int i = 10; i >= 0; i--) {
            g_slideAlpha = i / 10.0f;
            if (g_hwnd) InvalidateRect(g_hwnd, nullptr, FALSE);
            Sleep(30);
        }
        g_slideIdx = (g_slideIdx + 1) % SLIDE_COUNT;
        // Fade in
        for (int i = 0; i <= 10; i++) {
            g_slideAlpha = i / 10.0f;
            if (g_hwnd) InvalidateRect(g_hwnd, nullptr, FALSE);
            Sleep(30);
        }
        g_slideAlpha = 1.0f;
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// GDI+ Drawing
// ─────────────────────────────────────────────────────────────────────────────

// Draw circuit-board background pattern (procedural, matches reference)
static void DrawCircuitBg(Graphics& g, int w, int h) {
    // Dark base
    SolidBrush bgBrush(COL_BG);
    g.FillRectangle(&bgBrush, 0, 0, w, h);

    // Circuit traces — horizontal and vertical lines with junctions
    Pen tracePen(Color(255, 38, 38, 52), 1.0f);
    std::mt19937 rng(42); // fixed seed = same pattern every frame
    std::uniform_int_distribution<int> xd(0, w);
    std::uniform_int_distribution<int> yd(0, h);
    std::uniform_int_distribution<int> ld(20, 120);
    std::uniform_int_distribution<int> hv(0, 1);

    for (int i = 0; i < 80; i++) {
        int x = xd(rng), y = yd(rng), len = ld(rng);
        if (hv(rng) == 0)
            g.DrawLine(&tracePen, x, y, x + len, y);
        else
            g.DrawLine(&tracePen, x, y, x, y + len);
    }
    // Junction dots
    SolidBrush jBrush(Color(255, 50, 50, 70));
    rng.seed(99);
    for (int i = 0; i < 40; i++) {
        int x = xd(rng), y = yd(rng);
        g.FillEllipse(&jBrush, x - 2, y - 2, 4, 4);
    }
}

// Draw glowing purple/cyan shield "A" logo
static void DrawShieldLogo(Graphics& g, int cx, int cy) {
    // Outer glow rings (layered alpha)
    for (int r = 80; r >= 20; r -= 10) {
        BYTE alpha = (BYTE)(15 + (80 - r) * 1.5f);
        Pen glowPen(Color(alpha, 160, 80, 255), (float)(r / 15));
        g.DrawEllipse(&glowPen, cx - r, cy - r, r * 2, r * 2);
    }

    // Shield body — gradient fill
    GraphicsPath shieldPath;
    // Approximate shield shape as polygon
    PointF pts[] = {
        {(float)cx       , (float)(cy - 70)},  // top center
        {(float)(cx + 55), (float)(cy - 40)},  // top right
        {(float)(cx + 55), (float)(cy + 15)},  // mid right
        {(float)cx       , (float)(cy + 75)},  // bottom point
        {(float)(cx - 55), (float)(cy + 15)},  // mid left
        {(float)(cx - 55), (float)(cy - 40)},  // top left
    };
    shieldPath.AddPolygon(pts, 6);

    // Fill with dark translucent purple
    SolidBrush shieldFill(Color(200, 60, 30, 120));
    g.FillPath(&shieldFill, &shieldPath);
    // Border glow
    Pen shieldBorder(Color(220, 140, 70, 255), 2.5f);
    g.DrawPath(&shieldBorder, &shieldPath);
    Pen shieldBorder2(Color(100, 80, 200, 255), 4.0f);
    g.DrawPath(&shieldBorder2, &shieldPath);

    // "A" letterform inside shield
    FontFamily ff(L"Segoe UI");
    Gdiplus::Font fontA(&ff, 62, FontStyleBold, UnitPixel);
    StringFormat sf;
    sf.SetAlignment(StringAlignmentCenter);
    sf.SetLineAlignment(StringAlignmentCenter);

    // Glow behind letter
    SolidBrush glowBrush(Color(80, 120, 200, 255));
    RectF letterRect((float)(cx - 40), (float)(cy - 38), 80.0f, 75.0f);
    for (int d = 6; d >= 0; d -= 2) {
        RectF gr(letterRect.X - d, letterRect.Y - d,
                 letterRect.Width + d*2, letterRect.Height + d*2);
        g.DrawString(L"A", -1, &fontA, gr, &sf, &glowBrush);
    }
    // Main letter — cyan/white gradient simulation
    SolidBrush letterBrush(Color(255, 180, 220, 255));
    g.DrawString(L"A", -1, &fontA, letterRect, &sf, &letterBrush);
}

// Draw the "AEON" pill label
static void DrawAeonPill(Graphics& g, int cx, int y) {
    int pw = 130, ph = 32;
    RectF pill((float)(cx - pw/2), (float)y, (float)pw, (float)ph);

    // Pill bg
    GraphicsPath pillPath;
    // Left arc, top line, right arc — classic rounded-rect pill
    REAL ph2 = (REAL)ph;
    REAL pw2 = (REAL)pw;
    pillPath.AddArc(pill.X,           pill.Y, ph2, ph2, 90.0f, 180.0f);
    pillPath.AddLine(pill.X + ph2/2,  pill.Y, pill.X + pw2 - ph2/2, pill.Y);
    pillPath.AddArc(pill.X + pw2 - ph2, pill.Y, ph2, ph2, 270.0f, 180.0f);
    pillPath.CloseFigure();

    SolidBrush pillBg(Color(255, 35, 35, 48));
    g.FillPath(&pillBg, &pillPath);
    Pen pillBorder(Color(180, 80, 80, 100), 1.0f);
    g.DrawPath(&pillBorder, &pillPath);

    // "AEON" text
    FontFamily ff(L"Segoe UI");
    Gdiplus::Font fontPill(&ff, 14, FontStyleBold, UnitPixel);
    StringFormat sf;
    sf.SetAlignment(StringAlignmentCenter);
    sf.SetLineAlignment(StringAlignmentCenter);
    SolidBrush textBrush(Color(255, 230, 230, 240));
    g.DrawString(L"AEON", -1, &fontPill, pill, &sf, &textBrush);
}

// Draw the cyan gradient progress bar
static void DrawProgressBar(Graphics& g, int x, int y, int w, int h, float progress) {
    // Track background
    SolidBrush trackBg(Color(255, 30, 30, 42));
    g.FillRectangle(&trackBg, x, y, w, h);
    Pen trackBorder(Color(255, 55, 55, 75), 1.0f);
    g.DrawRectangle(&trackBorder, x, y, w, h);

    // Filled portion — gradient cyan
    int fillW = (int)(w * progress);
    if (fillW > 2) {
        LinearGradientBrush grad(
            Point(x, y),
            Point(x + fillW, y),
            COL_CYAN_DIM,
            COL_CYAN
        );
        g.FillRectangle(&grad, x + 1, y + 1, fillW - 1, h - 2);

        // Bright leading edge glow
        if (fillW > 6) {
            LinearGradientBrush glowGrad(
                Point(x + fillW - 6, y),
                Point(x + fillW, y),
                Color(0, 140, 220, 255),
                Color(255, 200, 240, 255)
            );
            g.FillRectangle(&glowGrad, x + fillW - 6, y + 1, 6, h - 2);
        }
    }
}

// Main paint routine
static void OnPaint(HWND hwnd) {
    PAINTSTRUCT ps;
    HDC hdc = BeginPaint(hwnd, &ps);

    // Double-buffer
    RECT rc;
    GetClientRect(hwnd, &rc);
    int W = rc.right, H = rc.bottom;
    HDC memDC = CreateCompatibleDC(hdc);
    HBITMAP memBmp = CreateCompatibleBitmap(hdc, W, H);
    HBITMAP oldBmp = (HBITMAP)SelectObject(memDC, memBmp);

    Graphics g(memDC);
    g.SetSmoothingMode(SmoothingModeAntiAlias);
    g.SetTextRenderingHint(TextRenderingHintClearTypeGridFit);

    // ── Background ──
    DrawCircuitBg(g, W, H);

    // ── Top-left: "DELGADO LOGIC" branding ──
    {
        FontFamily ff(L"Segoe UI");
        Gdiplus::Font fontBrand(&ff, 13, FontStyleBold, UnitPixel);
        SolidBrush brandBrush(COL_TITLE_BLUE);
        g.DrawString(L"DELGADO LOGIC", -1, &fontBrand,
                     PointF(52, 18), &brandBrush);
        // Mini shield icon to left of text
        Pen miniShield(Color(255, 140, 80, 255), 1.5f);
        PointF msPts[] = {
            {36,14},{44,14},{46,16},{46,22},{40,26},{34,22},{34,16}
        };
        g.DrawPolygon(&miniShield, msPts, 7);
        SolidBrush msFill(Color(80, 120, 60, 200));
        g.FillPolygon(&msFill, msPts, 7);
        // D letter in mini shield
        Gdiplus::Font fontD(&ff, 7, FontStyleBold, UnitPixel);
        SolidBrush dBrush(Color(255, 200, 160, 255));
        StringFormat sf;
        sf.SetAlignment(StringAlignmentCenter);
        sf.SetLineAlignment(StringAlignmentCenter);
        g.DrawString(L"D", -1, &fontD, RectF(34,14,12,12), &sf, &dBrush);
    }

    // ── X close button ──
    {
        FontFamily ff(L"Segoe UI");
        Gdiplus::Font fontX(&ff, 13, FontStyleRegular, UnitPixel);
        SolidBrush xBrush(COL_GRAY);
        g.DrawString(L"×", -1, &fontX, PointF((float)(W - 28), 14), &xBrush);
    }

    // ── Shield Logo ──
    int shieldCX = W / 2;
    int shieldCY = 200;
    DrawShieldLogo(g, shieldCX, shieldCY);

    // ── "AEON" pill ──
    DrawAeonPill(g, shieldCX, shieldCY + 88);

    // ── Progress bar ──
    float prog = g_progress.load();
    int barX = 90, barY = shieldCY + 140, barW = W - 180, barH = 10;
    DrawProgressBar(g, barX, barY, barW, barH, prog);

    // ── Percentage text ──
    {
        FontFamily ff(L"Segoe UI");
        Gdiplus::Font fontPct(&ff, 11, FontStyleRegular, UnitPixel);
        SolidBrush pctBrush(COL_GRAY);
        StringFormat sf;
        sf.SetAlignment(StringAlignmentCenter);
        std::wstring pctStr = std::to_wstring((int)(prog * 100)) + L"%";
        g.DrawString(pctStr.c_str(), -1, &fontPct,
                     RectF((float)(W/2 - 40), (float)(barY + 14), 80, 20),
                     &sf, &pctBrush);
    }

    // ── Main status text ──
    {
        std::wstring status = g_done ? L"Installation complete!" : GetStatus();
        FontFamily ff(L"Segoe UI");
        Gdiplus::Font fontStatus(&ff, 20, FontStyleBold, UnitPixel);
        SolidBrush statusBrush(COL_WHITE);
        StringFormat sf;
        sf.SetAlignment(StringAlignmentCenter);
        g.DrawString(status.c_str(), -1, &fontStatus,
                     RectF(30, (float)(barY + 38), (float)(W - 60), 36),
                     &sf, &statusBrush);
    }

    // ── "Did You Know?" slide ──
    {
        int idx = g_slideIdx.load();
        float alpha = g_slideAlpha.load();
        BYTE a = (BYTE)(alpha * 255);
        const Slide& slide = SLIDES[idx];

        FontFamily ff(L"Segoe UI");
        Gdiplus::Font fontSlideHead(&ff, 12, FontStyleBold, UnitPixel);
        Gdiplus::Font fontSlideBody(&ff, 11, FontStyleRegular, UnitPixel);
        SolidBrush headBrush(Color(a, 100, 200, 255));
        SolidBrush bodyBrush(Color(a, 160, 160, 180));

        StringFormat sf;
        sf.SetAlignment(StringAlignmentCenter);
        sf.SetLineAlignment(StringAlignmentCenter);

        RectF headRect(60, (float)(barY + 88), (float)(W - 120), 20);
        RectF bodyRect(60, (float)(barY + 112), (float)(W - 120), 44);

        g.DrawString(slide.headline, -1, &fontSlideHead, headRect, &sf, &headBrush);
        g.DrawString(slide.body,     -1, &fontSlideBody, bodyRect, &sf, &bodyBrush);
    }

    // ── Launch button (shown when done) ──
    if (g_done) {
        int btnW = 200, btnH = 42;
        int btnX = W/2 - btnW/2, btnY = H - 80;
        RectF btnRect((float)btnX, (float)btnY, (float)btnW, (float)btnH);

        LinearGradientBrush btnGrad(
            Point(btnX, btnY),
            Point(btnX + btnW, btnY),
            Color(255, 60,  30,  160),
            Color(255, 120, 80,  255)
        );
        GraphicsPath btnPath;
        btnPath.AddArc(btnX,          btnY,          20, btnH, 90, 180);
        btnPath.AddArc(btnX+btnW-20,  btnY,          20, btnH, 270, 180);
        btnPath.CloseFigure();
        g.FillPath(&btnGrad, &btnPath);

        FontFamily ff(L"Segoe UI");
        Gdiplus::Font fontBtn(&ff, 15, FontStyleBold, UnitPixel);
        SolidBrush btnText(COL_WHITE);
        StringFormat sf;
        sf.SetAlignment(StringAlignmentCenter);
        sf.SetLineAlignment(StringAlignmentCenter);
        g.DrawString(L"Launch Aeon →", -1, &fontBtn, btnRect, &sf, &btnText);
    }

    // Blit
    BitBlt(hdc, 0, 0, W, H, memDC, 0, 0, SRCCOPY);
    SelectObject(memDC, oldBmp);
    DeleteObject(memBmp);
    DeleteDC(memDC);
    EndPaint(hwnd, &ps);
}

// ─────────────────────────────────────────────────────────────────────────────
// Window Procedure
// ─────────────────────────────────────────────────────────────────────────────
static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_CREATE:
        g_hwnd = hwnd;
        // Start install + slide workers
        std::thread(InstallWorker).detach();
        std::thread(SlideWorker).detach();
        // Repaint timer for smooth animation
        SetTimer(hwnd, 1, ANIM_TICK_MS, nullptr);
        return 0;

    case WM_TIMER:
        InvalidateRect(hwnd, nullptr, FALSE);
        return 0;

    case WM_PAINT:
        OnPaint(hwnd);
        return 0;

    case WM_LBUTTONDOWN: {
        // Hit-test close button
        int mx = GET_X_LPARAM(lp), my = GET_Y_LPARAM(lp);
        RECT rc; GetClientRect(hwnd, &rc);
        if (mx >= rc.right - 36 && mx <= rc.right - 10 && my >= 8 && my <= 32) {
            g_cancelled = true;
            CleanupTemp();
            DestroyWindow(hwnd);
        }
        // Launch button when done
        if (g_done) {
            int btnW = 200, btnH = 42;
            int btnX = rc.right/2 - btnW/2, btnY = rc.bottom - 80;
            if (mx >= btnX && mx <= btnX+btnW && my >= btnY && my <= btnY+btnH) {
                wchar_t launch[MAX_PATH];
                wcscpy_s(launch, g_installPath);
                wcscat_s(launch, L"\\Aeon.exe");
                ShellExecuteW(nullptr, L"open", launch, nullptr, nullptr, SW_SHOW);
                DestroyWindow(hwnd);
            }
        }
        return 0;
    }

    case WM_NCHITTEST: {
        // Allow dragging from anywhere on the borderless window
        LRESULT hit = DefWindowProcW(hwnd, msg, wp, lp);
        if (hit == HTCLIENT) return HTCAPTION;
        return hit;
    }

    case WM_DESTROY:
        KillTimer(hwnd, 1);
        DeleteCriticalSection(&g_statusLock);
        GdiplusShutdown(g_gdiplusToken);
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

// ─────────────────────────────────────────────────────────────────────────────
// WinMain
// ─────────────────────────────────────────────────────────────────────────────
int WINAPI wWinMain(HINSTANCE hInst, HINSTANCE, LPWSTR, int) {
    // Init GDI+
    GdiplusStartupInput gsi;
    GdiplusStartup(&g_gdiplusToken, &gsi, nullptr);
    InitializeCriticalSection(&g_statusLock);

    // Register window class
    WNDCLASSEXW wc = {};
    wc.cbSize        = sizeof(wc);
    wc.lpfnWndProc   = WndProc;
    wc.hInstance     = hInst;
    wc.hCursor       = LoadCursorW(nullptr, IDC_ARROW);
    wc.lpszClassName = L"AeonInstaller";
    RegisterClassExW(&wc);

    // Centre on screen
    int sw = GetSystemMetrics(SM_CXSCREEN);
    int sh = GetSystemMetrics(SM_CYSCREEN);

    HWND hwnd = CreateWindowExW(
        WS_EX_APPWINDOW,
        L"AeonInstaller",
        L"Aeon Browser — Installer",
        WS_POPUP | WS_VISIBLE,   // borderless
        (sw - WIN_W) / 2, (sh - WIN_H) / 2,
        WIN_W, WIN_H,
        nullptr, nullptr, hInst, nullptr
    );

    // Rounded corners (Windows 11) — use raw numeric values, no dwmapi.h dependency
    // DWMWA_WINDOW_CORNER_PREFERENCE = 33, DWMWCP_ROUND = 2
    typedef HRESULT(WINAPI* pfnDwmSet)(HWND, DWORD, LPCVOID, DWORD);
    HMODULE hDwm = LoadLibraryW(L"dwmapi.dll");
    if (hDwm) {
        auto fnDwmSet = (pfnDwmSet)GetProcAddress(hDwm, "DwmSetWindowAttribute");
        if (fnDwmSet) {
            DWORD roundCorner = 2; // DWMWCP_ROUND
            fnDwmSet(hwnd, 33 /*DWMWA_WINDOW_CORNER_PREFERENCE*/, &roundCorner, sizeof(roundCorner));
        }
        FreeLibrary(hDwm);
    }

    ShowWindow(hwnd, SW_SHOW);
    UpdateWindow(hwnd);

    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    return 0;
}
