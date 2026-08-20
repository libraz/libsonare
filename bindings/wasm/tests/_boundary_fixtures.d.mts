/**
 * Declarations for `_boundary_fixtures.mjs`. The fixtures stay plain ESM so the
 * headless-browser smoke can serve them verbatim, which leaves TypeScript with
 * nothing to infer from.
 */

export declare const SR: number;
export declare const BLOCK: number;
export declare const WASM_FLOAT_BUDGET: number;

/** Drain counts that every boundary runner must reject. */
export declare const INVALID_DRAIN_COUNTS: number[];

/** Inverse (mel -> STFT) `[rows, frames]` shapes rejected as an overflow. */
export declare const INVERSE_OVERFLOW_SHAPES: Array<[number, number]>;

/** A 1 kHz sine of `BLOCK` frames at the given amplitude. */
export declare function sine(amplitude: number): Float32Array;

/** Root mean square of a sample block. */
export declare function rms(samples: Float32Array): number;

/** Configures band 0 as a dynamic, externally side-chained peak band. */
export declare function configureDynamicEq(eq: {
  setBand: (index: number, band: Record<string, unknown>) => void;
}): void;
