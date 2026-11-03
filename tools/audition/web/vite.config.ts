import react from '@vitejs/plugin-react';
import { defineConfig } from 'vitest/config';

/** Routes `serve.py` answers; the dev server forwards them so `yarn dev` runs against real sets. */
const API = [
  '/sets.json',
  '/bank.json',
  '/queue.json',
  '/feedback-index.json',
  '/feedback',
  '/render',
  '/s/',
];
const SERVER = process.env.AUDITION_SERVER ?? 'http://127.0.0.1:8730';

export default defineConfig({
  plugins: [react()],
  // Relative, so the built page works under whatever path serve.py is reached at.
  base: './',
  // One bundle, served from localhost: ~540 kB minified (react, zod, i18next), so splitting buys nothing.
  build: { outDir: 'dist', emptyOutDir: true, sourcemap: true, chunkSizeWarningLimit: 800 },
  server: { proxy: Object.fromEntries(API.map((p) => [p, SERVER])) },
  test: {
    environment: 'jsdom',
    globals: true,
    setupFiles: ['tests/setup.ts'],
    include: ['tests/**/*.test.{ts,tsx}'],
    restoreMocks: true,
    // A cached transform keeps the file list `import.meta.glob` expanded to, so a
    // table added or removed would go unseen until the source importing it changed.
    fsModuleCache: false,
  },
});
