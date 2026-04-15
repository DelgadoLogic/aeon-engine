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

#include "probe/HardwareProbe.h"
#include "engine/TierDispatcher.h"
#include "ui/EraChrome.h"
#include "tls/TlsAbstraction.h"
#include "session/SessionManager.h"
#include "crash/CrashHandler.h"
#include "telemetry/PulseBridge.h"

#include <windows.h>
#include <cstdio>

// Global profile — read-only after RunProbe(); never modify outside main().
static SystemProfile g_Profile;

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
    // PHASE 6: Tier Dispatcher — loads the correct rendering engine.
    // -----------------------------------------------------------------------
    TierDispatcher dispatch(g_Profile, hInst);
    if (!dispatch.LoadEngine()) {
        MessageBoxA(nullptr,
            "Aeon Browser: Could not load rendering engine for this OS tier.\n"
            "Please reinstall Aeon Browser.",
            "Aeon — Engine Error", MB_ICONERROR | MB_OK);
        return 1;
    }

    // -----------------------------------------------------------------------
    // PHASE 7: UI — paint the correct skin over the loaded engine.
    // -----------------------------------------------------------------------
    EraChrome ui(g_Profile, hInst, nCmdShow);
    ui.CreateBrowserWindow();

    // -----------------------------------------------------------------------
    // PHASE 8: Run the message loop.
    // -----------------------------------------------------------------------
    return ui.Run(lpCmd);
}
