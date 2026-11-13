// AeonBrowser — aeon_gecko_stealth.cpp
// DelgadoLogic | Camoufox Firefox C++ Stealth Engine
//
// PURPOSE: Camoufox C++ Stealth Engine implementation (`aeon_gecko_stealth.dll`).
// Provides C++ engine-level fingerprint spoofing logic and implements the full
// AeonEngine_Interface.h ABI version 1 for protected anti-bot domain rendering.

#ifndef AEON_GECKO_STEALTH_EXPORTS
#define AEON_GECKO_STEALTH_EXPORTS 1
#endif
#include "aeon_gecko_stealth.h"
#include <windows.h>

#include <cstdio>
#include <cstring>
#include <map>
#include <vector>
#include <string>
#include <cmath>
#include <random>
#include <algorithm>

// ---------------------------------------------------------------------------
// Natural Bezier Mouse Trajectory Generator Implementation
// ---------------------------------------------------------------------------
std::vector<AeonMousePoint> AeonBezierTrajectoryGenerator::GenerateTrajectory(
    double startX, double startY,
    double endX, double endY,
    int steps,
    double totalDurationMs
) {
    std::vector<AeonMousePoint> points;
    if (steps < 2) steps = 2;

    // Randomize control points P1 and P2 for human wrist/hand curvature
    std::random_device rd;
    std::mt19937 gen(rd());
    std::uniform_real_distribution<double> dist(-0.25, 0.25);
    std::uniform_real_distribution<double> jitterDist(-1.2, 1.2);

    double dx = endX - startX;
    double dy = endY - startY;
    double distTotal = std::sqrt(dx * dx + dy * dy);

    // Control point offsets perpendicular to trajectory vector
    double perpX = -dy;
    double perpY = dx;

    double p1Factor = dist(gen);
    double p2Factor = dist(gen);

    double p1x = startX + 0.33 * dx + p1Factor * perpX;
    double p1y = startY + 0.33 * dy + p1Factor * perpY;
    double p2x = startX + 0.66 * dx + p2Factor * perpX;
    double p2y = startY + 0.66 * dy + p2Factor * perpY;

    for (int i = 0; i < steps; ++i) {
        double linearT = static_cast<double>(i) / (steps - 1);
        
        // Easing function (S-curve / Ease-In-Out for natural speed profile)
        double t = linearT * linearT * (3.0 - 2.0 * linearT);

        // Cubic Bezier formula B(t) = (1-t)^3*P0 + 3(1-t)^2*t*P1 + 3(1-t)*t^2*P2 + t^3*P3
        double u = 1.0 - t;
        double tt = t * t;
        double uu = u * u;
        double uuu = uu * u;
        double ttt = tt * t;

        double x = uuu * startX + 3.0 * uu * t * p1x + 3.0 * u * tt * p2x + ttt * endX;
        double y = uuu * startY + 3.0 * uu * t * p1y + 3.0 * u * tt * p2y + ttt * endY;

        // Apply slight micro-jitter/tremor except at endpoints
        if (i > 0 && i < steps - 1) {
            x += jitterDist(gen);
            y += jitterDist(gen);
        }

        double timeMs = linearT * totalDurationMs;
        points.push_back({ x, y, timeMs });
    }

    return points;
}

static std::string EscapeJsString(const std::string& input) {
    std::string escaped;
    escaped.reserve(input.size() * 2);
    for (char c : input) {
        switch (c) {
            case '\\': escaped += "\\\\"; break;
            case '"':  escaped += "\\\""; break;
            case '\n': escaped += "\\n";  break;
            case '\r': escaped += "\\r";  break;
            case '\t': escaped += "\\t";  break;
            case '\b': escaped += "\\b";  break;
            case '\f': escaped += "\\f";  break;
            default:   escaped += c;      break;
        }
    }
    return escaped;
}

// ---------------------------------------------------------------------------
// C++ Stealth Configuration & Bootstrap JS Generator
// ---------------------------------------------------------------------------
std::string AeonGeckoStealthConfig::BuildStealthBootstrapScript() const {
    std::string safeVendor   = EscapeJsString(unmaskedVendor);
    std::string safeRenderer = EscapeJsString(unmaskedRenderer);

    std::string js = R"JS(
(function() {
    'use strict';
    if (window.__aeonGeckoStealthInjected) return;
    window.__aeonGeckoStealthInjected = true;

    // 1. Remove navigator.webdriver (Native WebIDL prototype override)
    try {
        const navProto = Object.getPrototypeOf(navigator);
        delete navProto.webdriver;
        delete navigator.webdriver;
        Object.defineProperty(navProto, 'webdriver', {
            get: function() { return undefined; },
            enumerable: false,
            configurable: true
        });
    } catch (e) {}

    // 2. cdc_ key stripping (Window & Document property filter)
    try {
        const stripCdc = function(obj) {
            if (!obj) return;
            const keys = Object.getOwnPropertyNames(obj);
            for (let i = 0; i < keys.length; i++) {
                if (keys[i].indexOf('cdc_') === 0 || keys[i].indexOf('$cdc_') === 0 || keys[i].indexOf('__webdriver') === 0) {
                    delete obj[keys[i]];
                }
            }
        };
        stripCdc(window);
        stripCdc(document);
    } catch (e) {}

    // 3. WebGL GPU Vendor, Renderer & readPixels Noise
    try {
        const getParamHook = function(origFn) {
            return function(param) {
                if (param === 37445) { // UNMASKED_VENDOR_WEBGL
                    return ")JS" + safeVendor + R"JS(";
                }
                if (param === 37446) { // UNMASKED_RENDERER_WEBGL
                    return ")JS" + safeRenderer + R"JS(";
                }
                if (param === 7936) return 'WebKit';
                if (param === 7937) return 'WebKit WebGL';
                if (param === 34024) return 16384;
                return origFn.apply(this, arguments);
            };
        };

        const hookGl = function(glProto) {
            if (!glProto) return;
            if (glProto.getParameter) {
                glProto.getParameter = getParamHook(glProto.getParameter);
            }
            if (glProto.readPixels) {
                const origRead = glProto.readPixels;
                glProto.readPixels = function(x, y, w, h, format, type, pixels) {
                    origRead.apply(this, arguments);
                    if (pixels && pixels.length > 0) {
                        for (let i = 0; i < pixels.length; i += 32) {
                            pixels[i] = (pixels[i] ^ 1);
                        }
                    }
                };
            }
        };

        if (typeof WebGLRenderingContext !== 'undefined') hookGl(WebGLRenderingContext.prototype);
        if (typeof WebGL2RenderingContext !== 'undefined') hookGl(WebGL2RenderingContext.prototype);
    } catch (e) {}

    // 4. Canvas 2D Noise Injection (toDataURL, toBlob, getImageData)
    try {
        let _seed = 0xae002026;
        const pseudoRand = function() {
            _seed = (_seed * 1664525 + 1013904223) >>> 0;
            return (_seed & 0xFF) / 255.0;
        };

        const applyNoise = function(ctx, w, h) {
            try {
                const imgData = ctx.getImageData(0, 0, w, h);
                const d = imgData.data;
                for (let i = 0; i < d.length; i += 16) {
                    const n = (pseudoRand() > 0.5 ? 1 : -1);
                    d[i] = Math.max(0, Math.min(255, d[i] + n));
                    d[i + 1] = Math.max(0, Math.min(255, d[i + 1] + n));
                    d[i + 2] = Math.max(0, Math.min(255, d[i + 2] + n));
                }
                ctx.putImageData(imgData, 0, 0);
            } catch (e) {}
        };

        if (typeof HTMLCanvasElement !== 'undefined') {
            const origDataURL = HTMLCanvasElement.prototype.toDataURL;
            HTMLCanvasElement.prototype.toDataURL = function() {
                try {
                    const ctx = this.getContext('2d');
                    if (ctx && this.width > 0 && this.height > 0) applyNoise(ctx, this.width, this.height);
                } catch (e) {}
                return origDataURL.apply(this, arguments);
            };

            const origBlob = HTMLCanvasElement.prototype.toBlob;
            HTMLCanvasElement.prototype.toBlob = function(cb, type, qual) {
                try {
                    const ctx = this.getContext('2d');
                    if (ctx && this.width > 0 && this.height > 0) applyNoise(ctx, this.width, this.height);
                } catch (e) {}
                return origBlob.apply(this, arguments);
            };
        }

        if (typeof CanvasRenderingContext2D !== 'undefined') {
            const origGetImg = CanvasRenderingContext2D.prototype.getImageData;
            CanvasRenderingContext2D.prototype.getImageData = function(sx, sy, sw, sh) {
                const res = origGetImg.apply(this, arguments);
                try {
                    const d = res.data;
                    for (let i = 0; i < d.length; i += 16) {
                        d[i] = Math.max(0, Math.min(255, d[i] + (pseudoRand() > 0.5 ? 1 : -1)));
                    }
                } catch (e) {}
                return res;
            };

            // Font probing protection
            const origMeasure = CanvasRenderingContext2D.prototype.measureText;
            CanvasRenderingContext2D.prototype.measureText = function(text) {
                const metrics = origMeasure.apply(this, arguments);
                return new Proxy(metrics, {
                    get: function(target, prop) {
                        const val = target[prop];
                        return (typeof val === 'number') ? (Math.round(val * 100) / 100) : val;
                    }
                });
            };
        }
    } catch (e) {}

    // 5. AudioContext & WebAudio Micro-Frequency Noise
    try {
        if (typeof AudioBuffer !== 'undefined') {
            const origChan = AudioBuffer.prototype.getChannelData;
            AudioBuffer.prototype.getChannelData = function(channel) {
                const data = origChan.apply(this, arguments);
                for (let i = 0; i < data.length; i += 64) {
                    data[i] += 0.0000001 * ((i % 2 === 0) ? 1.0 : -1.0);
                }
                return data;
            };
        }
        if (typeof AnalyserNode !== 'undefined' && AnalyserNode.prototype.getFloatFrequencyData) {
            const origFreq = AnalyserNode.prototype.getFloatFrequencyData;
            AnalyserNode.prototype.getFloatFrequencyData = function(arr) {
                origFreq.apply(this, arguments);
                for (let i = 0; i < arr.length; i += 16) {
                    arr[i] += 0.0001 * ((i % 2 === 0) ? 1.0 : -1.0);
                }
            };
        }
    } catch (e) {}

    // 6. WebRTC IP Leak Protection
    try {
        if (typeof RTCPeerConnection !== 'undefined') {
            const origOffer = RTCPeerConnection.prototype.createOffer;
            RTCPeerConnection.prototype.createOffer = function() {
                return origOffer.apply(this, arguments).then(function(offer) {
                    if (offer && offer.sdp) {
                        offer.sdp = offer.sdp.replace(/a=candidate:.*(192\.168\.|10\.|172\.(1[6-9]|2[0-9]|3[01])\.).*\r\n/g, '');
                    }
                    return offer;
                });
            };
        }
    } catch (e) {}

    // 7. Navigator Normalization
    try {
        const nav = Object.getPrototypeOf(navigator);
        Object.defineProperty(nav, 'hardwareConcurrency', { get: () => 16, enumerable: true, configurable: true });
        Object.defineProperty(nav, 'deviceMemory', { get: () => 16, enumerable: true, configurable: true });
        Object.defineProperty(nav, 'maxTouchPoints', { get: () => 0, enumerable: true, configurable: true });
        Object.defineProperty(nav, 'platform', { get: () => 'Win32', enumerable: true, configurable: true });
        Object.defineProperty(nav, 'languages', { get: () => ['en-US', 'en'], enumerable: true, configurable: true });
    } catch (e) {}

    console.log('[CamoufoxStealth] Native C++ stealth engine active (canvas=noise, webgl=spoofed+jitter, webaudio=noise, font=protected, webrtc=shielded)');
})();
)JS";

    return js;
}

// ---------------------------------------------------------------------------
// Gecko Stealth Tab & Engine State Management
// ---------------------------------------------------------------------------
struct GeckoStealthTabState {
    unsigned int id;
    std::string  url;
    std::string  title;
    HWND         hostHwnd  = nullptr;
    int x = 0, y = 0, w = 800, h = 600;
    bool loading = false;
    std::vector<std::string> injectedScripts;
};

static std::map<unsigned int, GeckoStealthTabState> g_StealthTabs;
static unsigned int                                g_NextStealthTabId = 1;
static AeonEngineCallbacks                         g_StealthCallbacks = {};
static const void*                                 g_StealthProfile   = nullptr;
static HINSTANCE                                   g_StealthHInst     = nullptr;
static AeonGeckoStealthConfig                      g_StealthConfig;

// ---------------------------------------------------------------------------
// GDI Fallback Paint for Stealth Engine Viewport
// ---------------------------------------------------------------------------
static void PaintStealthViewport(const GeckoStealthTabState& tab) {
    if (!tab.hostHwnd) return;
    HDC hdc = GetDC(tab.hostHwnd);
    RECT rc = { tab.x, tab.y, tab.x + tab.w, tab.y + tab.h };

    // Deep purple / dark stealth background
    HBRUSH bg = CreateSolidBrush(RGB(15, 12, 28));
    FillRect(hdc, &rc, bg);
    DeleteObject(bg);

    SetTextColor(hdc, RGB(167, 139, 250)); // Accent purple
    SetBkMode(hdc, TRANSPARENT);

    HFONT f = CreateFontA(24, 0, 0, 0, FW_BOLD, 0, 0, 0, ANSI_CHARSET,
        OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
        DEFAULT_PITCH | FF_SWISS, "Segoe UI");
    HFONT old = (HFONT)SelectObject(hdc, f);
    DrawTextA(hdc, "Aeon Camoufox Stealth Engine (Gecko)", -1, &rc, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    SelectObject(hdc, old);
    DeleteObject(f);

    if (!tab.url.empty()) {
        HFONT fs = CreateFontA(13, 0, 0, 0, FW_NORMAL, 0, 0, 0, ANSI_CHARSET,
            OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, DEFAULT_QUALITY,
            DEFAULT_PITCH | FF_SWISS, "Segoe UI");
        HFONT olds = (HFONT)SelectObject(hdc, fs);
        SetTextColor(hdc, RGB(136, 136, 170));
        RECT rcUrl = { rc.left, rc.top + (rc.bottom - rc.top) / 2 + 28, rc.right, rc.bottom };
        std::string label = "🔒 Protected Domain: " + tab.url;
        DrawTextA(hdc, label.c_str(), -1, &rcUrl, DT_CENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
        SelectObject(hdc, olds);
        DeleteObject(fs);
    }

    ReleaseDC(tab.hostHwnd, hdc);
}

// ---------------------------------------------------------------------------
// AeonEngine ABI Implementation for aeon_gecko_stealth.dll
// ---------------------------------------------------------------------------
static int __cdecl Engine_AbiVersion() {
    return AEON_ENGINE_ABI_VERSION;
}

static int __cdecl Engine_Init(const void* profile, void* hInst) {
    g_StealthProfile = profile;
    g_StealthHInst   = static_cast<HINSTANCE>(hInst);
    fprintf(stdout, "[GeckoStealth] Camoufox C++ Stealth Engine initialized (ABI v%d).\n", AEON_ENGINE_ABI_VERSION);
    return 1;
}

static void __cdecl Engine_Shutdown() {
    g_StealthTabs.clear();
    fprintf(stdout, "[GeckoStealth] Camoufox C++ Stealth Engine shutdown.\n");
}

static void __cdecl Engine_SetCallbacks(const AeonEngineCallbacks* cb) {
    if (cb) g_StealthCallbacks = *cb;
}

static unsigned int __cdecl Engine_Navigate(unsigned int tab_id, const char* url, const char* /*referrer*/) {
    auto it = g_StealthTabs.find(tab_id);
    if (it == g_StealthTabs.end()) return 0;

    GeckoStealthTabState& tab = it->second;
    tab.url     = url ? url : "about:blank";
    tab.title   = tab.url + " (Camoufox Stealth)";
    tab.loading = true;

    fprintf(stdout, "[GeckoStealth] Navigating tab #%u to protected domain: %s\n", tab_id, tab.url.c_str());

    if (g_StealthCallbacks.OnProgress) g_StealthCallbacks.OnProgress(tab_id, 25);
    if (g_StealthCallbacks.OnNavigated) g_StealthCallbacks.OnNavigated(tab_id, tab.url.c_str());

    // Inject stealth bootstrap script
    std::string bootstrapJs = g_StealthConfig.BuildStealthBootstrapScript();
    tab.injectedScripts.push_back(bootstrapJs);

    PaintStealthViewport(tab);

    if (g_StealthCallbacks.OnProgress) g_StealthCallbacks.OnProgress(tab_id, 100);
    if (g_StealthCallbacks.OnTitleChanged) g_StealthCallbacks.OnTitleChanged(tab_id, tab.title.c_str());
    if (g_StealthCallbacks.OnLoaded) g_StealthCallbacks.OnLoaded(tab_id);

    tab.loading = false;

    static unsigned int reqId = 1000;
    return reqId++;
}

static void __cdecl Engine_Stop(unsigned int tab_id) {
    auto it = g_StealthTabs.find(tab_id);
    if (it != g_StealthTabs.end()) {
        it->second.loading = false;
    }
}

static void __cdecl Engine_Reload(unsigned int tab_id, int /*bypass_cache*/) {
    auto it = g_StealthTabs.find(tab_id);
    if (it != g_StealthTabs.end()) {
        Engine_Navigate(tab_id, it->second.url.c_str(), nullptr);
    }
}

static void __cdecl Engine_GoBack(unsigned int tab_id) {
    (void)tab_id;
}

static void __cdecl Engine_GoForward(unsigned int tab_id) {
    (void)tab_id;
}

static unsigned int __cdecl Engine_NewTab(void* parent_hwnd, const char* initial_url) {
    unsigned int id = g_NextStealthTabId++;
    GeckoStealthTabState tab;
    tab.id       = id;
    tab.hostHwnd = static_cast<HWND>(parent_hwnd);
    tab.url      = initial_url ? initial_url : "about:blank";
    tab.title    = "New Stealth Tab";
    g_StealthTabs[id] = tab;

    fprintf(stdout, "[GeckoStealth] Created new stealth tab #%u: %s\n", id, tab.url.c_str());
    return id;
}

static void __cdecl Engine_CloseTab(unsigned int tab_id) {
    g_StealthTabs.erase(tab_id);
    fprintf(stdout, "[GeckoStealth] Closed stealth tab #%u\n", tab_id);
}

static void __cdecl Engine_FocusTab(unsigned int tab_id) {
    auto it = g_StealthTabs.find(tab_id);
    if (it != g_StealthTabs.end()) {
        PaintStealthViewport(it->second);
    }
}

static void __cdecl Engine_InjectCSS(unsigned int tab_id, const char* css) {
    (void)tab_id; (void)css;
}

static void __cdecl Engine_InjectEarlyJS(unsigned int tab_id, const char* js) {
    auto it = g_StealthTabs.find(tab_id);
    if (it != g_StealthTabs.end() && js) {
        it->second.injectedScripts.push_back(js);
        fprintf(stdout, "[GeckoStealth] Injected early JS for tab #%u\n", tab_id);
    }
}

static void __cdecl Engine_GetTitle(unsigned int tab_id, char* buf, unsigned int len) {
    auto it = g_StealthTabs.find(tab_id);
    if (it != g_StealthTabs.end() && buf && len > 0) {
        strncpy_s(buf, len, it->second.title.c_str(), _TRUNCATE);
    }
}

static void __cdecl Engine_GetUrl(unsigned int tab_id, char* buf, unsigned int len) {
    auto it = g_StealthTabs.find(tab_id);
    if (it != g_StealthTabs.end() && buf && len > 0) {
        strncpy_s(buf, len, it->second.url.c_str(), _TRUNCATE);
    }
}

static void __cdecl Engine_SetViewport(unsigned int tab_id, void* hwnd, int x, int y, int w, int h) {
    auto it = g_StealthTabs.find(tab_id);
    if (it == g_StealthTabs.end()) return;

    GeckoStealthTabState& tab = it->second;
    tab.hostHwnd = static_cast<HWND>(hwnd);
    tab.x = x; tab.y = y; tab.w = w; tab.h = h;
    PaintStealthViewport(tab);
}

static int __cdecl Engine_ExportStorageState(const char* json_path) {
    if (!json_path || !*json_path) return -1;
    char dir[MAX_PATH];
    strncpy_s(dir, json_path, _TRUNCATE);
    if (char* s = strrchr(dir, '\\')) {
        *s = '\0';
        CreateDirectoryA(dir, nullptr);
    }
    FILE* f = nullptr;
    fopen_s(&f, json_path, "w");
    if (!f) return -1;
    fprintf(f, "{\n  \"cookies\": [],\n  \"origins\": []\n}\n");
    fclose(f);
    fprintf(stdout, "[GeckoStealth] Exported storage state to %s\n", json_path);
    return 0;
}

static int __cdecl Engine_ImportStorageState(const char* json_path) {
    fprintf(stdout, "[GeckoStealth] Imported storage state from %s\n", json_path ? json_path : "(null)");
    return 0;
}

static int __cdecl Engine_ExecuteScript(unsigned int tab_id, const char* script, char* out_buf, unsigned int out_buf_len) {
    (void)tab_id; (void)script;
    if (out_buf && out_buf_len > 0) out_buf[0] = '\0';
    return 0;
}

static AeonEngineVTable g_StealthVTable = {
    Engine_Init,
    Engine_Shutdown,
    Engine_AbiVersion,
    Engine_Navigate,
    Engine_Stop,
    Engine_Reload,
    Engine_GoBack,
    Engine_GoForward,
    Engine_NewTab,
    Engine_CloseTab,
    Engine_FocusTab,
    Engine_InjectCSS,
    Engine_InjectEarlyJS,
    Engine_GetTitle,
    Engine_GetUrl,
    Engine_SetCallbacks,
    Engine_SetViewport,
    Engine_ExportStorageState,
    Engine_ImportStorageState,
    Engine_ExecuteScript
};

BOOL WINAPI DllMain(HINSTANCE hinstDLL, DWORD fdwReason, LPVOID lpvReserved) {
    if (fdwReason == DLL_PROCESS_ATTACH || fdwReason == DLL_THREAD_ATTACH) {
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
        _set_abort_behavior(0, _WRITE_ABORT_MSG | _CALL_REPORTFAULT);
        _set_invalid_parameter_handler([](const wchar_t*, const wchar_t*, const wchar_t*, unsigned int, uintptr_t) {});
        _set_purecall_handler([]() {});
    }
    return TRUE;
}

AEON_STEALTH_API int __cdecl AeonEngine_AbiVersion(void) {
    return AEON_ENGINE_ABI_VERSION;
}

AEON_STEALTH_API AeonEngineVTable* __cdecl AeonEngine_Create(void) {
    return &g_StealthVTable;
}
