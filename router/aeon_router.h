/* AeonBrowser — aeon_router.h
 * DelgadoLogic | Senior Security Engineer
 *
 * C-compatible header for the Rust protocol router DLL.
 * Include this in C++ code to call the Rust router without name-mangling.
 *
 * On Win9x/XP WITHOUT Rust link: use the stub fallback in router_stub.c
 * which routes only HTTP/HTTPS/FTP through WinINet directly.
 *
 * IT NOTE: All pointers are caller-owned. The router never frees caller memory.
 *          Always call aeon_router_init() before any other function.
 *          Always call aeon_router_shutdown() before process exit.
 */

#ifndef AEON_ROUTER_H
#define AEON_ROUTER_H

#ifdef __cplusplus
extern "C" {
#endif

/* Router lifecycle */
int  aeon_router_init(void);
void aeon_router_shutdown(void);

/* Dispatch a URL. Fills *request_id_out on success. Returns 1 ok, 0 fail. */
int  aeon_router_dispatch(const char* url, unsigned int* request_id_out);

/* Download manager */
unsigned int aeon_download_enqueue(const char* url, const char* dest_dir);
void         aeon_download_cancel(unsigned int task_id);
unsigned char aeon_download_progress(unsigned int task_id); /* 0-100, 255=err */

/* Tor (Arti SOCKS5 — starts async, binds 127.0.0.1:9150) */
int  aeon_tor_start(void);
void aeon_tor_stop(void);

/* Gemini protocol fetch */
/* Returns bytes written to buf (max buf_len-1). Null-terminates buf. */
unsigned int aeon_gemini_fetch(const char* url, char* buf, unsigned int buf_len);

#ifdef __cplusplus
}
#endif

#endif /* AEON_ROUTER_H */
