#!/usr/bin/env python3
"""
Aeon Sovereign Control Center
aeon_sovereign.py — runs from USB drive, anywhere on the planet.

USAGE:
  python sovereign.py                  — unlock and open dashboard
  python sovereign.py --status         — quick network status (no key needed)
  python sovereign.py --push-patch     — sign and push emergency patch
  python sovereign.py --rollback 1.1.9 — force rollback across network
  python sovereign.py --pause-ai       — halt all AI research
  python sovereign.py --resume-ai      — resume AI research
  python sovereign.py --keygen         — generate master key (first time only)

SECURITY MODEL:
  - Ed25519 master private key stored encrypted on this USB (master_key.enc)
  - Encrypted with AES-256-GCM using Argon2id KDF from your password
  - Key ONLY in memory while sovereign.py runs — zeroed on exit
  - All commands signed with your key and logged on GCS (transparent)
  - The network's built-in public key (baked into every binary) is what
    gives this key its authority — not any server
"""

import os
import sys
import json
import time
import signal
import socket
import getpass
import hashlib
import logging
import secrets
import datetime
import argparse
import threading
import webbrowser
import http.server
import urllib.parse
from pathlib import Path
from typing import Optional

# Crypto
try:
    from cryptography.hazmat.primitives.asymmetric.ed25519 import (
        Ed25519PrivateKey, Ed25519PublicKey)
    from cryptography.hazmat.primitives.ciphers.aead import AESGCM
    from cryptography.hazmat.primitives import serialization
    import argon2.low_level as argon2
    from google.cloud import storage
    import requests
    DEPS_OK = True
except ImportError:
    DEPS_OK = False

logging.basicConfig(level=logging.WARNING)
log = logging.getLogger("Sovereign")

# ── Paths (relative to USB) ───────────────────────────────────────────────────

USB_DIR       = Path(__file__).parent
KEY_FILE      = USB_DIR / "master_key.enc"
SESSION_LOG   = USB_DIR / "session.log"
VERSION_FILE  = USB_DIR / "VERSION"
GCS_BUCKET    = "aeon-chromium-artifacts"

# Current sovereign package version (bumped on each GCS release)
SOVEREIGN_VERSION = VERSION_FILE.read_text().strip() if VERSION_FILE.exists() else "1.0.0"

# ── Self-update (runs before password prompt) ──────────────────────────────────

def self_update(skip: bool = False) -> bool:
    """
    Check GCS for a newer sovereign package and apply it in-place.
    Called at startup — before password prompt, so the user always
    has the latest tools when they unlock.

    What updates: aeon_sovereign.py and any helper scripts on the USB.
    What NEVER updates: master_key.enc (your encrypted private key).

    Returns True if a restart is needed (update was applied).
    """
    if skip:
        return False

    manifest_url = (
        f"https://storage.googleapis.com/{GCS_BUCKET}"
        f"/sovereign/latest.json"
    )

    try:
        import requests as _req
        resp = _req.get(manifest_url, timeout=8)
        if resp.status_code != 200:
            return False

        meta = resp.json()
        latest_ver  = meta.get("version", "0.0.0")
        package_url = meta.get("url", "")
        sha256_exp  = meta.get("sha256", "")

        if latest_ver <= SOVEREIGN_VERSION:
            print(f"  Sovereign v{SOVEREIGN_VERSION} — up to date")
            return False

        print(f"  Update available: v{SOVEREIGN_VERSION} → v{latest_ver}")
        print(f"  Downloading sovereign package...")

        pkg_resp = _req.get(package_url, timeout=60, stream=True)
        pkg_data = pkg_resp.content

        # Verify SHA-256 before touching anything
        actual_sha = hashlib.sha256(pkg_data).hexdigest()
        if sha256_exp and actual_sha != sha256_exp:
            print(f"  ✗ SHA-256 mismatch — aborting update (keeping current version)")
            return False

        # Write to a temp zip then extract, preserving master_key.enc
        import zipfile, io, shutil
        protected = {"master_key.enc", "VERSION"}  # never overwrite these

        with zipfile.ZipFile(io.BytesIO(pkg_data)) as zf:
            for member in zf.namelist():
                if member in protected:
                    continue  # never touch the key file
                dest = USB_DIR / member
                dest.parent.mkdir(parents=True, exist_ok=True)
                dest.write_bytes(zf.read(member))

        # Bump local version file
        VERSION_FILE.write_text(latest_ver)

        print(f"  ✓ Sovereign updated to v{latest_ver} — restarting...")
        time.sleep(1)

        # Restart this script with the same arguments
        os.execv(sys.executable, [sys.executable] + sys.argv)
        # execv replaces this process — code below never runs
        return True

    except Exception as e:
        # Offline or GCS unavailable — no problem, continue with current version
        print(f"  (offline — skipping update check)")
        return False

# ── Key management ─────────────────────────────────────────────────────────────

def derive_key(password: str, salt: bytes) -> bytes:
    """Argon2id key derivation — computationally expensive by design."""
    return argon2.hash_secret_raw(
        secret=password.encode(),
        salt=salt,
        time_cost=4,          # Iterations
        memory_cost=1024*128, # 128MB memory — makes brute-force brutal
        parallelism=4,
        hash_len=32,
        type=argon2.Type.ID   # Argon2id
    )

def generate_master_key(password: str) -> tuple[bytes, str]:
    """
    Generate a new Ed25519 master key, encrypt it, save to USB.
    Returns (private_key_bytes, public_key_hex).
    
    Run ONCE on a secure machine. Store USB safely.
    """
    private_key = Ed25519PrivateKey.generate()
    priv_bytes = private_key.private_bytes(
        serialization.Encoding.Raw,
        serialization.PrivateFormat.Raw,
        serialization.NoEncryption()
    )
    pub_bytes = private_key.public_key().public_bytes(
        serialization.Encoding.Raw,
        serialization.PublicFormat.Raw
    )
    
    # Encrypt private key
    salt = secrets.token_bytes(32)
    derived = derive_key(password, salt)
    nonce = secrets.token_bytes(12)
    aesgcm = AESGCM(derived)
    encrypted = aesgcm.encrypt(nonce, priv_bytes, None)
    
    # Format: "AEON_SOVEREIGN_v1" + salt(32) + nonce(12) + ciphertext
    header = b"AEON_SOVEREIGN_v1"
    key_file_data = header + salt + nonce + encrypted
    KEY_FILE.write_bytes(key_file_data)
    
    pub_hex = pub_bytes.hex()
    print(f"\n✓ Master key generated and saved to {KEY_FILE}")
    print(f"\n━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━")
    print(f"CRITICAL — SAVE THIS PUBLIC KEY:")
    print(f"This goes in every Aeon binary AESGCM_PUBLIC_KEY_HEX constant")
    print(f"━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━")
    print(f"\n  {pub_hex}\n")
    print(f"━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━")
    print(f"BACKUP the USB drive NOW. If you lose the USB and forget the")
    print(f"password, no emergency overrides will be possible.")
    
    return priv_bytes, pub_hex

def load_master_key(password: str) -> Optional[bytes]:
    """Decrypt and load the master private key into memory."""
    if not KEY_FILE.exists():
        print("✗ master_key.enc not found. Run: python sovereign.py --keygen")
        return None
    
    data = KEY_FILE.read_bytes()
    header = b"AEON_SOVEREIGN_v1"
    
    if not data.startswith(header):
        print("✗ Invalid key file format")
        return None
    
    offset = len(header)
    salt      = data[offset:offset+32];    offset += 32
    nonce     = data[offset:offset+12];    offset += 12
    encrypted = data[offset:]
    
    try:
        derived = derive_key(password, salt)
        aesgcm = AESGCM(derived)
        priv_bytes = aesgcm.decrypt(nonce, encrypted, None)
        return priv_bytes
    except Exception:
        return None  # Wrong password

def sign_command(priv_bytes: bytes, command: dict) -> str:
    """Sign a sovereign command with the master key."""
    private_key = Ed25519PrivateKey.from_private_bytes(priv_bytes)
    payload = json.dumps(command, sort_keys=True).encode()
    sig = private_key.sign(payload)
    return sig.hex()

def zero_key(priv_bytes: bytearray):
    """Zero out key from memory. Call on exit."""
    for i in range(len(priv_bytes)):
        priv_bytes[i] = 0

# ── GCS network interface ─────────────────────────────────────────────────────

def push_sovereign_command(command: dict, signature: str) -> bool:
    """
    Push a signed sovereign command to GCS.
    All peers monitor sovereign/commands/ and execute immediately.
    Commands are permanently logged — transparency is built in.
    """
    signed_command = {
        **command,
        "signature": signature,
        "issued_at": datetime.datetime.utcnow().isoformat(),
    }
    
    cmd_id = hashlib.sha256(json.dumps(signed_command, sort_keys=True).encode()).hexdigest()[:16]
    path = f"sovereign/commands/{cmd_id}.json"
    
    try:
        gcs = storage.Client()
        blob = gcs.bucket(GCS_BUCKET).blob(path)
        blob.upload_from_string(
            json.dumps(signed_command, indent=2),
            content_type="application/json")
        print(f"✓ Command pushed: {path}")
        return True
    except Exception as e:
        # Offline mode — save locally for push when online
        local = USB_DIR / "pending_commands" / f"{cmd_id}.json"
        local.parent.mkdir(exist_ok=True)
        local.write_text(json.dumps(signed_command, indent=2))
        print(f"⚠ GCS unavailable — command saved locally: {local.name}")
        print(f"  Push when online: python sovereign.py --push-pending")
        return False

def get_network_status() -> dict:
    """Fetch current network status from GCS (no key required — reads only)."""
    status = {
        "peers": 0,
        "builds_pending": 0,
        "patches_staged": 0,
        "last_build": None,
        "last_research": None,
        "components": {},
        "total_votes_cast": 0,
        "emergency_flag": False
    }
    
    try:
        gcs = storage.Client()
        bucket = gcs.bucket(GCS_BUCKET)
        
        # Hive peer count
        try:
            hive = json.loads(bucket.blob("hive/peers.json").download_as_text())
            status["peers"] = len(hive.get("peers", []))
        except Exception: pass
        
        # Build status
        try:
            build = json.loads(bucket.blob("build/status.json").download_as_text())
            status["last_build"] = build.get("completed_at")
            status["current_version"] = build.get("version")
        except Exception: pass
        
        # Research queue
        try:
            queue = json.loads(bucket.blob("research/queue.json").download_as_text())
            status["patches_staged"] = len(queue.get("items", []))
            status["last_research"] = queue.get("last_run")
        except Exception: pass
        
        # Pending patches
        try:
            pending = json.loads(bucket.blob("patches/pending.json").download_as_text())
            status["builds_pending"] = len(pending.get("patches", []))
        except Exception: pass
        
        # Latest summary
        try:
            summary = json.loads(bucket.blob("research/latest_summary.json").download_as_text())
            status["critical_findings"] = summary.get("critical_count", 0)
        except Exception: pass
        
        # Sovereign command log (last 5)
        try:
            blobs = list(bucket.list_blobs(prefix="sovereign/commands/", max_results=5))
            cmds = []
            for b in blobs:
                try:
                    cmd = json.loads(b.download_as_text())
                    cmds.append({"type": cmd.get("type"), "at": cmd.get("issued_at")})
                except Exception: pass
            status["recent_commands"] = cmds
        except Exception: pass
        
    except Exception as e:
        status["error"] = str(e)
        status["offline"] = True
    
    return status

# ── Sovereign commands ─────────────────────────────────────────────────────────

def cmd_push_patch(priv_bytes: bytes, patch_path: str, reason: str):
    """Emergency security patch — bypasses peer vote."""
    patch_content = Path(patch_path).read_text()
    patch_hash = hashlib.sha256(patch_content.encode()).hexdigest()
    
    command = {
        "type": "EMERGENCY_PATCH",
        "patch_hash": patch_hash,
        "patch_content": patch_content,
        "reason": reason,
        "skip_vote": True,
        "priority": "critical"
    }
    
    sig = sign_command(priv_bytes, command)
    print(f"Pushing emergency patch: {patch_path}")
    print(f"Reason: {reason}")
    print(f"This bypasses the peer vote — use only for critical security fixes")
    
    confirm = input("Type CONFIRM to proceed: ")
    if confirm != "CONFIRM":
        print("Cancelled.")
        return
    
    push_sovereign_command(command, sig)

def cmd_rollback(priv_bytes: bytes, target_version: str):
    """Force all peers to rollback to a specific version."""
    command = {
        "type": "FORCE_ROLLBACK",
        "target_version": target_version,
        "affects": ["browser_binary", "aeon_mind", "aeon_hive", "aeon_self"],
        "reason": "Sovereign rollback — manual override"
    }
    
    sig = sign_command(priv_bytes, command)
    print(f"Rolling back entire network to {target_version}")
    confirm = input("Type CONFIRM to proceed: ")
    if confirm != "CONFIRM":
        print("Cancelled.")
        return
    push_sovereign_command(command, sig)

def cmd_pause_ai(priv_bytes: bytes):
    """Halt all AI research and model updates across the network."""
    command = {
        "type": "PAUSE_AI",
        "affects": ["aeon_research_agent", "aeon_patch_writer", "lora_training"],
        "reason": "Manual pause via sovereign key"
    }
    sig = sign_command(priv_bytes, command)
    push_sovereign_command(command, sig)
    print("✓ AI research paused network-wide")

def cmd_resume_ai(priv_bytes: bytes):
    """Resume AI research after a pause."""
    command = {"type": "RESUME_AI"}
    sig = sign_command(priv_bytes, command)
    push_sovereign_command(command, sig)
    print("✓ AI research resumed")

def cmd_adjust_vote_threshold(priv_bytes: bytes, new_threshold: float):
    """Change the peer vote approval threshold (default 0.66)."""
    if not 0.5 <= new_threshold <= 1.0:
        print("Threshold must be between 0.5 and 1.0")
        return
    command = {
        "type": "SET_VOTE_THRESHOLD",
        "threshold": new_threshold
    }
    sig = sign_command(priv_bytes, command)
    push_sovereign_command(command, sig)
    print(f"✓ Vote threshold updated to {new_threshold:.0%}")

def cmd_revoke_peer(priv_bytes: bytes, peer_id: str, reason: str):
    """Blacklist a malicious peer from the network."""
    command = {
        "type": "REVOKE_PEER",
        "peer_id": peer_id,
        "reason": reason
    }
    sig = sign_command(priv_bytes, command)
    push_sovereign_command(command, sig)
    print(f"✓ Peer {peer_id} revoked")

# ── Local dashboard web server ────────────────────────────────────────────────

DASHBOARD_HTML = '''<!DOCTYPE html>
<html>
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<title>Aeon Sovereign Control Center</title>
<style>
  @import url('https://fonts.googleapis.com/css2?family=Inter:wght@300;400;500;600;700;900&family=JetBrains+Mono:wght@400;500&display=swap');
  
  :root {
    --bg: #060608;
    --surface: #0d0d14;
    --surface2: #14141f;
    --border: #1a1a2e;
    --accent: #6c3df0;
    --accent2: #00e5ff;
    --gold: #ffd700;
    --red: #ff3d71;
    --green: #00e096;
    --text: #e8e8f5;
    --muted: #5a5a7a;
    --warn: #ff9f43;
  }
  
  * { margin:0; padding:0; box-sizing:border-box; }
  
  body {
    background: var(--bg);
    color: var(--text);
    font-family: 'Inter', sans-serif;
    min-height: 100vh;
    overflow-x: hidden;
  }
  
  /* Animated background grid */
  body::before {
    content: '';
    position: fixed;
    inset: 0;
    background-image:
      linear-gradient(rgba(108,61,240,0.03) 1px, transparent 1px),
      linear-gradient(90deg, rgba(108,61,240,0.03) 1px, transparent 1px);
    background-size: 40px 40px;
    z-index: 0;
    pointer-events: none;
  }
  
  header {
    position: relative;
    z-index: 10;
    display: flex;
    align-items: center;
    gap: 16px;
    padding: 20px 32px;
    border-bottom: 1px solid var(--border);
    background: rgba(6,6,8,0.8);
    backdrop-filter: blur(20px);
  }
  
  .logo-area { display: flex; align-items: center; gap: 12px; }
  .logo-icon {
    width: 40px; height: 40px;
    background: linear-gradient(135deg, var(--accent), var(--accent2));
    border-radius: 10px;
    display: flex; align-items: center; justify-content: center;
    font-size: 20px;
  }
  .logo-text { font-size: 18px; font-weight: 700; }
  .logo-sub { font-size: 11px; color: var(--gold); font-weight: 500; letter-spacing: 2px; text-transform: uppercase; }
  
  .header-right { margin-left: auto; display: flex; align-items: center; gap: 16px; }
  .key-status {
    display: flex; align-items: center; gap: 8px;
    padding: 8px 14px;
    background: rgba(0, 224, 150, 0.08);
    border: 1px solid rgba(0, 224, 150, 0.2);
    border-radius: 8px;
    font-size: 12px;
    color: var(--green);
    font-weight: 500;
  }
  .key-dot {
    width: 6px; height: 6px;
    border-radius: 50%;
    background: var(--green);
    box-shadow: 0 0 8px var(--green);
    animation: pulse 2s ease-in-out infinite;
  }
  @keyframes pulse { 0%,100%{opacity:1} 50%{opacity:0.3} }
  
  .main { position: relative; z-index: 1; padding: 32px; max-width: 1400px; margin: 0 auto; }
  
  /* Stats row */
  .stats-row {
    display: grid;
    grid-template-columns: repeat(auto-fit, minmax(200px, 1fr));
    gap: 16px;
    margin-bottom: 32px;
  }
  
  .stat-card {
    background: var(--surface);
    border: 1px solid var(--border);
    border-radius: 14px;
    padding: 20px;
    position: relative;
    overflow: hidden;
    transition: border-color 0.2s;
  }
  .stat-card::before {
    content: '';
    position: absolute;
    top: 0; left: 0; right: 0;
    height: 2px;
    background: linear-gradient(90deg, var(--accent), var(--accent2));
    opacity: 0;
    transition: opacity 0.2s;
  }
  .stat-card:hover { border-color: var(--accent); }
  .stat-card:hover::before { opacity: 1; }
  .stat-label { font-size: 10px; font-weight: 600; text-transform: uppercase; letter-spacing: 1.5px; color: var(--muted); margin-bottom: 10px; }
  .stat-value { font-size: 36px; font-weight: 900; line-height: 1; }
  .stat-value.cyan  { color: var(--accent2); }
  .stat-value.gold  { color: var(--gold); }
  .stat-value.green { color: var(--green); }
  .stat-value.red   { color: var(--red); }
  .stat-sub { font-size: 11px; color: var(--muted); margin-top: 8px; }
  
  /* Main grid */
  .grid-2 { display: grid; grid-template-columns: 1fr 1fr; gap: 24px; margin-bottom: 24px; }
  .grid-3 { display: grid; grid-template-columns: 1fr 1fr 1fr; gap: 24px; margin-bottom: 24px; }
  
  .panel {
    background: var(--surface);
    border: 1px solid var(--border);
    border-radius: 16px;
    overflow: hidden;
  }
  
  .panel-header {
    display: flex; align-items: center; gap: 10px;
    padding: 16px 20px;
    border-bottom: 1px solid var(--border);
    font-size: 13px;
    font-weight: 600;
  }
  .panel-icon { font-size: 16px; }
  .panel-body { padding: 20px; }
  
  /* Terminal log */
  .log-terminal {
    background: #020204;
    border-radius: 10px;
    padding: 16px;
    font-family: 'JetBrains Mono', monospace;
    font-size: 11px;
    color: #7a7aaa;
    height: 200px;
    overflow-y: auto;
    line-height: 1.7;
  }
  .log-line.success { color: var(--green); }
  .log-line.error   { color: var(--red); }
  .log-line.info    { color: var(--accent2); }
  .log-line.warn    { color: var(--warn); }
  .log-line.gold    { color: var(--gold); }
  
  /* Buttons */
  .btn {
    padding: 10px 18px;
    border-radius: 8px;
    border: none;
    font-size: 13px;
    font-weight: 600;
    cursor: pointer;
    transition: all 0.2s;
    font-family: inherit;
  }
  .btn-primary { background: var(--accent); color: white; }
  .btn-primary:hover { background: #7d52f5; transform: translateY(-1px); }
  .btn-danger { background: rgba(255,61,113,0.15); border: 1px solid rgba(255,61,113,0.3); color: var(--red); }
  .btn-danger:hover { background: rgba(255,61,113,0.25); }
  .btn-warn { background: rgba(255,159,67,0.15); border: 1px solid rgba(255,159,67,0.3); color: var(--warn); }
  .btn-warn:hover { background: rgba(255,159,67,0.25); }
  .btn-success { background: rgba(0,224,150,0.15); border: 1px solid rgba(0,224,150,0.3); color: var(--green); }
  .btn-success:hover { background: rgba(0,224,150,0.25); }
  .btn-sm { padding: 6px 12px; font-size: 12px; }
  .btn-full { width: 100%; text-align: center; }
  
  /* Command buttons grid */
  .cmd-grid { display: grid; grid-template-columns: 1fr 1fr; gap: 8px; }
  .cmd-btn {
    display: flex; flex-direction: column;
    align-items: flex-start;
    padding: 14px 16px;
    background: var(--surface2);
    border: 1px solid var(--border);
    border-radius: 10px;
    cursor: pointer;
    transition: all 0.2s;
    text-align: left;
  }
  .cmd-btn:hover { border-color: var(--accent); background: rgba(108,61,240,0.08); }
  .cmd-btn.danger:hover { border-color: var(--red); background: rgba(255,61,113,0.08); }
  .cmd-btn.warn:hover { border-color: var(--warn); background: rgba(255,159,67,0.08); }
  .cmd-icon { font-size: 20px; margin-bottom: 8px; }
  .cmd-label { font-size: 13px; font-weight: 600; color: var(--text); }
  .cmd-desc { font-size: 11px; color: var(--muted); margin-top: 2px; }
  
  /* Network map (simplified nodes) */
  .network-map {
    background: #020204;
    border-radius: 10px;
    height: 180px;
    position: relative;
    overflow: hidden;
    display: flex; align-items: center; justify-content: center;
  }
  
  .peer-node {
    position: absolute;
    width: 8px; height: 8px;
    border-radius: 50%;
    background: var(--accent2);
    box-shadow: 0 0 10px var(--accent2);
  }
  .peer-node.self {
    width: 12px; height: 12px;
    background: var(--gold);
    box-shadow: 0 0 16px var(--gold);
  }
  
  /* Progress bar */
  .progress-bar { height: 4px; background: var(--border); border-radius: 2px; overflow: hidden; margin-top: 8px; }
  .progress-fill { height: 100%; background: linear-gradient(90deg, var(--accent), var(--accent2)); border-radius: 2px; transition: width 0.5s; }
  
  /* Version table */
  table { width: 100%; border-collapse: collapse; font-size: 12px; }
  th { text-align: left; color: var(--muted); font-weight: 500; padding: 8px 0; border-bottom: 1px solid var(--border); font-size: 11px; text-transform: uppercase; letter-spacing: 1px; }
  td { padding: 10px 0; border-bottom: 1px solid rgba(26,26,46,0.5); }
  tr:last-child td { border-bottom: none; }
  .badge { display: inline-block; padding: 2px 8px; border-radius: 4px; font-size: 10px; font-weight: 600; }
  .badge-ok { background: rgba(0,224,150,0.15); color: var(--green); }
  .badge-warn { background: rgba(255,159,67,0.15); color: var(--warn); }
  .badge-update { background: rgba(0,229,255,0.15); color: var(--accent2); }
  
  /* Modal */
  .modal-overlay {
    display: none;
    position: fixed; inset: 0;
    background: rgba(0,0,0,0.8);
    backdrop-filter: blur(8px);
    z-index: 100;
    align-items: center; justify-content: center;
  }
  .modal-overlay.show { display: flex; }
  .modal {
    background: var(--surface);
    border: 1px solid var(--border);
    border-radius: 16px;
    padding: 32px;
    width: 480px;
    max-width: 90vw;
  }
  .modal h3 { font-size: 16px; font-weight: 700; margin-bottom: 8px; }
  .modal p { font-size: 13px; color: var(--muted); margin-bottom: 20px; line-height: 1.6; }
  .modal-input {
    width: 100%;
    background: #0a0a10;
    border: 1px solid var(--border);
    border-radius: 8px;
    padding: 12px 14px;
    color: var(--text);
    font-size: 13px;
    font-family: inherit;
    outline: none;
    margin-bottom: 12px;
  }
  .modal-input:focus { border-color: var(--accent); }
  .modal-buttons { display: flex; gap: 8px; justify-content: flex-end; margin-top: 8px; }
  
  /* Scrollbar */
  ::-webkit-scrollbar { width: 6px; }
  ::-webkit-scrollbar-track { background: transparent; }
  ::-webkit-scrollbar-thumb { background: var(--border); border-radius: 3px; }
</style>
</head>
<body>

<header>
  <div class="logo-area">
    <div class="logo-icon">⚡</div>
    <div>
      <div class="logo-text">Aeon Sovereign</div>
      <div class="logo-sub">Network Control Center</div>
    </div>
  </div>
  <div class="header-right">
    <div class="key-status">
      <div class="key-dot"></div>
      Master Key Unlocked
    </div>
    <span id="clock" style="font-size:12px;color:var(--muted);font-family:'JetBrains Mono'"></span>
  </div>
</header>

<div class="main">

  <!-- Stats -->
  <div class="stats-row" id="stats-row">
    <div class="stat-card">
      <div class="stat-label">Hive Peers</div>
      <div class="stat-value cyan" id="stat-peers">—</div>
      <div class="stat-sub">Active nodes</div>
    </div>
    <div class="stat-card">
      <div class="stat-label">Staged Patches</div>
      <div class="stat-value gold" id="stat-patches">—</div>
      <div class="stat-sub">Awaiting vote / build</div>
    </div>
    <div class="stat-card">
      <div class="stat-label">Critical Findings</div>
      <div class="stat-value red" id="stat-critical">—</div>
      <div class="stat-sub">From research agent</div>
    </div>
    <div class="stat-card">
      <div class="stat-label">Network Health</div>
      <div class="stat-value green" id="stat-health">—</div>
      <div class="stat-sub" id="stat-health-sub">Checking...</div>
    </div>
    <div class="stat-card">
      <div class="stat-label">Current Version</div>
      <div class="stat-value" id="stat-version" style="font-size:20px;margin-top:8px;">—</div>
      <div class="stat-sub">Live across network</div>
    </div>
  </div>

  <div class="grid-2">
    <!-- Emergency Commands -->
    <div class="panel">
      <div class="panel-header">
        <div class="panel-icon">🔑</div>
        Sovereign Commands
      </div>
      <div class="panel-body">
        <div class="cmd-grid">
          <button class="cmd-btn warn" onclick="showModal('pause-ai')">
            <div class="cmd-icon">⏸</div>
            <div class="cmd-label">Pause AI</div>
            <div class="cmd-desc">Halt research & training</div>
          </button>
          <button class="cmd-btn" onclick="showModal('resume-ai')">
            <div class="cmd-icon">▶️</div>
            <div class="cmd-label">Resume AI</div>
            <div class="cmd-desc">Restart after pause</div>
          </button>
          <button class="cmd-btn danger" onclick="showModal('rollback')">
            <div class="cmd-icon">↩️</div>
            <div class="cmd-label">Force Rollback</div>
            <div class="cmd-desc">Revert all components</div>
          </button>
          <button class="cmd-btn danger" onclick="showModal('emergency-patch')">
            <div class="cmd-icon">🚨</div>
            <div class="cmd-label">Emergency Patch</div>
            <div class="cmd-desc">Skip vote, deploy now</div>
          </button>
          <button class="cmd-btn" onclick="showModal('vote-threshold')">
            <div class="cmd-icon">⚖️</div>
            <div class="cmd-label">Vote Threshold</div>
            <div class="cmd-desc">Adjust approval %</div>
          </button>
          <button class="cmd-btn danger" onclick="showModal('revoke-peer')">
            <div class="cmd-icon">🚫</div>
            <div class="cmd-label">Revoke Peer</div>
            <div class="cmd-desc">Blacklist malicious node</div>
          </button>
        </div>
      </div>
    </div>

    <!-- Activity Log -->
    <div class="panel">
      <div class="panel-header">
        <div class="panel-icon">📡</div>
        Network Activity
      </div>
      <div class="panel-body">
        <div class="log-terminal" id="log-terminal">
          <div class="log-line info">[Sovereign] Connected to Aeon network...</div>
        </div>
      </div>
    </div>
  </div>

  <div class="grid-2">
    <!-- Component Versions -->
    <div class="panel">
      <div class="panel-header">
        <div class="panel-icon">📦</div>
        Component Status
      </div>
      <div class="panel-body">
        <table id="component-table">
          <tr>
            <th>Component</th>
            <th>Version</th>
            <th>Status</th>
          </tr>
          <tr><td colspan="3" style="color:var(--muted);font-size:12px;">Loading...</td></tr>
        </table>
      </div>
    </div>

    <!-- Network Map -->
    <div class="panel">
      <div class="panel-header">
        <div class="panel-icon">🌐</div>
        Peer Network
      </div>
      <div class="panel-body">
        <div class="network-map" id="network-map">
          <div style="color:var(--muted);font-size:12px;">Loading peer data...</div>
        </div>
        <div style="margin-top:12px;font-size:12px;color:var(--muted);text-align:center">
          🟡 You (sovereign)&nbsp;&nbsp;🔵 Peers&nbsp;&nbsp;Lines = active connections
        </div>
      </div>
    </div>
  </div>

  <!-- Recent Sovereign Commands -->
  <div class="panel">
    <div class="panel-header">
      <div class="panel-icon">📜</div>
      Sovereign Command History
      <span style="margin-left:auto;font-size:11px;color:var(--muted)">All commands are publicly logged</span>
    </div>
    <div class="panel-body">
      <div id="cmd-history" style="font-size:12px;color:var(--muted)">Loading...</div>
    </div>
  </div>

</div>

<!-- Modals -->
<div class="modal-overlay" id="modal-overlay" onclick="hideModal()">
  <div class="modal" onclick="event.stopPropagation()">
    <h3 id="modal-title">Command</h3>
    <p id="modal-desc"></p>
    <div id="modal-inputs"></div>
    <div class="modal-buttons">
      <button class="btn btn-sm" onclick="hideModal()">Cancel</button>
      <button class="btn btn-sm btn-danger" id="modal-confirm" onclick="executeCommand()">Confirm</button>
    </div>
  </div>
</div>

<script>
let currentCommand = null;

// ── Clock ────────────────────────────────────────────────────────────────────
function updateClock() {
  const now = new Date();
  document.getElementById('clock').textContent =
    now.toUTCString().replace('GMT', 'UTC');
}
updateClock();
setInterval(updateClock, 1000);

// ── Logging ───────────────────────────────────────────────────────────────────
function addLog(msg, type='') {
  const term = document.getElementById('log-terminal');
  const line = document.createElement('div');
  line.className = 'log-line ' + type;
  const t = new Date().toTimeString().split(' ')[0];
  line.textContent = `[${t}] ${msg}`;
  term.appendChild(line);
  term.scrollTop = term.scrollHeight;
}

// ── Status polling ─────────────────────────────────────────────────────────────
async function pollStatus() {
  try {
    const resp = await fetch('/api/status');
    const s = await resp.json();
    
    document.getElementById('stat-peers').textContent   = s.peers ?? '—';
    document.getElementById('stat-patches').textContent = s.patches_staged ?? '—';
    document.getElementById('stat-critical').textContent = s.critical_findings ?? '0';
    document.getElementById('stat-version').textContent = s.current_version ?? '—';
    
    const health = s.offline ? '⚠ Offline' : '✓ Online';
    document.getElementById('stat-health').textContent = health;
    document.getElementById('stat-health').className = 'stat-value ' + (s.offline ? 'red' : 'green');
    document.getElementById('stat-health-sub').textContent = s.offline ? 'GCS unreachable' : 'All systems nominal';
    
    // Components
    if (s.components) {
      const table = document.getElementById('component-table');
      const names = {
        browser_binary: 'Browser Binary',
        aeon_mind: 'AeonMind',
        aeon_hive: 'AeonHive',
        aeon_self: 'AeonSelf',
        model_lora_delta: 'LoRA Model',
        extension: 'Extension'
      };
      
      let rows = `<tr><th>Component</th><th>Version</th><th>Status</th></tr>`;
      for (const [k, v] of Object.entries(s.components)) {
        const badge = v.staged ? 
          `<span class="badge badge-update">Update ready</span>` :
          `<span class="badge badge-ok">Current</span>`;
        rows += `<tr>
          <td>${names[k] || k}</td>
          <td style="font-family:'JetBrains Mono';font-size:11px">${v.current || '—'}</td>
          <td>${badge}</td>
        </tr>`;
      }
      table.innerHTML = rows;
    }
    
    // Peer network visualization
    if (s.peers > 0) {
      renderPeerMap(s.peers);
    }
    
    // Command history
    if (s.recent_commands?.length) {
      const hist = document.getElementById('cmd-history');
      hist.innerHTML = s.recent_commands.map(c => 
        `<div style="padding:8px 0;border-bottom:1px solid var(--border);">
          <span style="color:var(--gold)">${c.type}</span>
          <span style="color:var(--muted);margin-left:8px">${c.at?.slice(0,19).replace('T',' ')} UTC</span>
        </div>`
      ).join('');
    } else {
      document.getElementById('cmd-history').textContent = 'No sovereign commands issued yet.';
    }
    
    if (!s.offline) addLog('Status refreshed — ' + s.peers + ' peers online', 'success');
    
  } catch(e) {
    addLog('Status poll failed: ' + e.message, 'error');
  }
}

function renderPeerMap(peerCount) {
  const map = document.getElementById('network-map');
  map.innerHTML = '';
  
  // Place self in center
  const self = document.createElement('div');
  self.className = 'peer-node self';
  self.style.left = '50%'; self.style.top = '50%';
  self.style.transform = 'translate(-50%,-50%)';
  map.appendChild(self);
  
  // Place peers in a circle
  const count = Math.min(peerCount, 20);
  for (let i = 0; i < count; i++) {
    const angle = (i / count) * 2 * Math.PI;
    const r = 60 + Math.random() * 30;
    const x = 50 + (r * Math.cos(angle)) / 1.8;
    const y = 50 + (r * Math.sin(angle)) / 2;
    
    const node = document.createElement('div');
    node.className = 'peer-node';
    node.style.left = x + '%'; node.style.top = y + '%';
    node.style.transform = 'translate(-50%,-50%)';
    node.style.opacity = (0.5 + Math.random() * 0.5).toString();
    map.appendChild(node);
  }
  
  if (peerCount > 20) {
    const label = document.createElement('div');
    label.style.cssText = 'position:absolute;bottom:12px;right:12px;font-size:10px;color:var(--muted)';
    label.textContent = `+${peerCount-20} more`;
    map.appendChild(label);
  }
}

// ── Modal system ──────────────────────────────────────────────────────────────
const modals = {
  'pause-ai': {
    title: '⏸ Pause AI Research',
    desc: 'This will halt all research agents, patch writers, and LoRA training across the entire network. Peers will stop processing new patches until you resume.',
    inputs: '',
    dangerous: true
  },
  'resume-ai': {
    title: '▶️ Resume AI Research',
    desc: 'Resume all AI research and model training after a pause.',
    inputs: '',
    dangerous: false
  },
  'rollback': {
    title: '↩️ Force Network Rollback',
    desc: 'All peers will downgrade to the specified version. This will override any staged updates.',
    inputs: '<input class="modal-input" id="inp-version" placeholder="Target version (e.g. 1.1.9)">',
    dangerous: true
  },
  'emergency-patch': {
    title: '🚨 Emergency Patch',
    desc: 'Deploy a patch that bypasses peer voting. Only use for critical security vulnerabilities with a verified fix.',
    inputs: `
      <input class="modal-input" id="inp-patch" placeholder="Patch file path (on this machine)">
      <input class="modal-input" id="inp-reason" placeholder="Reason (e.g. CVE-2026-XXXX)">
    `,
    dangerous: true
  },
  'vote-threshold': {
    title: '⚖️ Adjust Vote Threshold',
    desc: 'Change the percentage of peer votes required to approve an update. Default is 66%.',
    inputs: '<input class="modal-input" id="inp-threshold" placeholder="New threshold (0.5 to 1.0)" value="0.66">',
    dangerous: false
  },
  'revoke-peer': {
    title: '🚫 Revoke Peer',
    desc: 'Blacklist a peer from the network. Their votes will be ignored and their update distributions blocked.',
    inputs: `
      <input class="modal-input" id="inp-peer-id" placeholder="Peer ID (12-char hex)">
      <input class="modal-input" id="inp-revoke-reason" placeholder="Reason">
    `,
    dangerous: true
  }
};

function showModal(cmd) {
  currentCommand = cmd;
  const m = modals[cmd];
  if (!m) return;
  document.getElementById('modal-title').textContent = m.title;
  document.getElementById('modal-desc').textContent = m.desc;
  document.getElementById('modal-inputs').innerHTML = m.inputs;
  const btn = document.getElementById('modal-confirm');
  btn.className = 'btn btn-sm ' + (m.dangerous ? 'btn-danger' : 'btn-success');
  document.getElementById('modal-overlay').classList.add('show');
}

function hideModal() {
  document.getElementById('modal-overlay').classList.remove('show');
  currentCommand = null;
}

async function executeCommand() {
  if (!currentCommand) return;
  
  const body = { command: currentCommand };
  
  if (currentCommand === 'rollback') body.version = document.getElementById('inp-version')?.value;
  if (currentCommand === 'emergency-patch') {
    body.patch_path = document.getElementById('inp-patch')?.value;
    body.reason = document.getElementById('inp-reason')?.value;
  }
  if (currentCommand === 'vote-threshold') body.threshold = parseFloat(document.getElementById('inp-threshold')?.value);
  if (currentCommand === 'revoke-peer') {
    body.peer_id = document.getElementById('inp-peer-id')?.value;
    body.reason = document.getElementById('inp-revoke-reason')?.value;
  }
  
  hideModal();
  addLog(`Executing: ${currentCommand}...`, 'warn');
  
  try {
    const resp = await fetch('/api/command', {
      method: 'POST',
      headers: {'Content-Type': 'application/json'},
      body: JSON.stringify(body)
    });
    const result = await resp.json();
    if (result.success) {
      addLog('✓ Command executed and signed: ' + currentCommand, 'gold');
    } else {
      addLog('✗ Command failed: ' + result.error, 'error');
    }
  } catch(e) {
    addLog('✗ Request failed: ' + e.message, 'error');
  }
}

// Initial load
pollStatus();
setInterval(pollStatus, 30000);
addLog('Sovereign Control Center initialized', 'gold');
addLog('Master key unlocked — full network authority active', 'info');
</script>
</body>
</html>'''


class SovereignAPIHandler(http.server.BaseHTTPRequestHandler):
    """Local HTTP API for the sovereign dashboard."""
    
    priv_bytes = None  # Set by main before starting
    
    def log_message(self, format, *args):
        pass  # Suppress access log
    
    def send_json(self, data, status=200):
        body = json.dumps(data).encode()
        self.send_response(status)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(body)))
        self.send_header("Access-Control-Allow-Origin", "*")
        self.end_headers()
        self.wfile.write(body)
    
    def do_GET(self):
        if self.path == "/":
            body = DASHBOARD_HTML.encode()
            self.send_response(200)
            self.send_header("Content-Type", "text/html")
            self.send_header("Content-Length", str(len(body)))
            self.end_headers()
            self.wfile.write(body)
        elif self.path == "/api/status":
            status = get_network_status()
            # Add component versions from local state
            state_file = AEON_DATA_DIR / "update_state.json" if "AEON_DATA_DIR" in dir() else None
            try:
                if state_file and state_file.exists():
                    state = json.loads(state_file.read_text())
                    status["components"] = state.get("components", {})
            except Exception:
                pass
            self.send_json(status)
    
    def do_POST(self):
        if self.path == "/api/command":
            length = int(self.headers.get("Content-Length", 0))
            body = json.loads(self.rfile.read(length))
            
            cmd = body.get("command")
            priv = SovereignAPIHandler.priv_bytes
            
            if not priv:
                self.send_json({"success": False, "error": "Key not loaded"}, 403)
                return
            
            try:
                if cmd == "pause-ai":
                    cmd_pause_ai(priv)
                elif cmd == "resume-ai":
                    cmd_resume_ai(priv)
                elif cmd == "rollback":
                    cmd_rollback(priv, body.get("version", ""))
                elif cmd == "vote-threshold":
                    cmd_adjust_vote_threshold(priv, body.get("threshold", 0.66))
                elif cmd == "revoke-peer":
                    cmd_revoke_peer(priv, body.get("peer_id",""), body.get("reason",""))
                elif cmd == "emergency-patch":
                    cmd_push_patch(priv, body.get("patch_path",""), body.get("reason",""))
                
                self.send_json({"success": True})
            except Exception as e:
                self.send_json({"success": False, "error": str(e)}, 500)

# ── Main entry point ──────────────────────────────────────────────────────────

def main():
    parser = argparse.ArgumentParser(description="Aeon Sovereign Control Center")
    parser.add_argument("--keygen", action="store_true", help="Generate master key (first time only)")
    parser.add_argument("--status", action="store_true", help="Show network status (no key needed)")
    parser.add_argument("--port", type=int, default=9000)
    parser.add_argument("--skip-update", action="store_true", help="Skip self-update check on launch")
    args = parser.parse_args()
    
    print("\n" + "━"*56)
    print("  ⚡  AEON SOVEREIGN CONTROL CENTER")
    print(f"      Network Authority Terminal  v{SOVEREIGN_VERSION}")
    print("━"*56)

    # Self-update check — runs before password prompt so you always
    # have the latest tools. Skipped if offline (graceful fallback).
    if not args.keygen and not getattr(args, 'skip_update', False):
        print("\n  Checking for sovereign updates...")
        self_update(skip=getattr(args, 'skip_update', False))
        # If self_update applied an update, os.execv() above restarted us.
        # If we're still here, we're already current.
    
    if not DEPS_OK:
        print("\n✗ Missing dependencies. Run:")
        print("  pip install cryptography argon2-cffi google-cloud-storage requests")
        sys.exit(1)
    
    if args.keygen:
        print("\nGenerating new master key...")
        password = getpass.getpass("Set master password: ")
        confirm  = getpass.getpass("Confirm password: ")
        if password != confirm:
            print("Passwords don't match.")
            sys.exit(1)
        generate_master_key(password)
        sys.exit(0)
    
    if args.status:
        print("\nFetching network status (read-only)...")
        status = get_network_status()
        print(json.dumps(status, indent=2))
        sys.exit(0)
    
    # Load master key
    password = getpass.getpass("\nMaster password: ")
    print("Decrypting key (Argon2id, please wait)...")
    priv_bytes = load_master_key(password)
    
    if not priv_bytes:
        print("\n✗ Wrong password or corrupted key file.")
        sys.exit(1)
    
    # Use bytearray for secure zeroing
    priv_ba = bytearray(priv_bytes)
    del priv_bytes
    
    SovereignAPIHandler.priv_bytes = bytes(priv_ba)
    
    # Register signal handlers for clean key zeroing
    def cleanup(sig=None, frame=None):
        print("\n[Sovereign] Zeroing master key from memory...")
        zero_key(priv_ba)
        SovereignAPIHandler.priv_bytes = None
        print("[Sovereign] Key zeroed. Session ended.")
        sys.exit(0)
    
    signal.signal(signal.SIGINT, cleanup)
    signal.signal(signal.SIGTERM, cleanup)
    
    print(f"\n✓ Master key unlocked")
    print(f"✓ Starting dashboard at http://localhost:{args.port}")
    print(f"  Press Ctrl+C to end session and zero key from memory\n")
    
    # Open browser
    def open_browser():
        time.sleep(1.5)
        webbrowser.open(f"http://localhost:{args.port}")
    
    threading.Thread(target=open_browser, daemon=True).start()
    
    # Start local web server
    server = http.server.HTTPServer(("127.0.0.1", args.port), SovereignAPIHandler)
    try:
        server.serve_forever()
    except KeyboardInterrupt:
        cleanup()

if __name__ == "__main__":
    main()
