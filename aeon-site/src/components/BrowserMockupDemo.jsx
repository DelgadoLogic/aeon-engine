import React, { useState } from 'react';

export default function BrowserMockupDemo() {
  const [layoutMode, setLayoutMode] = useState('arc'); // 'arc' | 'chrome'
  const [activeSpace, setActiveSpace] = useState('sovereign'); // 'sovereign' | 'privacy' | 'work'
  const [activeTab, setActiveTab] = useState(0);
  const [shieldActive, setShieldActive] = useState(true);
  const [commandBarOpen, setCommandBarOpen] = useState(false);
  const [sidebarCollapsed, setSidebarCollapsed] = useState(false);

  const spaces = [
    { id: 'sovereign', name: '⚡ Sovereign AI', color: '#6c63ff', badge: 'Local MCP' },
    { id: 'privacy', name: '🛡️ Privacy Guard', color: '#ff5500', badge: 'Brave Shield' },
    { id: 'work', name: '💼 DelgadoLogic Work', color: '#3b82f6', badge: 'Dev Tools' }
  ];

  const tabs = [
    {
      id: 0,
      space: 'sovereign',
      title: '⚡ Aeon Sovereign AI',
      url: 'aeon://sovereign-agent',
      shieldCount: '1,428',
      content: {
        heading: 'Autonomous Web Intelligence Agent',
        status: 'AI Agent Active: Executing deep research sweep',
        badge: 'Local MCP Pipeline',
        logs: [
          'Connecting to local AeonWebMCP pipe...',
          'Parsing DOM snapshot with Rust tree serializer...',
          'Bypass tracker injected — 0 telemetry data leaked',
          'Autonomous task complete: 100% verified clean'
        ]
      }
    },
    {
      id: 1,
      space: 'privacy',
      title: '🛡️ Brave Shield Guard',
      url: 'aeon://shield-statistics',
      shieldCount: '2,891',
      content: {
        heading: 'Sovereign Privacy Shield Matrix',
        status: 'Brave-Grade Shield: 100% Protection Level',
        badge: 'Rust AdBlock Engine',
        logs: [
          'Trackers blocked today: 2,891',
          'Bandwidth saved: 418 MB',
          'Fingerprinting attempts thwarted: 94',
          'HTTPS-Everywhere enforced: Active'
        ]
      }
    },
    {
      id: 2,
      space: 'work',
      title: '🌐 Sovereign P2P Network',
      url: 'aeon://p2p-mesh',
      shieldCount: '847',
      content: {
        heading: 'Decentralized P2P Patch Network',
        status: 'Mesh Active: Connected to 847 peer nodes',
        badge: 'Ed25519 Signed',
        logs: [
          'Verified manifest signature: Ed25519 PASS',
          'Delta package received via encrypted webRTC mesh',
          'Zero central server reliance — zero kill switches'
        ]
      }
    }
  ];

  const currentTab = tabs[activeTab];
  const activeSpaceData = spaces.find(s => s.id === activeSpace);

  return (
    <div className="browser-mockup-wrapper">
      {/* Layout Mode Selector Bar */}
      <div className="layout-mode-selector">
        <span className="mode-label">UI Design Layout:</span>
        <button
          className={`mode-btn ${layoutMode === 'arc' ? 'active' : ''}`}
          onClick={() => setLayoutMode('arc')}
        >
          🎨 Arc Vertical Sidebar + Spaces
        </button>
        <button
          className={`mode-btn ${layoutMode === 'chrome' ? 'active' : ''}`}
          onClick={() => setLayoutMode('chrome')}
        >
          🌐 Brave + Chrome Horizontal
        </button>
        <button
          className="command-bar-trigger"
          onClick={() => setCommandBarOpen(!commandBarOpen)}
        >
          ⌘ Command Bar (Ctrl+K)
        </button>
      </div>

      {/* Main Browser Window Mockup */}
      <div className={`browser-mockup-container mode-${layoutMode}`}>
        {/* Floating Command Bar Overlay */}
        {commandBarOpen && (
          <div className="command-bar-overlay" onClick={() => setCommandBarOpen(false)}>
            <div className="command-bar-modal" onClick={e => e.stopPropagation()}>
              <div className="command-input-row">
                <span className="command-icon">🔍</span>
                <input
                  id="aeon-command-input"
                  name="commandQuery"
                  aria-label="Aeon Command Bar"
                  type="text"
                  placeholder="Search web, execute AI action, or switch Space... (e.g. 'clean junk')"
                  className="command-input"
                  autoFocus
                />
                <kbd className="command-kbd">ESC</kbd>
              </div>
              <div className="command-suggestions">
                <div className="command-item" onClick={() => { setActiveTab(0); setCommandBarOpen(false); }}>
                  <span>⚡ Switch to Sovereign AI Agent</span>
                  <span className="item-badge">Tab 1</span>
                </div>
                <div className="command-item" onClick={() => { setActiveTab(1); setCommandBarOpen(false); }}>
                  <span>🛡️ View Brave Privacy Shield Stats</span>
                  <span className="item-badge">Tab 2</span>
                </div>
                <div className="command-item" onClick={() => { setCommandBarOpen(false); }}>
                  <span>🔲 Toggle Split View Tiling</span>
                  <span className="item-badge">Shortcut</span>
                </div>
              </div>
            </div>
          </div>
        )}

        {/* ── ARC VERTICAL SIDEBAR LAYOUT ── */}
        {layoutMode === 'arc' ? (
          <div className="arc-browser-shell">
            {/* Left Vertical Sidebar */}
            <div className={`arc-sidebar ${sidebarCollapsed ? 'collapsed' : ''}`}>
              {/* Sidebar Header & Brand */}
              <div className="arc-sidebar-header">
                <div className="brand-badge">
                  <span className="brand-dot" style={{ background: activeSpaceData?.color }}></span>
                  <span className="brand-name">AEON</span>
                </div>
                <button
                  className="sidebar-toggle"
                  onClick={() => setSidebarCollapsed(!sidebarCollapsed)}
                  title="Toggle Sidebar (Ctrl+S)"
                >
                  ◀
                </button>
              </div>

              {!sidebarCollapsed && (
                <>
                  {/* Pinned Favorites */}
                  <div className="arc-pinned-bar">
                    <span className="pinned-icon" title="Local AI">⚡</span>
                    <span className="pinned-icon" title="Brave Shield">🛡️</span>
                    <span className="pinned-icon" title="GitHub">🐙</span>
                    <span className="pinned-icon" title="Settings">⚙️</span>
                  </div>

                  {/* Spaces Selector */}
                  <div className="arc-spaces-section">
                    <span className="section-label">SPACES</span>
                    <div className="space-chips">
                      {spaces.map(s => (
                        <button
                          key={s.id}
                          className={`space-chip ${activeSpace === s.id ? 'active' : ''}`}
                          style={{ '--space-color': s.color }}
                          onClick={() => setActiveSpace(s.id)}
                        >
                          {s.name}
                        </button>
                      ))}
                    </div>
                  </div>

                  {/* Vertical Tabs List */}
                  <div className="arc-tabs-section">
                    <span className="section-label">TABS</span>
                    <div className="arc-tab-tree">
                      {tabs.map((t, idx) => (
                        <button
                          key={t.id}
                          className={`arc-tab-item ${activeTab === idx ? 'active' : ''}`}
                          style={{ '--active-border': activeSpaceData?.color }}
                          onClick={() => setActiveTab(idx)}
                        >
                          <span className="tab-title">{t.title}</span>
                          <span className="tab-close">×</span>
                        </button>
                      ))}
                    </div>
                  </div>

                  {/* Sidebar Footer */}
                  <div className="arc-sidebar-footer">
                    <span className="route-indicator">🟢 Clearnet / DoH</span>
                    <span className="memory-tag">RAM: 1.2 GB</span>
                  </div>
                </>
              )}
            </div>

            {/* Main Content Area */}
            <div className="arc-content-wrapper">
              {/* Minimal Top Nav Row */}
              <div className="arc-top-nav">
                <div className="nav-controls">
                  <button className="nav-btn">←</button>
                  <button className="nav-btn">→</button>
                  <button className="nav-btn">↻</button>
                </div>

                {/* Pill URL Bar */}
                <div className="arc-url-bar">
                  <span className="lock-icon">🔒</span>
                  <span className="url-text">{currentTab.url}</span>
                  <button
                    className={`shield-badge ${shieldActive ? 'active' : ''}`}
                    onClick={() => setShieldActive(!shieldActive)}
                  >
                    <span>🛡️</span>
                    <span>{currentTab.shieldCount}</span>
                  </button>
                </div>

                <div className="window-controls">
                  <span>_</span>
                  <span>□</span>
                  <span className="close-x">✕</span>
                </div>
              </div>

              {/* Web View Body */}
              <div className="chrome-body">
                <div className="page-card">
                  <div className="page-header">
                    <div>
                      <span className="status-tag" style={{ background: `${activeSpaceData?.color}22`, color: activeSpaceData?.color }}>
                        {currentTab.content.badge}
                      </span>
                      <h3 className="page-h3">{currentTab.content.heading}</h3>
                    </div>
                    <div className="pulse-indicator">
                      <span className="pulse-dot"></span>
                      {currentTab.content.status}
                    </div>
                  </div>

                  <div className="log-console">
                    {currentTab.content.logs.map((log, i) => (
                      <div key={i} className="log-line">
                        <span className="log-time">08:25:{10 + i * 4}</span>
                        <span className="log-msg">{log}</span>
                      </div>
                    ))}
                  </div>
                </div>
              </div>
            </div>
          </div>
        ) : (
          /* ── BRAVE + CHROME HORIZONTAL LAYOUT ── */
          <div className="chrome-browser-shell">
            <div className="chrome-header">
              <div className="chrome-top-row">
                <div className="window-dots">
                  <span className="dot dot-close"></span>
                  <span className="dot dot-min"></span>
                  <span className="dot dot-max"></span>
                </div>

                <div className="tab-strip">
                  {tabs.map((t, idx) => (
                    <button
                      key={t.id}
                      className={`tab-item ${activeTab === idx ? 'active' : ''}`}
                      onClick={() => setActiveTab(idx)}
                    >
                      <span className="tab-title">{t.title}</span>
                      <span className="tab-close">×</span>
                    </button>
                  ))}
                  <button className="tab-add">+</button>
                </div>
              </div>

              <div className="chrome-nav-row">
                <div className="nav-buttons">
                  <button className="nav-btn">←</button>
                  <button className="nav-btn">→</button>
                  <button className="nav-btn">↻</button>
                </div>

                <div className="url-bar">
                  <span className="lock-icon">🔒</span>
                  <span className="url-text">{currentTab.url}</span>
                  <button
                    className={`shield-badge ${shieldActive ? 'active' : ''}`}
                    onClick={() => setShieldActive(!shieldActive)}
                  >
                    <span>🛡️</span>
                    <span>{currentTab.shieldCount}</span>
                  </button>
                </div>

                <div className="action-icons">
                  <span className="action-btn">⬇</span>
                  <span className="action-btn">★</span>
                  <span className="action-btn">⚡</span>
                  <span className="action-btn">⋮</span>
                </div>
              </div>
            </div>

            <div className="chrome-body">
              <div className="page-card">
                <div className="page-header">
                  <div>
                    <span className="status-tag">{currentTab.content.badge}</span>
                    <h3 className="page-h3">{currentTab.content.heading}</h3>
                  </div>
                  <div className="pulse-indicator">
                    <span className="pulse-dot"></span>
                    {currentTab.content.status}
                  </div>
                </div>

                <div className="log-console">
                  {currentTab.content.logs.map((log, i) => (
                    <div key={i} className="log-line">
                      <span className="log-time">08:25:{10 + i * 4}</span>
                      <span className="log-msg">{log}</span>
                    </div>
                  ))}
                </div>
              </div>
            </div>
          </div>
        )}
      </div>
    </div>
  );
}
