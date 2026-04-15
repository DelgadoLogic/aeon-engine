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
    const io = new IntersectionObserver((entries) => {
      entries.forEach((e) => {
        if (e.isIntersecting) {
          e.target.classList.add('visible');
        }
      });
    }, { threshold: 0.08, rootMargin: '0px 0px -40px 0px' });

    const elements = document.querySelectorAll('.reveal');
    elements.forEach((el) => io.observe(el));

    return () => {
      elements.forEach((el) => io.unobserve(el));
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
