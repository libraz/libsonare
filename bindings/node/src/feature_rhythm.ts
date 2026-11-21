import { withEntry } from './internal/entry.js';
import { assertMatrix2D, toMatrix2D, toNativeIndexList } from './internal/feature_matrix.js';
import { resolveFftOptions } from './internal/fft_options.js';
import { addon } from './native.js';
import type { Matrix2D, TempogramMode } from './types.js';
import type { OptionsOf } from './validation.js';
import {
  assertAudioInput,
  assertFiniteSamples,
  assertFloat32Array,
  assertFloatValue,
  assertNonNegativeSafeInteger,
  assertSampleRate,
  requestOnly,
  sampleOneShot,
} from './validation.js';

/** Request for {@link onsetBacktrack}. */
export interface OnsetBacktrackRequest {
  /** Onset frame indices. */
  events: Int32Array | ArrayLike<number>;
  /** Energy envelope the events are rolled back along. */
  energy: Float32Array;
}

/** Request for {@link peakPick}. */
export interface PeakPickRequest {
  /** Signal to pick peaks from, usually an onset envelope. */
  values: Float32Array;
  /** Frames before a candidate that its local maximum is taken over; zero or more. */
  preMax: number;
  /** Frames after a candidate that its local maximum is taken over; zero or more. */
  postMax: number;
  /** Frames before a candidate that its running mean is taken over; zero or more. */
  preAvg: number;
  /** Frames after a candidate that its running mean is taken over; zero or more. */
  postAvg: number;
  /** Offset added to the running mean, in the units of `values`. */
  delta: number;
  /** Frames to skip after a picked peak before another may be picked; zero or more. */
  wait: number;
}

/** Request for {@link tempogram}. */
export interface TempogramRequest {
  /** Onset strength envelope, one value per frame. */
  onsetEnvelope: Float32Array;
  /** Default 22050. */
  sampleRate?: number;
  /** Default 512. */
  hopLength?: number;
  /** Autocorrelation window in frames. Default 384. */
  winLength?: number;
  /** Default `'autocorrelation'`. */
  mode?: TempogramMode;
  /** Centre the windows. Default true. */
  center?: boolean;
  /** Normalise each column. Default true. */
  norm?: boolean;
}

/** Request for {@link cyclicTempogram}. */
export interface CyclicTempogramRequest {
  /** Onset strength envelope, one value per frame. */
  onsetEnvelope: Float32Array;
  /** Sample rate of the audio the envelope came from, in Hz. */
  sampleRate: number;
  /** Hop length in samples between envelope frames. Default `512`. */
  hopLength?: number;
  /** Autocorrelation window in frames. Default `384`. */
  winLength?: number;
  /** Base of the folded tempo octave, in BPM; not a search range. Default `60`. */
  bpmMin?: number;
  /** Tempo bins per octave. Default 60. */
  nBins?: number;
}

/** Request for {@link fourierTempogram}. */
export interface FourierTempogramRequest {
  /** Onset strength envelope, one value per frame. */
  onsetEnvelope: Float32Array;
  /** Sample rate of the audio the envelope came from, in Hz. Default `22050`. */
  sampleRate?: number;
  /** Hop length in samples between envelope frames. Default `512`. */
  hopLength?: number;
  /** Analysis window in frames. Default `384`. */
  winLength?: number;
  /** Centre the windows. Default `true`. */
  center?: boolean;
}

/**
 * Request for {@link plp}. The tempo search runs over 60-200 BPM unless
 * `bpmMin` / `bpmMax` say otherwise; that default range differs from librosa's.
 */
export interface PlpRequest {
  /** Onset strength envelope, one value per frame. */
  onsetEnvelope: Float32Array;
  /** Sample rate of the audio the envelope came from, in Hz. Default `22050`. */
  sampleRate?: number;
  /** Hop length in samples between envelope frames. Default `512`. */
  hopLength?: number;
  /** Lowest tempo searched, in BPM. Default 60. */
  bpmMin?: number;
  /** Highest tempo searched, in BPM. Default 200. */
  bpmMax?: number;
  /** Analysis window in frames. Default `384`. */
  winLength?: number;
}

/** Request for {@link onsetStrength}. */
export interface OnsetStrengthRequest {
  /** Mono audio samples. */
  samples: Float32Array;
  /** Sample rate of `samples` in Hz. */
  sampleRate: number;
  /** FFT size in samples. Default `2048`. */
  nFft?: number;
  /** Hop length in samples. Default `512`. */
  hopLength?: number;
  /** Mel bands the flux is taken over. Default 128. */
  nMels?: number;
}

/** Request for {@link onsetStrengthMulti}. */
export interface OnsetStrengthMultiRequest {
  /** Mono audio samples. */
  samples: Float32Array;
  /** Sample rate of `samples` in Hz. */
  sampleRate: number;
  /** Frequency bands. Default 3. */
  nBands?: number;
  /** FFT size in samples. Default `2048`. */
  nFft?: number;
  /** Hop length in samples. Default `512`. */
  hopLength?: number;
  /** Mel bands the flux is taken over. Default `128`. */
  nMels?: number;
}

/** Request for {@link tempogramRatio}. */
export interface TempogramRatioRequest {
  /** Autocorrelation tempogram: rows = tempogram lags, cols = frames. */
  tempogram: Matrix2D;
  /** Sample rate of the audio the tempogram came from, in Hz. Default `22050`. */
  sampleRate?: number;
  /** Hop length in samples between frames. Default `512`. */
  hopLength?: number;
  /**
   * Tempo multiples the ratio is taken against; omit for the defaults. Every
   * entry must be a finite number. A plain number array is checked entry by
   * entry and the refusal names the index; a `Float32Array` is not, because JS
   * folded an out-of-range entry to an infinity before the call.
   */
  factors?: Float32Array | number[];
}

/** Rolls onset events back to the preceding energy minimum. Request-only. */
export function onsetBacktrack(request: OnsetBacktrackRequest): Int32Array;
export function onsetBacktrack(...args: unknown[]): Int32Array {
  return withEntry('onsetBacktrack', () => {
    const r = requestOnly<OnsetBacktrackRequest>('onsetBacktrack', args);
    assertFloat32Array(r.energy, 'energy');
    return addon.onsetBacktrack(toNativeIndexList(r.events, 'events'), r.energy);
  });
}

/**
 * Pick local peaks that rise above an adaptive threshold, mirroring
 * `librosa.util.peak_pick`. Returns the indices of the picked peaks.
 *
 * `values` is typically an onset envelope from {@link onsetStrength}.
 *
 * @param request.values Signal to pick peaks from, usually an onset envelope.
 * @param request.preMax Frames before a candidate that its local maximum is taken over.
 * @param request.postMax Frames after a candidate that its local maximum is taken over.
 * @param request.preAvg Frames before a candidate that its running mean is taken over.
 * @param request.postAvg Frames after a candidate that its running mean is taken over.
 * @param request.delta Absolute offset added to the running mean, **in the units of
 *   `values`** — not a normalized or dB quantity. This library does not
 *   normalize the onset envelope, so the usable range depends entirely on the
 *   magnitudes your own envelope happens to carry and cannot be derived from
 *   this signature. Read a starting value off your own data: run
 *   {@link onsetStrength} on representative audio and take a small fraction of
 *   the envelope's mean or median, then adjust.
 * @param request.wait Frames to skip after picking a peak before another may be picked.
 *
 * @example
 * ```ts
 * const envelope = onsetStrength(samples, 22050);
 * const mean = envelope.reduce((a, b) => a + b, 0) / envelope.length;
 * // Start from the envelope's own scale rather than a guessed constant.
 * const peaks = peakPick({
 *   values: envelope,
 *   preMax: 3,
 *   postMax: 3,
 *   preAvg: 3,
 *   postAvg: 5,
 *   delta: mean * 0.1,
 *   wait: 10,
 * });
 * ```
 */
export function peakPick(request: PeakPickRequest): Int32Array;
export function peakPick(...args: unknown[]): Int32Array {
  return withEntry('peakPick', () => {
    const r = requestOnly<PeakPickRequest>('peakPick', args);
    assertFloat32Array(r.values, 'values');
    // Each is a frame count that may legitimately be zero.
    assertNonNegativeSafeInteger(r.preMax, 'preMax');
    assertNonNegativeSafeInteger(r.postMax, 'postMax');
    assertNonNegativeSafeInteger(r.preAvg, 'preAvg');
    assertNonNegativeSafeInteger(r.postAvg, 'postAvg');
    assertNonNegativeSafeInteger(r.wait, 'wait');
    assertFloatValue(r.delta, 'delta');
    return addon.peakPick(r.values, r.preMax, r.postMax, r.preAvg, r.postAvg, r.delta, r.wait);
  });
}

/** Autocorrelation or cosine tempogram. Returns rows = `winLength` lags, cols = frames. */
export function tempogram(request: TempogramRequest): Matrix2D;
export function tempogram(...args: unknown[]): Matrix2D {
  return withEntry('tempogram', () => {
    const r = requestOnly<TempogramRequest>('tempogram', args);
    assertFloat32Array(r.onsetEnvelope, 'onsetEnvelope');
    const sampleRate = r.sampleRate ?? 22050;
    assertSampleRate(sampleRate);
    const out = addon.tempogram(
      r.onsetEnvelope,
      sampleRate,
      r.hopLength ?? 512,
      r.winLength ?? 384,
      r.mode ?? 'autocorrelation',
      r.center ?? true,
      r.norm ?? true,
    );
    return toMatrix2D(out.data, out.winLength, out.nFrames);
  });
}

/** Tempogram folded onto one tempo octave. Returns rows = `nBins`, cols = frames. */
export function cyclicTempogram(request: CyclicTempogramRequest): Matrix2D;
export function cyclicTempogram(...args: unknown[]): Matrix2D {
  return withEntry('cyclicTempogram', () => {
    const r = requestOnly<CyclicTempogramRequest>('cyclicTempogram', args);
    assertFloat32Array(r.onsetEnvelope, 'onsetEnvelope');
    const sampleRate = r.sampleRate;
    assertSampleRate(sampleRate);
    const out = addon.cyclicTempogram(
      r.onsetEnvelope,
      sampleRate,
      r.hopLength ?? 512,
      r.winLength ?? 384,
      r.bpmMin,
      r.nBins,
    );
    return toMatrix2D(out.data, out.nBins, out.nFrames);
  });
}

/** Predominant local pulse curve, one value per onset-envelope frame. */
export function plp(request: PlpRequest): Float32Array;
export function plp(...args: unknown[]): Float32Array {
  return withEntry('plp', () => {
    const r = requestOnly<PlpRequest>('plp', args);
    assertFloat32Array(r.onsetEnvelope, 'onsetEnvelope');
    const sampleRate = r.sampleRate ?? 22050;
    assertSampleRate(sampleRate);
    return addon.plp(
      r.onsetEnvelope,
      sampleRate,
      r.hopLength ?? 512,
      r.bpmMin,
      r.bpmMax,
      r.winLength ?? 384,
    );
  });
}

/** Onset strength envelope, one value per frame. */
export function onsetStrength(request: OnsetStrengthRequest): Float32Array;
export function onsetStrength(
  samples: Float32Array,
  sampleRate: number,
  options?: OptionsOf<OnsetStrengthRequest>,
): Float32Array;
export function onsetStrength(...args: unknown[]): Float32Array {
  return withEntry('onsetStrength', () => {
    const request = sampleOneShot<OnsetStrengthRequest>('onsetStrength', args);
    assertAudioInput(request.samples, request.sampleRate, request);
    const fft = resolveFftOptions(request.nFft, request.hopLength);
    return addon.onsetEnvelope(
      request.samples,
      request.sampleRate,
      fft.nFft,
      fft.hopLength,
      request.nMels ?? 128,
    );
  });
}

/** Per-band onset strength. Returns rows = bands, cols = frames. */
export function onsetStrengthMulti(request: OnsetStrengthMultiRequest): Matrix2D;
export function onsetStrengthMulti(
  samples: Float32Array,
  sampleRate: number,
  options?: OptionsOf<OnsetStrengthMultiRequest>,
): Matrix2D;
export function onsetStrengthMulti(...args: unknown[]): Matrix2D {
  return withEntry('onsetStrengthMulti', () => {
    const request = sampleOneShot<OnsetStrengthMultiRequest>('onsetStrengthMulti', args);
    assertAudioInput(request.samples, request.sampleRate, request);
    const fft = resolveFftOptions(request.nFft, request.hopLength);
    const out = addon.onsetStrengthMulti(
      request.samples,
      request.sampleRate,
      fft.nFft,
      fft.hopLength,
      request.nMels ?? 128,
      request.nBands ?? 3,
    );
    return toMatrix2D(out.data, out.nBands, out.nFrames);
  });
}

/** Fourier tempogram. Returns rows = tempo bins, cols = frames. */
export function fourierTempogram(request: FourierTempogramRequest): Matrix2D;
export function fourierTempogram(...args: unknown[]): Matrix2D {
  return withEntry('fourierTempogram', () => {
    const r = requestOnly<FourierTempogramRequest>('fourierTempogram', args);
    assertFloat32Array(r.onsetEnvelope, 'onsetEnvelope');
    const sampleRate = r.sampleRate ?? 22050;
    assertSampleRate(sampleRate);
    assertFiniteSamples(r.onsetEnvelope, true, 'onsetEnvelope');
    const out = addon.fourierTempogram(
      r.onsetEnvelope,
      sampleRate,
      r.hopLength ?? 512,
      r.winLength ?? 384,
      r.center ?? true,
      true,
    );
    return toMatrix2D(out.data, out.nBins, out.nFrames);
  });
}

/** Tempo-ratio features of an autocorrelation tempogram. Returns one value per frame. */
export function tempogramRatio(request: TempogramRatioRequest): Float32Array;
export function tempogramRatio(...args: unknown[]): Float32Array {
  return withEntry('tempogramRatio', () => {
    const r = requestOnly<TempogramRatioRequest>('tempogramRatio', args);
    const sampleRate = r.sampleRate ?? 22050;
    assertSampleRate(sampleRate);
    assertMatrix2D(r.tempogram, 'tempogram');
    return addon.tempogramRatio(
      r.tempogram.data,
      r.tempogram.rows,
      sampleRate,
      r.hopLength ?? 512,
      r.factors,
    );
  });
}
