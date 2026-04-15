import type {ReactNode} from 'react';
import clsx from 'clsx';
import Link from '@docusaurus/Link';
import useDocusaurusContext from '@docusaurus/useDocusaurusContext';
import Layout from '@theme/Layout';
import Heading from '@theme/Heading';

import styles from './index.module.css';

function HeroSection() {
  return (
    <header className={clsx('hero hero--primary', styles.heroBanner)}>
      <div className={styles.heroGlow} />
      <div className="container">
        <div className={styles.heroContent}>
          <span className={styles.badge}>Developer Preview</span>
          <Heading as="h1" className="hero__title">
            Build the Future<br />of Browsing
          </Heading>
          <p className="hero__subtitle">
            The official developer hub for Aeon Browser — a privacy-first,
            AI-native browser built from the ground up. Explore the architecture,
            contribute to the engine, and shape what comes next.
          </p>
          <div className={styles.heroButtons}>
            <Link className="button button--primary button--lg" to="/docs/intro">
              Get Started →
            </Link>
            <Link
              className="button button--secondary button--lg"
              href="https://github.com/AeonBrowserDev/aeon-browser">
              View on GitHub
            </Link>
          </div>
          <div className={styles.heroMeta}>
            <span className={styles.metaItem}>
              <span className={styles.metaDot} style={{background: '#68d391'}} />
              Open Source
            </span>
            <span className={styles.metaItem}>
              <span className={styles.metaDot} style={{background: '#6382ff'}} />
              C++ / Chromium Core
            </span>
            <span className={styles.metaItem}>
              <span className={styles.metaDot} style={{background: '#a78bfa'}} />
              AI-Integrated
            </span>
          </div>
        </div>
      </div>
    </header>
  );
}

type FeatureItem = {
  icon: string;
  title: string;
  description: string;
  link?: string;
};

const features: FeatureItem[] = [
  {
    icon: '📐',
    title: 'Architecture',
    description:
      'Understand how Aeon is built — from the Chromium fork to the custom rendering pipeline, tab management, and the modular engine architecture.',
    link: '/docs/intro',
  },
  {
    icon: '🔒',
    title: 'Privacy Engine',
    description:
      'Deep dive into ad-blocking, tracker prevention, fingerprint resistance, and automatic HTTPS upgrades built directly into the browser core.',
    link: '/docs/intro',
  },
  {
    icon: '🤖',
    title: 'AI Integration',
    description:
      'Explore how Gemini and local LLMs integrate at the engine level — smart tab grouping, page summarization, and context-aware browsing.',
    link: '/docs/intro',
  },
  {
    icon: '⚡',
    title: 'Performance',
    description:
      'Memory optimization, lazy resource loading, efficient process management, and rendering pipeline tweaks that make Aeon feel instant.',
    link: '/docs/intro',
  },
  {
    icon: '🧩',
    title: 'Extensions API',
    description:
      'Full Manifest V3 compatibility with Aeon-specific APIs for deeper privacy controls and AI-powered extension capabilities.',
    link: '/docs/intro',
  },
  {
    icon: '🚀',
    title: 'Contributing',
    description:
      'Set up your development environment, understand the build system, and start contributing to Aeon Browser in minutes.',
    link: '/docs/intro',
  },
];

function FeatureCard({icon, title, description, link}: FeatureItem) {
  const content = (
    <div className={styles.featureCard}>
      <span className={styles.featureIcon}>{icon}</span>
      <h3 className={styles.featureTitle}>{title}</h3>
      <p className={styles.featureDescription}>{description}</p>
      {link && <span className={styles.featureLink}>Learn more →</span>}
    </div>
  );

  return link ? <Link to={link} className={styles.featureCardLink}>{content}</Link> : content;
}

function FeaturesSection() {
  return (
    <section className={styles.features}>
      <div className="container">
        <div className={styles.sectionHeader}>
          <span className={styles.sectionLabel}>Documentation</span>
          <Heading as="h2" className={styles.sectionTitle}>
            Everything you need to know
          </Heading>
          <p className={styles.sectionSub}>
            From architecture deep-dives to quick-start guides — comprehensive
            documentation for developers building with and on Aeon.
          </p>
        </div>
        <div className={styles.featureGrid}>
          {features.map((props, idx) => (
            <FeatureCard key={idx} {...props} />
          ))}
        </div>
      </div>
    </section>
  );
}

function StatsSection() {
  return (
    <section className={styles.stats}>
      <div className="container">
        <div className={styles.statsGrid}>
          <div className={styles.statItem}>
            <span className={styles.statNumber}>C++</span>
            <span className={styles.statLabel}>Engine Core</span>
          </div>
          <div className={styles.statItem}>
            <span className={styles.statNumber}>Chromium</span>
            <span className={styles.statLabel}>Foundation</span>
          </div>
          <div className={styles.statItem}>
            <span className={styles.statNumber}>100%</span>
            <span className={styles.statLabel}>Open Source</span>
          </div>
          <div className={styles.statItem}>
            <span className={styles.statNumber}>Zero</span>
            <span className={styles.statLabel}>Telemetry</span>
          </div>
        </div>
      </div>
    </section>
  );
}

function CTASection() {
  return (
    <section className={styles.cta}>
      <div className="container">
        <div className={styles.ctaInner}>
          <Heading as="h2" className={styles.ctaTitle}>
            Ready to dive in?
          </Heading>
          <p className={styles.ctaSub}>
            Start exploring the documentation, set up your dev environment,
            or jump straight into the source code.
          </p>
          <div className={styles.ctaButtons}>
            <Link className="button button--primary button--lg" to="/docs/intro">
              Read the Docs
            </Link>
            <Link
              className="button button--secondary button--lg"
              href="https://github.com/AeonBrowserDev/aeon-browser">
              Star on GitHub ⭐
            </Link>
          </div>
        </div>
      </div>
    </section>
  );
}

export default function Home(): ReactNode {
  return (
    <Layout
      title="Developer Documentation"
      description="Official developer documentation and resources for Aeon Browser — a privacy-first, AI-native web browser built from the ground up.">
      <HeroSection />
      <main>
        <StatsSection />
        <FeaturesSection />
        <CTASection />
      </main>
    </Layout>
  );
}
