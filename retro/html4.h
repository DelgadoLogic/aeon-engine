/* AeonBrowser — html4.h / html4.c
 * DelgadoLogic | Lead Engineer (Open Watcom 16-bit)
 *
 * PURPOSE: Lightweight HTML4/CSS2 renderer for Win3.x / Win9x / Win2000.
 * Targets machines with 2-64MB RAM. Renders using GDI only — no OpenGL,
 * no DirectDraw. Suitable for text pages, Gemini, and simple tables.
 *
 * DESIGN CONSTRAINTS (16-bit large model, Open Watcom 2.0):
 *   - No malloc() calls > 64KB per block (segment limit)
 *   - No STL / templates (not available in OW 16-bit)
 *   - All function pointers must be 32-bit FAR pointers
 *   - Stack limited to 8KB — avoid large local arrays
 *
 * SUPPORTED HTML SUBSET:
 *   <h1>-<h6>, <p>, <b>, <i>, <u>, <a href>, <br>, <hr>
 *   <ul>, <ol>, <li>, <table>, <tr>, <td>, <th>
 *   <img> (loads BMP only — no JPEG/PNG without extra DLL)
 *   Basic CSS: color, font-size, font-weight, margin, padding
 *
 * NOT SUPPORTED (by design — saves code space):
 *   JavaScript, CSS animations, WebAssembly, video, SVG, WebGL.
 */

#ifndef HTML4_H
#define HTML4_H

#include <windows.h>

#define HTML4_MAX_ELEMENTS 512   /* Max elements per page (16-bit constraint) */
#define HTML4_MAX_TEXT     8192  /* Max text chars per element                */
#define HTML4_MAX_URL      256

typedef enum {
    HTML4_ELEM_TEXT = 0,
    HTML4_ELEM_H1, HTML4_ELEM_H2, HTML4_ELEM_H3,
    HTML4_ELEM_H4, HTML4_ELEM_H5, HTML4_ELEM_H6,
    HTML4_ELEM_P, HTML4_ELEM_BR, HTML4_ELEM_HR,
    HTML4_ELEM_A,    /* anchor — text + href */
    HTML4_ELEM_IMG,  /* image — src only, BMP format */
    HTML4_ELEM_UL_ITEM, HTML4_ELEM_OL_ITEM,
    HTML4_ELEM_TABLE_CELL
} Html4_ElemType;

typedef struct {
    Html4_ElemType type;
    char  text[HTML4_MAX_TEXT];
    char  href[HTML4_MAX_URL];   /* for anchors and images */
    BOOL  bold;
    BOOL  italic;
    BOOL  underline;
    COLORREF color;
    int   font_size_pt;          /* 8–72pt */
} Html4_Element;

typedef struct {
    Html4_Element elements[HTML4_MAX_ELEMENTS];
    int           count;
    int           scroll_y;      /* vertical scroll position in pixels */
    char          title[256];
    COLORREF      bg_color;
    COLORREF      fg_color;
} Html4_Document;

/* Lifecycle */
void html4_init(Html4_Document* doc);
void html4_free(Html4_Document* doc);

/* Parse HTML string into doc. Resets doc.elements first. */
void html4_parse(Html4_Document* doc, const char* html, int len);

/* Render doc into hdc within the given RECT. */
void html4_render(const Html4_Document* doc, HDC hdc, const RECT* rc);

/* Return the href of the anchor at pixel position (x,y), or NULL if none. */
const char* html4_hittest(const Html4_Document* doc,
                           const RECT* rc, int x, int y);

#endif /* HTML4_H */
