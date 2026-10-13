import { init as initSonareModule, isInitialized } from '../index.js';
import type { SonareModule } from '../sonare.js';

type EmbindModuleFactory = (options?: {
  locateFile?: (path: string, prefix: string) => string;
  wasmBinary?: ArrayBuffer | Uint8Array;
}) => Promise<SonareModule>;

type EmbindGlobals = typeof globalThis & {
  SonareEmbindInitPromise?: Promise<void>;
  SonareEmbindModuleFactory?: EmbindModuleFactory;
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
    throw new Error('embind realtime engine module is not initialized.');
  }
  await initSonareModule({
    locateFile: (path) => path,
    wasmBinary,
    moduleFactory,
  });
}
