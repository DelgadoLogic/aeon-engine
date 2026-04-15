import {themes as prismThemes} from 'prism-react-renderer';
import type {Config} from '@docusaurus/types';
import type * as Preset from '@docusaurus/preset-classic';

const config: Config = {
  title: 'Aeon Browser',
  tagline: 'Developer Documentation & Resources',
  favicon: 'img/aeon-favicon.svg',

  future: {
    v4: true,
  },

  url: 'https://aeonbrowser.dev',
  baseUrl: '/',

  organizationName: 'AeonBrowserDev',
  projectName: 'aeon-browser',

  onBrokenLinks: 'warn',

  i18n: {
    defaultLocale: 'en',
    locales: ['en'],
  },

  headTags: [
    {
      tagName: 'link',
      attributes: {
        rel: 'preconnect',
        href: 'https://fonts.googleapis.com',
      },
    },
    {
      tagName: 'link',
      attributes: {
        rel: 'preconnect',
        href: 'https://fonts.gstatic.com',
        crossorigin: 'anonymous',
      },
    },
    {
      tagName: 'link',
      attributes: {
        rel: 'stylesheet',
        href: 'https://fonts.googleapis.com/css2?family=Inter:wght@400;500;600;700;800;900&family=JetBrains+Mono:wght@400;500;600&display=swap',
      },
    },
  ],

  presets: [
    [
      'classic',
      {
        docs: {
          sidebarPath: './sidebars.ts',
          editUrl: 'https://github.com/AeonBrowserDev/aeon-browser/tree/main/dev-site/',
        },
        blog: {
          showReadingTime: true,
          feedOptions: {
            type: ['rss', 'atom'],
            xslt: true,
          },
          blogTitle: 'Aeon Dev Blog',
          blogDescription: 'Engineering updates, release notes, and deep dives from the Aeon Browser team.',
          onInlineTags: 'warn',
          onInlineAuthors: 'warn',
          onUntruncatedBlogPosts: 'warn',
        },
        theme: {
          customCss: './src/css/custom.css',
        },
      } satisfies Preset.Options,
    ],
  ],

  themeConfig: {
    image: 'img/aeon-social-card.png',
    metadata: [
      {name: 'keywords', content: 'aeon, browser, developer, documentation, privacy, decentralized, open source'},
      {name: 'twitter:card', content: 'summary_large_image'},
      {name: 'og:type', content: 'website'},
    ],
    colorMode: {
      defaultMode: 'dark',
      disableSwitch: false,
      respectPrefersColorScheme: true,
    },
    announcementBar: {
      id: 'alpha_notice',
      content: '🚀 Aeon Browser is in active development. <a href="/docs/intro">Read the docs</a> to follow our progress.',
      backgroundColor: 'rgba(99, 130, 255, 0.12)',
      textColor: '#e8eaf6',
      isCloseable: true,
    },
    navbar: {
      title: 'Aeon Browser',
      logo: {
        alt: 'Aeon Browser Logo',
        src: 'img/aeon-logo.svg',
        srcDark: 'img/aeon-logo.svg',
        style: { height: '32px' },
      },
      style: 'dark',
      items: [
        {
          type: 'docSidebar',
          sidebarId: 'tutorialSidebar',
          position: 'left',
          label: 'Documentation',
        },
        {to: '/blog', label: 'Dev Blog', position: 'left'},
        {
          href: 'https://browseaeon.com',
          label: 'Main Site',
          position: 'right',
        },
        {
          href: 'https://github.com/AeonBrowserDev/aeon-browser',
          label: 'GitHub',
          position: 'right',
          className: 'header-github-link',
        },
      ],
    },
    footer: {
      style: 'dark',
      links: [
        {
          title: 'Documentation',
          items: [
            {
              label: 'Getting Started',
              to: '/docs/intro',
            },
            {
              label: 'Architecture',
              to: '/docs/intro',
            },
            {
              label: 'API Reference',
              to: '/docs/intro',
            },
          ],
        },
        {
          title: 'Product',
          items: [
            {
              label: 'browseaeon.com',
              href: 'https://browseaeon.com',
            },
            {
              label: 'Download (Coming Soon)',
              href: 'https://browseaeon.com',
            },
            {
              label: 'Roadmap',
              to: '/docs/intro',
            },
          ],
        },
        {
          title: 'Community',
          items: [
            {
              label: 'GitHub',
              href: 'https://github.com/AeonBrowserDev/aeon-browser',
            },
            {
              label: 'Dev Blog',
              to: '/blog',
            },
          ],
        },
        {
          title: 'Legal',
          items: [
            {
              label: 'Privacy Policy',
              to: '/docs/intro',
            },
            {
              label: 'Terms of Service',
              to: '/docs/intro',
            },
          ],
        },
      ],
      logo: {
        alt: 'Aeon Browser',
        src: 'img/aeon-circle.svg',
        href: 'https://browseaeon.com',
        width: 48,
        height: 48,
      },
      copyright: `© ${new Date().getFullYear()} DelgadoLogic. Aeon Browser — Built different.`,
    },
    prism: {
      theme: prismThemes.github,
      darkTheme: prismThemes.dracula,
      additionalLanguages: ['bash', 'json', 'powershell', 'cpp', 'rust'],
    },
  } satisfies Preset.ThemeConfig,
};

export default config;
