import js from '@eslint/js';
import globals from 'globals';

export default [
  { ignores: ['node_modules/', 'docs/.vitepress/cache/', 'docs/.vitepress/dist/', 'packages/npm/kura-pdf-wasm/engine/', 'site/kura.js', 'site/profiles.js', 'pdfa-engine/'] },
  js.configs.recommended,
  {
    files: ['packages/npm/*/index.js', 'packages/npm/*/bin/*.js', 'scripts/**/*.mjs', 'server/**/*.mjs', 'site/serve.mjs'],
    languageOptions: { ecmaVersion: 2022, sourceType: 'module', globals: { ...globals.node } },
  },
  {
    files: ['site/app.js', 'site/preflight.js', 'site/engine.js', 'site/engine-http.js', 'site/ui.js', 'site/icons/phosphor.js'],
    languageOptions: { ecmaVersion: 2022, sourceType: 'module', globals: { ...globals.browser } },
  },
  {
    files: ['site/worker.js'],
    languageOptions: { ecmaVersion: 2022, sourceType: 'module', globals: { ...globals.worker } },
  },
  {
    rules: {
      'no-console': 'off',
      'no-empty': ['error', { allowEmptyCatch: true }],
      'no-unused-vars': ['error', { argsIgnorePattern: '^_' }],
    },
  },
];
