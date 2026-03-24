/* AeonBrowser — wolfssl_bridge.h / wolfssl_bridge.c
 * DelgadoLogic | Senior Security Engineer (16-bit Open Watcom)
 *
 * PURPOSE: Thin shim that calls wolfssl16.dll (our 16-bit WolfSSL build)
 * to provide TLS 1.3 network access on Windows 3.x / Win9x machines.
 *
 * BACKGROUND: WolfSSL is a C-language TLS library with a 16-bit build target.
 * It was ported to Win16 originally by the WinGPT / dialup.net project
 * (studied from vintage2000.org research). We build our own from their
 * Open Watcom fork and ship wolfssl16.dll in our installer.
 *
 * IT TROUBLESHOOTING:
 *   - "wolfssl16.dll not found": DLL must be in AEON16.EXE directory.
 *   - "GetProcAddress failed": DLL version mismatch. Check installer version.
 *   - TLS handshake fails on old server: Server may require TLS 1.0 only.
 *     WolfSSL supports TLS 1.0-1.3; check server cert date.
 *   - WinSock not initialized: Call WSAStartup() before wolfssl_get().
 *     We call it in wolfssl_init(). If multiple DLLs call it, WSACleanup
 *     must be called equal number of times.
 */

#ifndef WOLFSSL_BRIDGE_H
#define WOLFSSL_BRIDGE_H

/* Returns bytes read into buf (0 on error). url must be https:// or http:// */
int wolfssl_get(const char* url, char* buf, int buf_len);
int http_get   (const char* url, char* buf, int buf_len);
int gemini_get (const char* url, char* buf, int buf_len);

void wolfssl_init(void);
void wolfssl_cleanup(void);

#endif /* WOLFSSL_BRIDGE_H */
