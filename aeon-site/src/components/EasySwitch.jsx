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
              <strong>1-Click Import</strong>
              <p>Bring your bookmarks, passwords, and history instantly from Chrome, Firefox, or Edge.</p>
            </div>
          </div>
          <div className="switch-item">
            <div className="switch-icon">🧩</div>
            <div className="switch-text">
              <strong>Extension Compatibility</strong>
              <p>Full, native support for everything on the Chrome Web Store. Your adblocker still works.</p>
            </div>
          </div>
          <div className="switch-item">
            <div className="switch-icon">💻</div>
            <div className="switch-text">
              <strong>Universal Hardware</strong>
              <p>Unprecedented support. Runs perfectly on Windows 3.1 all the way through Windows 11.</p>
            </div>
          </div>
        </div>
      </div>
    </section>
  );
}
