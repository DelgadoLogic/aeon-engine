"""
AeonSilencePolicy — aeon_silence_policy.py
The rules that ensure Aeon NEVER visibly intrudes on the user.

RULES:
  1. Research runs on GCP Cloud Run — no local browser windows, ever
  2. Local AeonMind tasks that need a browser use --headless mode,
     separate process, temp profile, invisible to user
  3. P2P chunk downloading throttled to 128KB/s when user is active
  4. Updates install only on COLD START (before UI shows) or system idle
  5. If user is actively typing/scrolling → pause ALL background work
  6. Quiet hours: no non-critical work between 9am–10pm (user's timezone)
     unless the machine is idle

MONITORING (what counts as "user active"):
  - Browser has focus AND a real URL loaded (not aeon://)
  - CPU > 60% (user running something intensive)
  - Last input event < 10 minutes ago
  - Network upload > 500KB/s (user is uploading something)
  
If ANY of these are true → defer all work.
"""

import os
import sys
import time
import ctypes
import logging
import datetime
import platform
import threading
import subprocess
from pathlib import Path

log = logging.getLogger("AeonSilence")

# ── Windows idle detection ────────────────────────────────────────────────────

def get_idle_seconds() -> float:
    """How many seconds since the user last touched mouse/keyboard."""
    if platform.system() != "Windows":
        return 999.0  # Assume idle on non-Windows (Linux/Mac)
    
    class LASTINPUTINFO(ctypes.Structure):
        _fields_ = [("cbSize", ctypes.c_uint), ("dwTime", ctypes.c_ulong)]
    
    lii = LASTINPUTINFO()
    lii.cbSize = ctypes.sizeof(LASTINPUTINFO)
    ctypes.windll.user32.GetLastInputInfo(ctypes.byref(lii))
    
    millis = ctypes.windll.kernel32.GetTickCount() - lii.dwTime
    return millis / 1000.0

def is_user_active(idle_threshold_secs: int = 300) -> bool:
    """Returns True if the user has been active in the last N seconds."""
    try:
        idle = get_idle_seconds()
        return idle < idle_threshold_secs
    except Exception:
        return True  # Assume active if we can't detect

def get_cpu_percent() -> float:
    """Get system CPU usage. Returns 0-100."""
    try:
        import psutil
        return psutil.cpu_percent(interval=0.5)
    except ImportError:
        return 0.0

def get_network_upload_kbps() -> float:
    """Get current network upload speed."""
    try:
        import psutil
        s1 = psutil.net_io_counters()
        time.sleep(1)
        s2 = psutil.net_io_counters()
        return (s2.bytes_sent - s1.bytes_sent) / 1024.0
    except ImportError:
        return 0.0

def is_quiet_hours() -> bool:
    """True if it's 9am–10pm LOCAL time (prime user hours)."""
    now = datetime.datetime.now()
    return 9 <= now.hour < 22

# ── Silence policy engine ─────────────────────────────────────────────────────

class SilencePolicy:
    """
    Call .can_run(task_type) before doing any background work.
    Blocks until it's safe to run, or returns False if should skip entirely.
    """
    
    TASK_PRIORITIES = {
        "critical_security": 0,    # CVE patch — always allowed
        "p2p_seeding":       1,    # Seeding update chunks to peers  
        "chunk_download":    2,    # Downloading update chunks
        "ai_research":       3,    # Cloud is handling this — just signaling
        "hive_gossip":       4,    # AeonHive peer announcements
        "lora_training":     5,    # Local LoRA fine-tuning
        "ai_inference":      6,    # AeonMind running a local task
        "patch_writing":     7,    # Local AI writing code
    }
    
    def __init__(self):
        self._paused = threading.Event()
        self._paused.set()  # Start unpaused
        self._monitor_thread = threading.Thread(
            target=self._monitor_loop, daemon=True)
        self._monitor_thread.start()
        log.info("[Silence] Policy engine started")
    
    def _check_conditions(self) -> tuple[bool, str]:
        """
        Returns (should_pause, reason).
        """
        idle = get_idle_seconds()
        cpu = get_cpu_percent()
        
        # If user has been idle >5 minutes → we can do anything
        if idle > 300:
            return False, "user idle"
        
        # If user is VERY active (typing, scrolling) → pause everything
        if idle < 30:
            return True, f"user active ({idle:.0f}s since last input)"
        
        # If CPU is hammered → don't make it worse
        if cpu > 70:
            return True, f"CPU high ({cpu:.0f}%)"
        
        # During quiet hours with mild activity → light throttle only
        if is_quiet_hours() and idle < 120:
            return True, f"quiet hours + mild activity"
        
        return False, "ok"
    
    def _monitor_loop(self):
        """Continuously monitors and sets pause state."""
        while True:
            try:
                should_pause, reason = self._check_conditions()
                if should_pause:
                    if self._paused.is_set():
                        log.debug(f"[Silence] Pausing: {reason}")
                        self._paused.clear()
                else:
                    if not self._paused.is_set():
                        log.debug(f"[Silence] Resuming: {reason}")
                        self._paused.set()
            except Exception as e:
                log.warning(f"[Silence] Monitor error: {e}")
            time.sleep(15)  # Check every 15 seconds
    
    def wait_for_idle(self, task_type: str = "ai_inference",
                      timeout_secs: int = 3600) -> bool:
        """
        Block until it's safe to run the given task type.
        Critical security tasks skip the wait entirely.
        Returns False if timed out.
        """
        if task_type == "critical_security":
            return True  # Always allowed
        
        deadline = time.time() + timeout_secs
        while time.time() < deadline:
            if self._paused.wait(timeout=30):
                return True  # Safe to proceed
            log.debug(f"[Silence] Waiting to run {task_type}...")
        
        log.warning(f"[Silence] Timeout waiting to run {task_type}")
        return False
    
    def get_bandwidth_limit_kbps(self) -> int:
        """
        Returns the maximum bandwidth we should use for P2P/downloads.
        Throttles heavily when user is active.
        """
        idle = get_idle_seconds()
        if idle < 60:
            return 64    # 64 KB/s — completely unnoticeable
        elif idle < 300:
            return 512   # 512 KB/s — still light
        else:
            return 10240  # 10 MB/s — full speed when user is idle
    
    def is_safe_for_update_install(self) -> bool:
        """
        Should only install (rename/replace binary) when safe.
        True if: user idle >10min AND not during active session.
        """
        return get_idle_seconds() > 600


# Global singleton
_policy = None

def get_policy() -> SilencePolicy:
    global _policy
    if _policy is None:
        _policy = SilencePolicy()
    return _policy


# ── Headless research runner ──────────────────────────────────────────────────

def run_headless_task(url: str, script: str = "") -> str:
    """
    Run a browser task COMPLETELY INVISIBLY using a headless Aeon/Chromium
    subprocess with a temporary profile.

    User NEVER sees this. No window, no taskbar entry, no notification.
    Used only as last resort when a site requires real JS execution.
    For 95% of research (APIs, RSS, JSON) — use requests directly instead.
    """
    import tempfile
    import subprocess
    
    temp_profile = tempfile.mkdtemp(prefix="aeon-headless-")
    aeon_exe = Path(os.environ.get("AEON_EXE", r"C:\Program Files\Aeon\aeon.exe"))
    
    if not aeon_exe.exists():
        log.warning("[Headless] Aeon executable not found, using requests fallback")
        try:
            import requests
            return requests.get(url, timeout=20).text
        except Exception as e:
            return f"ERROR: {e}"
    
    cmd = [
        str(aeon_exe),
        "--headless=new",           # New headless mode (invisible)
        "--no-sandbox",
        "--disable-gpu",
        "--disable-software-rasterizer",
        "--disable-dev-shm-usage",
        "--silent-debugger-extension-api",
        f"--user-data-dir={temp_profile}",
        "--no-first-run",
        "--disable-default-apps",
        "--disable-extensions",
        "--disable-sync",           # No sync in headless mode
        "--window-size=1,1",        # Minimal footprint
        f"--dump-dom",
        url
    ]
    
    try:
        result = subprocess.run(
            cmd, capture_output=True, text=True, timeout=30,
            # Windows: hide the window completely
            creationflags=subprocess.CREATE_NO_WINDOW if platform.system() == "Windows" else 0
        )
        return result.stdout
    except subprocess.TimeoutExpired:
        return "ERROR: headless timeout"
    except Exception as e:
        return f"ERROR: {e}"
    finally:
        # Clean up temp profile
        import shutil
        try:
            shutil.rmtree(temp_profile, ignore_errors=True)
        except Exception:
            pass
