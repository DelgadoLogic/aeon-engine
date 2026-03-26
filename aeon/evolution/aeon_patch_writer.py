#!/usr/bin/env python3
"""
AeonPatchWriter — AI-driven C++ patch generator.
Triggered by AeonResearchAgent when critical CVEs are found.
Uses a local LLM (via AeonAgent) to draft Chromium patches,
then submits them to the AeonHive vote ledger for peer review.
"""
import asyncio
import json
import os
import subprocess
import textwrap
from datetime import datetime, timezone
from pathlib import Path
from typing import Optional
import httpx

AGENT_PORT    = int(os.getenv("AEON_AGENT_PORT", "11434"))
HIVE_ENDPOINT = os.getenv("AEON_HIVE_URL", "http://localhost:7878")
CHROMIUM_SRC  = Path(os.getenv("CHROMIUM_SRC", "/chromium/src"))
PATCH_OUTPUT  = Path(os.getenv("PATCH_OUTPUT", "/tmp/aeon-patches"))

PATCH_OUTPUT.mkdir(exist_ok=True)

SYSTEM_PROMPT = """You are an expert Chromium/C++ security engineer working for Aeon Browser.
Your job is to write minimal, safe, production-quality patches to fix security vulnerabilities in Chromium.
Rules:
- Patches must be surgical — change only what is necessary to fix the vulnerability
- Prefer defensive checks over architectural rewrites
- Every patch must include a comment: "AeonShield: fix for {CVE_ID}"
- Output only the unified diff (git diff format), nothing else
- If you cannot safely patch this without deep context, output: NEEDS_HUMAN_REVIEW
"""

PATCH_TEMPLATES = {
    "use-after-free": """
Pattern: Use-after-free in {file}
Fix approach: Add nullptr check before dereference + DCHECK in debug builds
""",
    "heap-buffer-overflow": """
Pattern: Heap buffer overflow in {file}
Fix approach: Add bounds check before memcpy/write, validate size parameter
""",
    "type-confusion": """
Pattern: Type confusion in {file}
Fix approach: Add DCHECK_EQ on object type before cast, return nullptr on mismatch
""",
    "integer-overflow": """
Pattern: Integer overflow in {file}  
Fix approach: Use base::CheckedNumeric<T> wrapper for arithmetic
"""
}


async def call_local_llm(prompt: str) -> str:
    """Call AeonAgent's local LLM (Ollama-compatible endpoint)."""
    async with httpx.AsyncClient() as client:
        try:
            r = await client.post(
                f"http://127.0.0.1:{AGENT_PORT}/api/generate",
                json={"model": "codellama", "prompt": prompt, "stream": False},
                timeout=120
            )
            return r.json().get("response", "").strip()
        except Exception as e:
            return f"LLM_UNAVAILABLE: {e}"


async def generate_patch(finding: dict) -> Optional[dict]:
    """Generate a C++ patch for a CVE finding."""
    cve_id = finding.get("cve_id", "UNKNOWN")
    pkg    = finding.get("package", "chromium")
    desc   = finding.get("description", "")
    severity = finding.get("severity", "UNKNOWN")

    print(f"[PatchWriter] Processing {cve_id} ({severity}) — {pkg}")

    # Identify vulnerability pattern
    vuln_type = "unknown"
    for pattern in ["use-after-free", "heap-buffer-overflow", "type-confusion", "integer-overflow"]:
        if pattern.replace("-", " ") in desc.lower() or pattern in desc.lower():
            vuln_type = pattern
            break

    template = PATCH_TEMPLATES.get(vuln_type, "")
    prompt = f"""{SYSTEM_PROMPT}

CVE: {cve_id}
Severity: {severity}
Package: {pkg}
Description: {desc}

Vulnerability type detected: {vuln_type}
{template}

Generate a minimal Chromium C++ patch in unified diff format to fix this vulnerability.
The patch should apply cleanly to chromium/src.
"""
    patch_code = await call_local_llm(prompt)

    if "NEEDS_HUMAN_REVIEW" in patch_code or "LLM_UNAVAILABLE" in patch_code:
        print(f"[PatchWriter] {cve_id} flagged for human review")
        return None

    # Save patch file
    ts = datetime.now(timezone.utc).strftime("%Y%m%d_%H%M%S")
    patch_file = PATCH_OUTPUT / f"aeon_{cve_id.replace('/', '_')}_{ts}.patch"
    patch_file.write_text(patch_code)
    print(f"[PatchWriter] Patch saved: {patch_file}")

    return {
        "cve_id": cve_id,
        "severity": severity,
        "vuln_type": vuln_type,
        "patch_file": str(patch_file),
        "patch_preview": patch_code[:500],
        "generated_at": datetime.now(timezone.utc).isoformat(),
        "status": "pending_vote"
    }


async def submit_patch_for_vote(patch: dict) -> None:
    """Submit generated patch to AeonHive for peer review."""
    async with httpx.AsyncClient() as client:
        try:
            await client.post(f"{HIVE_ENDPOINT}/v1/vote/propose", json={
                "type": "security_patch",
                "patch": patch,
                "proposed_by": "aeon-patch-writer",
                "auto_apply_threshold": 0.66,  # 66% majority required
                "vote_deadline_hours": 24,
                "timestamp": datetime.now(timezone.utc).isoformat()
            }, timeout=10)
            print(f"[PatchWriter] {patch['cve_id']} submitted to vote ledger")
        except Exception as e:
            print(f"[WARN] Vote submission failed: {e}")


async def process_findings(findings_file: str = "/tmp/aeon_critical_findings.json") -> None:
    """Process a batch of findings from AeonResearchAgent."""
    try:
        findings = json.loads(Path(findings_file).read_text())
    except FileNotFoundError:
        print("[PatchWriter] No findings file — waiting for AeonResearchAgent")
        return
    except json.JSONDecodeError as e:
        print(f"[ERROR] Corrupt findings file: {e}")
        return

    critical = [f for f in findings if f.get("severity") in ("CRITICAL", "HIGH")]
    print(f"[PatchWriter] Processing {len(critical)} critical/high findings")

    for finding in critical:
        patch = await generate_patch(finding)
        if patch:
            await submit_patch_for_vote(patch)
        await asyncio.sleep(2)  # Rate limit LLM calls


async def main():
    print("=" * 60)
    print("AeonPatchWriter v1.0 — Autonomous C++ Patch Generator")
    print(f"LLM endpoint: 127.0.0.1:{AGENT_PORT} (local — no data leaves device)")
    print("=" * 60)
    # Watch for new findings every 5 minutes
    while True:
        await process_findings()
        print("[PatchWriter] Sleeping 5 min...")
        await asyncio.sleep(300)


if __name__ == "__main__":
    asyncio.run(main())
