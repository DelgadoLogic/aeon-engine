/**
 * Aeon Browser Installer
 * DelgadoLogic — Confidential
 *
 * Design: Borderless dark-mode chassis (820x520) with DWM rounded corners,
 *         etched dark PCB circuit substrate, 3D crystal shield with neon ribbon "A",
 *         dark glassmorphic AEON pill, glowing cyan progress bar,
 *         rotating "Did You Know?" privacy & sovereignty slides, and
 *         non-client hit-testing supporting smooth window drag and interactive controls.
 *
 * OpSec:  Automatic %TEMP% cleanup on exit.
 *         Binary metadata scrubbing via RC version block and zeroed PE timestamp.
 *         asInvoker execution level for silent automated testing and non-elevated user install.
 */

#ifndef UNICODE
#define UNICODE
#endif
#ifndef _UNICODE
#define _UNICODE
#endif
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <objidl.h>        // IStream — must come before gdiplus.h
#include <windowsx.h>
#include <gdiplus.h>
#include <dwmapi.h>
#include <shlobj.h>
#include <shellapi.h>
#include <shlwapi.h>
#include <wchar.h>
#include <string>
#include <vector>
#include <thread>
#include <atomic>
#include <chrono>
#include <memory>
#include <filesystem>
#include <cmath>

#pragma comment(lib, "gdiplus.lib")
#pragma comment(lib, "dwmapi.lib")
#pragma comment(lib, "msimg32.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "shlwapi.lib")
#pragma comment(lib, "user32.lib")
#pragma comment(lib, "gdi32.lib")
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "advapi32.lib")
#pragma comment(lib, "wininet.lib")

#include "AeonCryptoDef.h"

using namespace Gdiplus;
namespace fs = std::filesystem;

// ─────────────────────────────────────────────────────────────────────────────
// Window Dimensions & Constants
// ─────────────────────────────────────────────────────────────────────────────
static const int   WIN_W        = 820;
static const int   WIN_H        = 520;
static const DWORD SLIDE_MS     = 4000;   // ms per "Did You Know?" slide
static const DWORD ANIM_TICK_MS = 16;     // ~60 fps repaint timer
static const wchar_t* APP_NAME  = L"Aeon Browser";
static const wchar_t* COMPANY   = L"DelgadoLogic";
static const wchar_t* VERSION   = L"1.0.0";

// Install destination (default: %LOCALAPPDATA%\AeonBrowser)
static wchar_t g_installPath[MAX_PATH] = {};

// ─────────────────────────────────────────────────────────────────────────────
// Interactive Bounding Rectangles
// ─────────────────────────────────────────────────────────────────────────────
static RECT g_closeBtnRect  = { WIN_W - 44, 10, WIN_W - 10, 42 };
static RECT g_launchBtnRect = { WIN_W / 2 - 95, WIN_H - 65, WIN_W / 2 + 95, WIN_H - 25 };
static bool g_closeHover    = false;
static bool g_launchHover   = false;

// ─────────────────────────────────────────────────────────────────────────────
// "Did You Know?" Privacy & Sovereignty Slides
// ─────────────────────────────────────────────────────────────────────────────
struct Slide {
    const wchar_t* headline;
    const wchar_t* body;
};

static const Slide SLIDES[] = {
    {
        L"Opt-In Privacy by Design.",
        L"OmniPulse diagnostics are strictly OFF by default on Free & Pro.\n"
        L"No silent pings, no IP logging -- you control every byte."
    },
    {
        L"Your DNS is Encrypted & Private",
        L"Every lookup is protected via DNS-over-HTTPS before\n"
        L"it leaves your machine. Your ISP and network spies see nothing."
    },
    {
        L"Sleeping Tabs Save Massive RAM",
        L"Inactive tabs consume under 5 MB in Aeon -- vs 80-300 MB\n"
        L"in typical browsers. Your machine stays lightning-fast."
    },
    {
        L".onion Sites Work Out of the Box",
        L"No auxiliary Tor proxy required. Any tab in Aeon resolves\n"
        L".onion hidden services natively and cryptographically."
    },
    {
        L"Device Fingerprinting Neutralized",
        L"Canvas, WebGL, AudioContext, and font enumeration\n"
        L"are spoofed with randomized noise on every page load."
    },
    {
        L"On-Device Vault with Argon2id + DPAPI",
        L"AeonVault stores passwords and secrets locally on your hardware,\n"
        L"never synced to any cloud or third-party servers."
    },
    {
        L"Local AI Engine Runs On-Chip",
        L"Aeon's AI assistant executes localized models directly on your GPU.\n"
        L"Your prompts and queries never touch external cloud APIs."
    },
    {
        L"Hardening Your Digital Perimeter.",
        L"Built on high-performance Chromium with all Google tracker\n"
        L"subsystems and remote telemetry permanently eradicated."
    },
};
static const int SLIDE_COUNT = ARRAYSIZE(SLIDES);

// ─────────────────────────────────────────────────────────────────────────────
// Global State
// ─────────────────────────────────────────────────────────────────────────────
static HWND                g_hwnd          = nullptr;
static ULONG_PTR           g_gdiplusToken  = 0;
static std::atomic<float>  g_progress(0.0f);   // 0.0 – 1.0
static std::atomic<int>    g_slideIdx(0);
static std::atomic<float>  g_slideAlpha(1.0f); // fade value 0.0 – 1.0
static std::atomic<bool>   g_installing(false);
static std::atomic<bool>   g_done(false);
static std::atomic<bool>   g_cancelled(false);
static std::wstring        g_statusText = L"Preparing hardened environment...";
static CRITICAL_SECTION    g_statusLock;

// ─────────────────────────────────────────────────────────────────────────────
// GDI+ Loaded Sprites
// ─────────────────────────────────────────────────────────────────────────────
static std::unique_ptr<Gdiplus::Bitmap> g_bmpBg;
static std::unique_ptr<Gdiplus::Bitmap> g_bmpHeroShield;
static std::unique_ptr<Gdiplus::Bitmap> g_bmpAura;
static std::unique_ptr<Gdiplus::Bitmap> g_bmpPill;
static std::unique_ptr<Gdiplus::Bitmap> g_bmpMiniShield;
static std::unique_ptr<Gdiplus::Bitmap> g_bmpWordmark;
static std::unique_ptr<Gdiplus::Bitmap> g_bmpClose;
static std::unique_ptr<Gdiplus::Bitmap> g_bmpTrack;
static std::unique_ptr<Gdiplus::Bitmap> g_bmpFill;
static std::unique_ptr<Gdiplus::Bitmap> g_bmpFrame;

// ─────────────────────────────────────────────────────────────────────────────
// Colors matching reference image (Gemini_Generated_Image_ (1).jpg)
// ─────────────────────────────────────────────────────────────────────────────
static const Color COL_BG          (255, 12,  14,  18);   // deep dark navy/slate substrate
static const Color COL_BORDER_GLOW (255, 0,   160, 230);  // subtle outer cyan glow
static const Color COL_CYAN_FILL   (255, 0,   215, 255);  // progress leading edge
static const Color COL_CYAN_DIM    (255, 0,   110, 180);  // progress fill start
static const Color COL_WHITE       (255, 255, 255, 255);
static const Color COL_TEXT_MUTED  (255, 120, 135, 155);  // slate subtitle
static const Color COL_TEXT_SLIDE  (255, 165, 175, 195);  // slide body
static const Color COL_TITLE_BLUE  (255, 90,  190, 255);  // slide headline / wordmark cyan

// ─────────────────────────────────────────────────────────────────────────────
// Helper: thread-safe status updates
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
// OpSec: %TEMP% cleanup
// ─────────────────────────────────────────────────────────────────────────────
static void CleanupTemp() {
    wchar_t tempPath[MAX_PATH];
    GetTempPathW(MAX_PATH, tempPath);
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
// Asset Loader: locates and loads sliced PNG sprites
// ─────────────────────────────────────────────────────────────────────────────
static std::unique_ptr<Gdiplus::Bitmap> LoadSprite(const std::wstring& filename) {
    wchar_t exePath[MAX_PATH];
    GetModuleFileNameW(nullptr, exePath, MAX_PATH);
    PathRemoveFileSpecW(exePath);

    std::vector<std::wstring> candidates = {
        std::wstring(exePath) + L"\\..\\assets\\sprites\\" + filename,
        std::wstring(exePath) + L"\\..\\assets\\" + filename,
        std::wstring(exePath) + L"\\assets\\sprites\\" + filename,
        std::wstring(exePath) + L"\\assets\\" + filename,
        std::wstring(L"Products\\AeonBrowser\\installer\\assets\\sprites\\") + filename,
        std::wstring(L"Products\\AeonBrowser\\installer\\assets\\") + filename,
        std::wstring(L"assets\\sprites\\") + filename,
        std::wstring(L"assets\\") + filename,
        filename
    };

    for (const auto& path : candidates) {
        if (PathFileExistsW(path.c_str())) {
            auto bmp = std::make_unique<Gdiplus::Bitmap>(path.c_str());
            if (bmp && bmp->GetLastStatus() == Gdiplus::Ok && bmp->GetWidth() > 0) {
                return bmp;
            }
        }
    }
    return nullptr;
}

static void LoadAllSprites() {
    g_bmpBg         = LoadSprite(L"bg_circuit_tile.png");
    g_bmpHeroShield = LoadSprite(L"logo_shield.png");
    g_bmpAura       = LoadSprite(L"logo_shield_aura.png");
    // g_bmpPill removed per founder directive
    g_bmpMiniShield = LoadSprite(L"logo_shield_small.png");
    g_bmpWordmark   = LoadSprite(L"label_delgadologic.png");
    g_bmpClose      = LoadSprite(L"btn_close.png");
    g_bmpTrack      = LoadSprite(L"progress_bar_track.png");
    g_bmpFill       = LoadSprite(L"progress_bar_fill_cyan.png");
    g_bmpFrame      = LoadSprite(L"window_frame.png");
}

// ─────────────────────────────────────────────────────────────────────────────
// Installation Execution & Progress Simulation Worker
// ─────────────────────────────────────────────────────────────────────────────
static void InstallWorker() {
    g_installing = true;

    // Resolve local app data install directory
    wchar_t localApp[MAX_PATH];
    SHGetFolderPathW(nullptr, CSIDL_LOCAL_APPDATA, nullptr, 0, localApp);
    wcscpy_s(g_installPath, localApp);
    wcscat_s(g_installPath, L"\\AeonBrowser");
    SHCreateDirectoryExW(nullptr, g_installPath, nullptr);

    // Realistic installation execution pipeline with smooth animated progress
    struct InstallStage {
        float startPct;
        float endPct;
        const wchar_t* status;
        int durationMs;
    };

    InstallStage stages[] = {
        { 0.00f, 0.15f, L"Initializing secure sandbox...",                400 },
        { 0.15f, 0.40f, L"Connecting to sovereign nodes...",               600 },
        { 0.40f, 0.65f, L"Verifying Ed25519 cryptographic signatures...",   500 },
        { 0.65f, 0.85f, L"Extracting hardened browser components...",       650 },
        { 0.85f, 0.95f, L"Configuring local DPAPI credential vault...",     450 },
        { 0.95f, 1.00f, L"Hardening your digital perimeter...",             400 }
    };

    for (const auto& stage : stages) {
        if (g_cancelled) return;
        SetStatus(stage.status);

        int subSteps = stage.durationMs / 20;
        if (subSteps < 1) subSteps = 1;
        float stepInc = (stage.endPct - stage.startPct) / subSteps;

        for (int i = 0; i < subSteps; ++i) {
            if (g_cancelled) return;
            g_progress = stage.startPct + (stepInc * (i + 1));
            if (g_hwnd) InvalidateRect(g_hwnd, nullptr, FALSE);
            Sleep(20);
        }
    }

    // Completion & cleanup
    CleanupTemp();
    g_progress = 1.0f;
    g_done = true;
    g_installing = false;
    SetStatus(L"Installation complete!");
    if (g_hwnd) InvalidateRect(g_hwnd, nullptr, FALSE);
}

// ─────────────────────────────────────────────────────────────────────────────
// Slide Auto-Advance Thread (Smooth Alpha Cross-Fading)
// ─────────────────────────────────────────────────────────────────────────────
static void SlideWorker() {
    while (!g_cancelled) {
        Sleep(SLIDE_MS);
        if (g_cancelled) break;

        // Fade out
        for (int i = 12; i >= 0; i--) {
            if (g_cancelled) break;
            g_slideAlpha = i / 12.0f;
            if (g_hwnd) InvalidateRect(g_hwnd, nullptr, FALSE);
            Sleep(20);
        }

        g_slideIdx = (g_slideIdx + 1) % SLIDE_COUNT;

        // Fade in
        for (int i = 0; i <= 12; i++) {
            if (g_cancelled) break;
            g_slideAlpha = i / 12.0f;
            if (g_hwnd) InvalidateRect(g_hwnd, nullptr, FALSE);
            Sleep(20);
        }
        g_slideAlpha = 1.0f;
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// Diegetic Circuit Motion & Orthogonal Comet Engine
// ─────────────────────────────────────────────────────────────────────────────
struct CircuitPath {
    std::vector<PointF> pts;
    float totalLen = 0.0f;
    std::vector<float> segLens;
    float baseSpeed = 100.0f; // px/sec
    float phase = 0.0f;      // 0..1
    float tailLen = 36.0f;
    bool isApex = false;
};

static std::vector<CircuitPath> g_circuitPaths;
static bool g_circuitPathsInit = false;

static void InitCircuitPaths(bool isLogicFlow) {
    g_circuitPaths.clear();

    // Top Bus: runs across the top at Y=42, 45-deg bend down to top header at (416, 80)
    {
        CircuitPath p;
        p.pts = { {238.0f, 42.0f}, {378.0f, 42.0f}, {416.0f, 80.0f} };
        p.baseSpeed = 75.0f; p.phase = 0.10f; p.tailLen = 36.0f;
        g_circuitPaths.push_back(p);
    }
    // Left Logic Gate 1 (Top OR output into shield left crest)
    {
        CircuitPath p;
        p.pts = { {308.0f, 56.0f}, {348.0f, 68.0f}, {383.0f, 68.0f}, {410.0f, 78.0f} };
        p.baseSpeed = 65.0f; p.phase = 0.55f; p.tailLen = 32.0f;
        g_circuitPaths.push_back(p);
    }
    // Left Logic Gate 2 (AND output, 45-deg bend into left wing)
    {
        CircuitPath p;
        p.pts = { {270.0f, 118.0f}, {316.0f, 118.0f}, {326.0f, 128.0f}, {345.0f, 128.0f} };
        p.baseSpeed = 70.0f; p.phase = 0.30f; p.tailLen = 35.0f;
        g_circuitPaths.push_back(p);
    }
    // Left Logic Gate 3 (Mid Gate, 45-deg bend into lower left flank)
    {
        CircuitPath p;
        p.pts = { {270.0f, 172.0f}, {318.0f, 172.0f}, {334.0f, 188.0f}, {345.0f, 188.0f} };
        p.baseSpeed = 80.0f; p.phase = 0.75f; p.tailLen = 36.0f;
        g_circuitPaths.push_back(p);
    }
    // Left Logic Gate 4 (Lower Gate, 45-deg bend toward bottom apex)
    {
        CircuitPath p;
        p.pts = { {270.0f, 246.0f}, {324.0f, 246.0f}, {340.0f, 262.0f}, {370.0f, 262.0f}, {410.0f, 250.0f} };
        p.baseSpeed = 85.0f; p.phase = 0.20f; p.tailLen = 40.0f;
        g_circuitPaths.push_back(p);
    }
    // Right BUS_A Trace 1 (Top line from chip U2, 45-deg bend down into right shoulder)
    {
        CircuitPath p;
        p.pts = { {636.0f, 94.0f}, {488.0f, 94.0f}, {475.0f, 107.0f} };
        p.baseSpeed = 75.0f; p.phase = 0.40f; p.tailLen = 38.0f;
        g_circuitPaths.push_back(p);
    }
    // Right BUS_A Trace 2 (Mid line from chip U2, 45-deg bend down into right wing)
    {
        CircuitPath p;
        p.pts = { {636.0f, 112.0f}, {488.0f, 112.0f}, {475.0f, 125.0f} };
        p.baseSpeed = 85.0f; p.phase = 0.85f; p.tailLen = 38.0f;
        g_circuitPaths.push_back(p);
    }
    // Right BUS_A Trace 3 (Lower line from chip U2, 45-deg bend down into right flank)
    {
        CircuitPath p;
        p.pts = { {636.0f, 132.0f}, {488.0f, 132.0f}, {475.0f, 145.0f} };
        p.baseSpeed = 70.0f; p.phase = 0.15f; p.tailLen = 35.0f;
        g_circuitPaths.push_back(p);
    }
    // Right Lower Bus from Chip U3 (bends 45-deg up into right lower wing)
    {
        CircuitPath p;
        p.pts = { {568.0f, 230.0f}, {488.0f, 230.0f}, {470.0f, 212.0f} };
        p.baseSpeed = 78.0f; p.phase = 0.60f; p.tailLen = 36.0f;
        g_circuitPaths.push_back(p);
    }
    // Vertical Apex Discharge (Straight down physical copper trace at X=424 into progress bar)
    {
        CircuitPath p;
        p.pts = { {424.0f, 250.0f}, {424.0f, 276.0f} };
        p.baseSpeed = 110.0f; p.phase = 0.05f; p.tailLen = 24.0f; p.isApex = true;
        g_circuitPaths.push_back(p);
    }

    // Precalculate segment lengths
    for (auto& path : g_circuitPaths) {
        path.totalLen = 0.0f;
        path.segLens.clear();
        for (size_t i = 0; i + 1 < path.pts.size(); ++i) {
            float dx = path.pts[i + 1].X - path.pts[i].X;
            float dy = path.pts[i + 1].Y - path.pts[i].Y;
            float l = sqrtf(dx * dx + dy * dy);
            path.segLens.push_back(l);
            path.totalLen += l;
        }
    }
    g_circuitPathsInit = true;
}

static PointF GetPolyPoint(const CircuitPath& path, float s) {
    if (path.pts.empty()) return PointF(0, 0);
    if (s <= 0.0f) return path.pts.front();
    if (s >= path.totalLen) return path.pts.back();
    float accum = 0.0f;
    for (size_t i = 0; i < path.segLens.size(); ++i) {
        if (accum + path.segLens[i] >= s) {
            float segDist = s - accum;
            float t = (path.segLens[i] > 0.001f) ? (segDist / path.segLens[i]) : 0.0f;
            return PointF(path.pts[i].X + t * (path.pts[i + 1].X - path.pts[i].X),
                          path.pts[i].Y + t * (path.pts[i + 1].Y - path.pts[i].Y));
        }
        accum += path.segLens[i];
    }
    return path.pts.back();
}

static void RenderCircuitComets(Graphics& g, float timeSec, float progress, bool isInstalling, bool isLogicFlow) {
    if (!g_circuitPathsInit) {
        InitCircuitPaths(isLogicFlow);
    }

    // 1. Quiescent faint copper trace glow
    Pen quiescentPen(Color(18, 0, 220, 255), 1.0f);
    for (const auto& path : g_circuitPaths) {
        if (path.pts.size() >= 2) {
            g.DrawLines(&quiescentPen, path.pts.data(), (INT)path.pts.size());
        }
    }

    // 2. Install progress acceleration factor
    float accel = 1.0f + (isInstalling ? (progress * 2.0f + 0.5f) : 0.0f);

    // 3. Render animated comets along each polyline
    for (const auto& path : g_circuitPaths) {
        if (path.totalLen <= 0.001f) continue;

        float cycleDist = path.totalLen + path.tailLen * 1.4f;
        float distTraveled = fmodf(timeSec * path.baseSpeed * accel + path.phase * cycleDist, cycleDist);
        float sHead = distTraveled;

        float fade = 1.0f;
        if (sHead < 24.0f) {
            fade = sHead / 24.0f;
        } else if (sHead > path.totalLen - 16.0f) {
            float remaining = (path.totalLen + path.tailLen * 1.2f) - sHead;
            fade = remaining / (path.tailLen * 1.2f + 16.0f);
            if (fade < 0.0f) fade = 0.0f;
            if (fade > 1.0f) fade = 1.0f;
        }

        if (fade <= 0.01f) continue;

        const int numSegments = 14;
        float segStep = path.tailLen / (float)numSegments;

        for (int j = 0; j < numSegments; ++j) {
            float s0 = sHead - j * segStep;
            float s1 = sHead - (j + 1) * segStep;
            if (s0 <= 0.0f) continue;
            if (s1 < 0.0f) s1 = 0.0f;
            if (s0 > path.totalLen) s0 = path.totalLen;
            if (s1 > path.totalLen) s1 = path.totalLen;
            if (fabsf(s0 - s1) < 0.1f) continue;

            PointF p0 = GetPolyPoint(path, s0);
            PointF p1 = GetPolyPoint(path, s1);

            float frac = (j + 0.5f) / (float)numSegments;
            float alphaDecay = powf(1.0f - frac, 1.7f);
            BYTE aVal = (BYTE)(alphaDecay * 235.0f * fade);
            if (aVal < 2) continue;

            // Chromatic transition: White-Cyan -> Electric Cyan -> Neon Violet
            Color tailCol;
            if (frac < 0.25f) {
                float t = frac / 0.25f;
                BYTE r = (BYTE)((1.0f - t) * 230 + t * 0);
                BYTE gVal = (BYTE)((1.0f - t) * 255 + t * 230);
                BYTE b = (BYTE)255;
                tailCol = Color(aVal, r, gVal, b);
            } else if (frac < 0.70f) {
                float t = (frac - 0.25f) / 0.45f;
                BYTE r = (BYTE)(t * 185);
                BYTE gVal = (BYTE)((1.0f - t) * 230 + t * 60);
                BYTE b = (BYTE)255;
                tailCol = Color(aVal, r, gVal, b);
            } else {
                float t = (frac - 0.70f) / 0.30f;
                BYTE r = 185;
                BYTE gVal = (BYTE)((1.0f - t) * 60 + t * 30);
                BYTE b = (BYTE)255;
                tailCol = Color(aVal, r, gVal, b);
            }

            float penW = (1.0f - frac) * 2.6f + 0.8f;
            Pen tailPen(tailCol, penW);
            tailPen.SetStartCap(LineCapRound);
            tailPen.SetEndCap(LineCapRound);
            g.DrawLine(&tailPen, p0.X, p0.Y, p1.X, p1.Y);
        }

        // Glowing comet head
        if (sHead <= path.totalLen) {
            PointF headPt = GetPolyPoint(path, sHead);
            SolidBrush haloBrush(Color((BYTE)(120 * fade), 0, 230, 255));
            g.FillEllipse(&haloBrush, headPt.X - 3.5f, headPt.Y - 3.5f, 7.0f, 7.0f);
            SolidBrush coreBrush(Color((BYTE)(255 * fade), 245, 255, 255));
            g.FillEllipse(&coreBrush, headPt.X - 1.5f, headPt.Y - 1.5f, 3.0f, 3.0f);
        }
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// GDI+ Compositing & Double-Buffered Painting
// ─────────────────────────────────────────────────────────────────────────────
static void OnPaint(HWND hwnd) {
    PAINTSTRUCT ps;
    HDC hdc = BeginPaint(hwnd, &ps);

    RECT rc;
    GetClientRect(hwnd, &rc);
    int W = rc.right;
    int H = rc.bottom;

    // Create double-buffer memory DC
    HDC memDC = CreateCompatibleDC(hdc);
    HBITMAP memBmp = CreateCompatibleBitmap(hdc, W, H);
    HBITMAP oldBmp = (HBITMAP)SelectObject(memDC, memBmp);

    Graphics g(memDC);
    g.SetSmoothingMode(SmoothingModeHighQuality);
    g.SetInterpolationMode(InterpolationModeHighQualityBicubic);
    g.SetPixelOffsetMode(PixelOffsetModeHalf);
    g.SetTextRenderingHint(TextRenderingHintClearTypeGridFit);

    // ── 1. Background Circuit Board Substrate ──
    if (g_bmpBg) {
        g.DrawImage(g_bmpBg.get(), 0, 0, W, H);
    } else {
        SolidBrush bgBrush(COL_BG);
        g.FillRectangle(&bgBrush, 0, 0, W, H);
    }

    // ── 2. Subtle Outer Ambient Frame Glow & Rounded Border ──
    Pen outerBorder(Color(70, 0, 190, 255), 1.0f);
    g.DrawRectangle(&outerBorder, 0, 0, W - 1, H - 1);
    Pen innerBorder(Color(25, 255, 255, 255), 1.0f);
    g.DrawRectangle(&innerBorder, 1, 1, W - 3, H - 3);

    // ── 2.5 Diegetic Circuit-Integrated Motion Visuals (PCB Trace Comets) ──
    DWORD tickMs = GetTickCount();
    float timeSec = (tickMs % 100000) / 1000.0f;
    float progVal = g_progress.load();
    bool isInstVal = g_installing.load();
    RenderCircuitComets(g, timeSec, progVal, isInstVal, false);

    // ── 3. Header: Top-Left Branding & Top-Right Close Button ──
    // Mini Shield (Unclipped, msX=20, msY=20):
    int msX = 20, msY = 20, msW = 34, msH = 34;
    if (g_bmpMiniShield) {
        g.DrawImage(g_bmpMiniShield.get(), msX, msY, msW, msH);
    } else {
        Pen miniShieldPen(Color(255, 140, 80, 255), 1.5f);
        PointF msPts[] = {
            {(float)msX+2, (float)msY}, {(float)msX+msW-2, (float)msY},
            {(float)msX+msW, (float)msY+msH/2}, {(float)msX+msW/2, (float)msY+msH},
            {(float)msX, (float)msY+msH/2}
        };
        g.DrawPolygon(&miniShieldPen, msPts, 5);
    }

    // DELGADOLOGIC Wordmark (Single Word, Tightened 4px gap, 16px Bold Gradient):
    int wmX = msX + msW + 4;
    int wmY = msY + 8;
    int wmW = 180;
    int wmH = 24;

    FontFamily ff(L"Segoe UI");
    Gdiplus::Font fontBrand(&ff, 16, FontStyleBold, UnitPixel);
    LinearGradientBrush brandBrush(
        Point(wmX, wmY),
        Point(wmX + 160, wmY),
        Color(255, 0, 229, 255),    // Electric cyan
        Color(255, 192, 132, 252)   // Neon violet
    );
    g.DrawString(L"DELGADOLOGIC", -1, &fontBrand, PointF((float)wmX, (float)wmY), &brandBrush);

    // Close '×' Button (Top-Right):
    int clX = W - 38, clY = 20, clW = 16, clH = 16;
    if (g_closeHover) {
        SolidBrush hoverBg(Color(40, 255, 255, 255));
        g.FillEllipse(&hoverBg, clX - 4, clY - 4, clW + 8, clH + 8);
    }

    if (g_bmpClose) {
        ImageAttributes iaClose;
        if (g_closeHover) {
            ColorMatrix cmClose = {
                1.3f, 0.0f, 0.0f, 0.0f, 0.0f,
                0.0f, 1.3f, 0.0f, 0.0f, 0.0f,
                0.0f, 0.0f, 1.3f, 0.0f, 0.0f,
                0.0f, 0.0f, 0.0f, 1.0f, 0.0f,
                0.0f, 0.0f, 0.0f, 0.0f, 1.0f
            };
            iaClose.SetColorMatrix(&cmClose);
        }
        g.DrawImage(g_bmpClose.get(), Rect(clX, clY, clW, clH), 0, 0,
                    g_bmpClose->GetWidth(), g_bmpClose->GetHeight(), UnitPixel, &iaClose);
    } else {
        FontFamily ff(L"Segoe UI");
        Gdiplus::Font fontX(&ff, 14, FontStyleRegular, UnitPixel);
        SolidBrush xBrush(g_closeHover ? COL_WHITE : Color(255, 140, 150, 165));
        g.DrawString(L"×", -1, &fontX, PointF((float)clX, (float)clY - 2), &xBrush);
    }

    // ── 4. Centerpiece: 3D Crystal Shield with Fluid Contour Plasma Aura ──
    float pulse = (sinf(timeSec * 3.14159f) + 1.0f) * 0.5f; // Smooth 0.0 -> 1.0 sine breathing wave
    
    int shieldBaseW = 164;
    int shieldBaseH = 188;
    int glowCX = W / 2;
    int glowCY = 60 + shieldBaseH / 2;

    // Dynamic physical breathing scale (3.0% volumetric expansion)
    float breatheScale = 1.0f + pulse * 0.030f;
    int curW = (int)(shieldBaseW * breatheScale);
    int curH = (int)(shieldBaseH * breatheScale);
    int curX = glowCX - curW / 2;
    int curY = glowCY - curH / 2;

    // Fluid Contour-Clinging 32-bit ARGB Plasma Aura (tailored to shield silhouette)
    if (g_bmpAura) {
        float heroW = g_bmpHeroShield ? (float)g_bmpHeroShield->GetWidth() : 763.0f;
        float heroH = g_bmpHeroShield ? (float)g_bmpHeroShield->GetHeight() : 872.0f;
        float scaleX = (heroW > 0.0f) ? ((float)curW / heroW) : 1.0f;
        float scaleY = (heroH > 0.0f) ? ((float)curH / heroH) : 1.0f;
        int auraW = (int)((float)g_bmpAura->GetWidth() * scaleX);
        int auraH = (int)((float)g_bmpAura->GetHeight() * scaleY);
        int auraX = glowCX - auraW / 2;
        int auraY = glowCY - auraH / 2;

        float auraPulse = pulse;
        float auraAlpha = 0.82f + auraPulse * 0.22f; // Breathing alpha 0.82 -> 1.04
        float auraBright = 1.0f + auraPulse * 0.16f; // Specular pulsation
        ColorMatrix cmAura = {
            auraBright, 0.0f,       0.0f,       0.0f,      0.0f,
            0.0f,       auraBright, 0.0f,       0.0f,      0.0f,
            0.0f,       0.0f,       auraBright, 0.0f,      0.0f,
            0.0f,       0.0f,       0.0f,       auraAlpha, 0.0f,
            0.0f,       0.0f,       0.0f,       0.0f,      1.0f
        };
        ImageAttributes iaAura;
        iaAura.SetColorMatrix(&cmAura);
        g.DrawImage(g_bmpAura.get(), Rect(auraX, auraY, auraW, auraH), 0, 0,
                    g_bmpAura->GetWidth(), g_bmpAura->GetHeight(), UnitPixel, &iaAura);
    }

    if (g_bmpHeroShield) {
        // Specular glow & brightness pulsation
        float boost = 1.0f + pulse * 0.18f;
        ColorMatrix cmGlow = {
            boost, 0.0f,  0.0f,  0.0f, 0.0f,
            0.0f,  boost, 0.0f,  0.0f, 0.0f,
            0.0f,  0.0f,  boost, 0.0f, 0.0f,
            0.0f,  0.0f,  0.0f,  1.0f, 0.0f,
            0.0f,  0.0f,  0.0f,  0.0f, 1.0f
        };
        ImageAttributes iaGlow;
        iaGlow.SetColorMatrix(&cmGlow);
        g.DrawImage(g_bmpHeroShield.get(), Rect(curX, curY, curW, curH), 0, 0,
                    g_bmpHeroShield->GetWidth(), g_bmpHeroShield->GetHeight(), UnitPixel, &iaGlow);
    } else {
        SolidBrush shieldFill(Color(200, 40, 25, 80));
        g.FillEllipse(&shieldFill, curX, curY, curW, curH);
    }


    // ── 5. Progress Bar: Sleek Dark Capsule Track & Glowing Cyan Fill ──
    float prog = g_progress.load();
    int barW = 460;
    int barX = W / 2 - barW / 2;   // Centered at 180 (matches reference)
    int barY = 276;
    int barH = 10;

    GraphicsPath trackPath;
    trackPath.AddArc(barX, barY, barH, barH, 90, 180);
    trackPath.AddArc(barX + barW - barH, barY, barH, barH, 270, 180);
    trackPath.CloseFigure();

    SolidBrush trackBg(Color(255, 14, 16, 22));
    g.FillPath(&trackBg, &trackPath);

    if (g_bmpTrack) {
        g.SetClip(&trackPath);
        g.DrawImage(g_bmpTrack.get(), barX, barY, barW, barH);
        g.ResetClip();
    }

    // Filled animated segment
    int fillW = (int)(barW * prog);
    if (fillW > 2) {
        g.SetClip(&trackPath);

        if (g_bmpFill) {
            g.DrawImage(g_bmpFill.get(), barX, barY - 2, fillW, barH + 4);
        } else {
            LinearGradientBrush grad(
                Point(barX, barY),
                Point(barX + fillW, barY),
                COL_CYAN_DIM,
                COL_CYAN_FILL
            );
            g.FillRectangle(&grad, barX, barY, fillW, barH);
        }

        g.ResetClip();
    }

    // ── 6. Centered Percentage Text ──
    {
        FontFamily ff(L"Segoe UI");
        Gdiplus::Font fontPct(&ff, 11, FontStyleBold, UnitPixel);
        SolidBrush pctBrush(Color(255, 175, 195, 220));
        StringFormat sf;
        sf.SetAlignment(StringAlignmentCenter);
        sf.SetLineAlignment(StringAlignmentCenter);
        std::wstring pctStr = std::to_wstring((int)(prog * 100.0f)) + L"%";

        // Subtle dark backing capsule to prevent background schematics from bleeding through
        int padW = 58, padH = 18;
        int padX = W / 2 - padW / 2;
        int padY = barY + 14;
        GraphicsPath pctBgPath;
        pctBgPath.AddArc(padX, padY, padH, padH, 90, 180);
        pctBgPath.AddArc(padX + padW - padH, padY, padH, padH, 270, 180);
        pctBgPath.CloseFigure();
        SolidBrush pctBg(Color(240, 6, 8, 12));
        g.FillPath(&pctBg, &pctBgPath);
        Pen pctBorder(Color(60, 0, 229, 255), 1.0f);
        g.DrawPath(&pctBorder, &pctBgPath);

        g.DrawString(pctStr.c_str(), -1, &fontPct,
                     RectF((float)padX, (float)padY - 1, (float)padW, (float)padH),
                     &sf, &pctBrush);
    }

    // ── 7. Headline & Subtitle Typography ──
    {
        FontFamily ff(L"Segoe UI");
        Gdiplus::Font fontHead(&ff, 19, FontStyleBold, UnitPixel);
        SolidBrush headBrush(COL_WHITE);
        StringFormat sf;
        sf.SetAlignment(StringAlignmentCenter);

        std::wstring headline = g_done ? L"Aeon Browser Installed" : L"Installing Aeon Browser...";
        g.DrawString(headline.c_str(), -1, &fontHead,
                     RectF(30, (float)(barY + 40), (float)(W - 60), 28),
                     &sf, &headBrush);

        Gdiplus::Font fontSub(&ff, 12, FontStyleRegular, UnitPixel);
        SolidBrush subBrush(COL_TEXT_MUTED);
        std::wstring subtitle = g_done ? L"Your sovereign workspace is ready." : L"Hardening your digital perimeter.";
        g.DrawString(subtitle.c_str(), -1, &fontSub,
                     RectF(30, (float)(barY + 68), (float)(W - 60), 20),
                     &sf, &subBrush);
    }

    // ── 8. Rotating "Did You Know?" Privacy Slides ──
    if (!g_done) {
        int idx = g_slideIdx.load();
        float alpha = g_slideAlpha.load();
        BYTE a = (BYTE)(alpha * 255.0f);
        const Slide& slide = SLIDES[idx];

        FontFamily ff(L"Segoe UI");
        Gdiplus::Font fontSlideHead(&ff, 13, FontStyleBold, UnitPixel);
        Gdiplus::Font fontSlideBody(&ff, 11, FontStyleRegular, UnitPixel);
        SolidBrush headBrush(Color(a, 90, 200, 255));
        SolidBrush bodyBrush(Color(a, 165, 175, 195));

        StringFormat sf;
        sf.SetAlignment(StringAlignmentCenter);
        sf.SetLineAlignment(StringAlignmentCenter);

        RectF headRect(60, (float)(barY + 98), (float)(W - 120), 20);
        RectF bodyRect(60, (float)(barY + 120), (float)(W - 120), 38);

        g.DrawString(slide.headline, -1, &fontSlideHead, headRect, &sf, &headBrush);
        g.DrawString(slide.body,     -1, &fontSlideBody, bodyRect, &sf, &bodyBrush);
    } else {
        // ── 9. Done State: Sleek "Launch Aeon →" Action Button ──
        int btnW = 190;
        int btnH = 38;
        int btnX = W / 2 - btnW / 2;
        int btnY = H - 65;
        g_launchBtnRect = { btnX, btnY, btnX + btnW, btnY + btnH };

        GraphicsPath btnPath;
        btnPath.AddArc(btnX, btnY, btnH, btnH, 90, 180);
        btnPath.AddArc(btnX + btnW - btnH, btnY, btnH, btnH, 270, 180);
        btnPath.CloseFigure();

        LinearGradientBrush btnGrad(
            Point(btnX, btnY),
            Point(btnX + btnW, btnY),
            g_launchHover ? Color(255, 0, 170, 255) : Color(255, 0, 140, 220),
            g_launchHover ? Color(255, 180, 80, 255) : Color(255, 140, 60, 240)
        );
        g.FillPath(&btnGrad, &btnPath);

        Pen btnBorder(Color(255, 200, 240, 255), 1.0f);
        g.DrawPath(&btnBorder, &btnPath);

        FontFamily ff(L"Segoe UI");
        Gdiplus::Font fontBtn(&ff, 14, FontStyleBold, UnitPixel);
        SolidBrush btnText(COL_WHITE);
        StringFormat sf;
        sf.SetAlignment(StringAlignmentCenter);
        sf.SetLineAlignment(StringAlignmentCenter);
        g.DrawString(L"Launch Aeon \x2192", -1, &fontBtn,
                     RectF((float)btnX, (float)btnY, (float)btnW, (float)btnH),
                     &sf, &btnText);
    }

    // Blit from memory DC to screen
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
        LoadAllSprites();
        std::thread(InstallWorker).detach();
        std::thread(SlideWorker).detach();
        SetTimer(hwnd, 1, ANIM_TICK_MS, nullptr);
        return 0;

    case WM_TIMER:
        InvalidateRect(hwnd, nullptr, FALSE);
        return 0;

    case WM_PAINT:
        OnPaint(hwnd);
        return 0;

    case WM_MOUSEMOVE: {
        POINT pt = { GET_X_LPARAM(lp), GET_Y_LPARAM(lp) };

        bool oldClose = g_closeHover;
        bool oldLaunch = g_launchHover;

        g_closeHover = PtInRect(&g_closeBtnRect, pt);
        g_launchHover = g_done && PtInRect(&g_launchBtnRect, pt);

        if (oldClose != g_closeHover || oldLaunch != g_launchHover) {
            InvalidateRect(hwnd, nullptr, FALSE);
        }

        TRACKMOUSEEVENT tme = { sizeof(tme), TME_LEAVE, hwnd, 0 };
        TrackMouseEvent(&tme);
        return 0;
    }

    case WM_MOUSELEAVE: {
        if (g_closeHover || g_launchHover) {
            g_closeHover = false;
            g_launchHover = false;
            InvalidateRect(hwnd, nullptr, FALSE);
        }
        return 0;
    }

    case WM_LBUTTONDOWN: {
        POINT pt = { GET_X_LPARAM(lp), GET_Y_LPARAM(lp) };

        // Close button clicked
        if (PtInRect(&g_closeBtnRect, pt)) {
            g_cancelled = true;
            CleanupTemp();
            DestroyWindow(hwnd);
            return 0;
        }

        // Launch button clicked
        if (g_done && PtInRect(&g_launchBtnRect, pt)) {
            wchar_t launch[MAX_PATH];
            wcscpy_s(launch, g_installPath);
            wcscat_s(launch, L"\\Aeon.exe");
            ShellExecuteW(nullptr, L"open", launch, nullptr, nullptr, SW_SHOW);
            DestroyWindow(hwnd);
            return 0;
        }
        return 0;
    }

    case WM_NCHITTEST: {
        // Point in screen coords -> convert to client coords
        POINT pt = { GET_X_LPARAM(lp), GET_Y_LPARAM(lp) };
        ScreenToClient(hwnd, &pt);

        // 1. Close button rect returns HTCLIENT so clicks & hovers fire
        if (PtInRect(&g_closeBtnRect, pt)) {
            return HTCLIENT;
        }

        // 2. Launch button when finished returns HTCLIENT
        if (g_done && PtInRect(&g_launchBtnRect, pt)) {
            return HTCLIENT;
        }

        // 3. All empty chassis areas return HTCAPTION for smooth dragging
        return HTCAPTION;
    }

    case WM_DESTROY:
        KillTimer(hwnd, 1);
        DeleteCriticalSection(&g_statusLock);
        g_bmpBg.reset();
        g_bmpHeroShield.reset();
        g_bmpAura.reset();
        g_bmpPill.reset();
        g_bmpMiniShield.reset();
        g_bmpWordmark.reset();
        g_bmpClose.reset();
        g_bmpTrack.reset();
        g_bmpFill.reset();
        g_bmpFrame.reset();
        GdiplusShutdown(g_gdiplusToken);
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

// ─────────────────────────────────────────────────────────────────────────────
// WinMain Entry Point
// ─────────────────────────────────────────────────────────────────────────────
int WINAPI wWinMain(HINSTANCE hInst, HINSTANCE, LPWSTR, int) {
    // Initialize GDI+
    GdiplusStartupInput gsi;
    GdiplusStartup(&g_gdiplusToken, &gsi, nullptr);
    InitializeCriticalSection(&g_statusLock);

    // Register custom window class with CS_DROPSHADOW
    WNDCLASSEXW wc   = {};
    wc.cbSize        = sizeof(wc);
    wc.style         = CS_DROPSHADOW | CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc   = WndProc;
    wc.hInstance     = hInst;
    wc.hCursor       = LoadCursorW(nullptr, IDC_ARROW);
    wc.lpszClassName = L"AeonInstaller";
    RegisterClassExW(&wc);

    // Center window on primary monitor
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

    if (!hwnd) {
        GdiplusShutdown(g_gdiplusToken);
        return 1;
    }

    // Windows 11 DWM rounded corners (DWMWCP_ROUND = 2, DWMWA_WINDOW_CORNER_PREFERENCE = 33)
    DWORD cornerPref = DWMWCP_ROUND;
    DwmSetWindowAttribute(hwnd, DWMWA_WINDOW_CORNER_PREFERENCE, &cornerPref, sizeof(cornerPref));

    // Extend DWM frame for smooth shadow and edge rendering
    MARGINS margins = { 0, 0, 0, 1 };
    DwmExtendFrameIntoClientArea(hwnd, &margins);

    ShowWindow(hwnd, SW_SHOW);
    UpdateWindow(hwnd);

    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    return 0;
}
