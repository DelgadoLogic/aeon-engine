"""
AeonMind Research Agent — aeon_research_agent.py
Runs on GCP Cloud Run on a 6-hour schedule.

PURPOSE:
  Autonomously researches security patches, new web features, and
  AI improvements. Writes findings to GCS for the patch writer.

SOURCES MONITORED:
  - Chromium security advisories (chromium.googlesource.com)
  - CVE database (nvd.nist.gov) for browser-related vulns
  - ungoogled-chromium patches (github.com/ungoogled-software)
  - WHATWG/W3C spec proposals (whatwg.org, w3.org)
  - arXiv AI/ML papers (cs.AI, cs.LG, cs.CR)
  - AeonHive aggregated peer usage signals

COST: ~$0.00/month (Cloud Run free tier covers 6-hour interval jobs)
"""

import os
import json
import time
import hashlib
import datetime
import logging
import requests
from google.cloud import storage
from google.cloud import secretmanager
from typing import Optional

logging.basicConfig(level=logging.INFO)
log = logging.getLogger("AeonResearch")

GCS_BUCKET    = os.environ.get("GCS_BUCKET", "aeon-chromium-artifacts")
OLLAMA_URL    = os.environ.get("OLLAMA_URL", "http://localhost:11434")
OLLAMA_MODEL  = os.environ.get("OLLAMA_MODEL", "deepseek-coder:6.7b")

# ── GCS helpers ────────────────────────────────────────────────────────────

gcs = storage.Client()

def gcs_read_json(path: str, default=None):
    try:
        blob = gcs.bucket(GCS_BUCKET).blob(path)
        return json.loads(blob.download_as_text())
    except Exception:
        return default

def gcs_write_json(path: str, data):
    blob = gcs.bucket(GCS_BUCKET).blob(path)
    blob.upload_from_string(json.dumps(data, indent=2), content_type="application/json")
    log.info(f"[GCS] Wrote {path}")

# ── Source monitors ──────────────────────────────────────────────────────────

def check_chromium_security_advisories() -> list[dict]:
    """Check the Chromium bug tracker for recently fixed security issues."""
    findings = []
    try:
        # Chromium releases blog RSS
        resp = requests.get(
            "https://chromereleases.googleblog.com/feeds/posts/default?alt=json",
            timeout=15)
        if resp.status_code == 200:
            entries = resp.json().get("feed", {}).get("entry", [])
            for entry in entries[:5]:
                title = entry.get("title", {}).get("$t", "")
                if "security" in title.lower() or "stable" in title.lower():
                    findings.append({
                        "source": "chromium_security",
                        "title": title,
                        "url": entry.get("link", [{}])[0].get("href", ""),
                        "priority": "critical" if "security" in title.lower() else "normal",
                        "type": "security_patch"
                    })
    except Exception as e:
        log.warning(f"Chromium advisory check failed: {e}")
    return findings

def check_ungoogled_patches() -> list[dict]:
    """Check ungoogled-chromium for new privacy patches we haven't applied."""
    findings = []
    try:
        resp = requests.get(
            "https://api.github.com/repos/ungoogled-software/ungoogled-chromium/commits",
            headers={"Accept": "application/vnd.github.v3+json"},
            params={"per_page": 10},
            timeout=15)
        if resp.status_code == 200:
            for commit in resp.json():
                msg = commit.get("commit", {}).get("message", "")
                sha = commit.get("sha", "")[:8]
                if any(kw in msg.lower() for kw in ["patch", "privacy", "fix", "remove", "disable"]):
                    findings.append({
                        "source": "ungoogled_chromium",
                        "title": msg[:120],
                        "sha": sha,
                        "url": commit.get("html_url", ""),
                        "priority": "high",
                        "type": "privacy_patch"
                    })
    except Exception as e:
        log.warning(f"ungoogled check failed: {e}")
    return findings

def check_cve_database() -> list[dict]:
    """Check NVD for browser-related CVEs in the last 7 days."""
    findings = []
    try:
        end = datetime.datetime.utcnow()
        start = end - datetime.timedelta(days=7)
        resp = requests.get(
            "https://services.nvd.nist.gov/rest/json/cves/2.0",
            params={
                "keywordSearch": "chromium blink v8",
                "pubStartDate": start.strftime("%Y-%m-%dT00:00:00"),
                "pubEndDate": end.strftime("%Y-%m-%dT23:59:59"),
                "resultsPerPage": 10
            },
            timeout=20)
        if resp.status_code == 200:
            for vuln in resp.json().get("vulnerabilities", []):
                cve = vuln.get("cve", {})
                cve_id = cve.get("id", "")
                desc = cve.get("descriptions", [{}])[0].get("value", "")
                metrics = cve.get("metrics", {})
                score = 0
                if "cvssMetricV31" in metrics:
                    score = metrics["cvssMetricV31"][0].get("cvssData", {}).get("baseScore", 0)
                findings.append({
                    "source": "nvd_cve",
                    "cve_id": cve_id,
                    "title": f"{cve_id}: {desc[:100]}",
                    "score": score,
                    "priority": "critical" if score >= 7.0 else "high",
                    "type": "security_patch"
                })
    except Exception as e:
        log.warning(f"CVE check failed: {e}")
    return findings

def check_hive_pain_points() -> list[dict]:
    """
    Read aggregated (anonymous) hive signals from GCS.
    AeonHive peers write crash counts, slow page signals, etc.
    to gs://aeon-chromium-artifacts/hive/signals.json
    """
    signals = gcs_read_json("hive/signals.json", {})
    findings = []
    for key, count in signals.items():
        if count > 5:  # More than 5 peers reported it
            findings.append({
                "source": "aeon_hive",
                "title": f"Hive signal: {key} (reported by {count} peers)",
                "signal_key": key,
                "count": count,
                "priority": "high" if count > 20 else "normal",
                "type": "ux_improvement"
            })
    return findings

def check_arxiv_ai_papers() -> list[dict]:
    """Check arXiv for relevant papers on browser security, privacy, and local AI."""
    findings = []
    try:
        resp = requests.get(
            "http://export.arxiv.org/api/query",
            params={
                "search_query": "ti:browser+OR+ti:privacy+OR+ti:fingerprint+AND+cat:cs.CR",
                "sortBy": "submittedDate",
                "sortOrder": "descending",
                "max_results": 5
            },
            timeout=15)
        if resp.status_code == 200:
            # Parse Atom XML (simplified)
            import xml.etree.ElementTree as ET
            root = ET.fromstring(resp.text)
            ns = {"atom": "http://www.w3.org/2005/Atom"}
            for entry in root.findall("atom:entry", ns):
                title = entry.findtext("atom:title", namespaces=ns, default="").strip()
                link = entry.find("atom:link[@rel='alternate']", ns)
                url = link.attrib.get("href", "") if link is not None else ""
                findings.append({
                    "source": "arxiv",
                    "title": title,
                    "url": url,
                    "priority": "low",
                    "type": "research_insight"
                })
    except Exception as e:
        log.warning(f"arXiv check failed: {e}")
    return findings

# ── Prioritization via Ollama ─────────────────────────────────────────────

def rank_findings_with_ai(findings: list[dict]) -> list[dict]:
    """
    Use Ollama locally (or a Cloud Run Ollama sidecar) to rank and
    deduplicate findings, and suggest which to act on this cycle.
    """
    if not findings:
        return []

    summary = "\n".join([f"- [{f['type']}] {f['title']}" for f in findings[:30]])
    prompt = f"""You are the Aeon Browser autonomous research agent.
Here are recent findings about browser security, privacy, and features:

{summary}

Rank these by importance for a privacy-first browser. Return ONLY a JSON list
of ranks like: [{{"index": 0, "score": 10, "action": "apply_patch"}}, ...]
Actions: apply_patch | research_more | implement_feature | skip
Focus on: security > privacy > performance > features.
JSON only, no explanation."""

    try:
        resp = requests.post(
            f"{OLLAMA_URL}/api/generate",
            json={"model": OLLAMA_MODEL, "prompt": prompt, "stream": False},
            timeout=60)
        text = resp.json().get("response", "[]")
        # Extract JSON from response
        start = text.find("[")
        end = text.rfind("]") + 1
        if start >= 0 and end > start:
            ranks = json.loads(text[start:end])
            # Apply scores to findings
            for rank in ranks:
                idx = rank.get("index", -1)
                if 0 <= idx < len(findings):
                    findings[idx]["ai_score"] = rank.get("score", 0)
                    findings[idx]["ai_action"] = rank.get("action", "skip")
            # Sort by AI score
            findings.sort(key=lambda f: f.get("ai_score", 0), reverse=True)
    except Exception as e:
        log.warning(f"AI ranking failed: {e} — using raw priority order")

    return findings

# ── Main research cycle ───────────────────────────────────────────────────

def run_research_cycle():
    log.info("=== Aeon Research Agent: cycle starting ===")
    
    all_findings = []
    all_findings += check_chromium_security_advisories()
    all_findings += check_cve_database()
    all_findings += check_ungoogled_patches()
    all_findings += check_hive_pain_points()
    all_findings += check_arxiv_ai_papers()
    
    log.info(f"Found {len(all_findings)} raw findings")
    
    # Deduplicate by title hash
    seen = set()
    deduped = []
    for f in all_findings:
        key = hashlib.md5(f["title"].encode()).hexdigest()[:8]
        if key not in seen:
            seen.add(key)
            f["id"] = key
            deduped.append(f)
    
    log.info(f"Deduplicated to {len(deduped)} findings")
    
    # AI ranking
    ranked = rank_findings_with_ai(deduped)
    
    # Merge with existing queue (don't duplicate already-queued items)
    existing_queue = gcs_read_json("research/queue.json", {"items": [], "last_run": None})
    existing_ids = {item["id"] for item in existing_queue.get("items", [])}
    
    new_items = [f for f in ranked if f.get("id") not in existing_ids
                 and f.get("ai_action", "skip") != "skip"]
    
    existing_queue["items"] = new_items + existing_queue.get("items", [])
    existing_queue["items"] = existing_queue["items"][:50]  # Keep top 50
    existing_queue["last_run"] = datetime.datetime.utcnow().isoformat()
    existing_queue["total_found_this_cycle"] = len(all_findings)
    
    gcs_write_json("research/queue.json", existing_queue)
    
    # Write summary for aeon://self dashboard
    summary = {
        "last_research": datetime.datetime.utcnow().isoformat(),
        "findings_this_cycle": len(all_findings),
        "queued_for_patching": len(new_items),
        "top_priority": ranked[0]["title"] if ranked else "None",
        "critical_count": len([f for f in ranked if f.get("priority") == "critical"])
    }
    gcs_write_json("research/latest_summary.json", summary)
    
    log.info(f"=== Research cycle complete. Queued {len(new_items)} new items ===")
    
    # If there are critical security patches → immediately trigger build
    critical = [f for f in new_items if f.get("priority") == "critical"]
    if critical:
        log.info(f"CRITICAL: {len(critical)} critical findings — triggering emergency build")
        gcs_write_json("build/emergency_trigger.json", {
            "triggered_by": "research_agent",
            "reason": f"{len(critical)} critical CVEs found",
            "timestamp": datetime.datetime.utcnow().isoformat(),
            "items": critical
        })

if __name__ == "__main__":
    run_research_cycle()
