// AeonBrowser — AeonMain.cpp
// DelgadoLogic
//
// Main entry point — Win32 WinMain.
// Uses AeonProbe::RunProbe() for tier detection, TierDispatcher_LoadEngine() for
// engine DLL loading, and AeonBridge::Init() for JS↔C++ communication.
//
// Session 19: Added CrashHandler, TLS init, SessionManager, PulseBridge phases.
//             Fixed OnCrash/OnNewTab callbacks to actually modify UI state.
//             Added __cdecl calling convention for ABI compliance.

#include <windows.h>
#include <windowsx.h>
#include <dwmapi.h>
#include <shlobj.h>
#include <cstdio>

#pragma comment(lib, "dwmapi.lib")

#include "core/AeonVersion.h"
#include "core/probe/HardwareProbe.h"
#include "core/engine/AeonEngine_Interface.h"
#include "core/engine/AeonBridge.h"
#include "core/ui/BrowserChrome.h"
#include "core/network/NetworkSentinel.h"
#include "core/settings/SettingsEngine.h"
#include "core/memory/TabSleepManager.h"
#include "core/security/PasswordVault.h"
#include "core/crash/CrashHandler.h"
#include "core/crash/CrashKeys.h"
#include "core/crash/Breadcrumbs.h"
#include "core/crash/AeonLog.h"
#include "core/tls/TlsAbstraction.h"
#include "core/session/SessionManager.h"
#include "telemetry/PulseBridge.h"
#include "updater/AutoUpdater.h"
#include "core/billing/OmniLicense.h"
#include "core/history/HistoryEngine.h"
#include "core/download/DownloadManager.h"
#include "core/agent/AeonAgentPipe.h"
#include "ai/aeon_tab_intelligence.h"
#include "ai/aeon_journey_analytics.h"

#define AEON_WINDOW_CLASS L"AeonBrowserHost"
#define AEON_TITLE        L"Aeon Browser \u2014 by DelgadoLogic"

// Forward declarations
LRESULT CALLBACK AeonWndProc(HWND, UINT, WPARAM, LPARAM);

// Global engine pointer — shared between AeonMain and WndProc.
// BrowserChrome owns GWLP_USERDATA for its ChromeState, so we use a global.
static AeonEngineVTable* g_Engine = nullptr;

// Global main window handle — used by engine callbacks to push events
// into BrowserChrome's tab state via the public API.
static HWND g_MainHwnd = nullptr;

// Global AI engine pointers — initialized during boot, non-owning refs
static AeonTabIntelligence* g_TabIntel = nullptr;
static AeonJourneyAnalytics* g_JourneyAI = nullptr;

// Declared in TierDispatcher.cpp — loads the engine DLL chain
AeonEngineVTable* TierDispatcher_LoadEngine(const SystemProfile* profile);

int WINAPI WinMain(HINSTANCE hInstance, HINSTANCE, LPSTR, int nShowCmd) {

    // PHASE 0: Install crash handler FIRST — catches any init failures.
    AeonCrash::Install();
    AeonCrash::SetKey("boot_phase", "init");
    AeonCrash::SetKey("version", AEON_VERSION);
    AeonCrash::AddBreadcrumb("boot", "crash_handler_installed");

    // Console logging for diagnostics — attach console when --debug flag is passed.
    // Release builds use the internal logging subsystem (HistoryEngine trace log).
    {
        bool wantConsole = false;
        for (int i = 1; i < __argc; ++i) {
            if (strcmp(__argv[i], "--debug") == 0) { wantConsole = true; break; }
        }
        if (wantConsole) {
            AllocConsole();
            FILE* _f = nullptr;
            freopen_s(&_f, "CONOUT$", "w", stdout);
            freopen_s(&_f, "CONOUT$", "w", stderr);
            fprintf(stdout, "[Aeon] Debug console attached.\n");
        }
    }

    // Initialize structured logger — after console, before anything else
    AeonLog::Init();
    AeonCrash::AddBreadcrumb("boot", "logger_initialized");

    fprintf(stdout, "\n");
    fprintf(stdout, "\u2554\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2557\n");
    fprintf(stdout, "\u2551  Aeon Browser  \u2014  by DelgadoLogic    \u2551\n");
    fprintf(stdout, "\u2551  browseaeon.com              \u2551\n");
    fprintf(stdout, "\u255a\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u255d\n\n");

    // PHASE 1: Hardware probe — detect OS tier + CPU
    AeonCrash::SetKey("boot_phase", "hardware_probe");
    SystemProfile profile = {};
    AeonTier tier = AeonProbe::RunProbe(profile);
    ALOG_INFO("Boot", "Aeon Browser v%s starting...", AEON_VERSION);
    ALOG_INFO("Boot", "Tier: %s | OS: %u.%u.%u | Cores: %d | RAM: %.0f MB",
        AeonProbe::TierName(tier),
        profile.osMajor, profile.osMinor, profile.osBuild,
        (int)profile.cpu.cores,
        (double)profile.ramBytes / (1024.0 * 1024.0));
    AeonCrash::SetKey("tier", AeonProbe::TierName(tier));
    AeonCrash::SetKeyInt("ram_mb", (int64_t)(profile.ramBytes / (1024 * 1024)));
    AeonCrash::AddBreadcrumb("boot", "hardware_probe_complete");

    // PHASE 1b: TLS stack — must be initialised before ANY network call.
    if (!AeonTls::Initialize(profile)) {
        fprintf(stderr, "[Boot] WARNING: TLS init failed — network features may not work.\n");
    }

    // PHASE 1c: Session manager — restore previous tabs on crash/restart.
    SessionManager::Initialize(profile);

    // PHASE 1d: Telemetry baseline ping + crash upload from previous session
    AeonCrash::SetKey("boot_phase", "telemetry");
    PulseBridge::SendStartupPing(profile);
    PulseBridge::UploadPendingCrash();
    AeonCrash::AddBreadcrumb("boot", "telemetry_initialized");

    // OmniLicense Hardware Check
    fprintf(stdout, "[Boot] Evaluating Crypto Signature via OmniLicense...\n");
    std::string hwid = OmniLicense::GenerateHWID();
    fprintf(stdout, "[Boot] OmniLicense HWID: %s\n", hwid.c_str());

    // 2. Load settings (single load — used by vault, download manager, updater)
    AeonSettings settings = SettingsEngine::Load();

    // 3. Initialize Password Vault
    char appData[MAX_PATH] = {};
    SHGetFolderPathA(nullptr, CSIDL_APPDATA, nullptr, 0, appData);

    // Create parent dir %APPDATA%\DelgadoLogic first
    char dlDir[MAX_PATH];
    _snprintf_s(dlDir, sizeof(dlDir), _TRUNCATE, "%s\\DelgadoLogic", appData);
    if (!CreateDirectoryA(dlDir, nullptr) && GetLastError() != ERROR_ALREADY_EXISTS) {
        fprintf(stderr, "[Boot] WARNING: Cannot create %s: %lu\n", dlDir, GetLastError());
    }

    // Then create %APPDATA%\DelgadoLogic\Aeon
    char vaultDir[MAX_PATH];
    _snprintf_s(vaultDir, sizeof(vaultDir), _TRUNCATE, "%s\\Aeon", dlDir);
    if (!CreateDirectoryA(vaultDir, nullptr) && GetLastError() != ERROR_ALREADY_EXISTS) {
        fprintf(stderr, "[Boot] WARNING: Cannot create %s: %lu\n", vaultDir, GetLastError());
    }

    char vaultPath[MAX_PATH];
    _snprintf_s(vaultPath, sizeof(vaultPath), _TRUNCATE, "%s\\vault.db", vaultDir);
    if (!PasswordVault::Init(vaultPath)) {
        fprintf(stderr, "[Boot] WARNING: Vault init failed — passwords will not persist.\n");
    }

    // 4a. History engine (SQLite DB for visits + bookmarks)
    char historyPath[MAX_PATH];
    _snprintf_s(historyPath, sizeof(historyPath), _TRUNCATE, "%s\\history.db", vaultDir);
    if (!HistoryEngine::Init(historyPath, /*incognito=*/false)) {
        fprintf(stderr, "[Boot] WARNING: HistoryEngine init failed — history will not persist.\n");
    } else {
        fprintf(stdout, "[Boot] HistoryEngine initialized: %s\n", historyPath);
    }

    // 4b. Download manager (reuses settings loaded above)
    const char* downloadStartDir = (settings.dl_path[0]) ? settings.dl_path : nullptr;
    if (!DownloadManager::Init(downloadStartDir)) {
        fprintf(stderr, "[Boot] WARNING: DownloadManager init failed — downloads disabled.\n");
    } else {
        fprintf(stdout, "[Boot] DownloadManager initialized: %s\n",
            DownloadManager::GetDownloadDir());
    }

    // 5. Load rendering engine DLL for this tier
    AeonEngineVTable* engine = TierDispatcher_LoadEngine(&profile);
    if (!engine) {
        MessageBoxA(nullptr,
            "Aeon could not load a rendering engine for your system.\n"
            "Please visit browseaeon.com for troubleshooting.",
            "Aeon Browser \u2014 Engine Error", MB_ICONERROR);
        return 1;
    }
    g_Engine = engine;

    // Initialize the engine (creates COM apartment, WebView2 env, etc.)
    if (!engine->Init(&profile, hInstance)) {
        MessageBoxA(nullptr,
            "Aeon rendering engine failed to initialize.\n"
            "Ensure Microsoft Edge WebView2 Runtime is installed.",
            "Aeon Browser \u2014 Engine Error", MB_ICONERROR);
        return 1;
    }

    // Wire engine event callbacks — CRITICAL: without this, the engine
    // silently discards all navigation events and BrowserChrome never
    // learns that pages loaded, titles changed, or URLs committed.
    // NOTE: g_MainHwnd is set later (after CreateWindowExW), but these
    // callbacks only fire asynchronously from the WebView2 message pump,
    // so g_MainHwnd is guaranteed to be valid by invocation time.
    {
        static AeonEngineCallbacks cbs = {};
        cbs.OnProgress = [](unsigned int tab_id, int pct) {
            fprintf(stdout, "[Engine→Shell] Tab #%u progress: %d%%\n", tab_id, pct);
        };
        cbs.OnTitleChanged = [](unsigned int tab_id, const char* title) {
            fprintf(stdout, "[Engine→Shell] Tab #%u title: \"%s\"\n",
                tab_id, title ? title : "(null)");
            if (g_MainHwnd)
                BrowserChrome::UpdateTabTitle(g_MainHwnd, tab_id, title);
        };
        cbs.OnNavigated = [](unsigned int tab_id, const char* url) {
            fprintf(stdout, "[Engine→Shell] Tab #%u navigated: %s\n",
                tab_id, url ? url : "(null)");
            if (g_MainHwnd)
                BrowserChrome::UpdateTabUrl(g_MainHwnd, tab_id, url);
        };
        cbs.OnLoaded = [](unsigned int tab_id) {
            fprintf(stdout, "[Engine→Shell] Tab #%u loaded\n", tab_id);
            if (g_MainHwnd)
                BrowserChrome::SetTabLoaded(g_MainHwnd, tab_id);
        };
        cbs.OnCrash = [](unsigned int tab_id, const char* reason) {
            fprintf(stderr, "[Engine→Shell] CRASH: tab #%u — %s\n",
                tab_id, reason ? reason : "unknown");
            if (g_MainHwnd) {
                // Navigate the crashed tab to the internal crash page
                char crashUrl[512];
                _snprintf_s(crashUrl, sizeof(crashUrl), _TRUNCATE,
                            "aeon://crash?tab=%u&reason=%s", tab_id,
                            reason ? reason : "unknown");
                BrowserChrome::NavigateTab(g_MainHwnd, tab_id, crashUrl);
            }
        };
        cbs.OnNewTab = [](unsigned int parent_id, const char* url) {
            fprintf(stdout, "[Engine→Shell] NewTab: parent #%u → %s\n",
                parent_id, url ? url : "(null)");
            if (g_MainHwnd)
                BrowserChrome::CreateTab(g_MainHwnd, url);
        };
        engine->SetCallbacks(&cbs);
        fprintf(stdout, "[Boot] Engine callbacks wired (6 handlers).\n");
    }

    // 5. Tab Sleep Manager
    TabSleepManager::Initialize((unsigned int)tier);

    // 5b. AI — Tab Intelligence Engine
    // NOTE: AI engines are initialized AFTER engine validation above.
    // If engine loading failed, we already returned — no leak risk.
    {
        // Build resource budget from probe results
        ResourceBudget aiBudget;
        aiBudget.max_ram_bytes   = (profile.ramBytes > (2ULL * 1024 * 1024 * 1024))
                                     ? 64 * 1024 * 1024   // 64 MB budget on 2GB+ systems
                                     : 16 * 1024 * 1024;  // 16 MB budget on low-RAM
        aiBudget.cpu_class       = (profile.cpu.hasAVX2) ? AEON_CPU_CLASS_AVX2 :
                                   (profile.cpu.hasSSE4) ? AEON_CPU_CLASS_SSE4 :
                                   AEON_CPU_CLASS_SSE2;
        aiBudget.hive_offload_ok = false;  // Hive mesh not yet active
        aiBudget.target_latency_ms = 50;   // 50ms target for tab classification

        auto* tabAI = new AeonTabIntelligence();
        if (tabAI->Initialize(aiBudget)) {
            g_TabIntel = tabAI;
            fprintf(stdout, "[Boot] AeonTabIntelligence initialized (tier=%s, RAM=%.0f MB, budget=%zu KB)\n",
                AeonProbe::TierName(tier), (double)profile.ramBytes / (1024.0 * 1024.0),
                aiBudget.max_ram_bytes / 1024);
        } else {
            fprintf(stderr, "[Boot] WARNING: AeonTabIntelligence init failed — AI tab grouping disabled.\n");
            delete tabAI;
        }
    }

    // 5c. AI — Journey Analytics Engine
    {
        ResourceBudget journeyBudget;
        journeyBudget.max_ram_bytes    = 32 * 1024 * 1024;  // 32 MB for journey graphs
        journeyBudget.cpu_class        = (profile.cpu.hasAVX2) ? AEON_CPU_CLASS_AVX2 :
                                         (profile.cpu.hasSSE4) ? AEON_CPU_CLASS_SSE4 :
                                         AEON_CPU_CLASS_SSE2;
        journeyBudget.hive_offload_ok  = false;
        journeyBudget.target_latency_ms = 100;  // Journey detection is less latency-critical

        auto* journeyAI = new AeonJourneyAnalytics();
        if (journeyAI->Initialize(journeyBudget)) {
            g_JourneyAI = journeyAI;
            fprintf(stdout, "[Boot] AeonJourneyAnalytics initialized.\n");
        } else {
            fprintf(stderr, "[Boot] WARNING: AeonJourneyAnalytics init failed — journey tracking disabled.\n");
            delete journeyAI;
        }
    }

    // 6. Auto-updater (v2: staged install + background P2P poller)
    AutoUpdater::CheckAndInstallStagedUpdate(); // Apply staged update BEFORE windows
    if (settings.auto_update) {
        AutoUpdater::Start({}); // Empty peer list — GCS-only until Hive is live
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

    // NOTE: WS_EX_NOREDIRECTIONBITMAP was removed because BrowserChrome
    // renders via GDI (BitBlt), which paints to a null surface when that
    // flag is set. Mica/Acrylic requires DirectComposition; we can re-enable
    // this once the chrome rendering migrates to Direct2D/DXGI.
    DWORD exStyle = 0;

    HWND hWnd = CreateWindowExW(
        exStyle, AEON_WINDOW_CLASS, AEON_TITLE,
        WS_OVERLAPPEDWINDOW, X, Y, W, H,
        nullptr, nullptr, hInstance, nullptr);

    if (!hWnd) return 1;
    g_MainHwnd = hWnd;  // Set before BrowserChrome::Create so callbacks are routed

    // DWM frameless window setup (Win10+)
    // NOTE: margins={-1} was removed — it extended DWM glass over the entire
    // client area, covering our GDI-painted chrome. Mica backdrop was also
    // removed as it requires WS_EX_NOREDIRECTIONBITMAP (incompatible with GDI).
    // Use {0,0,1,0} for the DWM drop-shadow trick on frameless windows.
    if (profile.osMajor >= 10) {
        MARGINS margins = { 0, 0, 1, 0 };
        DwmExtendFrameIntoClientArea(hWnd, &margins);
        // Dark mode title bar (prevents white flash on any residual NC area)
        BOOL darkMode = TRUE;
        DwmSetWindowAttribute(hWnd, 20 /*DWMWA_USE_IMMERSIVE_DARK_MODE*/,
            &darkMode, sizeof(darkMode));
    }

    // Force Windows to re-evaluate WM_NCCALCSIZE — this activates the
    // frameless window (removes native title bar) so our custom chrome shows.
    SetWindowPos(hWnd, nullptr, 0, 0, 0, 0,
        SWP_FRAMECHANGED | SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER);

    // NOTE: GWLP_USERDATA is reserved for BrowserChrome's ChromeState.
    // Engine pointer is stored in g_Engine global instead.

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

    // 9b. Agent control pipe — Named Pipe IPC for shell control
    AeonAgentPipe::Start(hWnd);

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
    // AI engines — flush and free before tab/memory systems
    if (g_JourneyAI) { g_JourneyAI->Shutdown(); delete g_JourneyAI; g_JourneyAI = nullptr; }
    if (g_TabIntel) { g_TabIntel->Shutdown(); delete g_TabIntel; g_TabIntel = nullptr; }

    AeonAgentPipe::Stop();
    NetworkSentinel::StopMonitor();
    TabSleepManager::Shutdown();
    DownloadManager::Shutdown();
    HistoryEngine::Shutdown();
    PasswordVault::Lock();
    if (engine && engine->Shutdown) engine->Shutdown();

    fprintf(stdout, "[Boot] Aeon shutdown clean.\n");
    return (int)msg.wParam;
}

// Window procedure
LRESULT CALLBACK AeonWndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
        // ── Remove native title bar ──────────────────────────────────
        case WM_NCCALCSIZE: {
            if (wParam == TRUE) {
                // Return 0 with TRUE wParam = client area fills entire window
                // This eliminates the native title bar entirely.
                // Preserve the auto-hide taskbar behavior by adjusting for it.
                NCCALCSIZE_PARAMS* params = reinterpret_cast<NCCALCSIZE_PARAMS*>(lParam);
                HMONITOR hMon = MonitorFromWindow(hWnd, MONITOR_DEFAULTTONEAREST);
                MONITORINFO mi = { sizeof(mi) };
                GetMonitorInfo(hMon, &mi);
                if (IsZoomed(hWnd)) {
                    // When maximized, respect taskbar area
                    params->rgrc[0] = mi.rcWork;
                }
                return 0;
            }
            break;
        }

        // ── Hit-test for drag, resize, and caption buttons ───────────
        case WM_NCHITTEST: {
            POINT pt = { GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };
            ScreenToClient(hWnd, &pt);
            RECT rc;
            GetClientRect(hWnd, &rc);

            // Resize borders (6px) — always check first
            const int BORDER = 6;
            if (!IsZoomed(hWnd)) {
                if (pt.y < BORDER) {
                    if (pt.x < BORDER) return HTTOPLEFT;
                    if (pt.x > rc.right - BORDER) return HTTOPRIGHT;
                    return HTTOP;
                }
                if (pt.y > rc.bottom - BORDER) {
                    if (pt.x < BORDER) return HTBOTTOMLEFT;
                    if (pt.x > rc.right - BORDER) return HTBOTTOMRIGHT;
                    return HTBOTTOM;
                }
                if (pt.x < BORDER) return HTLEFT;
                if (pt.x > rc.right - BORDER) return HTRIGHT;
            }

            // Nav bar area (y < 40): check if over a button
            if (pt.y < 40) {
                int chromeHit = BrowserChrome::HitTest(hWnd, pt.x, pt.y);
                if (chromeHit >= 0) {
                    // Over a clickable button → let WM_LBUTTONDOWN handle it
                    return HTCLIENT;
                }
                // Empty nav bar area → draggable caption
                return HTCAPTION;
            }

            // Tab strip area (40-72): also check for tab clicks
            if (pt.y < 72) {
                int chromeHit = BrowserChrome::HitTest(hWnd, pt.x, pt.y);
                if (chromeHit >= 0) return HTCLIENT;
                return HTCAPTION; // Empty tab strip area = also draggable
            }

            return HTCLIENT;
        }

        case WM_PAINT: {
            // BeginPaint/EndPaint is required for proper DWM composition.
            // Without it, GDI content flashes and gets overwritten by DWM.
            PAINTSTRUCT ps;
            BeginPaint(hWnd, &ps);
            BrowserChrome::OnPaint(hWnd);
            EndPaint(hWnd, &ps);
            return 0;
        }
        case WM_SIZE:
            BrowserChrome::OnSize(hWnd, LOWORD(lParam), HIWORD(lParam));
            return 0;

        case WM_ERASEBKGND:
            return 1;

        // ── Mouse events → BrowserChrome ─────────────────────────────
        case WM_LBUTTONDOWN:
            BrowserChrome::OnLButtonDown(hWnd, GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam));
            return 0;

        case WM_LBUTTONUP:
            BrowserChrome::OnLButtonUp(hWnd, GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam));
            return 0;

        case WM_MOUSEMOVE:
            BrowserChrome::OnMouseMove(hWnd, GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam));
            return 0;

        case WM_AEONBRIDGE: {
            if ((int)wParam == BRIDGE_CMD_NAVIGATE) {
                char* url = reinterpret_cast<char*>(lParam);
                if (g_Engine && g_Engine->Navigate && url && url[0])
                    g_Engine->Navigate(0, url, nullptr);
                free(url);
            } else {
                AeonBridge::HandleWmAeonBridge(wParam, lParam);
            }
            return 0;
        }

        // ── Keyboard shortcuts → BrowserChrome ───────────────────────
        case WM_KEYDOWN:
        case WM_SYSKEYDOWN:
            if (BrowserChrome::OnKeyDown(hWnd, wParam, lParam))
                return 0;
            break;

        // ── Command from child controls (URL bar EDIT) ───────────────
        case WM_COMMAND:
            BrowserChrome::OnCommand(hWnd, LOWORD(wParam), HIWORD(wParam),
                reinterpret_cast<HWND>(lParam));
            return 0;

        // ── Agent pipe command dispatch ─────────────────────────────────
        case WM_AEON_AGENT:
            AeonAgentPipe::HandleCommand(wParam, lParam);
            return 0;

        case WM_DESTROY:
            PostQuitMessage(0);
            return 0;
    }
    return DefWindowProcW(hWnd, msg, wParam, lParam);
}
