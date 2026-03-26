"""
╔══════════════════════════════════════════════════════════╗
║          AeonMind — Autonomous Browser Agent v0.3        ║
║   CDP-native • Local LLM • AeonSelf • AeonHive-ready     ║
║   Engine: ungoogled-chromium 146 (pre-built, privacy)    ║
╚══════════════════════════════════════════════════════════╝

Architecture:
  AeonMind uses browser-use for CDP control + Ollama for
  local LLM inference. The agent loop:
    1. Receives a task (string or queue item)
    2. Plans steps via LLM + episodic memory context
    3. Executes via browser-use (CDP → Chromium)
    4. Reflects on result, logs to memory store
    5. Feeds outcome to AeonSelf (auto LoRA training trigger)
    6. Reports to AeonHive for federated learning

Usage:
    python aeon_mind.py --task "Search for the latest AI news and summarize"
    python aeon_mind.py --task-file tasks.json --model phi4
    python aeon_mind.py --server      # REST API on :7878
    python aeon_mind.py --hive        # also start AeonHive node
"""

import os
import sys
import json
import time
import asyncio
import logging
import argparse
import hashlib
import requests
from pathlib import Path
from datetime import datetime
from dataclasses import dataclass, field, asdict
from typing import Optional, List, Dict, Any

# ── Optional heavy deps, graceful fallback ──────────────────
try:
    from browser_use import Agent, Browser, BrowserConfig
    BROWSER_USE_AVAILABLE = True
except ImportError:
    BROWSER_USE_AVAILABLE = False
    print("[WARN] browser-use not installed. Run: pip install browser-use")

try:
    from langchain_ollama import ChatOllama
    OLLAMA_AVAILABLE = True
except ImportError:
    try:
        from langchain_community.chat_models import ChatOllama
        OLLAMA_AVAILABLE = True
    except ImportError:
        OLLAMA_AVAILABLE = False
        print("[WARN] LangChain-Ollama not installed. Run: pip install langchain-ollama")

# ── AeonSelf integration (self-improvement engine) ───────────
try:
    sys.path.insert(0, str(Path(__file__).parent))
    from aeon_self import AeonSelf
    _aeon_self = AeonSelf()
    SELF_IMPROVE_AVAILABLE = True
    log_placeholder = logging.getLogger("AeonMind")
    log_placeholder.info("🧬 AeonSelf self-improvement engine loaded")
except Exception as _e:
    _aeon_self = None
    SELF_IMPROVE_AVAILABLE = False

# ── Logging ─────────────────────────────────────────────────
logging.basicConfig(
    level=logging.INFO,
    format="%(asctime)s [%(levelname)s] %(name)s → %(message)s",
    datefmt="%H:%M:%S",
)
log = logging.getLogger("AeonMind")

# ── Constants ────────────────────────────────────────────────
AEON_DIR         = Path(__file__).parent
MEMORY_FILE      = AEON_DIR / "agent_memory.json"
TASK_LOG_FILE    = AEON_DIR / "task_log.jsonl"
OLLAMA_BASE_URL  = os.getenv("OLLAMA_BASE_URL", "http://localhost:11434")
DEFAULT_MODEL    = os.getenv("AEON_MODEL", "phi4")
HIVE_ENDPOINT    = os.getenv("AEON_HIVE_URL", "http://localhost:7879")  # AeonHive node
MAX_STEPS        = int(os.getenv("AEON_MAX_STEPS", "25"))
REFLECTION_DEPTH = int(os.getenv("AEON_REFLECT_DEPTH", "3"))

# ── Data Models ──────────────────────────────────────────────

@dataclass
class TaskResult:
    task_id: str
    task: str
    status: str          # "success" | "failed" | "partial"
    result: str
    steps_taken: int
    duration_sec: float
    model: str
    timestamp: str = field(default_factory=lambda: datetime.utcnow().isoformat())
    reflections: List[str] = field(default_factory=list)
    hive_submitted: bool = False

@dataclass
class AgentMemory:
    """Persistent episodic memory store for the agent."""
    tasks_completed: int = 0
    tasks_failed: int = 0
    successful_patterns: List[str] = field(default_factory=list)
    failed_patterns: List[str] = field(default_factory=list)
    domain_knowledge: Dict[str, str] = field(default_factory=dict)
    last_updated: str = field(default_factory=lambda: datetime.utcnow().isoformat())

# ── Memory Manager ───────────────────────────────────────────

class MemoryManager:
    """Loads/saves agent episodic memory from disk."""

    def __init__(self, memory_file: Path = MEMORY_FILE):
        self.path = memory_file
        self.memory = self._load()

    def _load(self) -> AgentMemory:
        if self.path.exists():
            try:
                data = json.loads(self.path.read_text())
                return AgentMemory(**{k: v for k, v in data.items()
                                     if k in AgentMemory.__dataclass_fields__})
            except Exception as e:
                log.warning(f"Memory load error (starting fresh): {e}")
        return AgentMemory()

    def save(self):
        self.memory.last_updated = datetime.utcnow().isoformat()
        self.path.write_text(json.dumps(asdict(self.memory), indent=2))

    def record_success(self, task: str, pattern: str = ""):
        self.memory.tasks_completed += 1
        if pattern and pattern not in self.memory.successful_patterns:
            self.memory.successful_patterns.append(pattern)
        self.save()

    def record_failure(self, task: str, reason: str = ""):
        self.memory.tasks_failed += 1
        if reason and reason not in self.memory.failed_patterns:
            self.memory.failed_patterns.append(reason)
        self.save()

    def add_domain_knowledge(self, key: str, value: str):
        self.memory.domain_knowledge[key] = value
        self.save()

    def get_context_prompt(self) -> str:
        """Returns memory context to prepend to agent prompts."""
        lines = [
            f"[AeonMind Memory] Tasks completed: {self.memory.tasks_completed}, "
            f"failed: {self.memory.tasks_failed}.",
        ]
        if self.memory.successful_patterns:
            lines.append(f"Known working approaches: {'; '.join(self.memory.successful_patterns[-5:])}")
        if self.memory.failed_patterns:
            lines.append(f"Known failure modes to avoid: {'; '.join(self.memory.failed_patterns[-3:])}")
        return " ".join(lines)

# ── Ollama Health Check ──────────────────────────────────────

def check_ollama(model: str = DEFAULT_MODEL) -> bool:
    try:
        r = requests.get(f"{OLLAMA_BASE_URL}/api/tags", timeout=3)
        if r.status_code == 200:
            models_available = [m["name"].split(":")[0] for m in r.json().get("models", [])]
            if model not in models_available:
                log.warning(f"Model '{model}' not found. Available: {models_available}")
                log.info(f"Pulling {model}...")
                requests.post(f"{OLLAMA_BASE_URL}/api/pull", json={"name": model}, timeout=300)
            return True
    except Exception as e:
        log.error(f"Ollama not running at {OLLAMA_BASE_URL}: {e}")
        log.error("Start Ollama: 'ollama serve'  then  'ollama pull phi4'")
    return False

# ── Reflection Engine ────────────────────────────────────────

class ReflectionEngine:
    """
    After each task, the agent reflects on what happened.
    This is the embryonic self-improvement loop:
      - Analyze what worked
      - Identify failure modes
      - Write improved strategies to memory
    In v1 this is LLM-based introspection.
    In v2+ this will generate LoRA training samples for AeonHive.
    """

    def __init__(self, llm, memory: MemoryManager):
        self.llm = llm
        self.memory = memory

    def reflect(self, task: str, result: str, success: bool) -> List[str]:
        if self.llm is None:
            return []

        prompt = f"""You are AeonMind's self-improvement engine.
Task: {task}
Result: {result}
Success: {success}

In 2-3 SHORT sentences:
1. What strategy worked or failed?
2. What would you do differently?
3. Is there a reusable pattern to remember?

Be very concise. No fluff."""

        try:
            response = self.llm.invoke(prompt)
            content = response.content if hasattr(response, "content") else str(response)
            reflections = [s.strip() for s in content.split("\n") if s.strip()]

            # Extract reusable pattern for memory
            if reflections and success:
                pattern = reflections[0][:120]
                self.memory.add_domain_knowledge(
                    hashlib.md5(task.encode()).hexdigest()[:8],
                    pattern
                )

            return reflections
        except Exception as e:
            log.warning(f"Reflection failed: {e}")
            return []

# ── Hive Submission ──────────────────────────────────────────

def submit_to_hive(result: TaskResult) -> bool:
    """
    Send task outcome to AeonHive for federated aggregation.
    In v1: simple HTTP POST to local hive node.
    In v2+: encrypted gradient updates via Flower.
    """
    try:
        payload = {
            "task_hash": hashlib.sha256(result.task.encode()).hexdigest()[:16],
            "status": result.status,
            "steps": result.steps_taken,
            "duration": result.duration_sec,
            "reflections": result.reflections,
            "model": result.model,
            "timestamp": result.timestamp,
        }
        r = requests.post(f"{HIVE_ENDPOINT}/api/outcome", json=payload, timeout=5)
        return r.status_code == 200
    except Exception:
        return False  # Hive offline is OK, agent still works locally

# ── Task Logger ──────────────────────────────────────────────

def log_task(result: TaskResult):
    with open(TASK_LOG_FILE, "a") as f:
        f.write(json.dumps(asdict(result)) + "\n")

# ── Core Agent ───────────────────────────────────────────────

class AeonMindAgent:
    """
    The main autonomous browser agent.
    Wraps browser-use with:
      - Local Ollama LLM inference
      - Episodic memory
      - Reflective self-improvement
      - AeonHive reporting
    """

    def __init__(self, model: str = DEFAULT_MODEL, headless: bool = True):
        self.model = model
        self.headless = headless
        self.memory = MemoryManager()
        self.llm = None
        self.reflector = None
        self._init_llm()

    def _init_llm(self):
        if not OLLAMA_AVAILABLE:
            log.warning("Running without LLM — install langchain-ollama")
            return

        if not check_ollama(self.model):
            return

        try:
            self.llm = ChatOllama(
                model=self.model,
                base_url=OLLAMA_BASE_URL,
                temperature=0.3,
                num_ctx=4096,
            )
            self.reflector = ReflectionEngine(self.llm, self.memory)
            log.info(f"✅ LLM ready: {self.model} @ {OLLAMA_BASE_URL}")
        except Exception as e:
            log.error(f"LLM init failed: {e}")

    async def run_task(self, task: str) -> TaskResult:
        """Execute a single autonomous browser task."""
        task_id = hashlib.sha256(f"{task}{time.time()}".encode()).hexdigest()[:12]
        log.info(f"🚀 Starting task [{task_id}]: {task[:80]}...")
        start = time.time()

        # Enrich task with memory context
        memory_ctx = self.memory.get_context_prompt()
        full_task = f"{memory_ctx}\n\nTask: {task}" if memory_ctx else task

        result_text = ""
        steps = 0
        success = False

        if not BROWSER_USE_AVAILABLE or self.llm is None:
            log.warning("Running in dry-run mode (no browser-use / no LLM)")
            result_text = f"[DRY RUN] Would execute: {task}"
            success = True
            steps = 1
        else:
            try:
                browser = Browser(config=BrowserConfig(headless=self.headless))
                agent = Agent(
                    task=full_task,
                    llm=self.llm,
                    browser=browser,
                    max_actions_per_step=5,
                )
                history = await agent.run(max_steps=MAX_STEPS)
                result_text = history.final_result() or "Task completed (no text result)"
                steps = len(history.history) if hasattr(history, "history") else MAX_STEPS
                success = True
                log.info(f"✅ Task complete in {steps} steps")
            except Exception as e:
                result_text = f"Error: {e}"
                log.error(f"❌ Task failed: {e}")

        duration = round(time.time() - start, 2)

        # Reflect on outcome
        reflections = []
        if self.reflector:
            reflections = self.reflector.reflect(task, result_text, success)
            if reflections:
                log.info(f"💭 Reflection: {reflections[0]}")

        # Record in memory
        if success:
            self.memory.record_success(task)
        else:
            self.memory.record_failure(task, result_text[:100])

        result = TaskResult(
            task_id=task_id,
            task=task,
            status="success" if success else "failed",
            result=result_text,
            steps_taken=steps,
            duration_sec=duration,
            model=self.model,
            reflections=reflections,
        )

        # ── Feed AeonSelf (triggers LoRA training when threshold reached) ──
        if SELF_IMPROVE_AVAILABLE and _aeon_self:
            try:
                _aeon_self.record_task(
                    task_id=task_id,
                    description=task,
                    steps=[{"action": f"step_{i}", "observation": ""}
                           for i in range(steps)],
                    outcome=result.status,
                    model=self.model
                )
            except Exception as _se:
                log.debug(f"AeonSelf record failed: {_se}")

        # Submit to AeonHive
        result.hive_submitted = submit_to_hive(result)
        log_task(result)

        return result

    async def run_task_file(self, task_file: str):
        """Run a batch of tasks from a JSON file."""
        data = json.loads(Path(task_file).read_text())
        tasks = data if isinstance(data, list) else data.get("tasks", [])
        log.info(f"📋 Running {len(tasks)} tasks from {task_file}")
        for item in tasks:
            t = item if isinstance(item, str) else item.get("task", str(item))
            r = await self.run_task(t)
            print(f"\n{'='*60}")
            print(f"TASK:   {r.task[:80]}")
            print(f"STATUS: {r.status} | STEPS: {r.steps_taken} | TIME: {r.duration_sec}s")
            print(f"RESULT: {r.result[:300]}")
            if r.reflections:
                print(f"REFLECT: {r.reflections[0]}")
            print(f"{'='*60}\n")
            await asyncio.sleep(1)

# ── REST API Server mode ─────────────────────────────────────

def run_server(agent: AeonMindAgent, port: int = 7878):
    """Minimal HTTP server for AeonMind API."""
    try:
        from http.server import HTTPServer, BaseHTTPRequestHandler
        import urllib.parse

        class Handler(BaseHTTPRequestHandler):
            def log_message(self, fmt, *args):
                pass  # suppress default logging

            def do_POST(self):
                if self.path == "/task":
                    length = int(self.headers.get("Content-Length", 0))
                    body = json.loads(self.rfile.read(length))
                    task = body.get("task", "")
                    if not task:
                        self.send_response(400)
                        self.end_headers()
                        self.wfile.write(b'{"error": "task required"}')
                        return
                    result = asyncio.run(agent.run_task(task))
                    self.send_response(200)
                    self.send_header("Content-Type", "application/json")
                    self.end_headers()
                    self.wfile.write(json.dumps(asdict(result)).encode())
                else:
                    self.send_response(404)
                    self.end_headers()

            def do_GET(self):
                if self.path == "/health":
                    self.send_response(200)
                    self.send_header("Content-Type", "application/json")
                    self.end_headers()
                    self.wfile.write(json.dumps({
                        "status": "ok",
                        "version": "0.3",
                        "model": agent.model,
                        "tasks_completed": agent.memory.memory.tasks_completed,
                        "tasks_failed": agent.memory.memory.tasks_failed,
                        "self_improve": SELF_IMPROVE_AVAILABLE,
                    }).encode())
                elif self.path == "/memory":
                    self.send_response(200)
                    self.send_header("Content-Type", "application/json")
                    self.end_headers()
                    self.wfile.write(json.dumps(asdict(agent.memory.memory)).encode())
                elif self.path == "/self-status":
                    self.send_response(200)
                    self.send_header("Content-Type", "application/json")
                    self.end_headers()
                    status = _aeon_self.get_status() if _aeon_self else {"error": "AeonSelf not loaded"}
                    self.wfile.write(json.dumps(status).encode())
                else:
                    self.send_response(404)
                    self.end_headers()

        server = HTTPServer(("0.0.0.0", port), Handler)
        log.info(f"🌐 AeonMind API running on http://0.0.0.0:{port}")
        log.info(f"   POST /task       {{\"task\": \"your task here\"}}")
        log.info(f"   GET  /health     status check")
        log.info(f"   GET  /memory     agent memory dump")
        server.serve_forever()
    except ImportError:
        log.error("Server mode requires Python standard library only — should always work")

# ── CLI Entry Point ──────────────────────────────────────────

def main():
    parser = argparse.ArgumentParser(
        description="AeonMind — Autonomous Browser Agent",
        formatter_class=argparse.RawDescriptionHelpFormatter,
    )
    parser.add_argument("--task", type=str, help="Task string to execute")
    parser.add_argument("--task-file", type=str, help="JSON file with task list")
    parser.add_argument("--model", type=str, default=DEFAULT_MODEL,
                        help=f"Ollama model to use (default: {DEFAULT_MODEL})")
    parser.add_argument("--headless", action="store_true", default=True,
                        help="Run browser in headless mode")
    parser.add_argument("--visible", action="store_true",
                        help="Run browser with visible window")
    parser.add_argument("--server", action="store_true",
                        help="Run as REST API server on port 7878")
    parser.add_argument("--port", type=int, default=7878,
                        help="API server port (default: 7878)")
    parser.add_argument("--status", action="store_true",
                        help="Print agent memory status and exit")
    args = parser.parse_args()

    headless = not args.visible

    if args.status:
        m = MemoryManager()
        print(json.dumps(asdict(m.memory), indent=2))
        return

    agent = AeonMindAgent(model=args.model, headless=headless)

    if args.server:
        run_server(agent, args.port)
    elif args.task:
        result = asyncio.run(agent.run_task(args.task))
        print(f"\n{'='*60}")
        print(f"TASK:    {result.task}")
        print(f"STATUS:  {result.status}")
        print(f"STEPS:   {result.steps_taken}")
        print(f"TIME:    {result.duration_sec}s")
        print(f"MODEL:   {result.model}")
        print(f"RESULT:\n{result.result}")
        if result.reflections:
            print(f"\nREFLECTIONS:")
            for r in result.reflections:
                print(f"  • {r}")
        print(f"HIVE:    {'submitted' if result.hive_submitted else 'offline (ok)'}")
        print(f"{'='*60}")
    elif args.task_file:
        asyncio.run(agent.run_task_file(args.task_file))
    else:
        # Interactive mode
        print("╔══════════════════════════════════╗")
        print("║   AeonMind Interactive Mode       ║")
        print("╚══════════════════════════════════╝")
        print(f"Model: {agent.model} | Type 'exit' to quit\n")
        while True:
            try:
                task = input("Task > ").strip()
                if task.lower() in ("exit", "quit", "q"):
                    break
                if not task:
                    continue
                result = asyncio.run(agent.run_task(task))
                print(f"\n→ {result.result}\n")
            except KeyboardInterrupt:
                break
        print("\nAeonMind session ended.")

if __name__ == "__main__":
    main()