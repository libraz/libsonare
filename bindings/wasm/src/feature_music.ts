import { convertSection } from './analysis_helpers.js';
import { refuse, refuseType, withEntry } from './internal/entry.js';
import { validatePositiveIntegers } from './internal/feature_validation.js';
import { resolveFftOptions } from './internal/fft_options.js';
import { getSonareModule } from './module_state.js';
import type {
  AnalyzeSectionsOptions,
  BoundaryOptions,
  BoundaryResult,
  CqtResult,
  LufsResult,
  Matrix2D,
  MelodyResult,
  SectionAnalysis,
} from './public_types.js';
import type { WasmCqtResult } from './sonare.js';
import type { OptionsOf } from './validation.js';
import {
  assertAudioInput,
  assertCqtHopLength,
  assertMatrix2D,
  assertNonNegativeScalar,
  assertPositiveFloatValue,
  assertSampleRate,
  assertSamples,
  optionalBoolean,
  requestOnly,
  resolveOptionalNonNegative,
  sampleOneShot,
  toMatrix2D,
  toMatrix2DByRows,
} from './validation.js';

function requireModule() {
  return getSonareModule();
}

/** The audio and its sample rate, which every one-shot over samples takes. */
interface SampleInput {
  /** Mono audio samples. */
  samples: Float32Array;
  /** Sample rate in Hz, an integer in `[8000, 384000]`; there is no default. */
  sampleRate: number;
}

/** Canonical request form shared by the Constant-Q transform variants. */
export interface CqtRequest extends SampleInput {
  /** Hop length in samples; default 512. */
  hopLength?: number;
  /** Lowest bin frequency in Hz; default 32.70319566257483 (C1). */
  fminHz?: number;
  /** Number of frequency bins; default 84. */
  nBins?: number;
  /** Bins per octave; default 12. */
  binsPerOctave?: number;
  /** Filter length scale factor, finite and greater than 0; default 1 (librosa's). */
  filterScale?: number;
}

/** Canonical request form for {@link pseudoCqt}. */
export interface PseudoCqtRequest extends CqtRequest {}

/** Canonical request form for {@link hybridCqt}. */
export interface HybridCqtRequest extends CqtRequest {}

/** Canonical request form for {@link vqt}. */
export interface VqtRequest extends CqtRequest {
  /** Bandwidth offset; omit for the automatic ERB-derived value, 0 is equivalent to CQT. */
  gamma?: number;
}

/** Canonical request form for {@link onsetStrength}. */
export interface OnsetStrengthRequest extends SampleInput {
  /** FFT size in samples. Default `2048`. */
  nFft?: number;
  /** Hop length in samples. Default `512`. */
  hopLength?: number;
  /** Mel bands the flux is taken over. Default `128`. */
  nMels?: number;
}

/** Canonical request form for {@link onsetStrengthMulti}. */
export interface OnsetStrengthMultiRequest extends OnsetStrengthRequest {
  /** Number of onset bands; default 3. */
  nBands?: number;
}

/** Canonical request form for {@link fourierTempogram}. */
export interface FourierTempogramRequest {
  /** Onset strength envelope, one value per frame. */
  onsetEnvelope: Float32Array;
  /** Sample rate in Hz of the audio the envelope came from; default 22050. */
  sampleRate?: number;
  /** Hop length in samples between envelope frames. Default `512`. */
  hopLength?: number;
  /** Analysis window in frames. Default `384`. */
  winLength?: number;
  /** Centre the windows. Default `true`. */
  center?: boolean;
}

/** Canonical request form for {@link tempogramRatio}. */
export interface TempogramRatioRequest {
  /** Tempogram as {@link Matrix2D}: rows = tempogram lags, cols = frames. */
  tempogram: Matrix2D;
  /** Sample rate in Hz of the audio the tempogram came from. Default `22050`. */
  sampleRate?: number;
  /** Hop length in samples between frames. Default `512`. */
  hopLength?: number;
  /** Lag ratios to evaluate. When omitted or empty, the library default {0.5, 1, 2, 3, 4} is used. */
  factors?: Float32Array | number[];
}

/** Canonical request form for {@link cqtToAudio}. */
export interface CqtToAudioRequest {
  /** CQT magnitude as {@link Matrix2D}: rows = CQT bins, cols = frames. */
  magnitude: Matrix2D;
  /** Output sample rate in Hz. */
  sampleRate: number;
  /** Hop length in samples. Default `512`. */
  hopLength?: number;
  /** Lowest bin frequency in Hz. Default `32.70319566257483` (C1). */
  fminHz?: number;
  /** CQT bins per octave. Default `12`. */
  binsPerOctave?: number;
  /** Griffin-Lim iterations, at most 256; default 32. */
  nIter?: number;
}

/** Canonical request form for {@link vqtToAudio}. */
export interface VqtToAudioRequest {
  /** VQT magnitude as {@link Matrix2D}: rows = VQT bins, cols = frames. */
  magnitude: Matrix2D;
  /** Output sample rate in Hz. */
  sampleRate: number;
  /** Bandwidth offset; omit for the automatic ERB-derived value, 0 is equivalent to CQT. */
  gamma?: number;
  /** Hop length in samples. Default `512`. */
  hopLength?: number;
  /** Lowest bin frequency in Hz. Default `32.70319566257483` (C1). */
  fminHz?: number;
  /** VQT bins per octave. Default `12`. */
  binsPerOctave?: number;
  /** Griffin-Lim iterations, at most 256. Default `32`. */
  nIter?: number;
}

/** Canonical request form for {@link analyzeSections}. */
export interface AnalyzeSectionsRequest extends SampleInput, AnalyzeSectionsOptions {}
/** Canonical (and positional) call form for {@link detectBoundaries}. */
export interface DetectBoundariesRequest extends SampleInput, BoundaryOptions {}
/** Canonical request form for {@link analyzeMelody}. */
export interface AnalyzeMelodyRequest extends SampleInput, MelodyOptions {}
/** Canonical request form for {@link lufs}. */
export interface LufsRequest extends SampleInput {}
/** Canonical request form for {@link momentaryLufs}. */
export interface MomentaryLufsRequest extends SampleInput {}
/** Canonical request form for {@link shortTermLufs}. */
export interface ShortTermLufsRequest extends SampleInput {}
/** Canonical request form for {@link nnlsChroma}. */
export interface NnlsChromaRequest extends SampleInput {
  /** Blend an STFT chroma into the NNLS chroma. Default `true`. */
  enableStftBlend?: boolean;
  /** Weight of the blended STFT chroma. Default `0.55`. */
  stftBlendWeight?: number;
  /** FFT size in samples of the blended STFT chroma. Default `4096`. */
  stftBlendNFft?: number;
  /** Hop length in samples. Default `512`. */
  hopLength?: number;
}

/**
 * Compute NNLS (non-negative least squares) chromagram.
 *
 * @param request - Mono `samples`, `sampleRate` in Hz, an integer in `[8000, 384000]`, and the
 *   optional NNLS settings; or pass `(samples, sampleRate, options?)`.
 * @returns {@link Matrix2D} whose rows are the 12 chroma bins and cols are frames
 */
export function nnlsChroma(request: NnlsChromaRequest): Matrix2D;
export function nnlsChroma(
  samples: Float32Array,
  sampleRate: number,
  options?: OptionsOf<NnlsChromaRequest>,
): Matrix2D;
export function nnlsChroma(...args: unknown[]): Matrix2D {
  return withEntry('nnlsChroma', () => {
    const request = sampleOneShot<NnlsChromaRequest>('nnlsChroma', args);
    assertAudioInput(request.samples, request.sampleRate, request);
    const hopLength = request.hopLength === undefined ? 512 : request.hopLength;
    const result = requireModule().nnlsChromaEx(
      request.samples,
      request.sampleRate,
      optionalBoolean(request.enableStftBlend, 'enableStftBlend', true),
      request.stftBlendWeight ?? 0.55,
      request.stftBlendNFft ?? 4096,
      hopLength,
    );
    return toMatrix2D(result.data, result.nChroma, result.nFrames);
  });
}

/** Shared body of the Constant-Q variants; `call` is the native transform. */
function cqtFamily<R extends CqtRequest>(
  requestName: 'cqt' | 'pseudoCqt' | 'hybridCqt' | 'vqt',
  args: readonly unknown[],
  call: (
    request: R,
    hopLength: number,
    fminHz: number,
    nBins: number,
    binsPerOctave: number,
  ) => WasmCqtResult,
): CqtResult {
  const request = sampleOneShot<R>(requestName, args);
  assertAudioInput(request.samples, request.sampleRate, request);
  const hopLength = request.hopLength ?? 512;
  const nBins = request.nBins ?? 84;
  const binsPerOctave = request.binsPerOctave ?? 12;
  const fminHz = request.fminHz ?? 32.70319566257483;
  assertPositiveFloatValue(fminHz, 'fminHz');
  const native = call(request, hopLength, fminHz, nBins, binsPerOctave);
  return {
    hopLength: native.hopLength,
    sampleRate: native.sampleRate,
    magnitude: toMatrix2D(native.magnitude, native.nBins, native.nFrames),
    frequencies: native.frequencies,
    gamma: native.gamma,
  };
}

/**
 * Compute the Constant-Q Transform magnitude.
 *
 * @param request - Mono `samples`, `sampleRate` in Hz, an integer in `[8000, 384000]`, `hopLength`
 *   (512), `fminHz` (32.70319566257483, C1), `nBins` (84), `binsPerOctave` (12) and `filterScale`
 *   (1); or pass `(samples, sampleRate, options?)`.
 * @returns CQT magnitude result
 */
export function cqt(request: CqtRequest): CqtResult;
export function cqt(
  samples: Float32Array,
  sampleRate: number,
  options?: OptionsOf<CqtRequest>,
): CqtResult;
export function cqt(...args: unknown[]): CqtResult {
  return withEntry('cqt', () => {
    return cqtFamily<CqtRequest>('cqt', args, (r, hop, fmin, nBins, bpo) =>
      requireModule().cqt(r.samples, r.sampleRate, hop, fmin, nBins, bpo, r.filterScale),
    );
  });
}

/**
 * Compute the pseudo Constant-Q Transform magnitude. Settings and defaults are
 * those of {@link cqt}.
 */
export function pseudoCqt(request: PseudoCqtRequest): CqtResult;
export function pseudoCqt(
  samples: Float32Array,
  sampleRate: number,
  options?: OptionsOf<PseudoCqtRequest>,
): CqtResult;
export function pseudoCqt(...args: unknown[]): CqtResult {
  return withEntry('pseudoCqt', () => {
    return cqtFamily<PseudoCqtRequest>('pseudoCqt', args, (r, hop, fmin, nBins, bpo) =>
      requireModule().pseudoCqt(r.samples, r.sampleRate, hop, fmin, nBins, bpo, r.filterScale),
    );
  });
}

/**
 * Compute the hybrid Constant-Q Transform magnitude. Settings and defaults are
 * those of {@link cqt}.
 */
export function hybridCqt(request: HybridCqtRequest): CqtResult;
export function hybridCqt(
  samples: Float32Array,
  sampleRate: number,
  options?: OptionsOf<HybridCqtRequest>,
): CqtResult;
export function hybridCqt(...args: unknown[]): CqtResult {
  return withEntry('hybridCqt', () => {
    return cqtFamily<HybridCqtRequest>('hybridCqt', args, (r, hop, fmin, nBins, bpo) =>
      requireModule().hybridCqt(r.samples, r.sampleRate, hop, fmin, nBins, bpo, r.filterScale),
    );
  });
}

/**
 * Compute the Variable-Q Transform magnitude (gamma controls Q). Settings and
 * defaults are those of {@link cqt}, plus `gamma`: omit it for the automatic
 * ERB-derived value, while 0 is equivalent to CQT. A negative or non-finite
 * value is refused.
 *
 * @returns VQT magnitude result (same shape as CQT)
 */
export function vqt(request: VqtRequest): CqtResult;
export function vqt(
  samples: Float32Array,
  sampleRate: number,
  options?: OptionsOf<VqtRequest>,
): CqtResult;
export function vqt(...args: unknown[]): CqtResult {
  return withEntry('vqt', () => {
    return cqtFamily<VqtRequest>('vqt', args, (r, hop, fmin, nBins, bpo) =>
      requireModule().vqt(
        r.samples,
        r.sampleRate,
        hop,
        fmin,
        nBins,
        bpo,
        r.filterScale,
        r.gamma ?? undefined,
      ),
    );
  });
}

/** Reads and checks the shared fields of a CQT/VQT inverse request. */
function cqtInverseInput(request: CqtToAudioRequest | VqtToAudioRequest) {
  assertMatrix2D(request.magnitude, 'magnitude');
  const sampleRate = request.sampleRate;
  const hopLength = request.hopLength ?? 512;
  const binsPerOctave = request.binsPerOctave ?? 12;
  const nIter = request.nIter ?? 32;
  assertSampleRate(sampleRate);
  assertCqtHopLength(hopLength);
  validatePositiveIntegers({ binsPerOctave });
  const fminHz = request.fminHz ?? 32.70319566257483;
  assertPositiveFloatValue(fminHz, 'fminHz');
  return { sampleRate, hopLength, binsPerOctave, nIter, fminHz };
}

/**
 * Reconstruct mono audio from CQT magnitude via Griffin-Lim.
 *
 * @throws {TypeError} when `magnitude` is not a {@link Matrix2D}
 * @throws {RangeError} when the matrix data length is not `rows * cols`, or a setting is out of domain
 */
export function cqtToAudio(request: CqtToAudioRequest): Float32Array {
  return withEntry('cqtToAudio', () => {
    const r = requestOnly<CqtToAudioRequest>('cqtToAudio', [request]);
    const c = cqtInverseInput(r);
    return requireModule().cqtToAudio(
      r.magnitude.data,
      r.magnitude.rows,
      r.magnitude.cols,
      c.sampleRate,
      c.hopLength,
      c.fminHz,
      c.binsPerOctave,
      c.nIter,
    );
  });
}

/**
 * Reconstruct mono audio from VQT magnitude via Griffin-Lim.
 *
 * @throws {TypeError} when `magnitude` is not a {@link Matrix2D}
 * @throws {RangeError} when the matrix data length is not `rows * cols`, or a setting is out of domain
 */
export function vqtToAudio(request: VqtToAudioRequest): Float32Array {
  return withEntry('vqtToAudio', () => {
    const r = requestOnly<VqtToAudioRequest>('vqtToAudio', [request]);
    const c = cqtInverseInput(r);
    return requireModule().vqtToAudio(
      r.magnitude.data,
      r.magnitude.rows,
      r.magnitude.cols,
      c.sampleRate,
      c.hopLength,
      c.fminHz,
      c.binsPerOctave,
      resolveOptionalNonNegative(r.gamma, 'gamma'),
      c.nIter,
    );
  });
}

/**
 * Detect song-structure sections (intro/verse/chorus/...).
 *
 * @param request - Mono `samples`, `sampleRate` in Hz, an integer in `[8000, 384000]`, `nFft`
 *   (2048), `hopLength` (512) and `minSectionSec` (4.0); or pass
 *   `(samples, sampleRate, options?)`.
 * @returns The detected sections and the form they spell
 */
export function analyzeSections(request: AnalyzeSectionsRequest): SectionAnalysis;
export function analyzeSections(
  samples: Float32Array,
  sampleRate: number,
  options?: OptionsOf<AnalyzeSectionsRequest>,
): SectionAnalysis;
export function analyzeSections(...args: unknown[]): SectionAnalysis {
  return withEntry('analyzeSections', () => {
    const request = sampleOneShot<AnalyzeSectionsRequest>('analyzeSections', args);
    assertAudioInput(request.samples, request.sampleRate, request);
    assertNonNegativeScalar(request.minSectionSec ?? 4.0, 'minSectionSec');
    // The embind value marshalling returns an array whose constructor is not this
    // realm's Array; chaining .map() onto it propagates that constructor via
    // Symbol.species, leaving a result that structuredClone (and so postMessage to
    // a Worker) rejects with "could not be cloned". Array.from() re-roots it as a
    // plain Array before mapping.
    const native = requireModule().analyzeSections(
      request.samples,
      request.sampleRate,
      request.nFft ?? 2048,
      request.hopLength ?? 512,
      request.minSectionSec ?? 4.0,
      request.threshold,
    );
    return { sections: Array.from(native.sections, convertSection), form: native.form };
  });
}

/**
 * Detect structural boundaries and return the novelty curve behind them.
 *
 * This is the unlabelled layer {@link analyzeSections} is built on, not a
 * coarser view of its output: sections are labelled spans, these are the
 * transitions plus the continuous curve they were picked from, so a caller
 * applying its own threshold needs this and cannot derive it from a section
 * list.
 *
 * @param request - Samples, sample rate and {@link BoundaryOptions}
 * @returns The boundaries, the novelty curve, and the analysis grid they live on
 * @throws {@link RangeError} when both `useMfcc` and
 * `useChroma` are `false`: the two feature streams are combined frame-for-frame,
 * so with neither enabled there is nothing to combine.
 *
 * @example
 * ```ts
 * const { boundaries, noveltyCurve, noveltyPeak } = detectBoundaries({
 *   samples,
 *   sampleRate: 44100,
 *   absoluteThreshold: 0.01,
 * });
 * // `noveltyCurve` is scaled by its own maximum; recover the raw response with
 * // `noveltyCurve[i] * noveltyPeak`.
 * ```
 */
export function detectBoundaries(request: DetectBoundariesRequest): BoundaryResult;
export function detectBoundaries(
  samples: Float32Array,
  sampleRate: number,
  options?: OptionsOf<DetectBoundariesRequest>,
): BoundaryResult;
export function detectBoundaries(...args: unknown[]): BoundaryResult {
  return withEntry('detectBoundaries', () => {
    const request = sampleOneShot<DetectBoundariesRequest>('detectBoundaries', args);
    const { samples, sampleRate } = request;
    assertAudioInput(samples, sampleRate, request);
    // Only the fields the caller set reach the embind wrapper, so an omitted one
    // takes BoundaryConfig's own default; restating the defaults here would make
    // them the effective ones on npm and leave the core's unreachable.
    for (const name of [
      'threshold',
      'absoluteThreshold',
      'peakDistanceSec',
      'referenceWindowSec',
    ] as const) {
      const value = request[name];
      if (value == null) {
        continue;
      }
      assertNonNegativeScalar(value, name);
    }
    // Only an explicit pair of falses; either one omitted leaves the other stream
    // on, so there is still something to combine.
    if (request.useMfcc === false && request.useChroma === false) {
      throw refuse('require useMfcc or useChroma');
    }
    const nativeOptions: Record<string, unknown> = { ...request };
    const result = requireModule().detectBoundaries(samples, sampleRate, nativeOptions);
    // Re-root the embind array as a plain Array, for the reason analyzeSections
    // spells out: chaining onto it propagates a constructor structuredClone
    // rejects, so a result posted from a Worker would fail to clone.
    return {
      boundaries: Array.from(result.boundaries, (b) => ({
        timeSec: b.time,
        frame: b.frame,
        strength: b.strength,
      })),
      noveltyCurve: result.noveltyCurve,
      noveltyPeak: result.noveltyPeak,
      sampleRate: result.sampleRate,
      hopLength: result.hopLength,
      frameStride: result.frameStride,
    };
  });
}

/** Options for {@link analyzeMelody}. All fields are optional. */
export interface MelodyOptions {
  /** Lowest f0 (Hz) the tracker will consider. Default 65 (≈ C2). */
  f0MinHz?: number;
  /** Highest f0 (Hz) the tracker will consider. Default 2093 (≈ C7). */
  f0MaxHz?: number;
  /** Analysis frame length in samples at 22050 Hz, rescaled to the input rate. Default 2048. */
  frameLength?: number;
  /** Hop length between frames in samples of the input buffer. Default 256. */
  hopLength?: number;
  /** Voicing confidence threshold in [0,1]; frames below are unvoiced. Default 0.1. */
  threshold?: number;
  /**
   * Use the pYIN tracker (Viterbi-smoothed) instead of plain per-frame YIN.
   * Produces a less octave-jumpy contour. Defaults to `false`.
   */
  usePyin?: boolean;
  /**
   * When {@link usePyin} is `true`, zero-pad by `frameLength / 2` so frame
   * `i` is centered at `i * hopLength` (matches `librosa.pyin(center=True)`).
   * Ignored by the plain-YIN path. Defaults to `true`.
   */
  center?: boolean;
}

/**
 * Extract the melody contour from monophonic audio via YIN (or pYIN).
 *
 * @param request - Mono `samples`, `sampleRate` in Hz, an integer in `[8000, 384000]`, and the
 *   tracker options ({@link MelodyOptions}); or pass `(samples, sampleRate, options?)`.
 * @returns Melody contour with per-frame pitch points and summary stats
 */
export function analyzeMelody(request: AnalyzeMelodyRequest): MelodyResult;
export function analyzeMelody(
  samples: Float32Array,
  sampleRate: number,
  options?: OptionsOf<AnalyzeMelodyRequest>,
): MelodyResult;
export function analyzeMelody(...args: unknown[]): MelodyResult {
  return withEntry('analyzeMelody', () => {
    const request = sampleOneShot<AnalyzeMelodyRequest>('analyzeMelody', args);
    assertAudioInput(request.samples, request.sampleRate, request);
    const result = requireModule().analyzeMelody(
      request.samples,
      request.sampleRate,
      request.f0MinHz ?? 65.0,
      request.f0MaxHz ?? 2093.0,
      request.frameLength ?? 2048,
      request.hopLength ?? 256,
      request.threshold ?? 0.1,
      optionalBoolean(request.usePyin, 'usePyin', false),
      optionalBoolean(request.center, 'center', true),
    );
    return {
      // Native reports an unvoiced frame as frequency 0; here it is absent.
      points: Array.from(result.points, (p) =>
        p.frequency > 0
          ? { timeSec: p.time, frequencyHz: p.frequency, confidence: p.confidence }
          : { timeSec: p.time, confidence: p.confidence },
      ),
      pitchRangeOctaves: result.pitchRangeOctaves,
      pitchStability: result.pitchStability,
      meanFrequencyHz: result.meanFrequency,
      vibratoRateHz: result.vibratoRate,
    };
  });
}

/**
 * Compute the onset strength envelope.
 *
 * @param request - Mono `samples`, `sampleRate` in Hz, an integer in `[8000, 384000]`, `nFft`
 *   (2048), `hopLength` (512) and `nMels` (128); or pass `(samples, sampleRate, options?)`.
 * @returns Onset strength for each frame
 */
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
    const fft = resolveFftOptions(request.nFft ?? 2048, request.hopLength ?? 512);
    const nMels = request.nMels ?? 128;
    return requireModule().onsetEnvelope(
      request.samples,
      request.sampleRate,
      fft.nFft,
      fft.hopLength,
      nMels,
    );
  });
}

/**
 * Compute multi-band onset strength envelopes.
 *
 * @param request - Mono `samples`, `sampleRate` in Hz, an integer in `[8000, 384000]`, `nFft`
 *   (2048), `hopLength` (512), `nMels` (128) and `nBands` (3); or pass
 *   `(samples, sampleRate, options?)`.
 * @returns {@link Matrix2D} whose rows are the onset bands and cols are frames
 */
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
    const fft = resolveFftOptions(request.nFft ?? 2048, request.hopLength ?? 512);
    const nMels = request.nMels ?? 128;
    const nBands = request.nBands ?? 3;
    const result = requireModule().onsetStrengthMulti(
      request.samples,
      request.sampleRate,
      fft.nFft,
      fft.hopLength,
      nMels,
      nBands,
    );
    return toMatrix2D(result.data, result.nBands, result.nFrames);
  });
}

/**
 * Compute the Fourier tempogram from an onset envelope.
 *
 * @returns {@link Matrix2D} whose rows are the tempo bins and cols are frames
 * @throws {TypeError} when the request is not an object or `onsetEnvelope` is not a `Float32Array`
 */
export function fourierTempogram(request: FourierTempogramRequest): Matrix2D {
  return withEntry('fourierTempogram', () => {
    const r = requestOnly<FourierTempogramRequest>('fourierTempogram', [request]);
    const sampleRate = r.sampleRate ?? 22050;
    const hopLength = r.hopLength ?? 512;
    const winLength = r.winLength ?? 384;
    if (!(r.onsetEnvelope instanceof Float32Array)) {
      throw refuseType('onsetEnvelope must be a Float32Array');
    }
    assertSampleRate(sampleRate);
    assertSamples(r.onsetEnvelope, true, 'onsetEnvelope');
    // Native takes a `norm` flag; the normalised form is the only one exposed.
    const result = requireModule().fourierTempogram(
      r.onsetEnvelope,
      sampleRate,
      hopLength,
      winLength,
      optionalBoolean(r.center, 'center', true),
      true,
    );
    return toMatrix2DByRows(result.data, result.nBins);
  });
}

/**
 * Compute tempogram ratio features.
 *
 * @returns Tempogram ratio features (one value per factor)
 * @throws {TypeError} when `tempogram` is not a {@link Matrix2D}
 * @throws {RangeError} when the matrix data length is not `rows * cols`
 */
export function tempogramRatio(request: TempogramRatioRequest): Float32Array {
  return withEntry('tempogramRatio', () => {
    const r = requestOnly<TempogramRatioRequest>('tempogramRatio', [request]);
    const tempogram = assertMatrix2D(r.tempogram, 'tempogram');
    const sampleRate = r.sampleRate ?? 22050;
    const hopLength = r.hopLength ?? 512;
    assertSampleRate(sampleRate);
    return requireModule().tempogramRatio(
      tempogram.data,
      tempogram.rows,
      sampleRate,
      hopLength,
      r.factors,
    );
  });
}

/**
 * Measure loudness (EBU R128 / ITU-R BS.1770).
 *
 * @param request - Mono `samples` and `sampleRate` in Hz; K-weighting is sample-rate dependent, so
 *   pass the buffer's actual rate; or pass `(samples, sampleRate, options?)`.
 * @returns Loudness measurement result
 */
export function lufs(request: LufsRequest): LufsResult;
export function lufs(
  samples: Float32Array,
  sampleRate: number,
  options?: OptionsOf<LufsRequest>,
): LufsResult;
export function lufs(...args: unknown[]): LufsResult {
  return withEntry('lufs', () => {
    const request = sampleOneShot<LufsRequest>('lufs', args);
    assertAudioInput(request.samples, request.sampleRate, request);
    const native = requireModule().lufs(request.samples, request.sampleRate);
    return {
      integratedLufs: native.integratedLufs,
      momentaryLufs: native.momentaryLufs,
      shortTermLufs: native.shortTermLufs,
      maxMomentaryLufs: native.maxMomentaryLufs,
      maxShortTermLufs: native.maxShortTermLufs,
      loudnessRangeLu: native.loudnessRangeLu,
    };
  });
}

/**
 * Compute the momentary loudness (LUFS) over time.
 *
 * @param request - Mono `samples` and `sampleRate` in Hz; K-weighting is sample-rate dependent, so
 *   pass the buffer's actual rate; or pass `(samples, sampleRate, options?)`.
 * @returns Momentary LUFS values over time
 */
export function momentaryLufs(request: MomentaryLufsRequest): Float32Array;
export function momentaryLufs(
  samples: Float32Array,
  sampleRate: number,
  options?: OptionsOf<MomentaryLufsRequest>,
): Float32Array;
export function momentaryLufs(...args: unknown[]): Float32Array {
  return withEntry('momentaryLufs', () => {
    const request = sampleOneShot<MomentaryLufsRequest>('momentaryLufs', args);
    assertAudioInput(request.samples, request.sampleRate, request);
    return requireModule().momentaryLufs(request.samples, request.sampleRate);
  });
}

/**
 * Compute the short-term loudness (LUFS) over time.
 *
 * @param request - Mono `samples` and `sampleRate` in Hz; K-weighting is sample-rate dependent, so
 *   pass the buffer's actual rate; or pass `(samples, sampleRate, options?)`.
 * @returns Short-term LUFS values over time
 */
export function shortTermLufs(request: ShortTermLufsRequest): Float32Array;
export function shortTermLufs(
  samples: Float32Array,
  sampleRate: number,
  options?: OptionsOf<ShortTermLufsRequest>,
): Float32Array;
export function shortTermLufs(...args: unknown[]): Float32Array {
  return withEntry('shortTermLufs', () => {
    const request = sampleOneShot<ShortTermLufsRequest>('shortTermLufs', args);
    assertAudioInput(request.samples, request.sampleRate, request);
    return requireModule().shortTermLufs(request.samples, request.sampleRate);
  });
}
