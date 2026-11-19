// The suite with every public export wrapped to record refusals; see refusal-capture-setup.mjs.
import { fileURLToPath } from 'node:url';
import { defineConfig, mergeConfig } from 'vitest/config';
import base from '../vitest.config';

export default mergeConfig(
  base,
  defineConfig({
    root: fileURLToPath(new URL('..', import.meta.url)),
    test: { setupFiles: [fileURLToPath(new URL('./refusal-capture-setup.mjs', import.meta.url))] },
  }),
);
