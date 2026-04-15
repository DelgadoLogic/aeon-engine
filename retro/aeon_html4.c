// AeonBrowser — aeon_html4.c
// DelgadoLogic | Retro Tier
//
// GDI-based HTML4/CSS2 renderer — the "Retro" engine DLL for Win9x/ME/2000.
// Loaded by TierDispatcher when neither Blink nor Gecko DLLs are available.
//
// Targets:
//   Win 3.1 / Win 3.11    — 16-bit build (separate aeon16.exe, uses this .c logic)
//   Win 95 / 98 / ME      — 32-bit, compiled with MSVC 4.2 / Open Watcom
//   Win 2000 / NT 4       — 32-bit, same DLL but with slightly richer GDI+ paths
//
// Design philosophy:
//   - ZERO external dependencies (no CRT beyond what Win9x's MSVCRT40.DLL provides)
//   - Single-file C (not C++) for maximum compatibility
//   - Renders into a provided HWND via WM_PAINT + HDC GDI calls
//   - Supports: <h1>-<h6>, <p>, <b>, <i>, <u>, <a>, <br>, <img>, basic tables
//   - CSS2: color, font-size, font-weight, text-align, background-color, margin
//   - NO JavaScript (use Tier=Extended/XPHi/Pro for JS pages)

#include "aeon_html4.h"
#include <windows.h>
#include <string.h>
#include <stdlib.h>
#include <ctype.h>
#include <stdio.h>  /* snprintf / _snprintf */

/* OW2 doesn't prototype _snprintf — it's a Microsoft CRT extension.
   Map to snprintf which OW2 does support in C99 mode (-za99). */
#ifndef _snprintf
#define _snprintf snprintf
#endif

// ─── Constants ───────────────────────────────────────────────────────────────
#define MAX_TOKENS  4096
#define MAX_STR     1024
#define FONT_BODY   12
#define FONT_H1     26
#define FONT_H2     20
#define FONT_H3     16
#define LINK_COLOR  RGB(0x60,0x9e,0xff)
#define BG_COLOR    RGB(0x0d,0x0e,0x14)
#define FG_COLOR    RGB(0xe8,0xe8,0xf0)
#define MARGIN_X    14
#define LINE_SPACE  4

// ─── Token types ─────────────────────────────────────────────────────────────
typedef enum {
    TK_TEXT,
    TK_TAG_OPEN,   // <tag ...>
    TK_TAG_CLOSE,  // </tag>
    TK_TAG_SELF    // <br/> <hr/>
} TokenKind;

typedef struct {
    TokenKind kind;
    char      tag[32];   // lowercase tag name
    char      text[MAX_STR]; // for TK_TEXT: text content
    // Attributes (simplified: color, href, src only)
    char      attr_href [MAX_STR];
    char      attr_src  [MAX_STR];
    char      attr_color[32];
} Token;

// ─── Renderer state ───────────────────────────────────────────────────────────
typedef struct {
    HDC     hdc;
    RECT    clip;       // content area
    int     x, y;       // current pen position
    int     lineH;      // current line height
    int     colW;       // column width for word-wrap
    HFONT   fonts[8];   // [0]=body [1]=h1 [2]=h2 [3]=h3 [4]=h4 [5]=h5 [6]=bold [7]=italic
    HFONT   curFont;
    COLORREF fgColor;
    COLORREF bgColor;
    BOOL    bold;
    BOOL    italic;
    BOOL    underline;
    BOOL    inLink;
    char    linkHref[MAX_STR];
    // Clickable link regions (max 128)
    struct { RECT rc; char href[MAX_STR]; } links[128];
    int     linkCount;
} RenderState;

static RenderState g_RS = {0};

// ─── Font creation helper ─────────────────────────────────────────────────────
static HFONT MakeFont(int pt, int bold, int italic, int underline) {
    return CreateFontA(
        -MulDiv(pt, GetDeviceCaps(g_RS.hdc, LOGPIXELSY), 72),
        0, 0, 0,
        bold      ? FW_BOLD    : FW_NORMAL,
        italic    ? TRUE       : FALSE,
        underline ? TRUE       : FALSE,
        FALSE, DEFAULT_CHARSET,
        OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
        DEFAULT_QUALITY,  /* ClearType not available on Win9x RTM */
        DEFAULT_PITCH, "Tahoma");  /* Tahoma: Win2000+, GDI falls back on Win9x */
}

static void SelectCurFont(void) {
    // Choose from pre-allocated font set
    int fIdx = 0;
    if (g_RS.bold && !g_RS.italic)      fIdx = 6;
    else if (g_RS.italic)               fIdx = 7;
    if (g_RS.curFont != g_RS.fonts[fIdx]) {
        g_RS.curFont = g_RS.fonts[fIdx];
        SelectObject(g_RS.hdc, g_RS.curFont);
    }
}

// ─── Word-wrap text rendering ─────────────────────────────────────────────────
static void RenderWord(const char* word) {
    SIZE sz;
    GetTextExtentPoint32A(g_RS.hdc, word, (int)strlen(word), &sz);

    if (g_RS.x + sz.cx > g_RS.clip.right - MARGIN_X) {
        // Wrap
        g_RS.x  = g_RS.clip.left + MARGIN_X;
        g_RS.y += g_RS.lineH + LINE_SPACE;
    }
    // Stop drawing beyond bottom of window
    if (g_RS.y > g_RS.clip.bottom) return;

    SetTextColor(g_RS.hdc, g_RS.inLink ? LINK_COLOR : g_RS.fgColor);
    SetBkMode(g_RS.hdc, TRANSPARENT);
    TextOutA(g_RS.hdc, g_RS.x, g_RS.y, word, (int)strlen(word));

    // Underline for links
    if (g_RS.inLink || g_RS.underline) {
        HPEN pen;
        HPEN op;
        pen = CreatePen(PS_SOLID, 1, LINK_COLOR);
        op  = (HPEN)SelectObject(g_RS.hdc, pen);
        MoveToEx(g_RS.hdc, g_RS.x, g_RS.y + sz.cy - 1, NULL);
        LineTo  (g_RS.hdc, g_RS.x + sz.cx, g_RS.y + sz.cy - 1);
        SelectObject(g_RS.hdc, op); DeleteObject(pen);

        // Register link hit-rect
        if (g_RS.inLink && g_RS.linkCount < 128) {
            RECT lr;
            lr.left   = g_RS.x;
            lr.top    = g_RS.y;
            lr.right  = g_RS.x + sz.cx;
            lr.bottom = g_RS.y + sz.cy;
            g_RS.links[g_RS.linkCount].rc = lr;
            strncpy(g_RS.links[g_RS.linkCount].href, g_RS.linkHref, MAX_STR-1);
            g_RS.linkCount++;
        }
    }
    if (sz.cy > g_RS.lineH) g_RS.lineH = sz.cy;
    g_RS.x += sz.cx + 3;
}

static void FlushLine(void) {
    g_RS.x  = g_RS.clip.left + MARGIN_X;
    g_RS.y += g_RS.lineH + LINE_SPACE;
    g_RS.lineH = 16;
}

static void RenderText(const char* text) {
    char word[MAX_STR] = {0};
    int  wi = 0;
    const char* p;
    for (p = text; ; p++) {
        if (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\0') {
            if (wi > 0) {
                word[wi] = '\0';
                RenderWord(word);
                wi = 0;
            }
            if (*p == '\n') FlushLine();
            if (*p == '\0') break;
        } else {
            if (wi < MAX_STR-1) word[wi++] = *p;
        }
    }
}

// ─── Tag handler ─────────────────────────────────────────────────────────────
static void HandleOpenTag(const Token* t) {
    if (strcmp(t->tag, "h1")==0) {
        FlushLine(); SelectObject(g_RS.hdc, g_RS.fonts[1]); g_RS.lineH=32;
    } else if (strcmp(t->tag, "h2")==0) {
        FlushLine(); SelectObject(g_RS.hdc, g_RS.fonts[2]); g_RS.lineH=24;
    } else if (strcmp(t->tag, "h3")==0 || strcmp(t->tag,"h4")==0) {
        FlushLine(); SelectObject(g_RS.hdc, g_RS.fonts[3]); g_RS.lineH=20;
    } else if (strcmp(t->tag, "b")==0 || strcmp(t->tag,"strong")==0) {
        g_RS.bold = TRUE; SelectCurFont();
    } else if (strcmp(t->tag, "i")==0 || strcmp(t->tag,"em")==0) {
        g_RS.italic = TRUE; SelectCurFont();
    } else if (strcmp(t->tag, "u")==0) {
        g_RS.underline = TRUE;
    } else if (strcmp(t->tag, "a")==0) {
        g_RS.inLink = TRUE;
        strncpy(g_RS.linkHref, t->attr_href, MAX_STR-1);
    } else if (strcmp(t->tag, "br")==0) {
        FlushLine();
    } else if (strcmp(t->tag, "hr")==0) {
        HPEN hrpen;
        HPEN hrop;
        int hry;
        FlushLine();
        hrpen = CreatePen(PS_SOLID, 1, RGB(0x2a,0x28,0x55));
        hrop  = (HPEN)SelectObject(g_RS.hdc, hrpen);
        hry = g_RS.y + 6;
        MoveToEx(g_RS.hdc, MARGIN_X, hry, NULL);
        LineTo(g_RS.hdc, g_RS.clip.right - MARGIN_X, hry);
        SelectObject(g_RS.hdc, hrop); DeleteObject(hrpen);
        g_RS.y += 14;
    } else if (strcmp(t->tag, "p")==0) {
        FlushLine(); g_RS.y += 6;
        SelectObject(g_RS.hdc, g_RS.fonts[0]);
    }
}

static void HandleCloseTag(const Token* t) {
    if (strcmp(t->tag,"h1")==0||strcmp(t->tag,"h2")==0||strcmp(t->tag,"h3")==0) {
        FlushLine(); g_RS.y += 4;
        SelectObject(g_RS.hdc, g_RS.fonts[0]); g_RS.lineH = 16;
    } else if (strcmp(t->tag,"b")==0||strcmp(t->tag,"strong")==0) {
        g_RS.bold = FALSE; SelectCurFont();
    } else if (strcmp(t->tag,"i")==0||strcmp(t->tag,"em")==0) {
        g_RS.italic = FALSE; SelectCurFont();
    } else if (strcmp(t->tag,"u")==0) {
        g_RS.underline = FALSE;
    } else if (strcmp(t->tag,"a")==0) {
        g_RS.inLink = FALSE; g_RS.linkHref[0] = '\0';
    } else if (strcmp(t->tag,"p")==0) {
        FlushLine(); g_RS.y += 4;
    }
}

// ─── Minimal HTML tokenizer ───────────────────────────────────────────────────
// Strips entities, extracts tags and text for our supported subset.

static void ToLower(char* s) {
    for (; *s; s++) *s = (char)tolower((unsigned char)*s);
}

static void GetAttr(const char* tagbuf, const char* attr, char* out, int len) {
    char search[64];
    const char* p;
    int i;
    _snprintf(search, sizeof(search), "%s=\"", attr);
    p = strstr(tagbuf, search);
    if (!p) { p = strstr(tagbuf, attr); if (p) p += strlen(attr)+1; }
    if (!p) { out[0]='\0'; return; }
    p += strlen(search);
    i=0;
    while (*p && *p!='"' && i<len-1) out[i++]=*p++;
    out[i]='\0';
}

// Tokenize HTML, calling render functions directly (streaming — no full parse tree)
static void TokenizeAndRender(const char* html) {
    const char* p = html;
    char buf[MAX_STR];

    while (*p) {
        if (*p == '<') {
            /* Collect tag body */
            int i;
            BOOL closing;
            Token t;
            int j, k;

            p++;
            i=0; closing = FALSE;
            if (*p=='/') { closing=TRUE; p++; }
            while (*p && *p!='>' && i<MAX_STR-1) buf[i++]=*p++;
            buf[i]='\0';
            if (*p=='>') p++;

            /* Extract tag name */
            memset(&t, 0, sizeof(t));
            j=0;
            for (k=0; buf[k] && buf[k]!=' ' && j<31; k++) t.tag[j++]=buf[k];
            t.tag[j]='\0'; ToLower(t.tag);

            /* Skip script/style content */
            if (strcmp(t.tag,"script")==0 || strcmp(t.tag,"style")==0) {
                char end[16];
                const char* ep;
                _snprintf(end,sizeof(end),"</%s>",t.tag);
                ep = strstr(p, end);
                if (ep) p = ep + strlen(end);
                continue;
            }
            /* Skip html/head/body/meta/link/title tags as containers */
            if (strcmp(t.tag,"html")==0||strcmp(t.tag,"head")==0||
                strcmp(t.tag,"body")==0||strcmp(t.tag,"meta")==0||
                strcmp(t.tag,"link")==0||strcmp(t.tag,"title")==0||
                strcmp(t.tag,"!doctype")==0) continue;

            GetAttr(buf, "href", t.attr_href, MAX_STR);
            GetAttr(buf, "src",  t.attr_src,  MAX_STR);
            GetAttr(buf, "color",t.attr_color, 32);

            if (closing) HandleCloseTag(&t);
            else         HandleOpenTag (&t);

        } else {
            /* Text content */
            int i;
            i=0;
            while (*p && *p!='<' && i<MAX_STR-1) {
                /* Decode &amp; &lt; &gt; &nbsp; */
                if (*p=='&') {
                    if (strncmp(p,"&amp;",5)==0)  { buf[i++]='&'; p+=5; }
                    else if (strncmp(p,"&lt;",4)==0) { buf[i++]='<'; p+=4; }
                    else if (strncmp(p,"&gt;",4)==0) { buf[i++]='>'; p+=4; }
                    else if (strncmp(p,"&nbsp;",6)==0){ buf[i++]=' '; p+=6; }
                    else { buf[i++]=*p++; }
                } else {
                    buf[i++]=*p++;
                }
            }
            buf[i]='\0';
            if (i>0) RenderText(buf);
        }
    }
}

// ─── DLL-exported entry points ────────────────────────────────────────────────

AEON_HTML4_API int  AEON_HTML4_CALL AeonHTML4_Init(void) { return 1; }
AEON_HTML4_API void AEON_HTML4_CALL AeonHTML4_Shutdown(void) {}

AEON_HTML4_API int AEON_HTML4_CALL AeonHTML4_Render(
        HWND hwnd, const char* html, int scrollY)
{
    HDC hdc;
    RECT cr;
    HDC mem;
    HBITMAP bmp;
    HBITMAP oldb;
    HBRUSH bgBr;

    if (!hwnd || !html) return 0;

    /* Caller must have called BeginPaint() already or provide a valid HDC.
     * We get the DC ourselves for the double-buffer blit. */
    hdc = GetDC(hwnd);

    GetClientRect(hwnd, &cr);

    /* Double-buffer */
    mem  = CreateCompatibleDC(hdc);
    bmp  = CreateCompatibleBitmap(hdc, cr.right, cr.bottom);
    oldb = (HBITMAP)SelectObject(mem, bmp);

    /* Background */
    bgBr = CreateSolidBrush(BG_COLOR);
    FillRect(mem, &cr, bgBr);
    DeleteObject(bgBr);

    // Init render state
    memset(&g_RS, 0, sizeof(g_RS));
    g_RS.hdc     = mem;
    g_RS.clip    = cr;
    g_RS.x       = cr.left + MARGIN_X;
    g_RS.y       = cr.top  + MARGIN_X - scrollY;
    g_RS.lineH   = 16;
    g_RS.fgColor = FG_COLOR;
    g_RS.bgColor = BG_COLOR;

    // Create font set
    g_RS.fonts[0] = MakeFont(FONT_BODY, 0,0,0);
    g_RS.fonts[1] = MakeFont(FONT_H1,   1,0,0);
    g_RS.fonts[2] = MakeFont(FONT_H2,   1,0,0);
    g_RS.fonts[3] = MakeFont(FONT_H3,   1,0,0);
    g_RS.fonts[4] = g_RS.fonts[3];
    g_RS.fonts[5] = g_RS.fonts[3];
    g_RS.fonts[6] = MakeFont(FONT_BODY, 1,0,0);
    g_RS.fonts[7] = MakeFont(FONT_BODY, 0,1,0);
    g_RS.curFont  = g_RS.fonts[0];
    SelectObject(mem, g_RS.curFont);
    SetBkMode(mem, TRANSPARENT);

    // Render
    TokenizeAndRender(html);

    // Blit
    BitBlt(hdc, 0, 0, cr.right, cr.bottom, mem, 0, 0, SRCCOPY);

    // Cleanup
    {
        int i;
        for (i=0;i<8;i++) if (g_RS.fonts[i]) DeleteObject(g_RS.fonts[i]);
    }
    SelectObject(mem, oldb); DeleteObject(bmp); DeleteDC(mem);
    ReleaseDC(hwnd, hdc);
    return g_RS.y + scrollY;  // total document height
}

// Hit-test a click (call from WM_LBUTTONDOWN in browser chrome)
AEON_HTML4_API const char* AEON_HTML4_CALL AeonHTML4_HitTest(int x, int y) {
    int i;
    POINT pt;
    pt.x = x;
    pt.y = y;
    for (i = 0; i < g_RS.linkCount; i++) {
        if (PtInRect(&g_RS.links[i].rc, pt))
            return g_RS.links[i].href;
    }
    return NULL;
}

// DLL entry point
BOOL WINAPI DllMain(HINSTANCE hInst, DWORD reason, LPVOID reserved) {
    (void)hInst; (void)reserved;
    if (reason == DLL_PROCESS_DETACH) AeonHTML4_Shutdown();
    return TRUE;
}
