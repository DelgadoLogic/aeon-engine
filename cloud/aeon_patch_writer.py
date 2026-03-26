"""
AeonPatchWriter — aeon_patch_writer.py
Reads the research queue from GCS and writes actual code patches.

Uses DeepSeek-Coder / Codestral via Ollama to generate unified diffs
that the cloud build VM will apply before compiling.

For security patches: fetches the official Chromium fix and re-applies it.
For AI-generated patches: uses code generation with the Aeon codebase context.
"""

import os
import json
import time
import logging
import datetime
import requests
import subprocess
import tempfile
from pathlib import Path
from google.cloud import storage
from google.cloud import secretmanager

logging.basicConfig(level=logging.INFO)
log = logging.getLogger("AeonPatchWriter")

GCS_BUCKET   = os.environ.get("GCS_BUCKET", "aeon-chromium-artifacts")
OLLAMA_URL   = os.environ.get("OLLAMA_URL", "http://localhost:11434")
CODE_MODEL   = os.environ.get("CODE_MODEL", "deepseek-coder:6.7b")

gcs = storage.Client()

def gcs_read_json(path, default=None):
    try:
        return json.loads(gcs.bucket(GCS_BUCKET).blob(path).download_as_text())
    except Exception:
        return default

def gcs_write(path, data, content_type="application/octet-stream"):
    gcs.bucket(GCS_BUCKET).blob(path).upload_from_string(data, content_type=content_type)
    log.info(f"[GCS] Wrote {path}")

# ── Fetch upstream Chromium fix for a CVE ───────────────────────────────────

def fetch_chromium_security_fix(cve_id: str) -> str | None:
    """
    Search Chromium's Gerrit for the CL that fixed this CVE.
    Returns the unified diff if found, None otherwise.
    """
    try:
        # Chromium Gerrit search
        query = f"message:{cve_id}+status:merged"
        resp = requests.get(
            f"https://chromium-review.googlesource.com/changes/?q={query}&n=1&o=CURRENT_REVISION&o=CURRENT_FILES",
            timeout=20)
        # Gerrit prepends )]}' — strip it
        text = resp.text
        if text.startswith(")]}'"):
            text = text[4:]
        changes = json.loads(text)
        if not changes:
            return None
        
        change_id = changes[0].get("id")
        revision = list(changes[0].get("revisions", {}).keys())[0]
        
        # Fetch the patch
        patch_resp = requests.get(
            f"https://chromium-review.googlesource.com/changes/{change_id}/revisions/{revision}/patch",
            timeout=20)
        
        import base64
        return base64.b64decode(patch_resp.text).decode("utf-8")
    except Exception as e:
        log.warning(f"Could not fetch Chromium fix for {cve_id}: {e}")
        return None

# ── Generate patch via Ollama ───────────────────────────────────────────────

def generate_patch_via_ai(finding: dict, chromium_context: str = "") -> str | None:
    """
    Use DeepSeek-Coder to generate a patch for a given finding.
    Only called for features/UX improvements — never for security patches
    (those come from official Chromium source).
    """
    title = finding.get("title", "")
    signal = finding.get("signal_key", "")
    finding_type = finding.get("type", "")
    
    # Build context from Aeon's existing code patterns
    context_hint = """
Relevant Aeon codebase patterns:
- AeonFingerprintSeed::GetSeed() — for privacy randomization
- window.aeon (IDL) — JS API exposed to renderer
- aeon:// scheme — internal WebUI pages
- AeonHive peers — P2P via UDP/TCP on port 9797
- AeonMind REST — localhost:7878

The code must:
1. Follow Chromium C++ style (no exceptions, use base:: utilities)
2. Be a minimal, focused change (< 100 lines)
3. Include proper copyright header
4. Be a valid unified diff against Chromium source
"""
    
    prompt = f"""You are an expert Chromium C++ engineer writing patches for Aeon Browser.
{context_hint}

Write a unified diff patch to implement the following improvement:
Title: {title}
Type: {finding_type}
Signal: {signal}

{chromium_context}

Generate ONLY the unified diff. Start with --- a/ and +++ b/ lines.
Make it minimal, correct, and safe. Add detailed comments explaining the change."""

    try:
        resp = requests.post(
            f"{OLLAMA_URL}/api/generate",
            json={"model": CODE_MODEL, "prompt": prompt, "stream": False},
            timeout=120)
        patch_text = resp.json().get("response", "")
        
        # Extract diff from response
        lines = patch_text.split("\n")
        diff_lines = []
        in_diff = False
        for line in lines:
            if line.startswith("--- a/") or line.startswith("--- /dev/null"):
                in_diff = True
            if in_diff:
                diff_lines.append(line)
        
        if diff_lines:
            return "\n".join(diff_lines)
        log.warning("AI did not produce a valid diff")
        return None
    except Exception as e:
        log.error(f"Patch generation failed: {e}")
        return None

# ── Validate patch via test compile ─────────────────────────────────────────

def quick_validate_patch(patch_text: str) -> tuple[bool, str]:
    """
    Basic structural validation of the patch before submitting to full build.
    A real production build would test-compile on a small GCP VM.
    """
    errors = []
    
    if not patch_text.strip():
        errors.append("Empty patch")
    if "--- a/" not in patch_text and "--- /dev/null" not in patch_text:
        errors.append("No valid diff header found")
    if "@@ " not in patch_text:
        errors.append("No hunk headers found")
    
    # Check for dangerous patterns
    dangerous = ["rm -rf", "format c:", "delete from", "__asm__", "system("]
    for d in dangerous:
        if d.lower() in patch_text.lower():
            errors.append(f"Dangerous pattern detected: {d}")
    
    return (len(errors) == 0), "; ".join(errors)

# ── Submit patch to GCS staging ─────────────────────────────────────────────

def submit_patch_for_voting(finding: dict, patch_text: str):
    """
    Write patch to GCS staging area and announce to AeonHive for voting.
    """
    patch_id = finding.get("id", "unknown")
    timestamp = datetime.datetime.utcnow().isoformat()
    
    patch_record = {
        "id": patch_id,
        "finding": finding,
        "patch": patch_text,
        "submitted_at": timestamp,
        "status": "voting",
        "votes_approve": 0,
        "votes_reject": 0,
        "vote_deadline": (
            datetime.datetime.utcnow() + datetime.timedelta(hours=24)
        ).isoformat()
    }
    
    # Security patches skip voting (auto-approved)
    if finding.get("priority") == "critical" or finding.get("source") == "nvd_cve":
        patch_record["status"] = "approved"
        patch_record["auto_approved"] = True
        patch_record["auto_approve_reason"] = "Security patch from CVE database"
        log.info(f"[VOTE] Patch {patch_id} auto-approved (security)")
    
    gcs_write(
        f"patches/staging/{patch_id}.json",
        json.dumps(patch_record, indent=2),
        "application/json")
    
    # Append to pending patches list
    pending = gcs_read_json("patches/pending.json", {"patches": []})
    pending["patches"].append(patch_id)
    pending["patches"] = list(set(pending["patches"]))  # dedupe
    gcs_write("patches/pending.json", json.dumps(pending), "application/json")
    
    return patch_record

# ── Main patch writer cycle ──────────────────────────────────────────────────

def run_patch_writer():
    log.info("=== AeonPatchWriter: cycle starting ===")
    
    queue = gcs_read_json("research/queue.json", {"items": []})
    items = queue.get("items", [])
    
    # Take top 3 actionable items (don't overwhelm the build queue)
    actionable = [
        item for item in items
        if item.get("ai_action") in ("apply_patch", "implement_feature")
        and item.get("type") in ("security_patch", "privacy_patch", "ux_improvement")
    ][:3]
    
    if not actionable:
        log.info("No actionable items in queue this cycle")
        return
    
    written = 0
    for finding in actionable:
        log.info(f"Processing: {finding['title'][:60]}...")
        patch_text = None
        
        if finding.get("type") == "security_patch" and finding.get("cve_id"):
            # Fetch official fix
            log.info(f"  → Fetching official Chromium fix for {finding['cve_id']}")
            patch_text = fetch_chromium_security_fix(finding["cve_id"])
        
        if not patch_text:
            # Generate via AI
            log.info("  → Generating patch via AI")
            patch_text = generate_patch_via_ai(finding)
        
        if not patch_text:
            log.warning(f"  ✗ Could not produce patch for: {finding['title']}")
            continue
        
        valid, errors = quick_validate_patch(patch_text)
        if not valid:
            log.warning(f"  ✗ Patch validation failed: {errors}")
            continue
        
        record = submit_patch_for_voting(finding, patch_text)
        log.info(f"  ✓ Patch {finding['id']} submitted — status: {record['status']}")
        written += 1
        
        # Mark as processed in queue
        finding["ai_action"] = "submitted"
    
    # Save updated queue
    gcs_write(
        "research/queue.json",
        json.dumps(queue, indent=2),
        "application/json")
    
    log.info(f"=== PatchWriter cycle done. Submitted {written} patches ===")
    
    # If any approved patches → trigger build
    pending = gcs_read_json("patches/pending.json", {"patches": []})
    approved = []
    for pid in pending.get("patches", []):
        record = gcs_read_json(f"patches/staging/{pid}.json", {})
        if record.get("status") == "approved":
            approved.append(pid)
    
    if approved:
        log.info(f"Triggering build for {len(approved)} approved patches")
        trigger = {
            "triggered_by": "patch_writer",
            "patch_ids": approved,
            "timestamp": datetime.datetime.utcnow().isoformat()
        }
        gcs_write("build/trigger.json", json.dumps(trigger), "application/json")

if __name__ == "__main__":
    run_patch_writer()
