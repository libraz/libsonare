import path from 'node:path';
import { defineConfig } from 'vitest/config';

export default defineConfig({
  resolve: {
    alias: {
      './sonare.js': path.resolve(__dirname, 'dist/sonare.js'),
    },
  },
  test: {
    globals: true,
    environment: 'node',
    // Heavy WASM analysis tests (e.g. NNLS chroma over multi-second signals) can
    // exceed vitest's 5s default on loaded CI runners; give them ample headroom.
    testTimeout: 30000,
    include: ['src/**/*.test.ts', 'tests/**/*.test.ts'],
    coverage: {
      provider: 'v8',
      reporter: ['text', 'json', 'html'],
      include: ['src/**/*.ts'],
      exclude: ['src/**/*.test.ts', 'src/**/*.d.ts'],
    },
  },
});
