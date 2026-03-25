// AeonBrowser — BookmarkToast.cpp
// DelgadoLogic | UI Team
//
// Star-button bookmark popup — exactly like Chrome's "Bookmark added" panel.
//
// Triggered by:
//   1. User clicks the ★ star button in the address bar toolbar
//   2. User presses Ctrl+D
//
// Behaviour:
//   - If page is NOT bookmarked: shows "Bookmark added" panel with editable
//     Name and Folder fields. "Done" saves, "Remove" deletes if it had just
//     been added automatically.
//   - If page IS bookmarked: shows same panel with "Edit" title and "Remove"
//     button present.
//   - Panel auto-closes after 5s if no interaction.
//   - Folder dropdown lists existing bookmark folders + "New folder" option.
//   - Saves via HistoryEngine::AddBookmark().

#include "BookmarkToast.h"
#include "../../history/HistoryEngine.h"
#include <windowsx.h>
#include <dwmapi.h>
#include <cstring>
#include <cstdio>

#pragma comment(lib, "dwmapi.lib")

namespace BookmarkToast {

// ─── Dimensions ──────────────────────────────────────────────────────────────
static const int W        = 300;
static const int H_FULL   = 180;  // Name + Folder + buttons
static const int RADIUS   = 12;
static const int PAD      = 14;

// ─── Colors ──────────────────────────────────────────────────────────────────
static const COLORREF C_BG      = RGB(0x16,0x18,0x2a);
static const COLORREF C_BORDER  = RGB(0x2a,0x28,0x55);
static const COLORREF C_TEXT    = RGB(0xe8,0xe8,0xf0);
static const COLORREF C_DIM     = RGB(0x88,0x88,0xaa);
static const COLORREF C_ACCENT  = RGB(0x6c,0x63,0xff);
static const COLORREF C_FIELD   = RGB(0x1e,0x21,0x40);
static const COLORREF C_RED     = RGB(0xef,0x44,0x44);

// ─── State ───────────────────────────────────────────────────────────────────
static HWND    g_hwnd       = nullptr;
static HFONT   g_fontMain   = nullptr;
static HFONT   g_fontSmall  = nullptr;
static HFONT   g_fontBold   = nullptr;
static HINSTANCE g_hInst    = nullptr;

static wchar_t g_title[512]  = {}; // Page title (editable)
static wchar_t g_folder[128] = {}; // Folder name
static char    g_url[2048]   = {}; // URL (not editable)
static bool    g_isEdit      = false; // true = already bookmarked
static UINT_PTR g_closeTimer = 0;

static DoneCallback  g_doneCb;
static RemoveCallback g_removeCb;

// Edit control IDs
static const int IDC_NAME_EDIT   = 5001;
static const int IDC_FOLDER_CB   = 5002;
static const int IDC_DONE_BTN    = 5003;
static const int IDC_REMOVE_BTN  = 5004;

static HWND   g_nameEdit   = nullptr;
static HWND   g_folderCombo= nullptr;
static HWND   g_doneBtn    = nullptr;
static HWND   g_removeBtn  = nullptr;

// ─── Create child controls ───────────────────────────────────────────────────
static void CreateControls(HWND hWnd) {
    // Name label
    // (drawn in WM_PAINT — not a HWND label to keep styling consistent)

    // Name edit box
    g_nameEdit = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", g_title,
        WS_CHILD|WS_VISIBLE|ES_AUTOHSCROLL,
        PAD, 56, W - PAD*2, 26,
        hWnd, (HMENU)IDC_NAME_EDIT, g_hInst, nullptr);

    // Set dark-ish styling on edit via subclass or at minimum set font
    SendMessageW(g_nameEdit, WM_SETFONT, (WPARAM)g_fontMain, TRUE);

    // Folder label + combo
    g_folderCombo = CreateWindowExW(0, L"COMBOBOX", nullptr,
        WS_CHILD|WS_VISIBLE|CBS_DROPDOWNLIST|WS_VSCROLL,
        PAD, 110, W - PAD*2, 200,
        hWnd, (HMENU)IDC_FOLDER_CB, g_hInst, nullptr);
    SendMessageW(g_folderCombo, WM_SETFONT, (WPARAM)g_fontMain, TRUE);

    // Populate folders from HistoryEngine
    auto bms = HistoryEngine::GetBookmarks();
    std::vector<std::string> seen;
    SendMessageW(g_folderCombo, CB_ADDSTRING, 0, (LPARAM)L"Bookmarks");
    for (auto& b : bms) {
        if (b.folder[0] && strcmp(b.folder, "Bookmarks") != 0) {
            std::string f = b.folder;
            if (std::find(seen.begin(), seen.end(), f) == seen.end()) {
                seen.push_back(f);
                wchar_t wf[128] = {};
                MultiByteToWideChar(CP_UTF8,0,b.folder,-1,wf,127);
                SendMessageW(g_folderCombo, CB_ADDSTRING, 0, (LPARAM)wf);
            }
        }
    }
    SendMessageW(g_folderCombo, CB_ADDSTRING, 0, (LPARAM)L"+ New folder");

    // Select current folder
    int sel = (int)SendMessageW(g_folderCombo, CB_FINDSTRINGEXACT, -1, (LPARAM)g_folder);
    SendMessageW(g_folderCombo, CB_SETCURSEL, sel >= 0 ? sel : 0, 0);

    // Done button
    g_doneBtn = CreateWindowExW(0, L"BUTTON",
        g_isEdit ? L"Save" : L"Done",
        WS_CHILD|WS_VISIBLE|BS_FLAT|BS_PUSHBUTTON,
        W - PAD - 70, H_FULL - 38, 70, 26,
        hWnd, (HMENU)IDC_DONE_BTN, g_hInst, nullptr);
    SendMessageW(g_doneBtn, WM_SETFONT, (WPARAM)g_fontMain, TRUE);

    // Remove button
    g_removeBtn = CreateWindowExW(0, L"BUTTON", L"Remove",
        WS_CHILD|WS_VISIBLE|BS_FLAT|BS_PUSHBUTTON,
        PAD, H_FULL - 38, 70, 26,
        hWnd, (HMENU)IDC_REMOVE_BTN, g_hInst, nullptr);
    SendMessageW(g_removeBtn, WM_SETFONT, (WPARAM)g_fontMain, TRUE);
}

// ─── Paint (background + labels) ─────────────────────────────────────────────
static void Paint(HDC hdc, RECT rc) {
    HBRUSH bg = CreateSolidBrush(C_BG);
    FillRect(hdc, &rc, bg);
    DeleteObject(bg);

    // Border
    HPEN pen = CreatePen(PS_SOLID,1,C_BORDER);
    HPEN op  = (HPEN)SelectObject(hdc, pen);
    HBRUSH nb = (HBRUSH)GetStockObject(NULL_BRUSH);
    HBRUSH ob = (HBRUSH)SelectObject(hdc, nb);
    RoundRect(hdc,0,0,rc.right,rc.bottom,RADIUS*2,RADIUS*2);
    SelectObject(hdc,op); SelectObject(hdc,ob); DeleteObject(pen);

    SetBkMode(hdc, TRANSPARENT);

    // Title row — star icon + heading
    SelectObject(hdc, g_fontBold);
    SetTextColor(hdc, C_TEXT);
    RECT hr = { PAD, 14, rc.right - PAD, 36 };
    DrawTextW(hdc, g_isEdit ? L"\u2605  Edit bookmark" : L"\u2605  Bookmark added",
        -1, &hr, DT_LEFT|DT_VCENTER|DT_SINGLELINE);

    // "Name" sub-label
    SelectObject(hdc, g_fontSmall);
    SetTextColor(hdc, C_DIM);
    RECT nl = { PAD, 42, rc.right-PAD, 58 };
    DrawTextW(hdc, L"Name", -1, &nl, DT_LEFT|DT_VCENTER|DT_SINGLELINE);

    // "Folder" sub-label
    RECT fl = { PAD, 96, rc.right-PAD, 112 };
    DrawTextW(hdc, L"Folder", -1, &fl, DT_LEFT|DT_VCENTER|DT_SINGLELINE);
}

// ─── Window Procedure ────────────────────────────────────────────────────────
static LRESULT CALLBACK WndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
    case WM_CREATE:
        g_fontMain  = CreateFontW(-13,0,0,0,FW_NORMAL,0,0,0,DEFAULT_CHARSET,
            OUT_DEFAULT_PRECIS,CLIP_DEFAULT_PRECIS,CLEARTYPE_QUALITY,DEFAULT_PITCH,L"Segoe UI");
        g_fontSmall = CreateFontW(-11,0,0,0,FW_NORMAL,0,0,0,DEFAULT_CHARSET,
            OUT_DEFAULT_PRECIS,CLIP_DEFAULT_PRECIS,CLEARTYPE_QUALITY,DEFAULT_PITCH,L"Segoe UI");
        g_fontBold  = CreateFontW(-13,0,0,0,FW_SEMIBOLD,0,0,0,DEFAULT_CHARSET,
            OUT_DEFAULT_PRECIS,CLIP_DEFAULT_PRECIS,CLEARTYPE_QUALITY,DEFAULT_PITCH,L"Segoe UI");
        CreateControls(hWnd);
        // Auto-close timer
        g_closeTimer = SetTimer(hWnd, 1, 5000, nullptr);
        return 0;

    case WM_TIMER:
        if (wParam == 1) { Hide(); return 0; } break;

    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC hdc = BeginPaint(hWnd,&ps);
        RECT rc; GetClientRect(hWnd,&rc);
        HDC mem = CreateCompatibleDC(hdc);
        HBITMAP bmp = CreateCompatibleBitmap(hdc,rc.right,rc.bottom);
        HBITMAP ob  = (HBITMAP)SelectObject(mem,bmp);
        Paint(mem,rc);
        BitBlt(hdc,0,0,rc.right,rc.bottom,mem,0,0,SRCCOPY);
        SelectObject(mem,ob); DeleteObject(bmp); DeleteDC(mem);
        EndPaint(hWnd,&ps);
        return 0;
    }

    case WM_COMMAND:
        switch (LOWORD(wParam)) {
        case IDC_DONE_BTN: {
            // Read name + folder
            wchar_t wname[512]={}, wfolder[128]={};
            GetWindowTextW(g_nameEdit,   wname,   511);
            int fsel = (int)SendMessageW(g_folderCombo, CB_GETCURSEL, 0, 0);
            if (fsel >= 0) SendMessageW(g_folderCombo, CB_GETLBTEXT, fsel, (LPARAM)wfolder);
            else wcscpy_s(wfolder, L"Bookmarks");

            // Convert to char
            char cname[512]={}, cfolder[128]={};
            WideCharToMultiByte(CP_UTF8,0,wname,-1,cname,511,nullptr,nullptr);
            WideCharToMultiByte(CP_UTF8,0,wfolder,-1,cfolder,127,nullptr,nullptr);

            HistoryEngine::AddBookmark(g_url, cname, cfolder);
            if (g_doneCb) g_doneCb(g_url, cname, cfolder);
            Hide();
            return 0;
        }
        case IDC_REMOVE_BTN:
            HistoryEngine::DeleteBookmark(g_url);
            if (g_removeCb) g_removeCb(g_url);
            Hide();
            return 0;
        }
        break;

    case WM_KILLFOCUS:
    case WM_ACTIVATE:
        if (LOWORD(wParam) == WA_INACTIVE) Hide();
        break;

    case WM_DESTROY:
        if (g_closeTimer) { KillTimer(hWnd,1); g_closeTimer=0; }
        if (g_fontMain)  { DeleteObject(g_fontMain);  g_fontMain  = nullptr; }
        if (g_fontSmall) { DeleteObject(g_fontSmall); g_fontSmall = nullptr; }
        if (g_fontBold)  { DeleteObject(g_fontBold);  g_fontBold  = nullptr; }
        g_hwnd = nullptr;
        return 0;
    }
    return DefWindowProcW(hWnd, msg, wParam, lParam);
}

// ─── Public API ──────────────────────────────────────────────────────────────
bool RegisterClass(HINSTANCE hInst) {
    g_hInst = hInst;
    WNDCLASSEXW wc = {};
    wc.cbSize      = sizeof(wc);
    wc.style       = CS_DROPSHADOW;
    wc.lpfnWndProc = WndProc;
    wc.hInstance   = hInst;
    wc.hCursor     = LoadCursor(nullptr, IDC_ARROW);
    wc.lpszClassName = L"AeonBookmarkToast";
    return RegisterClassExW(&wc) != 0;
}

void Show(HWND parent, POINT anchor, const char* url, const wchar_t* title,
          bool isEdit, DoneCallback doneCb, RemoveCallback removeCb) {
    if (g_hwnd) Hide();

    strncpy_s(g_url,     url,   sizeof(g_url)-1);
    wcsncpy_s(g_title,   title, 511);
    wcscpy_s (g_folder,  L"Bookmarks");
    g_isEdit   = isEdit;
    g_doneCb   = doneCb;
    g_removeCb = removeCb;

    int x = anchor.x - W;
    int y = anchor.y;
    if (x < 4) x = 4;

    g_hwnd = CreateWindowExW(
        WS_EX_TOPMOST,
        L"AeonBookmarkToast", L"",
        WS_POPUP | WS_CLIPCHILDREN,
        x, y, W, H_FULL,
        parent, nullptr, g_hInst, nullptr);

    if (!g_hwnd) return;

    // Win11 rounded corners
    DWORD corner = 2;
    DwmSetWindowAttribute(g_hwnd, 33 /* DWMWA_WINDOW_CORNER_PREFERENCE */, &corner, sizeof(corner));
    BOOL dark = TRUE;
    DwmSetWindowAttribute(g_hwnd, 20, &dark, sizeof(dark));

    ShowWindow(g_hwnd, SW_SHOW);
    SetFocus(g_nameEdit); // Auto-focus name field
}

void Hide() {
    if (g_hwnd) { DestroyWindow(g_hwnd); g_hwnd = nullptr; }
}

bool IsVisible() { return g_hwnd != nullptr; }

} // namespace BookmarkToast
