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
    include: ['js/**/*.test.ts', 'tests/wasm/**/*.test.ts'],
    coverage: {
      provider: 'v8',
      reporter: ['text', 'json', 'html'],
      include: ['js/**/*.ts'],
      exclude: ['js/**/*.test.ts', 'js/**/*.d.ts'],
    },
  },
});
