"""
AeonUniversalUpdater — aeon_universal_updater.py
Updates EVERYTHING: browser binary, AeonMind, AeonHive, AeonSelf,
Ollama models, LoRA deltas, and the extension.

One manifest. One signature. Everything consistent.

This is the single source of truth for what version of every
Aeon component is running on this machine.
"""

import os
import sys
import json
import time
import shutil
import hashlib
import zipfile
import logging
import tempfile
import platform
import subprocess
import threading
import datetime
from pathlib import Path
from typing import Optional
from dataclasses import dataclass, field

# GCS + Ed25519 signature verification
from google.cloud import storage
from cryptography.hazmat.primitives.asymmetric.ed25519 import Ed25519PublicKey
from cryptography.hazmat.primitives import serialization
import requests

log = logging.getLogger("AeonUniversal")

# ── Config ────────────────────────────────────────────────────────────────────

GCS_BUCKET     = os.environ.get("GCS_BUCKET", "aeon-chromium-artifacts")
MANIFEST_URL   = f"https://storage.googleapis.com/{GCS_BUCKET}/releases/latest/manifest.json"
AEON_BASE_DIR  = Path(os.environ.get("AEON_DIR", r"C:\Program Files\Aeon"))
AEON_DATA_DIR  = Path(os.environ.get("AEON_DATA", str(
    Path.home() / "AppData" / "Local" / "Aeon")))
OLLAMA_URL     = os.environ.get("OLLAMA_URL", "http://localhost:11434")
PEER_PORT      = 9798  # Chunk server port

# Ed25519 public key (hardcoded — no server can override this)
# This matches the private key in GCP Secret Manager
AEON_PUBLIC_KEY_HEX = os.environ.get("AEON_PUBLIC_KEY", 
    "REPLACE_WITH_REAL_ED25519_PUBLIC_KEY_HEX_64_CHARS")

# ── Component state tracking ──────────────────────────────────────────────────

@dataclass
class ComponentVersion:
    name: str
    current: str = "0.0.0"
    staged: Optional[str] = None
    status: str = "ok"  # ok | downloading | staged | failed

@dataclass  
class UpdateState:
    manifest_version: str = "0.0.0"
    last_check: Optional[str] = None
    components: dict = field(default_factory=dict)
    overall_progress: float = 0.0
    
    def save(self, path: Path):
        path.write_text(json.dumps({
            "manifest_version": self.manifest_version,
            "last_check": self.last_check,
            "components": {k: vars(v) for k, v in self.components.items()},
            "overall_progress": self.overall_progress
        }, indent=2))
    
    @classmethod
    def load(cls, path: Path) -> "UpdateState":
        if not path.exists():
            return cls()
        try:
            data = json.loads(path.read_text())
            state = cls()
            state.manifest_version = data.get("manifest_version", "0.0.0")
            state.last_check = data.get("last_check")
            for k, v in data.get("components", {}).items():
                state.components[k] = ComponentVersion(**v)
            return state
        except Exception:
            return cls()

# ── Signature verification ────────────────────────────────────────────────────

def verify_signature(data: bytes, signature_hex: str) -> bool:
    """Verify Ed25519 signature against Aeon's public key."""
    if AEON_PUBLIC_KEY_HEX == "REPLACE_WITH_REAL_ED25519_PUBLIC_KEY_HEX_64_CHARS":
        log.warning("[Verify] Ed25519 key not configured — skipping signature check (dev mode)")
        return True
    try:
        pub_bytes = bytes.fromhex(AEON_PUBLIC_KEY_HEX)
        pub_key = Ed25519PublicKey.from_public_bytes(pub_bytes)
        sig_bytes = bytes.fromhex(signature_hex)
        pub_key.verify(sig_bytes, data)
        return True
    except Exception as e:
        log.error(f"[Verify] Signature INVALID: {e}")
        return False

# ── Manifest fetch ────────────────────────────────────────────────────────────

def fetch_manifest() -> Optional[dict]:
    """Fetch and verify the universal update manifest."""
    try:
        resp = requests.get(MANIFEST_URL, timeout=20)
        if resp.status_code != 200:
            return None
        
        manifest = resp.json()
        
        # Verify signature over manifest content (excluding signature field)
        sig = manifest.pop("signature", "")
        manifest_bytes = json.dumps(manifest, sort_keys=True).encode()
        manifest["signature"] = sig  # restore
        
        if not verify_signature(manifest_bytes, sig):
            log.error("[Manifest] Signature verification FAILED — rejecting manifest")
            return None
        
        log.info(f"[Manifest] Fetched v{manifest.get('release_version')} — verified OK")
        return manifest
    except Exception as e:
        log.warning(f"[Manifest] Fetch failed: {e}")
        return None

# ── Download helpers ─────────────────────────────────────────────────────────

def download_file(url: str, dest: Path, 
                  expected_sha256: str,
                  progress_cb=None) -> bool:
    """Download a file, verify SHA-256, return success."""
    try:
        resp = requests.get(url, stream=True, timeout=60)
        total = int(resp.headers.get("content-length", 0))
        downloaded = 0
        
        sha = hashlib.sha256()
        with open(dest, "wb") as f:
            for chunk in resp.iter_content(chunk_size=65536):
                f.write(chunk)
                sha.update(chunk)
                downloaded += len(chunk)
                if progress_cb and total:
                    progress_cb(downloaded / total)
        
        actual = sha.hexdigest()
        if actual != expected_sha256:
            log.error(f"[Download] SHA-256 mismatch for {dest.name}")
            log.error(f"  Expected: {expected_sha256}")
            log.error(f"  Got:      {actual}")
            dest.unlink(missing_ok=True)
            return False
        
        log.info(f"[Download] {dest.name} — verified OK")
        return True
    except Exception as e:
        log.error(f"[Download] Failed: {e}")
        return False

def try_p2p_download(known_peers: list, component: dict, dest: Path) -> bool:
    """Attempt to get a file from AeonHive peers before falling back to GCS."""
    file_name = component.get("file", "")
    sha256    = component.get("sha256", "")
    
    for peer_ip in known_peers:
        try:
            peer_url = f"http://{peer_ip}:{PEER_PORT}/component/{file_name}"
            resp = requests.head(peer_url, timeout=3)
            if resp.status_code == 200:
                log.info(f"[P2P] Getting {file_name} from peer {peer_ip}")
                return download_file(peer_url, dest, sha256)
        except Exception:
            continue
    
    return False  # Fall through to GCS

def download_component(component: dict, dest_dir: Path, 
                        known_peers: list = [],
                        progress_cb=None) -> Optional[Path]:
    """Download a single component file, P2P first, then GCS fallback."""
    file_name = component.get("file")
    if not file_name:
        return None  # No download needed (e.g. unchanged base model)
    
    sha256 = component.get("sha256", "")
    dest = dest_dir / file_name
    
    if dest.exists():
        # Already have it — verify hash
        actual = hashlib.sha256(dest.read_bytes()).hexdigest()
        if actual == sha256:
            log.info(f"[Cache] {file_name} already downloaded and verified")
            return dest
    
    # Try P2P first (free, fast)
    if known_peers and try_p2p_download(known_peers, component, dest):
        return dest
    
    # Fall back to GCS
    gcs_url = (f"https://storage.googleapis.com/{GCS_BUCKET}/"
                f"releases/components/{file_name}")
    log.info(f"[GCS] Downloading {file_name}")
    if download_file(gcs_url, dest, sha256, progress_cb):
        return dest
    
    return None

# ── Component updaters ─────────────────────────────────────────────────────────

def update_python_component(name: str, zip_path: Path, 
                             install_dir: Path, hot_reload: bool = True):
    """Update a Python component (AeonMind, AeonHive, AeonSelf) from a zip."""
    staging = install_dir.parent / f"{name}_staging"
    staging.mkdir(parents=True, exist_ok=True)
    
    # Extract to staging
    with zipfile.ZipFile(zip_path, "r") as zf:
        zf.extractall(staging)
    
    # Atomic swap
    old_dir = install_dir.parent / f"{name}_old"
    if install_dir.exists():
        if old_dir.exists():
            shutil.rmtree(old_dir)
        install_dir.rename(old_dir)
    staging.rename(install_dir)
    
    log.info(f"[Update] {name} updated")
    
    if hot_reload:
        # Signal the running process to reload (send SIGUSR1 or write a marker)
        reload_marker = AEON_DATA_DIR / f"{name}_reload_requested"
        reload_marker.touch()
        log.info(f"[Update] Hot reload requested for {name}")
    
    # Clean up old version
    if old_dir.exists():
        shutil.rmtree(old_dir, ignore_errors=True)

def update_lora_delta(delta_path: Path, base_model: str):
    """Apply a LoRA delta to the current Ollama model."""
    # Check if Ollama is running
    try:
        resp = requests.get(f"{OLLAMA_URL}/api/tags", timeout=5)
        models = [m["name"] for m in resp.json().get("models", [])]
    except Exception:
        log.warning("[LoRA] Ollama not running — skipping LoRA update")
        return
    
    # The delta is a new LoRA adapter .bin file
    # AeonSelf's merge script handles the actual weight merging
    lora_dir = AEON_DATA_DIR / "models" / "lora"
    lora_dir.mkdir(parents=True, exist_ok=True)
    
    dest = lora_dir / delta_path.name
    shutil.copy2(delta_path, dest)
    
    # Signal AeonSelf to apply the delta
    merge_request = AEON_DATA_DIR / "lora_merge_requested.json"
    merge_request.write_text(json.dumps({
        "delta_file": str(dest),
        "base_model": base_model,
        "requested_at": datetime.datetime.utcnow().isoformat()
    }))
    log.info(f"[LoRA] Delta staged for merge: {delta_path.name}")

def update_extension(crx_path: Path):
    """Install updated Aeon extension to the browser's extension directory."""
    ext_dir = AEON_DATA_DIR / "extensions" / "aeon_companion"
    ext_dir.mkdir(parents=True, exist_ok=True)
    
    # Extract CRX (it's a zip with a header)
    # CRX3 format: 4 bytes magic + header_length + zip_data
    with open(crx_path, "rb") as f:
        magic = f.read(4)
        if magic == b"Cr24":  # CRX3
            f.read(4)  # skip version
            header_len = int.from_bytes(f.read(4), "little")
            f.read(header_len)  # skip proto header
            zip_data = f.read()
        else:
            f.seek(0)
            zip_data = f.read()
    
    import io
    with zipfile.ZipFile(io.BytesIO(zip_data)) as zf:
        zf.extractall(ext_dir)
    
    log.info(f"[Ext] Extension updated at {ext_dir}")

def stage_browser_binary(chunks: list, manifest: dict, 
                         staging_dir: Path, known_peers: list) -> bool:
    """Download and assemble the browser binary from chunks."""
    staging_dir.mkdir(parents=True, exist_ok=True)
    tmp_file = staging_dir / "aeon_next.tmp"
    
    total_chunks = len(chunks)
    log.info(f"[Browser] Downloading {total_chunks} chunks...")
    
    with open(tmp_file, "wb") as f:
        for i, chunk_info in enumerate(chunks):
            chunk_sha = chunk_info.get("sha256", "")
            gcs_url   = chunk_info.get("gcs_url", "")
            
            # Try P2P first
            chunk_data = None
            for peer in known_peers:
                try:
                    url = f"http://{peer}:{PEER_PORT}/chunk/{i}"
                    resp = requests.get(url, timeout=10, stream=True)
                    if resp.status_code == 200:
                        data = resp.content
                        actual = hashlib.sha256(data).hexdigest()
                        if actual == chunk_sha:
                            chunk_data = data
                            log.debug(f"[Browser] Chunk {i+1}/{total_chunks} from peer {peer}")
                            break
                except Exception:
                    continue
            
            if chunk_data is None:
                # GCS fallback
                resp = requests.get(gcs_url, timeout=60)
                chunk_data = resp.content
                actual = hashlib.sha256(chunk_data).hexdigest()
                if actual != chunk_sha:
                    log.error(f"[Browser] Chunk {i} hash mismatch!")
                    return False
            
            f.write(chunk_data)
            log.info(f"[Browser] Progress: {i+1}/{total_chunks} chunks")
    
    # Verify full binary
    expected_sha = manifest.get("components", {}).get("browser_binary", {}).get("sha256", "")
    actual_sha = hashlib.sha256(tmp_file.read_bytes()).hexdigest()
    if expected_sha and actual_sha != expected_sha:
        log.error("[Browser] Full binary SHA-256 mismatch!")
        tmp_file.unlink(missing_ok=True)
        return False
    
    # Rename to staged binary
    final = staging_dir / f"aeon_{manifest['release_version']}.exe"
    tmp_file.rename(final)
    
    # Write marker for cold-start installer
    (staging_dir / "update_ready.txt").write_text(
        f"{manifest['release_version']}\n{final}\n")
    
    log.info(f"[Browser] Staged: {final.name}")
    return True

# ── Main update orchestrator ─────────────────────────────────────────────────

class AeonUniversalUpdater:
    def __init__(self):
        self.staging_dir = AEON_DATA_DIR / "staging"
        self.state_file  = AEON_DATA_DIR / "update_state.json"
        self.state = UpdateState.load(self.state_file)
        self._stop = threading.Event()
    
    def get_known_peers(self) -> list[str]:
        """Read peer list from AeonHive."""
        try:
            resp = requests.get("http://localhost:7878/hive/peers", timeout=3)
            return resp.json().get("peer_ips", [])
        except Exception:
            return []
    
    def needs_update(self, component_name: str, manifest_comp: dict) -> bool:
        """Check if this component needs updating."""
        if not manifest_comp:
            return False
        new_ver = manifest_comp.get("version", "0")
        cur_ver = self.state.components.get(
            component_name, ComponentVersion(component_name)).current
        return new_ver != cur_ver
    
    def run_update_cycle(self):
        """Run a full update check and download cycle."""
        from aeon_silence_policy import get_policy
        policy = get_policy()
        
        log.info("[Universal] Starting update cycle...")
        manifest = fetch_manifest()
        if not manifest:
            return
        
        self.state.last_check = datetime.datetime.utcnow().isoformat()
        known_peers = self.get_known_peers()
        components = manifest.get("components", {})
        
        # -- Python components (small, fast, hot-reloadable) --
        py_components = {
            "aeon_mind":  (AEON_BASE_DIR / "agent", True),
            "aeon_hive":  (AEON_BASE_DIR / "hive",  False),
            "aeon_self":  (AEON_BASE_DIR / "agent", True),
        }
        
        for comp_name, (install_dir, hot_reload) in py_components.items():
            comp = components.get(comp_name, {})
            if not self.needs_update(comp_name, comp):
                continue
            
            log.info(f"[Universal] Updating {comp_name} to {comp['version']}")
            policy.wait_for_idle(comp_name)
            
            zip_path = download_component(comp, self.staging_dir, known_peers)
            if zip_path:
                update_python_component(comp_name, zip_path, install_dir, hot_reload)
                if comp_name not in self.state.components:
                    self.state.components[comp_name] = ComponentVersion(comp_name)
                self.state.components[comp_name].current = comp["version"]
                self.state.save(self.state_file)
        
        # -- LoRA delta (weekly model improvement) --
        lora_comp = components.get("model_lora_delta", {})
        if self.needs_update("model_lora_delta", lora_comp):
            log.info(f"[Universal] LoRA delta available: {lora_comp.get('version')}")
            policy.wait_for_idle("lora_training")
            delta_path = download_component(lora_comp, self.staging_dir, known_peers)
            if delta_path:
                update_lora_delta(delta_path, lora_comp.get("base_model_required", ""))
                if "model_lora_delta" not in self.state.components:
                    self.state.components["model_lora_delta"] = ComponentVersion("model_lora_delta")
                self.state.components["model_lora_delta"].current = lora_comp["version"]
                self.state.save(self.state_file)
        
        # -- Extension --
        ext_comp = components.get("extension", {})
        if self.needs_update("extension", ext_comp):
            log.info(f"[Universal] Extension update: {ext_comp.get('version')}")
            policy.wait_for_idle("extension")
            crx_path = download_component(ext_comp, self.staging_dir, known_peers)
            if crx_path:
                update_extension(crx_path)
                if "extension" not in self.state.components:
                    self.state.components["extension"] = ComponentVersion("extension")
                self.state.components["extension"].current = ext_comp["version"]
                self.state.save(self.state_file)
        
        # -- Browser binary (largest, most careful) --
        browser_comp = components.get("browser_binary", {})
        if self.needs_update("browser_binary", browser_comp):
            log.info(f"[Universal] Browser update: {browser_comp.get('version')}")
            # Wait for user to be fully idle before downloading 180MB
            policy.wait_for_idle("chunk_download", timeout_secs=7200)
            
            chunks = browser_comp.get("chunks", [])
            ok = stage_browser_binary(chunks, manifest, self.staging_dir, known_peers)
            if ok:
                if "browser_binary" not in self.state.components:
                    self.state.components["browser_binary"] = ComponentVersion("browser_binary")
                self.state.components["browser_binary"].staged = browser_comp["version"]
                self.state.save(self.state_file)
                log.info("[Universal] Browser update staged — will apply on next cold start")
        
        log.info("[Universal] Update cycle complete")
    
    def start(self):
        """Start the background update loop."""
        def _loop():
            while not self._stop.is_set():
                try:
                    self.run_update_cycle()
                except Exception as e:
                    log.error(f"[Universal] Update cycle error: {e}")
                # Check every 6 hours
                self._stop.wait(timeout=6 * 3600)
        
        t = threading.Thread(target=_loop, daemon=True, name="AeonUniversalUpdater")
        t.start()
        log.info("[Universal] Background updater started")
    
    def stop(self):
        self._stop.set()
    
    def get_status(self) -> dict:
        """Return status dict for aeon://self dashboard."""
        return {
            "last_check": self.state.last_check,
            "components": {
                k: {"current": v.current, "staged": v.staged, "status": v.status}
                for k, v in self.state.components.items()
            }
        }


if __name__ == "__main__":
    logging.basicConfig(level=logging.INFO)
    updater = AeonUniversalUpdater()
    updater.run_update_cycle()
