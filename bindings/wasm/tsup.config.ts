import { defineConfig } from 'tsup';

const common = {
  format: ['esm'] as const,
  dts: false,
  sourcemap: true,
  clean: false,
  target: 'es2020' as const,
  external: ['./sonare.js'],
  splitting: false,
};

// Each entry is an independent, self-contained JS bundle: a real
// AudioWorkletGlobalScope cannot resolve sibling chunks, so the worklet realm
// must carry its own copy of the runtime code.
//
// Declarations are not emitted here. A DTS rollup would either duplicate the
// classes shared between entries — making them nominally distinct, since they
// have private members — or hoist them into a content-hashed chunk
// (`index-<hash>.d.ts`) whose name churns on every change. `tsc
// --emitDeclarationOnly` instead emits one declaration per source module, so
// every entry references the same declaration through a stable path.
export default defineConfig([
  { ...common, entry: ['src/analysis.ts'], external: [] },
  { ...common, entry: ['src/index.ts'] },
  { ...common, entry: ['src/worklet.ts'] },
  { ...common, entry: ['src/worker.ts'] },
]);
