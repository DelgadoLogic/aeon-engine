// AeonBrowser — BookmarkToast.h
#pragma once
#include <windows.h>
#include <functional>

namespace BookmarkToast {
    using DoneCallback   = std::function<void(const char* url, const char* name, const char* folder)>;
    using RemoveCallback = std::function<void(const char* url)>;

    // Register the popup window class once at startup.
    bool RegisterClass(HINSTANCE hInst);

    // Show the "Bookmark added / Edit bookmark" panel.
    // anchor = bottom-left of the star button in screen coords.
    void Show(HWND parent, POINT anchor, const char* url, const wchar_t* title,
              bool isEdit, DoneCallback doneCb, RemoveCallback removeCb);

    void Hide();
    bool IsVisible();
} // namespace BookmarkToast
