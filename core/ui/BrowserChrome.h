// AeonBrowser — BrowserChrome.h
#pragma once
#include <windows.h>
#include "../../core/engine/AeonEngine_Interface.h"
#include "../../core/probe/HardwareProbe.h"

namespace BrowserChrome {
    // Call once after window creation to set up the chrome.
    void Create(HWND parent, const SystemProfile* profile, AeonEngineVTable* engine);

    // Forward WM_PAINT to draw the chrome.
    void OnPaint(HWND hwnd);

    // Forward WM_SIZE to reposition content area.
    void OnSize(HWND hwnd, int w, int h);

    // Mouse interaction
    void OnLButtonDown(HWND hwnd, int x, int y);
    void OnLButtonUp(HWND hwnd, int x, int y);
    void OnMouseMove(HWND hwnd, int x, int y);

    // Keyboard shortcuts (Ctrl+T, Ctrl+W, Ctrl+L, etc.)
    // Returns true if the key was handled, false to pass to DefWindowProc.
    bool OnKeyDown(HWND hwnd, WPARAM vk, LPARAM lParam);

    // Hit-test: returns button ID if (x,y) is over a chrome button, or -1 if
    // it's over empty nav bar area (suitable for HTCAPTION dragging).
    int HitTest(HWND hwnd, int x, int y);

    // WM_COMMAND from child controls (URL bar EDIT)
    void OnCommand(HWND hwnd, int id, int notifyCode, HWND ctlHwnd);

    // Engine callback helpers — called by AeonMain's engine callbacks
    // to push state changes from the engine into the chrome's tab list.
    void UpdateTabTitle(HWND hwnd, unsigned int tab_id, const char* title);
    void UpdateTabUrl(HWND hwnd, unsigned int tab_id, const char* url);
    void SetTabLoaded(HWND hwnd, unsigned int tab_id);

    // ── Agent Control API ────────────────────────────────────────────
    // Programmatic access for AeonAgentPipe and other automated callers.
    // These are thread-safe when called via PostMessage(WM_AEON_AGENT).

    // Returns the number of open tabs.
    int GetTabCount(HWND hwnd);

    // Fills out tab info for the tab at the given index (0-based).
    // Returns true on success.
    bool GetTabInfo(HWND hwnd, int index,
                    unsigned int* outId, char* outUrl, int urlLen,
                    char* outTitle, int titleLen, bool* outActive);

    // Returns the index of the currently active tab, or -1 if none.
    int GetActiveTabIndex(HWND hwnd);

    // Creates a new tab. Returns the engine tab_id (0 on failure).
    unsigned int CreateTab(HWND hwnd, const char* url);

    // Closes the tab with the given engine tab_id. Returns true on success.
    bool CloseTabById(HWND hwnd, unsigned int tabId);

    // Switches focus to the tab with the given engine tab_id.
    bool FocusTabById(HWND hwnd, unsigned int tabId);

    // Navigates the tab with the given engine tab_id to a URL.
    bool NavigateTab(HWND hwnd, unsigned int tabId, const char* url);
}

// Agent IPC custom window message — dispatched by AeonAgentPipe
#define WM_AEON_AGENT (WM_USER + 0x7AF)

