// AeonBrowser — PulseBridge.h
#pragma once
#include "../core/probe/HardwareProbe.h"

namespace PulseBridge {
    // Send anonymous startup ping. Fire-and-forget. Respects TelemetryEnabled.
    void SendStartupPing(const SystemProfile& p);

    // Upload pending crash minidump if crash sentinel exists.
    void UploadPendingCrash();
}
