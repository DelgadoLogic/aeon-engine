// AeonBrowser — BrowserChrome.h
// DelgadoLogic | Lead UI Engineer
#pragma once
#include <windows.h>
#include <cstdint>
#include "../../core/engine/AeonEngine_Interface.h"
#include "../../core/probe/HardwareProbe.h"

namespace BrowserChrome {
    // -- Chrome Layout Metrics ------------------------------------------
    constexpr int TAB_STRIP_HEIGHT   = 36;
    constexpr int NAV_TOOLBAR_HEIGHT = 38;
    constexpr int CHROME_H           = TAB_STRIP_HEIGHT + NAV_TOOLBAR_HEIGHT; // 74px
    constexpr int CHROME_HEIGHT      = CHROME_H;
    constexpr int CAPTION_BTN_WIDTH  = 46;
    constexpr int CAPTION_TOTAL_W    = CAPTION_BTN_WIDTH * 3; // 138px

    // -- Hit-Test Identifiers -------------------------------------------
    enum HitTestCode {
        HIT_EMPTY_SPACE         = -1,
        HIT_LOGO                = 0,
        HIT_BACK                = 1,
        HIT_FORWARD             = 2,
        HIT_REFRESH             = 3,
        HIT_HOME                = 4,
        HIT_URLBAR              = 5,
        HIT_LOCK                = 6,
        HIT_MINIMIZE            = 10,
        HIT_MAXIMIZE            = 11,
        HIT_CLOSE               = 12,
        HIT_DOWNLOADS           = 20,
        HIT_BOOKMARKS           = 21,
        HIT_SHIELD              = 22,
        HIT_AI                  = 23,
        HIT_SPLITVIEW           = 24, // In-Viewport Split View Toggle [?]
        HIT_MENU                = 25,
        HIT_NEWTAB              = 50,
        HIT_TAB_BASE            = 100, // 100 + tab_index
        HIT_TAB_CLOSE_BASE      = 200, // 200 + tab_index
        HIT_GROUP_HEADER_BASE   = 300, // 300 + group_index
        HIT_GROUP_CHEVRON_BASE  = 400, // 400 + group_index
        HIT_SPLITTER            = 500, // Draggable Splitter Divider
        HIT_SPLIT_SWAP          = 501, // Quick-swap button on splitter divider
        HIT_TAB_AUDIO_BASE      = 600  // 600 + tab_index (speaker/mute icon)
    };

    constexpr int PINNED_TAB_WIDTH = 36;

    // -- Context Menu Command Identifiers --------------------------------
    #define IDM_GROUP_RENAME          41001
    #define IDM_GROUP_COLOR_PURPLE    41002
    #define IDM_GROUP_COLOR_CYAN      41003
    #define IDM_GROUP_COLOR_GREEN     41004
    #define IDM_GROUP_COLOR_AMBER     41005
    #define IDM_GROUP_COLOR_PINK      41006
    #define IDM_GROUP_COLOR_RED       41007
    #define IDM_GROUP_NEW_TAB         41008
    #define IDM_GROUP_UNGROUP         41009
    #define IDM_GROUP_CLOSE           41010

    #define IDM_TAB_NEW_GROUP         42001
    #define IDM_TAB_REMOVE_GROUP      42002
    #define IDM_TAB_DUPLICATE         42003
    #define IDM_TAB_PIN               42004
    #define IDM_TAB_MUTE              42005
    #define IDM_TAB_CLOSE_OTHERS      42006
    #define IDM_TAB_CLOSE             42007
    #define IDM_TAB_SPLITVIEW         42008
    #define IDM_TAB_SLEEP_NOW         42009
    #define IDM_TAB_ADD_TO_GROUP_BASE 43000 // 43000 + group_index

    #define IDM_SPLIT_SWAP            42010
    #define IDM_SPLIT_SOLO_ACTIVE     42011
    #define IDM_SPLIT_RESET_RATIO     42012
    #define IDM_SPLIT_NEW_TAB         42013
    #define IDM_SPLIT_CLOSE_INACTIVE  42014
    #define IDM_SPLIT_EXIT            42015
    #define IDM_SPLIT_PRESET_50_50    42016
    #define IDM_SPLIT_PRESET_60_40    42017
    #define IDM_SPLIT_PRESET_70_30    42018
    #define IDM_TAB_TOGGLE_SIDEBAR    42019
    #define IDM_SPLIT_WITH_BASE       44000 // 44000 + target_tab_index
    #define IDM_SPLIT_SET_LEFT_BASE   45000 // 45000 + target_tab_index
    #define IDM_SPLIT_SET_RIGHT_BASE  46000 // 46000 + target_tab_index

    // Call once after window creation to set up the chrome.
    void Create(HWND parent, const SystemProfile* profile, AeonEngineVTable* engine);

    // Cleans up all cached fonts, brushes, and state to prevent shutdown GDI leaks.
    void Destroy(HWND hwnd);

    // Forward WM_PAINT to draw the chrome (double-buffered).
    void OnPaint(HWND hwnd);

    // Forward WM_TIMER for hover/smooth transition timers.
    void OnTimer(HWND hwnd, WPARAM wParam);

    // Forward WM_SIZE to reposition content area and sync viewport.
    void OnSize(HWND hwnd, int w, int h);

    // Forward WM_DPICHANGED to rescale chrome fonts and layout geometry.
    void OnDpiChanged(HWND hwnd, int newDpi);

    // Synchronizes child webview viewport position (0, topH, W, H-topH or dual split).
    void SyncViewport(HWND hwnd);

    // Triggers for popups anchored to Omnibox buttons
    void TriggerBookmarkToast(HWND hwnd);
    void TriggerShieldHUD(HWND hwnd);
    void TriggerAppMenu(HWND hwnd);

    // Mouse interaction
    void OnLButtonDown(HWND hwnd, int x, int y);
    void OnLButtonUp(HWND hwnd, int x, int y);
    void OnMouseMove(HWND hwnd, int x, int y);
    void OnMouseLeave(HWND hwnd);

    // Keyboard shortcuts (Ctrl+T, Ctrl+W, Ctrl+L, Ctrl+Tab, Ctrl+Alt+S, etc.)
    bool OnKeyDown(HWND hwnd, WPARAM vk, LPARAM lParam);

    // Hit-test
    int HitTest(HWND hwnd, int x, int y);

    // WM_COMMAND from child controls (URL bar EDIT)
    void OnCommand(HWND hwnd, int id, int notifyCode, HWND ctlHwnd);

    // WM_CTLCOLOREDIT — dark-theme URL bar background.
    HBRUSH OnCtlColorEdit(HWND hwnd, HDC hdc, HWND hEdit);

    // WM_CONTEXTMENU — right-click context menu in content area or tab strip.
    void OnContextMenu(HWND hwnd, int screenX, int screenY);

    // Context menus for Tab Strip components
    void ShowTabContextMenu(HWND hwnd, int tabIndex, int screenX, int screenY);
    void ShowGroupContextMenu(HWND hwnd, uint32_t groupId, int screenX, int screenY);

    // Engine callback helpers
    void UpdateTabTitle(HWND hwnd, unsigned int tab_id, const char* title);
    void UpdateTabUrl(HWND hwnd, unsigned int tab_id, const char* url);
    void SetTabLoaded(HWND hwnd, unsigned int tab_id);
    void SetTabProgress(HWND hwnd, unsigned int tab_id, int percent);

    // -- Tab Lifecycle & Selection API --------------------------------
    bool SelectTab(HWND hwnd, int index);
    bool CloseTab(HWND hwnd, int index);
    void NextTab(HWND hwnd);
    void PrevTab(HWND hwnd);
    unsigned int GetActiveTabId(HWND hwnd);

    // -- Visual Tab Group API -----------------------------------------
    uint32_t CreateTabGroup(HWND hwnd, const char* name, COLORREF color = RGB(147, 51, 234));
    bool AddTabToGroup(HWND hwnd, int tabIndex, uint32_t groupId);
    bool RemoveTabFromGroup(HWND hwnd, int tabIndex);
    bool ToggleGroupCollapsed(HWND hwnd, uint32_t groupId);
    uint32_t GetTabGroupId(HWND hwnd, int tabIndex);

    // -- In-Viewport Split View API -----------------------------------
    void ToggleSplitView(HWND hwnd);
    bool IsSplitViewActive(HWND hwnd);
    void SetSplitView(HWND hwnd, bool active, int primaryTab = -1, int secondaryTab = -1, float ratio = 0.5f);
    void SetSplitViewActivePane(HWND hwnd, int pane);
    void SwapSplitPanes(HWND hwnd);
    void SoloSplitPane(HWND hwnd);
    void ShowSplitViewMenu(HWND hwnd, int screenX, int screenY);
    void OnContentAreaClicked(HWND hwnd, int x, int y);

    // -- Memory Saver / Tab Sleep API ---------------------------------
    void SuspendTab(HWND hwnd, int tabIndex);
    void WakeTab(HWND hwnd, int tabIndex);

    // -- Agent Control API --------------------------------------------
    int GetTabCount(HWND hwnd);
    bool GetTabInfo(HWND hwnd, int index,
                    unsigned int* outId, char* outUrl, int urlLen,
                    char* outTitle, int titleLen, bool* outActive);
    int GetActiveTabIndex(HWND hwnd);
    unsigned int CreateTab(HWND hwnd, const char* url);
    bool CloseTabById(HWND hwnd, unsigned int tabId);
    bool FocusTabById(HWND hwnd, unsigned int tabId);
    bool NavigateTab(HWND hwnd, unsigned int tabId, const char* url);
    void OpenOrFocusUrl(HWND hwnd, const char* url);

    // -- Arc Vertical Sidebar & Command Bar API -----------------------
    void ToggleSidebar(HWND hwnd);
    void SetLayoutMode(HWND hwnd, bool verticalSidebar);
    bool IsVerticalSidebarActive(HWND hwnd);
    void ToggleCommandBar(HWND hwnd);

    // -- High-DPI & DWM Caption Hit-Test Helpers ----------------------
    int  GetTabStripHeight(HWND hwnd);
    int  GetChromeHeight(HWND hwnd);
    bool IsCaptionDragPoint(HWND hwnd, int x, int y);

    // -- Page Zoom API ------------------------------------------------
    void ZoomIn(HWND hwnd);
    void ZoomOut(HWND hwnd);
    void ZoomReset(HWND hwnd);
    void SetZoom(HWND hwnd, double factor);
    double GetZoom(HWND hwnd);

    // -- Find in Page (Ctrl+F) API ------------------------------------
    void ToggleFindBar(HWND hwnd);
    void CloseFindBar(HWND hwnd);
    void FindNext(HWND hwnd, bool reverse = false);

    // -- Bookmarks Bar (Ctrl+Shift+B) API -----------------------------
    void ToggleBookmarkBar(HWND hwnd);
    bool IsBookmarkBarVisible(HWND hwnd);

    // -- Tab Audio & Muting API ---------------------------------------
    void SetTabAudioState(HWND hwnd, unsigned int tab_id, bool isPlaying, bool isMuted);
    void ToggleTabMute(HWND hwnd, int tabIndex);
}

// Agent IPC custom window message
#define WM_AEON_AGENT (WM_USER + 0x7B0)

