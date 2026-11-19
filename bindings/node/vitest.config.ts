import { defineConfig } from 'vitest/config';
import { lowerDecorators } from './scripts/vitest-decorators.mjs';

export default defineConfig({
  plugins: [lowerDecorators()],
  test: { globals: true, environment: 'node', globalSetup: ['./scripts/build-freshness-setup.mjs'] },
});
