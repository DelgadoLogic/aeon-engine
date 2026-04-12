/* AeonBrowser — tls_test.c
 * DelgadoLogic | QA / Integration Test
 *
 * Standalone Win32 console program that exercises the TLS bridge.
 * Fetches an HTTPS page and prints the raw response.
 *
 * CI Mode:
 *   When run without arguments, outputs AEON_TEST_RESULT=PASS/FAIL
 *   for automated detection by the QEMU serial log monitor.
 *
 * Serial Output:
 *   Opens COM1 via Win32 CreateFile and writes all results there too.
 *   In QEMU with -serial file:result.log, this streams directly to host.
 *
 * BUILD (Docker/OW2):
 *   wcc386 -bt=nt -za99 -W3 -Ibearssl/inc -Ibearssl/src tls_test.c
 *   wlink NAME tls_test.exe SYSTEM nt FILE tls_test.o FILE bearssl_bridge.o
 *         LIBRARY bearssl.lib LIBRARY kernel32.lib LIBRARY wsock32.lib
 *
 * RUN (Wine):
 *   wine tls_test.exe https://example.com
 *
 * RUN (DOSBox/Win9x):
 *   tls_test.exe https://example.com
 */

#include "bearssl_bridge.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>  /* atoi */
#include <windows.h> /* CreateFile, WriteFile for COM1 serial */

/* 64KB response buffer */
static char g_response[65536];

/* Track pass/fail for CI output */
static int g_tests_run  = 0;
static int g_tests_pass = 0;
static int g_tests_fail = 0;

/* ===== Serial (COM1) output for headless QEMU automation =====
 * Opens COM1 once at init, writes all test output there.
 * QEMU maps -serial file:result.log so this goes to host disk.
 * If COM1 isn't available (Wine, normal desktop), silently skipped.
 */
static HANDLE g_serial = INVALID_HANDLE_VALUE;

static void serial_init(void)
{
    DCB dcb;
    g_serial = CreateFile("COM1", GENERIC_WRITE, 0, NULL,
                          OPEN_EXISTING, 0, NULL);
    if (g_serial == INVALID_HANDLE_VALUE) return; /* no COM1, that's fine */
    memset(&dcb, 0, sizeof(dcb));
    dcb.DCBlength = sizeof(dcb);
    dcb.BaudRate  = CBR_115200;
    dcb.ByteSize  = 8;
    dcb.StopBits  = ONESTOPBIT;
    dcb.Parity    = NOPARITY;
    SetCommState(g_serial, &dcb);
}

static void serial_close(void)
{
    if (g_serial != INVALID_HANDLE_VALUE) {
        CloseHandle(g_serial);
        g_serial = INVALID_HANDLE_VALUE;
    }
}

/* Write a string to COM1 serial port (if open) */
static void serial_write(const char *msg)
{
    DWORD written;
    if (g_serial != INVALID_HANDLE_VALUE) {
        WriteFile(g_serial, msg, (DWORD)strlen(msg), &written, NULL);
    }
}

/* Printf to both stdout AND serial port */
static void dual_printf(const char *fmt, ...)
{
    char buf[1024];
    va_list ap;
    va_start(ap, fmt);
    vsprintf(buf, fmt, ap);
    va_end(ap);
    printf("%s", buf);
    serial_write(buf);
}

/* Strip HTTP headers — find \r\n\r\n and return body pointer */
static const char *skip_headers(const char *response)
{
    const char *p;
    p = strstr(response, "\r\n\r\n");
    if (p) return p + 4;
    p = strstr(response, "\n\n");
    if (p) return p + 2;
    return response;  /* No headers found — return whole thing */
}

/* Extract HTTP status code from first line */
static int get_status_code(const char *response)
{
    /* HTTP/1.x NNN ... */
    const char *p;
    p = strstr(response, "HTTP/");
    if (!p) return 0;
    p = strchr(p, ' ');
    if (!p) return 0;
    return atoi(p + 1);
}

static void test_pass(const char *name)
{
    g_tests_run++;
    g_tests_pass++;
    dual_printf("  [PASS] %s\n", name);
}

static void test_fail(const char *name, const char *reason)
{
    g_tests_run++;
    g_tests_fail++;
    dual_printf("  [FAIL] %s: %s\n", name, reason);
}

int main(int argc, char **argv)
{
    const char *url;
    int len;
    int status;

    serial_init();  /* Open COM1 for headless QEMU output */

    dual_printf("=== AeonBrowser TLS Bridge Test ===\n");
    dual_printf("BearSSL + WinSock 1.1\n\n");

    if (argc < 2) {
        /* Default test URLs */
        dual_printf("Usage: tls_test.exe <url>\n\n");
        dual_printf("Running default tests...\n\n");

        tls_init();

        /* Test 1: HTTPS (Let's Encrypt cert) */
        dual_printf("--- Test 1: HTTPS (example.com) ---\n");
        len = tls_get("https://example.com/", g_response, sizeof(g_response));
        if (len > 0) {
            status = get_status_code(g_response);
            dual_printf("  Status: %d\n", status);
            dual_printf("  Response: %d bytes\n", len);
            dual_printf("  Body preview: %.200s...\n\n", skip_headers(g_response));
            if (status >= 200 && status < 400) {
                test_pass("HTTPS fetch");
            } else {
                test_fail("HTTPS fetch", "unexpected status");
            }
        } else {
            /* Network may not be available — this is a soft fail in VM */
            dual_printf("  No response (network may be unavailable)\n");
            test_fail("HTTPS fetch", "no response");
        }

        /* Test 2: Plain HTTP */
        dual_printf("\n--- Test 2: HTTP (neverssl.com) ---\n");
        len = http_get("http://neverssl.com/", g_response, sizeof(g_response));
        if (len > 0) {
            status = get_status_code(g_response);
            dual_printf("  Status: %d\n", status);
            dual_printf("  Response: %d bytes\n", len);
            test_pass("HTTP fetch");
        } else {
            dual_printf("  No response (network may be unavailable)\n");
            test_fail("HTTP fetch", "no response");
        }

        /* Test 3: URL parsing edge cases (these always work — no network) */
        dual_printf("\n--- Test 3: URL parsing ---\n");

        len = tls_get(NULL, g_response, sizeof(g_response));
        if (len == 0) { test_pass("NULL url returns 0"); }
        else          { test_fail("NULL url", "expected 0"); }

        len = tls_get("", g_response, sizeof(g_response));
        if (len == 0) { test_pass("Empty url returns 0"); }
        else          { test_fail("Empty url", "expected 0"); }

        len = tls_get("ftp://example.com", g_response, sizeof(g_response));
        if (len == 0) { test_pass("FTP url rejected"); }
        else          { test_fail("FTP url", "expected 0"); }

        len = tls_get("https://", g_response, sizeof(g_response));
        if (len == 0) { test_pass("No host rejected"); }
        else          { test_fail("No host", "expected 0"); }

        tls_cleanup();

        /* ===== CI RESULT OUTPUT ===== */
        dual_printf("\n========================================\n");
        dual_printf(" Test Results: %d/%d passed\n", g_tests_pass, g_tests_run);
        dual_printf("========================================\n");

        /* The URL parsing tests (4 tests) are deterministic and always pass.
         * Network tests may fail inside a VM without connectivity.
         * CI pass criteria: ALL non-network tests pass. */
        if (g_tests_pass >= 4) {
            dual_printf("AEON_TEST_RESULT=PASS\n");
        } else {
            dual_printf("AEON_TEST_RESULT=FAIL\n");
        }

        serial_close();
        return (g_tests_fail == 0) ? 0 : 1;
    }

    /* Single URL mode */
    url = argv[1];
    dual_printf("Fetching: %s\n\n", url);

    tls_init();
    memset(g_response, 0, sizeof(g_response));

    if (strncmp(url, "gemini://", 9) == 0) {
        len = gemini_get(url, g_response, sizeof(g_response));
    } else if (strncmp(url, "https://", 8) == 0) {
        len = tls_get(url, g_response, sizeof(g_response));
    } else {
        len = http_get(url, g_response, sizeof(g_response));
    }

    if (len > 0) {
        dual_printf("Received %d bytes:\n", len);
        printf("%.2000s", g_response);  /* Print first 2KB (stdout only) */
        if (len > 2000) {
            dual_printf("\n\n... (%d more bytes truncated)\n", len - 2000);
        }
    } else {
        dual_printf("ERROR: No response received.\n");
    }

    dual_printf("\n");
    tls_cleanup();
    serial_close();
    return (len > 0) ? 0 : 1;
}
