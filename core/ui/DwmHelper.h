// AeonBrowser — DwmHelper.h
// Delgado Creative Enterprises LLC d/b/a DelgadoLogic
//
// Dynamic PerMonitorV2 DPI & Windows 11 DWM Compositor helper.
// Resolves user32 / shcore / dwmapi entry points at runtime so Windows 11 22H2+
// gets Mica, Desktop Acrylic, crisp rounded corners, and obsidian 1px borders
// while Windows 10 builds degrade gracefully with zero loader or runtime errors.

#pragma once

#include <windows.h>
#include <dwmapi.h>

namespace AeonDwm {

// Windows 11 DWM attribute constants (guarded for older Windows SDK headers)
static constexpr DWORD AEON_DWMWA_USE_IMMERSIVE_DARK_MODE_OLD = 19;
static constexpr DWORD AEON_DWMWA_USE_IMMERSIVE_DARK_MODE     = 20;
static constexpr DWORD AEON_DWMWA_WINDOW_CORNER_PREFERENCE    = 33;
static constexpr DWORD AEON_DWMWA_BORDER_COLOR                = 34;
static constexpr DWORD AEON_DWMWA_CAPTION_COLOR               = 35;
static constexpr DWORD AEON_DWMWA_TEXT_COLOR                  = 36;
static constexpr DWORD AEON_DWMWA_SYSTEMBACKDROP_TYPE         = 38;
static constexpr DWORD AEON_DWMWA_MICA_EFFECT_LEGACY          = 1029;

// DWM_WINDOW_CORNER_PREFERENCE values
static constexpr DWORD AEON_DWMWCP_DEFAULT    = 0;
static constexpr DWORD AEON_DWMWCP_DONOTROUND = 1;
static constexpr DWORD AEON_DWMWCP_ROUND      = 2;
static constexpr DWORD AEON_DWMWCP_ROUNDSMALL = 3;

// DWM_SYSTEMBACKDROP_TYPE values
static constexpr DWORD AEON_DWMSBT_AUTO            = 0;
static constexpr DWORD AEON_DWMSBT_NONE            = 1;
static constexpr DWORD AEON_DWMSBT_MAINWINDOW      = 2; // Mica
static constexpr DWORD AEON_DWMSBT_TRANSIENTWINDOW = 3; // Desktop Acrylic (HUD popups)
static constexpr DWORD AEON_DWMSBT_TABBEDWINDOW    = 4; // Mica Alt

inline void EnablePerMonitorV2Dpi() {
    HMODULE hUser32 = GetModuleHandleW(L"user32.dll");
    if (hUser32) {
        using PFN_SetProcessDpiAwarenessContext = BOOL(WINAPI*)(HANDLE);
        auto pfnSetCtx = reinterpret_cast<PFN_SetProcessDpiAwarenessContext>(
            GetProcAddress(hUser32, "SetProcessDpiAwarenessContext"));
        if (pfnSetCtx) {
            // DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2 = ((HANDLE)-4)
            if (pfnSetCtx(reinterpret_cast<HANDLE>(static_cast<INT_PTR>(-4)))) {
                return;
            }
            // Fallback: DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE = ((HANDLE)-3)
            if (pfnSetCtx(reinterpret_cast<HANDLE>(static_cast<INT_PTR>(-3)))) {
                return;
            }
            // If ERROR_ACCESS_DENIED, awareness was already locked by manifest — safe to continue.
            if (GetLastError() == ERROR_ACCESS_DENIED) {
                return;
            }
        }
    }

    HMODULE hShcore = LoadLibraryW(L"shcore.dll");
    if (hShcore) {
        using PFN_SetProcessDpiAwareness = HRESULT(WINAPI*)(int);
        auto pfnSetAwareness = reinterpret_cast<PFN_SetProcessDpiAwareness>(
            GetProcAddress(hShcore, "SetProcessDpiAwareness"));
        if (pfnSetAwareness) {
            if (SUCCEEDED(pfnSetAwareness(2 /* PROCESS_PER_MONITOR_DPI_AWARE */))) {
                return;
            }
        }
    }

    if (hUser32) {
        using PFN_SetProcessDPIAware = BOOL(WINAPI*)();
        auto pfnLegacy = reinterpret_cast<PFN_SetProcessDPIAware>(
            GetProcAddress(hUser32, "SetProcessDPIAware"));
        if (pfnLegacy) {
            pfnLegacy();
        }
    }
}

inline UINT GetWindowDpiSafe(HWND hWnd) {
    if (hWnd) {
        HMODULE hUser32 = GetModuleHandleW(L"user32.dll");
        if (hUser32) {
            using PFN_GetDpiForWindow = UINT(WINAPI*)(HWND);
            static auto s_pfnGetDpiForWindow = reinterpret_cast<PFN_GetDpiForWindow>(
                GetProcAddress(hUser32, "GetDpiForWindow"));
            if (s_pfnGetDpiForWindow) {
                UINT dpi = s_pfnGetDpiForWindow(hWnd);
                if (dpi >= 72 && dpi <= 576) return dpi;
            }
        }
    }
    HDC hdc = GetDC(hWnd);
    UINT dpi = 96;
    if (hdc) {
        int logX = GetDeviceCaps(hdc, LOGPIXELSX);
        if (logX >= 72 && logX <= 576) dpi = static_cast<UINT>(logX);
        ReleaseDC(hWnd, hdc);
    }
    return dpi;
}

inline int ScaleByDpi(int px, UINT dpi) {
    return MulDiv(px, static_cast<int>(dpi > 0 ? dpi : 96), 96);
}

inline HRESULT SetWindowAttributeSafe(HWND hWnd, DWORD attribute, const void* pvAttribute, DWORD cbAttribute) {
    if (!hWnd || !pvAttribute || cbAttribute == 0) return E_INVALIDARG;
    static HMODULE s_hDwmapi = LoadLibraryW(L"dwmapi.dll");
    if (!s_hDwmapi) return E_NOTIMPL;

    using PFN_DwmSetWindowAttribute = HRESULT(WINAPI*)(HWND, DWORD, LPCVOID, DWORD);
    static auto s_pfnSetAttr = reinterpret_cast<PFN_DwmSetWindowAttribute>(
        GetProcAddress(s_hDwmapi, "DwmSetWindowAttribute"));
    if (!s_pfnSetAttr) return E_NOTIMPL;

    return s_pfnSetAttr(hWnd, attribute, pvAttribute, cbAttribute);
}

inline HRESULT ExtendFrameSafe(HWND hWnd, const MARGINS* pMarInset) {
    if (!hWnd || !pMarInset) return E_INVALIDARG;
    static HMODULE s_hDwmapi = LoadLibraryW(L"dwmapi.dll");
    if (!s_hDwmapi) return E_NOTIMPL;

    using PFN_DwmExtendFrameIntoClientArea = HRESULT(WINAPI*)(HWND, const MARGINS*);
    static auto s_pfnExtend = reinterpret_cast<PFN_DwmExtendFrameIntoClientArea>(
        GetProcAddress(s_hDwmapi, "DwmExtendFrameIntoClientArea"));
    if (!s_pfnExtend) return E_NOTIMPL;

    return s_pfnExtend(hWnd, pMarInset);
}

// Applies Windows 11 Obsidian dark mode, Mica backdrop, rounded corners, and crisp 1px border
// to the top-level browser host window. Safe no-op on older Windows 10 builds.
inline void ApplyObsidianMainFrameDwm(HWND hWnd) {
    if (!hWnd) return;

    // 1px top inset preserves native DWM drop shadow & Aero Snap animations without covering GDI chrome
    const MARGINS shadowMargins = { 0, 0, 1, 0 };
    ExtendFrameSafe(hWnd, &shadowMargins);

    // Immersive dark mode (Win10 20H1+ attr 20, fallback attr 19 for 1809/1903)
    const BOOL darkMode = TRUE;
    if (FAILED(SetWindowAttributeSafe(hWnd, AEON_DWMWA_USE_IMMERSIVE_DARK_MODE, &darkMode, sizeof(darkMode)))) {
        SetWindowAttributeSafe(hWnd, AEON_DWMWA_USE_IMMERSIVE_DARK_MODE_OLD, &darkMode, sizeof(darkMode));
    }

    // Crisp Win11 rounded corners (automatically squared by DWM when maximized)
    const DWORD cornerPref = AEON_DWMWCP_ROUND;
    SetWindowAttributeSafe(hWnd, AEON_DWMWA_WINDOW_CORNER_PREFERENCE, &cornerPref, sizeof(cornerPref));

    // Subtle Obsidian 1px border (#262044) & deep Obsidian caption (#070913)
    const COLORREF borderColor  = RGB(38, 32, 68);
    const COLORREF captionColor = RGB(7, 9, 19);
    const COLORREF textColor    = RGB(248, 250, 252);
    SetWindowAttributeSafe(hWnd, AEON_DWMWA_BORDER_COLOR, &borderColor, sizeof(borderColor));
    SetWindowAttributeSafe(hWnd, AEON_DWMWA_CAPTION_COLOR, &captionColor, sizeof(captionColor));
    SetWindowAttributeSafe(hWnd, AEON_DWMWA_TEXT_COLOR, &textColor, sizeof(textColor));

    // Windows 11 22H2+ Mica backdrop (DWMSBT_MAINWINDOW = 2), fallback to Win11 21H2 Mica (1029)
    const DWORD backdropMica = AEON_DWMSBT_MAINWINDOW;
    if (FAILED(SetWindowAttributeSafe(hWnd, AEON_DWMWA_SYSTEMBACKDROP_TYPE, &backdropMica, sizeof(backdropMica)))) {
        const BOOL legacyMica = TRUE;
        SetWindowAttributeSafe(hWnd, AEON_DWMWA_MICA_EFFECT_LEGACY, &legacyMica, sizeof(legacyMica));
    }
}

// Applies Windows 11 Desktop Acrylic transient backdrop, dark mode, rounded corners,
// and crisp obsidian-violet border to HUD / menu popup windows.
inline void ApplyObsidianTransientPopupDwm(HWND hWnd, bool smallCorners = false) {
    if (!hWnd) return;

    const BOOL darkMode = TRUE;
    if (FAILED(SetWindowAttributeSafe(hWnd, AEON_DWMWA_USE_IMMERSIVE_DARK_MODE, &darkMode, sizeof(darkMode)))) {
        SetWindowAttributeSafe(hWnd, AEON_DWMWA_USE_IMMERSIVE_DARK_MODE_OLD, &darkMode, sizeof(darkMode));
    }

    const DWORD cornerPref = smallCorners ? AEON_DWMWCP_ROUNDSMALL : AEON_DWMWCP_ROUND;
    SetWindowAttributeSafe(hWnd, AEON_DWMWA_WINDOW_CORNER_PREFERENCE, &cornerPref, sizeof(cornerPref));

    const COLORREF borderColor = RGB(58, 44, 98);
    SetWindowAttributeSafe(hWnd, AEON_DWMWA_BORDER_COLOR, &borderColor, sizeof(borderColor));

    const DWORD backdropAcrylic = AEON_DWMSBT_TRANSIENTWINDOW;
    SetWindowAttributeSafe(hWnd, AEON_DWMWA_SYSTEMBACKDROP_TYPE, &backdropAcrylic, sizeof(backdropAcrylic));
}

} // namespace AeonDwm
