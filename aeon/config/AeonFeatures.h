// AeonFeatures.h — Sovereign Chromium Feature Override
// Maps Chrome Canary features to Aeon's adopt/strip/replace policy.
// Include in: chrome/browser/aeon/aeon_features.cc

#pragma once
#include "base/feature_list.h"

namespace aeon {

// ─────────────────────────────────────────────────────────────────────────────
// ADOPTED from Chrome Canary — Enabled by default in Aeon
// These are security/privacy/platform wins with no cloud dependency.
// ─────────────────────────────────────────────────────────────────────────────

// DBSC: Device Bound Session Credentials (Chrome 145+)
// Binds session cookies to hardware — prevents cookie theft.
// AeonDNA verdict: ADOPT. Sovereign session protection.
BASE_DECLARE_FEATURE(kAeonDeviceBoundSessionCredentials);

// Sanitizer API: Native DOM sanitization to block XSS
// AeonDNA verdict: ADOPT. Zero-cost browser-level XSS defense.
BASE_DECLARE_FEATURE(kAeonSanitizerAPI);

// CSS Scroll-driven Animations (@scroll-timeline)
// AeonDNA verdict: ADOPT. Pure platform feature — no privacy concern.
BASE_DECLARE_FEATURE(kAeonScrollDrivenAnimations);

// Scoped Custom Element Registries
// Prevents cross-component leakage in web apps.
// AeonDNA verdict: ADOPT. Aligns with Aeon's site isolation goals.
BASE_DECLARE_FEATURE(kAeonScopedCustomElementRegistry);

// PDF OCR accessibility (screen readers on scanned PDFs)
// Runs locally via Tesseract — no cloud required.
// AeonDNA verdict: ADOPT. Privacy-neutral, significant accessibility win.
BASE_DECLARE_FEATURE(kAeonPdfOcr);

// Split-screen tabs (two tabs side-by-side in one window)
// AeonDNA verdict: ADOPT as AeonWorkspace tab-split mode.
BASE_DECLARE_FEATURE(kAeonSideBySideTabs);

// WebMCP (Model Context Protocol) — browser↔local-AI tool bridge
// AeonDNA verdict: ADOPT. Gateway for AeonAgent local tool integration.
// NOTE: Restricted to local MCP servers only (no remote MCP endpoints).
BASE_DECLARE_FEATURE(kAeonWebMCP);


// ─────────────────────────────────────────────────────────────────────────────
// STRIPPED from Chrome — Disabled at compile time, no runtime toggle exposed
// ─────────────────────────────────────────────────────────────────────────────

// Gemini AI (cloud assistant — answers, summaries, document drafting)
// Replacement: AeonAgent (local LLM via CTranslate2)
BASE_DECLARE_FEATURE(kAeonGeminiIntegration);   // FORCE DISABLED

// Auto Browse (AI web navigation — subscription gated, sends URLs to Google)
// Replacement: None. AeonAgent operates locally and never reports browsing.
BASE_DECLARE_FEATURE(kAeonAutoBrowse);          // FORCE DISABLED

// Nano Banana (cloud image generation inside Chrome)
// Replacement: None planned for v1. Future: local Stable Diffusion via AeonAgent.
BASE_DECLARE_FEATURE(kAeonCloudImageGen);       // FORCE DISABLED

// Google Safe Browsing server-side AI (sends every URL to Google for scoring)
// Replacement: AeonShield local threat model (heuristic + sovereign blocklist)
BASE_DECLARE_FEATURE(kAeonServerSideSafeBrowsing); // FORCE DISABLED

// Chrome account sync / Google profile
// No replacement needed — sovereign browser needs no accounts.
BASE_DECLARE_FEATURE(kAeonGoogleSync);          // FORCE DISABLED

} // namespace aeon
