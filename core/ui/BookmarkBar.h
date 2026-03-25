// AeonBrowser — BookmarkBar.h
#pragma once
#include <windows.h>
#include <functional>

#define IDC_BOOKMARK_BAR 4001

namespace BookmarkBar {

using NavigateCallback = std::function<void(const wchar_t* url)>;

bool RegisterClass(HINSTANCE hInst);
HWND Create(HWND parent, int x, int y, int width, NavigateCallback cb);
void Show(bool show);
bool IsVisible();
void Refresh();            // Called after star-button saves a new bookmark
void Reposition(int x, int y, int width); // Called on main window resize

} // namespace BookmarkBar
