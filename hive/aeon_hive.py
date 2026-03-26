"""
AeonHive — aeon_hive.py
The P2P mesh daemon. Everything plugs into this.

Runs as a background service on every Aeon install.
Provides:
  - Peer discovery (GCS bootstrap → gossip → pure P2P)
  - Peer health tracking + scoring
  - REST API on port 7878 (local processes query this)
  - Chunk server on port 9798 (serves update chunks to peers)
  - Vote aggregation (collect → tally → publish result)
  - Build worker registry (tracks eligible compile peers)
  - Sovereign command relay (GCS poll → broadcast locally)
  - Silence policy integration (pauses seeding when user active)

Phase awareness:
  Phase 1: Peer discovery only, chunk serving, vote passing
  Phase 2: Build worker coordination added
  Phase 3: Full autonomous build master election
"""

import os
import sys
import json
import time
import math
import uuid
import zlib
import socket
import hashlib
import logging
import secrets
import datetime
import threading
import argparse
import platform
import ipaddress
from pathlib import Path
from typing import Optional
from dataclasses import dataclass, field, asdict
from http.server import HTTPServer, BaseHTTPRequestHandler
import urllib.request
import urllib.error

log = logging.getLogger("AeonHive")

# ── Config ────────────────────────────────────────────────────────────────────

HIVE_VERSION    = "1.0.0"
REST_PORT       = 7878   # Local REST API (other Aeon processes use this)
CHUNK_PORT      = 9798   # Peer chunk serving
GOSSIP_PORT     = 9797   # Peer gossip/discovery
GCS_BUCKET      = os.environ.get("GCS_BUCKET", "aeon-chromium-artifacts")
AEON_DATA       = Path(os.environ.get("AEON_DATA",
                    str(Path.home() / "AppData" / "Local" / "Aeon")))
PEER_DB         = AEON_DATA / "hive_peers.json"
CHUNK_CACHE     = AEON_DATA / "chunk_cache"
SOVEREIGN_POLL  = 30       # Seconds between sovereign command checks
GOSSIP_INTERVAL = 120      # Seconds between peer gossip rounds
MAX_PEERS       = 500      # Cap stored peers
PEER_TIMEOUT    = 3600     # Forget peers not seen in 1 hour

# Bootstrap peers — GCS list until network is large enough for pure DHT
BOOTSTRAP_URL = f"https://storage.googleapis.com/{GCS_BUCKET}/hive/bootstrap_peers.json"

# ── Peer model ────────────────────────────────────────────────────────────────

@dataclass
class Peer:
    peer_id:     str
    ip:          str
    gossip_port: int = GOSSIP_PORT
    chunk_port:  int = CHUNK_PORT
    last_seen:   float = field(default_factory=time.time)
    reputation:  int   = 100       # 0–1000, starts at 100
    is_build_worker: bool = False
    build_cpu_cores: int  = 0
    version:     str  = ""
    
    def endpoint(self, port: int) -> str:
        return f"{self.ip}:{port}"
    
    def is_alive(self) -> bool:
        return (time.time() - self.last_seen) < PEER_TIMEOUT
    
    def to_dict(self) -> dict:
        return asdict(self)
    
    @classmethod
    def from_dict(cls, d: dict) -> "Peer":
        return cls(**{k: v for k, v in d.items() if k in cls.__dataclass_fields__})

# ── Peer database ──────────────────────────────────────────────────────────────

class PeerDB:
    def __init__(self):
        self._lock = threading.RLock()
        self._peers: dict[str, Peer] = {}
        self._load()
    
    def _load(self):
        if PEER_DB.exists():
            try:
                data = json.loads(PEER_DB.read_text())
                for d in data:
                    p = Peer.from_dict(d)
                    if p.is_alive():
                        self._peers[p.peer_id] = p
            except Exception:
                pass
    
    def save(self):
        with self._lock:
            PEER_DB.write_text(json.dumps(
                [p.to_dict() for p in self._peers.values()], indent=2))
    
    def add_or_update(self, peer: Peer):
        with self._lock:
            existing = self._peers.get(peer.peer_id)
            if existing:
                existing.last_seen = time.time()
                existing.ip = peer.ip
                if peer.reputation > 0:
                    existing.reputation = min(1000, existing.reputation + 1)
            else:
                if len(self._peers) < MAX_PEERS:
                    self._peers[peer.peer_id] = peer
    
    def remove_dead(self):
        with self._lock:
            dead = [pid for pid, p in self._peers.items() if not p.is_alive()]
            for pid in dead:
                del self._peers[pid]
    
    def get_all(self) -> list[Peer]:
        with self._lock:
            return list(self._peers.values())
    
    def get_live(self) -> list[Peer]:
        return [p for p in self.get_all() if p.is_alive()]
    
    def get_build_workers(self) -> list[Peer]:
        return [p for p in self.get_live() if p.is_build_worker]
    
    def get_random_sample(self, n: int = 10) -> list[Peer]:
        import random
        live = self.get_live()
        return random.sample(live, min(n, len(live)))
    
    def penalize(self, peer_id: str, amount: int = 10):
        with self._lock:
            if peer_id in self._peers:
                self._peers[peer_id].reputation = max(0, 
                    self._peers[peer_id].reputation - amount)
    
    def revoke(self, peer_id: str):
        """Sovereign command: permanent ban."""
        with self._lock:
            self._peers.pop(peer_id, None)
            # Write to blocklist so it doesn't come back
            bl = AEON_DATA / "peer_blocklist.json"
            data = json.loads(bl.read_text()) if bl.exists() else []
            if peer_id not in data:
                data.append(peer_id)
            bl.write_text(json.dumps(data))
    
    def count(self) -> int:
        with self._lock:
            return len(self._peers)

# ── Vote ledger ───────────────────────────────────────────────────────────────

@dataclass
class VoteRecord:
    patch_id:    str
    votes_yes:   int = 0
    votes_no:    int = 0
    voters:      list = field(default_factory=list)
    started_at:  float = field(default_factory=time.time)
    threshold:   float = 0.66
    status:      str = "open"  # open | approved | rejected | expired
    
    @property
    def total(self) -> int:
        return self.votes_yes + self.votes_no
    
    @property
    def approval_rate(self) -> float:
        return self.votes_yes / self.total if self.total > 0 else 0.0
    
    def check_result(self) -> str:
        age_hours = (time.time() - self.started_at) / 3600
        if age_hours > 48:
            self.status = "expired"
        elif self.total >= 10 and self.approval_rate >= self.threshold:
            self.status = "approved"
        elif self.total >= 10 and (1 - self.approval_rate) > self.threshold:
            self.status = "rejected"
        return self.status

class VoteLedger:
    def __init__(self):
        self._lock = threading.Lock()
        self._votes: dict[str, VoteRecord] = {}
        self._file = AEON_DATA / "vote_ledger.json"
        self._load()
    
    def _load(self):
        if self._file.exists():
            try:
                data = json.loads(self._file.read_text())
                for d in data:
                    r = VoteRecord(**d)
                    self._votes[r.patch_id] = r
            except Exception:
                pass
    
    def save(self):
        with self._lock:
            self._file.write_text(json.dumps(
                [asdict(v) for v in self._votes.values()], indent=2))
    
    def cast_vote(self, patch_id: str, peer_id: str, yes: bool, 
                  threshold: float = 0.66) -> dict:
        with self._lock:
            if patch_id not in self._votes:
                self._votes[patch_id] = VoteRecord(patch_id, threshold=threshold)
            
            record = self._votes[patch_id]
            
            if peer_id in record.voters:
                return {"error": "already_voted"}
            
            if record.status != "open":
                return {"error": f"vote_{record.status}"}
            
            record.voters.append(peer_id)
            if yes:
                record.votes_yes += 1
            else:
                record.votes_no += 1
            
            status = record.check_result()
            self.save()
            
            return {
                "patch_id": patch_id,
                "votes_yes": record.votes_yes,
                "votes_no": record.votes_no,
                "approval_rate": record.approval_rate,
                "status": status,
                "threshold": threshold
            }
    
    def get_status(self, patch_id: str) -> Optional[dict]:
        with self._lock:
            r = self._votes.get(patch_id)
            if not r:
                return None
            r.check_result()
            return asdict(r)
    
    def get_open_votes(self) -> list[dict]:
        with self._lock:
            return [asdict(v) for v in self._votes.values() if v.status == "open"]

# ── Gossip protocol ───────────────────────────────────────────────────────────

class GossipServer:
    """
    UDP gossip for peer discovery and lightweight message propagation.
    Each gossip message is a <= 1400 byte JSON blob.
    
    Message types:
      HELLO    — announce yourself to a peer, share peer list
      PONG     — reply to HELLO with your peer list
      VOTE     — broadcast a vote
      ANNOUNCE — build worker / chunk availability announcement
    """
    
    def __init__(self, peer_db: PeerDB, vote_ledger: VoteLedger,
                 local_peer: Peer):
        self.db           = peer_db
        self.votes        = vote_ledger
        self.local        = local_peer
        self._stop        = threading.Event()
    
    def _make_hello(self) -> bytes:
        peers = self.db.get_random_sample(20)
        msg = {
            "type": "HELLO",
            "from": self.local.to_dict(),
            "peers": [
                {"peer_id": p.peer_id, "ip": p.ip,
                 "gossip_port": p.gossip_port}
                for p in peers
            ]
        }
        return json.dumps(msg).encode()[:1400]
    
    def _handle_message(self, data: bytes, addr):
        try:
            msg = json.loads(data)
            mtype = msg.get("type")
            
            if mtype in ("HELLO", "PONG"):
                # Register the sender
                sender = msg.get("from", {})
                if sender.get("peer_id") != self.local.peer_id:
                    p = Peer.from_dict({**sender, "ip": addr[0],
                                        "last_seen": time.time()})
                    self.db.add_or_update(p)
                
                # Register shared peers
                for pd in msg.get("peers", []):
                    if pd.get("peer_id") != self.local.peer_id:
                        p = Peer(
                            peer_id=pd["peer_id"],
                            ip=pd.get("ip", ""),
                            gossip_port=pd.get("gossip_port", GOSSIP_PORT)
                        )
                        if p.ip:
                            self.db.add_or_update(p)
            
            elif mtype == "VOTE":
                # Relay vote to vote ledger
                self.votes.cast_vote(
                    msg["patch_id"],
                    msg["peer_id"],
                    msg["yes"],
                    msg.get("threshold", 0.66)
                )
            
            elif mtype == "ANNOUNCE":
                # Build worker or chunk availability
                peer_id = msg.get("peer_id")
                if peer_id and peer_id != self.local.peer_id:
                    # Update peer metadata
                    for p in self.db.get_all():
                        if p.peer_id == peer_id:
                            if msg.get("type_detail") == "build_worker":
                                p.is_build_worker = True
                                p.build_cpu_cores = msg.get("cpu_cores", 0)
                            break
        
        except Exception as e:
            log.debug(f"[Gossip] Message parse error: {e}")
    
    def start_server(self):
        """UDP listener for incoming gossip."""
        sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        sock.bind(("0.0.0.0", GOSSIP_PORT))
        sock.settimeout(1.0)
        log.info(f"[Gossip] Listening on UDP :{GOSSIP_PORT}")
        
        while not self._stop.is_set():
            try:
                data, addr = sock.recvfrom(2048)
                threading.Thread(
                    target=self._handle_message,
                    args=(data, addr), daemon=True).start()
            except socket.timeout:
                continue
        sock.close()
    
    def gossip_round(self):
        """Proactively gossip to random peers."""
        peers = self.db.get_random_sample(5)
        if not peers:
            return
        
        msg = self._make_hello()
        sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        
        for peer in peers:
            try:
                sock.sendto(msg, (peer.ip, peer.gossip_port))
            except Exception:
                pass
        sock.close()
    
    def broadcast_vote(self, patch_id: str, yes: bool):
        """Broadcast this peer's vote to the network."""
        msg = json.dumps({
            "type": "VOTE",
            "patch_id": patch_id,
            "peer_id": self.local.peer_id,
            "yes": yes,
            "threshold": 0.66
        }).encode()
        
        sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        for peer in self.db.get_random_sample(20):
            try:
                sock.sendto(msg, (peer.ip, peer.gossip_port))
            except Exception:
                pass
        sock.close()
    
    def stop(self):
        self._stop.set()

# ── Chunk server ──────────────────────────────────────────────────────────────

class ChunkServer(BaseHTTPRequestHandler):
    """
    HTTP server that serves cached update chunks to peers.
    GET /chunk/<sha256>      — serve a cached chunk
    GET /component/<file>   — serve a full component file
    HEAD /component/<file>  — check if we have it
    """
    
    chunk_cache = CHUNK_CACHE
    
    def log_message(self, fmt, *args):
        pass  # Suppress access log
    
    def do_HEAD(self):
        fname = self.path.split("/")[-1]
        fpath = self.chunk_cache / fname
        if fpath.exists():
            self.send_response(200)
            self.send_header("Content-Length", str(fpath.stat().st_size))
            self.end_headers()
        else:
            self.send_response(404)
            self.end_headers()
    
    def do_GET(self):
        parts = self.path.strip("/").split("/")
        if len(parts) < 2:
            self.send_response(400); self.end_headers(); return
        
        category = parts[0]  # "chunk" or "component"
        fname    = parts[1]
        
        fpath = self.chunk_cache / fname
        if not fpath.exists():
            self.send_response(404); self.end_headers(); return
        
        data = fpath.read_bytes()
        self.send_response(200)
        self.send_header("Content-Type", "application/octet-stream")
        self.send_header("Content-Length", str(len(data)))
        self.end_headers()
        self.wfile.write(data)

# ── Sovereign command processor ───────────────────────────────────────────────

class SovereignCommandProcessor:
    """
    Polls GCS for sovereign commands and executes them locally.
    Also watches for a local sovereign process (the USB control center)
    and relays its commands immediately.
    """
    
    def __init__(self, peer_db: PeerDB, vote_ledger: VoteLedger):
        self.db        = peer_db
        self.votes     = vote_ledger
        self._seen     = set()  # Already executed command IDs
        self._paused   = False  # AI paused by sovereign command
        self._threshold = 0.66
        self._seen_file = AEON_DATA / "sovereign_seen.json"
        self._load_seen()
    
    def _load_seen(self):
        if self._seen_file.exists():
            try:
                self._seen = set(json.loads(self._seen_file.read_text()))
            except Exception:
                pass
    
    def _save_seen(self):
        self._seen_file.write_text(json.dumps(list(self._seen)))
    
    def _verify_signature(self, command: dict) -> bool:
        """Verify Ed25519 signature from the sovereign key."""
        # Public key is baked in via environment / binary constant
        pub_key_hex = os.environ.get("AEON_PUBLIC_KEY", "")
        if not pub_key_hex or pub_key_hex == "REPLACE_WITH_REAL_ED25519_PUBLIC_KEY_HEX_64_CHARS":
            log.warning("[Sovereign] Public key not configured — accepting commands (dev mode)")
            return True
        
        try:
            from cryptography.hazmat.primitives.asymmetric.ed25519 import Ed25519PublicKey
            sig_hex = command.get("signature", "")
            payload = {k: v for k, v in command.items() if k != "signature"}
            payload_bytes = json.dumps(payload, sort_keys=True).encode()
            pub = Ed25519PublicKey.from_public_bytes(bytes.fromhex(pub_key_hex))
            pub.verify(bytes.fromhex(sig_hex), payload_bytes)
            return True
        except Exception:
            return False
    
    def _execute(self, command: dict):
        cmd_type = command.get("type")
        log.info(f"[Sovereign] Executing: {cmd_type}")
        
        if cmd_type == "PAUSE_AI":
            self._paused = True
            (AEON_DATA / "ai_paused").touch()
            log.info("[Sovereign] AI research PAUSED")
        
        elif cmd_type == "RESUME_AI":
            self._paused = False
            (AEON_DATA / "ai_paused").unlink(missing_ok=True)
            log.info("[Sovereign] AI research RESUMED")
        
        elif cmd_type == "FORCE_ROLLBACK":
            version = command.get("target_version", "")
            (AEON_DATA / "rollback_requested.json").write_text(
                json.dumps({"version": version, "at": time.time()}))
            log.warning(f"[Sovereign] ROLLBACK to {version} requested")
        
        elif cmd_type == "SET_VOTE_THRESHOLD":
            self._threshold = command.get("threshold", 0.66)
            (AEON_DATA / "vote_config.json").write_text(
                json.dumps({"threshold": self._threshold}))
            log.info(f"[Sovereign] Vote threshold → {self._threshold:.0%}")
        
        elif cmd_type == "REVOKE_PEER":
            peer_id = command.get("peer_id", "")
            self.db.revoke(peer_id)
            log.warning(f"[Sovereign] Peer REVOKED: {peer_id}")
        
        elif cmd_type == "EMERGENCY_PATCH":
            # Write patch to incoming queue — patch writer handles application
            patch_dir = AEON_DATA / "incoming_patches"
            patch_dir.mkdir(exist_ok=True)
            patch_hash = command.get("patch_hash", "unknown")
            (patch_dir / f"emergency_{patch_hash}.json").write_text(
                json.dumps(command))
            log.warning(f"[Sovereign] EMERGENCY PATCH queued: {patch_hash}")
    
    def poll_and_execute(self):
        """Fetch new sovereign commands from GCS and execute them."""
        try:
            import urllib.request
            list_url = (f"https://storage.googleapis.com/storage/v1/b/{GCS_BUCKET}"
                        f"/o?prefix=sovereign/commands/&maxResults=50")
            with urllib.request.urlopen(list_url, timeout=10) as r:
                items = json.loads(r.read()).get("items", [])
            
            for item in items:
                cmd_id = item["name"].split("/")[-1].replace(".json", "")
                if cmd_id in self._seen:
                    continue
                
                # Download the command
                dl_url = item["mediaLink"]
                with urllib.request.urlopen(dl_url, timeout=10) as r:
                    command = json.loads(r.read())
                
                if not self._verify_signature(command):
                    log.error(f"[Sovereign] INVALID SIGNATURE on command {cmd_id}")
                    continue
                
                self._execute(command)
                self._seen.add(cmd_id)
            
            self._save_seen()
            
        except Exception as e:
            log.debug(f"[Sovereign] Poll error: {e}")

# ── Local REST API ────────────────────────────────────────────────────────────

class HiveRESTHandler(BaseHTTPRequestHandler):
    """
    Local REST API on port 7878.
    Used by: AeonMind, AutoUpdater, BuildWorker, SilencePolicy, Sovereign.
    
    GET  /hive/status           — overall hive status
    GET  /hive/peers            — peer list (json)
    GET  /hive/build_workers    — eligible build worker peers
    GET  /vote/<patch_id>       — vote status
    POST /vote/<patch_id>       — cast a vote {"yes": true/false}
    POST /hive/announce         — register as build worker / chunk holder
    GET  /sovereign/paused      — check if AI is paused
    GET  /sovereign/threshold   — current vote threshold
    """
    
    hive: "AeonHive" = None  # Injected before server starts
    
    def log_message(self, fmt, *args):
        pass
    
    def _read_body(self) -> dict:
        length = int(self.headers.get("Content-Length", 0))
        if length:
            return json.loads(self.rfile.read(length))
        return {}
    
    def _send_json(self, data, status=200):
        body = json.dumps(data).encode()
        self.send_response(status)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)
    
    def do_GET(self):
        h = self.hive
        
        if self.path == "/hive/status":
            self._send_json({
                "version": HIVE_VERSION,
                "peer_id": h.local_peer.peer_id,
                "peers_known": h.peer_db.count(),
                "peers_live": len(h.peer_db.get_live()),
                "build_workers": len(h.peer_db.get_build_workers()),
                "open_votes": len(h.vote_ledger.get_open_votes()),
                "ai_paused": h.sovereign.i_paused,
                "uptime_seconds": int(time.time() - h.start_time)
            })
        
        elif self.path == "/hive/peers":
            peers = h.peer_db.get_live()
            self._send_json({
                "count": len(peers),
                "peer_ips": [p.ip for p in peers],
                "peers": [p.to_dict() for p in peers[:50]]
            })
        
        elif self.path == "/hive/build_workers":
            workers = h.peer_db.get_build_workers()
            self._send_json({
                "count": len(workers),
                "workers": [
                    {"peer_id": p.peer_id, "ip": p.ip,
                     "cpu_cores": p.build_cpu_cores,
                     "reputation": p.reputation}
                    for p in workers
                ]
            })
        
        elif self.path.startswith("/vote/"):
            patch_id = self.path.split("/vote/")[1]
            status = h.vote_ledger.get_status(patch_id)
            if status:
                self._send_json(status)
            else:
                self._send_json({"error": "not_found"}, 404)
        
        elif self.path == "/vote/open":
            self._send_json(h.vote_ledger.get_open_votes())
        
        elif self.path == "/sovereign/paused":
            self._send_json({"paused": h.sovereign._paused})
        
        elif self.path == "/sovereign/threshold":
            self._send_json({"threshold": h.sovereign._threshold})
        
        else:
            self._send_json({"error": "not_found"}, 404)
    
    def do_POST(self):
        h = self.hive
        body = self._read_body()
        
        if self.path.startswith("/vote/"):
            patch_id = self.path.split("/vote/")[1]
            result = h.vote_ledger.cast_vote(
                patch_id,
                h.local_peer.peer_id,
                body.get("yes", True)
            )
            # Gossip vote to peers
            h.gossip.broadcast_vote(patch_id, body.get("yes", True))
            self._send_json(result)
        
        elif self.path == "/hive/announce":
            # Register as build worker
            ann_type = body.get("type", "")
            if ann_type == "build_worker":
                h.local_peer.is_build_worker = True
                h.local_peer.build_cpu_cores = body.get("cpu_cores", 0)
                # Gossip announcement to peers
                msg = json.dumps({
                    "type": "ANNOUNCE",
                    "type_detail": "build_worker",
                    "peer_id": h.local_peer.peer_id,
                    "cpu_cores": body.get("cpu_cores", 0)
                }).encode()
                sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
                for peer in h.peer_db.get_random_sample(10):
                    try: sock.sendto(msg, (peer.ip, peer.gossip_port))
                    except Exception: pass
                sock.close()
            self._send_json({"ok": True})
        
        else:
            self._send_json({"error": "not_found"}, 404)

# ── Bootstrap ─────────────────────────────────────────────────────────────────

def bootstrap_from_gcs(peer_db: PeerDB, local_peer: Peer):
    """Fetch initial peer list from GCS, add them to the DB."""
    try:
        with urllib.request.urlopen(BOOTSTRAP_URL, timeout=10) as r:
            data = json.loads(r.read())
        
        peers = data.get("peers", [])
        log.info(f"[Bootstrap] Found {len(peers)} bootstrap peers")
        
        for pd in peers:
            if pd.get("peer_id") == local_peer.peer_id:
                continue
            p = Peer(
                peer_id=pd["peer_id"],
                ip=pd["ip"],
                gossip_port=pd.get("gossip_port", GOSSIP_PORT),
                chunk_port=pd.get("chunk_port", CHUNK_PORT)
            )
            peer_db.add_or_update(p)
        
        # Register ourselves on GCS bootstrap list
        _register_on_bootstrap(local_peer)
        
    except Exception as e:
        log.warning(f"[Bootstrap] GCS unavailable: {e}")
        log.info("[Bootstrap] Using cached peer list only")

def _register_on_bootstrap(local_peer: Peer):
    """
    Tell GCS we exist so other new peers can find us.
    This is Phase 1/2 only — Phase 3 uses pure DHT.
    """
    try:
        # Simple: upload our peer record to a well-known path
        from google.cloud import storage
        gcs = storage.Client()
        blob = gcs.bucket(GCS_BUCKET).blob(
            f"hive/peers/{local_peer.peer_id}.json")
        blob.upload_from_string(
            json.dumps(local_peer.to_dict()),
            content_type="application/json"
        )
    except Exception:
        pass  # Not critical — other peers will find us via gossip eventually

def get_public_ip() -> str:
    """Get our public IP address."""
    services = [
        "https://api.ipify.org",
        "https://ifconfig.me/ip",
        "https://icanhazip.com"
    ]
    for url in services:
        try:
            with urllib.request.urlopen(url, timeout=5) as r:
                return r.read().decode().strip()
        except Exception:
            continue
    return "127.0.0.1"

# ── Main hive orchestrator ────────────────────────────────────────────────────

class AeonHive:
    def __init__(self):
        AEON_DATA.mkdir(parents=True, exist_ok=True)
        CHUNK_CACHE.mkdir(parents=True, exist_ok=True)
        
        # Generate or load persistent peer ID
        id_file = AEON_DATA / "peer_id"
        if id_file.exists():
            peer_id = id_file.read_text().strip()
        else:
            peer_id = hashlib.sha256(
                secrets.token_bytes(32) + socket.getfqdn().encode()
            ).hexdigest()[:32]
            id_file.write_text(peer_id)
        
        my_ip = get_public_ip()
        
        self.local_peer = Peer(
            peer_id=peer_id,
            ip=my_ip,
            version=HIVE_VERSION
        )
        
        self.peer_db      = PeerDB()
        self.vote_ledger  = VoteLedger()
        self.sovereign    = SovereignCommandProcessor(
                                self.peer_db, self.vote_ledger)
        self.gossip       = GossipServer(
                                self.peer_db, self.vote_ledger, self.local_peer)
        self.start_time   = time.time()
        self._stop        = threading.Event()
        
        log.info(f"[Hive] Peer ID: {peer_id}")
        log.info(f"[Hive] My IP:   {my_ip}")
    
    def _background_loops(self):
        """Run all periodic background tasks in one thread."""
        last_gossip    = 0.0
        last_sovereign = 0.0
        last_peer_save = 0.0
        
        while not self._stop.is_set():
            now = time.time()
            
            if now - last_gossip >= GOSSIP_INTERVAL:
                self.gossip.gossip_round()
                self.peer_db.remove_dead()
                last_gossip = now
            
            if now - last_sovereign >= SOVEREIGN_POLL:
                self.sovereign.poll_and_execute()
                last_sovereign = now
            
            if now - last_peer_save >= 300:
                self.peer_db.save()
                self.vote_ledger.save()
                last_peer_save = now
            
            time.sleep(10)
    
    def start(self):
        """Start all hive services."""
        log.info("[Hive] Starting AeonHive...")
        
        # Bootstrap peer discovery
        bootstrap_from_gcs(self.peer_db, self.local_peer)
        log.info(f"[Hive] {self.peer_db.count()} peers known after bootstrap")
        
        # Gossip UDP server
        t_gossip = threading.Thread(
            target=self.gossip.start_server,
            daemon=True, name="HiveGossip")
        t_gossip.start()
        
        # Background maintenance loop
        t_bg = threading.Thread(
            target=self._background_loops,
            daemon=True, name="HiveBackground")
        t_bg.start()
        
        # Chunk HTTP server (non-blocking)
        chunk_srv = HTTPServer(("0.0.0.0", CHUNK_PORT), ChunkServer)
        t_chunk = threading.Thread(
            target=chunk_srv.serve_forever,
            daemon=True, name="HiveChunk")
        t_chunk.start()
        log.info(f"[Hive] Chunk server on :{CHUNK_PORT}")
        
        # Local REST API (blocking — main thread)
        HiveRESTHandler.hive = self
        rest_srv = HTTPServer(("127.0.0.1", REST_PORT), HiveRESTHandler)
        log.info(f"[Hive] REST API on 127.0.0.1:{REST_PORT}")
        log.info(f"[Hive] All services online ✓")
        
        try:
            rest_srv.serve_forever()
        except KeyboardInterrupt:
            self.stop()
    
    def stop(self):
        log.info("[Hive] Shutting down...")
        self._stop.set()
        self.gossip.stop()
        self.peer_db.save()
        self.vote_ledger.save()

# ── Entry point ───────────────────────────────────────────────────────────────

if __name__ == "__main__":
    logging.basicConfig(
        level=logging.INFO,
        format="%(asctime)s [%(name)s] %(message)s",
        datefmt="%H:%M:%S"
    )
    
    parser = argparse.ArgumentParser(description="AeonHive P2P Mesh Node")
    parser.add_argument("--debug", action="store_true")
    args = parser.parse_args()
    
    if args.debug:
        logging.getLogger().setLevel(logging.DEBUG)
    
    hive = AeonHive()
    hive.start()
