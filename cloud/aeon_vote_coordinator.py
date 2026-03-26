"""
AeonHive Vote Coordinator — aeon_vote_coordinator.py
Runs inside every AeonMind instance.

When a new patch appears in GCS staging:
1. Downloads the patch diff
2. Asks local Ollama: "Is this safe and beneficial?"
3. Submits vote to GCS vote tally
4. After 24h or 2/3 majority: declares result and triggers build

PHILOSOPHY: No single entity controls what ships.
The network of Aeon users collectively approves every non-security update.
This makes it impossible for anyone — even us — to push a malicious update
without the majority of the peer network approving it.
"""

import os
import json
import time
import logging
import datetime
import socket
import hashlib
import requests
from google.cloud import storage

logging.basicConfig(level=logging.INFO)
log = logging.getLogger("AeonVote")

GCS_BUCKET   = os.environ.get("GCS_BUCKET", "aeon-chromium-artifacts")
OLLAMA_URL   = os.environ.get("OLLAMA_URL", "http://localhost:11434")
OLLAMA_MODEL = os.environ.get("OLLAMA_MODEL", "deepseek-coder:6.7b")
PEER_ID      = hashlib.sha256(socket.getfqdn().encode()).hexdigest()[:12]

# Approval threshold (66% of participating peers must approve)
APPROVAL_THRESHOLD = 0.66
# Quorum minimum (at least 3 peers must vote before deciding)
MIN_QUORUM = 3

gcs = storage.Client()

def gcs_read_json(path, default=None):
    try:
        return json.loads(gcs.bucket(GCS_BUCKET).blob(path).download_as_text())
    except Exception:
        return default

def gcs_write_json(path, data):
    gcs.bucket(GCS_BUCKET).blob(path).upload_from_string(
        json.dumps(data, indent=2), content_type="application/json")

# ── AI safety check ──────────────────────────────────────────────────────────

def evaluate_patch_safety(patch_text: str, finding: dict) -> tuple[str, str]:
    """
    Ask local Ollama to evaluate whether this patch is safe to apply.
    Returns: ("APPROVE" | "REJECT" | "ABSTAIN", reasoning)
    """
    title = finding.get("title", "")
    source = finding.get("source", "")
    
    # If from official CVE database with Chromium fix — auto-approve
    if source in ("nvd_cve", "chromium_security") and "cve" in title.lower():
        return "APPROVE", "Official security patch from CVE database — auto-approved"
    
    # Truncate large patches to fit context
    patch_preview = patch_text[:3000] if len(patch_text) > 3000 else patch_text
    
    prompt = f"""You are a security reviewer for Aeon Browser, a privacy-focused Chromium fork.
Evaluate this proposed code patch for safety and benefit.

Finding: {title}
Source: {source}

Patch diff (may be truncated):
```diff
{patch_preview}
```

Answer with ONE of:
APPROVE — if the patch is safe, minimal, and beneficial for a privacy browser
REJECT — if the patch is dangerous, malicious, bloated, or introduces telemetry
ABSTAIN — if you cannot determine safety from available information

Then on a new line, 1 sentence explaining your vote.
Format: VOTE: [APPROVE/REJECT/ABSTAIN]
Reason: [1 sentence]"""

    try:
        resp = requests.post(
            f"{OLLAMA_URL}/api/generate",
            json={"model": OLLAMA_MODEL, "prompt": prompt, "stream": False},
            timeout=90)
        text = resp.json().get("response", "")
        
        vote = "ABSTAIN"
        reason = "Could not parse AI response"
        
        for line in text.split("\n"):
            if line.startswith("VOTE:"):
                raw = line.replace("VOTE:", "").strip().upper()
                if raw in ("APPROVE", "REJECT", "ABSTAIN"):
                    vote = raw
            elif line.startswith("Reason:"):
                reason = line.replace("Reason:", "").strip()
        
        return vote, reason
    except Exception as e:
        log.warning(f"AI evaluation failed: {e}")
        return "ABSTAIN", f"AI evaluation error: {e}"

# ── Voting ───────────────────────────────────────────────────────────────────

def cast_vote(patch_id: str, patch_record: dict):
    """Download patch, evaluate it, and record our vote."""
    
    # Check if we already voted on this patch
    our_vote_path = f"patches/votes/{patch_id}/{PEER_ID}.json"
    existing = gcs_read_json(our_vote_path)
    if existing:
        log.info(f"[VOTE] Already voted on patch {patch_id}")
        return
    
    log.info(f"[VOTE] Evaluating patch {patch_id}...")
    patch_text = patch_record.get("patch", "")
    finding = patch_record.get("finding", {})
    
    vote, reason = evaluate_patch_safety(patch_text, finding)
    
    vote_record = {
        "peer_id": PEER_ID,
        "patch_id": patch_id,
        "vote": vote,
        "reason": reason,
        "timestamp": datetime.datetime.utcnow().isoformat()
    }
    
    gcs_write_json(our_vote_path, vote_record)
    log.info(f"[VOTE] Cast {vote} on {patch_id}: {reason}")

# ── Tally votes ───────────────────────────────────────────────────────────────

def tally_votes(patch_id: str) -> dict:
    """Count all votes for a patch. Return tally summary."""
    prefix = f"patches/votes/{patch_id}/"
    blobs = list(gcs.bucket(GCS_BUCKET).list_blobs(prefix=prefix))
    
    approve = reject = abstain = 0
    reasons = []
    
    for blob in blobs:
        try:
            record = json.loads(blob.download_as_text())
            v = record.get("vote", "ABSTAIN")
            r = record.get("reason", "")
            if v == "APPROVE": approve += 1
            elif v == "REJECT": reject += 1
            else: abstain += 1
            reasons.append(f"[{v}] {r}")
        except Exception:
            pass
    
    total_definitive = approve + reject
    
    return {
        "patch_id": patch_id,
        "approve": approve,
        "reject": reject,
        "abstain": abstain,
        "total_votes": approve + reject + abstain,
        "total_definitive": total_definitive,
        "approval_rate": (approve / total_definitive) if total_definitive > 0 else 0,
        "quorum_reached": total_definitive >= MIN_QUORUM,
        "reasons": reasons[:10]  # Top 10
    }

def finalize_patch(patch_id: str, tally: dict):
    """If quorum and threshold reached, approve or reject the patch."""
    if not tally["quorum_reached"]:
        return  # Not enough votes yet
    
    patch_record = gcs_read_json(f"patches/staging/{patch_id}.json", {})
    if patch_record.get("status") in ("approved", "rejected"):
        return  # Already decided
    
    # Check deadline
    deadline = patch_record.get("vote_deadline", "")
    deadline_passed = False
    if deadline:
        try:
            dl = datetime.datetime.fromisoformat(deadline)
            deadline_passed = datetime.datetime.utcnow() > dl
        except Exception:
            pass
    
    should_decide = tally["approval_rate"] >= APPROVAL_THRESHOLD or deadline_passed
    
    if should_decide:
        if tally["approval_rate"] >= APPROVAL_THRESHOLD:
            patch_record["status"] = "approved"
            patch_record["approved_at"] = datetime.datetime.utcnow().isoformat()
            patch_record["vote_tally"] = tally
            log.info(f"[VOTE] Patch {patch_id} APPROVED ({tally['approval_rate']:.0%})")
        else:
            patch_record["status"] = "rejected"
            patch_record["rejected_at"] = datetime.datetime.utcnow().isoformat()
            patch_record["vote_tally"] = tally
            log.info(f"[VOTE] Patch {patch_id} REJECTED ({tally['approval_rate']:.0%})")
        
        gcs_write_json(f"patches/staging/{patch_id}.json", patch_record)
        
        if patch_record["status"] == "approved":
            # Signal build trigger
            trigger = gcs_read_json("build/trigger.json", {"patch_ids": []})
            if patch_id not in trigger["patch_ids"]:
                trigger["patch_ids"].append(patch_id)
                trigger["last_update"] = datetime.datetime.utcnow().isoformat()
                gcs_write_json("build/trigger.json", trigger)

# ── Main vote loop ─────────────────────────────────────────────────────────

def run_vote_cycle():
    log.info(f"=== AeonVote: cycle start (peer: {PEER_ID}) ===")
    
    pending = gcs_read_json("patches/pending.json", {"patches": []})
    
    for patch_id in pending.get("patches", []):
        record = gcs_read_json(f"patches/staging/{patch_id}.json", {})
        if not record:
            continue
        
        # Skip already-decided patches
        if record.get("status") in ("approved", "rejected"):
            continue
        
        # Auto-approved patches don't need voting
        if record.get("auto_approved"):
            finalize_patch(patch_id, {"approval_rate": 1.0, "quorum_reached": True,
                                       "total_definitive": 1, "approve": 1,
                                       "reject": 0, "abstain": 0, "reasons": []})
            continue
        
        # Cast our vote
        cast_vote(patch_id, record)
        
        # Tally and potentially finalize
        tally = tally_votes(patch_id)
        log.info(f"  Tally for {patch_id}: "
                 f"{tally['approve']}✓ {tally['reject']}✗ "
                 f"{tally['abstain']}? | rate: {tally['approval_rate']:.0%}")
        finalize_patch(patch_id, tally)
    
    log.info("=== Vote cycle complete ===")

if __name__ == "__main__":
    while True:
        try:
            run_vote_cycle()
        except Exception as e:
            log.error(f"Vote cycle error: {e}")
        time.sleep(3600)  # Check every hour
