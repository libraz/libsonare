import { getSonareModule } from './module_state';
import type { TempogramMode } from './public_types';
import type {
  WasmCyclicTempogramResult,
  WasmFrameResult,
  WasmTempogramResult,
  WasmTrimResult,
} from './sonare.js';

function requireModule() {
  return getSonareModule();
}

export function tone(request?: ToneRequest): Float32Array;
export function tone(
  frequency?: number,
  sampleRate?: number,
  duration?: number,
  phase?: number,
  amplitude?: number,
): Float32Array;
export function tone(
  frequency: number | ToneRequest = 440,
  sampleRate = 22050,
  duration = 1,
  phase = 0,
  amplitude = 1,
): Float32Array {
  const request =
    typeof frequency === 'number'
      ? { frequency, sampleRate, duration, phase, amplitude }
      : frequency;
  return requireModule().tone(
    request.frequency ?? 440,
    request.sampleRate ?? 22050,
    request.duration ?? 1,
    request.phase ?? 0,
    request.amplitude ?? 1,
  );
}

export function chirp(request?: ChirpRequest): Float32Array;
export function chirp(
  fmin?: number,
  fmax?: number,
  sampleRate?: number,
  duration?: number,
  linear?: boolean,
): Float32Array;
export function chirp(
  fmin: number | ChirpRequest = 440,
  fmax = 880,
  sampleRate = 22050,
  duration = 1,
  linear = true,
): Float32Array {
  const request = typeof fmin === 'number' ? { fmin, fmax, sampleRate, duration, linear } : fmin;
  return requireModule().chirp(
    request.fmin ?? 440,
    request.fmax ?? 880,
    request.sampleRate ?? 22050,
    request.duration ?? 1,
    request.linear ?? true,
  );
}

export function clicks(request: ClicksRequest): Float32Array;
export function clicks(
  times: Float32Array,
  sampleRate?: number,
  length?: number,
  frequency?: number,
  clickDuration?: number,
): Float32Array;
export function clicks(
  times: Float32Array | ClicksRequest,
  sampleRate = 22050,
  length = 0,
  frequency = 1000,
  clickDuration = 0.1,
): Float32Array {
  const request =
    times instanceof Float32Array ? { times, sampleRate, length, frequency, clickDuration } : times;
  return requireModule().clicks(
    request.times,
    request.sampleRate ?? 22050,
    request.length ?? 0,
    request.frequency ?? 1000,
    request.clickDuration ?? 0.1,
  );
}

export interface DbConversionRequest {
  values: Float32Array;
  ref?: number;
  amin?: number;
  topDb?: number;
}

export interface SilenceRequest {
  samples: Float32Array;
  topDb?: number;
  frameLength?: number;
  hopLength?: number;
}

/** Canonical request form for the common-silence union of several signals. */
export interface SplitSilenceCommonRequest {
  signals: Float32Array[];
  topDb?: number;
  frameLength?: number;
  hopLength?: number;
}

/**
 * Why {@link splitSilenceCommonWithReport} found the gaps it did.
 *
 * One interval covering everything is the answer to three different situations
 * and the interval list cannot separate them: no take has a quiet moment at all,
 * the takes each have one but not in the same place, or `topDb` was set too loose
 * to see the ones they have.
 *
 * **Read {@link silenceCeilingDb} against the `topDb` that was passed**, which is
 * the whole decision:
 *
 * - ceiling near 0 — a take is sounding continuously. No threshold helps, and a
 *   cut point has to come from somewhere other than silence.
 * - ceiling below `topDb` — the threshold was too loose to see the quiet these
 *   takes do have. A `topDb` under the reported ceiling finds it.
 * - ceiling at or above `topDb`, and still one interval — every take shows silence
 *   at this setting and they do not share any of it. That is the alignment case,
 *   and it is what {@link alignTakeToReference} is for.
 *
 * The figures come from the same RMS pass the intervals do, so they can never
 * describe a different measurement.
 */
export interface SilenceCommonReport {
  /**
   * The largest `topDb` at which EVERY signal still shows silence.
   *
   * A frame counts as silent when it sits at least `topDb` under its own signal's
   * peak RMS, so each signal's deepest dip decides whether any threshold can find
   * silence in it, and the union needs all of them quiet at once — hence the
   * minimum across the signals. 0 for an all-silent signal, where the peak is 0
   * and the ratio has no value; 120 is the floor the dB conversion clamps at,
   * reported for a signal holding a zero-valued frame.
   */
  silenceCeilingDb: number;
  /**
   * How many intervals the most fragmented signal produced alone, counted before
   * the union merges anything.
   *
   * A measure of shape rather than of cause: 1 is a take that sounds once and
   * stops, so a take that is loud then silent counts 1 exactly as a take with no
   * silence does. Use {@link silenceCeilingDb} to tell those apart; use these two
   * to see whether any take has an interior gap at all (`>= 2`) and whether the
   * takes differ in how broken up they are (`maxSignalIntervals !==
   * minSignalIntervals`).
   */
  maxSignalIntervals: number;
  /** How many intervals the least fragmented signal produced alone. */
  minSignalIntervals: number;
}

/** Result of {@link splitSilenceCommonWithReport}. */
export interface SplitSilenceCommonWithReportResult {
  /** Exactly what {@link splitSilenceCommon} returns for the same arguments. */
  intervals: Int32Array;
  /** Why those are the intervals. */
  report: SilenceCommonReport;
}

export interface FrameSignalRequest {
  samples: Float32Array;
  frameLength: number;
  hopLength: number;
}
export interface ToneRequest {
  frequency?: number;
  sampleRate?: number;
  duration?: number;
  phase?: number;
  amplitude?: number;
}
export interface ChirpRequest {
  fmin?: number;
  fmax?: number;
  sampleRate?: number;
  duration?: number;
  linear?: boolean;
}
export interface ClicksRequest {
  times: Float32Array;
  sampleRate?: number;
  length?: number;
  frequency?: number;
  clickDuration?: number;
}
/** Canonical request form for pre/de-emphasis filters. */
export interface EmphasisRequest {
  samples: Float32Array;
  coef?: number;
  zi?: number;
}
/** Canonical request form for centering or extending a vector. */
export interface PadCenterRequest {
  values: Float32Array;
  targetSize: number;
  padValue?: number;
}
/** Canonical request form for resizing a vector. */
export interface FixLengthRequest {
  values: Float32Array;
  targetSize: number;
  padValue?: number;
}
/** Canonical request form for bounding a frame-index vector. */
export interface FixFramesRequest {
  frames: Int32Array;
  xMin?: number;
  xMax?: number;
  pad?: boolean;
}
export interface OnsetBacktrackRequest {
  events: Int32Array;
  energy: Float32Array;
}
/** Canonical request form for peak selection. */
export interface PeakPickRequest {
  values: Float32Array;
  preMax: number;
  postMax: number;
  preAvg: number;
  postAvg: number;
  delta: number;
  wait: number;
}
/** Canonical request form for vector normalization. */
export interface VectorNormalizeRequest {
  values: Float32Array;
  normType?: number;
  threshold?: number;
}
/** Canonical request form for tonal centroid projection. */
export interface TonnetzRequest {
  chromagram: Float32Array;
  nChroma: number;
  nFrames: number;
}
export interface PcenRequest {
  values: Float32Array;
  nBins: number;
  nFrames: number;
  sampleRate?: number;
  hopLength?: number;
  timeConstant?: number;
  gain?: number;
  bias?: number;
  power?: number;
  eps?: number;
  /** @deprecated Put PCEN fields directly on the request object. */
  options?: Record<string, number>;
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
export interface CyclicTempogramRequest {
  onsetEnvelope: Float32Array;
  sampleRate?: number;
  hopLength?: number;
  winLength?: number;
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

// ============================================================================
// Core - Unit Conversion
// ============================================================================

/**
 * Convert frequency in Hz to Mel scale.
 *
 * A total function, matching the C ABI and librosa: a non-finite `hz`
 * propagates rather than throwing, and a magnitude past the 32-bit float range
 * saturates to an infinity the same way the C conversion does. Out-of-audio
 * frequencies are not refused either — the mapping is defined over the whole
 * real line.
 *
 * @param hz - Frequency in Hz
 * @returns Mel frequency
 */
export function hzToMel(hz: number): number {
  return requireModule().hzToMel(hz);
}

/**
 * Convert Mel scale to frequency in Hz. Total over the same domain as
 * {@link hzToMel}.
 *
 * @param mel - Mel frequency
 * @returns Frequency in Hz
 */
export function melToHz(mel: number): number {
  return requireModule().melToHz(mel);
}

/**
 * Convert frequency in Hz to MIDI note number.
 *
 * Total, like {@link hzToMel}. A non-positive `hz` returns `-Infinity`, the log2
 * limit, and {@link midiToHz} maps that back to 0. A NaN propagates, which is
 * what makes a default `pitchPyin` track — whose unvoiced frames are NaN — safe
 * to map through.
 *
 * @param hz - Frequency in Hz
 * @returns MIDI note number (A4 = 440 Hz = 69)
 */
export function hzToMidi(hz: number): number {
  return requireModule().hzToMidi(hz);
}

/**
 * Convert MIDI note number to frequency in Hz. Total, like {@link hzToMel};
 * `-Infinity` bottoms out at 0 rather than propagating its sign.
 *
 * @param midi - MIDI note number
 * @returns Frequency in Hz
 */
export function midiToHz(midi: number): number {
  return requireModule().midiToHz(midi);
}

/**
 * Convert frequency in Hz to note name.
 *
 * Every frequency with no note answers `"?"` rather than throwing: zero,
 * negative, past the representable MIDI range, and non-finite alike. A default
 * `pitchPyin` track fills unvoiced frames with NaN, so mapping one through this
 * yields `"?"` at those frames.
 *
 * @param hz - Frequency in Hz
 * @returns Note name (e.g., "A4", "C#5")
 */
export function hzToNote(hz: number): string {
  return requireModule().hzToNote(hz);
}

/**
 * Convert note name to frequency in Hz.
 *
 * @param note - Note name (e.g., "A4", "C#5")
 * @returns Frequency in Hz
 */
export function noteToHz(note: string): number {
  return requireModule().noteToHz(note);
}

/**
 * Convert frame index to time in seconds.
 *
 * @param frames - Frame index
 * @param sr - Sample rate in Hz (default: 22050)
 * @param hopLength - Hop length in samples (default: 512)
 * @returns Time in seconds
 */
export function framesToTime(frames: number, sr = 22050, hopLength = 512): number {
  return requireModule().framesToTime(frames, sr, hopLength);
}

/**
 * Convert time in seconds to frame index.
 *
 * @param time - Time in seconds
 * @param sr - Sample rate in Hz (default: 22050)
 * @param hopLength - Hop length in samples (default: 512)
 * @returns Frame index
 */
export function timeToFrames(time: number, sr = 22050, hopLength = 512): number {
  return requireModule().timeToFrames(time, sr, hopLength);
}

export function framesToSamples(frames: number, hopLength = 512, nFft = 0): number {
  return requireModule().framesToSamples(frames, hopLength, nFft);
}

export function samplesToFrames(samples: number, hopLength = 512, nFft = 0): number {
  return requireModule().samplesToFrames(samples, hopLength, nFft);
}

export function powerToDb(request: DbConversionRequest): Float32Array;
export function powerToDb(
  values: Float32Array,
  ref?: number,
  amin?: number,
  topDb?: number,
): Float32Array;
export function powerToDb(
  values: Float32Array | DbConversionRequest,
  ref = 1.0,
  amin = 1e-10,
  topDb = 80.0,
): Float32Array {
  if (!(values instanceof Float32Array)) {
    return powerToDb(values.values, values.ref, values.amin, values.topDb);
  }
  return requireModule().powerToDb(values, ref, amin, topDb);
}

export function amplitudeToDb(request: DbConversionRequest): Float32Array;
export function amplitudeToDb(
  values: Float32Array,
  ref?: number,
  amin?: number,
  topDb?: number,
): Float32Array;
export function amplitudeToDb(
  values: Float32Array | DbConversionRequest,
  ref = 1.0,
  amin = 1e-5,
  topDb = 80.0,
): Float32Array {
  if (!(values instanceof Float32Array)) {
    return amplitudeToDb(values.values, values.ref, values.amin, values.topDb);
  }
  return requireModule().amplitudeToDb(values, ref, amin, topDb);
}

export function dbToPower(values: Float32Array, ref = 1.0): Float32Array {
  return requireModule().dbToPower(values, ref);
}

export function dbToAmplitude(values: Float32Array, ref = 1.0): Float32Array {
  return requireModule().dbToAmplitude(values, ref);
}

export function preemphasis(request: EmphasisRequest): Float32Array;
export function preemphasis(samples: Float32Array, coef?: number, zi?: number): Float32Array;
export function preemphasis(
  samples: Float32Array | EmphasisRequest,
  coef = 0.97,
  zi?: number,
): Float32Array {
  if (!(samples instanceof Float32Array)) {
    return preemphasis(samples.samples, samples.coef, samples.zi);
  }
  return requireModule().preemphasis(samples, coef, zi ?? null);
}

export function deemphasis(request: EmphasisRequest): Float32Array;
export function deemphasis(samples: Float32Array, coef?: number, zi?: number): Float32Array;
export function deemphasis(
  samples: Float32Array | EmphasisRequest,
  coef = 0.97,
  zi?: number,
): Float32Array {
  if (!(samples instanceof Float32Array)) {
    return deemphasis(samples.samples, samples.coef, samples.zi);
  }
  return requireModule().deemphasis(samples, coef, zi ?? null);
}

export function trimSilence(request: SilenceRequest): WasmTrimResult;
export function trimSilence(
  samples: Float32Array,
  topDb?: number,
  frameLength?: number,
  hopLength?: number,
): WasmTrimResult;
export function trimSilence(
  samples: Float32Array | SilenceRequest,
  topDb = 60.0,
  frameLength = 2048,
  hopLength = 512,
): WasmTrimResult {
  if (!(samples instanceof Float32Array)) {
    return trimSilence(samples.samples, samples.topDb, samples.frameLength, samples.hopLength);
  }
  return requireModule().trimSilence(samples, topDb, frameLength, hopLength);
}

export function splitSilence(request: SilenceRequest): Int32Array;
export function splitSilence(
  samples: Float32Array,
  topDb?: number,
  frameLength?: number,
  hopLength?: number,
): Int32Array;
export function splitSilence(
  samples: Float32Array | SilenceRequest,
  topDb = 60.0,
  frameLength = 2048,
  hopLength = 512,
): Int32Array {
  if (!(samples instanceof Float32Array)) {
    return splitSilence(samples.samples, samples.topDb, samples.frameLength, samples.hopLength);
  }
  return requireModule().splitSilence(samples, topDb, frameLength, hopLength);
}

/**
 * Lists the intervals where ANY of `signals` is sounding, so every gap
 * between them is silent in all of them -- what several takes of one part
 * share is their silence, not their sound.
 *
 * @returns The union of {@link splitSilence}'s per-signal intervals, merged
 *   where they touch. A single signal returns exactly what `splitSilence`
 *   does for it.
 */
export function splitSilenceCommon(request: SplitSilenceCommonRequest): Int32Array {
  return requireModule().splitSilenceCommon(
    request.signals,
    request.topDb ?? 60.0,
    request.frameLength ?? 2048,
    request.hopLength ?? 512,
  );
}

/**
 * {@link splitSilenceCommon} plus the report that says why those are the
 * intervals.
 *
 * Identical intervals, identical refusals, identical defaults; the only
 * difference is the second field. The plain entry point stays because a caller
 * cutting takes has no use for the diagnosis, and one interval covering
 * everything is the answer to three different situations the interval list cannot
 * separate — see {@link SilenceCommonReport} for reading them apart.
 *
 * @returns The union intervals and the report measured on the same RMS pass
 */
export function splitSilenceCommonWithReport(
  request: SplitSilenceCommonRequest,
): SplitSilenceCommonWithReportResult {
  return requireModule().splitSilenceCommonWithReport(
    request.signals,
    request.topDb ?? 60.0,
    request.frameLength ?? 2048,
    request.hopLength ?? 512,
  );
}

export function frameSignal(request: FrameSignalRequest): WasmFrameResult;
export function frameSignal(
  samples: Float32Array,
  frameLength: number,
  hopLength: number,
): WasmFrameResult;
export function frameSignal(
  samples: Float32Array | FrameSignalRequest,
  frameLength?: number,
  hopLength?: number,
): WasmFrameResult {
  if (!(samples instanceof Float32Array)) {
    return frameSignal(samples.samples, samples.frameLength, samples.hopLength);
  }
  return requireModule().frameSignal(samples, frameLength as number, hopLength as number);
}

export function padCenter(request: PadCenterRequest): Float32Array;
export function padCenter(
  values: Float32Array,
  targetSize: number,
  padValue?: number,
): Float32Array;
export function padCenter(
  values: Float32Array | PadCenterRequest,
  targetSize?: number,
  padValue = 0.0,
): Float32Array {
  if (!(values instanceof Float32Array)) {
    return padCenter(values.values, values.targetSize, values.padValue);
  }
  return requireModule().padCenter(values, targetSize as number, padValue);
}

export function fixLength(request: FixLengthRequest): Float32Array;
export function fixLength(
  values: Float32Array,
  targetSize: number,
  padValue?: number,
): Float32Array;
export function fixLength(
  values: Float32Array | FixLengthRequest,
  targetSize?: number,
  padValue = 0.0,
): Float32Array {
  if (!(values instanceof Float32Array)) {
    return fixLength(values.values, values.targetSize, values.padValue);
  }
  return requireModule().fixLength(values, targetSize as number, padValue);
}

export function fixFrames(request: FixFramesRequest): Int32Array;
export function fixFrames(
  frames: Int32Array,
  xMin?: number,
  xMax?: number,
  pad?: boolean,
): Int32Array;
export function fixFrames(
  frames: Int32Array | FixFramesRequest,
  xMin = 0,
  xMax = -1,
  pad = true,
): Int32Array {
  if (!(frames instanceof Int32Array)) {
    return fixFrames(frames.frames, frames.xMin, frames.xMax, frames.pad);
  }
  return requireModule().fixFrames(frames, xMin, xMax, pad);
}

export function onsetBacktrack(request: OnsetBacktrackRequest): Int32Array;
export function onsetBacktrack(events: Int32Array, energy: Float32Array): Int32Array;
export function onsetBacktrack(
  events: Int32Array | OnsetBacktrackRequest,
  energy?: Float32Array,
): Int32Array {
  if (!(events instanceof Int32Array)) {
    return onsetBacktrack(events.events, events.energy);
  }
  return requireModule().onsetBacktrack(events, energy as Float32Array);
}

export function peakPick(request: PeakPickRequest): Int32Array;
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
  values: Float32Array | PeakPickRequest,
  preMax?: number,
  postMax?: number,
  preAvg?: number,
  postAvg?: number,
  delta?: number,
  wait?: number,
): Int32Array {
  if (!(values instanceof Float32Array)) {
    const r = values;
    return peakPick(r.values, r.preMax, r.postMax, r.preAvg, r.postAvg, r.delta, r.wait);
  }
  return requireModule().peakPick(
    values,
    preMax as number,
    postMax as number,
    preAvg as number,
    postAvg as number,
    delta as number,
    wait as number,
  );
}

export function vectorNormalize(request: VectorNormalizeRequest): Float32Array;
export function vectorNormalize(
  values: Float32Array,
  normType?: number,
  threshold?: number,
): Float32Array;
export function vectorNormalize(
  values: Float32Array | VectorNormalizeRequest,
  normType = 0,
  threshold = 0.0,
): Float32Array {
  if (!(values instanceof Float32Array)) {
    return vectorNormalize(values.values, values.normType, values.threshold);
  }
  return requireModule().vectorNormalize(values, normType, threshold);
}

export function pcen(request: PcenRequest): Float32Array;
export function pcen(
  values: Float32Array,
  nBins: number,
  nFrames: number,
  options?: Record<string, number>,
): Float32Array;
export function pcen(
  values: Float32Array | PcenRequest,
  nBins = 0,
  nFrames = 0,
  options: Record<string, number> = {},
): Float32Array {
  if (!(values instanceof Float32Array)) {
    const r = values;
    const {
      values: requestValues,
      nBins: requestBins,
      nFrames: requestFrames,
      options: legacyOptions,
      ...flatOptions
    } = r;
    return pcen(requestValues, requestBins, requestFrames, {
      ...legacyOptions,
      ...flatOptions,
    });
  }
  return requireModule().pcen(values, nBins, nFrames, options);
}

export function tonnetz(request: TonnetzRequest): Float32Array;
export function tonnetz(chromagram: Float32Array, nChroma: number, nFrames: number): Float32Array;
export function tonnetz(
  chromagram: Float32Array | TonnetzRequest,
  nChroma?: number,
  nFrames?: number,
): Float32Array {
  if (!(chromagram instanceof Float32Array)) {
    return tonnetz(chromagram.chromagram, chromagram.nChroma, chromagram.nFrames);
  }
  return requireModule().tonnetz(chromagram, nChroma as number, nFrames as number);
}

export function tempogram(request: TempogramRequest): WasmTempogramResult;
export function tempogram(
  onsetEnvelope: Float32Array,
  sampleRate?: number,
  hopLength?: number,
  winLength?: number,
  mode?: TempogramMode,
  center?: boolean,
  norm?: boolean,
): WasmTempogramResult;
export function tempogram(
  onsetEnvelope: Float32Array | TempogramRequest,
  sampleRate = 22050,
  hopLength = 512,
  winLength = 384,
  mode: TempogramMode = 'autocorrelation',
  center = true,
  norm = true,
): WasmTempogramResult {
  if (!(onsetEnvelope instanceof Float32Array)) {
    const r = onsetEnvelope;
    return tempogram(
      r.onsetEnvelope,
      r.sampleRate,
      r.hopLength,
      r.winLength,
      r.mode,
      r.center,
      r.norm,
    );
  }
  return requireModule().tempogram(
    onsetEnvelope,
    sampleRate,
    hopLength,
    winLength,
    mode,
    center,
    norm,
  );
}

export function cyclicTempogram(request: CyclicTempogramRequest): WasmCyclicTempogramResult;
export function cyclicTempogram(
  onsetEnvelope: Float32Array,
  sampleRate?: number,
  hopLength?: number,
  winLength?: number,
  bpmMin?: number,
  nBins?: number,
): WasmCyclicTempogramResult;
export function cyclicTempogram(
  onsetEnvelope: Float32Array | CyclicTempogramRequest,
  sampleRate = 22050,
  hopLength = 512,
  winLength = 384,
  bpmMin = 60.0,
  nBins = 60,
): WasmCyclicTempogramResult {
  if (!(onsetEnvelope instanceof Float32Array)) {
    const r = onsetEnvelope;
    return cyclicTempogram(
      r.onsetEnvelope,
      r.sampleRate,
      r.hopLength,
      r.winLength,
      r.bpmMin,
      r.nBins,
    );
  }
  return requireModule().cyclicTempogram(
    onsetEnvelope,
    sampleRate,
    hopLength,
    winLength,
    bpmMin,
    nBins,
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
  if (!(onsetEnvelope instanceof Float32Array)) {
    const r = onsetEnvelope;
    return plp(r.onsetEnvelope, r.sampleRate, r.hopLength, r.tempoMin, r.tempoMax, r.winLength);
  }
  return requireModule().plp(onsetEnvelope, sampleRate, hopLength, tempoMin, tempoMax, winLength);
}
