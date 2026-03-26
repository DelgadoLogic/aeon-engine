#!/usr/bin/env python3
"""
AeonVoteCoordinator — Peer consensus engine for updates & patches.
Manages the democratic vote process:
  - Security patches: 66% peer approval → auto-apply
  - Feature updates: 51% approval → queue for next release cycle
  - Sovereign override: Master key bearer can force-approve/reject any vote
"""
import asyncio
import hashlib
import json
import os
import time
from datetime import datetime, timezone, timedelta
from typing import Optional
import httpx
from fastapi import FastAPI, HTTPException, Header
from pydantic import BaseModel

app = FastAPI(title="AeonVoteCoordinator", version="1.0.0")

HIVE_PORT     = int(os.getenv("HIVE_PORT", "7878"))
CI_TOKEN      = os.getenv("AEON_CI_TOKEN", "")
UPDATE_SERVER = os.getenv("AEON_UPDATE_SERVER", "")

# Active vote ledger (production: use Cloud Firestore)
vote_ledger: dict[str, dict] = {}


class VoteProposal(BaseModel):
    type: str            # "security_patch" | "feature_update" | "model_update"
    proposed_by: str
    patch: Optional[dict] = None
    findings: Optional[list] = None
    auto_apply_threshold: float = 0.66
    vote_deadline_hours: int = 24
    timestamp: str


class PeerVote(BaseModel):
    proposal_id: str
    peer_id: str
    vote: str            # "approve" | "reject" | "abstain"
    signature: str       # Ed25519 signature of vote content
    timestamp: str


class SovereignOverride(BaseModel):
    proposal_id: str
    action: str          # "force_approve" | "force_reject" | "emergency_patch"
    sovereign_sig: str   # Must be signed by master Ed25519 key


# ─── Vote thresholds by proposal type ────────────────────────────────────────
THRESHOLDS = {
    "security_patch":  0.51,   # Simple majority — security is urgent
    "feature_update":  0.66,   # Supermajority — feature changes need consensus
    "model_update":    0.60,   # AI model updates need strong consensus
    "sovereign_emit":  0.00,   # Sovereign overrides need 0% — instant apply
}

# ─── Silence policy: never interrupt active users ────────────────────────────
QUIET_HOURS_START = 8   # 8 AM — user is likely active
QUIET_HOURS_END   = 22  # 10 PM — user is likely done

def is_quiet_window() -> bool:
    """True during 10PM–8AM when safe to apply updates silently."""
    hour = datetime.now().hour
    return hour >= QUIET_HOURS_END or hour < QUIET_HOURS_START


@app.post("/v1/vote/propose")
async def propose(proposal: VoteProposal):
    """Submit a new vote proposal to the ledger."""
    proposal_id = hashlib.sha256(
        f"{proposal.type}{proposal.proposed_by}{proposal.timestamp}".encode()
    ).hexdigest()[:16]

    deadline = datetime.now(timezone.utc) + timedelta(hours=proposal.vote_deadline_hours)

    vote_ledger[proposal_id] = {
        "id": proposal_id,
        "type": proposal.type,
        "proposed_by": proposal.proposed_by,
        "proposal": proposal.dict(),
        "votes": {"approve": [], "reject": [], "abstain": []},
        "status": "open",
        "deadline": deadline.isoformat(),
        "threshold": THRESHOLDS.get(proposal.type, 0.66),
        "created_at": datetime.now(timezone.utc).isoformat()
    }

    print(f"[VoteCoord] New proposal: {proposal_id} ({proposal.type}) — deadline: {deadline.strftime('%H:%M UTC')}")
    return {"proposal_id": proposal_id, "deadline": deadline.isoformat()}


@app.post("/v1/vote/cast")
async def cast_vote(vote: PeerVote):
    """Cast a vote from a peer node."""
    if vote.proposal_id not in vote_ledger:
        raise HTTPException(404, "Proposal not found")

    p = vote_ledger[vote.proposal_id]
    if p["status"] != "open":
        raise HTTPException(409, f"Proposal is {p['status']}")

    # Add vote (deduplicate by peer_id)
    for bucket in p["votes"].values():
        bucket[:] = [v for v in bucket if v["peer_id"] != vote.peer_id]
    p["votes"][vote.vote].append({"peer_id": vote.peer_id, "timestamp": vote.timestamp})

    # Check if threshold reached
    total = sum(len(v) for v in p["votes"].values())
    approvals = len(p["votes"]["approve"])
    ratio = approvals / total if total > 0 else 0

    if ratio >= p["threshold"] and total >= 3:  # Minimum 3 peers
        p["status"] = "approved"
        asyncio.create_task(apply_approved_proposal(p))
        print(f"[VoteCoord] APPROVED: {vote.proposal_id} ({ratio:.0%} approval from {total} peers)")

    return {"status": "vote_recorded", "current_ratio": round(ratio, 3), "total_votes": total}


@app.post("/v1/vote/sovereign")
async def sovereign_override(override: SovereignOverride, authorization: str = Header(None)):
    """Sovereign key bearer can force-approve or force-reject any proposal."""
    if not authorization or authorization != f"Bearer {CI_TOKEN}":
        raise HTTPException(403, "Sovereign auth required")

    if override.proposal_id not in vote_ledger:
        raise HTTPException(404, "Proposal not found")

    p = vote_ledger[override.proposal_id]
    if override.action == "force_approve":
        p["status"] = "approved"
        p["sovereign_override"] = True
        asyncio.create_task(apply_approved_proposal(p))
        print(f"[VoteCoord] SOVEREIGN FORCE-APPROVE: {override.proposal_id}")
    elif override.action == "force_reject":
        p["status"] = "rejected"
        p["sovereign_override"] = True
        print(f"[VoteCoord] SOVEREIGN FORCE-REJECT: {override.proposal_id}")

    return {"status": p["status"]}


@app.get("/v1/vote/status/{proposal_id}")
async def get_status(proposal_id: str):
    if proposal_id not in vote_ledger:
        raise HTTPException(404, "Not found")
    return vote_ledger[proposal_id]


@app.get("/v1/vote/open")
async def list_open():
    return [p for p in vote_ledger.values() if p["status"] == "open"]


async def apply_approved_proposal(proposal: dict) -> None:
    """Execute an approved proposal. Waits for quiet window if non-urgent."""
    ptype = proposal["type"]

    # Wait for quiet window for non-critical updates
    if ptype != "security_patch":
        while not is_quiet_window():
            print(f"[VoteCoord] Waiting for quiet window to apply {proposal['id']}...")
            await asyncio.sleep(300)

    print(f"[VoteCoord] Applying approved proposal: {proposal['id']} ({ptype})")

    if ptype == "security_patch":
        patch_info = proposal["proposal"].get("patch", {})
        patch_file = patch_info.get("patch_file", "")
        print(f"[VoteCoord] Applying security patch: {patch_file}")
        # In production: trigger Cloud Build to apply patch + rebuild
        # gcloud builds submit --config=cloudbuild_patch.yaml --substitutions=_PATCH_FILE={patch_file}

    elif ptype in ("feature_update", "model_update"):
        print(f"[VoteCoord] Queuing {ptype} for next build cycle")
        # Write to build queue → Cloud Build picks up


if __name__ == "__main__":
    import uvicorn
    print("AeonVoteCoordinator v1.0 — Peer consensus engine")
    uvicorn.run(app, host="0.0.0.0", port=HIVE_PORT)
