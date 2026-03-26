#!/usr/bin/env python3
"""
AeonResearchAgent — Autonomous CVE & vulnerability scout.
Runs 24/7 on GCP Cloud Run. Scans:
  - NVD/NIST CVE feed (Chromium, V8, Blink, libwebp, etc.)
  - Google Project Zero advisories
  - GitHub Security Advisories (chromium/chromium)
  - arXiv: browser security, memory safety, fingerprinting
Reports findings to AeonHive vote ledger.
"""
import asyncio
import json
import os
import re
import time
import hashlib
import httpx
from datetime import datetime, timedelta, timezone
from typing import Optional

GCP_PROJECT   = os.getenv("GCP_PROJECT", "aeon-browser-build")
HIVE_ENDPOINT = os.getenv("AEON_HIVE_URL", "http://localhost:7878")
UPDATE_SERVER = os.getenv("AEON_UPDATE_SERVER", "https://aeon-update-server-y2r5ogip6q-ue.a.run.app")
CI_TOKEN      = os.getenv("AEON_CI_TOKEN", "")
SCAN_INTERVAL = int(os.getenv("SCAN_INTERVAL_SECONDS", "3600"))  # 1 hour

# Chromium-adjacent packages we track
TRACKED_PKGS = [
    "chromium", "v8", "blink", "libwebp", "libjpeg-turbo",
    "openssl", "boringssl", "libpng", "zlib", "expat",
    "pdfium", "opus", "libvpx", "ffmpeg"
]

NVD_API = "https://services.nvd.nist.gov/rest/json/cves/2.0"
GH_ADVISORY_API = "https://api.github.com/advisories"
ARXIV_API = "https://export.arxiv.org/api/query"


async def fetch_nvd_cves(client: httpx.AsyncClient, since_hours: int = 25) -> list[dict]:
    """Pull recent CVEs from NIST NVD affecting tracked packages."""
    since = (datetime.now(timezone.utc) - timedelta(hours=since_hours)).strftime("%Y-%m-%dT%H:%M:%S.000")
    found = []
    for pkg in TRACKED_PKGS:
        try:
            r = await client.get(NVD_API, params={
                "keywordSearch": pkg,
                "pubStartDate": since,
                "resultsPerPage": 20
            }, timeout=15)
            if r.status_code == 200:
                data = r.json()
                for item in data.get("vulnerabilities", []):
                    cve = item.get("cve", {})
                    cve_id = cve.get("id", "")
                    severity = "UNKNOWN"
                    metrics = cve.get("metrics", {})
                    if "cvssMetricV31" in metrics:
                        severity = metrics["cvssMetricV31"][0]["cvssData"]["baseSeverity"]
                    elif "cvssMetricV30" in metrics:
                        severity = metrics["cvssMetricV30"][0]["cvssData"]["baseSeverity"]
                    desc = cve.get("descriptions", [{}])[0].get("value", "")
                    found.append({
                        "source": "NVD",
                        "cve_id": cve_id,
                        "package": pkg,
                        "severity": severity,
                        "description": desc[:300],
                        "url": f"https://nvd.nist.gov/vuln/detail/{cve_id}",
                        "discovered_at": datetime.now(timezone.utc).isoformat()
                    })
        except Exception as e:
            print(f"[WARN] NVD fetch for {pkg}: {e}")
    return found


async def fetch_gh_advisories(client: httpx.AsyncClient) -> list[dict]:
    """Pull GitHub Security Advisories for tracked packages."""
    found = []
    try:
        r = await client.get(GH_ADVISORY_API, params={
            "ecosystem": "other",
            "per_page": 50,
            "sort": "published",
            "direction": "desc"
        }, headers={"Accept": "application/vnd.github+json"}, timeout=15)
        if r.status_code == 200:
            for adv in r.json():
                pkg_names = [p.get("package", {}).get("name", "").lower()
                             for p in adv.get("vulnerabilities", [])]
                if any(t in " ".join(pkg_names) for t in TRACKED_PKGS):
                    found.append({
                        "source": "GitHub",
                        "cve_id": adv.get("cve_id", adv.get("ghsa_id", "")),
                        "package": ",".join(pkg_names[:3]),
                        "severity": adv.get("severity", "UNKNOWN").upper(),
                        "description": adv.get("summary", "")[:300],
                        "url": adv.get("html_url", ""),
                        "discovered_at": datetime.now(timezone.utc).isoformat()
                    })
    except Exception as e:
        print(f"[WARN] GH advisories: {e}")
    return found


async def fetch_arxiv_papers(client: httpx.AsyncClient) -> list[dict]:
    """Check arXiv for new browser security research (for AeonPatchWriter context)."""
    terms = ["browser+fingerprinting", "chromium+vulnerability", "V8+security", "memory+safety+browser"]
    found = []
    for term in terms:
        try:
            r = await client.get(ARXIV_API, params={
                "search_query": f"ti:{term}+OR+abs:{term}",
                "sortBy": "submittedDate",
                "sortOrder": "descending",
                "max_results": 5
            }, timeout=15)
            if r.status_code == 200:
                # Parse Atom feed minimally
                entries = re.findall(r'<entry>(.*?)</entry>', r.text, re.DOTALL)
                for entry in entries:
                    title = re.search(r'<title>(.*?)</title>', entry)
                    link  = re.search(r'<id>(.*?)</id>', entry)
                    summary = re.search(r'<summary>(.*?)</summary>', entry, re.DOTALL)
                    if title and link:
                        found.append({
                            "source": "arXiv",
                            "cve_id": "",
                            "package": term,
                            "severity": "RESEARCH",
                            "description": (summary.group(1).strip()[:200] if summary else ""),
                            "url": link.group(1).strip(),
                            "discovered_at": datetime.now(timezone.utc).isoformat()
                        })
        except Exception as e:
            print(f"[WARN] arXiv {term}: {e}")
    return found


async def submit_to_vote_ledger(findings: list[dict]) -> None:
    """POST findings to AeonHive vote ledger for peer review."""
    if not findings or not HIVE_ENDPOINT:
        return
    async with httpx.AsyncClient() as client:
        try:
            await client.post(f"{HIVE_ENDPOINT}/v1/vote/propose", json={
                "type": "security_findings",
                "findings": findings,
                "proposed_by": "aeon-research-agent",
                "requires_vote": [f for f in findings if f["severity"] in ("CRITICAL", "HIGH")],
                "auto_approve": [f for f in findings if f["severity"] not in ("CRITICAL", "HIGH")],
                "timestamp": datetime.now(timezone.utc).isoformat()
            }, timeout=10)
            print(f"[INFO] Submitted {len(findings)} findings to vote ledger")
        except Exception as e:
            print(f"[WARN] Vote ledger unreachable: {e}")


async def alert_patch_writer(critical: list[dict]) -> None:
    """Notify AeonPatchWriter about critical findings needing immediate patches."""
    if not critical:
        return
    print(f"[ALERT] {len(critical)} CRITICAL/HIGH findings — notifying AeonPatchWriter")
    # In production: write to a Cloud Tasks queue → triggers aeon_patch_writer.py
    with open("/tmp/aeon_critical_findings.json", "w") as f:
        json.dump(critical, f, indent=2)


async def scan_cycle() -> None:
    """One full scan cycle."""
    ts = datetime.now(timezone.utc).isoformat()
    print(f"\n[{ts}] AeonResearchAgent — scan cycle starting")

    async with httpx.AsyncClient() as client:
        nvd, gh, arxiv = await asyncio.gather(
            fetch_nvd_cves(client),
            fetch_gh_advisories(client),
            fetch_arxiv_papers(client)
        )

    all_findings = nvd + gh + arxiv
    critical = [f for f in all_findings if f["severity"] in ("CRITICAL", "HIGH")]

    print(f"[INFO] Found: {len(nvd)} NVD | {len(gh)} GH | {len(arxiv)} arXiv | {len(critical)} critical")

    await submit_to_vote_ledger(all_findings)
    await alert_patch_writer(critical)

    # Deduplicate by CVE ID for reporting
    seen = set()
    unique = []
    for f in all_findings:
        key = f["cve_id"] or hashlib.md5(f["url"].encode()).hexdigest()
        if key not in seen:
            seen.add(key)
            unique.append(f)

    print(f"[INFO] Cycle complete — {len(unique)} unique findings")
    return unique


async def main():
    print("=" * 60)
    print("AeonResearchAgent v1.0 — Sovereign CVE Intelligence")
    print(f"Scan interval: {SCAN_INTERVAL}s | Tracked: {len(TRACKED_PKGS)} packages")
    print("=" * 60)
    while True:
        try:
            await scan_cycle()
        except Exception as e:
            print(f"[ERROR] Scan cycle failed: {e}")
        print(f"[INFO] Sleeping {SCAN_INTERVAL}s until next scan...")
        await asyncio.sleep(SCAN_INTERVAL)


if __name__ == "__main__":
    asyncio.run(main())
