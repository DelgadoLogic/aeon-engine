import React from 'react';

export default function Features() {
  return (
    <section className="section" id="features">
      <div className="reveal">
        <div className="section-label">Capabilities</div>
        <h2 className="section-title">Built differently.<br />On purpose.</h2>
        <p className="section-sub">Every design decision was a deliberate rejection of ad-profiling browser bloat.</p>
      </div>

      <div className="features-grid">
        <div className="feature-card reveal">
          <div className="feature-icon">🧠</div>
          <div className="feature-title">AeonMind — Local GGUF AI</div>
          <div className="feature-desc">Run quantized Gemma 4 GGUF weights directly on your hardware for page summaries, structured extraction, and omnibox intelligence without cloud dependency.</div>
        </div>
        <div className="feature-card reveal">
          <div className="feature-icon">⚡</div>
          <div className="feature-title">WebMCP &amp; OmniHub Bridge</div>
          <div className="feature-desc">Built-in Model Context Protocol pipe lets local agents inspect semantic DOM trees and hand off downloads or sessions directly to LogicFlow and OmniHub.</div>
        </div>
        <div className="feature-card reveal">
          <div className="feature-icon">🔐</div>
          <div className="feature-title">Ed25519 + SHA-256 Updates</div>
          <div className="feature-desc">Every release manifest is verified with canonical sorted-JSON RFC 8032 Ed25519 signatures and SHA-256 binary integrity checks before execution.</div>
        </div>
        <div className="feature-card reveal">
          <div className="feature-icon">👁️</div>
          <div className="feature-title">Camoufox Stealth Shield</div>
          <div className="feature-desc">Per-session Canvas LSB noise, WebGL vendor cloaking, AudioContext protection, and header normalization thwart cross-site browser fingerprinting.</div>
        </div>
        <div className="feature-card reveal">
          <div className="feature-icon">🛡️</div>
          <div className="feature-title">DPAPI Sovereign Vault</div>
          <div className="feature-desc">History, bookmarks, and credentials are encrypted locally via Windows DPAPI + AES-256-GCM — no forced Google account sync, and crash diagnostics are strictly opt-in with zero IP persistence.</div>
        </div>
        <div className="feature-card reveal">
          <div className="feature-icon">🎨</div>
          <div className="feature-title">Obsidian DWM Shell</div>
          <div className="feature-desc">Native Win32 + DWM Mica/Acrylic compositor with PerMonitorV2 DPI scaling, Arc-style vertical workspaces, collapsible tab groups, and split-view panes.</div>
        </div>
      </div>
    </section>
  );
}
