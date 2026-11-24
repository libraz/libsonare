/**
 * Frame-level spectral descriptors: the scalar shape measures taken per STFT
 * frame, and the zero-crossing counts beside them.
 */

import { withEntry } from './internal/entry.js';
import {
  sampleNeed,
  stftBins,
  stftFrames,
  withHeapNeed,
  withMonoHeap,
} from './internal/memory_budget.js';
import { getSonareModule } from './module_state.js';
import type { Matrix2D } from './public_types.js';
import type { OptionsOf } from './validation.js';
import {
  assertAudioInput,
  assertAudioSamples,
  assertPositiveFloatValue,
  requireBoolean,
  sampleOneShot,
} from './validation.js';

/** Spectral centroid request. */
export interface SpectralCentroidRequest {
  /** Mono audio samples. */
  samples: Float32Array;
  /** Sample rate of `samples` in Hz. */
  sampleRate: number;
  /** FFT size. Default 2048. */
  nFft?: number;
  /** Hop in samples. Default 512. */
  hopLength?: number;
}

/** Spectral bandwidth request. */
export interface SpectralBandwidthRequest {
  /** Mono audio samples. */
  samples: Float32Array;
  /** Sample rate of `samples` in Hz. */
  sampleRate: number;
  /** FFT size. Default 2048. */
  nFft?: number;
  /** Hop in samples. Default 512. */
  hopLength?: number;
  /** Norm exponent. Default 2. */
  p?: number;
}

/** Spectral rolloff request. */
export interface SpectralRolloffRequest {
  /** Mono audio samples. */
  samples: Float32Array;
  /** Sample rate of `samples` in Hz. */
  sampleRate: number;
  /** FFT size. Default 2048. */
  nFft?: number;
  /** Hop in samples. Default 512. */
  hopLength?: number;
  /** Energy fraction below the rolloff frequency. Default 0.85. */
  rollPercent?: number;
}

/** Spectral flatness request. */
export interface SpectralFlatnessRequest {
  /** Mono audio samples. */
  samples: Float32Array;
  /** Sample rate of `samples` in Hz. */
  sampleRate: number;
  /** FFT size. Default 2048. */
  nFft?: number;
  /** Hop in samples. Default 512. */
  hopLength?: number;
}

/** Spectral flux request. */
export interface SpectralFluxRequest {
  /** Mono audio samples. */
  samples: Float32Array;
  /** Sample rate of `samples` in Hz. */
  sampleRate: number;
  /** FFT size. Default 2048. */
  nFft?: number;
  /** Hop in samples. Default 512. */
  hopLength?: number;
  /** Frame lag of the difference. Default 1. */
  lag?: number;
}

/** Spectral contrast request. */
export interface SpectralContrastRequest {
  /** Mono audio samples. */
  samples: Float32Array;
  /** Sample rate of `samples` in Hz. */
  sampleRate: number;
  /** FFT size. Default 2048. */
  nFft?: number;
  /** Hop in samples. Default 512. */
  hopLength?: number;
  /** Contrast bands above the lowest. Default 6. */
  nBands?: number;
  /** Upper edge of the lowest band, in Hz. Default 200. */
  fminHz?: number;
  /** Quantile separating peaks from valleys. Default 0.02. */
  quantile?: number;
}

/** Polynomial feature request. */
export interface PolyFeaturesRequest {
  /** Mono audio samples. */
  samples: Float32Array;
  /** Sample rate of `samples` in Hz. */
  sampleRate: number;
  /** FFT size. Default 2048. */
  nFft?: number;
  /** Hop in samples. Default 512. */
  hopLength?: number;
  /** Polynomial order. Default 1. */
  order?: number;
}

/** Zero-crossing rate request. */
export interface ZeroCrossingRateRequest {
  /** Mono audio samples. */
  samples: Float32Array;
  /** Sample rate of `samples` in Hz. */
  sampleRate: number;
  /** Frame length in samples. Default 2048. */
  frameLength?: number;
  /** Hop in samples. Default 512. */
  hopLength?: number;
}

/** RMS energy request. */
export interface RmsEnergyRequest {
  /** Mono audio samples. */
  samples: Float32Array;
  /** Sample rate of `samples` in Hz. */
  sampleRate: number;
  /** Frame length in samples. Default 2048. */
  frameLength?: number;
  /** Hop in samples. Default 512. */
  hopLength?: number;
}

/** Zero-crossing index request. The rate is validated but does not change the result. */
export interface ZeroCrossingsRequest {
  /** Mono audio samples. */
  samples: Float32Array;
  /** Audio sample rate in Hz, an integer in [8000, 384000]; required for the shared call shape, not read by the computation. */
  sampleRate: number;
  /** Magnitude below which a sample counts as zero. Default 1e-10. */
  threshold?: number;
  /** Scale `threshold` by the maximum absolute sample. Default `false`. */
  refMagnitude?: boolean;
  /** Count a crossing at index 0. Default `true`. */
  pad?: boolean;
  /** Treat the sign of zero as positive. Default `true`. */
  zeroPos?: boolean;
}

// ============================================================================
// Features - Spectral
// ============================================================================

/** Frames a time-domain framing of `n` samples certainly yields. */
function centeredFrames(n: number, hop: number): number {
  return n > 0 ? 1 + Math.floor((n - 1) / hop) : 0;
}

/**
 * Runs an STFT-backed descriptor under the heap guard: the heap copy of the input, the complex
 * spectrogram held beside the result and `rows` floats of result per frame. A size or hop outside
 * the core's domain skips the guard so the native refusal stays the one that speaks.
 */
function guardedStft<T>(
  samples: Float32Array,
  sampleRate: number,
  nFft: unknown,
  hop: unknown,
  rows: number,
  call: () => T,
): T {
  const bins = stftBins(nFft, hop);
  if (bins === undefined) {
    return call();
  }
  return withHeapNeed(
    sampleNeed(
      'samples',
      samples.length,
      sampleRate,
      (n) => 4 * n + (8 * bins + 4 * rows) * stftFrames(n, hop as number),
    ),
    call,
  );
}

/** Same guard for a time-domain framing: the input copy and `rows` floats of result per frame. */
function guardedFraming<T>(
  samples: Float32Array,
  sampleRate: number,
  hop: unknown,
  call: () => T,
): T {
  if (typeof hop !== 'number' || !Number.isInteger(hop) || hop < 1) {
    return call();
  }
  return withHeapNeed(
    sampleNeed('samples', samples.length, sampleRate, (n) => 4 * n + 4 * centeredFrames(n, hop)),
    call,
  );
}

/**
 * Spectral centroid (center of mass of the spectrum) in Hz, one value per frame.
 */
export function spectralCentroid(request: SpectralCentroidRequest): Float32Array;
export function spectralCentroid(
  samples: Float32Array,
  sampleRate: number,
  options?: OptionsOf<SpectralCentroidRequest>,
): Float32Array;
export function spectralCentroid(...args: unknown[]): Float32Array {
  return withEntry('spectralCentroid', () => {
    const r = sampleOneShot<SpectralCentroidRequest>('spectralCentroid', args);
    assertAudioInput(r.samples, r.sampleRate, r);
    const nFft = r.nFft ?? 2048;
    const hopLength = r.hopLength ?? 512;
    return guardedStft(r.samples, r.sampleRate, nFft, hopLength, 1, () =>
      getSonareModule().spectralCentroid(r.samples, r.sampleRate, nFft, hopLength),
    );
  });
}

/**
 * Compute spectral contrast (librosa.feature.spectral_contrast).
 *
 * @remarks
 * Band 0 spans `[0, fminHz]`, so an `fminHz` below one analysis bin
 * (`sampleRate / nFft`) leaves it empty after the band trim. Row 0 is still
 * finite, but comes from the other bands' extremes rather than from itself, and
 * is neither level-invariant nor confined to the band. Keep `fminHz` at or above
 * one bin width for row 0 to mean anything — at the defaults (22050 Hz, 2048)
 * one bin is 10.8 Hz, so only a small `nFft` or a tiny `fminHz` reaches this.
 *
 * @returns {@link Matrix2D} whose rows are the `nBands + 1` contrast bands and cols are frames.
 */
export function spectralContrast(request: SpectralContrastRequest): Matrix2D;
export function spectralContrast(
  samples: Float32Array,
  sampleRate: number,
  options?: OptionsOf<SpectralContrastRequest>,
): Matrix2D;
export function spectralContrast(...args: unknown[]): Matrix2D {
  return withEntry('spectralContrast', () => {
    const r = sampleOneShot<SpectralContrastRequest>('spectralContrast', args);
    assertAudioInput(r.samples, r.sampleRate, r);
    const fminHz = r.fminHz ?? 200.0;
    assertPositiveFloatValue(fminHz, 'fminHz');
    const nFft = r.nFft ?? 2048;
    const hopLength = r.hopLength ?? 512;
    const nBands = r.nBands ?? 6;
    const call = () =>
      getSonareModule().spectralContrast(
        r.samples,
        r.sampleRate,
        nFft,
        hopLength,
        nBands,
        fminHz,
        r.quantile ?? 0.02,
      );
    if (typeof nBands !== 'number' || !Number.isInteger(nBands) || nBands < 1) {
      return call();
    }
    return guardedStft(r.samples, r.sampleRate, nFft, hopLength, nBands + 1, call);
  });
}

/**
 * Fit per-frame polynomial coefficients (librosa.feature.poly_features).
 *
 * @returns {@link Matrix2D} whose rows are the `order + 1` polynomial coefficients and cols are frames.
 */
export function polyFeatures(request: PolyFeaturesRequest): Matrix2D;
export function polyFeatures(
  samples: Float32Array,
  sampleRate: number,
  options?: OptionsOf<PolyFeaturesRequest>,
): Matrix2D;
export function polyFeatures(...args: unknown[]): Matrix2D {
  return withEntry('polyFeatures', () => {
    const r = sampleOneShot<PolyFeaturesRequest>('polyFeatures', args);
    assertAudioInput(r.samples, r.sampleRate, r);
    const nFft = r.nFft ?? 2048;
    const hopLength = r.hopLength ?? 512;
    const order = r.order ?? 1;
    const call = () =>
      getSonareModule().polyFeatures(r.samples, r.sampleRate, nFft, hopLength, order);
    if (typeof order !== 'number' || !Number.isInteger(order) || order < 0) {
      return call();
    }
    return guardedStft(r.samples, r.sampleRate, nFft, hopLength, order + 1, call);
  });
}

/**
 * Locate zero-crossing indices of a signal (librosa.zero_crossings).
 * `sampleRate` is validated as an audio sample rate but not read.
 */
export function zeroCrossings(request: ZeroCrossingsRequest): Int32Array;
export function zeroCrossings(
  samples: Float32Array,
  sampleRate: number,
  options?: OptionsOf<ZeroCrossingsRequest>,
): Int32Array;
export function zeroCrossings(...args: unknown[]): Int32Array {
  return withEntry('zeroCrossings', () => {
    const r = sampleOneShot<ZeroCrossingsRequest>('zeroCrossings', args);
    assertAudioSamples(r.samples, r, 'samples', true);
    const refMagnitude = requireBoolean(r.refMagnitude ?? false, 'refMagnitude');
    const pad = requireBoolean(r.pad ?? true, 'pad');
    const zeroPos = requireBoolean(r.zeroPos ?? true, 'zeroPos');
    // The index list can be as long as the input, but only an empty one is certain.
    return withMonoHeap(r.samples, undefined, 1, () =>
      getSonareModule().zeroCrossings(r.samples, r.threshold ?? 1e-10, refMagnitude, pad, zeroPos),
    );
  });
}

/**
 * Spectral bandwidth in Hz, one value per frame.
 */
export function spectralBandwidth(request: SpectralBandwidthRequest): Float32Array;
export function spectralBandwidth(
  samples: Float32Array,
  sampleRate: number,
  options?: OptionsOf<SpectralBandwidthRequest>,
): Float32Array;
export function spectralBandwidth(...args: unknown[]): Float32Array {
  return withEntry('spectralBandwidth', () => {
    const r = sampleOneShot<SpectralBandwidthRequest>('spectralBandwidth', args);
    assertAudioInput(r.samples, r.sampleRate, r);
    const nFft = r.nFft ?? 2048;
    const hopLength = r.hopLength ?? 512;
    return guardedStft(r.samples, r.sampleRate, nFft, hopLength, 1, () =>
      getSonareModule().spectralBandwidth(r.samples, r.sampleRate, nFft, hopLength, r.p ?? 2),
    );
  });
}

/**
 * Spectral rolloff frequency in Hz, one value per frame.
 */
export function spectralRolloff(request: SpectralRolloffRequest): Float32Array;
export function spectralRolloff(
  samples: Float32Array,
  sampleRate: number,
  options?: OptionsOf<SpectralRolloffRequest>,
): Float32Array;
export function spectralRolloff(...args: unknown[]): Float32Array {
  return withEntry('spectralRolloff', () => {
    const r = sampleOneShot<SpectralRolloffRequest>('spectralRolloff', args);
    assertAudioInput(r.samples, r.sampleRate, r);
    const nFft = r.nFft ?? 2048;
    const hopLength = r.hopLength ?? 512;
    return guardedStft(r.samples, r.sampleRate, nFft, hopLength, 1, () =>
      getSonareModule().spectralRolloff(
        r.samples,
        r.sampleRate,
        nFft,
        hopLength,
        r.rollPercent ?? 0.85,
      ),
    );
  });
}

/**
 * Spectral flatness per frame (0 = tonal, 1 = noise-like).
 */
export function spectralFlatness(request: SpectralFlatnessRequest): Float32Array;
export function spectralFlatness(
  samples: Float32Array,
  sampleRate: number,
  options?: OptionsOf<SpectralFlatnessRequest>,
): Float32Array;
export function spectralFlatness(...args: unknown[]): Float32Array {
  return withEntry('spectralFlatness', () => {
    const r = sampleOneShot<SpectralFlatnessRequest>('spectralFlatness', args);
    assertAudioInput(r.samples, r.sampleRate, r);
    const nFft = r.nFft ?? 2048;
    const hopLength = r.hopLength ?? 512;
    return guardedStft(r.samples, r.sampleRate, nFft, hopLength, 1, () =>
      getSonareModule().spectralFlatness(r.samples, r.sampleRate, nFft, hopLength),
    );
  });
}

/**
 * Spectral flux per frame.
 */
export function spectralFlux(request: SpectralFluxRequest): Float32Array;
export function spectralFlux(
  samples: Float32Array,
  sampleRate: number,
  options?: OptionsOf<SpectralFluxRequest>,
): Float32Array;
export function spectralFlux(...args: unknown[]): Float32Array {
  return withEntry('spectralFlux', () => {
    const r = sampleOneShot<SpectralFluxRequest>('spectralFlux', args);
    assertAudioInput(r.samples, r.sampleRate, r);
    const nFft = r.nFft ?? 2048;
    const hopLength = r.hopLength ?? 512;
    return guardedStft(r.samples, r.sampleRate, nFft, hopLength, 1, () =>
      getSonareModule().spectralFlux(r.samples, r.sampleRate, nFft, hopLength, r.lag ?? 1),
    );
  });
}

/**
 * Zero-crossing rate per frame.
 */
export function zeroCrossingRate(request: ZeroCrossingRateRequest): Float32Array;
export function zeroCrossingRate(
  samples: Float32Array,
  sampleRate: number,
  options?: OptionsOf<ZeroCrossingRateRequest>,
): Float32Array;
export function zeroCrossingRate(...args: unknown[]): Float32Array {
  return withEntry('zeroCrossingRate', () => {
    const r = sampleOneShot<ZeroCrossingRateRequest>('zeroCrossingRate', args);
    assertAudioInput(r.samples, r.sampleRate, r);
    const hopLength = r.hopLength ?? 512;
    return guardedFraming(r.samples, r.sampleRate, hopLength, () =>
      getSonareModule().zeroCrossingRate(r.samples, r.sampleRate, r.frameLength ?? 2048, hopLength),
    );
  });
}

/**
 * RMS energy per frame.
 */
export function rmsEnergy(request: RmsEnergyRequest): Float32Array;
export function rmsEnergy(
  samples: Float32Array,
  sampleRate: number,
  options?: OptionsOf<RmsEnergyRequest>,
): Float32Array;
export function rmsEnergy(...args: unknown[]): Float32Array {
  return withEntry('rmsEnergy', () => {
    const r = sampleOneShot<RmsEnergyRequest>('rmsEnergy', args);
    assertAudioInput(r.samples, r.sampleRate, r);
    const hopLength = r.hopLength ?? 512;
    return guardedFraming(r.samples, r.sampleRate, hopLength, () =>
      getSonareModule().rmsEnergy(r.samples, r.sampleRate, r.frameLength ?? 2048, hopLength),
    );
  });
}
