// AeonBrowser — AeonMain.cpp
// DelgadoLogic
//
// Main entry point — Win32 WinMain.
// Runs HardwareProbe, loads the right engine tier, starts NetworkSentinel,
// then creates the BrowserChrome window.

#include <windows.h>
#include "core/probe/HardwareProbe.h"
#include "core/engine/TierDispatcher.h"
#include "core/ui/BrowserChrome.h"
#include "core/network/NetworkSentinel.h"
#include "core/network/CircumventionEngine.h"
#include "core/settings/SettingsEngine.h"
#include "core/memory/TabSleepManager.h"
#include "core/security/PasswordVault.h"
#include "updater/AutoUpdater.h"
#include <cstdio>

#define AEON_WINDOW_CLASS L"AeonBrowserHost"
#define AEON_TITLE        L"Aeon Browser — by DelgadoLogic"

// ─── Forward declarations ────────────────────────────────────────────────
LRESULT CALLBACK AeonWndProc(HWND, UINT, WPARAM, LPARAM);

int WINAPI WinMain(HINSTANCE hInstance, HINSTANCE, LPSTR lpCmdLine, int nShowCmd) {

    // ── 0. Allocate debug console on Debug builds ──────────────────────
#ifdef _DEBUG
    AllocConsole();
    FILE* f; freopen_s(&f, "CONOUT$", "w", stdout);
    freopen_s(&f, "CONOUT$", "w", stderr);
#endif

    fprintf(stdout, "\n");
    fprintf(stdout, "╔══════════════════════════════════════╗\n");
    fprintf(stdout, "║  Aeon Browser  —  by DelgadoLogic    ║\n");
    fprintf(stdout, "║  delgadologic.tech/aeon              ║\n");
    fprintf(stdout, "╚══════════════════════════════════════╝\n\n");

    // ── 1. Hardware probe — detect OS tier + CPU ───────────────────────
    const SystemProfile* profile = HardwareProbe::Run();
    fprintf(stdout, "[Boot] Tier: %s | OS: %s | CPU: %s | RAM: %d MB\n",
        profile->tier_name, profile->os_name,
        profile->has_sse2 ? "SSE2+" : "Legacy",
        profile->ram_mb);

    // ── 2. Load settings ───────────────────────────────────────────────
    AeonSettings settings = SettingsEngine::Load();

    // ── 3. Initialize Password Vault ───────────────────────────────────
    char vaultPath[MAX_PATH];
    char appData[MAX_PATH];
    SHGetFolderPathA(nullptr, CSIDL_APPDATA, nullptr, 0, appData);
    _snprintf_s(vaultPath, sizeof(vaultPath), _TRUNCATE,
        "%s\\DelgadoLogic\\Aeon\\vault.db", appData);
    CreateDirectoryA(vaultPath, nullptr); // no-op if exists
    // Trim trailing filename from dir creation (create parent dir only)
    char vaultDir[MAX_PATH];
    strncpy_s(vaultDir, vaultPath, sizeof(vaultDir)-1);
    char* lastSlash = strrchr(vaultDir, '\\');
    if (lastSlash) { *lastSlash = '\0'; CreateDirectoryA(vaultDir, nullptr); }
    PasswordVault::Init(vaultPath);

    // ── 4. Load engine DLL for this tier ──────────────────────────────
    AeonEngineVTable* engine = TierDispatcher::LoadEngine(profile);
    if (!engine) {
        MessageBoxA(nullptr,
            "Aeon could not load a rendering engine for your system.\n"
            "Please visit delgadologic.tech/aeon for troubleshooting.",
            "Aeon Browser — Engine Error", MB_ICONERROR);
        return 1;
    }

    // ── 5. Network Sentinel — auto-detect restriction + bypass ────────
    //   This runs BEFORE drawing any UI so the first request is already
    //   behind the best available bypass layer.
    fprintf(stdout, "[Boot] Running Network Sentinel...\n");
    auto netEnv = NetworkSentinel::Analyze();
    NetworkSentinel::ApplyBestStrategy();
    NetworkSentinel::StartMonitor(); // background re-probe every 30s

    // If firewall mode was saved ON in settings, also engage CircumventionEngine
    if (settings.firewall_mode && !netEnv.captive_portal) {
        CircumventionEngine::Enable(settings.ss_uri);
    }

    // ── 6. Tab Sleep Manager ───────────────────────────────────────────
    TabSleepManager::Start();

    // ── 7. Auto-updater (background, silent) ──────────────────────────
    if (settings.auto_update) {
        AutoUpdater::CheckAsync(settings.update_channel);
    }

    // ── 8. Create main window ──────────────────────────────────────────
    WNDCLASSEXW wc   = {};
    wc.cbSize        = sizeof(wc);
    wc.style         = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc   = AeonWndProc;
    wc.hInstance     = hInstance;
    wc.hIcon         = LoadIconA(hInstance, "IDI_AEON");
    wc.hCursor       = LoadCursor(nullptr, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)GetStockObject(BLACK_BRUSH);
    wc.lpszClassName = AEON_WINDOW_CLASS;
    RegisterClassExW(&wc);

    int W = GetSystemMetrics(SM_CXSCREEN) * 85 / 100;  // 85% of screen
    int H = GetSystemMetrics(SM_CYSCREEN) * 85 / 100;
    int X = (GetSystemMetrics(SM_CXSCREEN) - W) / 2;
    int Y = (GetSystemMetrics(SM_CYSCREEN) - H) / 2;

    // WS_EX_NOREDIRECTIONBITMAP: required for DWM Mica/Acrylic (Win11)
    DWORD exStyle = (profile->tier >= OSTier::Modern)
        ? WS_EX_NOREDIRECTIONBITMAP : 0;

    HWND hWnd = CreateWindowExW(
        exStyle, AEON_WINDOW_CLASS, AEON_TITLE,
        WS_OVERLAPPEDWINDOW, X, Y, W, H,
        nullptr, nullptr, hInstance, nullptr);

    if (!hWnd) return 1;

    // Enable DWM Mica background on Windows 11
    if (profile->tier >= OSTier::Modern) {
        MARGINS margins = { -1 };
        DwmExtendFrameIntoClientArea(hWnd, &margins);
        // Mica: DWM_SYSTEMBACKDROP_MICA = 2
        int backdropType = 2;
        DwmSetWindowAttribute(hWnd, 38 /*DWMWA_SYSTEMBACKDROP_TYPE*/,
            &backdropType, sizeof(backdropType));
    }

    // Store engine pointer in window data
    SetWindowLongPtr(hWnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(engine));

    // Initialize browser chrome (draws the "A" badge, tab strip, nav bar)
    BrowserChrome::Create(hWnd, profile, engine);

    // If captive portal detected — open it in a special tab immediately
    if (NetworkSentinel::GetState().need_captive_portal) {
        const char* portalUrl = NetworkSentinel::GetState().captive_portal_url;
        if (engine && portalUrl[0]) {
            engine->NewTab(hWnd, portalUrl);
            fprintf(stdout, "[Boot] Captive portal tab opened: %s\n", portalUrl);
        }
    }

    ShowWindow(hWnd, nShowCmd);
    UpdateWindow(hWnd);

    fprintf(stdout, "[Boot] Ready.\n\n");

    // ── 9. Message loop ────────────────────────────────────────────────
    MSG msg = {};
    while (GetMessageW(&msg, nullptr, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    // ── 10. Cleanup ────────────────────────────────────────────────────
    NetworkSentinel::StopMonitor();
    CircumventionEngine::Disable();
    TabSleepManager::Stop();
    PasswordVault::Lock();
    if (engine) engine->Shutdown();

    fprintf(stdout, "[Boot] Aeon shutdown clean.\n");
    return (int)msg.wParam;
}

// ─── Window procedure ─────────────────────────────────────────────────────
LRESULT CALLBACK AeonWndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
        case WM_PAINT: {
            BrowserChrome::OnPaint(hWnd);
            ValidateRect(hWnd, nullptr);
            return 0;
        }
        case WM_SIZE:
            BrowserChrome::OnSize(hWnd, LOWORD(lParam), HIWORD(lParam));
            return 0;

        case WM_ERASEBKGND:
            return 1; // BrowserChrome handles background, prevent flicker

        case WM_DESTROY:
            PostQuitMessage(0);
            return 0;
    }
    return DefWindowProcW(hWnd, msg, wParam, lParam);
}
