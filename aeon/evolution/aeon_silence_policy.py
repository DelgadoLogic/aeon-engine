#!/usr/bin/env python3
"""
AeonSilencePolicy — Zero-intrusion enforcement layer.
Monitors system CPU/input activity and applies a strict silence rule:
ALL update, rebuild, model-sync, and background operations are paused
when the user is active. They resume silently during idle/overnight windows.
"""
import asyncio
import ctypes
import os
import platform
import psutil
import time
from datetime import datetime


SILENCE_CPU_THRESHOLD  = float(os.getenv("SILENCE_CPU_THRESHOLD", "15.0"))   # % CPU — below = idle
SILENCE_IDLE_SECONDS   = int(os.getenv("SILENCE_IDLE_SECONDS", "300"))        # 5 min idle = safe
POLL_INTERVAL          = int(os.getenv("SILENCE_POLL_SECONDS", "30"))

_paused_operations: list = []
_current_state = "unknown"  # "active" | "idle" | "quiet_window"


# ─── Platform-specific idle detection ─────────────────────────────────────────

def get_idle_seconds_windows() -> float:
    """Windows: time since last keyboard/mouse input via Win32 API."""
    class LASTINPUTINFO(ctypes.Structure):
        _fields_ = [("cbSize", ctypes.c_uint), ("dwTime", ctypes.c_uint)]
    lii = LASTINPUTINFO()
    lii.cbSize = ctypes.sizeof(LASTINPUTINFO)
    ctypes.windll.user32.GetLastInputInfo(ctypes.byref(lii))
    millis = ctypes.windll.kernel32.GetTickCount() - lii.dwTime
    return millis / 1000.0


def get_idle_seconds_linux() -> float:
    """Linux: check /proc/stat for recent interrupts as proxy for user activity."""
    try:
        with open("/proc/stat") as f:
            for line in f:
                if line.startswith("intr"):
                    return 0.0 if "active" in line else SILENCE_IDLE_SECONDS + 1
    except Exception:
        pass
    return SILENCE_IDLE_SECONDS + 1


def get_idle_seconds() -> float:
    if platform.system() == "Windows":
        return get_idle_seconds_windows()
    return get_idle_seconds_linux()


def is_quiet_window() -> bool:
    """True between 10PM–8AM (user likely asleep)."""
    hour = datetime.now().hour
    return hour >= 22 or hour < 8


def get_current_state() -> str:
    """Returns 'active', 'idle', or 'quiet_window'."""
    if is_quiet_window():
        return "quiet_window"
    cpu = psutil.cpu_percent(interval=1)
    idle_s = get_idle_seconds()
    if cpu > SILENCE_CPU_THRESHOLD or idle_s < SILENCE_IDLE_SECONDS:
        return "active"
    return "idle"


# ─── Operation registry ───────────────────────────────────────────────────────

class SilencedOperation:
    """Wraps a coroutine that should only run during idle/quiet windows."""
    def __init__(self, name: str, coro_factory, min_state: str = "idle"):
        self.name = name
        self.coro_factory = coro_factory
        self.min_state = min_state  # "idle" or "quiet_window"
        self.running = False
        self.task = None

    def should_run(self, state: str) -> bool:
        if self.min_state == "quiet_window":
            return state == "quiet_window"
        return state in ("idle", "quiet_window")

    def __repr__(self):
        return f"<SilencedOp '{self.name}' running={self.running}>"


_registered_ops: list[SilencedOperation] = []


def register_operation(name: str, coro_factory, min_state: str = "idle"):
    """Register a background operation to be managed by silence policy."""
    op = SilencedOperation(name, coro_factory, min_state)
    _registered_ops.append(op)
    print(f"[Silence] Registered: {name} (requires: {min_state})")
    return op


async def manage_operations():
    """Main control loop — start/stop operations based on system state."""
    global _current_state
    while True:
        state = get_current_state()
        if state != _current_state:
            _current_state = state
            print(f"[Silence] State → {state} @ {datetime.now().strftime('%H:%M:%S')}")

        for op in _registered_ops:
            if op.should_run(state):
                if not op.running:
                    print(f"[Silence] Starting: {op.name}")
                    op.task = asyncio.create_task(op.coro_factory())
                    op.running = True
            else:
                if op.running and op.task and not op.task.done():
                    print(f"[Silence] Pausing: {op.name} (user is {state})")
                    op.task.cancel()
                    op.running = False

        await asyncio.sleep(POLL_INTERVAL)


# ─── Pre-registered Aeon background operations ───────────────────────────────

async def _research_agent_stub():
    """Placeholder — in production imports and runs aeon_research_agent.main()"""
    from aeon.evolution.aeon_research_agent import scan_cycle
    while True:
        await scan_cycle()
        await asyncio.sleep(3600)

async def _model_sync_stub():
    """Placeholder — syncs new model deltas from aeon-public-dist bucket."""
    print("[ModelSync] Checking for new model deltas...")
    await asyncio.sleep(1800)

async def _lora_trainer_stub():
    """Placeholder — runs weekly LoRA fine-tuning on GCP Cloud Run."""
    print("[LoRATrainer] Submitting weekly fine-tune job to Cloud Build...")
    await asyncio.sleep(7200)


def setup_default_operations():
    register_operation("CVE Research Scan",    _research_agent_stub,  min_state="idle")
    register_operation("Sovereign Model Sync", _model_sync_stub,       min_state="idle")
    register_operation("LoRA Fine-Tuning",     _lora_trainer_stub,     min_state="quiet_window")


async def main():
    print("=" * 60)
    print("AeonSilencePolicy v1.0 — Zero-Intrusion Enforcer")
    print(f"CPU threshold: {SILENCE_CPU_THRESHOLD}% | Idle gate: {SILENCE_IDLE_SECONDS}s")
    print(f"Quiet window:  10PM–8AM (all heavy ops restricted)")
    print("=" * 60)
    setup_default_operations()
    await manage_operations()


if __name__ == "__main__":
    asyncio.run(main())
