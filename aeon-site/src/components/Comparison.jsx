import React from 'react';

const rows = [
  { feature: 'Zero telemetry / call-home',       aeon: true,  chrome: false, firefox: false, brave: false },
  { feature: 'Local-only AI (no cloud API)',      aeon: true,  chrome: false, firefox: false, brave: false },
  { feature: 'P2P update distribution',           aeon: true,  chrome: false, firefox: false, brave: false },
  { feature: 'Per-session fingerprint randomization', aeon: true, chrome: false, firefox: false, brave: true },
  { feature: 'Autonomous CVE patching',           aeon: true,  chrome: false, firefox: false, brave: false },
  { feature: 'Ed25519 signed updates',            aeon: true,  chrome: false, firefox: true,  brave: false },
  { feature: 'Built-in ad/tracker blocker',       aeon: true,  chrome: false, firefox: false, brave: true },
  { feature: 'No Google services dependency',     aeon: true,  chrome: false, firefox: true,  brave: false },
  { feature: 'Runs on legacy OS (XP, Vista, 7)',  aeon: true,  chrome: false, firefox: false, brave: false },
  { feature: 'Open source core',                  aeon: true,  chrome: true,  firefox: true,  brave: true },
];

function Check({ val }) {
  if (val) return <span className="cmp-check cmp-yes">✓</span>;
  return <span className="cmp-check cmp-no">✕</span>;
}

export default function Comparison() {
  return (
    <section className="section comparison-section" id="comparison">
      <div className="reveal">
        <div className="section-label" style={{ textAlign: 'center' }}>Comparison</div>
        <h2 className="section-title" style={{ textAlign: 'center' }}>
          How Aeon stacks up
        </h2>
        <p className="section-sub" style={{ textAlign: 'center', margin: '16px auto 0' }}>
          We built Aeon because no browser met all of these criteria.
        </p>
      </div>

      <div className="cmp-table-wrap reveal">
        <table className="cmp-table">
          <thead>
            <tr>
              <th>Capability</th>
              <th><span className="cmp-th-aeon">⚡ Aeon</span></th>
              <th>Chrome</th>
              <th>Firefox</th>
              <th>Brave</th>
            </tr>
          </thead>
          <tbody>
            {rows.map((r, i) => (
              <tr key={i}>
                <td>{r.feature}</td>
                <td><Check val={r.aeon} /></td>
                <td><Check val={r.chrome} /></td>
                <td><Check val={r.firefox} /></td>
                <td><Check val={r.brave} /></td>
              </tr>
            ))}
          </tbody>
        </table>
      </div>
    </section>
  );
}
