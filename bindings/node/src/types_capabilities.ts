/** Build and runtime capabilities reported by the loaded native library. */
export interface Capabilities {
  /** libsonare semantic version. */
  version: string;
  /** ABI versions used by project and realtime-engine data structures. */
  abi: {
    project: number;
    engine: number;
  };
  /** Native target identifier, such as `darwin-arm64` or `wasm32`. */
  platform: string;
  /** Optional feature families compiled into this library. */
  features: {
    mastering: boolean;
    mixing: boolean;
    fx: boolean;
    ffmpeg: boolean;
  };
  /** Audio decoding available without, and through, FFmpeg. */
  decode: {
    builtin: string[];
    ffmpeg: string[];
  };
  /** SIMD implementation selected for the target. */
  simd: string;
  /** Hardware thread count reported by the native runtime. */
  hardwareConcurrency: number;
}
