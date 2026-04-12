#!/usr/bin/env python3
"""
AeonOS Test Harness v2 — Multi-Era Win32 Validation
DelgadoLogic | CI/CD Infrastructure

Orchestrates automated testing across the full Windows spectrum:
  ERA0: Docker + Wine        (5 sec,  WinSock 1.1 validation)
  ERA1: DOSBox-X             (5 sec,  Win 3.1/DOS 16-bit validation)
  ERA2: QEMU + Win9x         (60 sec, Real Win98 kernel)
  ERA3: QEMU + ReactOS/NT    (90 sec, Real NT 5.2 kernel)
  ERA4: QEMU + Modern WinPE  (30 sec, Vista/7/8/10)
  ERA5: Native Windows       (0 sec,  Direct execution)

Usage:
    python aeon_test_harness.py                     # Auto-detect & run all available
    python aeon_test_harness.py --tier wine         # Wine only (fast CI gate)
    python aeon_test_harness.py --tier reactos      # ReactOS only
    python aeon_test_harness.py --tier all          # Run everything
    python aeon_test_harness.py --json              # JSON output for CI parsing

Environment variables:
    REACTOS_ISO     Path to ReactOS LiveCD ISO
    WIN98_IMAGE     Path to Win98 QCOW2 golden image
    QEMU_TIMEOUT    Max seconds to wait for results (default: 180)
    TEST_EXE        Path to tls_test.exe (default: tls_test.exe)
"""

import os
import sys
import json
import time
import shutil
import platform
import subprocess
import tempfile
import argparse
from pathlib import Path
from datetime import datetime
from dataclasses import dataclass, field, asdict
from typing import Optional, Dict, List


@dataclass
class TierResult:
    """Structured result from a single test tier."""
    tier: str
    name: str
    status: str  # "pass", "fail", "skip", "timeout", "error"
    duration_sec: float = 0.0
    output: str = ""
    error: str = ""
    exit_code: int = -1
    metadata: Dict = field(default_factory=dict)


class AeonTestHarness:
    """Orchestrates headless Win32 binary testing across multiple eras."""

    TIER_REGISTRY = {
        "wine":     {"era": 0, "name": "Docker + Wine",        "method": "run_wine"},
        "dosbox":   {"era": 1, "name": "DOSBox-X (Win3.1)",    "method": "run_dosbox"},
        "win9x":    {"era": 2, "name": "QEMU + Win98",         "method": "run_win9x"},
        "reactos":  {"era": 3, "name": "QEMU + ReactOS",       "method": "run_reactos"},
        "winpe":    {"era": 4, "name": "WinPE (Modern NT)",     "method": "run_winpe"},
        "native":   {"era": 5, "name": "Native Windows",       "method": "run_native"},
    }

    def __init__(self, args):
        self.test_exe = Path(args.test_exe)
        self.timeout = args.timeout
        self.serial_log = Path(tempfile.gettempdir()) / "aeon_serial.log"
        self.qemu_proc = None
        self.iso_path = args.iso
        self.win98_image = args.win98_image or os.environ.get("WIN98_IMAGE")
        self.tiers = args.tier
        self.json_output = args.json
        self.verbose = args.verbose
        self.results: List[TierResult] = []

    # ──── Logging ────────────────────────────────────────────

    def log(self, msg, level="INFO"):
        if self.json_output:
            return  # Suppress human output in JSON mode
        ts = datetime.now().strftime("%H:%M:%S")
        prefix = {"INFO": "→", "PASS": "✓", "FAIL": "✗", "WARN": "⚠", "SKIP": "◌"}
        print(f"[{ts}] {prefix.get(level, '·')} {msg}")

    # ──── Tier 0: Wine ───────────────────────────────────────

    def run_wine(self) -> TierResult:
        """ERA0: Direct Wine execution (fastest, no QEMU)."""
        self.log("=== ERA0: Wine Direct Test ===")
        start = time.time()

        wine = shutil.which("wine")
        if not wine:
            return TierResult("wine", "Docker + Wine", "skip",
                              error="Wine not found on system")

        if not self.test_exe.exists():
            return TierResult("wine", "Docker + Wine", "error",
                              error=f"Binary not found: {self.test_exe}")

        env = os.environ.copy()
        env["WINEDEBUG"] = "-all"
        env["WINEPREFIX"] = str(Path(tempfile.gettempdir()) / "aeon_wine")

        try:
            result = subprocess.run(
                [wine, str(self.test_exe)],
                capture_output=True, text=True,
                timeout=60, env=env
            )
            output = result.stdout + result.stderr
            duration = time.time() - start

            if "AEON_TEST_RESULT=PASS" in output:
                self.log("Wine test PASSED", "PASS")
                return TierResult("wine", "Docker + Wine", "pass",
                                  duration, output, exit_code=result.returncode)
            elif "AEON_TEST_RESULT=FAIL" in output:
                self.log("Wine test FAILED", "FAIL")
                return TierResult("wine", "Docker + Wine", "fail",
                                  duration, output, exit_code=result.returncode)
            else:
                status = "pass" if result.returncode == 0 else "fail"
                self.log(f"No result marker (exit={result.returncode})", "WARN")
                return TierResult("wine", "Docker + Wine", status,
                                  duration, output, exit_code=result.returncode)

        except subprocess.TimeoutExpired:
            return TierResult("wine", "Docker + Wine", "timeout",
                              time.time() - start, error="Wine execution timed out (60s)")
        except Exception as e:
            return TierResult("wine", "Docker + Wine", "error",
                              time.time() - start, error=str(e))

    # ──── Tier 1: DOSBox-X (Win3.1) ─────────────────────────

    def run_dosbox(self) -> TierResult:
        """ERA1: DOSBox-X for Win 3.1 / DOS 16-bit testing."""
        self.log("=== ERA1: DOSBox-X (Win3.1/DOS) ===")
        start = time.time()

        dosbox = shutil.which("dosbox-x") or shutil.which("dosbox")
        if not dosbox:
            return TierResult("dosbox", "DOSBox-X (Win3.1)", "skip",
                              error="DOSBox-X not found on system")

        # Would need a 16-bit test binary built with wcc (not wcc386)
        # and a pre-configured DOS HDD image
        return TierResult("dosbox", "DOSBox-X (Win3.1)", "skip",
                          time.time() - start,
                          error="16-bit test binary not yet built (requires wcc 16-bit mode)")

    # ──── Tier 2: Win9x QEMU ────────────────────────────────

    def run_win9x(self) -> TierResult:
        """ERA2: QEMU + Win98 SE golden image."""
        self.log("=== ERA2: QEMU + Win98 ===")
        start = time.time()

        qemu = shutil.which("qemu-system-i386")
        if not qemu:
            return TierResult("win9x", "QEMU + Win98", "skip",
                              error="qemu-system-i386 not found")

        if not self.win98_image or not Path(self.win98_image).exists():
            return TierResult("win9x", "QEMU + Win98", "skip",
                              error="Win98 golden image not found (set WIN98_IMAGE)")

        # Create payload disk with test binary
        payload = self._create_payload_disk()
        if not payload:
            return TierResult("win9x", "QEMU + Win98", "error",
                              error="Failed to create payload disk")

        cmd = [
            qemu,
            "-m", "64",
            "-cpu", "pentium",
            "-vga", "cirrus",
            "-hda", self.win98_image,
            "-hdb", str(payload),
            "-display", "none",
            "-serial", f"file:{self.serial_log}",
            "-no-reboot",
            "-net", "nic,model=ne2k_pci",
            "-net", "user",
        ]

        return self._run_qemu_tier("win9x", "QEMU + Win98", cmd, start)

    # ──── Tier 3: ReactOS QEMU ───────────────────────────────

    def run_reactos(self) -> TierResult:
        """ERA3: QEMU + ReactOS (NT 5.2 compatible)."""
        self.log("=== ERA3: QEMU + ReactOS ===")
        start = time.time()

        qemu = shutil.which("qemu-system-i386")
        if not qemu:
            return TierResult("reactos", "QEMU + ReactOS", "skip",
                              error="qemu-system-i386 not found")

        iso = self.iso_path or os.environ.get("REACTOS_ISO")
        if not iso or not Path(iso).exists():
            return TierResult("reactos", "QEMU + ReactOS", "skip",
                              error="ReactOS ISO not found (set REACTOS_ISO or --iso)")

        payload = self._create_payload_disk()

        cmd = [
            qemu,
            "-m", "512",
            "-boot", "d",
            "-cdrom", iso,
            "-display", "none",
            "-serial", f"file:{self.serial_log}",
            "-no-reboot",
            "-net", "nic,model=rtl8139",
            "-net", "user",
        ]
        if payload:
            cmd.extend(["-drive", f"file={payload},format=raw,if=ide"])

        return self._run_qemu_tier("reactos", "QEMU + ReactOS", cmd, start)

    # ──── Tier 4: WinPE ──────────────────────────────────────

    def run_winpe(self) -> TierResult:
        """ERA4: WinPE custom ISO (real modern Windows kernel, CLI-only)."""
        self.log("=== ERA4: WinPE ===")
        return TierResult("winpe", "WinPE (Modern NT)", "skip",
                          error="WinPE ISO not yet built (requires Windows ADK)")

    # ──── Tier 5: Native Windows ─────────────────────────────

    def run_native(self) -> TierResult:
        """ERA5: Direct execution on native Windows (highest fidelity)."""
        self.log("=== ERA5: Native Windows ===")
        start = time.time()

        if platform.system() != "Windows":
            return TierResult("native", "Native Windows", "skip",
                              error="Not running on Windows")

        if not self.test_exe.exists():
            return TierResult("native", "Native Windows", "error",
                              error=f"Binary not found: {self.test_exe}")

        try:
            result = subprocess.run(
                [str(self.test_exe)],
                capture_output=True, text=True,
                timeout=60
            )
            output = result.stdout + result.stderr
            duration = time.time() - start

            if "AEON_TEST_RESULT=PASS" in output:
                self.log("Native test PASSED", "PASS")
                return TierResult("native", "Native Windows", "pass",
                                  duration, output, exit_code=result.returncode,
                                  metadata={"os": platform.platform()})
            elif "AEON_TEST_RESULT=FAIL" in output:
                self.log("Native test FAILED", "FAIL")
                return TierResult("native", "Native Windows", "fail",
                                  duration, output, exit_code=result.returncode,
                                  metadata={"os": platform.platform()})
            else:
                status = "pass" if result.returncode == 0 else "fail"
                return TierResult("native", "Native Windows", status,
                                  duration, output, exit_code=result.returncode,
                                  metadata={"os": platform.platform()})

        except subprocess.TimeoutExpired:
            return TierResult("native", "Native Windows", "timeout",
                              time.time() - start, error="Execution timed out (60s)")
        except Exception as e:
            return TierResult("native", "Native Windows", "error",
                              time.time() - start, error=str(e))

    # ──── QEMU Shared Logic ──────────────────────────────────

    def _run_qemu_tier(self, tier_id, tier_name, cmd, start) -> TierResult:
        """Boot QEMU and monitor serial output for result markers."""
        # Clear serial log
        if self.serial_log.exists():
            self.serial_log.unlink()
        self.serial_log.touch()

        self.log(f"QEMU: {' '.join(cmd[:6])}...")
        self.log(f"Serial: {self.serial_log}")

        try:
            self.qemu_proc = subprocess.Popen(
                cmd, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL
            )
        except FileNotFoundError:
            return TierResult(tier_id, tier_name, "error",
                              error="QEMU binary not found")

        # Monitor serial output
        boot_detected = False
        while (time.time() - start) < self.timeout:
            time.sleep(5)
            elapsed = int(time.time() - start)

            if self.qemu_proc.poll() is not None:
                self.log(f"[{elapsed}s] QEMU exited (code={self.qemu_proc.returncode})", "WARN")
                break

            if self.serial_log.exists():
                content = self.serial_log.read_text(errors="replace")

                if not boot_detected and any(k in content for k in ["ReactOS", "Loading", "Windows"]):
                    self.log(f"[{elapsed}s] OS boot detected")
                    boot_detected = True

                if "AEON_TEST_RESULT=PASS" in content:
                    self.log(f"[{elapsed}s] TEST PASSED via serial", "PASS")
                    self._cleanup_qemu()
                    return TierResult(tier_id, tier_name, "pass",
                                      time.time() - start, content)

                if "AEON_TEST_RESULT=FAIL" in content:
                    self.log(f"[{elapsed}s] TEST FAILED via serial", "FAIL")
                    self._cleanup_qemu()
                    return TierResult(tier_id, tier_name, "fail",
                                      time.time() - start, content)

                self.log(f"[{elapsed}s] serial: {len(content)} bytes")

        self._cleanup_qemu()
        serial_content = self.serial_log.read_text(errors="replace") if self.serial_log.exists() else ""
        return TierResult(tier_id, tier_name, "timeout",
                          time.time() - start, serial_content,
                          error=f"Timeout ({self.timeout}s) — no result marker")

    def _create_payload_disk(self) -> Optional[Path]:
        """Create a FAT16 disk image with tls_test.exe injected."""
        disk_path = Path(tempfile.gettempdir()) / "aeon_payload.img"

        try:
            subprocess.run(
                ["qemu-img", "create", "-f", "raw", str(disk_path), "16M"],
                check=True, capture_output=True
            )
        except (subprocess.CalledProcessError, FileNotFoundError):
            self.log("Cannot create payload disk (qemu-img missing)", "WARN")
            return None

        mformat = shutil.which("mformat")
        mcopy = shutil.which("mcopy")
        if mformat and mcopy and self.test_exe.exists():
            try:
                subprocess.run([mformat, "-i", str(disk_path), "-F", "::"],
                               check=True, capture_output=True)
                subprocess.run([mcopy, "-i", str(disk_path),
                               str(self.test_exe), "::tls_test.exe"],
                               check=True, capture_output=True)
                self.log("Injected tls_test.exe via mtools")
            except subprocess.CalledProcessError as e:
                self.log(f"mtools failed: {e}", "WARN")

        return disk_path

    def _cleanup_qemu(self):
        """Kill QEMU process if running."""
        if self.qemu_proc and self.qemu_proc.poll() is None:
            self.qemu_proc.terminate()
            try:
                self.qemu_proc.wait(timeout=5)
            except subprocess.TimeoutExpired:
                self.qemu_proc.kill()

    # ──── Orchestration ──────────────────────────────────────

    def _get_tiers_to_run(self) -> List[str]:
        """Determine which tiers to execute based on args and availability."""
        if self.tiers == ["all"]:
            return list(self.TIER_REGISTRY.keys())

        if self.tiers:
            return [t for t in self.tiers if t in self.TIER_REGISTRY]

        # Auto-detect: run what's available
        available = []
        if shutil.which("wine"):
            available.append("wine")
        if shutil.which("dosbox-x") or shutil.which("dosbox"):
            available.append("dosbox")
        if shutil.which("qemu-system-i386"):
            if self.win98_image:
                available.append("win9x")
            if self.iso_path or os.environ.get("REACTOS_ISO"):
                available.append("reactos")
        if platform.system() == "Windows":
            available.append("native")

        return available if available else ["wine"]  # Default to Wine

    def run(self) -> int:
        """Execute the full tiered test strategy."""
        if not self.json_output:
            print("=" * 55)
            print("  AeonOS Test Harness v2 — DelgadoLogic")
            print(f"  Binary:  {self.test_exe}")
            print(f"  Host:    {platform.system()} {platform.machine()}")
            print(f"  Time:    {datetime.now().strftime('%Y-%m-%d %H:%M:%S EST')}")
            print("=" * 55)
            print()

        tiers = self._get_tiers_to_run()
        self.log(f"Running tiers: {', '.join(tiers)}")

        for tier_id in tiers:
            tier_info = self.TIER_REGISTRY[tier_id]
            method = getattr(self, tier_info["method"])
            result = method()
            self.results.append(result)

            if self.verbose and result.output:
                print(f"\n--- {tier_id} output ---")
                print(result.output[-2000:])
                print("--- end ---\n")

        # Output
        if self.json_output:
            output = {
                "timestamp": datetime.now().isoformat(),
                "binary": str(self.test_exe),
                "host": platform.platform(),
                "tiers": [asdict(r) for r in self.results],
                "overall": self._overall_status(),
            }
            print(json.dumps(output, indent=2))
        else:
            print()
            print("=" * 55)
            print("  RESULTS SUMMARY")
            print("=" * 55)
            for r in self.results:
                icon = {"pass": "✓", "fail": "✗", "skip": "◌",
                        "timeout": "⏱", "error": "⚠"}.get(r.status, "?")
                dur = f"{r.duration_sec:.1f}s" if r.duration_sec > 0 else "—"
                err = f" ({r.error})" if r.error and r.status != "pass" else ""
                print(f"  {icon} ERA{self.TIER_REGISTRY[r.tier]['era']} "
                      f"{r.name:.<30s} {r.status.upper():>8s}  {dur:>6s}{err}")
            print("=" * 55)
            overall = self._overall_status()
            self.log(f"Overall: {overall.upper()}", overall.upper())

        # Exit code
        overall = self._overall_status()
        if overall == "pass":
            return 0
        elif overall == "fail":
            return 1
        else:
            return 2

    def _overall_status(self) -> str:
        """Determine overall status from all tier results."""
        statuses = [r.status for r in self.results]
        if any(s == "pass" for s in statuses):
            return "pass"
        elif any(s == "fail" for s in statuses):
            return "fail"
        else:
            return "skip"


def main():
    parser = argparse.ArgumentParser(
        description="AeonOS Multi-Era Win32 Test Harness",
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog="""
Tiers:
  wine      ERA0: Wine on Linux (fast CI gate)
  dosbox    ERA1: DOSBox-X (Win3.1/DOS 16-bit)
  win9x     ERA2: QEMU + Win98 SE golden image
  reactos   ERA3: QEMU + ReactOS (NT 5.2 kernel)
  winpe     ERA4: WinPE custom ISO (modern NT)
  native    ERA5: Direct execution on Windows

Examples:
  %(prog)s --tier wine              # Just Wine (fastest)
  %(prog)s --tier wine reactos      # Wine + ReactOS
  %(prog)s --tier all --json        # All tiers, JSON output
  %(prog)s                          # Auto-detect available tiers
""")
    parser.add_argument("--test-exe", default=os.environ.get("TEST_EXE", "tls_test.exe"),
                        help="Path to test executable")
    parser.add_argument("--iso", default=os.environ.get("REACTOS_ISO"),
                        help="Path to bootable OS ISO (ReactOS, WinPE)")
    parser.add_argument("--win98-image", default=None,
                        help="Path to Win98 SE QCOW2 golden image")
    parser.add_argument("--timeout", type=int,
                        default=int(os.environ.get("QEMU_TIMEOUT", "180")),
                        help="Max seconds to wait for QEMU results (default: 180)")
    parser.add_argument("--tier", nargs="+", default=None,
                        help="Specific tier(s) to run (wine, dosbox, win9x, reactos, winpe, native, all)")
    parser.add_argument("--json", action="store_true",
                        help="Output results as JSON (for CI parsing)")
    parser.add_argument("--verbose", "-v", action="store_true",
                        help="Show full test output")
    args = parser.parse_args()

    # Default: auto-detect
    if args.tier is None:
        args.tier = []

    harness = AeonTestHarness(args)

    try:
        sys.exit(harness.run())
    except KeyboardInterrupt:
        harness._cleanup_qemu()
        print("\nAborted.")
        sys.exit(130)


if __name__ == "__main__":
    main()
