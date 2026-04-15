/* AeonBrowser — aeon16.c
 * DelgadoLogic | Senior Engineer (Open Watcom 16-bit)
 *
 * PURPOSE: Main entry point for the 16-bit Windows 3.x / Win9x retro tier.
 * Compiled with Open Watcom 2.0: wcl -bd -l=win -zW -ml aeon16.c html4.c bearssl_bridge.c
 *
 * ARCHITECTURE: This is a self-contained 16-bit Windows application.
 * It does NOT use the Rust router or the C++ core — those require 32-bit.
 * Instead, it directly calls:
 *   - WinSock (WINSOCK.DLL — 16-bit, ships with Windows 3.11 TCP/IP)
 *   - BearSSL (statically linked — MIT licensed TLS 1.2 engine)
 *   - html4.c — our HTML4/CSS2 rendering engine
 *
 * LIMITATIONS (DOCUMENTED — NOT BUGS):
 *   - No JavaScript. Win3.x has no 16-bit JS engine viable for modern web.
 *     Static pages, Gemini, and Gopher work. HTTPS works via BearSSL.
 *   - 640x480 minimum resolution assumed.
 *   - Max segment size: 64KB (16-bit near pointer limitation).
 *     Pages >1MB are chunked and rendered in segments.
 *   - No tabs — single-window document model (like Netscape 1.0).
 *
 * IT TROUBLESHOOTING:
 *   - "WINSOCK.DLL not found": Install Trumpet Winsock or Microsoft TCP/IP-32.
 *   - TLS handshake fails: Server may require TLS 1.3 (use modern tier).
 *   - App shows only grey window: WM_PAINT not reaching html4_render().
 *     Check DefWindowProc() is in the else branch of WndProc.
 */

/* Open Watcom: force 16-bit large model */
#include <windows.h>
#include <commdlg.h>
#include "html4.h"
#include "bearssl_bridge.h"

/* Address bar buffer */
static char g_CurrentUrl[512] = "about:blank";
static HWND g_hAddrBar = NULL;
static HWND g_hStatus  = NULL;
static HWND g_hMain    = NULL;

/* Edit control subclass — intercept Enter key */
static WNDPROC g_OldEditProc = NULL;
LRESULT CALLBACK AddrBarSubProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    if (msg == WM_KEYDOWN && wParam == VK_RETURN) {
        char url[512] = {0};
        GetWindowText(hwnd, url, sizeof(url));
        DoNavigate(url);
        return 0;
    }
    return CallWindowProc(g_OldEditProc, hwnd, msg, wParam, lParam);
}

/* HTML4 document state — our renderer fills these */
static Html4_Document g_Doc;

/* -------------------------------------------------------------------------
 * Navigation: fetch URL over HTTP/HTTPS and render the response
 * ---------------------------------------------------------------------- */
static void DoNavigate(const char* url) {
    static char buf[4096]; /* static to avoid 16-bit stack overflow (8KB limit) */
    int  len = 0;

    /* Update status bar */
    SetWindowText(g_hStatus, "Connecting...");

    if (strncmp(url, "https://", 8) == 0) {
        /* HTTPS via BearSSL (statically linked) */
        len = tls_get(url, buf, sizeof(buf));
    } else if (strncmp(url, "http://", 7) == 0) {
        /* Plain HTTP via WinSock (implemented in bearssl_bridge.c) */
        len = http_get(url, buf, sizeof(buf));
    } else if (strncmp(url, "gemini://", 9) == 0) {
        /* Gemini via BearSSL (Gemini uses TLS 1.2+) */
        len = gemini_get(url, buf, sizeof(buf));
    } else {
        /* about:blank or unknown */
        strncpy(buf, "<html><body><h1>Aeon Browser</h1>"
                     "<p>by DelgadoLogic</p></body></html>",
                sizeof(buf));
        len = (int)strlen(buf);
    }

    if (len <= 0) {
        SetWindowText(g_hStatus, "Error: could not connect.");
        return;
    }

    /* Parse HTML4 into our document model */
    html4_parse(&g_Doc, buf, len);

    /* Force repaint */
    InvalidateRect(g_hMain, NULL, TRUE);
    SetWindowText(g_hStatus, "Done");
    strncpy(g_CurrentUrl, url, sizeof(g_CurrentUrl) - 1);
    g_CurrentUrl[sizeof(g_CurrentUrl) - 1] = '\0';
    SetWindowText(g_hAddrBar, g_CurrentUrl);
}

/* -------------------------------------------------------------------------
 * Window Procedure
 * ---------------------------------------------------------------------- */
LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {

    case WM_CREATE: {
        /* Address bar (edit control) */
        RECT rcClient;
        GetClientRect(hwnd, &rcClient);
        g_hAddrBar = CreateWindow("EDIT", "about:blank",
            WS_CHILD | WS_VISIBLE | WS_BORDER | ES_AUTOHSCROLL,
            60, 4, (int)(rcClient.right - 140), 22,
            hwnd, (HMENU)1, ((LPCREATESTRUCT)lParam)->hInstance, NULL);
        SetWindowText(g_hAddrBar, "about:blank");

        /* Subclass address bar to intercept Enter key */
        g_OldEditProc = (WNDPROC)SetWindowLong(g_hAddrBar, GWL_WNDPROC,
                            (LONG)AddrBarSubProc);

        /* Status bar (static text at bottom) */
        g_hStatus = CreateWindow("STATIC", "Ready",
            WS_CHILD | WS_VISIBLE | SS_LEFT,
            0, 0, 400, 18,
            hwnd, (HMENU)2, ((LPCREATESTRUCT)lParam)->hInstance, NULL);

        g_hMain = hwnd;

        /* Initialise BearSSL TLS bridge */
        tls_init();

        html4_init(&g_Doc);
        DoNavigate("about:blank");
        return 0;
    }

    case WM_COMMAND:
        /* Go button (ID=3) */
        if (LOWORD(wParam) == 3)  {
            char url[512] = {0};
            GetWindowText(g_hAddrBar, url, sizeof(url));
            DoNavigate(url);
        }
        return 0;

    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC hdc;
        RECT rc;
        hdc = BeginPaint(hwnd, &ps);
        GetClientRect(hwnd, &rc);
        rc.top    += 32; /* below toolbar */
        rc.bottom -= 20; /* above status bar */
        html4_render(&g_Doc, hdc, &rc);
        EndPaint(hwnd, &ps);
        return 0;
    }

    case WM_SIZE:
        /* Resize address bar and status on window resize */
        MoveWindow(g_hAddrBar, 60, 4, LOWORD(lParam) - 140, 22, TRUE);
        MoveWindow(g_hStatus,   0, HIWORD(lParam) - 18,
                   LOWORD(lParam), 18, TRUE);
        InvalidateRect(hwnd, NULL, TRUE);
        return 0;

    case WM_DESTROY:
        html4_free(&g_Doc);
        tls_cleanup();
        PostQuitMessage(0);
        return 0;

    default:
        return DefWindowProc(hwnd, msg, wParam, lParam);
    }
}

/* -------------------------------------------------------------------------
 * WinMain
 * ---------------------------------------------------------------------- */
int PASCAL WinMain(HINSTANCE hInst, HINSTANCE hPrev,
                   LPSTR lpCmd, int nCmdShow) {

    static const char CLASS[] = "AeonBrowser16";
    WNDCLASS wc;
    HWND hwnd;
    MSG msg;

    if (!hPrev) {
        memset(&wc, 0, sizeof(wc));
        wc.style         = CS_HREDRAW | CS_VREDRAW;
        wc.lpfnWndProc   = WndProc;
        wc.hInstance     = hInst;
        wc.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
        wc.lpszClassName = CLASS;
        wc.hCursor       = LoadCursor(NULL, IDC_ARROW);
        RegisterClass(&wc);
    }

    hwnd = CreateWindow(CLASS,
        "Aeon Browser by DelgadoLogic (Windows 3.x Edition)",
        WS_OVERLAPPEDWINDOW,
        CW_USEDEFAULT, CW_USEDEFAULT, 640, 480,
        NULL, NULL, hInst, NULL);

    ShowWindow(hwnd, nCmdShow);
    UpdateWindow(hwnd);

    while (GetMessage(&msg, NULL, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessage(&msg);
    }
    return msg.wParam;
}
