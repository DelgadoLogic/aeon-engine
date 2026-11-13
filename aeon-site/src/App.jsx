import React, { useEffect } from 'react';
import Navigation from './components/Navigation';
import Hero from './components/Hero';
import StatsBar from './components/StatsBar';
import EasySwitch from './components/EasySwitch';
import Features from './components/Features';
import HowItWorks from './components/HowItWorks';
import Comparison from './components/Comparison';
import TrustSignals from './components/TrustSignals';
import Pricing from './components/Pricing';
import Waitlist from './components/Download';
import Footer from './components/Footer';

function App() {
  useEffect(() => {
    const elements = document.querySelectorAll('.reveal');
    if (!('IntersectionObserver' in window)) {
      elements.forEach((el) => el.classList.add('visible'));
      return undefined;
    }

    const io = new IntersectionObserver((entries) => {
      entries.forEach((e) => {
        if (e.isIntersecting) {
          e.target.classList.add('visible');
        }
      });
    }, { threshold: 0.02, rootMargin: '200px 0px 200px 0px' });

    elements.forEach((el) => io.observe(el));

    // Progressive enhancement safety timer so sections never remain invisible
    const fallbackTimer = setTimeout(() => {
      elements.forEach((el) => el.classList.add('visible'));
    }, 450);

    return () => {
      clearTimeout(fallbackTimer);
      io.disconnect();
    };
  }, []);

  return (
    <div className="app-container">
      <Navigation />
      <Hero />
      <StatsBar />
      <EasySwitch />
      <Features />
      <HowItWorks />
      <Comparison />
      <TrustSignals />
      <Pricing />
      <Waitlist />
      <Footer />
    </div>
  );
}

export default App;
