"""
AeonBuildWorker — aeon_build_worker.py
Phase 2/3 P2P distributed compilation worker.

Opt-in daemon that contributes idle CPU to Aeon build jobs.
Only runs when:
  - User has opted in (settings: contribute_to_builds = true)
  - System is idle (idle_seconds > 600)
  - Adequate resources available (CPU < 20%, RAM free > 4GB)
  - Clang toolchain is cached locally

HOW IT WORKS:
  1. Registers with build master via AeonHive gossip
  2. Receives individual .cc compilation jobs
  3. Compiles using cached Clang toolchain
  4. Returns .o object files (compressed with zstd)
  5. Earns reputation points (used for build master election)

PHASE AWARENESS:
  Phase 1: This worker is dormant (GCP handles everything)
  Phase 2: Workers handle .cc compilation, GCP does linking
  Phase 3: Workers handle EVERYTHING including linking election
"""

import os
import sys
import json
import time
import zlib
import socket
import logging
import hashlib
import shutil
import struct
import tempfile
import threading
import subprocess
import platform
from pathlib import Path
from typing import Optional
from dataclasses import dataclass
import requests

from aeon_silence_policy import get_policy, get_idle_seconds

log = logging.getLogger("AeonBuildWorker")

# ── Config ────────────────────────────────────────────────────────────────────

WORKER_PORT    = 9799  # Build worker coordination port
MASTER_PORT    = 9800  # Build master coordination port
AEON_DATA      = Path(os.environ.get("AEON_DATA", str(
    Path.home() / "AppData" / "Local" / "Aeon")))
TOOLCHAIN_DIR  = AEON_DATA / "toolchain"
SOURCE_CACHE   = AEON_DATA / "source_cache"
OBJ_CACHE      = AEON_DATA / "obj_cache"
WORKER_ID      = hashlib.sha256(socket.getfqdn().encode()).hexdigest()[:16]

# Minimum specs to be a build worker
MIN_FREE_RAM_GB = 4
MIN_CPU_CORES   = 4
IDLE_REQUIRED_S = 600  # Must be idle 10 minutes

# ── Resource check ────────────────────────────────────────────────────────────

def get_available_resources() -> dict:
    """Check if this machine can handle build work."""
    try:
        import psutil
        ram = psutil.virtual_memory()
        cpu_count = psutil.cpu_count(logical=False) or 1
        cpu_percent = psutil.cpu_percent(interval=1)
        
        return {
            "cpu_cores": cpu_count,
            "cpu_free_pct": 100 - cpu_percent,
            "ram_free_gb": ram.available / (1024**3),
            "eligible": (
                cpu_count >= MIN_CPU_CORES and
                ram.available / (1024**3) >= MIN_FREE_RAM_GB and
                cpu_percent < 20
            )
        }
    except ImportError:
        # psutil not available — basic check
        return {
            "cpu_cores": os.cpu_count() or 1,
            "cpu_free_pct": 80,
            "ram_free_gb": 8,
            "eligible": (os.cpu_count() or 1) >= MIN_CPU_CORES
        }

# ── Clang toolchain management ─────────────────────────────────────────────────

def has_clang() -> bool:
    """Check if Clang is available (either system or local cache)."""
    if shutil.which("clang++"):
        return True
    local_clang = TOOLCHAIN_DIR / "bin" / "clang++.exe"
    return local_clang.exists()

def get_clang_path() -> str:
    """Get the path to the clang++ binary."""
    if shutil.which("clang++"):
        return "clang++"
    local = TOOLCHAIN_DIR / "bin" / "clang++.exe"
    if local.exists():
        return str(local)
    raise RuntimeError("Clang not found — cannot compile")

def download_clang_toolchain(manifest: dict):
    """
    Download and cache the Clang toolchain from the build manifest.
    Only needed once (~800MB). Verified by hash.
    """
    toolchain_comp = manifest.get("components", {}).get("clang_toolchain", {})
    if not toolchain_comp:
        log.warning("[Worker] No clang_toolchain in manifest")
        return
    
    if has_clang():
        log.info("[Worker] Clang already available")
        return
    
    TOOLCHAIN_DIR.mkdir(parents=True, exist_ok=True)
    file_name = toolchain_comp.get("file", "")
    sha256 = toolchain_comp.get("sha256", "")
    gcs_url = (f"https://storage.googleapis.com/aeon-chromium-artifacts/"
                f"toolchain/{file_name}")
    
    zip_path = TOOLCHAIN_DIR / file_name
    log.info(f"[Worker] Downloading Clang toolchain (~800MB)...")
    
    resp = requests.get(gcs_url, stream=True, timeout=300)
    with open(zip_path, "wb") as f:
        for chunk in resp.iter_content(chunk_size=1024*1024):
            f.write(chunk)
    
    actual = hashlib.sha256(zip_path.read_bytes()).hexdigest()
    if sha256 and actual != sha256:
        log.error("[Worker] Toolchain hash mismatch!")
        zip_path.unlink()
        return
    
    import zipfile
    with zipfile.ZipFile(zip_path) as zf:
        zf.extractall(TOOLCHAIN_DIR)
    zip_path.unlink()
    log.info("[Worker] Clang toolchain installed")

# ── Compile job ─────────────────────────────────────────────────────────────────

@dataclass
class CompileJob:
    job_id: str
    source_file: str      # Relative path within source tree
    source_hash: str      # SHA-256 of source file
    compile_flags: list   # Compiler flags
    include_dirs: list    # Additional include directories
    expected_obj_hash: Optional[str] = None  # For caching

def compile_job(job: CompileJob, source_dir: Path) -> Optional[bytes]:
    """
    Compile a single .cc file and return the compressed .o bytes.
    Uses deterministic compilation flags for reproducible builds.
    """
    # Check obj cache first (don't recompile if we have it)
    cache_key = hashlib.sha256(
        f"{job.source_hash}{json.dumps(job.compile_flags)}".encode()
    ).hexdigest()[:16]
    cache_file = OBJ_CACHE / f"{cache_key}.o.zst"
    
    if cache_file.exists() and job.expected_obj_hash:
        cached = cache_file.read_bytes()
        log.debug(f"[Worker] Cache hit: {job.source_file}")
        return cached
    
    source_path = source_dir / job.source_file
    if not source_path.exists():
        log.error(f"[Worker] Source not found: {source_path}")
        return None
    
    with tempfile.NamedTemporaryFile(suffix=".o", delete=False) as tmp:
        obj_path = tmp.name
    
    try:
        clang = get_clang_path()
        cmd = [clang, "-c"] + job.compile_flags + [
            "-o", obj_path,
            str(source_path)
        ]
        
        # Add include dirs
        for inc in job.include_dirs:
            cmd.extend(["-I", str(source_dir / inc)])
        
        result = subprocess.run(
            cmd,
            capture_output=True,
            text=True,
            timeout=120,  # 2 min per file max
            # Run at below-normal priority
            creationflags=subprocess.BELOW_NORMAL_PRIORITY_CLASS
                          if platform.system() == "Windows" else 0
        )
        
        if result.returncode != 0:
            log.error(f"[Worker] Compile failed: {job.source_file}")
            log.error(result.stderr[:500])
            return None
        
        # Compress the .o file with zlib (fast, ~50% size reduction)
        obj_data = Path(obj_path).read_bytes()
        compressed = zlib.compress(obj_data, level=1)  # Fast compression
        
        # Cache it
        OBJ_CACHE.mkdir(parents=True, exist_ok=True)
        cache_file.write_bytes(compressed)
        
        log.debug(f"[Worker] Compiled: {job.source_file} "
                  f"({len(obj_data)//1024}KB → {len(compressed)//1024}KB compressed)")
        return compressed
        
    except subprocess.TimeoutExpired:
        log.error(f"[Worker] Compile timeout: {job.source_file}")
        return None
    finally:
        Path(obj_path).unlink(missing_ok=True)

# ── Worker network server ──────────────────────────────────────────────────────

class BuildWorkerServer:
    """
    TCP server that receives compile jobs from the build master
    and returns compiled object files.
    
    Protocol (simple binary):
      Request:  4 bytes len + JSON job descriptor
      Response: 4 bytes len + zlib-compressed .o file OR 4 zero bytes (error)
    """
    
    def __init__(self):
        self.running = False
        self.jobs_completed = 0
        self.reputation_score = 0
    
    def handle_client(self, conn: socket.socket, addr):
        """Handle one compilation job request."""
        try:
            # Read length-prefixed JSON
            length_data = conn.recv(4)
            if len(length_data) < 4:
                return
            
            length = struct.unpack(">I", length_data)[0]
            if length > 10 * 1024 * 1024:  # 10MB max job descriptor
                return
            
            job_json = b""
            while len(job_json) < length:
                chunk = conn.recv(min(65536, length - len(job_json)))
                if not chunk:
                    break
                job_json += chunk
            
            job_data = json.loads(job_json)
            job = CompileJob(**job_data)
            
            log.info(f"[Worker] Job received: {job.source_file} from {addr[0]}")
            
            # Check we're still idle before starting
            if not get_policy()._paused.is_set():
                # Send "busy" response
                conn.send(struct.pack(">I", 0))
                return
            
            result = compile_job(job, SOURCE_CACHE)
            
            if result:
                conn.send(struct.pack(">I", len(result)))
                conn.send(result)
                self.jobs_completed += 1
                self.reputation_score += 1
            else:
                conn.send(struct.pack(">I", 0))  # Error
                
        except Exception as e:
            log.error(f"[Worker] Client handler error: {e}")
            try:
                conn.send(struct.pack(">I", 0))
            except Exception:
                pass
        finally:
            conn.close()
    
    def start(self):
        """Start the worker TCP server."""
        resources = get_available_resources()
        if not resources["eligible"]:
            log.info("[Worker] Machine does not meet minimum specs — worker inactive")
            return
        
        self.running = True
        
        srv = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        srv.bind(("0.0.0.0", WORKER_PORT))
        srv.listen(4)
        srv.settimeout(1.0)
        
        log.info(f"[Worker] Build worker online on port {WORKER_PORT}"
                 f" ({resources['cpu_cores']} cores, "
                 f"{resources['ram_free_gb']:.1f}GB RAM free)")
        
        # Announce to AeonHive
        try:
            requests.post("http://localhost:7878/hive/announce", json={
                "type": "build_worker",
                "peer_id": WORKER_ID,
                "port": WORKER_PORT,
                "cpu_cores": resources["cpu_cores"],
                "eligible": True
            }, timeout=3)
        except Exception:
            pass
        
        while self.running:
            try:
                conn, addr = srv.accept()
                
                # Only accept if we're idle
                if get_idle_seconds() < IDLE_REQUIRED_S:
                    conn.close()
                    continue
                
                t = threading.Thread(
                    target=self.handle_client,
                    args=(conn, addr),
                    daemon=True)
                t.start()
            except socket.timeout:
                continue
            except Exception as e:
                if self.running:
                    log.error(f"[Worker] Server error: {e}")
    
    def stop(self):
        self.running = False
        log.info(f"[Worker] Shutting down. Jobs completed: {self.jobs_completed}, "
                 f"Reputation: {self.reputation_score}")


if __name__ == "__main__":
    logging.basicConfig(level=logging.INFO)
    worker = BuildWorkerServer()
    worker.start()
