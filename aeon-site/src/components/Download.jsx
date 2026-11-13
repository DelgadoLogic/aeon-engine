import React, { useState, useEffect } from 'react';

export default function Waitlist() {
  const [email, setEmail] = useState('');
  const [consent, setConsent] = useState(false);
  const [website, setWebsite] = useState('');
  const [status, setStatus] = useState('idle'); // idle | sending | success | queued | error
  const [message, setMessage] = useState('');

  useEffect(() => {
    async function syncQueue() {
      try {
        const queueStr = localStorage.getItem('delgadologic_waitlist_queue');
        if (!queueStr) return;
        const queue = JSON.parse(queueStr);
        if (!Array.isArray(queue) || queue.length === 0) return;

        const pending = [...queue];
        const remaining = [];

        for (const item of pending) {
          try {
            const r = await fetch('https://api.delgadologic.tech/waitlist', {
              method: 'POST',
              headers: { 'Content-Type': 'application/json', 'Accept': 'application/json' },
              body: JSON.stringify(item),
            });
            if (!r.ok && r.status !== 400) {
              remaining.push(item);
            }
          } catch {
            remaining.push(item);
          }
        }

        if (remaining.length > 0) {
          localStorage.setItem('delgadologic_waitlist_queue', JSON.stringify(remaining));
        } else {
          localStorage.removeItem('delgadologic_waitlist_queue');
        }
      } catch {
        // Ignore storage errors in restricted environments
      }
    }

    window.addEventListener('online', syncQueue);
    syncQueue();
    return () => window.removeEventListener('online', syncQueue);
  }, []);

  async function handleSubmit(e) {
    e.preventDefault();
    if (website) return;
    const cleanEmail = (email || '').replace(/[\s\uFEFF\xA0\u200B\u200C\u200D]+/g, '').trim().toLowerCase();
    if (!cleanEmail || !cleanEmail.includes('@') || !cleanEmail.includes('.')) {
      setStatus('error');
      setMessage('Please enter a valid email address.');
      return;
    }
    if (!consent) {
      setStatus('error');
      setMessage('Please check the box to confirm you want a launch notification email.');
      return;
    }
    setStatus('sending');

    const payload = { email: cleanEmail, product: 'aeon', tier: 'free', consent: true, source: 'aeon-site' };
    const endpoints = [
      'https://delgadologic.tech/waitlist',
      'https://api.delgadologic.tech/waitlist',
    ];

    let responseData = null;
    let res = null;
    let lastErr = null;

    for (const endpoint of endpoints) {
      try {
        const controller = new AbortController();
        const timer = setTimeout(() => controller.abort(), 15000);
        const r = await fetch(endpoint, {
          method: 'POST',
          headers: {
            'Content-Type': 'application/json',
            'Accept': 'application/json'
          },
          body: JSON.stringify(payload),
          signal: controller.signal
        });
        clearTimeout(timer);

        const contentType = r.headers.get('content-type') || '';
        if (contentType.includes('application/json')) {
          try {
            responseData = await r.json();
            res = r;
            if (r.ok || responseData.success || responseData.duplicate) {
              break;
            }
          } catch (jsonErr) {
            lastErr = jsonErr;
          }
        } else if (r.ok) {
          res = r;
          responseData = { success: true };
          break;
        }
      } catch (err) {
        lastErr = err;
      }
    }

    if (res && responseData && (res.ok || responseData.success || responseData.duplicate)) {
      setStatus('success');
      setMessage(responseData.message || "You're on the list. We'll notify you when Aeon Beta opens.");
      setEmail('');
      return;
    }

    if (responseData && responseData.error) {
      setStatus('error');
      setMessage(responseData.error);
      return;
    }

    try {
      const queueStr = localStorage.getItem('delgadologic_waitlist_queue');
      const queue = queueStr ? JSON.parse(queueStr) : [];
      queue.push({ ...payload, timestamp: new Date().toISOString() });
      localStorage.setItem('delgadologic_waitlist_queue', JSON.stringify(queue));
      setStatus('queued');
      setMessage("You're queued for early access! We've saved your spot and will sync automatically.");
      setEmail('');
      return;
    } catch {
      // Fall through to connection error
    }

    setStatus('error');
    setMessage(lastErr?.message && !lastErr.message.includes('object') ? lastErr.message : 'Connection error — please check your network and try again.');
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
          Join the early-access list and be first in line when Aeon Beta ships. Core browser is $0 forever; optional Aeon Pro ($4.99/mo or $39/yr) carries a 60-day money-back guarantee.
        </p>

        {status === 'success' || status === 'queued' ? (
          <div className="waitlist-success">
            <span className="waitlist-success-icon">{status === 'queued' ? '⏳' : '✓'}</span>
            <p>{message || "You're on the list. We'll notify you at launch."}</p>
          </div>
        ) : (
          <form noValidate className="waitlist-form" onSubmit={handleSubmit}>
            <div className="waitlist-input-wrap">
              <input
                type="email"
                name="email"
                id="waitlist-email"
                inputMode="email"
                autoCapitalize="none"
                autoCorrect="off"
                spellCheck="false"
                placeholder="you@example.com"
                aria-label="Email address"
                value={email}
                onChange={(e) => setEmail(e.target.value.replace(/[\s\uFEFF\xA0\u200B\u200C\u200D]+/g, ''))}
                className="waitlist-input"
                required
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
            <div style={{ position: 'absolute', left: '-9999px', opacity: 0, pointerEvents: 'none' }} aria-hidden="true">
              <label htmlFor="waitlist-hp-website">Website</label>
              <input
                id="waitlist-hp-website"
                name="website"
                type="text"
                tabIndex={-1}
                autoComplete="off"
                value={website}
                onChange={(e) => setWebsite(e.target.value)}
              />
            </div>
            <label style={{ display: 'inline-flex', alignItems: 'flex-start', gap: '8px', marginTop: '14px', fontSize: '12px', color: 'var(--muted)', maxWidth: '460px', textAlign: 'left', cursor: 'pointer' }}>
              <input
                type="checkbox"
                name="consent"
                id="waitlist-consent"
                checked={consent}
                onChange={(e) => setConsent(e.target.checked)}
                style={{ marginTop: '3px' }}
              />
              <span>
                Email me once when Aeon Beta opens. I can unsubscribe any time. See the{' '}
                <a href="https://delgadologic.tech/legal/privacy" target="_blank" rel="noreferrer" style={{ color: 'var(--accent)' }}>Privacy Policy</a>.
              </span>
            </label>
            {status === 'error' && (
              <p className="waitlist-error">{message || 'Something went wrong. Please try again.'}</p>
            )}
          </form>
        )}

        <div className="waitlist-meta">
          <span>🔒 Unsubscribe any time</span>
          <span>🖥️ Windows 10 (1607+) &amp; 11 x64</span>
          <span>🛡️ 60-day Pro refund guarantee</span>
        </div>

        <p style={{ marginTop: '24px', fontSize: '12px', color: 'var(--muted)' }}>
          Ed25519 + SHA-256 verification manifests published at <code>api.delgadologic.tech</code>.
        </p>
      </div>
    </section>
  );
}
