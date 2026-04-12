/* AeonBrowser — BearSSL config overrides for 16-bit Open Watcom
 * DelgadoLogic | Security Engineer
 *
 * PURPOSE: Force-configure BearSSL for our 16-bit/32-bit retro target.
 * This file is included BEFORE config.h via -DBEARSSL_AEON_CONFIG=1
 * and overrides autodetection that would fail on Open Watcom.
 *
 * The original config.h uses comments to document options (none are
 * defined by default). We define the ones we need here, and config.h
 * will skip autodetection for anything already defined.
 */

#ifndef BEARSSL_AEON_CONFIG_H
#define BEARSSL_AEON_CONFIG_H

/* ---- Architecture ---------------------------------------------------- */

/* 16-bit: definitely NOT 64-bit. Disable 64-bit optimizations. */
#define BR_64          0

/* No SSE2, no AES-NI, no RDRAND on 16-bit targets */
#define BR_AES_X86NI   0
#define BR_SSE2        0
#define BR_RDRAND      0
#define BR_POWER8      0

/* No 128-bit integer support */
#define BR_INT128      0
#define BR_UMUL128     0

/* Unaligned access: x86 is LE-unaligned, but on 16-bit large model
 * with far pointers, unaligned access through casts can be risky.
 * Disable and use the safe byte-by-byte path. */
#define BR_LE_UNALIGNED  0
#define BR_BE_UNALIGNED  0

/* Multiplications: 16-bit x86 has slow 32-bit multiplies.
 * Enable the "slow multiply" path to use shifts instead. */
#define BR_SLOW_MUL    1
#define BR_LOMUL       1

/* ---- Randomness ------------------------------------------------------ */

/* Windows CryptoAPI for entropy (CryptGenRandom).
 * On Win3.x this isn't available — we'll need a fallback for 16-bit.
 * On Win9x/NT it works fine via advapi32.dll.
 * For 16-bit: BearSSL will use its HMAC-DRBG without auto-seeding;
 * we manually seed from WinSock socket entropy + timer. */
#define BR_USE_URANDOM      0
#define BR_USE_GETENTROPY   0

/* Only enable Win32 rand on 32-bit builds */
#ifdef __386__
#define BR_USE_WIN32_RAND   1
#else
#define BR_USE_WIN32_RAND   0
#endif

/* ---- Time ------------------------------------------------------------ */

/* Win32 time for certificate validation (32-bit only).
 * 16-bit: we skip time checks (certs accepted regardless of expiry). */
#define BR_USE_UNIX_TIME    0
#ifdef __386__
#define BR_USE_WIN32_TIME   1
#else
#define BR_USE_WIN32_TIME   0
#endif

/* ---- Compiler -------------------------------------------------------- */

/* Open Watcom is not GCC, Clang, or MSVC */
#define BR_GCC     0
#define BR_CLANG   0
#define BR_MSC     0

/* No target attributes on OW2 */
#define BR_TARGET(x)

#endif /* BEARSSL_AEON_CONFIG_H */
