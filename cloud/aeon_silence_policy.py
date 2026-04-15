"""
aeon_silence_policy.py
======================
DelgadoLogic — Aeon Browser · Evolution Engine

Cloud Function / Cloud Run agent responsible for enforcing the
"Absolute Silence" contract: all background operations (research,
compilation, patching, seeding) MUST pause when the user is actively
using the browser and resume when the user goes idle.

This module runs as a lightweight daemon on each Aeon deployment and
communicates with the other Evolution Engine agents via Cloud Pub/Sub.

Architecture:
  ┌──────────────────────┐
  │  User Activity Probe │  (local C++ component, reports via IPC)
  │         │            │
  │         ▼            │
  │  SilencePolicyDaemon │  ← this module
  │    │           │     │
  │    ▼           ▼     │
  │ Pub/Sub:     Pub/Sub:│
  │ PAUSE_ALL    RESUME  │
  └──────────────────────┘

Pub/Sub topics:
  aeon-silence-pause   → all agents subscribe; pauses their work loops
  aeon-silence-resume  → all agents subscribe; resumes their work loops

gRPC probe signal from C++ side: ActivityStatus { idle_seconds: u32 }
"""

import os
import logging
import time
import json
from datetime import datetime, timezone

# Google Cloud clients (lazy-init for cold-start perf)
_publisher = None
_PROJECT   = os.environ.get("GCP_PROJECT", "delgadologic")
_TOPIC_PAUSE  = f"projects/{_PROJECT}/topics/aeon-silence-pause"
_TOPIC_RESUME = f"projects/{_PROJECT}/topics/aeon-silence-resume"

# Silence thresholds (seconds)
IDLE_THRESHOLD  = 120   # Seconds of inactivity before ops resume
ACTIVE_COOLDOWN = 3     # Seconds of activity before ops pause

logging.basicConfig(
    level=logging.INFO,
    format="%(asctime)s [SilencePolicy] %(message)s",
    datefmt="%H:%M:%S",
)
log = logging.getLogger(__name__)


class SilenceState:
    """Tracks current user activity state."""
    ACTIVE = "ACTIVE"
    IDLE   = "IDLE"


class SilencePolicyDaemon:
    """
    Monitors user activity reports from the C++ browser core and
    publishes PAUSE/RESUME events to Pub/Sub so all Evolution Engine
    agents respect the Absolute Silence contract.
    """

    def __init__(self):
        self.state = SilenceState.IDLE
        self.last_activity_ts = 0.0
        self.last_state_change = datetime.now(timezone.utc)
        self.stats = {
            "pause_count": 0,
            "resume_count": 0,
            "total_paused_seconds": 0,
        }

    def _get_publisher(self):
        global _publisher
        if _publisher is None:
            try:
                from google.cloud import pubsub_v1
                _publisher = pubsub_v1.PublisherClient()
                log.info("Pub/Sub publisher initialized")
            except ImportError:
                log.warning("google-cloud-pubsub not installed — dry-run mode")
        return _publisher

    def _publish(self, topic: str, data: dict):
        """Publish a command to a Pub/Sub topic."""
        pub = self._get_publisher()
        if pub is None:
            log.info(f"[DRY-RUN] Would publish to {topic}: {data}")
            return

        payload = json.dumps({
            **data,
            "timestamp": datetime.now(timezone.utc).isoformat(),
            "source": "silence-policy",
        }).encode("utf-8")

        future = pub.publish(topic, payload)
        msg_id = future.result(timeout=10)
        log.info(f"Published to {topic} (msg_id={msg_id})")

    def on_activity_report(self, idle_seconds: int):
        """
        Called when the C++ probe sends an ActivityStatus report.

        Args:
            idle_seconds: Number of seconds since the last user input event.
        """
        self.last_activity_ts = time.time()

        if idle_seconds < ACTIVE_COOLDOWN and self.state == SilenceState.IDLE:
            self._transition_to_active()
        elif idle_seconds >= IDLE_THRESHOLD and self.state == SilenceState.ACTIVE:
            self._transition_to_idle()

    def _transition_to_active(self):
        """User became active — PAUSE all background operations."""
        now = datetime.now(timezone.utc)
        idle_duration = (now - self.last_state_change).total_seconds()

        self.state = SilenceState.ACTIVE
        self.last_state_change = now
        self.stats["pause_count"] += 1

        log.info(f"→ ACTIVE (was idle for {idle_duration:.0f}s) — pausing operations")
        self._publish(_TOPIC_PAUSE, {
            "command": "PAUSE_ALL",
            "reason": "user_active",
            "idle_duration_secs": idle_duration,
        })

    def _transition_to_idle(self):
        """User went idle — RESUME all background operations."""
        now = datetime.now(timezone.utc)
        active_duration = (now - self.last_state_change).total_seconds()

        self.state = SilenceState.IDLE
        self.last_state_change = now
        self.stats["resume_count"] += 1
        self.stats["total_paused_seconds"] += active_duration

        log.info(f"→ IDLE (was active for {active_duration:.0f}s) — resuming operations")
        self._publish(_TOPIC_RESUME, {
            "command": "RESUME_ALL",
            "reason": "user_idle",
            "active_duration_secs": active_duration,
        })

    def get_status(self) -> dict:
        """Return current silence policy state for health checks."""
        return {
            "state": self.state,
            "last_state_change": self.last_state_change.isoformat(),
            "uptime_seconds": time.time() - self.last_activity_ts if self.last_activity_ts else 0,
            "stats": self.stats,
        }


# ── Cloud Run / Cloud Function entrypoint ──────────────────────────────────

_daemon = SilencePolicyDaemon()

def handle_activity_report(request):
    """
    HTTP Cloud Function / Cloud Run handler.
    Expects POST with JSON body: { "idle_seconds": <int> }
    Called by the C++ browser's ActivityProbe via the local gRPC bridge.
    """
    if request.method != "POST":
        return {"error": "POST required"}, 405

    body = request.get_json(silent=True) or {}
    idle_seconds = body.get("idle_seconds", 0)

    if not isinstance(idle_seconds, (int, float)):
        return {"error": "idle_seconds must be a number"}, 400

    _daemon.on_activity_report(int(idle_seconds))
    return _daemon.get_status(), 200


def health_check(request):
    """Health check endpoint for Cloud Run."""
    return _daemon.get_status(), 200


# ── Local dev / testing ────────────────────────────────────────────────────

if __name__ == "__main__":
    log.info("SilencePolicy running in local simulation mode")
    log.info(f"  IDLE_THRESHOLD  = {IDLE_THRESHOLD}s")
    log.info(f"  ACTIVE_COOLDOWN = {ACTIVE_COOLDOWN}s")

    # Simulate user activity cycle
    scenarios = [
        (1,   "User typing"),
        (2,   "User scrolling"),
        (0,   "User clicked"),
        (60,  "User paused (1min)"),
        (121, "User idle > threshold"),
        (150, "Still idle"),
        (0,   "User returned!"),
    ]
    for idle, desc in scenarios:
        log.info(f"  Sim: {desc} (idle_seconds={idle})")
        _daemon.on_activity_report(idle)
        time.sleep(0.3)

    log.info(f"Final stats: {json.dumps(_daemon.get_status(), indent=2)}")
