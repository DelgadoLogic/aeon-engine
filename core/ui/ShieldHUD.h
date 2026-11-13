// AeonBrowser — ShieldHUD.h
// DelgadoLogic | UI & Privacy Team
//
// Sovereign Shield Dropdown HUD — Brave-style interactive privacy center.
// Anchored directly below the Omnibox shield icon (x_shield, y = 74).

#pragma once
#include <windows.h>
#include <functional>
#include <string>

namespace ShieldHUD {

    // Callback invoked when the user toggles protection for the active domain.
    using ShieldToggleCallback = std::function<void(bool bShieldsActive, const std::string& domain)>;
    using ShieldActionCallback = std::function<void(const std::string& action, const std::string& domain)>;

    // Register the popup window class once at startup.
    bool RegisterClass(HINSTANCE hInst);

    // Displays the Sovereign Shield HUD anchored to the Omnibox shield button.
    // anchorPt = screen coordinates of the shield icon bottom-center (x_shield, 74).
    // currentUrl = URL of the currently active tab.
    void Show(HWND parent, POINT anchorPt, const char* currentUrl,
              ShieldToggleCallback toggleCb,
              ShieldActionCallback actionCb = nullptr);

    // Hides and destroys the HUD popup programmatically.
    void Hide();

    // Returns true if the HUD is currently open.
    bool IsVisible();

    // Checks if Sovereign Shield protection is active for a given URL/domain.
    bool IsShieldActiveForUrl(const char* url);

    // Sets Sovereign Shield protection status for a given URL/domain.
    void SetShieldActiveForUrl(const char* url, bool active);

    // Rescales fonts and popup geometry on PerMonitorV2 DPI change.
    void OnDpiChanged(int newDpi);

} // namespace ShieldHUD
