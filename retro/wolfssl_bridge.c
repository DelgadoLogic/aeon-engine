/* AeonBrowser — wolfssl_bridge.c
 * DelgadoLogic | Security Engineer (16-bit Open Watcom)
 *
 * Thin wrapper around wolfssl16.dll for HTTPS/TLS 1.3 on Win3.x / Win9x.
 * WinSock 1.1 (16-bit) API used intentionally — works on Win3.11 TCP/IP
 * and Trumpet Winsock. WinSock 2 is NOT used (unavailable on Win3.x).
 *
 * IT NOTE: WinSock on Win3.x requires WINSOCK.DLL to be loaded. It is
 * part of Microsoft TCP/IP-32 or Trumpet Winsock. Users without it
 * cannot browse the internet — this is a user-side prereq, not our bug.
 */

#include "wolfssl_bridge.h"
#include <winsock.h>  /* 16-bit WinSock 1.1 header */
#include <windows.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>

/* WolfSSL DLL function pointer types (16-bit calling convention) */
typedef int  (FAR PASCAL *WolfSSL_Init_t)        (void);
typedef void (FAR PASCAL *WolfSSL_Cleanup_t)      (void);
typedef void*(FAR PASCAL *WolfSSL_CTX_New_t)      (void* method);
typedef void*(FAR PASCAL *WolfSSL_TLSv1_3_t)      (void);
typedef void*(FAR PASCAL *WolfSSL_New_t)           (void* ctx);
typedef int  (FAR PASCAL *WolfSSL_SetFd_t)        (void* ssl, int fd);
typedef int  (FAR PASCAL *WolfSSL_Connect_t)      (void* ssl);
typedef int  (FAR PASCAL *WolfSSL_Write_t)        (void* ssl, const void* data, int sz);
typedef int  (FAR PASCAL *WolfSSL_Read_t)         (void* ssl, void* buf, int sz);
typedef void (FAR PASCAL *WolfSSL_Free_t)         (void* ssl);
typedef void (FAR PASCAL *WolfSSL_CTX_Free_t)     (void* ctx);

static HINSTANCE g_hWolfSSL = NULL;
static WolfSSL_Init_t      pfnInit     = NULL;
static WolfSSL_Cleanup_t   pfnCleanup  = NULL;
static WolfSSL_CTX_New_t   pfnCtxNew   = NULL;
static WolfSSL_TLSv1_3_t   pfnMethod   = NULL;
static WolfSSL_New_t        pfnNew      = NULL;
static WolfSSL_SetFd_t      pfnSetFd    = NULL;
static WolfSSL_Connect_t    pfnConnect  = NULL;
static WolfSSL_Write_t      pfnWrite    = NULL;
static WolfSSL_Read_t       pfnRead     = NULL;
static WolfSSL_Free_t       pfnFree     = NULL;
static WolfSSL_CTX_Free_t   pfnCtxFree  = NULL;

void wolfssl_init(void) {
    WSADATA wd;
    WSAStartup(0x0101, &wd); /* WinSock 1.1 */

    g_hWolfSSL = LoadLibrary("wolfssl16.dll");
    if (!g_hWolfSSL) {
        MessageBox(NULL, "wolfssl16.dll not found.\n"
            "Place it in the same directory as AEON16.EXE.\n"
            "HTTPS will not be available.",
            "Aeon Browser — TLS Error", MB_ICONERROR | MB_OK);
        return;
    }

    /* Resolve function pointers */
    pfnInit    = (WolfSSL_Init_t)   GetProcAddress(g_hWolfSSL, "wolfSSL_Init");
    pfnCleanup = (WolfSSL_Cleanup_t)GetProcAddress(g_hWolfSSL, "wolfSSL_Cleanup");
    pfnMethod  = (WolfSSL_TLSv1_3_t)GetProcAddress(g_hWolfSSL, "wolfTLSv1_3_client_method");
    pfnCtxNew  = (WolfSSL_CTX_New_t) GetProcAddress(g_hWolfSSL, "wolfSSL_CTX_new");
    pfnNew     = (WolfSSL_New_t)     GetProcAddress(g_hWolfSSL, "wolfSSL_new");
    pfnSetFd   = (WolfSSL_SetFd_t)   GetProcAddress(g_hWolfSSL, "wolfSSL_set_fd");
    pfnConnect = (WolfSSL_Connect_t) GetProcAddress(g_hWolfSSL, "wolfSSL_connect");
    pfnWrite   = (WolfSSL_Write_t)   GetProcAddress(g_hWolfSSL, "wolfSSL_write");
    pfnRead    = (WolfSSL_Read_t)    GetProcAddress(g_hWolfSSL, "wolfSSL_read");
    pfnFree    = (WolfSSL_Free_t)    GetProcAddress(g_hWolfSSL, "wolfSSL_free");
    pfnCtxFree = (WolfSSL_CTX_Free_t)GetProcAddress(g_hWolfSSL, "wolfSSL_CTX_free");

    if (pfnInit) pfnInit();
}

void wolfssl_cleanup(void) {
    if (pfnCleanup) pfnCleanup();
    if (g_hWolfSSL) FreeLibrary(g_hWolfSSL);
    g_hWolfSSL = NULL;
    WSACleanup();
}

/* Parse host and path from "https://host/path" or "http://host/path" */
static void parse_url(const char* url, char* host, char* path, int* port, int* is_https) {
    const char* p = url;
    int i;
    *is_https = 0;
    *port     = 80;
    if (strncmp(p, "https://", 8) == 0) { *is_https = 1; *port = 443; p += 8; }
    else if (strncmp(p, "http://",  7) == 0)               p += 7;

    /* Extract host (up to '/' or ':' or end) */
    i = 0;
    while (*p && *p != '/' && *p != ':' && i < 255) host[i++] = *p++;
    host[i] = '\0';

    if (*p == ':') { *port = atoi(++p); while (*p && *p != '/') p++; }
    if (*p == '/') { strncpy(path, p, 255); path[255] = '\0'; }
    else             strcpy(path, "/");
}

/* Perform plain HTTP GET via WinSock 1.1 */
int http_get(const char* url, char* buf, int buf_len) {
    char host[256], path[256];
    int  port, is_https;
    struct hostent FAR* he;
    SOCKET sock;
    struct sockaddr_in sa;
    char req[512];
    int total, r;

    memset(host, 0, sizeof(host));
    memset(path, 0, sizeof(path));
    port = 80; is_https = 0;
    parse_url(url, host, path, &port, &is_https);

    he = gethostbyname(host);
    if (!he) return 0;

    sock = socket(AF_INET, SOCK_STREAM, 0);
    memset(&sa, 0, sizeof(sa));
    sa.sin_family = AF_INET;
    sa.sin_port   = htons((u_short)port);
    memcpy(&sa.sin_addr, he->h_addr, he->h_length);

    if (connect(sock, (struct sockaddr FAR*)&sa, sizeof(sa)) != 0) {
        closesocket(sock); return 0;
    }

    wsprintf(req, "GET %s HTTP/1.0\r\nHost: %s\r\nUser-Agent: Aeon/1.0\r\n\r\n",
        path, host);
    send(sock, req, lstrlen(req), 0);

    total = 0;
    while ((r = recv(sock, buf + total, buf_len - total - 1, 0)) > 0)
        total += r;
    buf[total] = '\0';
    closesocket(sock);

    /* Strip HTTP response headers (find \r\n\r\n boundary) */
    {
        char* body = strstr(buf, "\r\n\r\n");
        if (body) {
            body += 4;
            total = total - (int)(body - buf);
            memmove(buf, body, total + 1);
        }
    }
    return total;
}

/* HTTPS via WolfSSL 16-bit */
int wolfssl_get(const char* url, char* buf, int buf_len) {
    char host[256], path[256];
    int  port, is_https;
    struct hostent FAR* he;
    SOCKET sock;
    struct sockaddr_in sa;
    void* ctx;
    void* ssl;
    char req[512];
    int total, r;

    if (!g_hWolfSSL || !pfnMethod) return 0; /* No WolfSSL - offline mode */

    memset(host, 0, sizeof(host));
    memset(path, 0, sizeof(path));
    port = 443; is_https = 0;
    parse_url(url, host, path, &port, &is_https);

    he = gethostbyname(host);
    if (!he) return 0;

    sock = socket(AF_INET, SOCK_STREAM, 0);
    memset(&sa, 0, sizeof(sa));
    sa.sin_family = AF_INET;
    sa.sin_port   = htons((u_short)port);
    memcpy(&sa.sin_addr, he->h_addr, he->h_length);

    if (connect(sock, (struct sockaddr FAR*)&sa, sizeof(sa)) != 0) {
        closesocket(sock); return 0;
    }

    /* TLS handshake via WolfSSL */
    ctx = pfnCtxNew(pfnMethod());
    ssl = pfnNew(ctx);
    pfnSetFd(ssl, (int)sock);

    if (pfnConnect(ssl) != 1) {
        pfnFree(ssl); pfnCtxFree(ctx); closesocket(sock); return 0;
    }

    wsprintf(req, "GET %s HTTP/1.0\r\nHost: %s\r\nUser-Agent: Aeon/1.0\r\n\r\n",
        path, host);
    pfnWrite(ssl, req, lstrlen(req));

    total = 0;
    while ((r = pfnRead(ssl, buf + total, buf_len - total - 1)) > 0)
        total += r;
    buf[total] = '\0';

    pfnFree(ssl); pfnCtxFree(ctx); closesocket(sock);

    /* Strip HTTP response headers */
    {
        char* body = strstr(buf, "\r\n\r\n");
        if (body) {
            body += 4;
            total = total - (int)(body - buf);
            memmove(buf, body, total + 1);
        }
    }
    return total;
}

/* Gemini: TLS + "URL\r\n" + read response */
int gemini_get(const char* url, char* buf, int buf_len) {
    char host[256], path[256];
    int  port, dummy;
    struct hostent FAR* he;
    SOCKET sock;
    struct sockaddr_in sa;
    void* ctx;
    void* ssl;
    char req[512];
    int total, r;

    if (!g_hWolfSSL || !pfnMethod) return 0;

    memset(host, 0, sizeof(host));
    memset(path, 0, sizeof(path));
    port = 1965; dummy = 0;
    parse_url(url, host, path, &port, &dummy);
    port = 1965; /* Gemini always 1965 */

    he = gethostbyname(host);
    if (!he) return 0;

    sock = socket(AF_INET, SOCK_STREAM, 0);
    memset(&sa, 0, sizeof(sa));
    sa.sin_family = AF_INET;
    sa.sin_port   = htons(1965);
    memcpy(&sa.sin_addr, he->h_addr, he->h_length);

    if (connect(sock, (struct sockaddr FAR*)&sa, sizeof(sa)) != 0) {
        closesocket(sock); return 0;
    }

    ctx = pfnCtxNew(pfnMethod());
    ssl = pfnNew(ctx);
    pfnSetFd(ssl, (int)sock);
    if (pfnConnect(ssl) != 1) {
        pfnFree(ssl); pfnCtxFree(ctx); closesocket(sock); return 0;
    }

    /* Gemini request: just the URL + CRLF */
    lstrcpy(req, url); lstrcat(req, "\r\n");
    pfnWrite(ssl, req, lstrlen(req));

    total = 0;
    while ((r = pfnRead(ssl, buf + total, buf_len - total - 1)) > 0)
        total += r;
    buf[total] = '\0';

    pfnFree(ssl); pfnCtxFree(ctx); closesocket(sock);
    return total;
}
