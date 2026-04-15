// AeonBrowser — AeonAgentPipe.h
// DelgadoLogic | Agent Control Architecture
//
// PURPOSE: Named Pipe IPC server for agent control of the browser shell.
// Agents connect to \\.\pipe\aeon-agent and send newline-delimited JSON
// commands to create/close/navigate tabs, resize windows, query state, etc.
//
// THREAD SAFETY: The pipe listener runs on a background thread. All shell
// mutations are dispatched to the main UI thread via PostMessage(WM_AEON_AGENT).
// Read-only queries (tab.list, tab.active, etc.) are handled via
// SendMessage() to guarantee synchronous response on the pipe thread.
//
// SECURITY: LOCAL_ONLY — only the current user on localhost can connect.

#pragma once
#include <windows.h>

namespace AeonAgentPipe {

    // Start the pipe server. Call after g_MainHwnd is set.
    // Returns true if the listener thread was launched.
    bool Start(HWND mainHwnd);

    // Stop the pipe server. Disconnects all clients, closes the pipe.
    void Stop();

    // Returns true if the pipe server is running.
    bool IsRunning();

    // Process a pipe command on the UI thread. Called from WndProc
    // when WM_AEON_AGENT is received.
    // wParam = pointer to the command string (heap-allocated, callee frees)
    // lParam = pipe handle to write the response to
    void HandleCommand(WPARAM wParam, LPARAM lParam);

} // namespace AeonAgentPipe
