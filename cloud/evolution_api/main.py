"""
Aeon Evolution Engine API — Cloud Run Service
═══════════════════════════════════════════════
Orchestrates the autonomous Evolution Engine pipeline:
  1. /scan       → Trigger CVE research scan
  2. /patch      → Generate patch for a CVE
  3. /vote       → Start peer vote on a patch
  4. /build      → Dispatch build to AeonHive
  5. /train      → Trigger LoRA fine-tuning
  6. /silence    → PAUSE/RESUME agent operations
  7. /health     → Health check
  8. /waitlist   → Waitlist signup endpoint

Deployed to: Cloud Run (us-east1) with aeon-evolution-sa
"""

import os
import json
import hashlib
import logging
from datetime import datetime, timezone

from fastapi import FastAPI, HTTPException, Request
from fastapi.middleware.cors import CORSMiddleware
from pydantic import BaseModel, EmailStr
from google.cloud import pubsub_v1, firestore, secretmanager

# ── Config ──────────────────────────────────────────────────────────────
PROJECT_ID = os.getenv("GCP_PROJECT", "aeon-browser-build")
REGION = os.getenv("GCP_REGION", "us-east1")
PORT = int(os.getenv("PORT", 8080))

TOPIC_EVOLUTION = f"projects/{PROJECT_ID}/topics/aeon-evolution-events"
TOPIC_SILENCE = f"projects/{PROJECT_ID}/topics/aeon-silence-signals"
TOPIC_PATCH = f"projects/{PROJECT_ID}/topics/aeon-patch-events"
TOPIC_BUILD = f"projects/{PROJECT_ID}/topics/aeon-build-events"
TOPIC_TRAINING = f"projects/{PROJECT_ID}/topics/aeon-training-events"

# ── Logging ─────────────────────────────────────────────────────────────
logging.basicConfig(level=logging.INFO, format="%(asctime)s [%(levelname)s] %(message)s")
logger = logging.getLogger("aeon-evolution-api")

# ── App ─────────────────────────────────────────────────────────────────
app = FastAPI(
    title="Aeon Evolution Engine",
    description="Autonomous browser patching pipeline API",
    version="1.0.0",
)

app.add_middleware(
    CORSMiddleware,
    allow_origins=[
        "https://aeonbrowser.com",
        "https://aeon-site.web.app",
        "http://localhost:5173",
        "http://localhost:3000",
    ],
    allow_credentials=True,
    allow_methods=["*"],
    allow_headers=["*"],
)

# ── Lazy Clients (cold-start optimization) ──────────────────────────────
_publisher = None
_db = None


def get_publisher():
    global _publisher
    if _publisher is None:
        _publisher = pubsub_v1.PublisherClient()
    return _publisher


def get_firestore():
    global _db
    if _db is None:
        _db = firestore.Client(project=PROJECT_ID)
    return _db


# ── Pydantic Models ─────────────────────────────────────────────────────

class ScanRequest(BaseModel):
    source: str = "scheduler"
    scan_type: str = "full"  # full | incremental | targeted
    target_component: str | None = None  # e.g., "V8", "Blink", "skia"


class PatchRequest(BaseModel):
    cve_id: str
    severity: str = "high"
    component: str = "chromium"
    auto_vote: bool = False


class VoteRequest(BaseModel):
    patch_id: str
    action: str = "start"  # start | sovereign_override
    ed25519_signature: str | None = None


class BuildRequest(BaseModel):
    patch_id: str
    target_tiers: list[str] = ["pro", "modern", "extended"]
    use_hive: bool = True


class TrainRequest(BaseModel):
    trigger: str = "manual"  # manual | post_patch | scheduled
    min_patches: int = 5


class SilenceRequest(BaseModel):
    action: str  # PAUSE | RESUME
    reason: str = "user_active"


class WaitlistSignup(BaseModel):
    email: str
    tier: str = "free"  # free | pro


# ── Routes ──────────────────────────────────────────────────────────────

@app.get("/health")
async def health():
    return {
        "status": "operational",
        "service": "aeon-evolution-engine",
        "version": "1.0.0",
        "timestamp": datetime.now(timezone.utc).isoformat(),
        "agents": {
            "research": "ready",
            "patch_writer": "ready",
            "vote_coordinator": "ready",
            "build_worker": "ready",
            "self_trainer": "standby",
            "silence_policy": "active",
        },
    }


@app.post("/scan")
async def trigger_scan(req: ScanRequest):
    """Trigger a CVE research scan. Called hourly by Cloud Scheduler."""
    payload = {
        "action": "scan",
        "scan_type": req.scan_type,
        "target_component": req.target_component,
        "triggered_by": req.source,
        "timestamp": datetime.now(timezone.utc).isoformat(),
    }
    _publish(TOPIC_EVOLUTION, payload)

    # Log to Firestore
    db = get_firestore()
    db.collection("evolution_scans").add({
        **payload,
        "status": "dispatched",
    })

    logger.info(f"CVE scan dispatched: type={req.scan_type}, component={req.target_component}")
    return {"status": "dispatched", "scan_type": req.scan_type}


@app.post("/patch")
async def generate_patch(req: PatchRequest):
    """Dispatch patch generation for a specific CVE."""
    payload = {
        "action": "generate_patch",
        "cve_id": req.cve_id,
        "severity": req.severity,
        "component": req.component,
        "auto_vote": req.auto_vote,
        "timestamp": datetime.now(timezone.utc).isoformat(),
    }
    _publish(TOPIC_PATCH, payload)

    db = get_firestore()
    doc_ref = db.collection("patches").document(req.cve_id.replace("-", "_"))
    doc_ref.set({
        **payload,
        "status": "generating",
    })

    logger.info(f"Patch dispatch: {req.cve_id} ({req.severity}) for {req.component}")
    return {"status": "generating", "cve_id": req.cve_id}


@app.post("/vote")
async def manage_vote(req: VoteRequest):
    """Start or override a peer vote on a patch."""
    if req.action == "sovereign_override" and not req.ed25519_signature:
        raise HTTPException(status_code=403, detail="Sovereign override requires Ed25519 signature")

    payload = {
        "action": f"vote_{req.action}",
        "patch_id": req.patch_id,
        "timestamp": datetime.now(timezone.utc).isoformat(),
    }

    if req.ed25519_signature:
        payload["sovereign_override"] = True
        payload["signature_hash"] = hashlib.sha256(req.ed25519_signature.encode()).hexdigest()[:16]

    _publish(TOPIC_EVOLUTION, payload)

    db = get_firestore()
    db.collection("votes").document(req.patch_id).set({
        **payload,
        "status": "in_progress" if req.action == "start" else "sovereign_approved",
    })

    logger.info(f"Vote {req.action}: patch={req.patch_id}")
    return {"status": "vote_initiated", "patch_id": req.patch_id, "action": req.action}


@app.post("/build")
async def dispatch_build(req: BuildRequest):
    """Dispatch a build to AeonHive or cloud workers."""
    payload = {
        "action": "build",
        "patch_id": req.patch_id,
        "target_tiers": req.target_tiers,
        "use_hive": req.use_hive,
        "timestamp": datetime.now(timezone.utc).isoformat(),
    }
    _publish(TOPIC_BUILD, payload)

    db = get_firestore()
    db.collection("builds").document(req.patch_id).set({
        **payload,
        "status": "queued",
    })

    logger.info(f"Build dispatched: patch={req.patch_id}, tiers={req.target_tiers}, hive={req.use_hive}")
    return {"status": "queued", "patch_id": req.patch_id, "tiers": req.target_tiers}


@app.post("/train")
async def trigger_training(req: TrainRequest):
    """Trigger LoRA self-fine-tuning cycle."""
    payload = {
        "action": "train",
        "trigger": req.trigger,
        "min_patches": req.min_patches,
        "timestamp": datetime.now(timezone.utc).isoformat(),
    }
    _publish(TOPIC_TRAINING, payload)

    logger.info(f"Training triggered: source={req.trigger}, min_patches={req.min_patches}")
    return {"status": "training_dispatched", "trigger": req.trigger}


@app.post("/silence")
async def silence_control(req: SilenceRequest):
    """Publish PAUSE or RESUME signals to all agents."""
    payload = {
        "action": req.action,
        "reason": req.reason,
        "timestamp": datetime.now(timezone.utc).isoformat(),
    }
    _publish(TOPIC_SILENCE, payload)

    logger.info(f"Silence signal: {req.action} — reason: {req.reason}")
    return {"status": f"signal_{req.action.lower()}_sent", "reason": req.reason}


@app.post("/waitlist")
async def waitlist_signup(req: WaitlistSignup):
    """Capture waitlist email and store in Firestore."""
    db = get_firestore()

    # Deduplicate by email hash
    email_hash = hashlib.sha256(req.email.lower().strip().encode()).hexdigest()

    existing = db.collection("waitlist").document(email_hash).get()
    if existing.exists:
        return {"status": "already_registered", "tier": existing.to_dict().get("tier", "free")}

    doc = {
        "email": req.email.lower().strip(),
        "email_hash": email_hash,
        "tier": req.tier,
        "signed_up_at": datetime.now(timezone.utc).isoformat(),
        "source": "aeon-site",
        "notified": False,
    }
    db.collection("waitlist").document(email_hash).set(doc)

    logger.info(f"Waitlist signup: tier={req.tier}, hash={email_hash[:8]}...")
    return {"status": "registered", "tier": req.tier}


@app.get("/stats")
async def get_stats():
    """Return public-facing stats for the landing page."""
    db = get_firestore()

    # Count documents in key collections
    waitlist_count = len(list(db.collection("waitlist").limit(10000).stream()))
    scan_count = len(list(db.collection("evolution_scans").limit(10000).stream()))
    patch_count = len(list(db.collection("patches").limit(10000).stream()))

    return {
        "waitlist_signups": waitlist_count,
        "cve_scans_completed": scan_count,
        "patches_generated": patch_count,
        "p2p_peers_simulated": 847,
        "os_tiers_supported": 8,
    }


# ── Internal Helpers ────────────────────────────────────────────────────

def _publish(topic: str, data: dict):
    """Publish a JSON message to a Pub/Sub topic."""
    publisher = get_publisher()
    message = json.dumps(data).encode("utf-8")
    future = publisher.publish(topic, message)
    logger.debug(f"Published to {topic}: message_id={future.result()}")


# ── Main ────────────────────────────────────────────────────────────────

if __name__ == "__main__":
    import uvicorn
    uvicorn.run(app, host="0.0.0.0", port=PORT)
