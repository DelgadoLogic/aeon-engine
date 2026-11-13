import React from 'react';

export default function EasySwitch() {
  return (
    <section className="switch-banner">
      <div className="switch-content reveal">
        <h2 className="switch-title">Switch in 60 seconds.</h2>
        <div className="switch-grid">
          <div className="switch-item">
            <div className="switch-icon">⚡</div>
            <div className="switch-text">
              <strong>1-Click Browser Import</strong>
              <p>Bring your bookmarks, history, and credentials locally from Chrome, Brave, Firefox, or Edge.</p>
            </div>
          </div>
          <div className="switch-item">
            <div className="switch-icon">🧩</div>
            <div className="switch-text">
              <strong>WebMCP + Extension Ready</strong>
              <p>Chromium extension compatibility paired with a native Model Context Protocol bridge for local AI agents.</p>
            </div>
          </div>
          <div className="switch-item">
            <div className="switch-icon">💻</div>
            <div className="switch-text">
              <strong>Windows 10 &amp; 11 Native x64</strong>
              <p>Engineered for Windows 10 (1607+) and Windows 11 with DWM Mica composition, PerMonitorV2 DPI, and OmniHub handoff.</p>
            </div>
          </div>
        </div>
      </div>
    </section>
  );
}
