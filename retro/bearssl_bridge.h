/* AeonBrowser — bearssl_bridge.h
 * DelgadoLogic | Security Engineer (16-bit / 32-bit Retro Tier)
 *
 * PURPOSE: Drop-in TLS bridge using BearSSL (MIT licensed) for HTTPS,
 * HTTP, and Gemini protocol access on Win3.x / Win9x / WinXP.
 *
 * Proprietary bridge — BearSSL (MIT) is statically linked.
 *
 * BearSSL is statically linked — no external DLL dependency.
 * WinSock 1.1 (16-bit) API used for socket I/O.
 *
 * SUPPORTED PROTOCOLS:
 *   - HTTPS (TLS 1.0 - 1.2 via BearSSL)
 *   - HTTP  (plaintext via WinSock)
 *   - Gemini (TLS via BearSSL, port 1965)
 *
 * IT TROUBLESHOOTING:
 *   - "TLS handshake failed": Server may require TLS 1.3 (unsupported
 *     on retro tier). Modern tiers use native Schannel instead.
 *   - "Certificate validation failed": Root CA store may be stale.
 *     Ship updated trust_anchors.h with releases.
 *   - WinSock not initialized: Call tls_init() before any *_get().
 */

#ifndef BEARSSL_BRIDGE_H
#define BEARSSL_BRIDGE_H

/* Returns bytes read into buf (0 on error). url must be https:// or http:// */
int tls_get    (const char *url, char *buf, int buf_len);
int http_get   (const char *url, char *buf, int buf_len);
int gemini_get (const char *url, char *buf, int buf_len);

void tls_init(void);
void tls_cleanup(void);

#endif /* BEARSSL_BRIDGE_H */
