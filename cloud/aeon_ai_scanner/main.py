"""
Aeon AI Security Scanner + Censorship Intelligence — Cloud Run Service
═══════════════════════════════════════════════════════════════════════
Uses Vertex AI (Gemini) to:
  1. Analyze CVEs for Aeon browser security
  2. Generate censorship intelligence reports
  3. Recommend bypass strategies per country/network profile
  4. Track global internet freedom indicators

Endpoints:
  POST /analyze-cve           → Analyze a CVE for Aeon relevance
  POST /batch-analyze         → Batch analyze multiple CVEs
  GET  /report                → Weekly security report
  POST /analyze-censorship    → Analyze a country's censorship landscape
  POST /strategy-recommend    → AI-recommended bypass strategy for a network profile
  GET  /freedom-report        → Global internet freedom assessment
  GET  /health                → Health check
"""

import os
import json
import re
import logging
from datetime import datetime, timezone

from fastapi import FastAPI, HTTPException, Query
from fastapi.middleware.cors import CORSMiddleware
from pydantic import BaseModel
import vertexai
from vertexai.generative_models import GenerativeModel, GenerationConfig

# ── Config ──────────────────────────────────────────────────────────────
PROJECT_ID = os.getenv("GCP_PROJECT", "aeon-browser-build")
REGION = os.getenv("GCP_REGION", "us-central1")
PORT = int(os.getenv("PORT", 8080))
MODEL_ID = os.getenv("GEMINI_MODEL", "gemini-2.5-flash")

# ── Logging ─────────────────────────────────────────────────────────────
logging.basicConfig(level=logging.INFO, format="%(asctime)s [%(levelname)s] %(message)s")
logger = logging.getLogger("aeon-ai-scanner")

# ── Initialize Vertex AI ───────────────────────────────────────────────
vertexai.init(project=PROJECT_ID, location=REGION)

# ── App ─────────────────────────────────────────────────────────────────
app = FastAPI(
    title="Aeon AI Security & Censorship Intelligence",
    description="Vertex AI-powered CVE analysis + censorship intelligence for Aeon Browser",
    version="2.0.0",
)

app.add_middleware(
    CORSMiddleware,
    allow_origins=["*"],
    allow_methods=["*"],
    allow_headers=["*"],
)


# ── Models ──────────────────────────────────────────────────────────────

class CVEAnalysisRequest(BaseModel):
    cve_id: str
    description: str
    affected_component: str = "chromium"
    cvss_score: float = 0.0

class BatchCVERequest(BaseModel):
    cves: list[CVEAnalysisRequest]

class CensorshipAnalysisRequest(BaseModel):
    country_code: str
    country_name: str = ""
    focus_areas: list[str] = []

class StrategyRecommendRequest(BaseModel):
    network_profile: str  # open, captive_portal, content_filter, gfw_moderate, gfw_severe
    country_code: str = "unknown"
    blocked_domains: list[str] = []
    dns_poisoned: bool = False
    dpi_detected: bool = False
    doh_works: bool = True
    tor_reachable: bool = True


# ── Prompts ─────────────────────────────────────────────────────────────

AEON_ANALYSIS_PROMPT = """You are a senior security researcher analyzing CVEs for the Aeon Browser project.

Aeon is a sovereign browser built from scratch using these components:
- Blink rendering engine (stripped Chromium fork, WebView2 on Windows)
- V8 JavaScript engine
- Custom networking stack (WolfSSL for legacy, native TLS 1.3 for modern)
- Custom content blocker engine
- Ed25519-signed auto-updater
- 8-tier OS support (Windows 3.1 through Windows 11)
- P2P update distribution via AeonHive (Libp2p/GossipSub)
- Local AI inference via CTranslate2 (phi-3-mini)

Analyze the following CVE:
- ID: {cve_id}
- Description: {description}
- Affected Component: {affected_component}
- CVSS Score: {cvss_score}

Return ONLY valid JSON with these keys:
- "relevance_score": integer 0-10
- "affected_aeon_components": list of strings
- "patch_strategy": string with detailed patch plan
- "priority": one of "CRITICAL", "HIGH", "MEDIUM", "LOW", "NOT_APPLICABLE"
- "estimated_hours": number
- "aeon_specific_notes": string
- "tier_impact": object mapping affected tier names to impact description
- "recommended_tests": list of test descriptions"""


CENSORSHIP_ANALYSIS_PROMPT = """You are an expert in internet censorship, digital rights, and circumvention technology. Analyze the current censorship landscape for {country_name} ({country_code}).

Provide a comprehensive assessment covering:

1. **Censorship Infrastructure**:
   - What filtering technologies are deployed? (DPI, DNS poisoning, IP blocking, SNI filtering, etc.)
   - How sophisticated is the detection of circumvention tools?
   - Are there known government partnerships with DPI vendors (like Huawei, ZTE, Sandvine)?

2. **Blocked Services**:
   - Which major platforms are blocked? (Google, YouTube, Facebook, Twitter, WhatsApp, Telegram, Wikipedia, etc.)
   - Are VPN protocols specifically targeted?
   - Is there keyword filtering or content-based blocking?

3. **Current Working Bypass Methods**:
   - Which circumvention techniques currently work in this country?
   - Rate each: VLESS+REALITY, V2Ray/VMess, Shadowsocks, Tor, MEEK, Domain Fronting, DoH, ECH
   - What is the risk level for users caught using circumvention tools?

4. **Aeon-Specific Recommendations**:
   - Which AeonCE strategies should be prioritized for this country?
   - Recommended relay regions (which countries' IPs are not blocked?)
   - Specific protocol configurations that work best

{focus_areas_section}

Return as valid JSON with keys:
- "country_code": string
- "censorship_level": one of "NONE", "LOW", "MODERATE", "HIGH", "SEVERE", "TOTAL"
- "infrastructure": object describing filtering tech
- "blocked_services": list of blocked platform names
- "working_methods": object mapping method name to effectiveness rating 0-10
- "risk_level": one of "NONE", "LOW", "MODERATE", "HIGH", "EXTREME"
- "aeon_strategy_order": ordered list of strategies for this country
- "recommended_relay_regions": list of country codes for relay exit nodes
- "protocol_configs": object with specific configuration recommendations
- "notes": string with additional intelligence"""


STRATEGY_RECOMMEND_PROMPT = """You are an expert in internet censorship circumvention. Given the following network diagnostic data, recommend the optimal bypass strategy for the Aeon Browser's Circumvention Engine (AeonCE).

Network Profile: {network_profile}
Country: {country_code}
DNS Poisoned: {dns_poisoned}
DPI Detected: {dpi_detected}
DoH Works: {doh_works}
Tor Reachable: {tor_reachable}
Blocked Domains Detected: {blocked_domains}

Available AeonCE strategies (in order of preference when all work):
1. direct — No proxy, fastest
2. doh_sovereign — DNS-over-HTTPS via AeonDNS (bypasses DNS poisoning)
3. ech — Encrypted Client Hello (defeats SNI filtering)
4. ws_tunnel — WebSocket tunnel via AeonRelay (looks like normal HTTPS)
5. vless_reality — VLESS+REALITY protocol (mimics legitimate TLS, gold standard vs DPI)
6. shadowsocks — Encrypted SOCKS5 proxy (looks like random data)
7. meek — Traffic disguised as Azure/Google traffic (slow, last resort)
8. tor — Tor onion routing (slowest, most private)

Return as valid JSON with keys:
- "recommended_order": ordered list of strategy names to try
- "primary_strategy": the single best strategy for this profile
- "reasoning": string explaining why
- "estimated_success_rate": percentage 0-100
- "fallback_chain": ordered list of fallbacks if primary fails
- "config_hints": object with per-strategy configuration tips
- "warnings": list of risk warnings"""


FREEDOM_REPORT_PROMPT = """Generate a comprehensive Global Internet Freedom Report for the Aeon Browser development team.

This report will be used to prioritize which countries' users need the most help accessing the open internet. For each country assessed, provide:

1. **Freedom Score** (0-100, where 0 = completely free, 100 = totalitarian control)
2. **Censorship techniques** used
3. **What's blocked** (major platforms and services)
4. **What circumvention methods work** right now
5. **Aeon priority** (how urgently should we optimize for this country)

Cover at minimum these 20 countries:
China, Russia, Iran, North Korea, Myanmar, Turkmenistan, Eritrea, Saudi Arabia, UAE, Vietnam,
Turkey, Egypt, Pakistan, Cuba, Ethiopia, Belarus, Venezuela, Thailand, Indonesia, India

Also include:
- **Global trends**: Is internet freedom improving or declining?
- **Emerging threats**: New censorship technologies being deployed
- **Opportunities**: Countries where Aeon could have the most impact
- **Infrastructure recommendations**: Where should AeonRelay nodes be deployed?

Format as a professional intelligence briefing in markdown."""


WEEKLY_REPORT_PROMPT = """Generate a comprehensive weekly security intelligence report for the Aeon Browser development team.

Context about Aeon Browser:
- Sovereign browser built from scratch, not a Chromium fork
- Uses WebView2 (Blink) on Windows 10/11, custom renderers for legacy OS
- Custom V8 integration for JavaScript execution
- WolfSSL for legacy TLS, native TLS 1.3 for modern platforms
- Ed25519-signed autonomous update system
- P2P compute network (AeonHive) for distributed builds
- 8-tier OS support from Windows 3.1 to Windows 11
- Local AI via CTranslate2 (phi-3-mini), zero cloud telemetry
- AeonShield circumvention infrastructure (DoH, WebSocket relay, VLESS+REALITY)

Generate a report covering:
1. **Executive Summary**: 2-3 sentence overview of the week's browser security landscape
2. **Critical CVEs**: Top 10 CVEs affecting Chromium, Blink, V8, Skia, WebRTC, or WebView2
3. **Attack Vector Analysis**: Emerging techniques targeting browser engines
4. **Privacy Threat Intelligence**: New tracking methods, fingerprinting techniques
5. **Censorship Updates**: Changes in blocking patterns across China, Russia, Iran, and others
6. **Legacy Platform Risks**: Security considerations for Windows XP/Vista/7 users
7. **P2P/Update Security**: Risks to peer-to-peer update distribution or code signing
8. **AI/ML Security**: Threats related to local model inference
9. **Recommended Actions**: Prioritized sprint items for the development team
10. **Competitive Intelligence**: Notable moves by Chrome, Firefox, Brave, or Arc

Format as a professional security briefing in markdown."""


# ── Helpers ─────────────────────────────────────────────────────────────

def _parse_json_response(text: str) -> dict:
    """Extract JSON from Gemini response."""
    clean = text.strip()
    if clean.startswith("```"):
        first_nl = clean.index("\n")
        last_fence = clean.rfind("```")
        if last_fence > first_nl:
            clean = clean[first_nl + 1:last_fence].strip()
    try:
        return json.loads(clean)
    except json.JSONDecodeError:
        match = re.search(r'\{[\s\S]*\}', clean)
        if match:
            try:
                return json.loads(match.group())
            except json.JSONDecodeError:
                pass
        return {"raw_analysis": text}


def _get_model() -> GenerativeModel:
    return GenerativeModel(MODEL_ID)


def _json_config(max_tokens: int = 2048, temp: float = 0.2) -> GenerationConfig:
    return GenerationConfig(
        max_output_tokens=max_tokens,
        temperature=temp,
        top_p=0.8,
        response_mime_type="application/json",
    )


def _text_config(max_tokens: int = 8192, temp: float = 0.4) -> GenerationConfig:
    return GenerationConfig(
        max_output_tokens=max_tokens,
        temperature=temp,
        top_p=0.9,
    )


def _token_counts(response) -> dict:
    return {
        "prompt": response.usage_metadata.prompt_token_count if response.usage_metadata else 0,
        "completion": response.usage_metadata.candidates_token_count if response.usage_metadata else 0,
    }


# ── Routes: CVE Analysis ───────────────────────────────────────────────

@app.get("/health")
async def health():
    return {
        "status": "operational",
        "service": "aeon-ai-scanner",
        "version": "2.0.0",
        "model": MODEL_ID,
        "capabilities": ["cve_analysis", "censorship_intelligence", "strategy_recommendation", "freedom_report"],
        "timestamp": datetime.now(timezone.utc).isoformat(),
    }


@app.post("/analyze-cve")
async def analyze_cve(req: CVEAnalysisRequest):
    """Analyze a CVE using Vertex AI Gemini for Aeon relevance."""
    try:
        model = _get_model()
        prompt = AEON_ANALYSIS_PROMPT.format(
            cve_id=req.cve_id, description=req.description,
            affected_component=req.affected_component, cvss_score=req.cvss_score,
        )
        response = model.generate_content(prompt, generation_config=_json_config())
        return {
            "status": "analyzed", "cve_id": req.cve_id, "model": MODEL_ID,
            "analysis": _parse_json_response(response.text),
            "tokens_used": _token_counts(response),
            "timestamp": datetime.now(timezone.utc).isoformat(),
        }
    except Exception as e:
        logger.error(f"CVE analysis failed: {e}")
        raise HTTPException(status_code=500, detail=str(e))


@app.post("/batch-analyze")
async def batch_analyze(req: BatchCVERequest):
    """Batch analyze multiple CVEs."""
    results = []
    total_p, total_c = 0, 0
    model = _get_model()

    for cve in req.cves:
        try:
            prompt = AEON_ANALYSIS_PROMPT.format(
                cve_id=cve.cve_id, description=cve.description,
                affected_component=cve.affected_component, cvss_score=cve.cvss_score,
            )
            response = model.generate_content(prompt, generation_config=_json_config())
            tokens = _token_counts(response)
            total_p += tokens["prompt"]
            total_c += tokens["completion"]
            results.append({
                "cve_id": cve.cve_id, "status": "analyzed",
                "analysis": _parse_json_response(response.text),
                "tokens": tokens,
            })
        except Exception as e:
            results.append({"cve_id": cve.cve_id, "status": "error", "error": str(e)})

    return {
        "status": "batch_complete", "model": MODEL_ID,
        "total_analyzed": len([r for r in results if r["status"] == "analyzed"]),
        "total_errors": len([r for r in results if r["status"] == "error"]),
        "total_tokens": {"prompt": total_p, "completion": total_c, "total": total_p + total_c},
        "results": results,
        "timestamp": datetime.now(timezone.utc).isoformat(),
    }


# ── Routes: Censorship Intelligence ────────────────────────────────────

@app.post("/analyze-censorship")
async def analyze_censorship(req: CensorshipAnalysisRequest):
    """Analyze a country's censorship landscape using Gemini."""
    try:
        model = _get_model()

        focus_section = ""
        if req.focus_areas:
            focus_section = f"\nFocus especially on: {', '.join(req.focus_areas)}"

        country_name = req.country_name or req.country_code.upper()
        prompt = CENSORSHIP_ANALYSIS_PROMPT.format(
            country_name=country_name,
            country_code=req.country_code,
            focus_areas_section=focus_section,
        )

        response = model.generate_content(prompt, generation_config=_json_config(max_tokens=4096))
        logger.info(f"Censorship analysis complete: {req.country_code}")

        return {
            "status": "analyzed",
            "country_code": req.country_code,
            "model": MODEL_ID,
            "analysis": _parse_json_response(response.text),
            "tokens_used": _token_counts(response),
            "timestamp": datetime.now(timezone.utc).isoformat(),
        }
    except Exception as e:
        logger.error(f"Censorship analysis failed: {e}")
        raise HTTPException(status_code=500, detail=str(e))


@app.post("/strategy-recommend")
async def strategy_recommend(req: StrategyRecommendRequest):
    """AI-recommended bypass strategy for a network profile."""
    try:
        model = _get_model()
        prompt = STRATEGY_RECOMMEND_PROMPT.format(
            network_profile=req.network_profile,
            country_code=req.country_code,
            dns_poisoned=req.dns_poisoned,
            dpi_detected=req.dpi_detected,
            doh_works=req.doh_works,
            tor_reachable=req.tor_reachable,
            blocked_domains=", ".join(req.blocked_domains) if req.blocked_domains else "none detected",
        )

        response = model.generate_content(prompt, generation_config=_json_config(max_tokens=2048))
        logger.info(f"Strategy recommendation complete: {req.network_profile}/{req.country_code}")

        return {
            "status": "recommended",
            "network_profile": req.network_profile,
            "country_code": req.country_code,
            "model": MODEL_ID,
            "recommendation": _parse_json_response(response.text),
            "tokens_used": _token_counts(response),
            "timestamp": datetime.now(timezone.utc).isoformat(),
        }
    except Exception as e:
        logger.error(f"Strategy recommendation failed: {e}")
        raise HTTPException(status_code=500, detail=str(e))


@app.get("/freedom-report")
async def freedom_report():
    """Generate a global internet freedom assessment using Gemini."""
    try:
        model = _get_model()
        response = model.generate_content(FREEDOM_REPORT_PROMPT, generation_config=_text_config())
        logger.info("Global freedom report generated")
        return {
            "status": "generated",
            "report": response.text,
            "model": MODEL_ID,
            "tokens_used": _token_counts(response),
            "timestamp": datetime.now(timezone.utc).isoformat(),
        }
    except Exception as e:
        logger.error(f"Freedom report generation failed: {e}")
        raise HTTPException(status_code=500, detail=str(e))


@app.get("/report")
async def weekly_report():
    """Generate a weekly security intelligence report using Gemini."""
    try:
        model = _get_model()
        response = model.generate_content(WEEKLY_REPORT_PROMPT, generation_config=_text_config())
        logger.info("Weekly security report generated")
        return {
            "status": "generated",
            "report": response.text,
            "model": MODEL_ID,
            "tokens_used": _token_counts(response),
            "timestamp": datetime.now(timezone.utc).isoformat(),
        }
    except Exception as e:
        logger.error(f"Report generation failed: {e}")
        raise HTTPException(status_code=500, detail=str(e))


# ── Main ────────────────────────────────────────────────────────────────

if __name__ == "__main__":
    import uvicorn
    uvicorn.run(app, host="0.0.0.0", port=PORT)
