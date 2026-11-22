import type { TrimResult } from './feature_spectrogram.js';
import { refuse, refuseType, withEntry } from './internal/entry.js';
import { requireListBudget } from './internal/list_budget.js';
import { isAbsent, isNonArrayObject } from './internal/value_guards.js';
import { getSonareModule } from './module_state.js';
import type { Matrix2D, NormType, TempogramMode } from './public_types.js';
import type { OptionsOf } from './validation.js';
import {
  assertAudioInput,
  assertAudioSamples,
  assertFloat32Array,
  assertFloatFields,
  assertFloatValue,
  assertIntegerValue,
  assertMatrix2D,
  assertNonNegativeSafeInteger,
  assertNumber,
  assertPositiveInteger,
  assertSampleRate,
  assertString,
  optionalBoolean,
  requestOnly,
  requireBoolean,
  requireToken,
  resolveOptionalNonNegative,
  sampleOneShot,
  toInt32Array,
  toMatrix2D,
  toMatrix2DByRows,
} from './validation.js';

/** Sine tone request. Every field is optional. */
export interface ToneRequest {
  /** Default 440. */
  frequencyHz?: number;
  /** Default 22050. */
  sampleRate?: number;
  /** Default 1. */
  durationSec?: number;
  /** Starting phase in radians. Default 0. */
  phase?: number;
  /** Default 1. */
  amplitude?: number;
}

/** Frequency sweep request from `fminHz` to `fmaxHz`. Every field is optional. */
export interface ChirpRequest {
  /** Default 440. */
  fminHz?: number;
  /** Default 880. */
  fmaxHz?: number;
  /** Default 22050. */
  sampleRate?: number;
  /** Default 1. */
  durationSec?: number;
  /** `true` for a linear sweep, `false` for an exponential one. Default `false`. */
  linear?: boolean;
}

/** Click train request. */
export interface ClicksRequest {
  /** Click onsets in seconds. */
  times: Float32Array;
  /** Default 22050. */
  sampleRate?: number;
  /** Output length in samples; absent derives it from the last click, an explicit 0 is refused. */
  length?: number;
  /** Default 1000. */
  frequencyHz?: number;
  /** Default 0.1. */
  clickDurationSec?: number;
}

/** Power-to-dB conversion request. */
export interface PowerToDbRequest<T extends Float32Array | Matrix2D = Float32Array | Matrix2D> {
  /** Power values to convert, as a flat array or a matrix; the result takes the same form. */
  values: T;
  /** Reference power. Default 1. */
  ref?: number;
  /** Floor applied before the log. Default 1e-10. */
  amin?: number;
  /** Dynamic range kept below the peak, in dB: at least 0, or `Infinity` for no clipping. Default 80. */
  topDb?: number;
}

/** Amplitude-to-dB conversion request. */
export interface AmplitudeToDbRequest<T extends Float32Array | Matrix2D = Float32Array | Matrix2D> {
  /** Amplitude values to convert, as a flat array or a matrix; the result takes the same form. */
  values: T;
  /** Reference amplitude. Default 1. */
  ref?: number;
  /** Floor applied before the log. Default 1e-5. */
  amin?: number;
  /** Dynamic range kept below the peak, in dB: at least 0, or `Infinity` for no clipping. Default 80. */
  topDb?: number;
}

/** dB-to-power conversion request. */
export interface DbToPowerRequest<T extends Float32Array | Matrix2D = Float32Array | Matrix2D> {
  /** Decibel values to convert to power, as a flat array or a matrix; the result takes the same form. */
  values: T;
  /** Reference power. Default 1. */
  ref?: number;
}

/** dB-to-amplitude conversion request. */
export interface DbToAmplitudeRequest<T extends Float32Array | Matrix2D = Float32Array | Matrix2D> {
  /** Decibel values to convert to amplitude, as a flat array or a matrix; the result takes the same form. */
  values: T;
  /** Reference amplitude. Default 1. */
  ref?: number;
}

/** Pre-emphasis filter request. */
export interface PreemphasisRequest {
  /** Mono audio samples. */
  samples: Float32Array;
  /** Sample rate of `samples` in Hz. */
  sampleRate: number;
  /** Filter coefficient. Default 0.97. */
  coef?: number;
  /** Initial filter state. */
  zi?: number;
}

/** De-emphasis filter request. */
export interface DeemphasisRequest {
  /** Mono audio samples. */
  samples: Float32Array;
  /** Sample rate of `samples` in Hz. */
  sampleRate: number;
  /** Filter coefficient. Default 0.97. */
  coef?: number;
  /** Initial filter state. */
  zi?: number;
}

/** Silence trim request. */
export interface TrimSilenceRequest {
  /** Mono audio samples. */
  samples: Float32Array;
  /** Sample rate of `samples` in Hz. */
  sampleRate: number;
  /** Threshold below the peak, in dB. Default 60. */
  topDb?: number;
  /** Analysis frame in samples. Default 2048. */
  frameLength?: number;
  /** Hop in samples. Default 512. */
  hopLength?: number;
}

/** Non-silent interval request for one signal. */
export interface SplitSilenceRequest {
  /** Mono audio samples. */
  samples: Float32Array;
  /** Sample rate of `samples` in Hz. */
  sampleRate: number;
  /** Threshold below the peak, in dB. Default 60. */
  topDb?: number;
  /** Analysis frame in samples. Default 2048. */
  frameLength?: number;
  /** Hop in samples. Default 512. */
  hopLength?: number;
}

/** Canonical request form for the common-silence union of several signals. */
export interface SplitSilenceCommonRequest {
  /** Signals to compare; lengths may differ. */
  signals: Float32Array[];
  /** Threshold below each signal's peak, in dB. Default `60`. */
  topDb?: number;
  /** Framing window in samples. Default `2048`. */
  frameLength?: number;
  /** Hop in samples. Default `512`. */
  hopLength?: number;
}

/** Request form of {@link splitSilenceCommonWithReport}; the fields of {@link SplitSilenceCommonRequest}. */
export interface SplitSilenceCommonWithReportRequest {
  /** Signals to compare; lengths may differ. */
  signals: Float32Array[];
  /** Threshold below each signal's peak, in dB. Default `60`. */
  topDb?: number;
  /** Framing window in samples. Default `2048`. */
  frameLength?: number;
  /** Hop in samples. Default `512`. */
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
   * minimum across the signals. Absent when any signal's peak is 0 or it holds
   * an exactly zero frame, where the depth has no finite value. Absent when not measured.
   */
  silenceCeilingDb?: number;
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

/** Overlapping frames of a signal. The rate is validated but does not change the result. */
export interface FrameSignalRequest {
  /** Mono audio samples. */
  samples: Float32Array;
  /** Sample rate of `samples` in Hz. */
  sampleRate: number;
  /** Frame size in samples. */
  frameLength: number;
  /** Hop between frames in samples. */
  hopLength: number;
}

/** Canonical request form for centering or extending a vector. */
export interface PadCenterRequest {
  /** Input values. */
  values: Float32Array;
  /** Length of the result in elements. */
  targetSize: number;
  /** Value written into every added element. Default `0`. */
  padValue?: number;
}

/** Canonical request form for resizing a vector. */
export interface FixLengthRequest {
  /** Input values. */
  values: Float32Array;
  /** Length of the result in elements. */
  targetSize: number;
  /** Value written into every added element. Default `0`. */
  padValue?: number;
}

/** Canonical request form for bounding a frame-index vector. */
export interface FixFramesRequest {
  /** Frame indices to bound. */
  frames: Int32Array | ArrayLike<number>;
  /** Lower bound. Default `0`. */
  xMin?: number;
  /** Upper bound; omit for none. Negative is refused. */
  xMax?: number;
  /** Pad to the bounds instead of clipping. Default `true`. */
  pad?: boolean;
}

/** Request for {@link onsetBacktrack}. */
export interface OnsetBacktrackRequest {
  /** Onset event indices in frames. */
  events: Int32Array | ArrayLike<number>;
  /** Energy curve the events are rolled back along. */
  energy: Float32Array;
}

/** Canonical request form for peak selection. */
export interface PeakPickRequest {
  /** Onset-strength-like vector to pick peaks from. */
  values: Float32Array;
  /** Frames before a candidate over which it must be the maximum. */
  preMax: number;
  /** Frames after a candidate over which it must be the maximum. */
  postMax: number;
  /** Frames before a candidate averaged for the threshold. */
  preAvg: number;
  /** Frames after a candidate averaged for the threshold. */
  postAvg: number;
  /** Offset above the local mean a peak must exceed, in the units of `values`. */
  delta: number;
  /** Minimum frames between two picked peaks. */
  wait: number;
}

/** Canonical request form for vector normalization. */
export interface VectorNormalizeRequest {
  /** Values to normalize. */
  values: Float32Array;
  /** Default `'max'`. */
  norm?: NormType;
  /** Magnitude below which a norm is treated as zero and the value left unscaled. Default `0`. */
  threshold?: number;
}

/** Canonical request form for tonal centroid projection. */
export interface TonnetzRequest {
  /** Rows are chroma bins, cols are frames. */
  chromagram: Matrix2D;
}

/** Request for {@link pcen}. */
export interface PcenRequest {
  /** Rows are bins, cols are frames. */
  values: Matrix2D;
  /** Sample rate in Hz used to derive the smoothing time constant. Default `22050`. */
  sampleRate?: number;
  /** Hop length in samples used to derive the smoothing time constant. Default `512`. */
  hopLength?: number;
  /** Smoothing filter time constant in seconds. Default `0.4`. */
  timeConstant?: number;
  /** Gain exponent applied to the smoothed energy. Default `0.98`. */
  gain?: number;
  /** Bias added before the power compression. Default `2.0`. */
  bias?: number;
  /** Power exponent of the final compression. Default `0.5`. */
  power?: number;
  /** Numerical floor against division by zero. Default `1e-06`. */
  eps?: number;
}

/** Request for {@link tempogram}. */
export interface TempogramRequest {
  /** Onset-strength envelope, one value per frame. */
  onsetEnvelope: Float32Array;
  /** Sample rate in Hz of the audio the envelope came from. Default `22050`. */
  sampleRate?: number;
  /** Hop length in samples of the envelope. Default `512`. */
  hopLength?: number;
  /** Analysis window in frames. Default `384`. */
  winLength?: number;
  /** Tempogram method, `'autocorrelation'` or `'cosine'`. Default `'autocorrelation'`. */
  mode?: TempogramMode;
  /** Pad so frames are centred on their time stamps. Default `true`. */
  center?: boolean;
  /** Normalize each frame. Default `true`. */
  norm?: boolean;
}

/** Request for {@link cyclicTempogram}. */
export interface CyclicTempogramRequest {
  /** Onset-strength envelope, one value per frame. */
  onsetEnvelope: Float32Array;
  /** Sample rate in Hz of the audio the envelope came from. */
  sampleRate: number;
  /** Hop length in samples of the envelope. Default `512`. */
  hopLength?: number;
  /** Analysis window in frames. Default `384`. */
  winLength?: number;
  /** Base of the folded tempo octave, in BPM; not a search range. Default `60`. */
  bpmMin?: number;
  /** Tempo bins in the folded octave. Default `60`. */
  nBins?: number;
}

/** Predominant local pulse request. The tempo range defaults to 60-200 BPM, narrower than librosa's. */
export interface PlpRequest {
  /** Onset-strength envelope, one value per frame. */
  onsetEnvelope: Float32Array;
  /** Sample rate in Hz of the audio the envelope came from. Default `22050`. */
  sampleRate?: number;
  /** Hop length in samples of the envelope. Default `512`. */
  hopLength?: number;
  /** Lowest tempo considered, in BPM. Default `60`. */
  bpmMin?: number;
  /** Highest tempo considered, in BPM. Default `200`. */
  bpmMax?: number;
  /** Analysis window in frames. Default `384`. */
  winLength?: number;
}

// ============================================================================
// Core - Synthesis
// ============================================================================

/**
 * Sine tone.
 *
 * @returns The samples, `durationSec * sampleRate` long.
 */
export function tone(request?: ToneRequest): Float32Array;
export function tone(...args: unknown[]): Float32Array {
  return withEntry('tone', () => {
    const r = requestOnly<ToneRequest>('tone', args.length === 0 ? [{}] : args);
    const frequencyHz = r.frequencyHz ?? 440;
    const durationSec = r.durationSec ?? 1;
    assertFloatFields({ frequencyHz, durationSec });
    return getSonareModule().tone(
      frequencyHz,
      r.sampleRate ?? 22050,
      durationSec,
      r.phase ?? 0,
      r.amplitude ?? 1,
    );
  });
}

/** Frequency sweep; exponential unless `linear` is set. */
export function chirp(request?: ChirpRequest): Float32Array;
export function chirp(...args: unknown[]): Float32Array {
  return withEntry('chirp', () => {
    const r = requestOnly<ChirpRequest>('chirp', args.length === 0 ? [{}] : args);
    const fminHz = r.fminHz ?? 440;
    const fmaxHz = r.fmaxHz ?? 880;
    const durationSec = r.durationSec ?? 1;
    assertFloatFields({ fminHz, fmaxHz, durationSec });
    return getSonareModule().chirp(
      fminHz,
      fmaxHz,
      r.sampleRate ?? 22050,
      durationSec,
      optionalBoolean(r.linear, 'linear', false),
    );
  });
}

/**
 * Click train.
 */
export function clicks(request: ClicksRequest): Float32Array;
export function clicks(...args: unknown[]): Float32Array {
  return withEntry('clicks', () => {
    const r = requestOnly<ClicksRequest>('clicks', args);
    assertFloat32Array(r.times, 'times');
    const frequencyHz = r.frequencyHz ?? 1000;
    const clickDurationSec = r.clickDurationSec ?? 0.1;
    assertFloatFields({ frequencyHz, clickDurationSec });
    return getSonareModule().clicks(
      r.times,
      r.sampleRate ?? 22050,
      r.length,
      frequencyHz,
      clickDurationSec,
    );
  });
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
  return withEntry('hzToMel', () => {
    assertNumber(hz, 'hz');
    return getSonareModule().hzToMel(hz);
  });
}

/**
 * Convert Mel scale to frequency in Hz. Total over the same domain as
 * {@link hzToMel}.
 *
 * @param mel - Mel frequency
 * @returns Frequency in Hz
 */
export function melToHz(mel: number): number {
  return withEntry('melToHz', () => {
    assertNumber(mel, 'mel');
    return getSonareModule().melToHz(mel);
  });
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
  return withEntry('hzToMidi', () => {
    assertNumber(hz, 'hz');
    return getSonareModule().hzToMidi(hz);
  });
}

/**
 * Convert MIDI note number to frequency in Hz. Total, like {@link hzToMel};
 * `-Infinity` bottoms out at 0 rather than propagating its sign.
 *
 * @param midi - MIDI note number
 * @returns Frequency in Hz
 */
export function midiToHz(midi: number): number {
  return withEntry('midiToHz', () => {
    assertNumber(midi, 'midi');
    return getSonareModule().midiToHz(midi);
  });
}

/**
 * Convert frequency in Hz to note name.
 *
 * A default `pitchPyin` track fills unvoiced frames with NaN, so drop those
 * frames before mapping the track through this.
 *
 * @param hz - Frequency in Hz
 * @returns Note name (e.g., "A4", "C#5")
 * @throws {@link RangeError} for a frequency with no
 * note: zero, negative, past the representable MIDI range, and non-finite alike.
 */
export function hzToNote(hz: number): string {
  return withEntry('hzToNote', () => {
    assertNumber(hz, 'hz');
    return getSonareModule().hzToNote(hz);
  });
}

/**
 * Convert note name to frequency in Hz.
 *
 * @param note - Note name (e.g., "A4", "C#5")
 * @returns Frequency in Hz
 * @throws {@link RangeError} for a name that does not
 * parse or whose octave is out of range.
 */
export function noteToHz(note: string): number {
  return withEntry('noteToHz', () => {
    assertString(note, 'note');
    return getSonareModule().noteToHz(note);
  });
}

/**
 * Convert frame index to time in seconds.
 *
 * @param frames - Frame index
 * @param sampleRate - Sample rate in Hz (default: 22050)
 * @param hopLength - Hop length in samples (default: 512)
 * @returns Time in seconds
 */
export function framesToTime(frames: number, sampleRate = 22050, hopLength = 512): number {
  return withEntry('framesToTime', () => {
    assertNumber(frames, 'frames');
    assertNumber(sampleRate, 'sampleRate');
    return getSonareModule().framesToTime(frames, sampleRate, hopLength);
  });
}

/**
 * Convert time in seconds to frame index.
 *
 * @param timeSec - Time in seconds
 * @param sampleRate - Sample rate in Hz (default: 22050)
 * @param hopLength - Hop length in samples (default: 512)
 * @returns Frame index
 */
export function timeToFrames(timeSec: number, sampleRate = 22050, hopLength = 512): number {
  return withEntry('timeToFrames', () => {
    assertNumber(timeSec, 'timeSec');
    assertNumber(sampleRate, 'sampleRate');
    return getSonareModule().timeToFrames(timeSec, sampleRate, hopLength);
  });
}

// Absent nFft is the native "no centring" 0; an explicit 0 or negative is out of domain.
function resolveOptionalFft(nFft: number | undefined | null): number {
  if (isAbsent(nFft)) {
    return 0;
  }
  assertIntegerValue(nFft, 'nFft');
  if (nFft === 0) {
    throw refuse('nFft must not be 0; omit it for no centring');
  }
  assertPositiveInteger(nFft, 'nFft');
  return nFft;
}

/**
 * Convert frame index to a sample index.
 *
 * @param hopLength - Hop length in samples, a positive integer (default: 512)
 * @param nFft - FFT size of a centred analysis; absent applies no centring offset, an explicit 0 or negative throws `RangeError`.
 */
export function framesToSamples(frames: number, hopLength = 512, nFft?: number): number {
  return withEntry('framesToSamples', () => {
    assertNumber(frames, 'frames');
    assertPositiveInteger(hopLength, 'hopLength');
    const fft = resolveOptionalFft(nFft);
    return getSonareModule().framesToSamples(frames, hopLength, fft);
  });
}

/**
 * Convert a sample index to a frame index.
 *
 * @param hopLength - Hop length in samples, a positive integer (default: 512)
 * @param nFft - FFT size of a centred analysis; absent applies no centring offset, an explicit 0 or negative throws `RangeError`.
 */
export function samplesToFrames(samples: number, hopLength = 512, nFft?: number): number {
  return withEntry('samplesToFrames', () => {
    assertNumber(samples, 'samples');
    assertPositiveInteger(hopLength, 'hopLength');
    const fft = resolveOptionalFft(nFft);
    return getSonareModule().samplesToFrames(samples, hopLength, fft);
  });
}

// ============================================================================
// Core - dB conversion and filters
// ============================================================================

/**
 * Applies an elementwise conversion to a flat array or to a matrix's data,
 * returning the same form.
 */
function convertElementwise(
  values: unknown,
  convert: (data: Float32Array) => Float32Array,
): Float32Array | Matrix2D {
  if (values instanceof Float32Array) {
    return convert(values);
  }
  if (!isNonArrayObject(values)) {
    throw refuseType('values must be a Float32Array or a Matrix2D');
  }
  const matrix = assertMatrix2D(values, 'values', false);
  return { data: convert(matrix.data), rows: matrix.rows, cols: matrix.cols };
}

/** Power values to dB. */
export function powerToDb<T extends Float32Array | Matrix2D>(request: PowerToDbRequest<T>): T;
export function powerToDb(...args: unknown[]): Float32Array | Matrix2D {
  return withEntry('powerToDb', () => {
    const r = requestOnly<PowerToDbRequest>('powerToDb', args);
    return convertElementwise(r.values, (data) =>
      getSonareModule().powerToDb(data, r.ref ?? 1.0, r.amin ?? 1e-10, r.topDb ?? 80.0),
    );
  });
}

/** Amplitude values to dB. */
export function amplitudeToDb<T extends Float32Array | Matrix2D>(
  request: AmplitudeToDbRequest<T>,
): T;
export function amplitudeToDb(...args: unknown[]): Float32Array | Matrix2D {
  return withEntry('amplitudeToDb', () => {
    const r = requestOnly<AmplitudeToDbRequest>('amplitudeToDb', args);
    return convertElementwise(r.values, (data) =>
      getSonareModule().amplitudeToDb(data, r.ref ?? 1.0, r.amin ?? 1e-5, r.topDb ?? 80.0),
    );
  });
}

/** dB values to power. */
export function dbToPower<T extends Float32Array | Matrix2D>(request: DbToPowerRequest<T>): T;
export function dbToPower(...args: unknown[]): Float32Array | Matrix2D {
  return withEntry('dbToPower', () => {
    const r = requestOnly<DbToPowerRequest>('dbToPower', args);
    assertNumber(r.ref ?? 1.0, 'ref');
    return convertElementwise(r.values, (data) => getSonareModule().dbToPower(data, r.ref ?? 1.0));
  });
}

/** dB values to amplitude. */
export function dbToAmplitude<T extends Float32Array | Matrix2D>(
  request: DbToAmplitudeRequest<T>,
): T;
export function dbToAmplitude(...args: unknown[]): Float32Array | Matrix2D {
  return withEntry('dbToAmplitude', () => {
    const r = requestOnly<DbToAmplitudeRequest>('dbToAmplitude', args);
    assertNumber(r.ref ?? 1.0, 'ref');
    return convertElementwise(r.values, (data) =>
      getSonareModule().dbToAmplitude(data, r.ref ?? 1.0),
    );
  });
}

/** First-order pre-emphasis filter. */
export function preemphasis(request: PreemphasisRequest): Float32Array;
export function preemphasis(
  samples: Float32Array,
  sampleRate: number,
  options?: OptionsOf<PreemphasisRequest>,
): Float32Array;
export function preemphasis(...args: unknown[]): Float32Array {
  return withEntry('preemphasis', () => {
    const r = sampleOneShot<PreemphasisRequest>('preemphasis', args);
    assertAudioInput(r.samples, r.sampleRate, r);
    return getSonareModule().preemphasis(r.samples, r.coef ?? 0.97, r.zi ?? null);
  });
}

/** First-order de-emphasis filter, the inverse of {@link preemphasis}. */
export function deemphasis(request: DeemphasisRequest): Float32Array;
export function deemphasis(
  samples: Float32Array,
  sampleRate: number,
  options?: OptionsOf<DeemphasisRequest>,
): Float32Array;
export function deemphasis(...args: unknown[]): Float32Array {
  return withEntry('deemphasis', () => {
    const r = sampleOneShot<DeemphasisRequest>('deemphasis', args);
    assertAudioInput(r.samples, r.sampleRate, r);
    return getSonareModule().deemphasis(r.samples, r.coef ?? 0.97, r.zi ?? null);
  });
}

/** Trim leading and trailing silence. */
export function trimSilence(request: TrimSilenceRequest): TrimResult;
export function trimSilence(
  samples: Float32Array,
  sampleRate: number,
  options?: OptionsOf<TrimSilenceRequest>,
): TrimResult;
export function trimSilence(...args: unknown[]): TrimResult {
  return withEntry('trimSilence', () => {
    const r = sampleOneShot<TrimSilenceRequest>('trimSilence', args);
    assertAudioInput(r.samples, r.sampleRate, r);
    return getSonareModule().trimSilence(
      r.samples,
      r.topDb ?? 60.0,
      r.frameLength ?? 2048,
      r.hopLength ?? 512,
    );
  });
}

/** Non-silent `[start, end)` sample intervals, flattened. */
export function splitSilence(request: SplitSilenceRequest): Int32Array;
export function splitSilence(
  samples: Float32Array,
  sampleRate: number,
  options?: OptionsOf<SplitSilenceRequest>,
): Int32Array;
export function splitSilence(...args: unknown[]): Int32Array {
  return withEntry('splitSilence', () => {
    const r = sampleOneShot<SplitSilenceRequest>('splitSilence', args);
    assertAudioInput(r.samples, r.sampleRate, r);
    return getSonareModule().splitSilence(
      r.samples,
      r.topDb ?? 60.0,
      r.frameLength ?? 2048,
      r.hopLength ?? 512,
    );
  });
}

/** Preflight each signal; an empty or non-array `signals` is left to the native refusal. */
function assertCommonSignals(signals: unknown): void {
  if (Array.isArray(signals)) {
    requireListBudget(signals, 'signals');
    signals.forEach((signal, i) => {
      assertAudioSamples(signal, {}, `signals[${i}]`);
    });
  }
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
export function splitSilenceCommon(request: SplitSilenceCommonRequest): Int32Array;
export function splitSilenceCommon(...args: unknown[]): Int32Array {
  return withEntry('splitSilenceCommon', () => {
    const r = requestOnly<SplitSilenceCommonRequest>('splitSilenceCommon', args);
    assertCommonSignals(r.signals);
    return getSonareModule().splitSilenceCommon(
      r.signals,
      r.topDb ?? 60.0,
      r.frameLength ?? 2048,
      r.hopLength ?? 512,
      undefined,
    );
  });
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
  request: SplitSilenceCommonWithReportRequest,
): SplitSilenceCommonWithReportResult;
export function splitSilenceCommonWithReport(
  ...args: unknown[]
): SplitSilenceCommonWithReportResult {
  return withEntry('splitSilenceCommonWithReport', () => {
    const r = requestOnly<SplitSilenceCommonWithReportRequest>(
      'splitSilenceCommonWithReport',
      args,
    );
    assertCommonSignals(r.signals);
    return getSonareModule().splitSilenceCommon(
      r.signals,
      r.topDb ?? 60.0,
      r.frameLength ?? 2048,
      r.hopLength ?? 512,
      true,
    );
  });
}

/**
 * Overlapping frames of a signal.
 *
 * @returns {@link Matrix2D} whose rows are the `frameLength` samples of a frame and cols are frames.
 * @throws TypeError when `frameLength` or `hopLength` is missing.
 */
export function frameSignal(request: FrameSignalRequest): Matrix2D;
export function frameSignal(
  samples: Float32Array,
  sampleRate: number,
  options: OptionsOf<FrameSignalRequest>,
): Matrix2D;
export function frameSignal(...args: unknown[]): Matrix2D {
  return withEntry('frameSignal', () => {
    const r = sampleOneShot<FrameSignalRequest>('frameSignal', args);
    assertAudioInput(r.samples, r.sampleRate, r);
    const { nFrames, frames } = getSonareModule().frameSignal(
      r.samples,
      r.frameLength,
      r.hopLength,
    );
    // Native stores one frame after another; the surface puts samples on rows and frames on columns.
    const data = new Float32Array(frames.length);
    for (let f = 0; f < nFrames; f++) {
      for (let i = 0; i < r.frameLength; i++) {
        data[i * nFrames + f] = frames[f * r.frameLength + i] as number;
      }
    }
    return toMatrix2D(data, r.frameLength, nFrames);
  });
}

/** Center `values` in a vector of `targetSize`. */
export function padCenter(request: PadCenterRequest): Float32Array;
export function padCenter(...args: unknown[]): Float32Array {
  return withEntry('padCenter', () => {
    const r = requestOnly<PadCenterRequest>('padCenter', args);
    assertFloat32Array(r.values, 'values');
    assertIntegerValue(r.targetSize, 'targetSize');
    return getSonareModule().padCenter(r.values, r.targetSize, r.padValue ?? 0.0);
  });
}

/** Pad or truncate `values` to `targetSize`. */
export function fixLength(request: FixLengthRequest): Float32Array;
export function fixLength(...args: unknown[]): Float32Array {
  return withEntry('fixLength', () => {
    const r = requestOnly<FixLengthRequest>('fixLength', args);
    assertFloat32Array(r.values, 'values');
    assertIntegerValue(r.targetSize, 'targetSize');
    return getSonareModule().fixLength(r.values, r.targetSize, r.padValue ?? 0.0);
  });
}

/** Bound a frame-index vector to `[xMin, xMax]`, padding the ends when `pad` is set. */
export function fixFrames(request: FixFramesRequest): Int32Array;
export function fixFrames(...args: unknown[]): Int32Array {
  return withEntry('fixFrames', () => {
    const r = requestOnly<FixFramesRequest>('fixFrames', args);
    return getSonareModule().fixFrames(
      toInt32Array(r.frames, 'frames'),
      r.xMin ?? 0,
      resolveOptionalNonNegative(r.xMax, 'xMax'),
      requireBoolean(r.pad ?? true, 'pad'),
    );
  });
}

/** Roll each onset event back to the preceding energy minimum. */
export function onsetBacktrack(request: OnsetBacktrackRequest): Int32Array;
export function onsetBacktrack(...args: unknown[]): Int32Array {
  return withEntry('onsetBacktrack', () => {
    const r = requestOnly<OnsetBacktrackRequest>('onsetBacktrack', args);
    const events = toInt32Array(r.events, 'events');
    assertFloat32Array(r.energy, 'energy');
    return getSonareModule().onsetBacktrack(events, r.energy);
  });
}

/** Pick peaks from an onset-strength-like vector (librosa.util.peak_pick). */
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
    return getSonareModule().peakPick(
      r.values,
      r.preMax,
      r.postMax,
      r.preAvg,
      r.postAvg,
      r.delta,
      r.wait,
    );
  });
}

const NORM_TOKENS = ['max', 'l1', 'l2', 'power'] as const;

/**
 * Normalize a vector.
 *
 * @throws TypeError for a non-string `norm`, RangeError for an unknown token.
 */
export function vectorNormalize(request: VectorNormalizeRequest): Float32Array;
export function vectorNormalize(...args: unknown[]): Float32Array {
  return withEntry('vectorNormalize', () => {
    const r = requestOnly<VectorNormalizeRequest>('vectorNormalize', args);
    assertFloat32Array(r.values, 'values');
    const norm = requireToken(r.norm, 'norm', NORM_TOKENS, 'max');
    return getSonareModule().vectorNormalize(
      r.values,
      NORM_TOKENS.indexOf(norm),
      r.threshold ?? 0.0,
    );
  });
}

const PCEN_OPTION_KEYS = [
  'sampleRate',
  'hopLength',
  'timeConstant',
  'gain',
  'bias',
  'power',
  'eps',
] as const;

/**
 * Per-channel energy normalisation.
 *
 * @returns {@link Matrix2D} of the input's shape: rows are bins, cols are frames.
 */
export function pcen(request: PcenRequest): Matrix2D;
export function pcen(...args: unknown[]): Matrix2D {
  return withEntry('pcen', () => {
    const r = requestOnly<PcenRequest>('pcen', args);
    const values = assertMatrix2D(r.values, 'values');
    const options: Record<string, number> = {};
    for (const key of PCEN_OPTION_KEYS) {
      if (!isAbsent(r[key])) {
        options[key] = r[key] as number;
      }
    }
    const out = getSonareModule().pcen(values.data, values.rows, values.cols, options);
    return toMatrix2D(out, values.rows, values.cols);
  });
}

/**
 * Tonal centroid features.
 *
 * @returns {@link Matrix2D} whose rows are the 6 tonnetz dimensions and cols are frames.
 */
export function tonnetz(request: TonnetzRequest): Matrix2D;
export function tonnetz(...args: unknown[]): Matrix2D {
  return withEntry('tonnetz', () => {
    const r = requestOnly<TonnetzRequest>('tonnetz', args);
    const chromagram = assertMatrix2D(r.chromagram, 'chromagram');
    const out = getSonareModule().tonnetz(chromagram.data, chromagram.rows, chromagram.cols);
    return toMatrix2D(out, 6, chromagram.cols);
  });
}

const TEMPOGRAM_MODES = ['autocorrelation', 'cosine'] as const;

/**
 * Tempogram of an onset-strength envelope.
 *
 * @returns {@link Matrix2D} whose rows are the `winLength` lags and cols are frames.
 * @throws TypeError for a non-string `mode`, RangeError for one other than `'autocorrelation'` or `'cosine'`.
 */
export function tempogram(request: TempogramRequest): Matrix2D;
export function tempogram(...args: unknown[]): Matrix2D {
  return withEntry('tempogram', () => {
    const r = requestOnly<TempogramRequest>('tempogram', args);
    assertFloat32Array(r.onsetEnvelope, 'onsetEnvelope');
    const mode = requireToken(r.mode, 'mode', TEMPOGRAM_MODES, 'autocorrelation');
    const native = getSonareModule().tempogram(
      r.onsetEnvelope,
      r.sampleRate ?? 22050,
      r.hopLength ?? 512,
      r.winLength ?? 384,
      mode,
      r.center ?? true,
      r.norm ?? true,
    );
    return toMatrix2DByRows(native.data, native.winLength);
  });
}

/**
 * Tempogram folded onto one octave of tempo.
 *
 * @returns {@link Matrix2D} whose rows are the `nBins` tempo bins and cols are frames.
 */
export function cyclicTempogram(request: CyclicTempogramRequest): Matrix2D;
export function cyclicTempogram(...args: unknown[]): Matrix2D {
  return withEntry('cyclicTempogram', () => {
    const r = requestOnly<CyclicTempogramRequest>('cyclicTempogram', args);
    assertFloat32Array(r.onsetEnvelope, 'onsetEnvelope');
    assertSampleRate(r.sampleRate);
    const native = getSonareModule().cyclicTempogram(
      r.onsetEnvelope,
      r.sampleRate,
      r.hopLength ?? 512,
      r.winLength ?? 384,
      r.bpmMin,
      r.nBins,
    );
    return toMatrix2DByRows(native.data, native.nBins);
  });
}

/** Predominant local pulse curve; the default range is 60-200 BPM. */
export function plp(request: PlpRequest): Float32Array;
export function plp(...args: unknown[]): Float32Array {
  return withEntry('plp', () => {
    const r = requestOnly<PlpRequest>('plp', args);
    assertFloat32Array(r.onsetEnvelope, 'onsetEnvelope');
    return getSonareModule().plp(
      r.onsetEnvelope,
      r.sampleRate ?? 22050,
      r.hopLength ?? 512,
      r.bpmMin,
      r.bpmMax,
      r.winLength ?? 384,
    );
  });
}
