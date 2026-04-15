import React from 'react';

export default function Footer() {
  return (
    <footer>
      <p style={{ marginBottom: '12px' }}>
        <a href="/privacy">Privacy Policy</a> &nbsp;·&nbsp;
        <a href="/terms">Terms</a> &nbsp;·&nbsp;
        <a href="https://github.com/aeonbrowser/aeon">GitHub</a> &nbsp;·&nbsp;
        <a href="mailto:hello@browseaeon.com">Contact</a>
      </p>
      <p>
        Aeon Browser is an independent fork of Chromium.
        Not affiliated with Google. BSD Licensed.
      </p>
      <p style={{ marginTop: '8px', fontSize: '12px' }}>
        © 2026 Aeon Browser Project
      </p>
    </footer>
  );
}
