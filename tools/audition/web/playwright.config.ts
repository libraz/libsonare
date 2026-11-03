import { defineConfig } from '@playwright/test';

/* The browser half: the built page against a real serve.py over synthetic renders.
 * `e2e/global-setup.ts` writes the sets and points the scratch root at a temp
 * directory, so no run reads or writes a listener's own feedback log. */
export default defineConfig({
  testDir: 'e2e',
  globalSetup: './e2e/global-setup.ts',
  globalTeardown: './e2e/global-teardown.ts',
  workers: 1,
  fullyParallel: false,
  retries: 0,
  use: {
    baseURL: 'http://127.0.0.1:8749/',
    channel: 'chrome',
    trace: 'retain-on-failure',
  },
});
