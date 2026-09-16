import { resolveFftOptions } from './_fft_options.js';
import type { MelSpectrogramRequest } from './feature_spectral.js';
import type { ValuesRequest } from './feature_units.js';
import { addon } from './native.js';
import type { TempogramMode } from './types.js';
import {
  assertNonNegativeSafeInteger,
  assertPositiveInteger,
  assertSampleRate,
} from './validation.js';

export interface OnsetBacktrackRequest {
  events: Int32Array | number[];
  energy: Float32Array;
}

export interface TempogramRequest {
  onsetEnvelope: Float32Array;
  sampleRate?: number;
  hopLength?: number;
  winLength?: number;
  mode?: TempogramMode;
  center?: boolean;
  norm?: boolean;
}
export interface CyclicTempogramRequest extends TempogramRequest {
  bpmMin?: number;
  nBins?: number;
}
export interface PlpRequest {
  onsetEnvelope: Float32Array;
  sampleRate?: number;
  hopLength?: number;
  tempoMin?: number;
  tempoMax?: number;
  winLength?: number;
}
export interface OnsetEnvelopeRequest extends MelSpectrogramRequest {}
export interface OnsetStrengthMultiRequest extends MelSpectrogramRequest {
  nBands?: number;
}

export interface TempogramRatioRequest {
  tempogramData: Float32Array;
  winLength?: number;
  sampleRate?: number;
  hopLength?: number;
  /**
   * Tempo multiples the ratio is taken against; omit for the defaults. Every
   * entry must be a finite number. A plain number array is checked entry by
   * entry and the refusal names the index; a `Float32Array` is not, because JS
   * folded an out-of-range entry to an infinity before the call.
   */
  factors?: Float32Array | number[];
}

export function onsetBacktrack(request: OnsetBacktrackRequest): Int32Array;
export function onsetBacktrack(events: Int32Array | number[], energy: Float32Array): Int32Array;
export function onsetBacktrack(
  events: Int32Array | number[] | OnsetBacktrackRequest,
  energy?: Float32Array,
): Int32Array {
  const request =
    events instanceof Int32Array || Array.isArray(events) ? { events, energy } : events;
  return addon.onsetBacktrack(request.events, request.energy);
}

/**
 * Pick local peaks that rise above an adaptive threshold, mirroring
 * `librosa.util.peak_pick`. Returns the indices of the picked peaks.
 *
 * `values` is typically an onset envelope from {@link onsetEnvelope}.
 *
 * @param values Signal to pick peaks from, usually an onset envelope.
 * @param preMax Frames before a candidate that its local maximum is taken over.
 * @param postMax Frames after a candidate that its local maximum is taken over.
 * @param preAvg Frames before a candidate that its running mean is taken over.
 * @param postAvg Frames after a candidate that its running mean is taken over.
 * @param delta Absolute offset added to the running mean, **in the units of
 *   `values`** — not a normalized or dB quantity. This library does not
 *   normalize the onset envelope, so the usable range depends entirely on the
 *   magnitudes your own envelope happens to carry and cannot be derived from
 *   this signature. Read a starting value off your own data: run
 *   {@link onsetEnvelope} on representative audio and take a small fraction of
 *   the envelope's mean or median, then adjust.
 * @param wait Frames to skip after picking a peak before another may be picked.
 *
 * @example
 * ```ts
 * const envelope = onsetEnvelope(samples, 22050);
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
export function peakPick(
  request: ValuesRequest & {
    preMax: number;
    postMax: number;
    preAvg: number;
    postAvg: number;
    delta: number;
    wait: number;
  },
): Int32Array;
export function peakPick(
  values: Float32Array,
  preMax: number,
  postMax: number,
  preAvg: number,
  postAvg: number,
  delta: number,
  wait: number,
): Int32Array;
export function peakPick(
  values:
    | Float32Array
    | (ValuesRequest & {
        preMax: number;
        postMax: number;
        preAvg: number;
        postAvg: number;
        delta: number;
        wait: number;
      }),
  preMax = 0,
  postMax = 1,
  preAvg = 0,
  postAvg = 1,
  delta = 0,
  wait = 0,
): Int32Array {
  const request =
    values instanceof Float32Array
      ? { values, preMax, postMax, preAvg, postAvg, delta, wait }
      : values;
  // Each is a frame count that may legitimately be zero (see the defaults above).
  assertNonNegativeSafeInteger('peakPick', request.preMax, 'preMax');
  assertNonNegativeSafeInteger('peakPick', request.postMax, 'postMax');
  assertNonNegativeSafeInteger('peakPick', request.preAvg, 'preAvg');
  assertNonNegativeSafeInteger('peakPick', request.postAvg, 'postAvg');
  assertNonNegativeSafeInteger('peakPick', request.wait, 'wait');
  return addon.peakPick(
    request.values,
    request.preMax,
    request.postMax,
    request.preAvg,
    request.postAvg,
    request.delta,
    request.wait,
  );
}

export function tempogram(request: TempogramRequest): {
  nFrames: number;
  winLength: number;
  data: Float32Array;
};
export function tempogram(
  onsetEnvelope: Float32Array,
  sampleRate?: number,
  hopLength?: number,
  winLength?: number,
  mode?: TempogramMode,
  center?: boolean,
  norm?: boolean,
): { nFrames: number; winLength: number; data: Float32Array };
export function tempogram(
  onsetEnvelope: Float32Array | TempogramRequest,
  sampleRate = 22050,
  hopLength = 512,
  winLength = 384,
  mode: TempogramMode = 'autocorrelation',
  center = true,
  norm = true,
): { nFrames: number; winLength: number; data: Float32Array } {
  const request =
    onsetEnvelope instanceof Float32Array
      ? { onsetEnvelope, sampleRate, hopLength, winLength, mode, center, norm }
      : onsetEnvelope;
  const resolvedSampleRate = request.sampleRate ?? 22050;
  assertSampleRate('tempogram', resolvedSampleRate);
  assertPositiveInteger('tempogram', request.hopLength ?? 512, 'hopLength');
  assertPositiveInteger('tempogram', request.winLength ?? 384, 'winLength');
  return addon.tempogram(
    request.onsetEnvelope,
    resolvedSampleRate,
    request.hopLength ?? 512,
    request.winLength ?? 384,
    request.mode ?? 'autocorrelation',
    request.center ?? true,
    request.norm ?? true,
  );
}

export function cyclicTempogram(request: CyclicTempogramRequest): {
  nFrames: number;
  nBins: number;
  data: Float32Array;
};
export function cyclicTempogram(
  onsetEnvelope: Float32Array,
  sampleRate?: number,
  hopLength?: number,
  winLength?: number,
  center?: boolean,
  norm?: boolean,
  bpmMin?: number,
  nBins?: number,
): { nFrames: number; nBins: number; data: Float32Array };
export function cyclicTempogram(
  onsetEnvelope: Float32Array | CyclicTempogramRequest,
  sampleRate = 22050,
  hopLength = 512,
  winLength = 384,
  center = true,
  norm = true,
  bpmMin = 60.0,
  nBins = 60,
): { nFrames: number; nBins: number; data: Float32Array } {
  const request =
    onsetEnvelope instanceof Float32Array
      ? { onsetEnvelope, sampleRate, hopLength, winLength, center, norm, bpmMin, nBins }
      : onsetEnvelope;
  const resolvedSampleRate = request.sampleRate ?? 22050;
  assertSampleRate('cyclicTempogram', resolvedSampleRate);
  assertPositiveInteger('cyclicTempogram', request.hopLength ?? 512, 'hopLength');
  assertPositiveInteger('cyclicTempogram', request.winLength ?? 384, 'winLength');
  assertPositiveInteger('cyclicTempogram', request.nBins ?? 60, 'nBins');
  return addon.cyclicTempogram(
    request.onsetEnvelope,
    resolvedSampleRate,
    request.hopLength ?? 512,
    request.winLength ?? 384,
    request.bpmMin ?? 60,
    request.nBins ?? 60,
  );
}

export function plp(request: PlpRequest): Float32Array;
export function plp(
  onsetEnvelope: Float32Array,
  sampleRate?: number,
  hopLength?: number,
  tempoMin?: number,
  tempoMax?: number,
  winLength?: number,
): Float32Array;
export function plp(
  onsetEnvelope: Float32Array | PlpRequest,
  sampleRate = 22050,
  hopLength = 512,
  tempoMin = 30.0,
  tempoMax = 300.0,
  winLength = 384,
): Float32Array {
  const request =
    onsetEnvelope instanceof Float32Array
      ? { onsetEnvelope, sampleRate, hopLength, tempoMin, tempoMax, winLength }
      : onsetEnvelope;
  const resolvedSampleRate = request.sampleRate ?? 22050;
  assertSampleRate('plp', resolvedSampleRate);
  assertPositiveInteger('plp', request.hopLength ?? 512, 'hopLength');
  assertPositiveInteger('plp', request.winLength ?? 384, 'winLength');
  return addon.plp(
    request.onsetEnvelope,
    resolvedSampleRate,
    request.hopLength ?? 512,
    request.tempoMin ?? 30,
    request.tempoMax ?? 300,
    request.winLength ?? 384,
  );
}

export function onsetEnvelope(request: OnsetEnvelopeRequest): Float32Array;
export function onsetEnvelope(
  samples: Float32Array,
  sampleRate?: number,
  nFft?: number,
  hopLength?: number,
  nMels?: number,
): Float32Array;
export function onsetEnvelope(
  samples: Float32Array | OnsetEnvelopeRequest,
  sampleRate = 22050,
  nFft = 2048,
  hopLength = 512,
  nMels = 128,
): Float32Array {
  const request =
    samples instanceof Float32Array ? { samples, sampleRate, nFft, hopLength, nMels } : samples;
  const resolvedSampleRate = request.sampleRate ?? 22050;
  assertSampleRate('onsetEnvelope', resolvedSampleRate);
  const fft = resolveFftOptions('onsetEnvelope', request.nFft, request.hopLength);
  assertPositiveInteger('onsetEnvelope', request.nMels ?? 128, 'nMels');
  return addon.onsetEnvelope(
    request.samples,
    resolvedSampleRate,
    fft.nFft,
    fft.hopLength,
    request.nMels ?? 128,
  );
}

export function onsetStrengthMulti(request: OnsetStrengthMultiRequest): {
  nBands: number;
  nFrames: number;
  data: Float32Array;
};
export function onsetStrengthMulti(
  samples: Float32Array,
  sampleRate?: number,
  nFft?: number,
  hopLength?: number,
  nMels?: number,
  nBands?: number,
): { nBands: number; nFrames: number; data: Float32Array };
export function onsetStrengthMulti(
  samples: Float32Array | OnsetStrengthMultiRequest,
  sampleRate = 22050,
  nFft = 2048,
  hopLength = 512,
  nMels = 128,
  nBands = 3,
): { nBands: number; nFrames: number; data: Float32Array } {
  const request =
    samples instanceof Float32Array
      ? { samples, sampleRate, nFft, hopLength, nMels, nBands }
      : samples;
  const resolvedSampleRate = request.sampleRate ?? 22050;
  assertSampleRate('onsetStrengthMulti', resolvedSampleRate);
  const fft = resolveFftOptions('onsetStrengthMulti', request.nFft, request.hopLength);
  assertPositiveInteger('onsetStrengthMulti', request.nMels ?? 128, 'nMels');
  assertPositiveInteger('onsetStrengthMulti', request.nBands ?? 3, 'nBands');
  return addon.onsetStrengthMulti(
    request.samples,
    resolvedSampleRate,
    fft.nFft,
    fft.hopLength,
    request.nMels ?? 128,
    request.nBands ?? 3,
  );
}

export function fourierTempogram(request: TempogramRequest): {
  nBins: number;
  nFrames: number;
  data: Float32Array;
};
export function fourierTempogram(
  onsetEnvelope: Float32Array,
  sampleRate?: number,
  hopLength?: number,
  winLength?: number,
  center?: boolean,
  norm?: boolean,
): { nBins: number; nFrames: number; data: Float32Array };
export function fourierTempogram(
  onsetEnvelope: Float32Array | TempogramRequest,
  sampleRate = 22050,
  hopLength = 512,
  winLength = 384,
  center = true,
  norm = true,
): { nBins: number; nFrames: number; data: Float32Array } {
  const request =
    onsetEnvelope instanceof Float32Array
      ? { onsetEnvelope, sampleRate, hopLength, winLength, center, norm }
      : onsetEnvelope;
  const resolvedSampleRate = request.sampleRate ?? 22050;
  assertSampleRate('fourierTempogram', resolvedSampleRate);
  assertPositiveInteger('fourierTempogram', request.hopLength ?? 512, 'hopLength');
  assertPositiveInteger('fourierTempogram', request.winLength ?? 384, 'winLength');
  return addon.fourierTempogram(
    request.onsetEnvelope,
    resolvedSampleRate,
    request.hopLength ?? 512,
    request.winLength ?? 384,
    request.center ?? true,
    request.norm ?? true,
  );
}

export function tempogramRatio(request: TempogramRatioRequest): Float32Array;
export function tempogramRatio(
  tempogramData: Float32Array,
  winLength?: number,
  sampleRate?: number,
  hopLength?: number,
  factors?: Float32Array,
): Float32Array;
export function tempogramRatio(
  tempogramData: Float32Array | TempogramRatioRequest,
  winLength = 384,
  sampleRate = 22050,
  hopLength = 512,
  factors?: Float32Array | number[],
): Float32Array {
  const request =
    tempogramData instanceof Float32Array
      ? { tempogramData, winLength, sampleRate, hopLength, factors }
      : tempogramData;
  const resolvedSampleRate = request.sampleRate ?? 22050;
  assertSampleRate('tempogramRatio', resolvedSampleRate);
  assertPositiveInteger('tempogramRatio', request.winLength ?? 384, 'winLength');
  assertPositiveInteger('tempogramRatio', request.hopLength ?? 512, 'hopLength');
  return addon.tempogramRatio(
    request.tempogramData,
    request.winLength ?? 384,
    resolvedSampleRate,
    request.hopLength ?? 512,
    request.factors,
  );
}
