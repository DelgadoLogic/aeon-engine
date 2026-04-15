import React, { useEffect, useState, useRef } from 'react';

const stats = [
  { value: 70,    suffix: '+', label: 'CVE Sources Scanned',  icon: '🔬' },
  { value: 0,     suffix: '',  label: 'Bytes Phoned Home',    icon: '🔒' },
  { value: 8,     suffix: '',  label: 'OS Tiers Supported',   icon: '🖥️' },
  { value: 847,   suffix: '+', label: 'P2P Peers (Simulated)', icon: '🕸️' },
];

function AnimatedNumber({ target, active }) {
  const [count, setCount] = useState(0);
  useEffect(() => {
    if (!active) return;
    if (target === 0) { setCount(0); return; }
    let start = null;
    const duration = 1600;
    const step = (ts) => {
      if (!start) start = ts;
      const progress = Math.min((ts - start) / duration, 1);
      const ease = 1 - Math.pow(1 - progress, 3);
      setCount(Math.floor(ease * target));
      if (progress < 1) requestAnimationFrame(step);
      else setCount(target);
    };
    requestAnimationFrame(step);
  }, [active, target]);
  return <>{count.toLocaleString()}</>;
}

export default function StatsBar() {
  const ref = useRef(null);
  const [active, setActive] = useState(false);

  useEffect(() => {
    if (!ref.current) return;
    const obs = new IntersectionObserver(([e]) => {
      if (e.isIntersecting && !active) setActive(true);
    }, { threshold: 0.4 });
    obs.observe(ref.current);
    return () => obs.disconnect();
  }, [active]);

  return (
    <div className="stats-bar" ref={ref}>
      {stats.map((s, i) => (
        <div className="stat" key={i}>
          <div className="stat-icon">{s.icon}</div>
          <div className="stat-number">
            <AnimatedNumber target={s.value} active={active} />
            <span className="accent">{s.suffix}</span>
          </div>
          <div className="stat-label">{s.label}</div>
        </div>
      ))}
    </div>
  );
}
