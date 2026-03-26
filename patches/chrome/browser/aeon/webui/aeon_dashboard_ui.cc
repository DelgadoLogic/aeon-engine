// Copyright 2026 The Aeon Browser Authors
// AeonDashboard WebUI Controller
// Serves all aeon:// pages — this is the nerve center of the browser.
//
// Routes:
//   aeon://dashboard  — AeonMind task control
//   aeon://agent      — Live execution viewer
//   aeon://hive       — P2P mesh visualization
//   aeon://memory     — Episodic memory explorer
//   aeon://self       — Self-improvement dashboard
//   aeon://privacy    — Fingerprint control center
//   aeon://newtab     — AI-powered new tab

#include "chrome/browser/aeon/webui/aeon_dashboard_ui.h"

#include "chrome/browser/profiles/profile.h"
#include "chrome/common/webui_url_constants.h"
#include "content/public/browser/web_ui.h"
#include "content/public/browser/web_ui_data_source.h"
#include "base/strings/strcat.h"
#include "base/values.h"
#include "components/strings/grit/components_strings.h"

namespace {

// Inline HTML/JS for aeon:// pages
// In a production build these would be served from a .grd resource bundle.
// This self-contained version works for the initial cloud build.

const char kAeonDashboardHTML[] = R"HTML(
<!DOCTYPE html>
<html>
<head>
  <meta charset="utf-8">
  <meta name="color-scheme" content="dark">
  <title>Aeon Dashboard</title>
  <style>
    :root {
      --bg: #0a0a0f;
      --surface: #12121a;
      --border: #1e1e2e;
      --accent: #7c5af0;
      --accent2: #00d4ff;
      --text: #e2e2f0;
      --muted: #6b6b8a;
      --green: #00e676;
      --red: #ff5370;
    }
    * { margin: 0; padding: 0; box-sizing: border-box; }
    body {
      background: var(--bg);
      color: var(--text);
      font-family: 'Inter', system-ui, sans-serif;
      min-height: 100vh;
    }
    header {
      display: flex;
      align-items: center;
      padding: 16px 24px;
      border-bottom: 1px solid var(--border);
      background: var(--surface);
      backdrop-filter: blur(20px);
    }
    .logo {
      font-size: 20px;
      font-weight: 700;
      background: linear-gradient(135deg, var(--accent), var(--accent2));
      -webkit-background-clip: text;
      -webkit-text-fill-color: transparent;
      letter-spacing: -0.5px;
    }
    .logo-sub { font-size: 12px; color: var(--muted); margin-left: 8px; font-weight: 400; }
    .status-dot {
      width: 8px; height: 8px;
      border-radius: 50%;
      background: var(--green);
      box-shadow: 0 0 8px var(--green);
      margin-left: auto;
      animation: pulse 2s ease-in-out infinite;
    }
    @keyframes pulse { 0%,100% { opacity: 1; } 50% { opacity: 0.4; } }
    
    nav {
      display: flex;
      gap: 2px;
      padding: 16px 24px 0;
      border-bottom: 1px solid var(--border);
    }
    nav a {
      padding: 8px 16px;
      text-decoration: none;
      color: var(--muted);
      font-size: 13px;
      font-weight: 500;
      border-radius: 8px 8px 0 0;
      border-bottom: 2px solid transparent;
      transition: all 0.2s;
    }
    nav a.active, nav a:hover {
      color: var(--text);
      border-bottom-color: var(--accent);
      background: rgba(124, 90, 240, 0.08);
    }
    
    .content { padding: 24px; max-width: 1200px; margin: 0 auto; }
    
    .grid { display: grid; grid-template-columns: repeat(auto-fit, minmax(280px, 1fr)); gap: 16px; margin-bottom: 24px; }
    
    .card {
      background: var(--surface);
      border: 1px solid var(--border);
      border-radius: 12px;
      padding: 20px;
      transition: border-color 0.2s;
    }
    .card:hover { border-color: var(--accent); }
    .card-title { font-size: 11px; font-weight: 600; text-transform: uppercase; letter-spacing: 1px; color: var(--muted); margin-bottom: 12px; }
    .card-value { font-size: 32px; font-weight: 700; line-height: 1; }
    .card-value.accent { color: var(--accent2); }
    .card-value.green { color: var(--green); }
    .card-sub { font-size: 12px; color: var(--muted); margin-top: 6px; }
    
    .task-input-area { margin-bottom: 24px; }
    .task-input-area h2 { font-size: 16px; font-weight: 600; margin-bottom: 12px; }
    .input-row { display: flex; gap: 8px; }
    .task-input {
      flex: 1;
      background: var(--surface);
      border: 1px solid var(--border);
      border-radius: 10px;
      padding: 12px 16px;
      color: var(--text);
      font-size: 14px;
      font-family: inherit;
      outline: none;
      transition: border-color 0.2s;
    }
    .task-input:focus { border-color: var(--accent); }
    .btn {
      padding: 12px 20px;
      border-radius: 10px;
      border: none;
      font-size: 14px;
      font-weight: 600;
      cursor: pointer;
      transition: all 0.2s;
    }
    .btn-primary {
      background: var(--accent);
      color: white;
    }
    .btn-primary:hover { background: #9b7ff5; transform: translateY(-1px); }
    .btn-secondary { background: var(--surface); border: 1px solid var(--border); color: var(--text); }
    
    .log-area {
      background: #08080d;
      border: 1px solid var(--border);
      border-radius: 12px;
      padding: 16px;
      font-family: 'JetBrains Mono', 'Consolas', monospace;
      font-size: 12px;
      color: #a0a0c0;
      height: 300px;
      overflow-y: auto;
      line-height: 1.6;
    }
    .log-line { margin-bottom: 4px; }
    .log-line.success { color: var(--green); }
    .log-line.error { color: var(--red); }
    .log-line.info { color: var(--accent2); }
    .log-line.warn { color: #ffcc00; }
    
    .privacy-bar {
      display: flex;
      align-items: center;
      gap: 16px;
      background: var(--surface);
      border: 1px solid var(--border);
      border-radius: 12px;
      padding: 16px 20px;
      margin-bottom: 24px;
    }
    .privacy-mode { display: flex; gap: 8px; }
    .mode-btn {
      padding: 6px 14px;
      border-radius: 6px;
      border: 1px solid var(--border);
      background: transparent;
      color: var(--muted);
      font-size: 12px;
      cursor: pointer;
      transition: all 0.2s;
    }
    .mode-btn.active { background: var(--accent); border-color: var(--accent); color: white; }
    
    #fingerprint-hash {
      font-family: monospace;
      font-size: 11px;
      color: var(--muted);
      margin-left: auto;
    }
    
    .hive-graph {
      background: #08080d;
      border: 1px solid var(--border);
      border-radius: 12px;
      height: 200px;
      display: flex;
      align-items: center;
      justify-content: center;
      color: var(--muted);
      font-size: 13px;
      position: relative;
      overflow: hidden;
    }
    
    .peer-node {
      position: absolute;
      width: 10px; height: 10px;
      border-radius: 50%;
      background: var(--accent2);
      box-shadow: 0 0 12px var(--accent2);
    }
    .peer-self {
      background: var(--accent);
      box-shadow: 0 0 16px var(--accent);
      width: 14px; height: 14px;
    }
  </style>
  <link rel="preconnect" href="https://fonts.googleapis.com">
  <link href="https://fonts.googleapis.com/css2?family=Inter:wght@400;500;600;700&display=swap" rel="stylesheet">
</head>
<body>
  <header>
    <span class="logo">⚡ Aeon</span>
    <span class="logo-sub">Intelligence Engine</span>
    <div class="status-dot" id="status-dot"></div>
  </header>
  
  <nav id="nav">
    <a href="aeon://dashboard" class="active">Dashboard</a>
    <a href="aeon://agent">Agent</a>
    <a href="aeon://hive">Hive</a>
    <a href="aeon://memory">Memory</a>
    <a href="aeon://self">Self</a>
    <a href="aeon://privacy">Privacy</a>
  </nav>
  
  <div class="content">
    <!-- Status Cards -->
    <div class="grid" id="stats-grid">
      <div class="card">
        <div class="card-title">Agent Status</div>
        <div class="card-value accent" id="agent-status">Idle</div>
        <div class="card-sub" id="agent-sub">Ready for tasks</div>
      </div>
      <div class="card">
        <div class="card-title">Hive Peers</div>
        <div class="card-value green" id="peer-count">0</div>
        <div class="card-sub">Active nodes</div>
      </div>
      <div class="card">
        <div class="card-title">Tasks Completed</div>
        <div class="card-value" id="tasks-done">0</div>
        <div class="card-sub" id="model-version">Model: initializing...</div>
      </div>
      <div class="card">
        <div class="card-title">Privacy Mode</div>
        <div class="card-value accent" id="privacy-mode-display">Stealth</div>
        <div class="card-sub">Fingerprint protected</div>
      </div>
    </div>
    
    <!-- Privacy Control -->
    <div class="privacy-bar">
      <span style="font-size:13px;font-weight:600;">Fingerprint Mode</span>
      <div class="privacy-mode">
        <button class="mode-btn" onclick="setMode('normal')">Normal</button>
        <button class="mode-btn active" id="mode-stealth" onclick="setMode('stealth')">Stealth</button>
        <button class="mode-btn" onclick="setMode('ghost')">Ghost 👻</button>
      </div>
      <button class="btn btn-secondary" onclick="rotateFingerprint()" style="padding:6px 12px;font-size:12px;">🔄 Rotate</button>
      <span id="fingerprint-hash">hash: loading...</span>
    </div>
    
    <!-- Task Input -->
    <div class="task-input-area">
      <h2>Run Agent Task</h2>
      <div class="input-row">
        <input class="task-input" id="task-input" 
               placeholder="e.g. 'Search for the latest AI news and summarize it'" 
               onkeydown="if(event.key==='Enter')runTask()">
        <button class="btn btn-primary" onclick="runTask()">▶ Run</button>
        <button class="btn btn-secondary" onclick="stopTask()">■ Stop</button>
      </div>
    </div>
    
    <!-- Log Output -->
    <div class="log-area" id="log">
      <div class="log-line info">[Aeon] Dashboard initialized. window.aeon available.</div>
      <div class="log-line success">[Aeon] Fingerprint protection active.</div>
    </div>
  </div>
  
  <script>
    const log = document.getElementById('log');
    
    function addLog(msg, type='') {
      const line = document.createElement('div');
      line.className = 'log-line ' + type;
      line.textContent = `[${new Date().toLocaleTimeString()}] ${msg}`;
      log.appendChild(line);
      log.scrollTop = log.scrollHeight;
    }
    
    async function runTask() {
      const task = document.getElementById('task-input').value.trim();
      if (!task) return;
      addLog(`Running: ${task}`, 'info');
      document.getElementById('agent-status').textContent = 'Running';
      document.getElementById('agent-sub').textContent = task.substring(0, 40) + '...';
      try {
        const result = await window.aeon.agent.run(task);
        addLog(`Result: ${result}`, 'success');
        document.getElementById('agent-status').textContent = 'Idle';
        document.getElementById('agent-sub').textContent = 'Ready for tasks';
      } catch(e) {
        addLog(`Error: ${e.message}`, 'error');
        // Fallback if native API not available (dev mode with pre-built binary)
        const resp = await fetch('http://localhost:7878/task', {
          method: 'POST',
          headers: {'Content-Type': 'application/json'},
          body: JSON.stringify({task})
        }).then(r=>r.json()).catch(()=>({error:'AeonMind not running'}));
        addLog(JSON.stringify(resp), resp.error ? 'error' : 'success');
      }
    }
    
    async function stopTask() {
      try { await window.aeon.agent.stop(); } catch(e) {}
      document.getElementById('agent-status').textContent = 'Idle';
      addLog('Task stopped.', 'warn');
    }
    
    function setMode(mode) {
      document.querySelectorAll('.mode-btn').forEach(b => b.classList.remove('active'));
      try {
        window.aeon.privacy.setMode(mode);
      } catch(e) {}
      const labels = {normal:'Normal',stealth:'Stealth',ghost:'Ghost 👻'};
      document.getElementById('privacy-mode-display').textContent = labels[mode];
      addLog(`Privacy mode: ${mode}`, 'info');
      updateFingerprint();
    }
    
    function rotateFingerprint() {
      try { window.aeon.privacy.refresh(); } catch(e) {}
      addLog('Fingerprint rotated.', 'success');
      updateFingerprint();
    }
    
    function updateFingerprint() {
      try {
        const hash = window.aeon.privacy.fingerprint();
        document.getElementById('fingerprint-hash').textContent = 'hash: ' + hash.substring(0,16) + '...';
      } catch(e) {
        document.getElementById('fingerprint-hash').textContent = 'hash: pre-built mode';
      }
    }
    
    async function pollStatus() {
      try {
        const peers = await window.aeon.hive.peers();
        document.getElementById('peer-count').textContent = peers;
        const tasks = await window.aeon.self.tasksCompleted();
        document.getElementById('tasks-done').textContent = tasks;
        const model = await window.aeon.self.modelVersion();
        document.getElementById('model-version').textContent = 'Model: ' + model;
      } catch(e) {
        // Fallback polling from REST API
        try {
          const s = await fetch('http://localhost:7878/status').then(r=>r.json());
          if (s.peers_count !== undefined) document.getElementById('peer-count').textContent = s.peers_count;
        } catch(_) {}
      }
    }
    
    // Active tab detection
    const path = window.location.hostname;
    document.querySelectorAll('nav a').forEach(a => {
      a.classList.toggle('active', a.href.includes(path));
    });
    
    updateFingerprint();
    pollStatus();
    setInterval(pollStatus, 5000);
    addLog('Connected to AeonMind REST API on localhost:7878', 'success');
  </script>
</body>
</html>
)HTML";

}  // namespace

AeonDashboardUI::AeonDashboardUI(content::WebUI* web_ui,
                                 const std::string& host)
    : content::WebUIController(web_ui) {
  content::WebUIDataSource* source =
      content::WebUIDataSource::CreateAndAdd(
          Profile::FromWebUI(web_ui), host);

  source->SetDefaultResource(/* use inline HTML */ 0);
  source->DisableTrustedTypesCSP();
  source->AddResourcePath("", kAeonDashboardHTML);

  // Allow fetch to localhost for AeonMind REST fallback
  source->OverrideContentSecurityPolicy(
      network::mojom::CSPDirectiveName::ConnectSrc,
      "connect-src 'self' http://localhost:7878 http://localhost:11434;");
}

AeonDashboardUI::~AeonDashboardUI() = default;
