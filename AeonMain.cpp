// AeonBrowser — AeonMain.cpp
// DelgadoLogic
//
// Main entry point — Win32 WinMain.
// Uses AeonProbe::RunProbe() for tier detection, TierDispatcher_LoadEngine() for
// engine DLL loading, and AeonBridge::Init() for JS↔C++ communication.

#include <windows.h>
#include <dwmapi.h>
#include <shlobj.h>
#include <cstdio>

#pragma comment(lib, "dwmapi.lib")

#include "core/probe/HardwareProbe.h"
#include "core/engine/AeonEngine_Interface.h"
#include "core/engine/AeonBridge.h"
#include "core/ui/BrowserChrome.h"
#include "core/network/NetworkSentinel.h"
#include "core/settings/SettingsEngine.h"
#include "core/memory/TabSleepManager.h"
#include "core/security/PasswordVault.h"
#include "updater/AutoUpdater.h"

#define AEON_WINDOW_CLASS L"AeonBrowserHost"
#define AEON_TITLE        L"Aeon Browser \u2014 by DelgadoLogic"

// Forward declarations
LRESULT CALLBACK AeonWndProc(HWND, UINT, WPARAM, LPARAM);

// Declared in TierDispatcher.cpp — loads the engine DLL chain
AeonEngineVTable* TierDispatcher_LoadEngine(const SystemProfile* profile);

int WINAPI WinMain(HINSTANCE hInstance, HINSTANCE, LPSTR, int nShowCmd) {

#ifdef _DEBUG
    AllocConsole();
    FILE* f; freopen_s(&f, "CONOUT$", "w", stdout);
    freopen_s(&f, "CONOUT$", "w", stderr);
#endif

    fprintf(stdout, "\n");
    fprintf(stdout, "\u2554\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2557\n");
    fprintf(stdout, "\u2551  Aeon Browser  \u2014  by DelgadoLogic    \u2551\n");
    fprintf(stdout, "\u2551  delgadologic.tech/aeon              \u2551\n");
    fprintf(stdout, "\u255a\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u255d\n\n");

    // 1. Hardware probe — detect OS tier + CPU
    SystemProfile profile = {};
    AeonTier tier = AeonProbe::RunProbe(profile);
    fprintf(stdout, "[Boot] Tier: %s | OS: %u.%u.%u | Cores: %d | RAM: %.0f MB\n",
        AeonProbe::TierName(tier),
        profile.osMajor, profile.osMinor, profile.osBuild,
        (int)profile.cpu.cores,
        (double)profile.ramBytes / (1024.0 * 1024.0));

    // 2. Load settings
    AeonSettings settings = SettingsEngine::Load();

    // 3. Initialize Password Vault
    char appData[MAX_PATH] = {};
    SHGetFolderPathA(nullptr, CSIDL_APPDATA, nullptr, 0, appData);
    char vaultDir[MAX_PATH];
    _snprintf_s(vaultDir, sizeof(vaultDir), _TRUNCATE, "%s\\DelgadoLogic\\Aeon", appData);
    CreateDirectoryA(vaultDir, nullptr);
    char vaultPath[MAX_PATH];
    _snprintf_s(vaultPath, sizeof(vaultPath), _TRUNCATE, "%s\\vault.db", vaultDir);
    PasswordVault::Init(vaultPath);

    // 4. Load rendering engine DLL for this tier
    AeonEngineVTable* engine = TierDispatcher_LoadEngine(&profile);
    if (!engine) {
        MessageBoxA(nullptr,
            "Aeon could not load a rendering engine for your system.\n"
            "Please visit delgadologic.tech/aeon for troubleshooting.",
            "Aeon Browser \u2014 Engine Error", MB_ICONERROR);
        return 1;
    }

    // 5. Tab Sleep Manager
    TabSleepManager::Initialize((unsigned int)tier);

    // 6. Auto-updater (async background — starts immediately, non-blocking)
    if (settings.auto_update) {
        AutoUpdater::CheckAsync(settings.update_channel);
    }

    // 7. Create main window
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

    int W = GetSystemMetrics(SM_CXSCREEN) * 85 / 100;
    int H = GetSystemMetrics(SM_CYSCREEN) * 85 / 100;
    int X = (GetSystemMetrics(SM_CXSCREEN) - W) / 2;
    int Y = (GetSystemMetrics(SM_CYSCREEN) - H) / 2;

    // WS_EX_NOREDIRECTIONBITMAP: required for DWM Mica/Acrylic (Win11)
    DWORD exStyle = (profile.osMajor >= 10 && profile.osBuild >= 22000)
        ? WS_EX_NOREDIRECTIONBITMAP : 0;

    HWND hWnd = CreateWindowExW(
        exStyle, AEON_WINDOW_CLASS, AEON_TITLE,
        WS_OVERLAPPEDWINDOW, X, Y, W, H,
        nullptr, nullptr, hInstance, nullptr);

    if (!hWnd) return 1;

    // Enable DWM Mica on Windows 11
    if (profile.osBuild >= 22000) {
        MARGINS margins = { -1 };
        DwmExtendFrameIntoClientArea(hWnd, &margins);
        int backdropType = 2; // DWMSBT_MAINWINDOW (Mica)
        DwmSetWindowAttribute(hWnd, 38 /*DWMWA_SYSTEMBACKDROP_TYPE*/,
            &backdropType, sizeof(backdropType));
    }

    // Store engine pointer in window USERDATA for WndProc
    SetWindowLongPtr(hWnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(engine));

    // 8. AeonBridge — wire JS↔C++ for all internal aeon:// pages
    AeonBridge::Init(hWnd, [](const char* url) {
        char* urlCopy = _strdup(url);
        if (urlCopy)
            PostMessage(GetForegroundWindow(), WM_AEONBRIDGE,
                BRIDGE_CMD_NAVIGATE, (LPARAM)urlCopy);
    });

    // 9. Browser chrome (tab strip, nav bar, WebView2 host)
    BrowserChrome::Create(hWnd, &profile, engine);

    // Captive portal — open in first tab if detected
    const NetworkSentinel::SentinelState& sentState = NetworkSentinel::GetState();
    if (sentState.need_captive_portal && sentState.captive_portal_url[0]) {
        if (engine) engine->NewTab(hWnd, sentState.captive_portal_url);
        fprintf(stdout, "[Boot] Captive portal tab: %s\n", sentState.captive_portal_url);
    }

    ShowWindow(hWnd, nShowCmd);
    UpdateWindow(hWnd);
    fprintf(stdout, "[Boot] Ready.\n\n");

    // 10. Message loop
    MSG msg = {};
    while (GetMessageW(&msg, nullptr, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    // 11. Cleanup
    NetworkSentinel::StopMonitor();
    TabSleepManager::Shutdown();
    PasswordVault::Lock();
    if (engine && engine->Shutdown) engine->Shutdown();

    fprintf(stdout, "[Boot] Aeon shutdown clean.\n");
    return (int)msg.wParam;
}

// Window procedure
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
            return 1;

        case WM_AEONBRIDGE: {
            if ((int)wParam == BRIDGE_CMD_NAVIGATE) {
                char* url = reinterpret_cast<char*>(lParam);
                AeonEngineVTable* eng = reinterpret_cast<AeonEngineVTable*>(
                    GetWindowLongPtr(hWnd, GWLP_USERDATA));
                // Navigate signature: (tab_id, url, referrer)
                if (eng && eng->Navigate && url && url[0])
                    eng->Navigate(0, url, nullptr);
                free(url);
            } else {
                AeonBridge::HandleWmAeonBridge(wParam, lParam);
            }
            return 0;
        }

        case WM_DESTROY:
            PostQuitMessage(0);
            return 0;
    }
    return DefWindowProcW(hWnd, msg, wParam, lParam);
}
