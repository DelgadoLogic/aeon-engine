import React from 'react';

export default function Pricing() {
  return (
    <section className="section" id="pro">
      <div className="reveal" style={{ textAlign: 'center' }}>
        <div className="section-label">Pricing</div>
        <h2 className="section-title">Simple. Honest.</h2>
        <p className="section-sub" style={{ margin: '0 auto' }}>
          The core sovereign browser is $0 forever with zero ad-network profiling. Pro unlocks unlimited cloud &amp; agent compute.
        </p>
      </div>

      <div className="pricing-cards">
        <div className="pricing-card reveal">
          <div className="pricing-tier">Free · Forever</div>
          <div className="pricing-price">$0</div>
          <div className="pricing-desc">No trial. No credit card. No expiry.</div>
          <ul className="pricing-features">
            <li>Full sovereign browser &amp; Camoufox stealth shield</li>
            <li>AeonMind &amp; WebMCP bridge (50 queries/day + unlimited local GGUF)</li>
            <li>Ed25519-signed updates + SHA-256 verification</li>
            <li>Per-session Canvas, WebGL &amp; Audio anti-fingerprinting</li>
            <li>Post-quantum TLS 1.3 + RFC 8484 DoH</li>
          </ul>
          <a className="btn-secondary" href="#waitlist" style={{ display: 'block', textAlign: 'center' }} id="free-waitlist-btn">
            Join Waitlist — Free
          </a>
        </div>
        <div className="pricing-card featured reveal">
          <div className="pricing-badge">Most popular</div>
          <div className="pricing-tier">Aeon Pro</div>
          <div className="pricing-price">$4.99 <span>/month</span></div>
          <div className="pricing-desc">Or $39/year — save 35% · 60-day refund guarantee</div>
          <ul className="pricing-features">
            <li>Everything in Aeon Free (up to 3 devices)</li>
            <li>Unlimited AeonMind cloud &amp; autonomous agent queries</li>
            <li>Priority security patch channel (48h early)</li>
            <li>Encrypted zero-knowledge settings &amp; vault sync</li>
            <li>AeonHive priority relay &amp; multi-hop routing</li>
            <li>Direct OmniHub ↔ LogicFlow workflow automation</li>
          </ul>
          <a className="btn-primary" href="#waitlist" style={{ display: 'block', textAlign: 'center' }} id="pro-waitlist-btn">
            Join Waitlist — Pro
          </a>
        </div>
      </div>
      <p style={{ textAlign: 'center', marginTop: '24px', fontSize: '0.88rem', color: 'var(--text-muted, #94a3b8)' }}>
        Deploying for a team or security lab? <strong>Aeon Enterprise</strong> ($15/seat/month) includes centralized fleet policy, Network Sentinel governance, and SLA support — <a href="mailto:billing@delgadologic.tech" style={{ color: '#c084fc' }}>contact billing@delgadologic.tech</a>.
      </p>
    </section>
  );
}
