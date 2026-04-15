import React from 'react';

export default function Features() {
  return (
    <section className="section" id="features">
      <div className="reveal">
        <div className="section-label">Capabilities</div>
        <h2 className="section-title">Built differently.<br />On purpose.</h2>
        <p className="section-sub">Every design decision was a deliberate rejection of how browsers are normally built.</p>
      </div>

      <div className="features-grid">
        <div className="feature-card reveal">
          <div className="feature-icon">🧠</div>
          <div className="feature-title">AeonMind — Local AI</div>
          <div className="feature-desc">A full LLM runs on your device. It answers questions, summarizes pages, and powers the omnibox — without any request ever leaving your machine.</div>
        </div>
        <div className="feature-card reveal">
          <div className="feature-icon">🔬</div>
          <div className="feature-title">Autonomous CVE Research</div>
          <div className="feature-desc">A research agent scans 50+ security databases every 6 hours. When it finds something critical, Aeon patches itself before most browsers know the CVE exists.</div>
        </div>
        <div className="feature-card reveal">
          <div className="feature-icon">🕸️</div>
          <div className="feature-title">P2P Update Distribution</div>
          <div className="feature-desc">Updates are signed, chunked, and distributed peer-to-peer. There's no update server to hack, seize, or take down. The network is the server.</div>
        </div>
        <div className="feature-card reveal">
          <div className="feature-icon">👁️</div>
          <div className="feature-title">Fingerprint Randomization</div>
          <div className="feature-desc">Every session presents a different browser fingerprint. Navigator, Canvas, WebGL, audio — all randomized. You look like a different person every time.</div>
        </div>
        <div className="feature-card reveal">
          <div className="feature-icon">🛡️</div>
          <div className="feature-title">Total Data Ownership</div>
          <div className="feature-desc">Your history, bookmarks, and passwords never leave your device. There is no cloud sync, no tracking accounts, and zero telemetry collection.</div>
        </div>
        <div className="feature-card reveal">
          <div className="feature-icon">🔇</div>
          <div className="feature-title">Absolute Silence</div>
          <div className="feature-desc">All background operations — research, compilation, patching, seeding — pause when you're active. You will never notice the network doing its work.</div>
        </div>
      </div>
    </section>
  );
}
