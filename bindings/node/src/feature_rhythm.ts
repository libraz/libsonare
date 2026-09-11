import type { MelSpectrogramRequest } from './feature_spectral.js';
import type { ValuesRequest } from './feature_units.js';
import { addon } from './native.js';
import type { TempogramMode } from './types.js';

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
  return addon.tempogram(
    request.onsetEnvelope,
    request.sampleRate ?? 22050,
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
  return addon.cyclicTempogram(
    request.onsetEnvelope,
    request.sampleRate ?? 22050,
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
  return addon.plp(
    request.onsetEnvelope,
    request.sampleRate ?? 22050,
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
  return addon.onsetEnvelope(
    request.samples,
    request.sampleRate ?? 22050,
    request.nFft ?? 2048,
    request.hopLength ?? 512,
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
  return addon.onsetStrengthMulti(
    request.samples,
    request.sampleRate ?? 22050,
    request.nFft ?? 2048,
    request.hopLength ?? 512,
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
  return addon.fourierTempogram(
    request.onsetEnvelope,
    request.sampleRate ?? 22050,
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
  return addon.tempogramRatio(
    request.tempogramData,
    request.winLength ?? 384,
    request.sampleRate ?? 22050,
    request.hopLength ?? 512,
    request.factors,
  );
}
