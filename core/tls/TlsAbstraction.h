// AeonBrowser — TlsAbstraction.h
#pragma once
#include "../probe/HardwareProbe.h"

namespace AeonTls {
    // Returns true if TLS stack is ready for use.
    // On failure, caller may continue in offline mode.
    bool Initialize(const SystemProfile& p);
}
