import React, { useEffect, useState, useRef } from 'react';

const stats = [
  { value: 100, suffix: '%', label: 'Chromium Engine (Stripped)', icon: '⚡' },
  { value: 0,   suffix: '',  label: 'Google Background Pings',    icon: '🔒' },
  { value: 8,   suffix: '',  label: 'Obsidian aeon:// Hubs',      icon: '🖥️' },
  { value: 60,  suffix: 'd', label: 'Pro Refund Guarantee',       icon: '🛡️' },
];

function AnimatedNumber({ target, active }) {
  const [count, setCount] = useState(target);
  useEffect(() => {
    if (!active || target === 0) return undefined;
    let rafId = 0;
    let start = null;
    const duration = 900;
    const step = (ts) => {
      if (!start) start = ts;
      const progress = Math.min((ts - start) / duration, 1);
      const ease = 1 - Math.pow(1 - progress, 3);
      setCount(Math.floor(ease * target));
      if (progress < 1) rafId = requestAnimationFrame(step);
      else setCount(target);
    };
    rafId = requestAnimationFrame(step);
    return () => cancelAnimationFrame(rafId);
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
    }, { threshold: 0.15 });
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
