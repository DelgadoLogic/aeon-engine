import React from 'react';

export default function Navigation() {
  return (
    <nav>
      <a className="nav-logo" href="/">
        <img src="/aeon-logo.svg" alt="Aeon" style={{ height: '28px', width: '28px' }} />
        Aeon Browser
      </a>
      <div className="nav-links">
        <a href="#features">Features</a>
        <a href="#how-it-works">How it works</a>
        <a href="#comparison">Compare</a>
        <a href="#pro">Pro</a>
        <a href="https://github.com/aeonbrowser/aeon" target="_blank" rel="noreferrer">GitHub</a>
      </div>
      <a className="nav-cta" href="#waitlist" id="nav-waitlist-btn">Join Waitlist</a>
    </nav>
  );
}
