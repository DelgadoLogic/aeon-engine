import React, { useState } from 'react';

export default function Waitlist() {
  const [email, setEmail] = useState('');
  const [status, setStatus] = useState('idle'); // idle | sending | success | error

  async function handleSubmit(e) {
    e.preventDefault();
    if (!email || !email.includes('@')) return;
    setStatus('sending');

    try {
      const API_URL = 'https://aeon-evolution-engine-343794371528.us-east1.run.app';
      const res = await fetch(`${API_URL}/waitlist`, {
        method: 'POST',
        headers: { 'Content-Type': 'application/json' },
        body: JSON.stringify({ email, tier: 'free' }),
      });

      if (!res.ok) throw new Error(`HTTP ${res.status}`);

      const data = await res.json();
      if (data.status === 'already_registered') {
        setStatus('success'); // Treat duplicate as success — don't embarrass the user
      } else {
        setStatus('success');
      }
      setEmail('');

      // Also store locally as offline backup
      const waitlist = JSON.parse(localStorage.getItem('aeon_waitlist') || '[]');
      waitlist.push({ email, timestamp: new Date().toISOString(), synced: true });
      localStorage.setItem('aeon_waitlist', JSON.stringify(waitlist));
    } catch {
      // Fallback: store locally if API is unreachable
      const waitlist = JSON.parse(localStorage.getItem('aeon_waitlist') || '[]');
      waitlist.push({ email, timestamp: new Date().toISOString(), synced: false });
      localStorage.setItem('aeon_waitlist', JSON.stringify(waitlist));
      setStatus('success'); // Show success anyway — we'll sync later
      setEmail('');
    }
  }

  return (
    <section className="section waitlist-section" id="waitlist">
      <div className="reveal" style={{ textAlign: 'center' }}>
        <div className="waitlist-glow" aria-hidden="true"></div>
        <div className="section-label">Early Access</div>
        <h2 className="section-title">
          Get Aeon first.
        </h2>
        <p className="section-sub" style={{ margin: '16px auto 0' }}>
          Join the waitlist and be first in line when we ship. No spam, no
          sharing your email — just a single notification when Aeon is ready.
        </p>

        {status === 'success' ? (
          <div className="waitlist-success">
            <span className="waitlist-success-icon">✓</span>
            <p>You're on the list. We'll notify you at launch.</p>
          </div>
        ) : (
          <form className="waitlist-form" onSubmit={handleSubmit}>
            <div className="waitlist-input-wrap">
              <input
                type="email"
                placeholder="you@example.com"
                value={email}
                onChange={(e) => setEmail(e.target.value)}
                className="waitlist-input"
                required
                id="waitlist-email"
              />
              <button
                type="submit"
                className="waitlist-btn"
                disabled={status === 'sending'}
                id="waitlist-submit-btn"
              >
                {status === 'sending' ? (
                  <span className="waitlist-spinner"></span>
                ) : (
                  'Join Waitlist →'
                )}
              </button>
            </div>
            {status === 'error' && (
              <p className="waitlist-error">Something went wrong. Please try again.</p>
            )}
          </form>
        )}

        <div className="waitlist-meta">
          <span>🔒 No spam, ever</span>
          <span>🖥️ Windows 10+ at launch</span>
          <span>📡 macOS & Linux following</span>
        </div>

        <p style={{ marginTop: '32px', fontSize: '13px', color: 'var(--muted)' }}>
          SHA-256 verification hash will be published on release day.
        </p>
      </div>
    </section>
  );
}
