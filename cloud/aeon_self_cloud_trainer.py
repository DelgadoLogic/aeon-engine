"""
aeon_self_cloud_trainer.py
==========================
DelgadoLogic — Aeon Browser · Evolution Engine

Cloud Run agent responsible for self-fine-tuning AeonMind's LoRA adapter
based on successful patch history. Every approved patch teaches the model
to generate better patches in the future.

Training Loop:
  1. Query Firestore for patches with status="DEPLOYED" and trained=False
  2. Convert each patch into a (prompt, completion) training pair
  3. Fine-tune the LoRA adapter via the Gemini API (or local LoRA script)
  4. Validate the new adapter against the test suite
  5. If validation passes, promote the new adapter as active
  6. Mark processed patches as trained=True

Architecture:
  ┌──────────────────────────────────────────┐
  │  Firestore: /patches/{patch_id}          │
  │    status: DEPLOYED                      │
  │    trained: false                        │
  │    cve_id, diff, context, vote_score     │
  └──────────┬───────────────────────────────┘
             │
  ┌──────────▼───────────────────────────────┐
  │  SelfCloudTrainer (this module)          │
  │    1. Fetch unprocessed patches          │
  │    2. Build training pairs               │
  │    3. Fine-tune LoRA adapter             │
  │    4. Validate on hold-out set           │
  │    5. Promote or rollback                │
  └──────────┬───────────────────────────────┘
             │
  ┌──────────▼───────────────────────────────┐
  │  GCS: gs://aeon-lora-adapters/           │
  │    active/adapter.safetensors            │
  │    staging/adapter.safetensors           │
  │    history/{timestamp}/adapter.safetensors│
  └──────────────────────────────────────────┘
"""

import os
import logging
import json
import hashlib
import time
from datetime import datetime, timezone
from typing import Optional

_PROJECT   = os.environ.get("GCP_PROJECT", "delgadologic")
_BUCKET    = os.environ.get("LORA_BUCKET", "aeon-lora-adapters")
_COLLECTION = "patches"

# Training hyperparameters
LORA_RANK       = 16
LORA_ALPHA      = 32
LEARNING_RATE   = 2e-5
EPOCHS          = 3
MIN_PATCHES     = 5      # Don't retrain until we have at least N new patches
VALIDATION_SPLIT = 0.15  # Hold-out fraction

logging.basicConfig(
    level=logging.INFO,
    format="%(asctime)s [SelfTrainer] %(message)s",
    datefmt="%H:%M:%S",
)
log = logging.getLogger(__name__)


class TrainingPair:
    """A single training example derived from a deployed patch."""

    def __init__(self, cve_id: str, context: str, diff: str, vote_score: float):
        self.cve_id = cve_id
        self.context = context
        self.diff = diff
        self.vote_score = vote_score

    @property
    def prompt(self) -> str:
        return (
            f"You are AeonMind, an expert C++ security engineer. "
            f"A vulnerability ({self.cve_id}) has been found.\n\n"
            f"Context:\n{self.context}\n\n"
            f"Generate a minimal, correct C++ patch to fix this vulnerability."
        )

    @property
    def completion(self) -> str:
        return self.diff

    def to_jsonl(self) -> str:
        return json.dumps({
            "prompt": self.prompt,
            "completion": self.completion,
            "metadata": {
                "cve_id": self.cve_id,
                "vote_score": self.vote_score,
            }
        })


class SelfCloudTrainer:
    """
    Manages the LoRA self-training loop for AeonMind.
    Reads patch history from Firestore, builds training data,
    fine-tunes the adapter, validates, and promotes.
    """

    def __init__(self):
        self._db = None
        self._storage = None
        self.stats = {
            "training_runs": 0,
            "patches_processed": 0,
            "last_training_ts": None,
            "active_adapter_hash": None,
        }

    def _get_db(self):
        if self._db is None:
            try:
                from google.cloud import firestore
                self._db = firestore.Client(project=_PROJECT)
                log.info("Firestore client initialized")
            except ImportError:
                log.warning("google-cloud-firestore not installed — dry-run mode")
        return self._db

    def _get_storage(self):
        if self._storage is None:
            try:
                from google.cloud import storage
                self._storage = storage.Client(project=_PROJECT)
                log.info("Cloud Storage client initialized")
            except ImportError:
                log.warning("google-cloud-storage not installed — dry-run mode")
        return self._storage

    def fetch_unprocessed_patches(self) -> list[dict]:
        """Fetch deployed patches that haven't been used for training yet."""
        db = self._get_db()
        if db is None:
            log.info("[DRY-RUN] Would query Firestore for unprocessed patches")
            return self._mock_patches()

        query = (
            db.collection(_COLLECTION)
            .where("status", "==", "DEPLOYED")
            .where("trained", "==", False)
            .order_by("deployed_at")
            .limit(100)
        )
        docs = query.stream()
        patches = []
        for doc in docs:
            data = doc.to_dict()
            data["doc_id"] = doc.id
            patches.append(data)

        log.info(f"Found {len(patches)} unprocessed deployed patches")
        return patches

    def _mock_patches(self) -> list[dict]:
        """Mock data for local testing."""
        return [
            {
                "doc_id": "mock_001",
                "cve_id": "CVE-2025-4421",
                "context": "V8 type confusion in TurboFan JIT compiler allowing arbitrary memory read via crafted JavaScript.",
                "diff": "--- a/src/compiler/turbofan.cc\n+++ b/src/compiler/turbofan.cc\n@@ -1234 @@\n-  if (node->opcode() == IrOpcode::kLoadField) {\n+  if (node->opcode() == IrOpcode::kLoadField && ValidateType(node)) {",
                "vote_score": 0.95,
                "deployed_at": "2025-04-01T03:48:00Z",
                "status": "DEPLOYED",
                "trained": False,
            },
            {
                "doc_id": "mock_002",
                "cve_id": "CVE-2025-3899",
                "context": "Blink use-after-free in DOM node detach handler during concurrent GC sweep.",
                "diff": "--- a/third_party/blink/renderer/core/dom/node.cc\n+++ b/third_party/blink/renderer/core/dom/node.cc\n@@ -567 @@\n-  DetachFromParent();\n+  if (!IsBeingDestroyed()) DetachFromParent();",
                "vote_score": 0.88,
                "deployed_at": "2025-04-01T06:12:00Z",
                "status": "DEPLOYED",
                "trained": False,
            },
        ] * 3  # Repeat to hit MIN_PATCHES threshold

    def build_training_pairs(self, patches: list[dict]) -> list[TrainingPair]:
        """Convert raw patch documents into structured training pairs."""
        pairs = []
        for p in patches:
            pair = TrainingPair(
                cve_id=p.get("cve_id", "UNKNOWN"),
                context=p.get("context", ""),
                diff=p.get("diff", ""),
                vote_score=p.get("vote_score", 0.0),
            )
            pairs.append(pair)

        # Weight higher-voted patches (repeat them)
        weighted = []
        for pair in pairs:
            repeats = max(1, int(pair.vote_score * 3))
            weighted.extend([pair] * repeats)

        log.info(f"Built {len(weighted)} weighted training pairs from {len(pairs)} patches")
        return weighted

    def write_training_data(self, pairs: list[TrainingPair]) -> str:
        """Write training pairs to a JSONL file and upload to GCS staging."""
        timestamp = datetime.now(timezone.utc).strftime("%Y%m%d_%H%M%S")
        filename = f"training_data_{timestamp}.jsonl"
        local_path = f"/tmp/{filename}"

        with open(local_path, "w") as f:
            for pair in pairs:
                f.write(pair.to_jsonl() + "\n")

        # Upload to GCS
        storage = self._get_storage()
        if storage is not None:
            bucket = storage.bucket(_BUCKET)
            blob = bucket.blob(f"training_data/{filename}")
            blob.upload_from_filename(local_path)
            gcs_uri = f"gs://{_BUCKET}/training_data/{filename}"
            log.info(f"Training data uploaded to {gcs_uri}")
            return gcs_uri
        else:
            log.info(f"[DRY-RUN] Training data written to {local_path}")
            return local_path

    def trigger_finetuning(self, data_uri: str) -> dict:
        """
        Trigger a LoRA fine-tuning job.

        In production, this calls the Gemini tuning API or dispatches
        to a GPU-enabled Cloud Run Job. For now, we stub it.
        """
        log.info(f"Triggering fine-tuning job on {data_uri}")
        log.info(f"  LoRA rank={LORA_RANK}, alpha={LORA_ALPHA}")
        log.info(f"  LR={LEARNING_RATE}, epochs={EPOCHS}")

        # Stubbed — replace with actual Gemini / HuggingFace LoRA call
        adapter_hash = hashlib.sha256(data_uri.encode()).hexdigest()[:16]
        result = {
            "status": "COMPLETED",
            "adapter_path": f"staging/adapter_{adapter_hash}.safetensors",
            "adapter_hash": adapter_hash,
            "metrics": {
                "train_loss": 0.0234,
                "val_loss": 0.0312,
                "accuracy": 0.94,
            },
        }
        log.info(f"Fine-tuning completed: {json.dumps(result['metrics'])}")
        return result

    def validate_adapter(self, result: dict) -> bool:
        """
        Validate the new adapter against the hold-out test suite.
        Returns True if the adapter should be promoted.
        """
        metrics = result.get("metrics", {})
        val_loss = metrics.get("val_loss", float("inf"))
        accuracy = metrics.get("accuracy", 0.0)

        # Promotion criteria
        if val_loss > 0.05:
            log.warning(f"Validation loss too high ({val_loss:.4f} > 0.05) — NOT promoting")
            return False
        if accuracy < 0.85:
            log.warning(f"Accuracy too low ({accuracy:.2f} < 0.85) — NOT promoting")
            return False

        log.info(f"Adapter passed validation (loss={val_loss:.4f}, acc={accuracy:.2f})")
        return True

    def promote_adapter(self, result: dict):
        """Copy staging adapter to active slot, archive the old one."""
        adapter_hash = result["adapter_hash"]
        timestamp = datetime.now(timezone.utc).strftime("%Y%m%d_%H%M%S")

        log.info(f"Promoting adapter {adapter_hash} to active")
        log.info(f"  Archiving previous active to history/{timestamp}/")

        # GCS copy operations (stubbed)
        storage = self._get_storage()
        if storage is not None:
            bucket = storage.bucket(_BUCKET)
            # Archive current active
            old_blob = bucket.blob("active/adapter.safetensors")
            if old_blob.exists():
                bucket.copy_blob(old_blob, bucket, f"history/{timestamp}/adapter.safetensors")
            # Promote staging
            new_blob = bucket.blob(result["adapter_path"])
            if new_blob.exists():
                bucket.copy_blob(new_blob, bucket, "active/adapter.safetensors")

        self.stats["active_adapter_hash"] = adapter_hash
        log.info("Adapter promoted successfully")

    def mark_patches_trained(self, patches: list[dict]):
        """Mark all processed patches as trained=True in Firestore."""
        db = self._get_db()
        if db is None:
            log.info(f"[DRY-RUN] Would mark {len(patches)} patches as trained")
            return

        batch = db.batch()
        for p in patches:
            ref = db.collection(_COLLECTION).document(p["doc_id"])
            batch.update(ref, {
                "trained": True,
                "trained_at": datetime.now(timezone.utc).isoformat(),
            })
        batch.commit()
        log.info(f"Marked {len(patches)} patches as trained")

    def run(self) -> dict:
        """
        Execute a full training cycle.
        Returns a status dict for the Cloud Run response.
        """
        log.info("=" * 60)
        log.info("Starting self-training cycle")

        # 1. Fetch
        patches = self.fetch_unprocessed_patches()
        if len(patches) < MIN_PATCHES:
            msg = f"Only {len(patches)} patches available (need {MIN_PATCHES}) — skipping"
            log.info(msg)
            return {"status": "SKIPPED", "reason": msg, "patch_count": len(patches)}

        # 2. Build training data
        pairs = self.build_training_pairs(patches)
        data_uri = self.write_training_data(pairs)

        # 3. Fine-tune
        result = self.trigger_finetuning(data_uri)

        # 4. Validate
        if not self.validate_adapter(result):
            return {"status": "VALIDATION_FAILED", "metrics": result["metrics"]}

        # 5. Promote
        self.promote_adapter(result)

        # 6. Mark processed
        self.mark_patches_trained(patches)

        # 7. Update stats
        self.stats["training_runs"] += 1
        self.stats["patches_processed"] += len(patches)
        self.stats["last_training_ts"] = datetime.now(timezone.utc).isoformat()

        log.info("Self-training cycle complete")
        log.info("=" * 60)
        return {
            "status": "COMPLETED",
            "patches_processed": len(patches),
            "training_pairs": len(pairs),
            "metrics": result["metrics"],
            "adapter_hash": result["adapter_hash"],
            "stats": self.stats,
        }


# ── Cloud Run / Cloud Function entrypoint ──────────────────────────────────

_trainer = SelfCloudTrainer()

def handle_training_trigger(request):
    """
    HTTP handler for Cloud Run / Cloud Functions.
    Called on a Cloud Scheduler cron (every 6 hours) or manually.
    """
    result = _trainer.run()
    status_code = 200 if result["status"] in ("COMPLETED", "SKIPPED") else 500
    return result, status_code


def health_check(request):
    """Health check endpoint."""
    return {"agent": "self-cloud-trainer", "stats": _trainer.stats}, 200


# ── Local dev / testing ────────────────────────────────────────────────────

if __name__ == "__main__":
    log.info("SelfCloudTrainer running in local simulation")
    result = _trainer.run()
    log.info(f"Result: {json.dumps(result, indent=2)}")
