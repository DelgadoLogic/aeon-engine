import React from 'react';

const signals = [
  { icon: '🔐', text: 'Ed25519 Signed Updates' },
  { icon: '📡', text: 'Zero Google Pings' },
  { icon: '🧠', text: 'Local GGUF AI' },
  { icon: '🔗', text: 'OmniHub Handoff' },
  { icon: '🛡️', text: 'Zero Ad Profiling' },
  { icon: '🔒', text: 'DPAPI Local Vault' },
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
