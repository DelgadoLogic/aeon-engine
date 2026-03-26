"""
aeon_search.py — Aeon Search layer
Provides:
  1. Default search provider management (DuckDuckGo default, user-selectable)
  2. AI-enhanced result summarization via AeonMind (local REST on :8765)
  3. Privacy-respecting affiliate link injection for commercial queries
  4. Search deal analytics (anonymous, opt-in — for partner reporting)

This runs as a lightweight local service on port 8766.
The browser's omnibox calls it instead of hitting a search engine directly.
This is what makes our search engine partner deals valuable:
  - We control the search gateway
  - We can route to different backends
  - We augment results with AI (competitive advantage)
  - We track zero PII

Flow:
  Browser address bar → aeon_search.py:8766 → [{search provider} + AeonMind] → results page
"""

import os
import re
import json
import time
import hashlib
import logging
import urllib.request
import urllib.parse
from pathlib import Path
from http.server import HTTPServer, BaseHTTPRequestHandler
from typing import Optional

log = logging.getLogger("AeonSearch")

# ── Config ────────────────────────────────────────────────────────────────────

AEONMIND_URL = "http://127.0.0.1:8765"   # Local AeonMind REST
HIVE_URL     = "http://127.0.0.1:7878"   # AeonHive
SEARCH_PORT  = 8766
AEON_DATA    = Path(os.environ.get("AEON_DATA",
                 str(Path.home() / "AppData" / "Local" / "Aeon")))
PREFS_FILE   = AEON_DATA / "search_prefs.json"

# ── Search providers ──────────────────────────────────────────────────────────

PROVIDERS = {
    "ddg": {
        "name":    "DuckDuckGo",
        "search":  "https://duckduckgo.com/?q={query}&kp=-2&kl=us-en",
        "api":     "https://api.duckduckgo.com/?q={query}&format=json&no_html=1",
        "logo":    "🦆",
        "private": True
    },
    "bing": {
        "name":    "Bing",
        "search":  "https://www.bing.com/search?q={query}",
        "api":     None,   # No free API — redirect only
        "logo":    "🔷",
        "private": False
    },
    "brave": {
        "name":    "Brave Search",
        "search":  "https://search.brave.com/search?q={query}&source=web",
        "api":     "https://api.search.brave.com/res/v1/web/search?q={query}",
        "logo":    "🦁",
        "private": True
    },
    "ecosia": {
        "name":    "Ecosia",
        "search":  "https://www.ecosia.org/search?q={query}",
        "api":     None,
        "logo":    "🌱",
        "private": True
    }
}

DEFAULT_PROVIDER = "ddg"

# ── Affiliate patterns ────────────────────────────────────────────────────────
# These are matched against search queries to detect commercial intent.
# When detected, the search results page includes an affiliate link notice.
# Privacy-respecting: no tracking, links are contextual, not behavioral.

AFFILIATE_PATTERNS = {
    "amazon": {
        "keywords": ["buy", "cheap", "deal", "price", "best", "review",
                     "vs", "alternatives to", "where to get"],
        "domains":  ["amazon.com", "amzn.to"],
        "tag":      "aeonbrowser-20",   # Your Amazon Associates tag
        "template": "https://www.amazon.com/s?k={query}&tag=aeonbrowser-20"
    },
    "ebay": {
        "keywords": ["buy", "used", "refurbished", "listing"],
        "domains":  ["ebay.com"],
        "tag":      "aeonbrowser",
        "template": "https://www.ebay.com/sch/i.html?_nkw={query}&mkevt=1&mkcid=1&mkrid=711-53200-19255-0&campid=aeonbrowser"
    }
}

# ── Commercial intent detection ───────────────────────────────────────────────

def detect_commercial_intent(query: str) -> Optional[dict]:
    """
    Returns affiliate info if the query has clear commercial buying intent.
    Privacy note: this runs 100% locally. Query never leaves the device for this check.
    """
    q_lower = query.lower()
    
    for retailer, config in AFFILIATE_PATTERNS.items():
        kw_match = any(kw in q_lower for kw in config["keywords"])
        domain_match = any(d in q_lower for d in config["domains"])
        
        if kw_match or domain_match:
            encoded = urllib.parse.quote_plus(query)
            return {
                "retailer": retailer,
                "url": config["template"].format(query=encoded),
                "label": f"Shop on {retailer.title()}"
            }
    return None

# ── AeonMind integration ──────────────────────────────────────────────────────

def aeonmind_summarize(query: str, context: str = "") -> Optional[str]:
    """
    Ask local AeonMind to provide a brief AI answer for the query.
    Returns None if AeonMind is unavailable (fails gracefully).
    """
    try:
        payload = json.dumps({
            "prompt": f"Answer this search query concisely in 2-3 sentences: {query}",
            "context": context,
            "max_tokens": 150,
            "temperature": 0.3
        }).encode()
        
        req = urllib.request.Request(
            f"{AEONMIND_URL}/generate",
            data=payload,
            headers={"Content-Type": "application/json"},
            method="POST"
        )
        with urllib.request.urlopen(req, timeout=3) as r:
            result = json.loads(r.read())
            return result.get("response", "").strip() or None
    except Exception:
        return None  # AeonMind offline or slow — just show search results normally

# ── DDG instant answers ───────────────────────────────────────────────────────

def fetch_ddg_instant(query: str) -> Optional[dict]:
    """
    DuckDuckGo Instant Answer API — free, no key required.
    Good for factual queries (definitions, quick facts, etc.)
    """
    try:
        encoded = urllib.parse.quote_plus(query)
        url = f"https://api.duckduckgo.com/?q={encoded}&format=json&no_html=1"
        with urllib.request.urlopen(url, timeout=4) as r:
            data = json.loads(r.read())
        
        # DDG returns empty strings when no instant answer
        abstract = data.get("AbstractText", "").strip()
        answer   = data.get("Answer",       "").strip()
        
        if abstract:
            return {
                "text":   abstract,
                "source": data.get("AbstractSource", "Wikipedia"),
                "url":    data.get("AbstractURL", "")
            }
        elif answer:
            return {"text": answer, "source": "DuckDuckGo", "url": ""}
        
        return None
    except Exception:
        return None

# ── Search result page builder ────────────────────────────────────────────────

def build_search_page(query: str, provider: str,
                       ai_answer: Optional[str],
                       instant: Optional[dict],
                       affiliate: Optional[dict],
                       redirect_url: str) -> str:
    """
    Returns an HTML "launch page" that:
    1. Shows the AI answer (if AeonMind is available)
    2. Shows DDG instant answer (if available)
    3. Shows affiliate shopping suggestion (if commercial intent)
    4. Immediately redirects to the actual search engine
    """
    
    ai_section = ""
    if ai_answer:
        ai_section = f"""
        <div class="card ai-card">
          <div class="card-label">⚡ AeonMind</div>
          <p class="ai-text">{ai_answer}</p>
        </div>"""
    
    instant_section = ""
    if instant:
        src = f'<a href="{instant["url"]}" target="_blank">{instant["source"]}</a>' if instant.get("url") else instant.get("source","")
        instant_section = f"""
        <div class="card instant-card">
          <div class="card-label">📖 {src}</div>
          <p>{instant["text"]}</p>
        </div>"""
    
    affiliate_section = ""
    if affiliate:
        affiliate_section = f"""
        <div class="card affiliate-card">
          <div class="card-label">🛒 Shopping</div>
          <a href="{affiliate['url']}" target="_blank" class="affiliate-btn">
            {affiliate['label']} →
          </a>
          <p class="affiliate-note">Affiliate link — supports Aeon at no cost to you</p>
        </div>"""
    
    providers_html = "".join([
        f'<a href="?q={urllib.parse.quote_plus(query)}&p={k}" class="provider-btn {"active" if k == provider else ""}">'
        f'{v["logo"]} {v["name"]}</a>'
        for k, v in PROVIDERS.items()
    ])
    
    return f"""<!DOCTYPE html>
<html lang="en">
<head>
<meta charset="UTF-8">
<title>Aeon Search — {query}</title>
<meta http-equiv="refresh" content="1; url={redirect_url}">
<link href="https://fonts.googleapis.com/css2?family=Inter:wght@400;500;600&display=swap" rel="stylesheet">
<style>
  *, *::before, *::after {{ box-sizing: border-box; margin: 0; padding: 0; }}
  body {{
    background: #04050a; color: #e8eaf6;
    font-family: 'Inter', sans-serif; line-height: 1.6;
    min-height: 100vh; display: flex; flex-direction: column;
    align-items: center; padding: 48px 24px;
  }}
  h1 {{ font-size: 13px; font-weight: 600; color: #8892b0; margin-bottom: 24px; letter-spacing: 1px; text-transform: uppercase; }}
  .query {{ font-size: 24px; font-weight: 700; margin-bottom: 24px; color: #e8eaf6; }}
  .providers {{ display: flex; gap: 8px; flex-wrap: wrap; margin-bottom: 32px; }}
  .provider-btn {{ background: #111827; border: 1px solid rgba(99,130,255,0.15);
    border-radius: 8px; padding: 8px 16px; font-size: 13px; color: #8892b0;
    text-decoration: none; transition: border-color .2s; }}
  .provider-btn.active {{ border-color: #6382ff; color: #6382ff; }}
  .provider-btn:hover {{ border-color: rgba(99,130,255,0.4); }}
  .cards {{ display: flex; flex-direction: column; gap: 16px; max-width: 640px; width: 100%; }}
  .card {{ background: #080b14; border: 1px solid rgba(99,130,255,0.12);
    border-radius: 12px; padding: 20px 24px; }}
  .card-label {{ font-size: 11px; font-weight: 700; letter-spacing: 1.5px;
    text-transform: uppercase; color: #8892b0; margin-bottom: 10px; }}
  .ai-text {{ font-size: 16px; color: #e8eaf6; line-height: 1.7; }}
  .affiliate-btn {{ display: inline-block; margin-top: 8px; color: #6382ff;
    text-decoration: none; font-weight: 600; font-size: 15px; }}
  .affiliate-note {{ font-size: 11px; color: #4a5568; margin-top: 8px; }}
  .redirect-note {{ margin-top: 32px; font-size: 13px; color: #4a5568; }}
  .redirect-note a {{ color: #6382ff; }}
</style>
</head>
<body>
<h1>Aeon Search</h1>
<div class="query">"{query}"</div>
<div class="providers">{providers_html}</div>
<div class="cards">
  {ai_section}
  {instant_section}
  {affiliate_section}
</div>
<p class="redirect-note">Redirecting to {PROVIDERS.get(provider, PROVIDERS['ddg'])['name']}... <a href="{redirect_url}">Go now</a></p>
</body>
</html>"""

# ── HTTP handler ──────────────────────────────────────────────────────────────

class SearchHandler(BaseHTTPRequestHandler):
    def log_message(self, fmt, *args): pass
    
    def _prefs(self) -> dict:
        if PREFS_FILE.exists():
            try: return json.loads(PREFS_FILE.read_text())
            except: pass
        return {"provider": DEFAULT_PROVIDER}
    
    def _save_prefs(self, prefs: dict):
        PREFS_FILE.write_text(json.dumps(prefs))
    
    def do_GET(self):
        parsed = urllib.parse.urlparse(self.path)
        params = dict(urllib.parse.parse_qsl(parsed.query))
        
        if parsed.path == "/health":
            self._json({"ok": True})
            return
        
        if parsed.path == "/prefs":
            self._json(self._prefs())
            return
        
        if parsed.path != "/search" or "q" not in params:
            self._json({"error": "use /search?q=..."}, 400)
            return
        
        query    = params["q"].strip()
        prefs    = self._prefs()
        provider = params.get("p", prefs.get("provider", DEFAULT_PROVIDER))
        if provider not in PROVIDERS:
            provider = DEFAULT_PROVIDER
        
        # Build the real search URL
        encoded  = urllib.parse.quote_plus(query)
        redirect = PROVIDERS[provider]["search"].format(query=encoded)
        
        # Run enrichment in parallel (fire and hope — all optional)
        import threading
        results  = {"ai": None, "instant": None, "affiliate": None}
        
        def get_ai():
            results["ai"] = aeonmind_summarize(query)
        def get_instant():
            if provider == "ddg":
                results["instant"] = fetch_ddg_instant(query)
        def get_affiliate():
            results["affiliate"] = detect_commercial_intent(query)
        
        threads = [threading.Thread(target=f, daemon=True)
                   for f in [get_ai, get_instant, get_affiliate]]
        for t in threads: t.start()
        for t in threads: t.join(timeout=3)  # Max 3s for enrichment
        
        # Build and serve the intermediate page
        page = build_search_page(
            query, provider,
            results["ai"], results["instant"], results["affiliate"],
            redirect
        ).encode()
        
        self.send_response(200)
        self.send_header("Content-Type", "text/html; charset=utf-8")
        self.send_header("Content-Length", str(len(page)))
        self.end_headers()
        self.wfile.write(page)
    
    def do_POST(self):
        parsed = urllib.parse.urlparse(self.path)
        if parsed.path == "/prefs":
            length = int(self.headers.get("Content-Length", 0))
            body   = json.loads(self.rfile.read(length)) if length else {}
            prefs  = self._prefs()
            prefs.update(body)
            self._save_prefs(prefs)
            self._json({"ok": True})
        else:
            self._json({"error": "not found"}, 404)
    
    def _json(self, data, status=200):
        body = json.dumps(data).encode()
        self.send_response(status)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

# ── Entry point ───────────────────────────────────────────────────────────────

if __name__ == "__main__":
    logging.basicConfig(level=logging.INFO, format="%(asctime)s %(message)s")
    AEON_DATA.mkdir(parents=True, exist_ok=True)
    srv = HTTPServer(("127.0.0.1", SEARCH_PORT), SearchHandler)
    log.info(f"[AeonSearch] Listening on :{SEARCH_PORT}")
    srv.serve_forever()
