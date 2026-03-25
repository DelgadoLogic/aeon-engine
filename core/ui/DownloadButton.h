// AeonBrowser — DownloadButton.h
// DelgadoLogic | UI Team
//
// Toolbar download indicator button.
//  - Lives in the right side of the address bar toolbar (beside the star and three-dot)
//  - Invisible when no downloads are active/recent
//  - Shows a ⬇ arrow with a progress arc and item count badge while active
//  - Clicking it navigates to the aeon://downloads page

#pragma once
#include <windows.h>

#define IDC_DOWNLOAD_BTN 4002

namespace DownloadButton {

// Call once at startup
bool RegisterClass(HINSTANCE hInst);

// Create the owner-drawn button as a child of the toolbar
HWND Create(HWND parent, int x, int y, int size);

// Call from DownloadManager callbacks to update state
// activeCount = number of in-progress downloads
// totalPct    = overall progress 0-100 (-1 = unknown)
void Update(int activeCount, int totalPct);

// Show/hide the button (hidden when no downloads)
void SetVisible(bool visible);

// Reposition (called on toolbar resize)
void Move(int x, int y, int size);

} // namespace DownloadButton
