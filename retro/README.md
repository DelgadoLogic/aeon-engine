# Aeon Browser — Retro Engine (`retro/`)

> GDI-based HTML4/CSS2 rendering engine with BearSSL TLS for Win3.1 through WinXP.
> Compiled with Open Watcom 2.0. Zero external DLL dependencies.

## Architecture

```
┌──────────────────────────────────────────────┐
│  Aeon Browser Core (C++17)                     │
│    └─ TierDispatcher                           │
│         └─ LoadLibrary("aeon_html4.dll")       │
│              └─ AeonEngine_Create()            │
├──────────────────────────────────────────────┤
│  aeon_html4.dll (276 KB)                       │
│  ├─ aeon_html4_adapter.c   ABI entry point    │
│  ├─ aeon_html4.c           GDI HTML4 renderer │
│  ├─ bearssl_bridge.c       TLS networking     │
│  └─ bearssl.lib            Crypto (682 KB)    │
│       └─ 259 source files (MIT license)       │
├──────────────────────────────────────────────┤
│  trust_anchors.h                               │
│  ├─ ISRG Root X1 (Let's Encrypt RSA)          │
│  ├─ ISRG Root X2 (Let's Encrypt ECDSA)        │
│  ├─ DigiCert Global Root G2                   │
│  ├─ Google Trust Services GTS Root R1         │
│  └─ GlobalSign Root CA R3                     │
└──────────────────────────────────────────────┘
```

## Build Requirements

| Tool | Version | Notes |
|------|---------|-------|
| Docker | Any recent | Hosts the OW2 build container |
| Open Watcom 2.0 | Current-build | Included in Dockerfile |

## Quick Start

### 1. Build Docker Image (one time)
```bash
cd retro/
docker build -t aeon-retro-build .
```

### 2. Build DLL
```bash
# From AeonBrowser project root:
docker run --rm -v "${PWD}:/project" -w /project/retro aeon-retro-build sh build_full.sh
```

Output:
- `aeon_html4.dll` — 276 KB, 32-bit Windows DLL
- `aeon_html4.lib` — Import library
- `bearssl.lib` — 682 KB, static crypto library

### 3. Build Test Program (optional)
```bash
docker run --rm -v "${PWD}:/project" -w /project/retro aeon-retro-build sh -c "sh build_full.sh && sh build_test.sh"
```

Output: `tls_test.exe` — Standalone HTTPS test (136 KB)

### 4. Regenerate Trust Anchors (if CAs change)
```powershell
powershell -ExecutionPolicy Bypass -File retro/gen_trust_anchors.ps1
```

## File Map

### Application Code
| File | Purpose |
|------|---------|
| `aeon_html4.c` | GDI-based HTML4/CSS2 renderer |
| `aeon_html4.h` | Renderer public API |
| `aeon_html4_adapter.c` | ABI adapter (VTable for TierDispatcher) |
| `bearssl_bridge.c` | TLS/HTTP/Gemini networking via BearSSL |
| `bearssl_bridge.h` | Bridge public API |
| `trust_anchors.h` | 5 embedded root CA certificates |
| `tls_test.c` | Standalone TLS integration test |

### Build System
| File | Purpose |
|------|---------|
| `Dockerfile` | Ubuntu 22.04 + Open Watcom 2.0 container |
| `build_full.sh` | Full build: BearSSL + app code + link DLL |
| `build_docker.sh` | Quick build: BearSSL + app code (no adapter) |
| `build_test.sh` | Build + optional Wine test of `tls_test.exe` |
| `makefile` | wmake rules (16-bit, deferred) |
| `makefile_32bit` | wmake rules (32-bit, alternative to bash) |
| `audit_skiplist.sh` | Verifies BearSSL skip list correctness |
| `gen_trust_anchors.ps1` | Regenerates trust_anchors.h from Mozilla CA bundle |

### BearSSL Library
| Path | Contents |
|------|----------|
| `bearssl/` | Full BearSSL source (MIT license) |
| `bearssl/inc/` | Public headers |
| `bearssl/src/` | 294 source files (259 compiled, 35 skipped) |
| `bearssl/LICENSE.txt` | MIT license |
| `bearssl_aeon_config.h` | OW2 platform configuration overrides |

## Supported Protocols

| Protocol | Port | TLS | Status |
|----------|------|-----|--------|
| HTTPS | 443 | ✅ BearSSL | Working |
| HTTP | 80 | N/A | Working |
| Gemini | 1965 | ✅ BearSSL | Working |

## Supported HTML

- **Tags:** `<h1>`-`<h6>`, `<p>`, `<b>`, `<i>`, `<u>`, `<a>`, `<br>`, `<img>`, basic tables
- **CSS2:** `color`, `font-size`, `font-weight`, `text-align`, `background-color`, `margin`
- **No JavaScript** — use higher tiers for JS-heavy pages

## Target Platforms

| OS | Build | Notes |
|----|-------|-------|
| Windows 3.1 / 3.11 | 16-bit (deferred) | Requires ia16-elf-gcc migration |
| Windows 95 / 98 / ME | 32-bit DLL ✅ | Primary target |
| Windows 2000 / NT 4 | 32-bit DLL ✅ | Includes GDI+ paths |
| Windows XP | 32-bit DLL ✅ | Full functionality |

## Security

### Certificate Validation
All TLS connections validate against 5 embedded root CAs covering >90% of HTTPS traffic. Certificate validation cannot be bypassed or disabled.

### Buffer Safety
All string formatting uses bounds-checked `snprintf()`. All public functions validate NULL inputs and buffer sizes.

### License
- **BearSSL:** MIT (no copyleft restriction)
- **Bridge/Adapter/Renderer:** Proprietary DelgadoLogic
- **WolfSSL:** Fully purged (zero references in production code)
- **GPL:** Zero references in production code

## BearSSL Skip List

35 files are skipped during compilation — all contain x86-64 intrinsics, POWER8 instructions, or 64-bit-only math that Open Watcom 2.0 (32-bit target) cannot compile:

- `*_x86ni*` — AES-NI intrinsics (`_mm_aesenc_si128`, etc.)
- `*_pclmul*` — Carry-less multiply intrinsics
- `*_pwr8*` — IBM POWER8 crypto instructions
- `*_sse2*` — SSE2 intrinsics
- `*_ct64*` / `aes_ct64*` — 64-bit constant-time implementations
- `ec_c25519_m6[24]` / `ec_p256_m6[24]` — 64-bit elliptic curve math
- `i62_modpow2` / `rsa_i62_*` — 62-bit RSA implementations
- `poly1305_ctmulq` — 64-bit Poly1305

Run `sh audit_skiplist.sh` to verify no files are incorrectly skipped.

## Known Issues

1. **g_docHeight unused** — Placeholder for scroll support (W202 warning in adapter)
2. **Single-threaded only** — `g_iobuf` is shared global; only one TLS session at a time
3. **16-bit build deferred** — Requires ia16-elf-gcc migration to replace OW2 16-bit compiler
