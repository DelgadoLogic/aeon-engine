// AeonBrowser — AppMenu.h
// DelgadoLogic | UI Team
//
// Chrome-style three-dot application menu.
// Renders as a native Win32 layered popup window (not a Win32 HMENU).
// Using a custom-drawn window gives us:
//   - Rounded corners (Win11 DWM or manual GDI on older)
//   - Icons, submenu arrows, zoom slider
//   - Aeon-specific items (Firewall Mode, Network Sentinel)
//   - Keyboard navigation + shortcut labels

#pragma once
#include <windows.h>
#include <functional>
#include <string>
#include <vector>

namespace AppMenu {

// ─── Menu item descriptor ───────────────────────────────────────────────────
enum class ItemType {
    Action,       // Normal clickable item
    Submenu,      // Has ▶ arrow, opens flyout
    Separator,    // Thin line
    Section,      // Bold label (non-clickable header)
    ZoomRow,      // Special zoom control row
    ProfileRow,   // User profile (avatar + name + signed-in badge)
};

enum class ItemId {
    NewTab, NewWindow, NewIncognito,
    Profile,
    Passwords, History, Downloads, Bookmarks, Extensions, DeleteData,
    Zoom,
    FirewallMode, NetworkSentinel,
    Print, SavePageAs, FindInPage,
    MoreTools,
    DevTools,
    Settings, Help, About, Exit,
};

struct MenuItem {
    ItemType            type;
    ItemId              id;
    std::wstring        label;
    std::wstring        shortcut;   // e.g. L"Ctrl+T"
    std::wstring        emoji;      // optional prefix emoji/icon char
    bool                danger;     // red tint (Delete browsing data)
    bool                highlight;  // violet tint (Firewall Mode, Sentinel)
    bool                checked;    // checkmark
};

using MenuCallback = std::function<void(ItemId)>;

// ─── Public API ──────────────────────────────────────────────────────────────

/// Register the popup window class. Call once at startup.
bool RegisterClass(HINSTANCE hInst);

/// Show the menu anchored below a given point (e.g. top-right of browser chrome).
/// callback is invoked when the user clicks an item.
void Show(HWND parent, POINT anchorPt, MenuCallback callback);

/// Hide the menu programmatically.
void Hide();

/// Update the Firewall Mode item label at runtime (called by NetworkSentinel).
void SetFirewallModeLabel(const wchar_t* label, bool active);

/// Update the zoom percentage shown in the zoom row.
void SetZoom(int pct); // e.g. 100 for 100%

/// Returns true if the menu popup is currently visible.
bool IsVisible();

} // namespace AppMenu
