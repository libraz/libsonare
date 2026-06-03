import type { SonareModule } from './sonare.js';

let wasmModule: SonareModule | null = null;

export function setSonareModule(module: SonareModule): void {
  wasmModule = module;
}

export function getSonareModule(): SonareModule {
  if (!wasmModule) {
    throw new Error('Module not initialized. Call init() first.');
  }
  return wasmModule;
}
