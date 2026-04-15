import React from 'react';

const signals = [
  { icon: '🔐', text: 'Ed25519 Signed Updates' },
  { icon: '📡', text: 'Zero Telemetry' },
  { icon: '🧠', text: 'Local-Only AI' },
  { icon: '🕸️', text: 'P2P Distribution' },
  { icon: '🛡️', text: 'No Cloud Sync' },
  { icon: '🔓', text: 'Open Source Core' },
];

export default function TrustSignals() {
  return (
    <div className="trust-strip">
      {signals.map((s, i) => (
        <div className="trust-item" key={i}>
          <span className="trust-icon">{s.icon}</span>
          <span className="trust-text">{s.text}</span>
        </div>
      ))}
    </div>
  );
}
