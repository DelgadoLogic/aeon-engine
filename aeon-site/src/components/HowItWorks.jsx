import React from 'react';

export default function HowItWorks() {
  return (
    <section className="how-works" id="how-it-works">
      <div className="section">
        <div className="reveal" style={{ textAlign: 'center', maxWidth: '600px', margin: '0 auto' }}>
          <div className="section-label">Process</div>
          <h2 className="section-title">How Aeon stays<br />ahead of threats</h2>
        </div>
        <div className="flow">
          <div className="flow-step reveal">
            <div className="flow-line"></div>
            <div className="flow-num">1</div>
            <div className="flow-content">
              <h3>Research agent scans 24/7</h3>
              <p>Running on distributed compute, it continuously monitors CVE databases, security papers, and GitHub advisories for anything affecting Chromium, V8, or Blink.</p>
            </div>
          </div>
          <div className="flow-step reveal">
            <div className="flow-line"></div>
            <div className="flow-num">2</div>
            <div className="flow-content">
              <h3>AI generates the patch</h3>
              <p>AeonMind analyzes the vulnerability and writes a targeted C++ patch. For non-critical features, a 48-hour peer vote determines if the change ships.</p>
            </div>
          </div>
          <div className="flow-step reveal">
            <div className="flow-line"></div>
            <div className="flow-num">3</div>
            <div className="flow-content">
              <h3>Signed and distributed</h3>
              <p>The patch is signed with an Ed25519 master key. No unsigned binary can enter the network. Chunks are distributed peer-to-peer — no central server involved.</p>
            </div>
          </div>
          <div className="flow-step reveal">
            <div className="flow-num">4</div>
            <div className="flow-content">
              <h3>Silent install on your machine</h3>
              <p>The update waits until you step away. It installs, verifies, and restarts components atomically. You come back to a browser that's already been patched.</p>
            </div>
          </div>
        </div>
      </div>
    </section>
  );
}
