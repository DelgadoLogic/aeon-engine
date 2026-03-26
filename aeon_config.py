"""
Aeon Browser - Central Configuration
All modules import from here. Change once, affects everything.
"""
import os
from pathlib import Path

# ── Engine (ungoogled-chromium 146, pre-built) ────────────────────────────────
CHROME_EXE = r"C:\Users\Manuel A Delgado\AppData\Local\Chromium\Application\chrome.exe"

# Fallback search if path moves (e.g. after update)
if not Path(CHROME_EXE).exists():
    _candidates = [
        Path(os.environ.get("LOCALAPPDATA","")) / "Chromium" / "Application" / "chrome.exe",
        Path("C:/Program Files/ungoogled-chromium/chrome.exe"),
        Path("C:/Program Files (x86)/ungoogled-chromium/chrome.exe"),
    ]
    for _c in _candidates:
        if _c.exists():
            CHROME_EXE = str(_c)
            break

# ── CDP ───────────────────────────────────────────────────────────────────────
CDP_PORT           = 9222
CDP_HOST           = "127.0.0.1"
CDP_URL            = f"http://{CDP_HOST}:{CDP_PORT}"

# ── Paths ─────────────────────────────────────────────────────────────────────
AEON_ROOT          = Path(__file__).parent
AEON_PROFILE_DIR   = AEON_ROOT / "profile"          # browser user data
AEON_LOGS_DIR      = AEON_ROOT / "logs"
AEON_MODELS_DIR    = AEON_ROOT / "models"
AEON_MEMORY_DIR    = AEON_ROOT / "memory"

# ── LLM ───────────────────────────────────────────────────────────────────────
DEFAULT_MODEL      = "phi4"                          # Ollama model
OLLAMA_BASE_URL    = "http://localhost:11434"

# ── AeonHive ──────────────────────────────────────────────────────────────────
HIVE_PORT          = 7879
HIVE_BROADCAST_PORT= 7880

# ── Agent API server ──────────────────────────────────────────────────────────
AGENT_PORT         = 7878

# ── Chrome launch flags (privacy + CDP + performance) ─────────────────────────
CHROME_FLAGS = [
    f"--remote-debugging-port={CDP_PORT}",
    f"--user-data-dir={AEON_PROFILE_DIR}",
    "--no-first-run",
    "--no-default-browser-check",
    "--disable-sync",
    "--disable-background-networking",
    "--disable-client-side-phishing-detection",
    "--disable-default-apps",
    "--disable-extensions-except=",           # allow only explicit extensions
    "--disable-hang-monitor",
    "--metrics-recording-only",               # metrics go nowhere (ungoogled default)
    "--safebrowsing-disable-auto-update",
    "--disable-breakpad",
    "--disable-domain-reliability",
    "--no-pings",
    "--disable-features=TranslateUI,OptimizationGuideModelDownloading",
]
