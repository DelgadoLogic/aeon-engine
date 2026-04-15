// AeonBrowser — main.cpp
// DelgadoLogic | Lead Systems Architect
//
// PURPOSE: Browser entry point. Runs HardwareProbe, then dispatches to the
// correct engine, UI skin, and TLS stack. Every downstream module trusts
// the SystemProfile filled here — it is immutable after init.
//
// IT TROUBLESHOOTING:
//   - If the wrong tier loads, run with AEON_DEBUG=1 env var to dump probe.
//   - Crash at startup on XP usually means BearSSL init failed or OCA missing.
//   - "No SSE2" crash on XP: confirm EraChrome is loading GeckoLite, not Blink.

#include "AeonVersion.h"
#include "probe/HardwareProbe.h"
#include "engine/TierDispatcher.h"
#include "engine/AeonEngine_Interface.h"
#include "engine/AeonBridge.h"
#include "ui/EraChrome.h"
#include "ui/BrowserChrome.h"
#include "tls/TlsAbstraction.h"
#include "session/SessionManager.h"
#include "crash/CrashHandler.h"
#include "telemetry/PulseBridge.h"

#include <windows.h>
#include <cstdio>
#include <cstring>

// Global profile — read-only after RunProbe(); never modify outside main().
static SystemProfile g_Profile;

// Global main HWND — needed by callbacks that fire from engine threads.
static HWND g_MainHwnd = nullptr;

// ---------------------------------------------------------------------------
// Engine Callback Implementations
// ---------------------------------------------------------------------------
// These are called by the engine DLL (potentially on a background thread
// in multiprocess engines). They forward to BrowserChrome via the UI thread.
// For single-process engines (HTML4, WebView2 stub), they are already on
// the UI thread so the call is direct.

static void __cdecl CB_OnProgress(unsigned int tab_id, int percent) {
    // Future: update a loading progress bar in the chrome.
    // For now, just trace for diagnostics.
    fprintf(stdout, "[Engine→Shell] OnProgress: tab #%u → %d%%\n", tab_id, percent);
}

static void __cdecl CB_OnTitleChanged(unsigned int tab_id, const char* title) {
    fprintf(stdout, "[Engine→Shell] OnTitleChanged: tab #%u → \"%s\"\n",
            tab_id, title ? title : "(null)");
    if (g_MainHwnd) {
        BrowserChrome::UpdateTabTitle(g_MainHwnd, tab_id, title);
    }
}

static void __cdecl CB_OnNavigated(unsigned int tab_id, const char* url) {
    fprintf(stdout, "[Engine→Shell] OnNavigated: tab #%u → %s\n",
            tab_id, url ? url : "(null)");
    if (g_MainHwnd) {
        BrowserChrome::UpdateTabUrl(g_MainHwnd, tab_id, url);
    }
}

static void __cdecl CB_OnLoaded(unsigned int tab_id) {
    fprintf(stdout, "[Engine→Shell] OnLoaded: tab #%u\n", tab_id);
    if (g_MainHwnd) {
        BrowserChrome::SetTabLoaded(g_MainHwnd, tab_id);
    }
}

static void __cdecl CB_OnCrash(unsigned int tab_id, const char* reason) {
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
}

static void __cdecl CB_OnNewTab(unsigned int parent_tab_id, const char* url) {
    fprintf(stdout, "[Engine→Shell] OnNewTab: parent #%u → %s\n",
            parent_tab_id, url ? url : "(null)");
    if (g_MainHwnd) {
        BrowserChrome::CreateTab(g_MainHwnd, url);
    }
}

// The callback struct — wired to engine via SetCallbacks()
static AeonEngineCallbacks g_EngineCallbacks = {
    CB_OnProgress,
    CB_OnTitleChanged,
    CB_OnNavigated,
    CB_OnLoaded,
    CB_OnCrash,
    CB_OnNewTab
};

// ---------------------------------------------------------------------------
// AeonBridge navigate callback — routes JS navigation requests to the engine
// ---------------------------------------------------------------------------
static AeonEngineVTable* g_EngineVTable = nullptr;

static void BridgeNavigateCallback(const char* url) {
    if (!g_MainHwnd || !g_EngineVTable || !url || !url[0]) return;

    // Navigate the active tab
    int activeIdx = BrowserChrome::GetActiveTabIndex(g_MainHwnd);
    if (activeIdx >= 0) {
        unsigned int tabId = 0;
        BrowserChrome::GetTabInfo(g_MainHwnd, activeIdx, &tabId,
                                 nullptr, 0, nullptr, 0, nullptr);
        if (tabId > 0) {
            BrowserChrome::NavigateTab(g_MainHwnd, tabId, url);
        }
    }
}

// ---------------------------------------------------------------------------
// WinMain — entry point
// ---------------------------------------------------------------------------
int WINAPI WinMain(HINSTANCE hInst, HINSTANCE, LPSTR lpCmd, int nCmdShow) {

    // -----------------------------------------------------------------------
    // PHASE 1: Install crash handler FIRST — catches any init failures.
    // IT NOTE: CrashHandler registers a VectoredExceptionHandler that writes
    // a minidump to %TEMP%\aeon_crash_<timestamp>.dmp and queues it for
    // upload to telemetry.delgadologic.tech via PulseBridge on next launch.
    // -----------------------------------------------------------------------
    AeonCrash::Install();

    // -----------------------------------------------------------------------
    // PHASE 2: Hardware probe — determines everything that follows.
    // -----------------------------------------------------------------------
    AeonTier tier = AeonProbe::RunProbe(g_Profile);

#ifdef AEON_DEBUG
    AeonProbe::DumpProfile(g_Profile);
#endif

    fprintf(stdout, "[Main] Aeon Browser v%s starting...\n", AEON_VERSION);

    // -----------------------------------------------------------------------
    // PHASE 3: TLS stack — must be initialised before ANY network call.
    // IT NOTE: On Win9x this uses BearSSL (statically linked in retro DLL).
    //          On Vista it applies the TLS 1.2 schannel registry keys.
    //          On Win7+ it just enables TLS 1.2/1.3 in schannel if disabled.
    //          On Win10/11 the OS already supports TLS 1.3 natively.
    // -----------------------------------------------------------------------
    if (!AeonTls::Initialize(g_Profile)) {
        MessageBoxA(nullptr,
            "Aeon Browser: TLS initialization failed.\n"
            "BearSSL could not initialize. Network features will not work.\n"
            "Try reinstalling Aeon Browser.",
            "Aeon — TLS Error", MB_ICONERROR | MB_OK);
        // Continue in offline mode — don't exit, browser still useful locally
    }

    // -----------------------------------------------------------------------
    // PHASE 4: Session manager — restore previous tabs on crash/restart.
    // -----------------------------------------------------------------------
    SessionManager::Initialize(g_Profile);

    // -----------------------------------------------------------------------
    // PHASE 5: Telemetry bridge — fire-and-forget baseline on first run.
    // IT NOTE: Reads HKLM\SOFTWARE\DelgadoLogic\Aeon\TelemetryEnabled.
    //          If 0, all telemetry is suppressed. Same opt-out as LogicFlow.
    // -----------------------------------------------------------------------
    PulseBridge::SendStartupPing(g_Profile);

    // -----------------------------------------------------------------------
    // PHASE 6: Tier Dispatcher — loads AND initializes the rendering engine.
    // The engine DLL's Init() is called inside LoadEngine(). If it fails,
    // the engine is shut down and we show an error.
    // -----------------------------------------------------------------------
    TierDispatcher dispatch(g_Profile, hInst);
    if (!dispatch.LoadEngine()) {
        MessageBoxA(nullptr,
            "Aeon Browser: Could not load rendering engine for this OS tier.\n"
            "Please reinstall Aeon Browser.",
            "Aeon — Engine Error", MB_ICONERROR | MB_OK);
        return 1;
    }

    AeonEngineVTable* engine = dispatch.GetEngine();
    g_EngineVTable = engine;

    // -----------------------------------------------------------------------
    // PHASE 7: Register engine callbacks — wires engine events to the shell.
    // This must happen after Init() but before any navigation occurs.
    // -----------------------------------------------------------------------
    if (engine && engine->SetCallbacks) {
        engine->SetCallbacks(&g_EngineCallbacks);
        fprintf(stdout, "[Main] Engine callbacks registered (6 handlers).\n");
    }

    // -----------------------------------------------------------------------
    // PHASE 8: UI — paint the correct skin over the loaded engine.
    // EraChrome::CreateBrowserWindow() internally calls:
    //   1. BrowserChrome::Create() — sets up tabs, URL bar, first tab
    //   2. AeonAgentPipe::Start()  — starts the named pipe IPC server
    // -----------------------------------------------------------------------
    EraChrome ui(g_Profile, hInst, nCmdShow);
    ui.SetEngine(engine);
    ui.CreateBrowserWindow();

    g_MainHwnd = ui.GetHwnd();

    // -----------------------------------------------------------------------
    // PHASE 9: Initialize AeonBridge — JS host object for aeon:// pages.
    // Must happen after the main HWND exists.
    // -----------------------------------------------------------------------
    if (g_MainHwnd) {
        AeonBridge::Init(g_MainHwnd, BridgeNavigateCallback);
        fprintf(stdout, "[Main] AeonBridge initialized.\n");
    }

    // -----------------------------------------------------------------------
    // PHASE 10: Run the message loop.
    // -----------------------------------------------------------------------
    fprintf(stdout, "[Main] Entering message loop. Browser is live.\n");
    return ui.Run(lpCmd);
}
