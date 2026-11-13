import React from 'react';

export default function Footer() {
  return (
    <footer>
      <p style={{ marginBottom: '12px' }}>
        <a href="https://delgadologic.tech/legal/privacy" target="_blank" rel="noreferrer">Privacy Policy</a> &nbsp;·&nbsp;
        <a href="https://delgadologic.tech/legal/terms" target="_blank" rel="noreferrer">Terms of Service</a> &nbsp;·&nbsp;
        <a href="https://delgadologic.tech/legal/refunds" target="_blank" rel="noreferrer">60-Day Refund Policy</a> &nbsp;·&nbsp;
        <a href="https://github.com/DelgadoLogic/aeon-engine" target="_blank" rel="noreferrer">GitHub</a> &nbsp;·&nbsp;
        <a href="mailto:support@delgadologic.tech">Contact</a>
      </p>
      <p>
        Aeon Browser is engineered by Delgado Creative Enterprises LLC d/b/a DelgadoLogic.
        Built on Chromium — not affiliated with Google LLC.
      </p>
      <p style={{ marginTop: '8px', fontSize: '12px', color: 'var(--muted)' }}>
        © 2026 Delgado Creative Enterprises LLC d/b/a DelgadoLogic · 6218 Georgia Avenue NW, Ste 1-698, Washington, DC 20011, USA · Age 16+
      </p>
    </footer>
  );
}
