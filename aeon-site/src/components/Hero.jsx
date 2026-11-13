import React from 'react';
import TerminalDemo from './TerminalDemo';
import BrowserMockupDemo from './BrowserMockupDemo';

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
          Developer Preview v0.22 · Beta Waitlist Open
        </div>

        {/* Main headline */}
        <h1 className="hero-h1-anim">
          The browser<br />
          <span className="gradient">no one controls.</span>
        </h1>

        {/* Tagline */}
        <p className="hero-sub">
          Aeon combines Brave-grade privacy shields and Obsidian workspace ergonomics with
          an autonomous local WebMCP AI bridge — zero Google background pings, Camoufox
          stealth anti-fingerprinting, and Ed25519-signed updates.
        </p>

        {/* CTA buttons */}
        <div className="hero-actions">
          <a className="btn-primary" href="#waitlist" id="hero-download-btn">
            Join Early Access
          </a>
          <a className="btn-secondary" href="#pro">
            View Editions →
          </a>
        </div>

        {/* Sub-note unified with FACTS.md & Pricing.jsx */}
        <p className="hero-note">Core Browser $0 Forever · Optional Aeon Pro ($4.99/mo or $39/yr) · 60-Day Refund Guarantee</p>

        {/* Feature pills */}
        <div className="hero-pills">
          {['Camoufox Stealth Shield', 'Obsidian Workspaces', 'Local WebMCP AI', 'OmniHub Bridge', 'Zero Google Pings', 'Ed25519 Signed'].map((pill, i) => (
            <span className="hero-pill" key={i} style={{ animationDelay: `${0.6 + i * 0.08}s` }}>
              {pill}
            </span>
          ))}
        </div>

        {/* Live Interactive Browser Mockup */}
        <BrowserMockupDemo />

        {/* Live terminal demo */}
        <TerminalDemo />
      </div>
    </section>
  );
}
