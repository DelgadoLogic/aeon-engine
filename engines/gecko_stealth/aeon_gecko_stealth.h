// AeonBrowser — aeon_gecko_stealth.h
// DelgadoLogic | Camoufox Firefox C++ Stealth Engine
//
// PURPOSE: Camoufox C++ Stealth Engine header interface.
// Implements engine-level fingerprint spoofing logic:
//   1. navigator.webdriver removal (native C++ binding/WebIDL override).
//   2. cdc_ key stripping (window object automation property filter).
//   3. WebGL GPU vendor spoofing (UNMASKED_VENDOR_WEBGL 37445 & UNMASKED_RENDERER_WEBGL 37446).
//   4. Natural Bezier curve mouse trajectory generation.

#ifndef AEON_GECKO_STEALTH_H
#define AEON_GECKO_STEALTH_H

#include "../../core/engine/AeonEngine_Interface.h"
#include <vector>
#include <string>
#include <cmath>

#ifdef AEON_GECKO_STEALTH_EXPORTS
#define AEON_STEALTH_API extern "C" __declspec(dllexport)
#else
#define AEON_STEALTH_API extern "C" __declspec(dllimport)
#endif

// Struct representing a 2D point along a synthetic mouse movement path
struct AeonMousePoint {
    double x;
    double y;
    double timestampMs;
};

// C++ Bezier curve natural mouse trajectory generator
class AeonBezierTrajectoryGenerator {
public:
    static std::vector<AeonMousePoint> GenerateTrajectory(
        double startX, double startY,
        double endX, double endY,
        int steps = 60,
        double totalDurationMs = 400.0
    );
};

// Stealth Config and Native C++ Fingerprint Spoofing Manager
class AeonGeckoStealthConfig {
public:
    std::string unmaskedVendor   = "Google Inc. (NVIDIA)";
    std::string unmaskedRenderer = "ANGLE (NVIDIA, NVIDIA GeForce RTX 4090 Direct3D11 vs_5_0 ps_5_0, D3D11)";
    bool removeWebdriver         = true;
    bool stripCdcKeys            = true;

    // Generates the early C++ engine-level stealth JS patch to inject before any page scripts
    std::string BuildStealthBootstrapScript() const;
};

// Exported Pure C ABI functions
AEON_STEALTH_API int               __cdecl AeonEngine_AbiVersion(void);
AEON_STEALTH_API AeonEngineVTable* __cdecl AeonEngine_Create(void);

#endif // AEON_GECKO_STEALTH_H
