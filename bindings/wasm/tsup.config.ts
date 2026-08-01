import { defineConfig } from 'tsup';

const common = {
  format: ['esm'] as const,
  dts: true,
  sourcemap: true,
  clean: false,
  target: 'es2020' as const,
  external: ['./sonare.js'],
  splitting: false,
};

// Build each entry as an independent bundle so its .d.ts is fully
// self-contained. A single multi-entry build makes tsup's DTS rollup hoist
// the types shared between index and worklet into a content-hashed chunk
// (e.g. `index-<hash>.d.ts`) whose name churns on every change.
export default defineConfig([
  { ...common, entry: ['src/analysis.ts'], external: [] },
  { ...common, entry: ['src/index.ts'] },
  { ...common, entry: ['src/worklet.ts'] },
  { ...common, entry: ['src/worker.ts'] },
]);
