import { defineConfig } from 'vitepress'

const SITE_URL = 'https://kura.bentopdf.com'

export default defineConfig({
  title: 'Kura Docs',
  description: 'Documentation for Kura, the open source PDF standards and preflight engine by BentoPDF: PDF/A, PDF/UA, PDF/X, PDF/E, PDF/VT, print preflight and e-invoices.',
  base: '/docs/',
  cleanUrls: true,

  transformPageData(pageData) {
    const relPath = pageData.relativePath.replace(/\.md$/, '')
    const slug = relPath === 'index' ? '' : relPath.replace(/\/index$/, '/')
    const canonicalUrl = slug ? `${SITE_URL}/docs/${slug}` : `${SITE_URL}/docs/`
    pageData.frontmatter.head ??= []
    pageData.frontmatter.head.push(['link', { rel: 'canonical', href: canonicalUrl }])
  },

  themeConfig: {
    logo: '/images/logo.svg',

    nav: [
      { text: 'Convert a PDF', link: 'https://kura.bentopdf.com' },
      { text: 'Getting Started', link: '/getting-started' },
      { text: 'Standards', link: '/standards' },
      { text: 'BentoPDF', link: 'https://www.bentopdf.com' },
    ],

    sidebar: [
      {
        text: 'Guide',
        items: [
          { text: 'What is Kura', link: '/' },
          { text: 'Getting Started', link: '/getting-started' },
          { text: 'The standards', link: '/standards' },
          { text: 'Check mode', link: '/check-mode' },
          { text: 'E-invoices', link: '/e-invoices' },
          { text: 'Preflight profiles', link: '/preflight' },
        ],
      },
      {
        text: 'Reference',
        items: [
          { text: 'CLI', link: '/cli' },
          { text: 'npm package', link: '/npm' },
          { text: 'C API', link: '/c-api' },
          { text: 'Rejection codes', link: '/rejections' },
        ],
      },
      {
        text: 'Operate',
        items: [
          { text: 'Building from source', link: '/building' },
          { text: 'Fuzzing', link: '/fuzzing' },
          { text: 'Security', link: '/security' },
          { text: 'Licensing', link: '/licensing' },
        ],
      },
    ],

    socialLinks: [
      { icon: 'github', link: 'https://github.com/alam00000/bentopdf-kura' },
      { icon: 'npm', link: 'https://www.npmjs.com/package/kura-pdf' },
    ],

    footer: {
      message: 'Dual-licensed under AGPL-3.0 and Commercial License.',
      copyright: 'Copyright © 2026 BentoPDF',
    },

    search: {
      provider: 'local',
    },
  },
})
