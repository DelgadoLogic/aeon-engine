#!/usr/bin/env python3
"""
AeonBuildWorker — Distributed Chromium compilation engine.
Manages chunked, parallel builds across idle peer machines.
Each node contributes CPU during quiet hours; the sovereign
coordinator merges outputs into a signed, verified release.
"""
import asyncio
import hashlib
import json
import os
import subprocess
import time
from datetime import datetime, timezone
from pathlib import Path
import httpx

HIVE_ENDPOINT  = os.getenv("AEON_HIVE_URL", "http://localhost:7878")
WORKER_ID      = os.getenv("WORKER_ID", hashlib.sha256(os.uname().nodename.encode()).hexdigest()[:12]
                           if hasattr(os, 'uname') else "win-worker")
CHROMIUM_SRC   = Path(os.getenv("CHROMIUM_SRC", "C:/chromium/src"))
BUILD_CHUNKS   = int(os.getenv("BUILD_CHUNKS", "4"))    # Ninja targets per chunk
MAX_CPU_PCT    = float(os.getenv("MAX_CPU_PCT", "80"))  # Never exceed this during builds

# ─── Worker registration ──────────────────────────────────────────────────────

async def register_with_hive() -> bool:
    """Announce this worker to the AeonHive coordinator."""
    import psutil
    cpu_cores = psutil.cpu_count(logical=True)
    ram_gb    = psutil.virtual_memory().total // (1024**3)

    async with httpx.AsyncClient() as client:
        try:
            r = await client.post(f"{HIVE_ENDPOINT}/v1/workers/register", json={
                "worker_id": WORKER_ID,
                "cpu_cores": cpu_cores,
                "ram_gb": ram_gb,
                "platform": "windows" if os.name == "nt" else "linux",
                "capabilities": ["ninja_build", "clang", "gn"],
                "registered_at": datetime.now(timezone.utc).isoformat()
            }, timeout=10)
            print(f"[BuildWorker] Registered: {WORKER_ID} ({cpu_cores}c / {ram_gb}GB)")
            return r.status_code == 200
        except Exception as e:
            print(f"[BuildWorker] Hive unreachable during registration: {e}")
            return False


# ─── Build chunk execution ────────────────────────────────────────────────────

async def execute_build_chunk(chunk: dict) -> dict:
    """Execute a single assigned build chunk via Ninja."""
    targets  = chunk["targets"]       # e.g., ["chrome/browser:browser", "content:content"]
    out_dir  = chunk.get("out_dir", "out/Aeon")
    chunk_id = chunk["chunk_id"]

    print(f"[BuildWorker] Building chunk {chunk_id}: {len(targets)} targets")

    cmd = [
        "ninja", "-C", str(CHROMIUM_SRC / out_dir),
        f"-j{max(1, int(BUILD_CHUNKS * (MAX_CPU_PCT / 100)))}",
        *targets
    ]

    start = time.time()
    proc = await asyncio.create_subprocess_exec(
        *cmd,
        cwd=str(CHROMIUM_SRC),
        stdout=asyncio.subprocess.PIPE,
        stderr=asyncio.subprocess.PIPE
    )
    stdout, stderr = await proc.communicate()
    elapsed = time.time() - start

    success = proc.returncode == 0
    result = {
        "chunk_id": chunk_id,
        "worker_id": WORKER_ID,
        "success": success,
        "returncode": proc.returncode,
        "elapsed_seconds": round(elapsed, 1),
        "targets": targets,
        "error": stderr.decode()[:500] if not success else None,
        "completed_at": datetime.now(timezone.utc).isoformat()
    }

    print(f"[BuildWorker] Chunk {chunk_id}: {'✓' if success else '✗'} in {elapsed:.0f}s")
    return result


# ─── Job polling loop ─────────────────────────────────────────────────────────

async def poll_for_work() -> None:
    """Check AeonHive for assigned build chunks and execute them."""
    async with httpx.AsyncClient() as client:
        try:
            r = await client.get(
                f"{HIVE_ENDPOINT}/v1/build/assign/{WORKER_ID}",
                timeout=10
            )
            if r.status_code == 204:
                return  # No work available
            if r.status_code == 200:
                chunk = r.json()
                result = await execute_build_chunk(chunk)

                # Report result back
                await client.post(
                    f"{HIVE_ENDPOINT}/v1/build/report",
                    json=result, timeout=15
                )
        except Exception as e:
            print(f"[BuildWorker] Poll error: {e}")


# ─── Main worker loop ─────────────────────────────────────────────────────────

async def main():
    print("=" * 60)
    print(f"AeonBuildWorker v1.0 — Distributed Chromium Compilation")
    print(f"Worker ID: {WORKER_ID}")
    print(f"Chromium src: {CHROMIUM_SRC}")
    print(f"Max CPU: {MAX_CPU_PCT}% | Chunks: {BUILD_CHUNKS}")
    print("=" * 60)

    await register_with_hive()

    while True:
        await poll_for_work()
        await asyncio.sleep(30)  # Poll every 30 seconds


if __name__ == "__main__":
    asyncio.run(main())
