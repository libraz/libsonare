import type { SonareModule } from './sonare.js';

declare const createSonareAnalysis: (options?: {
  locateFile?: (path: string, prefix: string) => string;
  wasmBinary?: ArrayBuffer | Uint8Array;
}) => Promise<SonareModule>;

export default createSonareAnalysis;
