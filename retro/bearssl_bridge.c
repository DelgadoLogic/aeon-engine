/* AeonBrowser — bearssl_bridge.c
 * DelgadoLogic | Security Engineer (16-bit / 32-bit Retro Tier)
 *
 * BearSSL-based TLS bridge for Win3.x / Win9x / WinXP.
 * Statically compiled — no external DLL dependency.
 *
 * BearSSL operates as a "sans-I/O" state machine: it processes TLS
 * records without touching sockets. We provide read/write callbacks
 * that bridge BearSSL to WinSock 1.1.
 *
 * Threading: NOT thread-safe. Single-tab retro tier — only one
 * active TLS connection at a time (g_iobuf is shared global).
 *
 * All code is strict C89 compliant for Open Watcom 2.0.
 *
 * LICENSE: BearSSL is MIT. This bridge is proprietary DelgadoLogic.
 */

#include "bearssl_bridge.h"
#include "bearssl/inc/bearssl.h"
#include "trust_anchors.h"   /* Embedded root CAs for cert validation */
#include <winsock.h>  /* 16-bit WinSock 1.1 header */
#include <windows.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>

/* OW2 C99 compatibility */
#ifndef snprintf
#define snprintf _snprintf
#endif

/* ------------------------------------------------------------------ */
/*  Trust Anchors: 5 Root CAs from Mozilla CA Bundle                   */
/*                                                                     */
/*  Embedded via trust_anchors.h (auto-generated).                     */
/*  Covers >90% of HTTPS traffic:                                      */
/*    - ISRG Root X1 (Let's Encrypt RSA)                               */
/*    - ISRG Root X2 (Let's Encrypt ECDSA)                             */
/*    - DigiCert Global Root G2                                        */
/*    - Google Trust Services GTS Root R1                              */
/*    - GlobalSign Root CA R3                                          */
/*                                                                     */
/*  To regenerate: powershell -File gen_trust_anchors.ps1              */
/* ------------------------------------------------------------------ */

/* I/O buffer for BearSSL TLS engine.
 * Half-duplex mode: HTTP is request-then-response, so we only need
 * one direction at a time. 16KB is the minimum for standard TLS.
 * Allocated as static global to avoid stack overflow on 16-bit.
 *
 * WARNING: Shared global — only one TLS session at a time.          */
static unsigned char g_iobuf[BR_SSL_BUFSIZE_BIDI];

/* Max HTTP request line — host(256) + path(1024) + headers(~200) */
#define REQUEST_BUF_SIZE 1500

/* ------------------------------------------------------------------ */
/*  WinSock I/O callbacks for BearSSL simplified I/O wrapper           */
/* ------------------------------------------------------------------ */

/*
 * sock_read: Read up to 'len' bytes from socket into 'data'.
 * Returns number of bytes read, or -1 on error/EOF.
 * The 'read_context' is a pointer to an int holding the socket fd.
 */
static int sock_read(void *read_context, unsigned char *data, size_t len)
{
    int fd;
    int rlen;
    fd = *(int *)read_context;
    rlen = recv(fd, (char *)data, (int)len, 0);
    if (rlen <= 0) {
        return -1;
    }
    return rlen;
}

/*
 * sock_write: Write up to 'len' bytes from 'data' to socket.
 * Returns number of bytes written, or -1 on error.
 * The 'write_context' is a pointer to an int holding the socket fd.
 */
static int sock_write(void *write_context,
                      const unsigned char *data, size_t len)
{
    int fd;
    int wlen;
    fd = *(int *)write_context;
    wlen = send(fd, (const char *)data, (int)len, 0);
    if (wlen <= 0) {
        return -1;
    }
    return wlen;
}

/* ------------------------------------------------------------------ */
/*  URL parsing (C89-compliant string helpers)                        */
/* ------------------------------------------------------------------ */

static int parse_url(const char *url,
                     char *host, int host_len,
                     char *path, int path_len,
                     int *port, int *is_tls)
{
    const char *p;
    const char *host_start;
    const char *host_end;
    const char *path_start;
    int hlen;
    int plen;

    *is_tls = 0;
    *port = 80;

    if (!url || !*url) return 0;  /* NULL or empty URL */

    if (strncmp(url, "https://", 8) == 0) {
        p = url + 8;
        *is_tls = 1;
        *port = 443;
    } else if (strncmp(url, "http://", 7) == 0) {
        p = url + 7;
    } else if (strncmp(url, "gemini://", 9) == 0) {
        p = url + 9;
        *is_tls = 1;
        *port = 1965;
    } else {
        return 0;
    }

    host_start = p;
    host_end = NULL;

    /* Find end of host (port separator or path) */
    while (*p && *p != '/' && *p != ':') {
        p++;
    }
    host_end = p;

    /* Parse optional port */
    if (*p == ':') {
        p++;
        *port = atoi(p);
        if (*port <= 0 || *port > 65535) {
            *port = (*is_tls) ? 443 : 80;
        }
        while (*p && *p != '/') {
            p++;
        }
    }

    /* Rest is path */
    path_start = (*p == '/') ? p : "/";

    hlen = (int)(host_end - host_start);
    if (hlen <= 0) return 0;  /* Empty hostname */
    if (hlen >= host_len) hlen = host_len - 1;
    memcpy(host, host_start, (size_t)hlen);
    host[hlen] = '\0';

    plen = (int)strlen(path_start);
    if (plen >= path_len) plen = path_len - 1;
    memcpy(path, path_start, (size_t)plen);
    path[plen] = '\0';

    return 1;
}

/* ------------------------------------------------------------------ */
/*  TCP socket connection helper                                       */
/* ------------------------------------------------------------------ */

static int tcp_connect(const char *host, int port)
{
    struct hostent *he;
    struct sockaddr_in sa;
    int sock;

    he = gethostbyname(host);
    if (!he) return -1;

    sock = (int)socket(AF_INET, SOCK_STREAM, 0);
    if (sock < 0) return -1;

    memset(&sa, 0, sizeof(sa));
    sa.sin_family = AF_INET;
    sa.sin_port = htons((unsigned short)port);
    memcpy(&sa.sin_addr, he->h_addr, (size_t)he->h_length);

    if (connect(sock, (struct sockaddr *)&sa, sizeof(sa)) != 0) {
        closesocket(sock);
        return -1;
    }
    return sock;
}

/* ------------------------------------------------------------------ */
/*  Internal: Init BearSSL TLS session with trust anchors              */
/* ------------------------------------------------------------------ */

static int tls_session_init(br_ssl_client_context *sc,
                            br_x509_minimal_context *xc,
                            br_sslio_context *ioc,
                            int *sock_ptr,
                            const char *host)
{
    /* Initialize BearSSL client context with "full" profile.
     * This enables all cipher suites and TLS 1.0-1.2.
     * Trust anchors from trust_anchors.h enable proper X.509
     * certificate chain validation against embedded root CAs. */
    br_ssl_client_init_full(sc, xc, TAs, TAs_NUM);

    /* Set the I/O buffer (bidirectional for standard TLS) */
    br_ssl_engine_set_buffer(&sc->eng, g_iobuf, sizeof(g_iobuf), 1);

    /* Start the TLS handshake with the server hostname (for SNI) */
    br_ssl_client_reset(sc, host, 0);

    /* Wire up simplified I/O to our WinSock callbacks */
    br_sslio_init(ioc, &sc->eng, sock_read, sock_ptr,
                  sock_write, sock_ptr);

    return 1;
}

/* ------------------------------------------------------------------ */
/*  Internal: Check BearSSL engine error after I/O failure             */
/* ------------------------------------------------------------------ */

static int tls_check_error(br_ssl_client_context *sc)
{
    int err;
    err = br_ssl_engine_last_error(&sc->eng);
    /* err == BR_ERR_OK (0) means graceful close, not an error.
     * Non-zero means something went wrong during handshake or I/O. */
    (void)err; /* TODO: Log error code for diagnostics */
    return err;
}

/* ------------------------------------------------------------------ */
/*  tls_get: HTTPS fetch via BearSSL                                   */
/* ------------------------------------------------------------------ */

int tls_get(const char *url, char *buf, int buf_len)
{
    char host[256];
    char path[1024];
    int port;
    int is_tls;
    int sock;
    int total;
    int ret;

    /* BearSSL state — must be declared at top for C89 */
    br_ssl_client_context sc;
    br_x509_minimal_context xc;
    br_sslio_context ioc;
    char request[REQUEST_BUF_SIZE];
    int req_len;

    /* Validate output buffer */
    if (!buf || buf_len <= 0) return 0;
    buf[0] = '\0';

    /* If not TLS, delegate to http_get */
    if (!parse_url(url, host, sizeof(host),
                   path, sizeof(path), &port, &is_tls)) {
        return 0;
    }
    if (!is_tls) {
        return http_get(url, buf, buf_len);
    }

    /* Open TCP socket */
    sock = tcp_connect(host, port);
    if (sock < 0) return 0;

    /* Initialize BearSSL TLS session */
    tls_session_init(&sc, &xc, &ioc, &sock, host);

    /* Build and send HTTP request over TLS (bounds-checked) */
    req_len = snprintf(request, sizeof(request),
        "GET %s HTTP/1.0\r\n"
        "Host: %s\r\n"
        "User-Agent: AeonBrowser/1.0 (Retro)\r\n"
        "Connection: close\r\n"
        "\r\n", path, host);
    if (req_len < 0 || req_len >= (int)sizeof(request)) {
        closesocket(sock);
        return 0;  /* URL too long — request wouldn't fit */
    }

    if (br_sslio_write_all(&ioc, request, (size_t)req_len) != 0) {
        tls_check_error(&sc);
        closesocket(sock);
        return 0;
    }
    if (br_sslio_flush(&ioc) != 0) {
        tls_check_error(&sc);
        closesocket(sock);
        return 0;
    }

    /* Read response */
    total = 0;
    while (total < buf_len - 1) {
        ret = br_sslio_read(&ioc, buf + total,
                            (size_t)(buf_len - 1 - total));
        if (ret < 0) break;
        total += ret;
    }
    buf[total] = '\0';

    /* Check for TLS errors (handshake failures, cert rejection, etc.) */
    if (total == 0) {
        tls_check_error(&sc);
    }

    /* Clean shutdown */
    br_sslio_close(&ioc);
    closesocket(sock);

    return total;
}

/* ------------------------------------------------------------------ */
/*  http_get: Plain HTTP fetch (no TLS)                                */
/* ------------------------------------------------------------------ */

int http_get(const char *url, char *buf, int buf_len)
{
    char host[256];
    char path[1024];
    char request[REQUEST_BUF_SIZE];
    int port;
    int is_tls;
    int sock;
    int sent;
    int total;
    int n;
    int req_len;

    /* Validate output buffer */
    if (!buf || buf_len <= 0) return 0;
    buf[0] = '\0';

    if (!parse_url(url, host, sizeof(host),
                   path, sizeof(path), &port, &is_tls)) {
        return 0;
    }
    if (is_tls) {
        return tls_get(url, buf, buf_len);
    }

    sock = tcp_connect(host, port);
    if (sock < 0) return 0;

    req_len = snprintf(request, sizeof(request),
        "GET %s HTTP/1.0\r\n"
        "Host: %s\r\n"
        "User-Agent: AeonBrowser/1.0 (Retro)\r\n"
        "Connection: close\r\n"
        "\r\n", path, host);
    if (req_len < 0 || req_len >= (int)sizeof(request)) {
        closesocket(sock);
        return 0;
    }

    sent = send(sock, request, req_len, 0);
    if (sent <= 0) {
        closesocket(sock);
        return 0;
    }

    total = 0;
    while (total < buf_len - 1) {
        n = recv(sock, buf + total, buf_len - 1 - total, 0);
        if (n <= 0) break;
        total += n;
    }
    buf[total] = '\0';

    closesocket(sock);
    return total;
}

/* ------------------------------------------------------------------ */
/*  gemini_get: Gemini protocol fetch (TLS on port 1965)               */
/* ------------------------------------------------------------------ */

int gemini_get(const char *url, char *buf, int buf_len)
{
    char host[256];
    char path[1024];
    int port;
    int is_tls;
    int sock;
    int total;
    int ret;

    /* BearSSL state */
    br_ssl_client_context sc;
    br_x509_minimal_context xc;
    br_sslio_context ioc;
    char request[REQUEST_BUF_SIZE];
    int req_len;

    /* Validate output buffer */
    if (!buf || buf_len <= 0) return 0;
    buf[0] = '\0';

    if (!parse_url(url, host, sizeof(host),
                   path, sizeof(path), &port, &is_tls)) {
        return 0;
    }

    /* Gemini always uses TLS */
    port = 1965;
    sock = tcp_connect(host, port);
    if (sock < 0) return 0;

    /* Initialize BearSSL TLS session */
    tls_session_init(&sc, &xc, &ioc, &sock, host);

    /* Gemini request: just the URL followed by CRLF */
    req_len = snprintf(request, sizeof(request), "%s\r\n", url);
    if (req_len < 0 || req_len >= (int)sizeof(request)) {
        closesocket(sock);
        return 0;
    }

    if (br_sslio_write_all(&ioc, request, (size_t)req_len) != 0) {
        tls_check_error(&sc);
        closesocket(sock);
        return 0;
    }
    if (br_sslio_flush(&ioc) != 0) {
        tls_check_error(&sc);
        closesocket(sock);
        return 0;
    }

    /* Read response */
    total = 0;
    while (total < buf_len - 1) {
        ret = br_sslio_read(&ioc, buf + total,
                            (size_t)(buf_len - 1 - total));
        if (ret < 0) break;
        total += ret;
    }
    buf[total] = '\0';

    if (total == 0) {
        tls_check_error(&sc);
    }

    br_sslio_close(&ioc);
    closesocket(sock);

    return total;
}

/* ------------------------------------------------------------------ */
/*  Init / Cleanup                                                     */
/* ------------------------------------------------------------------ */

void tls_init(void)
{
    WSADATA wd;
    WSAStartup(0x0101, &wd);  /* WinSock 1.1 */
    /* BearSSL requires no global init — everything is per-context */
}

void tls_cleanup(void)
{
    WSACleanup();
    /* BearSSL requires no global cleanup */
}
