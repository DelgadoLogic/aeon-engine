# Project: Aeon Browser Modernization & Feature Alignment Overhaul

## Architecture
- **Win32 C++17/20 Core Application**: `Aeon.exe` (entry point `AeonMain.cpp`), managing window chrome (`BrowserChrome.cpp`), engine dispatch (`TierDispatcher.cpp`), IPC pipe server (`AeonAgentPipe.cpp` on `\\.\pipe\aeon-agent`), settings (`SettingsEngine.cpp`), history & bookmarks (`HistoryEngine.cpp`), credentials (`PasswordVault.cpp` via DPAPI), downloads (`DownloadManager.cpp`), protocol routing (`NewTabHandler.cpp`), and DPI/Circumvention (`CircumventionEngine.cpp`, `NetworkSentinel.cpp`, `DnsResolver.cpp`).
- **Rendering & Subsystem DLLs**: `aeon_blink.dll` (WebView2 adapter), `aeon_router.dll` (Rust protocol router), `aeon_gecko_stealth.dll` (Camoufox C++ anti-fingerprint engine), `aeon_ai.dll` (Gemma 4 local AI inference).
- **Internal Web UI & Design System**: 7 canonical internal pages in `resources/pages/` and `resources/newtab/` (`settings.html`, `history.html`, `bookmarks.html`, `passwords.html`, `downloads.html`, `newtab.html`, `intelligence.html`) styled with DelgadoLogic's Obsidian design tokens (`#0b0616`, `#121424`, `#181a32`, `#9333ea`, `#c084fc`, `#38bdf8`, Plus Jakarta Sans, JetBrains Mono, glassmorphism, smooth micro-interactions).
- **JavaScript Bridge**: Injected `window.aeonBridge` host object dispatching JSON-RPC commands to `AeonBridge::Dispatch()` in C++.

## Feature Inventory
| # | Feature | Description | Milestone | Status | Source |
|---|---------|-------------|-----------|--------|--------|
| 1 | Unified Obsidian Design Tokens | `#0b0616`, `#121424`, `#181a32`, `#9333ea`, `#c084fc`, `#38bdf8`, Plus Jakarta Sans, JetBrains Mono | M1 | DONE | R1 |
| 2 | Internal Pages Glassmorphism & UI Micro-Interactions | Glassmorphic cards, glowing borders, smooth hover animations across all 7 pages | M1 | DONE | R1 |
| 3 | `newtab.html` Modernization & Bug Fixes | Fix template literal syntax error, remove duplicate SVG tags, integrate Obsidian stats/shortcuts | M1 | DONE | R1 |
| 4 | Protocol Handler Routing for `aeon://intelligence` | Add `intelligence` route in `NewTabHandler.cpp` to serve `pages/intelligence.html` | M1 | DONE | R1 |
| 5 | AI Model Provider Selector & Settings | Global AI selector (Gemini 1.5, Groq Llama 3.3, DeepSeek R1/V3, OpenAI, Ollama, LM Studio, Sovereign Local), API keys | M2 | DONE | R2 |
| 6 | Named Pipe IPC Automation Toggle | Setting for `\\.\pipe\aeon-agent` IPC pipe in settings and SettingsEngine | M2 | DONE | R2 |
| 7 | 2026 Circumvention & DPI Engine Settings | ByeDPI SOCKS5, GoodbyeDPI kernel desync modes (1-9), ECH, Kyber-768 TLS 1.3 | M2 | DONE | R2 |
| 8 | Stealth & Privacy Sentinel Settings | Camoufox C++ noise injection (Canvas, WebGL, AudioContext) and tracker blocking modes | M2 | DONE | R2 |
| 9 | Appearance & Chrome Controls | Theme picker (Obsidian Dark / Midnight Violet / Cyber Neon), vertical tabs toggle, new tab shortcut manager | M2 | DONE | R2 |
| 10 | History Chronological Grouping Tabs | Chronological tabs (Today, Yesterday, Last 7 Days, Older) in `history.html` | M3 | DONE | R3 |
| 11 | History Multi-Item Batch Selection & Deletion | Selection checkboxes, sticky batch action bar, batch deletion bridge call | M3 | DONE | R3 |
| 12 | History Instant Search & Filtering | Instant search filtering with empty state and term matching | M3 | DONE | R3 |
| 13 | Clear Browsing Data Modal | Dedicated Obsidian modal dialog with time ranges and granular checkboxes (history, downloads, cookies, cache, passwords) | M3 | DONE | R3 |
| 14 | Bookmarks Folder Hierarchy Tree | Nested directory tree navigation with collapsible folders and item counters | M3 | DONE | R3 |
| 15 | Bookmarks Instant Search | Tree-aware keyword search and highlight in `bookmarks.html` | M3 | DONE | R3 |
| 16 | Bookmarks HTML / JSON Import & Export | Direct Netscape HTML and JSON bookmark import/export triggers and bridge wiring | M3 | DONE | R3 |
| 17 | `HistoryEngine::GetBookmarks` SQL Fix | Fix missing `id` column selection in SQL query at line 272 of `HistoryEngine.cpp` | M3 | DONE | R3 |
| 18 | Password Vault DPAPI Unlock & State | DPAPI vault unlock state card, PIN/master password verification, Hello fallback | M4 | DONE | R4 |
| 19 | One-Click Password Reveal & Clipboard Copy | One-click copy, unmask plaintext reveal with auto-hide timer, and `revealPassword` bridge | M4 | DONE | R4 |
| 20 | New Credential Modal & Password Generator | Modal with length slider (8-64), character switches, and live strength indicator | M4 | DONE | R4 |
| 21 | Password Breach Warning Indicators | Categorized badges (Compromised, Reused, Secure) and security health metrics | M4 | DONE | R4 |
| 22 | Real-Time Download Progress Cards | Polling loop, animated glowing progress bars, speed (MB/s), ETA (mm:ss), total bytes | M4 | DONE | R4 |
| 23 | Download Manager File-Type SVG Badges | Clean SVG vector badges for .exe, .zip, .pdf, .iso, .png, .mp4, .mp3, .code | M4 | DONE | R4 |
| 24 | Download Actions & List Management | Open file, show in folder, pause, resume, retry, cancel, clear completed buttons | M4 | DONE | R4 |
| 25 | Complete C++ Bridge Method Dispatch | Implement all bridge methods in `AeonBridge.cpp` (`revealPassword`, `clearBrowsingData`, `importBookmarks`, `exportBookmarks`, `searchHistory`, `deleteHistoryEntries`, `removeDownload`, `getAiSettings`, `setAiSetting`) with 0 stubs | M5 | DONE | R5 |
| 26 | Bridge Script Injection Sync | Ensure `AeonBridge::BuildInjectionScript()` injects all new settings into `window.__aeon.settings` | M5 | DONE | R5 |
| 27 | 0 Fatal Build Errors (`build.ps1`) | Clean compilation of all C++ core and Blink engine binaries into `publish/Pro/` | M6 | DONE | AC1 |
| 28 | Inno Setup Clean Installer Build | Build `dist/AeonBrowser_Setup_v0.22.0.exe` via `installer/AeonProSetup.iss` | M6 | DONE | AC2 |
| 29 | Browser Launch & Toolbar Verification | Verify browser launch, toolbar buttons, omnibox, shield HUD, bookmarks toast, AI sparkle | M6 | DONE | AC3 |
| 30 | 100% Pass Rate Across E2E Test Suite | Full pass across Tiers 1-6 (308/308 tests passed) | M6 | DONE | AC |

## Milestones
| # | Name | Scope | Dependencies | Status |
|---|------|-------|-------------|--------|
| M1 | Design System & Internal Pages UI Overhaul | Unify all 7 internal pages under Obsidian design tokens, fix `newtab.html` bugs, fix `NewTabHandler.cpp` protocol routing | none | DONE |
| M2 | Settings Engine & UI Overhaul (R2) | Global settings overhaul in `settings.html` and `SettingsEngine.cpp/.h` (AI providers, IPC pipe, 2026 DPI, Camoufox noise, Appearance) | M1 | DONE |
| M3 | Bookmarks & History Managers Power Features (R3) | Chronological tabs, batch select, search, clear modal in `history.html`; folder tree, search, import/export in `bookmarks.html`; fix `HistoryEngine.cpp` rowid bug | M1 | DONE |
| M4 | Password Vault & Download Manager Polish (R4) | DPAPI unlock, reveal/copy, modal, breach badges in `passwords.html`; progress cards, SVG file icons, action controls in `downloads.html` | M1 | DONE |
| M5 | C++ Backend Bridge Wiring & 0-Stub Integration (R5) | Full bridge wiring in `AeonBridge.cpp/.h`, `SettingsEngine.cpp`, `HistoryEngine.cpp`, `PasswordVault.cpp`, `DownloadManager.cpp` with 0 stubs | M2, M3, M4 | DONE |
| M6 | Build, Packaging & E2E Acceptance (Final Milestone) | Clean compilation with `build.ps1`, Inno Setup build of `dist/AeonBrowser_Setup_v0.22.0.exe`, 100% E2E test pass, browser launch verification | M1, M2, M3, M4, M5 | DONE |

## Interface Contracts
All interface contracts between JavaScript `window.aeonBridge` and native C++ `AeonBridge::Dispatch` are 100% implemented with 0 stubs and verified across unit, adversarial, and E2E suites.

## Code Layout
- `resources/pages/settings.html` — Global settings page
- `resources/pages/history.html` — History manager page
- `resources/pages/bookmarks.html` — Bookmarks manager page
- `resources/pages/passwords.html` — Password vault page
- `resources/pages/downloads.html` — Download manager page
- `resources/pages/intelligence.html` — Intelligence workspace page
- `resources/newtab/newtab.html` — New tab start page
- `core/protocol/NewTabHandler.cpp` / `.h` — Internal protocol handler (`aeon://`)
- `core/settings/SettingsEngine.cpp` / `.h` — Settings persistence and schema
- `core/history/HistoryEngine.cpp` / `.h` — SQLite history and bookmarks database
- `core/security/PasswordVault.cpp` / `.h` — DPAPI encrypted password store
- `core/download/DownloadManager.cpp` / `.h` — WinINet background downloader
- `core/engine/AeonBridge.cpp` / `.h` — Host object bridge dispatcher and script injector
- `core/agent/AeonAgentPipe.cpp` / `.h` — Named pipe IPC server
- `installer/AeonProSetup.iss` — Canonical Inno Setup installer script
- `tests/e2e/` — E2E test suite (308 tests across 6 tiers)
