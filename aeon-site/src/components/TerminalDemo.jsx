import React, { useEffect, useState } from 'react';

const linesData = [
  { time: '03:47:22', svc: 'AeonResearch', svcClass: 't-blue', msg: 'Scanning Chromium CVE database...' },
  { time: '03:47:31', svc: 'AeonResearch', svcClass: 't-yellow', msg: 'Found: CVE-2025-4421 — V8 type confusion (Critical)' },
  { time: '03:47:32', svc: 'AeonPatch   ', svcClass: 't-purple', msg: 'Generating C++ patch via AeonMind...' },
  { time: '03:48:01', svc: 'AeonPatch   ', svcClass: 't-green', msg: 'Patch ready. Signing with master key...' },
  { time: '03:48:02', svc: 'AeonUpdate  ', svcClass: 't-green', msg: 'Emergency patch distributed to 847 peers ✓' },
  { time: '03:48:02', svc: 'Silence     ', svcClass: 't-dim', msg: 'User online — all operations paused' },
  { time: '04:12:00', svc: 'Silence     ', svcClass: 't-dim', msg: 'User idle — operations resumed' },
  { time: '04:12:01', svc: 'AutoUpdater ', svcClass: 't-green', msg: 'Patch installed silently. Browser protected.' },
];

export default function TerminalDemo() {
  const [visibleLines, setVisibleLines] = useState([]);

  useEffect(() => {
    let currentLine = 0;
    let timeoutId;

    const addLine = () => {
      if (currentLine >= linesData.length) {
        timeoutId = setTimeout(() => {
          currentLine = 0;
          setVisibleLines([]);
          addLine();
        }, 3000);
        return;
      }
      
      const lineToPush = linesData[currentLine];
      setVisibleLines(prev => [...prev, lineToPush]);
      currentLine++;
      timeoutId = setTimeout(addLine, currentLine === linesData.length ? 800 : 600);
    };

    addLine();

    return () => clearTimeout(timeoutId);
  }, []);

  return (
    <div className="terminal-wrap">
      <div className="terminal">
        <div className="terminal-bar">
          <div className="dot"></div>
          <div className="dot"></div>
          <div className="dot"></div>
          <span className="label">aeon:// — Autonomous Operations Log</span>
        </div>
        <div className="terminal-body">
          {visibleLines.map((line, index) => (
            <div key={index} style={{ animation: 'fadeUp 0.4s ease both' }}>
              <span className="t-dim">{line.time}</span>{' '}
              <span className={line.svcClass}>{line.svc}</span>{' '}
              <span className="t-white">
                {line.msg}
                {index === linesData.length - 1 && <span className="cursor-blink"></span>}
              </span>
            </div>
          ))}
        </div>
      </div>
    </div>
  );
}
