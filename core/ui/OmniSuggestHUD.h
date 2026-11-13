// AeonBrowser — OmniSuggestHUD.h
// DelgadoLogic | Omnibox Auto-Suggest Engine (AeonOmni)
//
// Floating Omnibox Auto-Suggest Dropdown HUD.
// Top-most, non-activating Win32 popup with double-buffered GDI+ rendering,
// DWM rounded glass corners, multi-provider search predictions, local SQLite
// history/bookmark fuzzy matching, and inline recursive-descent math evaluation.

#pragma once

#include <windows.h>
#include <string>
#include <vector>
#include <functional>
#include <cstdint>

namespace AeonOmni {

    enum class ItemType {
        MathResult,         // Inline math evaluation or unit conversion
        SearchPrediction,   // Multi-provider search suggestions (DDG, Google, Brave, etc.)
        BookmarkMatch,      // Local bookmarks.json / SQLite bookmark match
        HistoryMatch,       // Local HistoryEngine SQLite visit match
        DirectUrl,          // Directly typed URL target
        AiPrompt            // @ai or @ask prompt trigger
    };

    struct SuggestionItem {
        ItemType    type;
        std::string title;           // Display title or expression result
        std::string subtitle;        // Subtitle, URL target, or formula description
        std::string actionUrl;       // Full navigation target URL
        std::string providerTag;     // Tag badge label ("MATH", "DUCKDUCKGO", "BOOKMARK", etc.)
        COLORREF    accentColor;     // Left accent bar and icon color
        const wchar_t* iconGlyph;    // Segoe MDL2 Assets icon glyph
    };

    using NavigationCallback = std::function<void(const std::string& url)>;

    // Registers the Win32 window class "AeonOmniSuggestHUD"
    bool RegisterClass(HINSTANCE hInst);

    // Initializes the HUD singleton with parent window reference and optional navigation callback
    void Initialize(HWND parentHwnd, HINSTANCE hInst, NavigationCallback navCb = nullptr);

    // Assembles suggestions and presents the floating popup anchored below anchorRect
    void ShowSuggestions(const std::string& query, const RECT& anchorRect);

    // Displays pre-assembled suggestions directly
    void ShowSuggestions(const std::string& query, const RECT& anchorRect, const std::vector<SuggestionItem>& items);

    // Hides and collapses the HUD popup
    void Hide();

    // Returns true if the suggestion HUD is currently visible on screen
    bool IsVisible();

    // Handles keyboard navigation (VK_UP, VK_DOWN, VK_RETURN, VK_ESCAPE, VK_TAB).
    // Returns true if the key was consumed by the HUD.
    bool HandleKeyDown(WPARAM wParam);

    // Returns the navigation URL associated with the currently highlighted item (or typed query)
    std::string GetSelectedUrl();

    // Returns the index of the currently highlighted suggestion row (-1 if none selected)
    int GetSelectedIndex();

    // Updates the active default search engine name ("duckduckgo", "google", "brave", "bing", "searxng", etc.)
    void SetSearchEngine(const std::string& providerName);

    // Returns the window handle of the HUD popup
    HWND GetHwnd();

    // Cleans up cached GDI fonts, brushes, and window resources on shutdown
    void Destroy();

    // Public helper to evaluate math / unit conversions directly
    bool EvaluateMathExpression(const std::string& input, std::string& outResult, std::string& outFormatted);

    // Rescales cached HUD fonts and row geometry on PerMonitorV2 DPI change
    void OnDpiChanged(int newDpi);

} // namespace AeonOmni
