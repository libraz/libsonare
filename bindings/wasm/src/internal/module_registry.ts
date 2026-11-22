/**
 * The one WASM module holder of a realm, shared by every entry point of a build.
 *
 * Each published entry (`index`, `worklet`, `worker`, `vocal-edit-worker`,
 * `analysis`) is bundled self-contained, so each carries its own copy of this
 * code. The holder itself lives on `globalThis` under a key naming the build and
 * the module flavour, so the copies in one realm find the same holder: a module
 * `init()` loaded through one entry serves the others. An AudioWorklet or Worker
 * realm has its own `globalThis` and initialises its own instance. Two builds on
 * one page (a pinned dependency, a stale cached worklet file) carry different
 * build ids and never share a module whose exports their wrapper code does not
 * expect.
 */

import type { ModuleHandle } from '../public_types.js';
import type { SonareModule } from '../sonare.js';

/**
 * Stamped into every entry of one build by the bundler (`tsup.config.ts`):
 * a digest of the sources and the emscripten glue the build bundled.
 */
declare const __SONARE_BUILD_ID__: string | undefined;

/** This build's identity; `source` when running unbundled from `src/`. */
const BUILD_ID: string = typeof __SONARE_BUILD_ID__ === 'string' ? __SONARE_BUILD_ID__ : 'source';

/** Which WASM binary a holder carries: the full module or the analysis-only one. */
export type ModuleFlavour = 'full' | 'analysis';

/** What `init()` accepts, the same on every entry. */
export interface ModuleInitOptions {
  locateFile?: (path: string, prefix: string) => string;
  wasmBinary?: ArrayBuffer | Uint8Array;
  moduleFactory?: (options?: {
    locateFile?: (path: string, prefix: string) => string;
    wasmBinary?: ArrayBuffer | Uint8Array;
  }) => Promise<ModuleHandle>;
}

/** One realm's module state for one build and flavour. */
export interface ModuleSlot {
  /** The loaded module, as `init()` created it; each bundle wraps it for itself. */
  module: SonareModule | null;
  /** The load in flight, so concurrent `init()` calls share it. */
  pending: Promise<void> | null;
  /** The options of the call that started the load; later calls must match them. */
  firstOptions: object | undefined;
}

/** The registry key of @p flavour for @p buildId. */
export function moduleSlotKey(flavour: ModuleFlavour, buildId: string = BUILD_ID): symbol {
  return Symbol.for(`libsonare.module/${buildId}/${flavour}`);
}

/** The realm's holder for @p flavour of @p buildId, created empty on first use. */
export function moduleSlot(flavour: ModuleFlavour, buildId: string = BUILD_ID): ModuleSlot {
  const key = moduleSlotKey(flavour, buildId);
  const realm = globalThis as unknown as Record<symbol, ModuleSlot | undefined>;
  const found = realm[key];
  if (found) {
    return found;
  }
  const slot: ModuleSlot = { module: null, pending: null, firstOptions: undefined };
  Object.defineProperty(realm, key, { value: slot, enumerable: false });
  return slot;
}
