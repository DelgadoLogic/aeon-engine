# Aeon Browser by DelgadoLogic

> **Zero inherited vulnerabilities. Our DNA. Every OS from Windows 3.1 to Windows 11.**

![Build: In Development](https://img.shields.io/badge/build-in%20development-orange)
![License: Proprietary](https://img.shields.io/badge/license-proprietary-red)
![Company: DelgadoLogic](https://img.shields.io/badge/by-DelgadoLogic-blue)

---

## What is Aeon Browser?

Aeon is a **completely original** web browser written from scratch by DelgadoLogic.
It is NOT a Chromium fork. It is NOT a Firefox fork.

We studied the best open-source browser engines (Chromium, Mypal68, Supermium, Floorp, Waterfox, Roytam1 UXP) and rewrote everything ourselves — inheriting zero bugs, zero CVEs, and zero upstream dependencies.

**Ships bundled inside the LogicFlow installer** for DelgadoLogic users, and available as a standalone install from [delgadologic.tech](https://delgadologic.tech).

---

## Target Platform Matrix

| Tier | OS | Renderer | TLS | RAM Target |
|------|----|----------|-----|-----------|
| **Pro** | Windows 10 / 11 | Blink (our wrapper) | Native TLS 1.3 | < 200MB idle |
| **Modern** | Windows 8 / 8.1 | Blink | Native TLS 1.2/1.3 | < 150MB idle |
| **Extended** | Windows Vista / 7 | Gecko (lightweight) | Schannel + registry unlock | < 100MB idle |
| **XP-Hi** | Windows XP SP3 + SSE2 | Blink (XP build) | OCA TLS 1.2 or WolfSSL | < 80MB idle |
| **XP-Lo** | Windows XP (no SSE2) | Gecko (no SSE2) | WolfSSL | < 60MB idle |
| **2000** | Windows 2000 | HTML4 renderer | WolfSSL | < 40MB idle |
| **9x** | Windows 95/98/ME | HTML4 renderer | WolfSSL 32-bit | < 30MB idle |
| **Win16** | Windows 3.1 / 3.11 | HTML4 (16-bit GDI) | WolfSSL 16-bit | < 8MB |

---

## Supported Protocols

| Protocol | Status |
|----------|--------|
| HTTPS (TLS 1.3) | ✅ All tiers |
| HTTP | ✅ All tiers (auto-upgrade to HTTPS) |
| Tor / .onion | ✅ Via embedded Arti SOCKS5 (Win7+) |
| I2P / .i2p | ✅ Via i2pd HTTP proxy (Win7+) |
| IPFS / IPNS | ✅ Via IPFS gateway (configurable) |
| Gemini | ✅ Built-in renderer (all tiers) |
| Gopher | ✅ Built-in (all tiers) |
| FTP/FTPS | ✅ → Download Manager |
| Magnet links | ✅ → Download Manager |
| BitTorrent | ✅ Download only — no seeding EVER |
| ED2K | 🔄 Planned |
| file:// | ✅ Local file viewer |

---

## Architecture Overview

```
AeonBrowser/
├── core/                     C++ Browser Core
│   ├── probe/                HardwareProbe — OS/CPU/Tier detection
│   ├── engine/               TierDispatcher + AeonEngine DLL interface
│   ├── ui/                   EraChrome adaptive UI (Mica/Win32)
│   ├── tls/                  Universal TLS abstraction
│   ├── extensions/           MV2 shim (modern) / Native AdBlock (legacy)
│   ├── session/              Tab persistence + crash recovery
│   ├── crash/                Minidump handler + telemetry queue
│   └── memory/               Tab sleep manager (30-min idle suspension)
├── router/                   Rust Protocol Router (aeon_router.dll)
│   └── src/
│       ├── lib.rs            C FFI exports
│       ├── router.rs         14-protocol dispatcher
│       ├── downloader.rs     Download engine (NO seeding)
│       ├── tor.rs            Arti Tor integration
│       └── gemini.rs         Gemini + Gopher renderer
├── privacy/                  Content Blocker (EasyList-compatible)
├── telemetry/                PulseBridge → delgadologic.tech endpoint
└── retro/                    16-bit Open Watcom retro tier
    ├── aeon16.c              Win3.x main entry point
    ├── html4.c / html4.h     HTML4/CSS2 GDI renderer
    ├── wolfssl_bridge.c      16-bit WinSock + WolfSSL TLS 1.3
    └── makefile              Open Watcom 2.0 build
```

---

## Building

### Modern Tier (Win10/11) — MSVC x64
```bash
mkdir build && cd build
cmake .. -DAEON_TARGET_TIER=Pro -DCMAKE_BUILD_TYPE=Release
cmake --build . --config Release
```

### Legacy Tier (Vista/7) — MSVC x86
```bash
cmake .. -DAEON_TARGET_TIER=Extended -DCMAKE_BUILD_TYPE=Release
```

### XP Tier
```bash
cmake .. -DAEON_TARGET_TIER=XPHi
# Requires MSVC 2005 / 2008 toolset + Platform SDK for XP
```

### Retro 16-bit Tier (Win3.x) — Open Watcom 2.0
```bash
cd retro
wmake
```

### Rust Router DLL
```bash
cd router
cargo build --release
# Output: target/release/aeon_router.dll
```

---

## Security Model

- **Zero inherited CVEs** — not a fork, written from scratch
- **Native content blockers** — faster than a JS extension engine
- **Seeding permanently disabled** at compile-time (not config)
- **Tab process isolation** — one renderer crash does not kill the browser
- **DPAPI-backed password vault** — tied to Windows user account
- **Fingerprint randomization** — canvas, WebGL, audio, navigator
- **GPC header injected** on every request (user opt-out toggle)
- **DNS-over-HTTPS** by default (Cloudflare 1.1.1.1)
- **Telemetry** — anonymous, category-level, opt-out, same endpoint as LogicFlow

---

## Telemetry

Data collected (anonymous):
- OS tier (e.g., "WinXP-HiSpec")
- RAM bucket (<512MB / 512MB–2GB / 2GB–8GB / 8GB+)
- Crash reports (minidump path only)
- Browser locale

Data NEVER collected:
- URLs, search queries, form data, passwords, IP address

Opt-out: `HKLM\SOFTWARE\DelgadoLogic\Aeon\TelemetryEnabled = 0`
(same key as LogicFlow — one opt-out covers all DelgadoLogic products)

---

## License

Proprietary — © DelgadoLogic. All rights reserved.
Reference codebases were studied for architecture patterns only.
Component licenses (WolfSSL, Arti) are maintained via isolated DLL linking.

---

*Aeon Browser — Timeless. From Windows 3.1 to Windows 11.*
*by DelgadoLogic | [delgadologic.tech](https://delgadologic.tech)*
