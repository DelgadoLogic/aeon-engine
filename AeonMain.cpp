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
#pragma execution_character_set("utf-8")
#include <windowsx.h>
#include <dwmapi.h>
#include <shlobj.h>
#include <cstdio>
#include <objidl.h>
#include <gdiplus.h>
#include <thread>

#pragma comment(lib, "dwmapi.lib")
#pragma comment(lib, "gdiplus.lib")

#include "core/AeonVersion.h"
#include "core/probe/HardwareProbe.h"
#include "core/engine/AeonEngine_Interface.h"
#include "core/engine/AeonBridge.h"
#include "core/ui/BrowserChrome.h"
#include "core/ui/DwmHelper.h"
#include "core/ui/ShieldHUD.h"
#include "core/ui/BookmarkToast.h"
#include "core/ui/AppMenu.h"
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
AeonTabIntelligence* g_TabIntel = nullptr;
AeonJourneyAnalytics* g_JourneyAI = nullptr;
static bool g_WantDebugTrace = false;

// Declared in TierDispatcher.cpp — loads the engine DLL chain
AeonEngineVTable* TierDispatcher_LoadEngine(const SystemProfile* profile);

struct GdiplusInit {
    ULONG_PTR token;
    GdiplusInit() {
        Gdiplus::GdiplusStartupInput input;
        Gdiplus::GdiplusStartup(&token, &input, nullptr);
    }
    ~GdiplusInit() {
        Gdiplus::GdiplusShutdown(token);
    }
};

int WINAPI WinMain(HINSTANCE hInstance, HINSTANCE, LPSTR, int nShowCmd) {
    AeonDwm::EnablePerMonitorV2Dpi();
    SetCurrentProcessExplicitAppUserModelID(L"DelgadoLogic.AeonBrowser.0.22.0");
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX | SEM_NOOPENFILEERRORBOX);
    HMODULE hK32 = GetModuleHandleW(L"kernel32.dll");
    if (hK32) {
        typedef BOOL(WINAPI* PFN_SetProcessErrorMode)(DWORD);
        PFN_SetProcessErrorMode pfn = (PFN_SetProcessErrorMode)GetProcAddress(hK32, "SetProcessErrorMode");
        if (pfn) pfn(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX | SEM_NOOPENFILEERRORBOX);
        typedef HRESULT(WINAPI* PFN_WerSetFlags)(DWORD);
        PFN_WerSetFlags pfnWer = (PFN_WerSetFlags)GetProcAddress(hK32, "WerSetFlags");
        if (pfnWer) pfnWer(1 /* WER_FAULT_REPORTING_NO_UI */);
    }
    HMODULE hNtdll = GetModuleHandleW(L"ntdll.dll");
    if (hNtdll) {
        typedef LONG(NTAPI* PFN_NtSetInformationProcess)(HANDLE, ULONG, PVOID, ULONG);
        PFN_NtSetInformationProcess pfnNt = (PFN_NtSetInformationProcess)GetProcAddress(hNtdll, "NtSetInformationProcess");
        if (pfnNt) {
            ULONG hardErrorMode = 0;
            pfnNt(GetCurrentProcess(), 12 /* ProcessDefaultHardErrorMode */, &hardErrorMode, sizeof(hardErrorMode));
        }
    }
    _set_abort_behavior(_WRITE_ABORT_MSG, _WRITE_ABORT_MSG | _CALL_REPORTFAULT);
    _set_invalid_parameter_handler([](const wchar_t* expression, const wchar_t* function, const wchar_t* file, unsigned int line, uintptr_t) {
        FILE* f = nullptr;
        fopen_s(&f, "crt_error.log", "a");
        if (f) {
            fwprintf(f, L"[CRT Invalid Parameter] Expr: %s, Func: %s, File: %s:%u\n",
                expression ? expression : L"(null)",
                function ? function : L"(null)",
                file ? file : L"(null)", line);
            fclose(f);
        }
    });
    _set_purecall_handler([]() {
        FILE* f = nullptr;
        fopen_s(&f, "crt_error.log", "a");
        if (f) {
            fwprintf(f, L"[CRT Purecall Error] Pure virtual function called.\n");
            fclose(f);
        }
    });

    GdiplusInit gdiplusInit;

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
        g_WantDebugTrace = wantConsole;
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

    fputws(L"\n", stdout);
    fputws(L"\u2554\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2557\n", stdout);
    fputws(L"\u2551  Aeon Browser  \u2014  by DelgadoLogic    \u2551\n", stdout);
    fputws(L"\u2551  aeon.delgadologic.tech      \u2551\n", stdout);
    fputws(L"\u255a\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u255d\n\n", stdout);

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

    // PHASE 1b-2: Network Sentinel — launch background probe and bypass monitoring.
    NetworkSentinel::StartMonitor();

    // PHASE 1c: Session manager — restore previous tabs on crash/restart.
    SessionManager::Initialize(profile);

    // PHASE 1d: Telemetry baseline ping + crash upload (async background thread to never block UI)
    AeonCrash::SetKey("boot_phase", "telemetry");
    std::thread([profile]() {
        PulseBridge::SendStartupPing(profile);
        PulseBridge::UploadPendingCrash();
    }).detach();
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
        MessageBoxW(nullptr,
            L"Aeon could not load a rendering engine for your system.\n"
            L"Please visit https://aeon.delgadologic.tech for troubleshooting.",
            L"Aeon Browser \u2014 Engine Error", MB_ICONERROR);
        return 1;
    }
    g_Engine = engine;
    SessionManager::SetEngine(engine);

    // Initialize the engine (creates COM apartment, WebView2 env, etc.)
    if (!engine->Init(&profile, hInstance)) {
        MessageBoxW(nullptr,
            L"Aeon rendering engine failed to initialize.\n"
            L"Ensure Microsoft Edge WebView2 Runtime is installed.",
            L"Aeon Browser \u2014 Engine Error", MB_ICONERROR);
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
            if (g_MainHwnd)
                BrowserChrome::SetTabProgress(g_MainHwnd, tab_id, pct);
        };
        cbs.OnTitleChanged = [](unsigned int tab_id, const char* title) {
            fprintf(stdout, "[Engine→Shell] Tab #%u title: \"%s\"\n",
                tab_id, title ? title : "(null)");
            if (g_MainHwnd)
                BrowserChrome::UpdateTabTitle(g_MainHwnd, tab_id, title);

            // Update history entry title (get URL from engine for the lookup)
            if (g_Engine && title && title[0]) {
                char tabUrl[2048] = {};
                g_Engine->GetUrl(tab_id, tabUrl, sizeof(tabUrl));
                if (tabUrl[0] &&
                    strncmp(tabUrl, "aeon://", 7) != 0 &&
                    strncmp(tabUrl, "file:///", 8) != 0 &&
                    strncmp(tabUrl, "about:", 6) != 0) {
                    HistoryEngine::RecordVisit(tabUrl, title);
                }
            }

            // AI: Feed title change to TabIntelligence
            if (g_TabIntel && title && title[0]) {
                char tabUrl[2048] = {};
                if (g_Engine) g_Engine->GetUrl(tab_id, tabUrl, sizeof(tabUrl));
                g_TabIntel->OnTabNavigated((uint64_t)tab_id, tabUrl, title);
            }
        };
        cbs.OnNavigated = [](unsigned int tab_id, const char* url) {
            fprintf(stdout, "[Engine→Shell] Tab #%u navigated: %s\n",
                tab_id, url ? url : "(null)");
            if (g_MainHwnd)
                BrowserChrome::UpdateTabUrl(g_MainHwnd, tab_id, url);

            // Record visit in history (skip internal/file URLs)
            if (url && url[0] &&
                strncmp(url, "aeon://", 7) != 0 &&
                strncmp(url, "file:///", 8) != 0 &&
                strncmp(url, "about:", 6) != 0) {
                HistoryEngine::RecordVisit(url, nullptr);
            }

            // AI: Feed navigation to TabIntelligence + JourneyAnalytics
            if (g_TabIntel && url && url[0])
                g_TabIntel->OnTabNavigated((uint64_t)tab_id, url, nullptr);
            if (g_JourneyAI && url && url[0] &&
                strncmp(url, "aeon://", 7) != 0 &&
                strncmp(url, "about:", 6) != 0) {
                g_JourneyAI->OnPageVisit((uint64_t)tab_id, url, nullptr,
                                         /*from_search=*/false, /*from_link=*/true, /*from_back=*/false);
            }
        };
        cbs.OnLoaded = [](unsigned int tab_id) {
            fprintf(stdout, "[Engine→Shell] Tab #%u loaded\n", tab_id);
            if (g_MainHwnd)
                BrowserChrome::SetTabLoaded(g_MainHwnd, tab_id);

            // AI: Notify JourneyAnalytics page finished loading (initial dwell = 0)
            if (g_JourneyAI)
                g_JourneyAI->OnPageDwell((uint64_t)tab_id, /*dwell_secs=*/0, /*engagement=*/0.0f);
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
            unsigned int new_id = 0;
            if (g_MainHwnd)
                new_id = BrowserChrome::CreateTab(g_MainHwnd, url);

            // AI: Notify TabIntelligence of new tab creation
            if (g_TabIntel && new_id) {
                AeonTabInfo info = {};
                info.tab_id = (uint64_t)new_id;
                if (url) _snprintf_s(info.url, sizeof(info.url), _TRUNCATE, "%s", url);
                _snprintf_s(info.title, sizeof(info.title), _TRUNCATE, "New Tab");
                info.state = AeonTabState::Active;
                g_TabIntel->OnTabCreated(info);
            }
            // AI: Feed new tab navigation to JourneyAnalytics
            if (g_JourneyAI && new_id && url && url[0] &&
                strncmp(url, "aeon://", 7) != 0) {
                g_JourneyAI->OnPageVisit((uint64_t)new_id, url, nullptr,
                                         /*from_search=*/false, /*from_link=*/false, /*from_back=*/false);
            }
        };
        engine->SetCallbacks(&cbs);
        fprintf(stdout, "[Boot] Engine callbacks wired (6 handlers + AI intelligence).\n");
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
    HICON hIconBig = (HICON)LoadImageW(hInstance, MAKEINTRESOURCEW(101), IMAGE_ICON,
        GetSystemMetrics(SM_CXICON), GetSystemMetrics(SM_CYICON), LR_DEFAULTCOLOR);
    if (!hIconBig) {
        hIconBig = (HICON)LoadImageW(hInstance, MAKEINTRESOURCEW(1), IMAGE_ICON,
            GetSystemMetrics(SM_CXICON), GetSystemMetrics(SM_CYICON), LR_DEFAULTCOLOR);
    }
    if (!hIconBig) {
        hIconBig = LoadIconA(hInstance, "IDI_AEON");
    }

    HICON hIconSmall = (HICON)LoadImageW(hInstance, MAKEINTRESOURCEW(101), IMAGE_ICON,
        GetSystemMetrics(SM_CXSMICON), GetSystemMetrics(SM_CYSMICON), LR_DEFAULTCOLOR);
    if (!hIconSmall) {
        hIconSmall = (HICON)LoadImageW(hInstance, MAKEINTRESOURCEW(1), IMAGE_ICON,
            GetSystemMetrics(SM_CXSMICON), GetSystemMetrics(SM_CYSMICON), LR_DEFAULTCOLOR);
    }

    WNDCLASSEXW wc   = {};
    wc.cbSize        = sizeof(wc);
    wc.style         = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc   = AeonWndProc;
    wc.hInstance     = hInstance;
    wc.hIcon         = hIconBig;
    wc.hIconSm       = hIconSmall;
    wc.hCursor       = LoadCursor(nullptr, IDC_ARROW);
    wc.hbrBackground = nullptr;
    wc.lpszClassName = AEON_WINDOW_CLASS;
    RegisterClassExW(&wc);

    // Register popup window classes
    ShieldHUD::RegisterClass(hInstance);
    BookmarkToast::RegisterClass(hInstance);
    AppMenu::RegisterClass(hInstance);

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
        WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN | WS_CLIPSIBLINGS, X, Y, W, H,
        nullptr, nullptr, hInstance, nullptr);

    if (!hWnd) return 1;

    // Explicitly bind high-DPI icons to window handle for Taskbar and Alt+Tab
    if (hIconBig)   SendMessageW(hWnd, WM_SETICON, ICON_BIG, (LPARAM)hIconBig);
    if (hIconSmall) SendMessageW(hWnd, WM_SETICON, ICON_SMALL, (LPARAM)hIconSmall);
    g_MainHwnd = hWnd;  // Set before BrowserChrome::Create so callbacks are routed

    // DWM frameless window + Win11 Mica dark backdrop, rounded corners & obsidian border
    if (profile.osMajor >= 10) {
        MARGINS margins = { 0, 0, 1, 0 };
        DwmExtendFrameIntoClientArea(hWnd, &margins);
        AeonDwm::ApplyObsidianMainFrameDwm(hWnd);
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

    AeonBridge::RegisterScriptEvaluator([](int tabId, const char* script) -> std::string {
        if (!g_Engine || !g_Engine->ExecuteScript) return "";
        char resultBuf[65536] = {0};
        int len = g_Engine->ExecuteScript((unsigned int)tabId, script, resultBuf, sizeof(resultBuf));
        if (len > 0) return std::string(resultBuf);
        return "";
    });

    // 9. Browser chrome (tab strip, nav bar, WebView2 host)
    BrowserChrome::Create(hWnd, &profile, engine);

    // 9a. Session manager — wire to main window, start autosave
    SessionManager::SetUiHooks({
        BrowserChrome::GetTabCount,
        BrowserChrome::GetActiveTabIndex,
        BrowserChrome::GetTabInfo,
        BrowserChrome::CreateTab,
        BrowserChrome::FocusTabById,
        BrowserChrome::AddTabToGroup,
        BrowserChrome::CloseTabById
    });
    SessionManager::SetMainWindow(hWnd);

    // Captive portal — open in first tab if detected
    const NetworkSentinel::SentinelState& sentState = NetworkSentinel::GetState();
    if (sentState.need_captive_portal && sentState.captive_portal_url[0]) {
        if (engine) engine->NewTab(hWnd, sentState.captive_portal_url);
        fprintf(stdout, "[Boot] Captive portal tab: %s\n", sentState.captive_portal_url);
    }

    // 9b. Agent control pipe — Named Pipe IPC for shell control
    AeonAgentPipe::Start(hWnd);

    int show = (nShowCmd <= 0 || nShowCmd == SW_HIDE) ? SW_SHOWNORMAL : nShowCmd;
    ShowWindow(hWnd, show);
    UpdateWindow(hWnd);
    SetForegroundWindow(hWnd);

    // 9c. Start fresh on new tab page (session restore disabled for clean boot)
    // SessionManager::RestorePreviousSession();

    fprintf(stdout, "[Boot] Ready. Entering message loop.\n\n");
    if (g_WantDebugTrace) {
        FILE* fTrace = nullptr;
        fopen_s(&fTrace, "boot_trace.txt", "a");
        if (fTrace) {
            fprintf(fTrace, "[Boot] Ready. Window hWnd=0x%p, entering message loop.\n", (void*)hWnd);
            fclose(fTrace);
        }
    }

    // 10. Message loop
    MSG msg = {};
    while (GetMessageW(&msg, nullptr, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    if (g_WantDebugTrace) {
        FILE* fTrace = nullptr;
        fopen_s(&fTrace, "boot_trace.txt", "a");
        if (fTrace) {
            fprintf(fTrace, "[Boot] Message loop exited. msg.wParam=%lld\n", (long long)msg.wParam);
            fclose(fTrace);
        }
    }

    // 11. Cleanup
    // Session — save all tabs before anything else shuts down
    SessionManager::SaveAndExit();

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
    if (g_WantDebugTrace && (msg == WM_CLOSE || msg == WM_DESTROY || msg == WM_QUIT || msg == WM_SYSCOMMAND || msg == WM_NCDESTROY)) {
        FILE* fTrace = nullptr;
        fopen_s(&fTrace, "wndproc_trace.txt", "a");
        if (fTrace) {
            fprintf(fTrace, "[WndProc] hWnd=0x%p, msg=0x%04X, wParam=0x%llX, lParam=0x%llX\n",
                (void*)hWnd, msg, (unsigned long long)wParam, (unsigned long long)lParam);
            fclose(fTrace);
        }
    }

    switch (msg) {
        // ── Remove native title bar ──────────────────────────────────
        case WM_NCCALCSIZE: {
            if (wParam == TRUE) {
                NCCALCSIZE_PARAMS* params = reinterpret_cast<NCCALCSIZE_PARAMS*>(lParam);
                HMONITOR hMon = MonitorFromWindow(hWnd, MONITOR_DEFAULTTONEAREST);
                MONITORINFO mi = { sizeof(mi) };
                GetMonitorInfo(hMon, &mi);
                if (IsZoomed(hWnd)) {
                    params->rgrc[0] = mi.rcWork;
                }
                return 0;
            }
            break;
        }

        // ── Hit-test for drag, DPI resize borders & Win11 Snap Layouts ─
        case WM_NCHITTEST: {
            POINT pt = { GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };
            ScreenToClient(hWnd, &pt);
            RECT rc;
            GetClientRect(hWnd, &rc);

            UINT dpi = AeonDwm::GetWindowDpiSafe(hWnd);
            const int BORDER = AeonDwm::ScaleByDpi(6, dpi);
            const int CORNER = BORDER * 2;

            if (!IsZoomed(hWnd)) {
                if (pt.y < BORDER) {
                    if (pt.x < CORNER) return HTTOPLEFT;
                    if (pt.x >= rc.right - CORNER) return HTTOPRIGHT;
                    int hit = BrowserChrome::HitTest(hWnd, pt.x, pt.y);
                    if (hit == BrowserChrome::HIT_MAXIMIZE) return HTMAXBUTTON;
                    if (hit >= 0) return HTCLIENT;
                    return HTTOP;
                }
                if (pt.y >= rc.bottom - BORDER) {
                    if (pt.x < CORNER) return HTBOTTOMLEFT;
                    if (pt.x >= rc.right - CORNER) return HTBOTTOMRIGHT;
                    return HTBOTTOM;
                }
                if (pt.x < BORDER) return HTLEFT;
                if (pt.x >= rc.right - BORDER) return HTRIGHT;
            }

            int hit = BrowserChrome::HitTest(hWnd, pt.x, pt.y);
            if (hit == BrowserChrome::HIT_MAXIMIZE) {
                return HTMAXBUTTON;
            }
            if (hit < 0 && BrowserChrome::IsCaptionDragPoint(hWnd, pt.x, pt.y)) {
                return HTCAPTION;
            }
            return HTCLIENT;
        }

        case WM_NCMOUSEMOVE: {
            POINT pt = { GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };
            ScreenToClient(hWnd, &pt);
            BrowserChrome::OnMouseMove(hWnd, pt.x, pt.y);
            TRACKMOUSEEVENT tme = {};
            tme.cbSize = sizeof(tme);
            tme.dwFlags = TME_LEAVE | TME_NONCLIENT;
            tme.hwndTrack = hWnd;
            TrackMouseEvent(&tme);
            if (wParam == HTMAXBUTTON) return 0;
            break;
        }

        case WM_NCMOUSELEAVE:
            BrowserChrome::OnMouseLeave(hWnd);
            return 0;

        case WM_NCLBUTTONDOWN:
            if (wParam == HTMAXBUTTON) return 0;
            break;

        case WM_NCLBUTTONUP:
            if (wParam == HTMAXBUTTON) {
                ShowWindow(hWnd, IsZoomed(hWnd) ? SW_RESTORE : SW_MAXIMIZE);
                return 0;
            }
            break;

        case WM_PAINT: {
            BrowserChrome::OnPaint(hWnd);
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

        case WM_MBUTTONUP: {
            int x = GET_X_LPARAM(lParam);
            int y = GET_Y_LPARAM(lParam);
            int hit = BrowserChrome::HitTest(hWnd, x, y);
            if (hit >= 100 && hit < 200) {
                // Middle click on tab = close tab
                BrowserChrome::CloseTab(hWnd, hit - 100);
            }
            return 0;
        }

        case WM_MOUSEMOVE:
            BrowserChrome::OnMouseMove(hWnd, GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam));
            return 0;

        case WM_MOUSELEAVE:
            BrowserChrome::OnMouseLeave(hWnd);
            return 0;

        case WM_PARENTNOTIFY: {
            if (LOWORD(wParam) == WM_LBUTTONDOWN) {
                int x = GET_X_LPARAM(lParam);
                int y = GET_Y_LPARAM(lParam);
                BrowserChrome::OnContentAreaClicked(hWnd, x, y);
            }
            break;
        }

        case WM_AEON_AGENT: {
            AeonAgentPipe::HandleCommand(wParam, lParam);
            return 0;
        }

        case WM_AEONBRIDGE: {
            if ((int)wParam == BRIDGE_CMD_NAVIGATE) {
                char* url = reinterpret_cast<char*>(lParam);
                if (url && url[0])
                    BrowserChrome::OpenOrFocusUrl(hWnd, url);
                free(url);
            } else {
                AeonBridge::HandleWmAeonBridge(wParam, lParam);
            }
            return 0;
        }

        // ── Mouse wheel (Ctrl + Wheel for Zoom) ──────────────────────────
        case WM_MOUSEWHEEL: {
            if (GetKeyState(VK_CONTROL) & 0x8000) {
                short delta = GET_WHEEL_DELTA_WPARAM(wParam);
                if (delta > 0) BrowserChrome::ZoomIn(hWnd);
                else if (delta < 0) BrowserChrome::ZoomOut(hWnd);
                return 0;
            }
            break;
        }

        // ── Download Button click notification ───────────────────────────
        case WM_APP + 100: {
            BrowserChrome::OpenOrFocusUrl(hWnd, "aeon://downloads");
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

        // ── Loading animation timer → repaint tab strip ──────────────────
        case WM_TIMER:
            if (wParam == 9005 /* ID_HOVER_TIMER */ || wParam == 9002 /* ID_LOADING_TIMER */) {
                BrowserChrome::OnTimer(hWnd, wParam);
                return 0;
            }
            if (wParam == SessionManager::AUTOSAVE_TIMER_ID) {
                SessionManager::OnAutosaveTimer();
                return 0;
            }
            break;

        // ── URL bar dark theme (WM_CTLCOLOREDIT from EDIT child) ────────
        case WM_CTLCOLOREDIT: {
            HBRUSH br = BrowserChrome::OnCtlColorEdit(hWnd,
                reinterpret_cast<HDC>(wParam),
                reinterpret_cast<HWND>(lParam));
            if (br) return reinterpret_cast<LRESULT>(br);
            break;
        }

        // ── Right-click context menu in content area ─────────────────────
        case WM_CONTEXTMENU:
            BrowserChrome::OnContextMenu(hWnd,
                GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam));
            return 0;

        // ── PerMonitorV2 Multi-Monitor DPI scaling ────────────────────
        case WM_DPICHANGED: {
            int newDpi = HIWORD(wParam);
            if (newDpi <= 0) newDpi = LOWORD(wParam);
            BrowserChrome::OnDpiChanged(hWnd, newDpi);
            const RECT* prcNewWindow = reinterpret_cast<const RECT*>(lParam);
            if (prcNewWindow) {
                SetWindowPos(hWnd, nullptr,
                    prcNewWindow->left, prcNewWindow->top,
                    prcNewWindow->right - prcNewWindow->left,
                    prcNewWindow->bottom - prcNewWindow->top,
                    SWP_NOZORDER | SWP_NOACTIVATE);
            }
            return 0;
        }

        case WM_DESTROY:
            AeonAgentPipe::Stop();
            BrowserChrome::Destroy(hWnd);
            PostQuitMessage(0);
            return 0;
    }
    return DefWindowProcW(hWnd, msg, wParam, lParam);
}
