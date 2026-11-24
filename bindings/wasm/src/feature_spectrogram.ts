/**
 * Spectrogram and chroma representations, and the silence trim that shares
 * their input checks.
 */

import { refuse, withEntry } from './internal/entry.js';
import { resolveFftOptions } from './internal/fft_options.js';
import {
  sampleNeed,
  stftBins,
  stftFrames,
  withHeapNeedWhen,
  withMonoHeap,
} from './internal/memory_budget.js';
import { getSonareModule } from './module_state.js';
import type {
  ChromaResult,
  Matrix2D,
  MelNorm,
  MelSpectrogramResult,
  MfccResult,
  ReassignedSpectrogramResult,
  StftResult,
} from './public_types.js';
import type { WasmChromaResult } from './sonare.js';
import type { OptionsOf } from './validation.js';
import {
  assertAudioInput,
  assertClosedToken,
  assertFiniteScalar,
  assertFloat32Array,
  assertMatrix2D,
  assertNonNegativeScalar,
  assertNumber,
  assertOptionalFmaxNotZero,
  assertSampleRate,
  optionalBoolean,
  requestOnly,
  sampleOneShot,
  toMatrix2D,
} from './validation.js';

const MAX_CQT_HOP = 65536;
const MAX_CQT_BINS_PER_OCTAVE = Math.floor(0x7fffffff / 7);
const MAX_MEL_BANDS = 4096;

const isInt = (value: unknown): value is number =>
  typeof value === 'number' && Number.isInteger(value);

/** Whether the native chroma-from-CQT reader accepts this hop, chroma count and grid. */
function cqtChromaInDomain(hopLength: unknown, nChroma: unknown, binsPerOctave: unknown): boolean {
  return (
    isInt(hopLength) &&
    hopLength >= 1 &&
    hopLength <= MAX_CQT_HOP &&
    isInt(nChroma) &&
    nChroma >= 1 &&
    isInt(binsPerOctave) &&
    binsPerOctave >= 1 &&
    binsPerOctave <= MAX_CQT_BINS_PER_OCTAVE &&
    binsPerOctave % nChroma === 0
  );
}

/** STFT request. */
export interface StftRequest {
  /** Mono audio samples. */
  samples: Float32Array;
  /** Sample rate of `samples` in Hz. */
  sampleRate: number;
  /** FFT size. Default 2048. */
  nFft?: number;
  /** Hop in samples. Default 512. */
  hopLength?: number;
  /** When true the result also carries `db`, the magnitude in decibels. Default false. */
  db?: boolean;
  /** When true the result also carries `phase` in radians. Default false. */
  phase?: boolean;
}

/** STFT chromagram request. */
export interface ChromaRequest {
  /** Mono audio samples. */
  samples: Float32Array;
  /** Sample rate of `samples` in Hz. */
  sampleRate: number;
  /** FFT size. Default 2048. */
  nFft?: number;
  /** Hop in samples. Default 512. */
  hopLength?: number;
}

/**
 * Chroma-energy-normalized request. Carries no `nFft`: it is built on a
 * constant-Q transform, which has no FFT size to set.
 */
export interface ChromaCensRequest {
  /** Mono audio samples. */
  samples: Float32Array;
  /** Sample rate of `samples` in Hz. */
  sampleRate: number;
  /** Hop in samples. Default 512. */
  hopLength?: number;
  /** Chroma bins. Default 12. */
  nChroma?: number;
  /** Constant-Q bins per octave; a multiple of `nChroma`. Default 36. */
  binsPerOctave?: number;
}

/** Constant-Q chromagram request. Carries no `nFft`, like {@link ChromaCensRequest}. */
export interface ChromaCqtRequest {
  /** Mono audio samples. */
  samples: Float32Array;
  /** Sample rate of `samples` in Hz. */
  sampleRate: number;
  /** Hop in samples. Default 512. */
  hopLength?: number;
  /** Chroma bins. Default 12. */
  nChroma?: number;
  /** Constant-Q bins per octave; a multiple of `nChroma`. Default 36. */
  binsPerOctave?: number;
}

/**
 * Bass-focused chroma request. Carries no `binsPerOctave`: the bass chroma fixes
 * its bin count and its lowest frequency together.
 */
export interface BassChromaRequest {
  /** Mono audio samples. */
  samples: Float32Array;
  /** Sample rate of `samples` in Hz. */
  sampleRate: number;
  /** Hop in samples. Default 512. */
  hopLength?: number;
  /** Chroma bins. Default 12. */
  nChroma?: number;
}

/** Mel spectrogram request. */
export interface MelSpectrogramRequest {
  /** Mono audio samples. */
  samples: Float32Array;
  /** Sample rate of `samples` in Hz. */
  sampleRate: number;
  /** FFT size. Default 2048. */
  nFft?: number;
  /** Hop in samples. Default 512. */
  hopLength?: number;
  /** Mel bands. Default 128. */
  nMels?: number;
  /** Lowest band edge in Hz. Default 0. */
  fminHz?: number;
  /** Highest band edge in Hz. Omit to use Nyquist; a supplied value must be positive and at most Nyquist. */
  fmaxHz?: number;
  /** Use the HTK mel formula instead of Slaney. Default `false`. */
  htk?: boolean;
}

/** MFCC request. */
export interface MfccRequest {
  /** Mono audio samples. */
  samples: Float32Array;
  /** Sample rate of `samples` in Hz. */
  sampleRate: number;
  /** FFT size. Default 2048. */
  nFft?: number;
  /** Hop in samples. Default 512. */
  hopLength?: number;
  /** Mel bands. Default 128. */
  nMels?: number;
  /** Lowest band edge in Hz. Default 0. */
  fminHz?: number;
  /** Highest band edge in Hz. Omit to use Nyquist; a supplied value must be positive and at most Nyquist. */
  fmaxHz?: number;
  /** Use the HTK mel formula instead of Slaney. Default `false`. */
  htk?: boolean;
  /** Coefficients kept. Default 20. */
  nMfcc?: number;
  /** Cepstral liftering coefficient; 0 is none. Default 0. */
  lifter?: number;
}

/** First-order delta request. */
export interface MelDeltaRequest {
  /** Rows are features, cols are frames. */
  features: Matrix2D;
  /** Regression window, odd and at least 3. Default 9. */
  width?: number;
}

/** Reassigned spectrogram request. */
export interface ReassignedSpectrogramRequest {
  /** Mono audio samples. */
  samples: Float32Array;
  /** Sample rate of `samples` in Hz. */
  sampleRate: number;
  /** FFT size. Default 2048. */
  nFft?: number;
  /** Hop in samples. Default 512. */
  hopLength?: number;
  /** Magnitude floor below which a bin is not reassigned. Default 1e-6. */
  refPower?: number;
  /** Fill unreassigned bins with NaN instead of their own coordinates. Default `false`. */
  fillNan?: boolean;
}

/** Request for {@link trim}. */
export interface TrimRequest {
  /** Mono audio samples. */
  samples: Float32Array;
  /** Sample rate in Hz, an integer in `[8000, 384000]`; there is no default. */
  sampleRate: number;
  /** Silence threshold in dBFS. Default -60. */
  thresholdDb?: number;
  /** Analysis frame in samples. Default 2048. */
  frameLength?: number;
  /** Hop in samples. Default 512. */
  hopLength?: number;
}

/** Trimmed signal and its bounds, in samples of the input. */
export interface TrimResult {
  /** The trimmed samples. */
  audio: Float32Array;
  /** Index in the input of the first kept sample. */
  startSample: number;
  /** Index in the input where the kept range ends (exclusive). */
  endSample: number;
}

/**
 * Trim silence from the beginning and end of audio.
 *
 * @returns Trimmed audio
 * @throws {TypeError} when `sampleRate` is missing
 * @throws {RangeError} on audio out of domain, an out-of-range `sampleRate` or a non-positive frame or hop
 */
export function trim(request: TrimRequest): Float32Array;
export function trim(
  samples: Float32Array,
  sampleRate: number,
  options?: OptionsOf<TrimRequest>,
): Float32Array;
export function trim(...args: unknown[]): Float32Array {
  return withEntry('trim', () => {
    const r = sampleOneShot<TrimRequest>('trim', args);
    const thresholdDb = r.thresholdDb ?? -60.0;
    const frameLength = r.frameLength ?? 2048;
    const hopLength = r.hopLength ?? 512;
    assertAudioInput(r.samples, r.sampleRate, r);
    assertFiniteScalar(thresholdDb, 'thresholdDb');
    const { samples, sampleRate } = r;
    return withMonoHeap(samples, sampleRate, 1, () =>
      getSonareModule().trim(samples, sampleRate, thresholdDb, frameLength, hopLength),
    );
  });
}

/** Request for {@link fade}. */
export interface FadeRequest {
  /** Mono audio samples. */
  samples: Float32Array;
  /** Sample rate in Hz, an integer in `[8000, 384000]`; there is no default. */
  sampleRate: number;
  /** Cosine fade-in length in seconds, finite and at least 0; clamped to the input. Default 0. */
  fadeInSec?: number;
  /** Cosine fade-out length in seconds, finite and at least 0; clamped to the input. Default 0. */
  fadeOutSec?: number;
}

/**
 * Apply a cosine fade-in and fade-out to mono audio; 0 leaves that end untouched.
 *
 * @returns Faded audio, the same length as the input
 * @throws {TypeError} on a wrong-typed field or an unknown key
 * @throws {RangeError} on a negative or non-finite length, or audio out of domain
 */
export function fade(request: FadeRequest): Float32Array {
  return withEntry('fade', () => {
    const r = requestOnly<FadeRequest>('fade', [request]);
    assertFloat32Array(r.samples, 'samples');
    assertSampleRate(r.sampleRate);
    const { samples, sampleRate } = r;
    return withMonoHeap(samples, sampleRate, 3, () =>
      getSonareModule().fade(samples, sampleRate, r.fadeInSec ?? 0, r.fadeOutSec ?? 0),
    );
  });
}

/** Response of a {@link biquadFilter}. */
export type BiquadFilterType = 'highPass' | 'lowPass' | 'bandPass' | 'notch';

const BIQUAD_FILTER_TYPES: readonly BiquadFilterType[] = [
  'highPass',
  'lowPass',
  'bandPass',
  'notch',
];

/** Request for {@link biquadFilter}. */
export interface BiquadFilterRequest {
  /** Mono audio samples. */
  samples: Float32Array;
  /** Sample rate in Hz, an integer in `[8000, 384000]`; there is no default. */
  sampleRate: number;
  /** Filter response. */
  type: BiquadFilterType;
  /** 2, or 4 for `highPass` / `lowPass` (a two-section cascade). Default 2. */
  order?: number;
  /** Run the filter forward and backward for zero phase shift. Default `false`. */
  zeroPhase?: boolean;
  /** Corner in Hz inside `(0, sampleRate / 2)`; `highPass` / `lowPass` only. */
  cutoffHz?: number;
  /** Center in Hz inside `(0, sampleRate / 2)`; `bandPass` / `notch` only. */
  centerHz?: number;
  /** Bandwidth in Hz, above 0; `bandPass` / `notch` only. */
  bandwidthHz?: number;
}

/**
 * Filter mono audio with a Butterworth high-pass or low-pass, or a band-pass or notch biquad. A
 * frequency field the `type` does not read must be omitted.
 *
 * @returns Filtered audio, the same length as the input
 * @throws {TypeError} on a wrong-typed field, an unknown key or a `type` that is not a string
 * @throws {RangeError} on an unknown `type`, a frequency field out of range or one `type` does not read, or an unsupported `order`
 */
export function biquadFilter(request: BiquadFilterRequest): Float32Array {
  return withEntry('biquadFilter', () => {
    const r = requestOnly<BiquadFilterRequest>('biquadFilter', [request]);
    assertFloat32Array(r.samples, 'samples');
    const zeroPhase = optionalBoolean(r.zeroPhase, 'zeroPhase', false);
    assertSampleRate(r.sampleRate);
    assertClosedToken(r.type, BIQUAD_FILTER_TYPES, 'type');
    const { samples, sampleRate } = r;
    return withMonoHeap(samples, sampleRate, 3, () =>
      getSonareModule().biquadFilter(
        samples,
        sampleRate,
        BIQUAD_FILTER_TYPES.indexOf(r.type),
        r.order ?? 2,
        zeroPhase,
        r.cutoffHz,
        r.centerHz,
        r.bandwidthHz,
      ),
    );
  });
}

// ============================================================================
// Features - Spectrogram
// ============================================================================

/**
 * Short-time Fourier transform.
 *
 * @returns Magnitude and power matrices whose rows are `nFft / 2 + 1` bins and cols are frames;
 *   `db` (magnitude in decibels) and `phase` (radians) are present only when requested.
 */
export function stft(request: StftRequest): StftResult;
export function stft(
  samples: Float32Array,
  sampleRate: number,
  options?: OptionsOf<StftRequest>,
): StftResult;
export function stft(...args: unknown[]): StftResult {
  return withEntry('stft', () => {
    const r = sampleOneShot<StftRequest>('stft', args);
    const withDb = optionalBoolean(r.db, 'db', false);
    const withPhase = optionalBoolean(r.phase, 'phase', false);
    assertAudioInput(r.samples, r.sampleRate, r);
    const fft = resolveFftOptions(r.nFft, r.hopLength);
    const { samples, sampleRate } = r;
    const bins = stftBins(fft.nFft, fft.hopLength);
    const native = withHeapNeedWhen(
      bins !== undefined,
      sampleNeed(
        'samples',
        samples.length,
        sampleRate,
        (n) =>
          4 * n +
          (8 + (withDb ? 4 : 0) + (withPhase ? 4 : 0)) *
            (bins as number) *
            stftFrames(n, fft.hopLength),
      ),
      () => getSonareModule().stft(samples, sampleRate, fft.nFft, fft.hopLength, withDb, withPhase),
    );
    const result: StftResult = {
      nFft: native.nFft,
      hopLength: native.hopLength,
      sampleRate: native.sampleRate,
      magnitude: toMatrix2D(native.magnitude, native.nBins, native.nFrames),
      power: toMatrix2D(native.power, native.nBins, native.nFrames),
    };
    if (native.db !== undefined) {
      result.db = toMatrix2D(native.db, native.nBins, native.nFrames);
    }
    if (native.phase !== undefined) {
      result.phase = toMatrix2D(native.phase, native.nBins, native.nFrames);
    }
    return result;
  });
}

function chromaResult(native: WasmChromaResult): ChromaResult {
  return {
    sampleRate: native.sampleRate,
    hopLength: native.hopLength,
    features: toMatrix2D(native.features, native.nChroma, native.nFrames),
    meanEnergy: native.meanEnergy,
  };
}

/**
 * Chroma Energy Normalized Statistics.
 *
 * @returns Chroma result; `features` rows are chroma bins and cols are frames.
 */
export function chromaCens(request: ChromaCensRequest): ChromaResult;
export function chromaCens(
  samples: Float32Array,
  sampleRate: number,
  options?: OptionsOf<ChromaCensRequest>,
): ChromaResult;
export function chromaCens(...args: unknown[]): ChromaResult {
  return withEntry('chromaCens', () => {
    const r = sampleOneShot<ChromaCensRequest>('chromaCens', args);
    assertAudioInput(r.samples, r.sampleRate, r);
    const hopLength = r.hopLength ?? 512;
    const nChroma = r.nChroma ?? 12;
    const binsPerOctave = r.binsPerOctave ?? 36;
    const { samples, sampleRate } = r;
    return chromaResult(
      withHeapNeedWhen(
        cqtChromaInDomain(hopLength, nChroma, binsPerOctave),
        sampleNeed(
          'samples',
          samples.length,
          sampleRate,
          (n) => 4 * n + 8 * nChroma * Math.floor(n / hopLength),
        ),
        () => getSonareModule().chromaCens(samples, sampleRate, hopLength, nChroma, binsPerOctave),
      ),
    );
  });
}

/**
 * Constant-Q chromagram (librosa.feature.chroma_cqt).
 *
 * @returns Chroma result; `features` rows are chroma bins and cols are frames.
 */
export function chromaCqt(request: ChromaCqtRequest): ChromaResult;
export function chromaCqt(
  samples: Float32Array,
  sampleRate: number,
  options?: OptionsOf<ChromaCqtRequest>,
): ChromaResult;
export function chromaCqt(...args: unknown[]): ChromaResult {
  return withEntry('chromaCqt', () => {
    const r = sampleOneShot<ChromaCqtRequest>('chromaCqt', args);
    assertAudioInput(r.samples, r.sampleRate, r);
    const hopLength = r.hopLength ?? 512;
    const nChroma = r.nChroma ?? 12;
    const binsPerOctave = r.binsPerOctave ?? 36;
    const { samples, sampleRate } = r;
    return chromaResult(
      withHeapNeedWhen(
        cqtChromaInDomain(hopLength, nChroma, binsPerOctave),
        sampleNeed(
          'samples',
          samples.length,
          sampleRate,
          (n) => 4 * n + 4 * nChroma * Math.floor(n / hopLength),
        ),
        () => getSonareModule().chromaCqt(samples, sampleRate, hopLength, nChroma, binsPerOctave),
      ),
    );
  });
}

/**
 * Low-frequency bass chroma.
 *
 * @returns Chroma result; `features` rows are chroma bins and cols are frames.
 */
export function bassChroma(request: BassChromaRequest): ChromaResult;
export function bassChroma(
  samples: Float32Array,
  sampleRate: number,
  options?: OptionsOf<BassChromaRequest>,
): ChromaResult;
export function bassChroma(...args: unknown[]): ChromaResult {
  return withEntry('bassChroma', () => {
    const r = sampleOneShot<BassChromaRequest>('bassChroma', args);
    assertAudioInput(r.samples, r.sampleRate, r);
    const hopLength = r.hopLength ?? 512;
    const nChroma = r.nChroma ?? 12;
    const { samples, sampleRate } = r;
    return chromaResult(
      withHeapNeedWhen(
        cqtChromaInDomain(hopLength, nChroma, 36),
        sampleNeed(
          'samples',
          samples.length,
          sampleRate,
          (n) => 4 * n + 4 * nChroma * Math.floor(n / hopLength),
        ),
        () => getSonareModule().bassChroma(samples, sampleRate, hopLength, nChroma),
      ),
    );
  });
}

// ============================================================================
// Features - Mel Spectrogram
// ============================================================================

/** Request for {@link melFilterbank}. */
export interface MelFilterbankRequest {
  /** Sample rate in Hz the spectrum was taken at. */
  sampleRate: number;
  /** FFT size; the filterbank spans its `nFft / 2 + 1` bins. */
  nFft: number;
  /** Number of bands, in [1, 4096]. Default 128. */
  nMels?: number;
  /** Lower edge of the lowest band in Hz. Default 0. */
  fminHz?: number;
  /** Upper edge of the highest band in Hz. Absent means the Nyquist; when given it must be greater than 0 and at most the Nyquist. */
  fmaxHz?: number;
  /** HTK mel formula instead of Slaney. Default false. */
  htk?: boolean;
  /** Band weighting. Default `'slaney'`. */
  norm?: MelNorm;
}

/** Request for {@link melFrequencies}. */
export interface MelFrequenciesRequest {
  /** Number of points, at least 1. Default 128. */
  nMels?: number;
  /** First point in Hz. Default 0. */
  fminHz?: number;
  /** Last point in Hz; greater than `fminHz`. Default 11025. */
  fmaxHz?: number;
  /** HTK mel formula instead of Slaney. Default false. */
  htk?: boolean;
}

const MEL_NORMS: readonly MelNorm[] = ['none', 'slaney'];

/**
 * The mel filterbank that maps an STFT power spectrum onto `nMels` bands, as rows = bands,
 * cols = FFT bins. Band `m` is a triangle over the bins rising from point `m` of
 * `melFrequencies({ nMels: nMels + 2, fminHz, fmaxHz, htk })` to its peak at point `m + 1`
 * and falling to point `m + 2`.
 *
 * A wrong type is a `TypeError`; a supplied `fmaxHz` must be positive and no greater than
 * Nyquist, and the resolved upper edge must be above `fminHz`.
 */
export function melFilterbank(request: MelFilterbankRequest): Matrix2D;
export function melFilterbank(...args: unknown[]): Matrix2D {
  return withEntry('melFilterbank', () => {
    const r = requestOnly<MelFilterbankRequest>('melFilterbank', args);
    const htk = optionalBoolean(r.htk, 'htk', false);
    const norm = r.norm ?? 'slaney';
    assertClosedToken(norm, MEL_NORMS, 'norm');
    assertOptionalFmaxNotZero(r.fmaxHz);
    const native = getSonareModule().melFilterbank(
      r.sampleRate,
      r.nFft,
      r.nMels ?? 128,
      r.fminHz ?? 0,
      r.fmaxHz ?? 0,
      htk,
      MEL_NORMS.indexOf(norm),
    );
    return toMatrix2D(native.data, native.nMels, native.nBins);
  });
}

/**
 * `nMels` frequencies in Hz equally spaced on the mel scale from `fminHz` to `fmaxHz`, both
 * included; one point is `fminHz` alone.
 *
 * A wrong type is a `TypeError`; `nMels` below 1, a negative or non-finite `fminHz`, or an
 * `fmaxHz` that is not finite or not above `fminHz` is a `RangeError`.
 */
export function melFrequencies(request: MelFrequenciesRequest): Float32Array;
export function melFrequencies(...args: unknown[]): Float32Array {
  return withEntry('melFrequencies', () => {
    const r = requestOnly<MelFrequenciesRequest>('melFrequencies', args);
    return getSonareModule().melFrequencies(
      r.nMels ?? 128,
      r.fminHz ?? 0,
      r.fmaxHz ?? 11025,
      optionalBoolean(r.htk, 'htk', false),
    );
  });
}

/**
 * Mel spectrogram.
 *
 * `fminHz` and `fmaxHz` set together round-trip with `melToStft` / `melToAudio`.
 *
 * @returns Mel result; `power` and `db` rows are mel bands and cols are frames.
 */
export function melSpectrogram(request: MelSpectrogramRequest): MelSpectrogramResult;
export function melSpectrogram(
  samples: Float32Array,
  sampleRate: number,
  options?: OptionsOf<MelSpectrogramRequest>,
): MelSpectrogramResult;
export function melSpectrogram(...args: unknown[]): MelSpectrogramResult {
  return withEntry('melSpectrogram', () => {
    const r = sampleOneShot<MelSpectrogramRequest>('melSpectrogram', args);
    assertAudioInput(r.samples, r.sampleRate, r);
    const fft = resolveFftOptions(r.nFft, r.hopLength);
    const nMels = r.nMels ?? 128;
    const fminHz = r.fminHz ?? 0;
    const fmaxHz = r.fmaxHz ?? 0;
    assertNumber(fminHz, 'fminHz');
    assertOptionalFmaxNotZero(r.fmaxHz);
    const { samples, sampleRate } = r;
    const bins = stftBins(fft.nFft, fft.hopLength);
    const native = withHeapNeedWhen(
      bins !== undefined && isInt(nMels) && nMels >= 1 && nMels <= MAX_MEL_BANDS,
      sampleNeed(
        'samples',
        samples.length,
        sampleRate,
        (n) => 4 * n + 8 * nMels * stftFrames(n, fft.hopLength),
      ),
      () =>
        getSonareModule().melSpectrogram(
          samples,
          sampleRate,
          fft.nFft,
          fft.hopLength,
          nMels,
          fminHz,
          fmaxHz,
          r.htk ?? false,
        ),
    );
    return {
      sampleRate: native.sampleRate,
      hopLength: native.hopLength,
      nFft: native.nFft,
      fminHz: native.fmin,
      fmaxHz: native.fmax,
      htk: native.htk,
      power: toMatrix2D(native.power, native.nMels, native.nFrames),
      db: toMatrix2D(native.db, native.nMels, native.nFrames),
    };
  });
}

/**
 * Mel-frequency cepstral coefficients.
 *
 * @returns MFCC result; `coefficients` rows are coefficients and cols are frames.
 */
export function mfcc(request: MfccRequest): MfccResult;
export function mfcc(
  samples: Float32Array,
  sampleRate: number,
  options?: OptionsOf<MfccRequest>,
): MfccResult;
export function mfcc(...args: unknown[]): MfccResult {
  return withEntry('mfcc', () => {
    const r = sampleOneShot<MfccRequest>('mfcc', args);
    assertAudioInput(r.samples, r.sampleRate, r);
    const fft = resolveFftOptions(r.nFft, r.hopLength);
    const nMels = r.nMels ?? 128;
    const nMfcc = r.nMfcc ?? 20;
    const fminHz = r.fminHz ?? 0;
    const fmaxHz = r.fmaxHz ?? 0;
    assertNumber(fminHz, 'fminHz');
    assertOptionalFmaxNotZero(r.fmaxHz);
    const { samples, sampleRate } = r;
    const bins = stftBins(fft.nFft, fft.hopLength);
    const native = withHeapNeedWhen(
      bins !== undefined &&
        isInt(nMels) &&
        nMels >= 1 &&
        nMels <= MAX_MEL_BANDS &&
        isInt(nMfcc) &&
        nMfcc >= 1 &&
        nMfcc <= nMels,
      sampleNeed(
        'samples',
        samples.length,
        sampleRate,
        (n) => 4 * n + 4 * (nMels + nMfcc) * stftFrames(n, fft.hopLength),
      ),
      () =>
        getSonareModule().mfcc(
          samples,
          sampleRate,
          fft.nFft,
          fft.hopLength,
          nMels,
          nMfcc,
          fminHz,
          fmaxHz,
          r.htk ?? false,
          r.lifter ?? 0,
        ),
    );
    return {
      sampleRate: native.sampleRate,
      hopLength: native.hopLength,
      nFft: native.nFft,
      nMels: native.nMels,
      fminHz: native.fmin,
      fmaxHz: native.fmax,
      htk: native.htk,
      lifter: native.lifter,
      coefficients: toMatrix2D(native.coefficients, native.nMfcc, native.nFrames),
    };
  });
}

/**
 * First-order regression delta of a feature matrix.
 *
 * @returns {@link Matrix2D} of the input's shape: rows are features, cols are frames.
 */
export function melDelta(request: MelDeltaRequest): Matrix2D;
export function melDelta(...args: unknown[]): Matrix2D {
  return withEntry('melDelta', () => {
    const r = requestOnly<MelDeltaRequest>('melDelta', args);
    const features = assertMatrix2D(r.features, 'features');
    const width = r.width ?? 9;
    const out = withHeapNeedWhen(
      isInt(width) && width >= 3 && width % 2 === 1,
      {
        subject: 'features',
        size: features.data.length,
        bytes: (n) => 4 * n + 4 * n,
        monotone: true,
      },
      () => getSonareModule().melDelta(features.data, features.rows, features.cols, width),
    );
    return toMatrix2D(out, features.rows, features.cols);
  });
}

/**
 * Auger-Flandrin reassigned spectrogram.
 *
 * @returns Magnitude, time and frequency matrices whose rows are FFT bins and cols are frames.
 */
export function reassignedSpectrogram(
  request: ReassignedSpectrogramRequest,
): ReassignedSpectrogramResult;
export function reassignedSpectrogram(
  samples: Float32Array,
  sampleRate: number,
  options?: OptionsOf<ReassignedSpectrogramRequest>,
): ReassignedSpectrogramResult;
export function reassignedSpectrogram(...args: unknown[]): ReassignedSpectrogramResult {
  return withEntry('reassignedSpectrogram', () => {
    const r = sampleOneShot<ReassignedSpectrogramRequest>('reassignedSpectrogram', args);
    assertAudioInput(r.samples, r.sampleRate, r);
    const nFft = r.nFft ?? 2048;
    const hopLength = r.hopLength ?? 512;
    const refPower = r.refPower ?? 1e-6;
    assertNonNegativeScalar(refPower, 'refPower');
    const { samples, sampleRate } = r;
    const bins = stftBins(nFft, hopLength);
    const native = withHeapNeedWhen(
      bins !== undefined,
      sampleNeed(
        'samples',
        samples.length,
        sampleRate,
        (n) => 4 * n + 36 * (bins as number) * stftFrames(n, hopLength),
      ),
      () =>
        getSonareModule().reassignedSpectrogram(
          samples,
          sampleRate,
          nFft,
          hopLength,
          refPower,
          r.fillNan ?? false,
        ),
    );
    return {
      magnitude: toMatrix2D(native.magnitude, native.nBins, native.nFrames),
      times: toMatrix2D(native.times, native.nBins, native.nFrames),
      frequencies: toMatrix2D(native.frequencies, native.nBins, native.nFrames),
    };
  });
}

// ============================================================================
// Features - Chroma
// ============================================================================

/**
 * STFT chromagram (librosa.feature.chroma_stft).
 *
 * The chroma filterbank uses a fixed tuning of 0 (concert A440). Unlike
 * librosa.feature.chroma_stft — which estimates tuning from the signal when none
 * is given — this does NOT auto-estimate and takes no tuning argument. A tuning
 * offset in cents ({@link estimateTuning} returns one) is applied through
 * `analyze`'s `tuningCents` option (and to chords through `detectChords`); both
 * also measure it themselves with `tuningCents: 'auto'`.
 *
 * @returns Chroma result; `features` rows are chroma bins and cols are frames.
 */
export function chroma(request: ChromaRequest): ChromaResult;
export function chroma(
  samples: Float32Array,
  sampleRate: number,
  options?: OptionsOf<ChromaRequest>,
): ChromaResult;
export function chroma(...args: unknown[]): ChromaResult {
  return withEntry('chroma', () => {
    const r = sampleOneShot<ChromaRequest>('chroma', args);
    assertAudioInput(r.samples, r.sampleRate, r);
    const fft = resolveFftOptions(r.nFft, r.hopLength);
    const { samples, sampleRate } = r;
    return chromaResult(
      withHeapNeedWhen(
        stftBins(fft.nFft, fft.hopLength) !== undefined,
        sampleNeed(
          'samples',
          samples.length,
          sampleRate,
          (n) => 4 * n + 4 * 12 * stftFrames(n, fft.hopLength),
        ),
        () => getSonareModule().chroma(samples, sampleRate, fft.nFft, fft.hopLength),
      ),
    );
  });
}
