import { createHash } from 'node:crypto';
import { readdirSync, readFileSync } from 'node:fs';
import { join } from 'node:path';
import { defineConfig } from 'tsup';

/** Every file under @p dir, sorted, so the digest does not depend on directory order. */
function filesUnder(dir: string): string[] {
  return readdirSync(dir, { withFileTypes: true, recursive: true })
    .filter((entry) => entry.isFile())
    .map((entry) => join(entry.parentPath, entry.name))
    .sort();
}

/** The emscripten glue and the binaries it loads, as `build:wasm` leaves them in `dist/`. */
const GLUE_AND_BINARIES = [
  'dist/sonare.js',
  'dist/sonare.wasm',
  'dist/sonare-analysis.js',
  'dist/sonare-analysis.wasm',
];

/**
 * One identity for every entry of this build: the sources, the emscripten glue
 * they bundle and the binaries that glue loads. Entries of one build share a
 * module holder per realm; two builds never do (`src/internal/module_registry.ts`).
 */
function buildId(): string {
  const hash = createHash('sha256');
  for (const file of [...filesUnder('src'), ...GLUE_AND_BINARIES]) {
    hash.update(file).update('\0').update(readFileSync(file)).update('\0');
  }
  return hash.digest('hex').slice(0, 16);
}

const common = {
  define: { __SONARE_BUILD_ID__: JSON.stringify(buildId()) },
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
  { ...common, entry: ['src/analysis.ts'], external: ['./sonare-analysis.js'] },
  { ...common, entry: ['src/index.ts'] },
  { ...common, entry: ['src/worklet.ts'] },
  { ...common, entry: ['src/worker.ts'] },
  { ...common, entry: ['src/vocal_edit_worker.ts'] },
]);
