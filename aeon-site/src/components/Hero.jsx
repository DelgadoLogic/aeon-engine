import React from 'react';
import TerminalDemo from './TerminalDemo';

export default function Hero() {
  return (
    <section className="hero">
      {/* Premium background effects */}
      <div className="hero-bg"></div>
      <div className="hero-grid"></div>
      <div className="hero-orb hero-orb--1" aria-hidden="true"></div>
      <div className="hero-orb hero-orb--2" aria-hidden="true"></div>

      <div className="hero-content">
        {/* Status badge */}
        <div className="hero-badge" style={{ animationDelay: '0.2s' }}>
          <span className="pulse"></span>
          Network online · 0 central servers
        </div>

        {/* Main headline */}
        <h1 className="hero-h1-anim">
          The browser<br />
          <span className="gradient">no one controls.</span>
        </h1>

        {/* Tagline */}
        <p className="hero-sub">
          Aeon finds its own vulnerabilities, patches them, and distributes
          updates through a peer-to-peer network — with zero telemetry,
          zero fingerprinting, and zero trust in anyone but you.
        </p>

        {/* CTA buttons */}
        <div className="hero-actions">
          <a className="btn-primary" href="#waitlist" id="hero-download-btn">
            Join the Waitlist
          </a>
          <a className="btn-secondary" href="#how-it-works">
            See how it works →
          </a>
        </div>

        {/* Sub-note with origin badge */}
        <p className="hero-note">Free · No account required · Open source core</p>

        {/* Feature pills */}
        <div className="hero-pills">
          {['Local AI', 'P2P Updates', 'Zero Telemetry', 'Ed25519 Signed', '8 OS Tiers'].map((pill, i) => (
            <span className="hero-pill" key={i} style={{ animationDelay: `${0.6 + i * 0.08}s` }}>
              {pill}
            </span>
          ))}
        </div>

        {/* Live terminal demo */}
        <TerminalDemo />
      </div>
    </section>
  );
}
