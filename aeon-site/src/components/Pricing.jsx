import React from 'react';

export default function Pricing() {
  return (
    <section className="section" id="pro">
      <div className="reveal" style={{ textAlign: 'center' }}>
        <div className="section-label">Pricing</div>
        <h2 className="section-title">Simple. Honest.</h2>
        <p className="section-sub" style={{ margin: '0 auto' }}>
          The free tier is genuinely good. Pro is for people who want more out of AeonMind.
        </p>
      </div>

      <div className="pricing-cards">
        <div className="pricing-card reveal">
          <div className="pricing-tier">Free · Always</div>
          <div className="pricing-price">$0</div>
          <div className="pricing-desc">No trial. No credit card. No expiry.</div>
          <ul className="pricing-features">
            <li>Full browser with all privacy features</li>
            <li>AeonMind (50 queries/day)</li>
            <li>P2P updates + auto-patching</li>
            <li>Fingerprint randomization</li>
            <li>AeonSearch (AI-enhanced)</li>
          </ul>
          <a className="btn-secondary" href="#waitlist" style={{ display: 'block', textAlign: 'center' }} id="free-waitlist-btn">
            Join Waitlist — Free
          </a>
        </div>
        <div className="pricing-card featured reveal">
          <div className="pricing-badge">Most popular</div>
          <div className="pricing-tier">Pro</div>
          <div className="pricing-price">$4 <span>/month</span></div>
          <div className="pricing-desc">Or $39/year — save 20%</div>
          <ul className="pricing-features">
            <li>Everything in Free</li>
            <li>Unlimited AeonMind queries</li>
            <li>Priority patch channel (48h early)</li>
            <li>Premium LoRA model</li>
            <li>Encrypted cloud settings sync</li>
            <li>Hive contribution rewards</li>
          </ul>
          <a className="btn-primary" href="#waitlist" style={{ display: 'block', textAlign: 'center' }} id="pro-waitlist-btn">
            Join Waitlist — Pro
          </a>
        </div>
      </div>
    </section>
  );
}
