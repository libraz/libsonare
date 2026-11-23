import { init as initSonareModule, isInitialized } from '../index.js';
import { invalidState } from '../internal/entry.js';
import type { ModuleFactory } from '../public_types.js';

/** The module build this realm's processors started, while it is in flight. */
let realmBuild: Promise<void> | undefined;

type EmbindGlobals = typeof globalThis & {
  SonareEmbindInitPromise?: Promise<void>;
  SonareEmbindModuleFactory?: ModuleFactory;
};

/**
 * Whether a processor can construct its bridge in its own constructor: the
 * module is initialized in this realm and no host-provided init is pending.
 */
export function isEmbindReady(): boolean {
  return isInitialized() && !(globalThis as EmbindGlobals).SonareEmbindInitPromise;
}

/**
 * Initializes the WASM module inside an AudioWorkletGlobalScope, where `init()`
 * cannot fetch. Waits for the host's `globalThis.SonareEmbindInitPromise` when
 * one is set, then, if the module is still not initialized, builds it from
 * `globalThis.SonareEmbindModuleFactory` and `wasmBinary`. Shared by every
 * worklet processor so they accept the same recipe.
 *
 * @param wasmBinary - Compiled module bytes posted through `processorOptions`
 */
export async function initializeEmbindInWorklet(
  wasmBinary?: ArrayBuffer | Uint8Array,
): Promise<void> {
  const scope = globalThis as EmbindGlobals;
  if (scope.SonareEmbindInitPromise) {
    await scope.SonareEmbindInitPromise;
  }
  if (isInitialized()) {
    return;
  }
  const moduleFactory = scope.SonareEmbindModuleFactory;
  if (!moduleFactory) {
    throw invalidState(
      'the AudioWorklet realm has no module; set globalThis.SonareEmbindModuleFactory to the ' +
        "default export of '@libraz/libsonare/module' before the processor is constructed, " +
        'and pass wasmBinary in its processorOptions',
    );
  }
  // Each processor holds its own copy of wasmBinary, so the ones arriving mid-build join it.
  realmBuild ??= initSonareModule({ locateFile: (path) => path, wasmBinary, moduleFactory }).catch(
    (error: unknown) => {
      realmBuild = undefined;
      throw error;
    },
  );
  await realmBuild;
}
