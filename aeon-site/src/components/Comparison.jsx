import React from 'react';

const rows = [
  { feature: 'Zero Google background pings',          aeon: true,  chrome: false, firefox: false, brave: false },
  { feature: 'Local GGUF AI + WebMCP pipe',           aeon: true,  chrome: false, firefox: false, brave: false },
  { feature: 'OmniHub ↔ LogicFlow workflow handoff',  aeon: true,  chrome: false, firefox: false, brave: false },
  { feature: 'Per-session fingerprint randomization', aeon: true,  chrome: false, firefox: false, brave: true },
  { feature: 'Opt-in crash diagnostics (zero IP disk)', aeon: true, chrome: false, firefox: false, brave: false },
  { feature: 'Ed25519 + SHA-256 signed updates',      aeon: true,  chrome: false, firefox: true,  brave: false },
  { feature: 'Built-in Rust ad/tracker blocker',      aeon: true,  chrome: false, firefox: false, brave: true },
  { feature: 'No forced Google account sync',         aeon: true,  chrome: false, firefox: true,  brave: true },
  { feature: 'Native Win32 DWM Mica + Split View',    aeon: true,  chrome: false, firefox: false, brave: false },
  { feature: 'Windows 10 (1607+) & Windows 11 x64',   aeon: true,  chrome: true,  firefox: true,  brave: true },
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
          We built Aeon because no mainstream browser met all of these engineering criteria.
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
