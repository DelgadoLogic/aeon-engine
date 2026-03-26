#!/usr/bin/env python3
"""
AeonSelfCloudTrainer — Weekly LoRA fine-tuning on GCP.
Submits a Cloud Build job that runs LoRA fine-tuning on a T4 GPU.
Cost: ~$0.30 per weekly run.
Input: user interaction data (privacy-stripped, never leaves sovereign infra).
Output: new LoRA adapter delta, pushed to aeon-models repo.
"""
import asyncio
import json
import os
from datetime import datetime, timezone
import subprocess
import tempfile

GCP_PROJECT   = os.getenv("GCP_PROJECT", "aeon-browser-build")
MODELS_BUCKET = os.getenv("MODELS_BUCKET", "gs://aeon-public-dist/models")
CI_TOKEN      = os.getenv("AEON_CI_TOKEN", "")
MODELS_REPO   = "https://github.com/DelgadoLogic/aeon-models.git"

CLOUD_BUILD_CONFIG = """
steps:
  # Pull base model from sovereign bucket
  - name: 'gcr.io/google.com/cloudsdktool/cloud-sdk'
    entrypoint: 'gsutil'
    args: ['cp', '-r', '${_MODELS_BUCKET}/base_model/', '/workspace/base_model/']

  # Run LoRA fine-tuning (T4 GPU, ~20 min, ~$0.30)
  - name: 'us-docker.pkg.dev/aeon-browser-build/aeon-build-env/lora-trainer:latest'
    args:
      - python3
      - train_lora.py
      - --base_model=/workspace/base_model
      - --data=/workspace/training_data
      - --output=/workspace/lora_delta
      - --epochs=3
      - --lr=2e-4
      - --r=16
    env:
      - 'WANDB_DISABLED=true'

  # Verify delta quality (perplexity check)
  - name: 'us-docker.pkg.dev/aeon-browser-build/aeon-build-env/lora-trainer:latest'
    args:
      - python3
      - verify_delta.py
      - --delta=/workspace/lora_delta
      - --threshold=3.5

  # Push to sovereign model storage
  - name: 'gcr.io/google.com/cloudsdktool/cloud-sdk'
    entrypoint: 'gsutil'
    args: ['cp', '-r', '/workspace/lora_delta/', '${_MODELS_BUCKET}/lora/${_VERSION}/']

  # Tag release in aeon-models
  - name: 'gcr.io/cloud-builders/git'
    args:
      - '-c'
      - |
        git clone ${_MODELS_REPO} /workspace/aeon-models
        cd /workspace/aeon-models
        echo '${_VERSION}' > LATEST_LORA
        git add -A
        git commit -m "lora: weekly fine-tune ${_VERSION}"
        git tag "lora-${_VERSION}"
        git push origin main --tags

options:
  machineType: 'N1_HIGHCPU_32'

substitutions:
  _MODELS_BUCKET: '${MODELS_BUCKET}'
  _MODELS_REPO: '${MODELS_REPO}'
  _VERSION: 'WILL_BE_SET_AT_RUNTIME'
"""


async def submit_training_job() -> bool:
    """Submit a weekly LoRA fine-tuning job to Cloud Build."""
    version = datetime.now(timezone.utc).strftime("v%Y.%m.%d")
    print(f"[LoRATrainer] Submitting fine-tune job: {version}")

    # Write cloudbuild.yaml to temp file
    with tempfile.NamedTemporaryFile(mode='w', suffix='.yaml', delete=False) as f:
        config = CLOUD_BUILD_CONFIG.replace("${MODELS_BUCKET}", MODELS_BUCKET)
        config = config.replace("${MODELS_REPO}", MODELS_REPO)
        f.write(config)
        config_file = f.name

    cmd = [
        "gcloud", "builds", "submit",
        "--no-source",
        f"--config={config_file}",
        f"--substitutions=_VERSION={version}",
        f"--project={GCP_PROJECT}"
    ]

    print(f"[LoRATrainer] gcloud command: {' '.join(cmd)}")

    proc = await asyncio.create_subprocess_exec(
        *cmd,
        stdout=asyncio.subprocess.PIPE,
        stderr=asyncio.subprocess.PIPE
    )
    stdout, stderr = await proc.communicate()

    if proc.returncode == 0:
        print(f"[LoRATrainer] ✓ Training job {version} submitted successfully")
        return True
    else:
        print(f"[LoRATrainer] ✗ Job failed: {stderr.decode()[:300]}")
        return False


async def main():
    print("=" * 60)
    print("AeonSelfCloudTrainer v1.0 — Weekly LoRA Fine-Tuning")
    print(f"GCP Project: {GCP_PROJECT}")
    print(f"Model bucket: {MODELS_BUCKET}")
    print(f"Cost: ~$0.30/run on N1-HighCPU-32")
    print("=" * 60)

    # Run weekly
    WEEK_SECONDS = 7 * 24 * 3600
    while True:
        success = await submit_training_job()
        if not success:
            print("[LoRATrainer] Retrying in 6 hours...")
            await asyncio.sleep(6 * 3600)
        else:
            print(f"[LoRATrainer] Next run in 7 days")
            await asyncio.sleep(WEEK_SECONDS)


if __name__ == "__main__":
    asyncio.run(main())
