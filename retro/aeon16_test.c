/*
 * aeon16_test.c — Minimal 16-bit DOS Test for AeonBrowser URL Parsing
 *
 * Validates that our core URL parsing logic works on the smallest possible
 * platform (DOS/Win 3.1). This is the ERA1 tier of the AeonOS test matrix.
 *
 * Build: wcl -bt=dos -ml -fe=aeon16.exe aeon16_test.c
 *        (Open Watcom 16-bit mode, large memory model)
 *
 * The binary outputs results to stdout AND COM1 (for serial capture).
 * DOSBox-X headless mode reads serial1=file:output.log
 *
 * Copyright (c) 2026 DelgadoLogic. All rights reserved.
 */

#include <stdio.h>
#include <string.h>
#include <stdlib.h>

/* ─── Serial I/O (DOS INT 14h) ─── */

static int g_serial_ok = 0;

#ifdef __WATCOMC__
#include <dos.h>
#include <conio.h>

static void serial_init(void)
{
    union REGS regs;
    /* INT 14h, AH=00 — initialize COM1 */
    /* 9600 baud, 8N1 = 0xE3 */
    regs.h.ah = 0x00;
    regs.h.al = 0xE3;  /* 9600,8,N,1 */
    regs.x.dx = 0x0000; /* COM1 */
    int86(0x14, &regs, &regs);
    g_serial_ok = 1;
}

static void serial_putchar(char c)
{
    union REGS regs;
    if (!g_serial_ok) return;
    regs.h.ah = 0x01;   /* Send character */
    regs.h.al = c;
    regs.x.dx = 0x0000; /* COM1 */
    int86(0x14, &regs, &regs);
}

static void serial_puts(const char *s)
{
    while (*s) {
        serial_putchar(*s);
        s++;
    }
}
#else
static void serial_init(void) { }
static void serial_puts(const char *s) { (void)s; }
#endif

/* dual_printf: write to both stdout and serial */
static char g_buf[512];

static void dual_printf(const char *fmt, ...)
{
    va_list args;
    va_start(args, fmt);
    vsprintf(g_buf, fmt, args);
    va_end(args);

    printf("%s", g_buf);
    serial_puts(g_buf);
}

/* ─── URL Parsing Logic (matches bearssl_bridge.c) ─── */

typedef struct {
    char scheme[16];
    char host[256];
    int  port;
    char path[512];
} ParsedUrl;

static int parse_url(const char *url, ParsedUrl *out)
{
    const char *p;
    const char *host_start;
    const char *host_end;
    int i;

    if (!url || !out) return -1;

    memset(out, 0, sizeof(ParsedUrl));

    /* Extract scheme */
    p = strstr(url, "://");
    if (!p) return -1;

    i = (int)(p - url);
    if (i <= 0 || i >= 15) return -1;
    memcpy(out->scheme, url, i);
    out->scheme[i] = '\0';

    /* Extract host */
    host_start = p + 3;
    if (*host_start == '\0') return -1;

    host_end = strchr(host_start, '/');
    if (!host_end) host_end = host_start + strlen(host_start);

    /* Check for port */
    {
        const char *colon = strchr(host_start, ':');
        if (colon && colon < host_end) {
            int hlen = (int)(colon - host_start);
            if (hlen <= 0 || hlen >= 255) return -1;
            memcpy(out->host, host_start, hlen);
            out->host[hlen] = '\0';
            out->port = atoi(colon + 1);
        } else {
            int hlen = (int)(host_end - host_start);
            if (hlen <= 0 || hlen >= 255) return -1;
            memcpy(out->host, host_start, hlen);
            out->host[hlen] = '\0';

            /* Default ports */
            if (strcmp(out->scheme, "https") == 0) out->port = 443;
            else if (strcmp(out->scheme, "http") == 0) out->port = 80;
            else if (strcmp(out->scheme, "gemini") == 0) out->port = 1965;
            else out->port = 0;
        }
    }

    /* Extract path */
    if (*host_end == '/') {
        strncpy(out->path, host_end, 511);
        out->path[511] = '\0';
    } else {
        strcpy(out->path, "/");
    }

    return 0;
}

/* ─── Test Framework ─── */

static int g_pass = 0;
static int g_fail = 0;
static int g_total = 0;

static void test_pass(const char *name)
{
    g_total++;
    g_pass++;
    dual_printf("  [PASS] %s\n", name);
}

static void test_fail(const char *name, const char *reason)
{
    g_total++;
    g_fail++;
    dual_printf("  [FAIL] %s: %s\n", name, reason);
}

/* ─── Test Cases ─── */

static void test_https_url(void)
{
    ParsedUrl u;
    int rc = parse_url("https://example.com/path/to/page", &u);
    if (rc != 0)           { test_fail("HTTPS parse", "returned error"); return; }
    if (strcmp(u.scheme, "https") != 0) { test_fail("HTTPS scheme", u.scheme); return; }
    if (strcmp(u.host, "example.com") != 0) { test_fail("HTTPS host", u.host); return; }
    if (u.port != 443)    { test_fail("HTTPS port", "not 443"); return; }
    if (strcmp(u.path, "/path/to/page") != 0) { test_fail("HTTPS path", u.path); return; }
    test_pass("HTTPS URL parse");
}

static void test_http_url(void)
{
    ParsedUrl u;
    int rc = parse_url("http://neverssl.com/", &u);
    if (rc != 0)           { test_fail("HTTP parse", "returned error"); return; }
    if (strcmp(u.scheme, "http") != 0) { test_fail("HTTP scheme", u.scheme); return; }
    if (strcmp(u.host, "neverssl.com") != 0) { test_fail("HTTP host", u.host); return; }
    if (u.port != 80)     { test_fail("HTTP port", "not 80"); return; }
    test_pass("HTTP URL parse");
}

static void test_gemini_url(void)
{
    ParsedUrl u;
    int rc = parse_url("gemini://gemini.circumlunar.space/", &u);
    if (rc != 0)           { test_fail("Gemini parse", "returned error"); return; }
    if (strcmp(u.scheme, "gemini") != 0) { test_fail("Gemini scheme", u.scheme); return; }
    if (u.port != 1965)   { test_fail("Gemini port", "not 1965"); return; }
    test_pass("Gemini URL parse");
}

static void test_custom_port(void)
{
    ParsedUrl u;
    int rc = parse_url("https://localhost:8443/api", &u);
    if (rc != 0)           { test_fail("Port parse", "returned error"); return; }
    if (strcmp(u.host, "localhost") != 0) { test_fail("Port host", u.host); return; }
    if (u.port != 8443)   { test_fail("Port value", "not 8443"); return; }
    if (strcmp(u.path, "/api") != 0) { test_fail("Port path", u.path); return; }
    test_pass("Custom port parse");
}

static void test_null_url(void)
{
    int rc = parse_url(NULL, NULL);
    if (rc != -1) { test_fail("NULL url", "expected -1"); return; }
    test_pass("NULL url rejected");
}

static void test_empty_url(void)
{
    ParsedUrl u;
    int rc = parse_url("", &u);
    if (rc != -1) { test_fail("Empty url", "expected -1"); return; }
    test_pass("Empty URL rejected");
}

static void test_no_scheme(void)
{
    ParsedUrl u;
    int rc = parse_url("example.com/path", &u);
    if (rc != -1) { test_fail("No scheme", "expected -1"); return; }
    test_pass("No-scheme URL rejected");
}

static void test_no_host(void)
{
    ParsedUrl u;
    int rc = parse_url("https://", &u);
    if (rc != -1) { test_fail("No host", "expected -1"); return; }
    test_pass("No-host URL rejected");
}

/* ─── Main ─── */

int main(void)
{
    serial_init();

    dual_printf("=== AeonBrowser 16-bit URL Parser Test ===\n");
    dual_printf("Platform: DOS (16-bit Real Mode)\n");
    dual_printf("Compiler: Open Watcom\n\n");

    dual_printf("--- URL Parsing Tests ---\n");
    test_https_url();
    test_http_url();
    test_gemini_url();
    test_custom_port();

    dual_printf("\n--- Edge Case Tests ---\n");
    test_null_url();
    test_empty_url();
    test_no_scheme();
    test_no_host();

    dual_printf("\n========================================\n");
    dual_printf(" Test Results: %d/%d passed\n", g_pass, g_total);
    dual_printf("========================================\n");

    if (g_pass >= g_total && g_total > 0) {
        dual_printf("AEON_TEST_RESULT=PASS\n");
    } else {
        dual_printf("AEON_TEST_RESULT=FAIL\n");
    }

    return (g_fail == 0) ? 0 : 1;
}
