export * from './public_types_acoustic';
export * from './public_types_mastering';
export * from './public_types_mixing';
export * from './public_types_music';
export * from './public_types_realtime';
export * from './public_types_spectral';

/** Runtime capabilities reported by the loaded libsonare build. */
export interface SonareCapabilities {
  version: string;
  abi: {
    project: number;
    engine: number;
  };
  platform: string;
  features: {
    mastering: boolean;
    mixing: boolean;
    fx: boolean;
    ffmpeg: boolean;
  };
  decode: {
    builtin: string[];
    ffmpeg: string[];
  };
  simd: string;
  hardwareConcurrency: number;
}

/** Return `false` to cancel at the next native progress boundary. */
// biome-ignore lint/suspicious/noConfusingVoidType: preserves legacy void callbacks while allowing false.
export type ProgressCallback = (progress: number, stage: string) => void | boolean;
