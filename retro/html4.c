/* AeonBrowser — html4.c
 * DelgadoLogic | Lead Engineer (16-bit Open Watcom)
 * See html4.h for full documentation.
 */

#include "html4.h"
#include <string.h>
#include <stdlib.h>
#include <stdio.h>

/* -------------------------------------------------------------------------
 * Init / Free
 * ---------------------------------------------------------------------- */
void html4_init(Html4_Document* doc) {
    memset(doc, 0, sizeof(*doc));
    doc->bg_color = RGB(255, 255, 255);
    doc->fg_color = RGB(0,   0,   0);
}

void html4_free(Html4_Document* doc) {
    /* No heap allocations in our model — all inline buffers */
    memset(doc, 0, sizeof(*doc));
}

/* -------------------------------------------------------------------------
 * PARSER: simple sequential tag scanner
 * Not a full DOM — we produce a flat list of renderable elements.
 * Sufficient for text pages, news sites (without JS), and Gemini content.
 * ---------------------------------------------------------------------- */

static void AddElem(Html4_Document* doc, Html4_ElemType type,
                    const char* text, const char* href,
                    BOOL bold, BOOL italic, int sz) {
    if (doc->count >= HTML4_MAX_ELEMENTS) return;
    Html4_Element* e = &doc->elements[doc->count++];
    e->type      = type;
    e->bold      = bold;
    e->italic    = italic;
    e->underline = (type == HTML4_ELEM_A);
    e->color     = (type == HTML4_ELEM_A) ? RGB(0,0,200) : RGB(0,0,0);
    e->font_size_pt = sz;
    if (text) strncpy(e->text, text, HTML4_MAX_TEXT - 1);
    if (href) strncpy(e->href, href, HTML4_MAX_URL  - 1);
}

/* Extremely simplified tag parser — no attribute parser, no CSS cascade */
void html4_parse(Html4_Document* doc, const char* html, int len) {
    doc->count = 0;
    BOOL in_tag = FALSE;
    char tag[64] = {0};
    int  ti = 0;
    char text[HTML4_MAX_TEXT] = {0};
    int  xi = 0;
    int  heading = 0;
    BOOL bold = FALSE, italic = FALSE;

    for (int i = 0; i < len; ++i) {
        char c = html[i];
        if (c == '<') {
            /* Flush pending text */
            if (xi > 0 && xi < HTML4_MAX_TEXT) {
                text[xi] = '\0';
                Html4_ElemType t = heading > 0
                    ? (Html4_ElemType)(HTML4_ELEM_H1 + heading - 1)
                    : HTML4_ELEM_P;
                int sz = heading > 0 ? (24 - heading * 2) : 10;
                AddElem(doc, t, text, NULL, bold || heading > 0, italic, sz);
                xi = 0;
            }
            in_tag = TRUE; ti = 0;
        } else if (c == '>' && in_tag) {
            in_tag = FALSE;
            tag[ti] = '\0';

            /* Parse tag type (first word) */
            char t0[32] = {0};
            sscanf(tag, "%31s", t0);
            /* Lowercase */
            for (char* p = t0; *p; ++p)
                if (*p >= 'A' && *p <= 'Z') *p |= 0x20;

            if (!strcmp(t0, "h1"))       heading = 1;
            else if (!strcmp(t0, "h2"))  heading = 2;
            else if (!strcmp(t0, "h3"))  heading = 3;
            else if (!strcmp(t0, "h4") || !strcmp(t0,"h5") || !strcmp(t0,"h6"))
                                         heading = 4;
            else if (!strcmp(t0, "/h1") || !strcmp(t0,"/h2") ||
                     !strcmp(t0, "/h3") || !strcmp(t0,"/h4"))
                                         heading = 0;
            else if (!strcmp(t0, "b") || !strcmp(t0,"strong"))  bold = TRUE;
            else if (!strcmp(t0, "/b") || !strcmp(t0,"/strong")) bold = FALSE;
            else if (!strcmp(t0, "i") || !strcmp(t0,"em"))      italic = TRUE;
            else if (!strcmp(t0, "/i") || !strcmp(t0,"/em"))    italic = FALSE;
            else if (!strcmp(t0, "br"))
                AddElem(doc, HTML4_ELEM_BR, NULL, NULL, FALSE, FALSE, 10);
            else if (!strcmp(t0, "hr"))
                AddElem(doc, HTML4_ELEM_HR, NULL, NULL, FALSE, FALSE, 10);
            else if (!strcmp(t0, "title")) {
                /* TODO: capture next text as doc->title */
            }
        } else if (in_tag) {
            if (ti < 63) tag[ti++] = c;
        } else {
            /* Regular character */
            if (c == '\n' || c == '\r') c = ' ';
            if (xi < HTML4_MAX_TEXT - 1) text[xi++] = c;
        }
    }
    /* Flush trailing text */
    if (xi > 0) {
        text[xi] = '\0';
        AddElem(doc, HTML4_ELEM_P, text, NULL, bold, italic, 10);
    }
}

/* -------------------------------------------------------------------------
 * RENDERER: GDI-based layout engine
 * Flows elements top-to-bottom within the given RECT.
 * ---------------------------------------------------------------------- */
void html4_render(const Html4_Document* doc, HDC hdc, const RECT* rc) {
    /* Clear background */
    HBRUSH bgBrush = CreateSolidBrush(doc->bg_color);
    FillRect(hdc, rc, bgBrush);
    DeleteObject(bgBrush);

    SetBkMode(hdc, TRANSPARENT);

    int y = rc->top - doc->scroll_y + 8;
    int margin = rc->left + 12;
    int maxW   = rc->right - margin - 12;

    for (int i = 0; i < doc->count; ++i) {
        const Html4_Element* e = &doc->elements[i];

        if (e->type == HTML4_ELEM_BR) { y += 6; continue; }
        if (e->type == HTML4_ELEM_HR) {
            HPEN pen = CreatePen(PS_SOLID, 1, RGB(128,128,128));
            HPEN old = SelectObject(hdc, pen);
            MoveToEx(hdc, margin, y + 4, NULL);
            LineTo  (hdc, rc->right - 12, y + 4);
            SelectObject(hdc, old);
            DeleteObject(pen);
            y += 12;
            continue;
        }
        if (e->text[0] == '\0') continue;

        /* Create font for this element */
        int ptH = -MulDiv(e->font_size_pt, GetDeviceCaps(hdc, LOGPIXELSY), 72);
        HFONT font = CreateFont(
            ptH, 0, 0, 0,
            e->bold ? FW_BOLD : FW_NORMAL,
            e->italic ? TRUE : FALSE,
            e->underline ? TRUE : FALSE,
            FALSE,
            ANSI_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
            DEFAULT_QUALITY, DEFAULT_PITCH | FF_ROMAN,
            "Times New Roman");
        HFONT oldFont = SelectObject(hdc, font);

        /* Measure and word-wrap */
        RECT textRect = { margin, y, margin + maxW, y + 1000 };
        SetTextColor(hdc, e->color);
        DrawText(hdc, e->text, -1, &textRect,
                 DT_WORDBREAK | DT_CALCRECT);
        DrawText(hdc, e->text, -1, &textRect, DT_WORDBREAK);

        y = textRect.bottom + (e->type >= HTML4_ELEM_H1 &&
                                e->type <= HTML4_ELEM_H6 ? 8 : 4);

        SelectObject(hdc, oldFont);
        DeleteObject(font);

        /* Stop rendering if past bottom of rect */
        if (y > rc->bottom) break;
    }
}

const char* html4_hittest(const Html4_Document* doc,
                           const RECT* rc, int x, int y) {
    /* TODO: iterate elements, check if (x,y) falls in a link's RECT */
    (void)doc; (void)rc; (void)x; (void)y;
    return NULL;
}
