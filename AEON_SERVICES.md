# Aeon Browser — Services & Systems Reference
### DelgadoLogic | Confidential | March 2026
> Quick-reference for every named system, service, protocol, and component in the Aeon ecosystem.
> **Status key:** 🔴 Planned | 🟡 In Progress | 🟢 Built/Stable

---

## CORE ENGINE

### `aeon_engine.dll`
**What it is:** Our Chromium fork compiled as a DLL. The rendering engine that powers the entire browser.
Chromium provides Blink (HTML/CSS rendering), V8 (JavaScript), and WebGL. We strip Google's telemetry and replace the network stack with our own Rust router. Every website on Earth renders correctly because it is Blink under the hood.
**Status:** 🟡 Build in progress at `C:\chromium\src\out\AeonRelease\`
**Produced by:** `ninja -C out\AeonRelease chrome`
**Output file:** `chrome.dll` → renamed to `aeon_engine.dll`
**Google code removed:** Safe Browsing, Sync, Crashpad, UMA metrics, Rappor, Google APIs, sign-in, optimization guide cloud calls
**Cost to run:** $0 (compiled locally, redistributed with installer)
**Dependencies:** VS BuildTools 2022, depot_tools, ATL stub (`C:\chromium\atl_stub\`)

---

### `aeon_ai.dll` *(Year 2 goal)*
**What it is:** Proprietary C++ AI inference engine embedded inside the browser process. No external server, no Python, no Ollama dependency required in production.
Built by studying and then replacing `llama.cpp` + `ggml`. Runs quantized 3B parameter models in ~2GB RAM.
**Status:** 🔴 Planned (currently using ollama as prototype)
**Primary model:** phi-3-mini (4-bit quantized, ~2.5GB) for chat + reasoning
**Background model:** TinyLlama 1.1B or SmolLM 360M for tab intelligence
**Cost to run:** $0 (all inference is local, user's hardware)
**Why it matters:** Our moat. No other browser has a fully integrated, proprietary, privacy-first AI engine.

---

## NETWORK LAYER

### Rust Protocol Router
**What it is:** Our custom Rust binary that intercepts all browser network requests before they touch Chrome's native network stack. The traffic cop of the browser.
**Handles:**
- DoH (DNS-over-HTTPS) with 5 fallback resolvers: Cloudflare, NextDNS, AdGuard, our own, quad9
- `.onion` URL routing (Tor integration — works in any tab, not a separate window)
- `ipfs://` and `magnet:` URLs natively
- CircumventionEngine integration (GoodbyeDPI, zapret, DPI bypass)
**Status:** 🟢 Built, integrated
**Cost to run:** $0 (compiled into `Aeon.exe`, runs locally)

---

### CircumventionEngine
**What it is:** DPI (Deep Packet Inspection) bypass system that allows Aeon to load sites blocked by ISPs and governments. Combines GoodbyeDPI, zapret, and our own signature-based router.
**Methods:** TCP fragmentation, fake SNI, HTTP header manipulation, QUIC-based bypass (hysteria2 transport)
**Static signatures:** Maintained by us, bundled with each release
**Dynamic signatures:** Supplied by AeonHive (community-discovered bypasses propagate to all users within minutes)
**Status:** 🟢 Built (static signatures), 🔴 AeonHive dynamic layer planned
**Cost to run:** $0 (runs locally). Relay bandwidth for Pro tier relay nodes: included in server costs.

---

### TierDispatcher
**What it is:** Routes each network request through the appropriate path based on the user's subscription tier and network conditions.
```
Free:       DoH + direct connection + basic circumvention
Pro:        DoH + Aeon relay nodes + full circumvention chain + faster routing
Enterprise: Company proxy + DLP rules + audit log + Aeon relay fallback
```
**Authorization:** Signed JWT from Aeon Account, verified in Rust router. 30-day offline grace period.
**Status:** 🔴 Planned (Rust router exists; tier logic not yet implemented)
**Cost to run:** Free tier costs nothing. Pro relay nodes: included in server infrastructure cost (~$40–50/month for relay VPS fleet).

---

## P2P COLLECTIVE INTELLIGENCE

### AeonHive
**What it is:** The P2P gossip network that makes every Aeon feature smarter as the user base grows. No central server. No user identification. Fully opt-in.
Uses GossipSub protocol (same as Ethereum) over a DHT (Distributed Hash Table). Every AeonHive node connects to 8–12 mesh peers. Messages hop exponentially — 10K-user network reached in ~4 hops (~200ms). 1M-user network in ~7 hops.
**What it shares:** Encrypted threat hashes, anonymized circumvention signatures, federated model gradients (differential privacy), anonymized agent task patterns, autocomplete aggregates (100+ user threshold)
**What it NEVER shares:** URLs, user identity, browsing history, passwords, personal data
**Opt-in default:** OFF. User explicitly enables.
**Status:** 🔴 Planned. Research repos cloned to `research/p2p/`
**Foundation library:** iroh (Rust P2P, Apache-2.0) + go-libp2p (Go DHT, MIT)
**Gossip protocol:** go-libp2p-pubsub (GossipSub, MIT)
**Cost to run:** Nearly $0 at scale — users carry the bandwidth and storage load. Only Anchor Nodes cost money.

---

### Anchor Nodes
**What they are:** 5–10 small VPS servers run by DelgadoLogic that serve as DHT bootstrap points. They are phonebook operators — they tell peers who else is online, then get out of the way. They hold zero user data, see zero user traffic, cannot read any gossip messages (all end-to-end encrypted).
**Phase 1 (0–10K users):** Required for peer discovery
**Phase 2 (10K–100K users):** Optional fallback, network self-discovers
**Phase 3 (100K+ users):** Shut off — network doesn't notice
**Cost:** ~$6/month per node × 7 nodes = **~$42/month total**
**Recommended provider:** Hetzner (EU) or DigitalOcean (US) — $6/month for CX11 instances
**Legal exposure:** Zero. They're phonebook operators. Snowflake (Tor) has run this model for years with no legal action.

---

### AeonHive Gossip Message Format
```json
{
  "type":       "threat_sig | circ_sig | model_grad | agent_pattern | autocomplete",
  "hash":       "sha256:a3f9c2...",
  "geo_region": "US-East",
  "weight":     0.4,
  "ttl":        48,
  "pow":        "0000ab3f...",
  "sig":        "Ed25519:..."
}
```
- `weight` = reporter's trust score (0.0 new node → 1.0 after 1 year of honest participation)
- `pow` = HashCash proof-of-work — stops spam flooding
- `sig` = Ed25519 — proves message authenticity, prevents forgery
- `ttl` = 48 hours — stale data auto-purges

---

### Sybil Protection (4-Layer Defense)
| Layer | Mechanism |
|-------|-----------|
| 1 — Temporal Weight | New nodes: weight 0.0. Takes 1 year of honest participation to reach 1.0 |
| 2 — Geographic Quorum | Threat must be confirmed from 3+ distinct regions independently |
| 3 — STAR Threshold | Individual reports hidden until consensus threshold (50+ reports) via Cloudflare's `sta-rs` |
| 4 — Pro Node Anchoring | Verified Pro subscribers get 2× vote weight — buying thousands of Pro subs is not economical |

---

### Aeon Credits (Internal Economy)
**What they are:** Non-transferable, non-cash reward tokens earned by contributing to AeonHive. Cannot be sold, transferred, or converted to cash. Not a security. Not crypto.
| Action | Credits Earned |
|--------|---------------|
| Relay 1GB of AeonHive gossip | 50 |
| Cache 200MB of content for 30 days | 10/month |
| Confirmed threat report (threshold reached) | 5 |
| Verified circumvention signature | 20 |
| Agent task pattern accepted | 3 |

**Redeemable for:**
- +5GB Aeon Sync storage (per 500 Credits)
- Priority relay routing (faster TierDispatcher)
- Early access to new AI models
- Extended offline cache quota
**Cost to us:** Credits are softcurrency. No cash cost. Redeemed benefits are bucketed into existing infrastructure costs.

---

## USER DATA SERVICES

### AeonSync
**What it is:** Cross-device browser data sync using a Firefox Sync-style zero-knowledge architecture.
**How it works:** All data is AES-256 encrypted with the user's local sync key before leaving the device. Our server stores encrypted blobs it cannot read. Even if our servers are breached, attackers get gibberish.
**Synced data:** Bookmarks, settings, AeonVault password list (encrypted), browsing history (last 30 days), open tabs, extension list (not extension data)
**NOT synced:** AI model weights (P2P only for Pro), raw browsing history older than 30 days

**Tier differences:**
| Tier | Sync Method | Server |
|------|-------------|--------|
| Free | Encrypted cloud sync, 100MB cap | Cloudflare R2 |
| Pro | Encrypted cloud + direct P2P device-to-device sync via AeonHive | Cloudflare R2 + AeonHive DHT |

**Pro P2P sync:** Devices discover each other via DHT, authenticate with shared device key (derived from master sync key via HKDF), merge data using CRDT (automerge library). No server involved. Works across networks via AeonHive relay.

**Storage cost:**
- 1K users × 1MB avg = 1GB = **$0.015/month** (Cloudflare R2)
- 100K users × 1MB avg = 100GB = **$1.50/month**
- The sync backend is essentially free.

**Your sync key:** Never leaves your devices. If lost, we cannot recover it. This is intentional. Zero-knowledge.
**Status:** 🔴 Planned (Phase 3)

---

### AeonVault
**What it is:** Built-in password manager. Zero-knowledge, DPAPI-encrypted locally. Hooks directly into Blink's autofill pipeline (not a floating overlay — works on sites that block standard autofill).
**Encryption:** AES-256 + DPAPI. Master password never stored — only its hash used as key derivation input (Argon2id).
**Features:** Auto-fill, password generator, breach detection, 2FA TOTP support
**Breach detection:** Via AeonHive gossip (peers share domain breach hashes — not your passwords, just the domain name hashes)
**Biometric unlock:** Windows Hello integration
**Status:** 🟡 Architecture designed, not yet implemented
**Cost to run:** $0 (fully local)

---

### Aeon Account
**What it is:** Optional user account system for Pro subscription management and sync key backup.
**What it stores:** Email (for account recovery), hashed password (Argon2id), encrypted sync key backup (user's password encrypts this — we can't read it), subscription status.
**What it does NOT store:** Browsing data, search history, AI conversations, anything from the browser.
**Status:** 🔴 Planned (Phase 3)
**Backend:** Lightweight stateless API on Cloud Run. ~$5–15/month at early scale.

---

## AI FEATURES

### AI Sidebar
**What it is:** A persistent right-side panel providing chat, summarization, and context-aware AI assistance for any webpage.
**Invocation:** Keyboard shortcut or toolbar button
**Features:** Summarize page, explain selected text, chat with page context, rewrite/simplify content, code explanation
**Model:** phi-3-mini (local, 4-bit quantized) via ollama initially, aeon_ai.dll eventually
**Architecture:** Renderer (sandboxed) → Mojo IPC → aeon_ai utility process → inference → streamed response
**Status:** 🔴 Planned (Phase 2, after engine build completes)
**Cost to run:** $0 (local inference only)

---

### Agent Mode (Auto-Browse)
**What it is:** Give Aeon a goal in natural language. The agent autonomously navigates, clicks, reads, and returns results without sending your data anywhere.
**Architecture:**
```
User intent → llama.cpp (local) → task plan
              → Blink DOM bridge (direct DOM access, no scraping hacks)
              → Execute steps → Return result
```
**Advantage over competitors:** Direct Blink DOM bridge (we ARE the browser — no computer vision, no extension injection). On-device inference = 150–200ms per step vs 800ms–2s cloud agents. 45 seconds faster on a 30-step task.
**Memory (2 layers):**
- Personal (local, encrypted, never shared): Your preferences, your task history, AeonVault
- Collective (AeonHive, anonymized): Successful navigation patterns across all users
**Status:** 🔴 Planned (Phase 3 - P2 for architecture, P3 for full release)
**Cost to run:** $0 (local inference)
**Research repos:** `semantic-kernel`, `autogen`, `langgraph`, `agentic-platform-engineering`

---

### Smart Address Bar (AeonOmni)
**What it is:** Replaces Google Suggest with a 3-layer privacy system that's as useful without any privacy compromise.
```
Layer 1: Local History         — Your URLs, searches, bookmarks. Zero network.
Layer 2: AeonHive Collective   — Top completions from 100+ other Aeon users this week only. Anonymous aggregate.
Layer 3: Local AI Intent       — phi-3-mini classifies intent, enriches result, previews answer. Zero network.
```
**Layer 2 minimum:** A completion only surfaces if 100+ independent users matched it this week. Differential privacy noise added to counts. You can never identify an individual.
**Intent examples:** "tesla stock" → mini chart | "pizza near me" → map pin | "cancel amazon" → direct cancellation link
**Status:** 🔴 Planned (Phase 2)
**Cost to run:** $0 (all local + anonymous AeonHive aggregates are already in network)

---

### Privacy Inspector
**What it is:** Real-time detection and blocking of browser fingerprinting, tracking scripts, and surveillance techniques on every page load.
**Detected vectors:** Canvas, WebGL vendor/renderer, AudioContext, font enumeration, screen API, timing attacks, battery API, network connection API, keyboard layout, …
**With AeonHive:** Collective threat intelligence — "1,247 Aeon users visited this site. New technique detected this week: WebGL vendor string extraction. We blocked it before you loaded."
**Zero-day detection:** 5 users report anomalous behavior → AeonHive flags it globally before most users are affected
**Status:** 🔴 Planned (Phase 2)
**Cost to run:** $0

---

### AeonSleep (Tab Memory Manager)
**What it is:** Suspends inactive tabs at the C++ renderer level (not just JS freeze). Sleeping tabs use <5MB RAM vs Chrome's 80–300MB.
```
Tier 3 (budget hardware):   Sleep after 30 seconds inactive
Tier 2 (mid-range):         Sleep after 90 seconds
Tier 1 (high-end):          Sleep after 5 minutes
```
**Restored instantly** when you click the tab — 200–400ms cold start from disk cache.
**With AeonHive:** Sleep timing calibrated by collective behavior. "Gmail compose: never sleep — 12% of users lost drafts." Learned from millions of tabs, not hardcoded.
**Status:** 🟡 Tab sleep manager coded. Blink-level suspension pending engine build.
**Cost to run:** $0

---

## TELEMETRY & COMPLIANCE

### Zero-Identify Telemetry
**What it is:** Opt-in only crash reporting. First-run explicit consent. Nothing leaves your machine without permission.
| Collected (with consent) | NEVER Collected |
|--------------------------|-----------------|
| Stack trace (crash type + version) | URL at time of crash |
| Crash count by version | Machine ID or hardware fingerprint |
| Anonymous feature opt-in rates | Any user identifier |
| Aggregate tab counts (numbers only) | Browsing history, search queries |

**Implementation:** Crashpad stripped of PII extraction. Only stack hash + version number sent. With AeonHive enabled: crash data propagates through gossip instead of hitting our server — we never even see it. 50-report threshold before a crash pattern surfaces to engineering.
**Marketing:** "Aeon is the only browser where you can read exactly what leaves your machine — because the code is open and the answer is almost nothing."
**Status:** 🔴 Planned (Phase 2 - strip Crashpad, add opt-in dialog)

---

### Optimization Guide (Hijacked from Google)
**What it is:** Chrome's `components/optimization_guide/` drives ~30 features (tab discard, preloading, omnibox predictions, anti-phishing ML). Most de-Google forks gut it, breaking those features. Aeon keeps the API surface, replaces the data source.
```
Google: optimization_guide/ → Google CDN (phones home constantly)
Aeon:   optimization_guide/ → Local Aeon Cache (stays on device)
        + AeonHive collective seeds it on day 1 for new installs
```
**Cold start problem:** New install has zero optimization data. AeonHive seeds it from peers with similar hardware specs on day 1.
**Status:** 🔴 Planned (Phase 2 — high priority differentiator vs ungoogled-chromium)

---

## INFRASTRUCTURE COST SUMMARY

| System | Component | Monthly Cost |
|--------|-----------|-------------|
| AeonHive | 7× Anchor Node VPS (Hetzner CX11) | ~$42 |
| AeonSync | Cloudflare R2 storage (per GB) | ~$0–$2 |
| Aeon Account | Cloud Run stateless API | ~$5–$15 |
| Aeon Relay (Pro) | 3× relay VPS (Hetzner CX21) | ~$30 |
| **Total (Beta)** | | **~$50–$90/month** |
| **Total (1M users)** | | **~$200–$300/month** |

**Why costs barely scale:** AeonHive users carry their own bandwidth + storage. Model updates distributed P2P (BitTorrent-style) — not from our CDN. Sync data is tiny and compressed. Anchor nodes cost the same whether you have 100 or 100K users.

---

## REVENUE MODEL

| Stream | Description | Potential |
|--------|-------------|-----------|
| **Search Partnership** | Default search deal (DDG easiest first) | $$$$ at scale |
| **Aeon Pro** | $4.99–$9.99/month — Sync + AI + Premium relay | $600K/yr at 10K users |
| **Aeon Shield / Enterprise** | $15–$50/seat/month — managed browser, zero telemetry, audit logs | Island Browser charges ~$20/seat; IBM/Goldman use it |
| **AeonVPN** | $3–$5/month — premium routing via CircumventionEngine + relay | Bundled into Pro |
| **Privacy Ads** | Opt-in ads, revenue share with user as Aeon Credits | Brave model |

---

## RESEARCH REPOSITORY INDEX

### AI Engines (`research/ai-engines/`)
| Repo | License | Role |
|------|---------|------|
| llama.cpp | MIT | Core inference engine to study/fork |
| ggml | MIT | Tensor math backend |
| ollama | MIT | Local model server (prototype) |
| gemma.cpp | Apache | Single-file C++ model packaging pattern |
| mistral.rs | Apache | Rust-native inference (ties to our router) |
| LocalAI | MIT | OpenAI-compatible local wrapper |
| whisper.cpp | MIT | Voice navigation ("Hey Aeon") |
| llama-models | MIT | Meta LLaMA architecture spec |
| TinyLlama | Apache | 1.1B background tab model |
| phi-3-mini | MIT | 3.8B primary chat model |
| SmolLM | Apache | 135M–1.7B background intelligence |
| MiniCPM | Apache | 2B multimodal (privacy inspector) |
| mediapipe | Apache | Gemini Nano runtime SDK |
| LiteRT | Apache | Gemini Nano execution engine |
| ai-edge-torch | Apache | Model export to Gemini Nano format |
| agentic-platform-engineering | MIT | MCP agent patterns blueprint |
| autogen | MIT | Multi-agent orchestration |
| semantic-kernel | MIT | C# AI SDK — our stack |
| langgraph | Apache | Stateful agent graphs |
| openai-swarm | MIT | Agent handoff patterns |
| crewAI | MIT | Role-based multi-agent automation |
| graphrag | MIT | Knowledge graph + LLM retrieval |
| Jan | AGPL | Local AI chat UX reference |
| chatbot-ui | MIT | Streaming chat UI patterns |

### P2P Networks (`research/p2p/`)
| Repo | License | Role |
|------|---------|------|
| iroh | Apache-2.0 | **Primary:** Rust P2P networking for AeonHive |
| go-libp2p | MIT | DHT peer discovery + NAT traversal |
| go-libp2p-pubsub | MIT | GossipSub protocol layer |
| libtorrent | BSD-3 | AI model P2P distribution |
| automerge | MIT | CRDT state sync (conflict-free shared state) |
| yjs | MIT | JS CRDT (agent task memory sync) |
| privacypass | MPL-2.0 | Anonymous token system (study only) |
| veilid | MPL-2.0 | Anonymous P2P framework (study only) |
| yacy_search_server | GPL-2 | Proof P2P collective intelligence works (study only) |

### Federated Learning (`research/federated/`)
| Repo | License | Role |
|------|---------|------|
| flower | Apache-2.0 | **Primary:** Federated learning framework |
| PySyft | Apache-2.0 | Secure aggregation protocols |
| federated (TF) | Apache-2.0 | FedAvg reference implementation |
| secretflow | Apache-2.0 | Secure multiparty computation |
| opendp | MIT | Rust differential privacy library |
| google/differential-privacy | Apache-2.0 | C++ DP reference |
| sta-rs | BSD-3 | Cloudflare's STAR protocol (AeonHive threshold reporting) |

### Circumvention (`research/circumvention/`)
| Repo | License | Role |
|------|---------|------|
| dnscrypt-proxy | ISC | DNS encryption |
| GoodbyeDPI | Apache-2.0 | DPI bypass (Windows) |
| zapret | MIT | DPI bypass (cross-platform) |
| outline-server | Apache-2.0 | Shadowsocks relay architecture |
| snowflake | BSD-3 | P2P bridge (Pro contribution model) |
| hysteria2 | MIT | QUIC-based censorship evasion |
| psiphon-tunnel-core | GPL-3 | Study only — production circumvention reference |

---

*Last Updated: March 2026 | DelgadoLogic / Aeon Browser*
