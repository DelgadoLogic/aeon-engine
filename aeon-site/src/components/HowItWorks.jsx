import React from 'react';

export default function HowItWorks() {
  return (
    <section className="how-works" id="how-it-works">
      <div className="section">
        <div className="reveal" style={{ textAlign: 'center', maxWidth: '620px', margin: '0 auto' }}>
          <div className="section-label">Architecture</div>
          <h2 className="section-title">How Aeon protects<br />every session</h2>
        </div>
        <div className="flow">
          <div className="flow-step reveal">
            <div className="flow-line"></div>
            <div className="flow-num">1</div>
            <div className="flow-content">
              <h3>Camoufox stealth + Rust shield layer</h3>
              <p>Every session applies deterministic Canvas LSB noise, WebGL vendor cloaking, AudioContext protection, and Rust-powered tracker filtering before network requests leave the tab.</p>
            </div>
          </div>
          <div className="flow-step reveal">
            <div className="flow-line"></div>
            <div className="flow-num">2</div>
            <div className="flow-content">
              <h3>Local GGUF inference &amp; WebMCP</h3>
              <p>AeonMind connects to shared quantized Gemma 4 GGUF weights and exposes an authenticated local named-pipe MCP bridge for structured page reasoning without cloud profiling.</p>
            </div>
          </div>
          <div className="flow-step reveal">
            <div className="flow-line"></div>
            <div className="flow-num">3</div>
            <div className="flow-content">
              <h3>Canonical Ed25519 + SHA-256 verification</h3>
              <p>Updates fetched from <code>api.delgadologic.tech/v1/update/aeon/stable</code> are verified against pinned RFC 8032 Ed25519 keys and SHA-256 hashes before staging.</p>
            </div>
          </div>
          <div className="flow-step reveal">
            <div className="flow-num">4</div>
            <div className="flow-content">
              <h3>Direct OmniHub ↔ LogicFlow handoff</h3>
              <p>Send downloads, clipped pages, or Playwright <code>auth.json</code> sessions straight into LogicFlow HyperStore, Sandbox, or Lazarus workflows over local IPC.</p>
            </div>
          </div>
        </div>
      </div>
    </section>
  );
}
