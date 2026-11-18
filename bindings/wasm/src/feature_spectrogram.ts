/**
 * Spectrogram and chroma representations, and the silence trim that shares
 * their input checks.
 */

import { validatePositiveIntegers } from './internal/feature_validation.js';
import { resolveFftOptions } from './internal/fft_options.js';
import { getSonareModule } from './module_state.js';
import type {
  ChromaResult,
  Matrix2D,
  MelSpectrogramResult,
  MfccResult,
  ReassignedSpectrogramResult,
  StftResult,
} from './public_types.js';
import type { WasmChromaResult } from './sonare.js';
import type { OptionsOf } from './validation.js';
import {
  assertAudioInput,
  assertFiniteScalar,
  assertFloatFields,
  assertMatrix2D,
  assertPositiveFloatValue,
  assertPositiveInteger,
  requestOnly,
  sampleOneShot,
  toMatrix2D,
} from './validation.js';

function requireModule() {
  return getSonareModule();
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
}

/** STFT-in-dB request. */
export interface StftDbRequest {
  /** Mono audio samples. */
  samples: Float32Array;
  /** Sample rate of `samples` in Hz. */
  sampleRate: number;
  /** FFT size. Default 2048. */
  nFft?: number;
  /** Hop in samples. Default 512. */
  hopLength?: number;
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
  /** Highest band edge in Hz. Absent means the Nyquist; an explicit 0 is refused (`RangeError`). */
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
  /** Highest band edge in Hz. Absent means the Nyquist; an explicit 0 is refused (`RangeError`). */
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
  const r = sampleOneShot<TrimRequest>('trim', args);
  const thresholdDb = r.thresholdDb ?? -60.0;
  const frameLength = r.frameLength ?? 2048;
  const hopLength = r.hopLength ?? 512;
  assertAudioInput('trim', r.samples, r.sampleRate, r);
  assertFiniteScalar('trim', thresholdDb, 'thresholdDb');
  assertPositiveInteger('trim', frameLength, 'frameLength');
  assertPositiveInteger('trim', hopLength, 'hopLength');
  return requireModule().trimEx(r.samples, r.sampleRate, thresholdDb, frameLength, hopLength);
}

// ============================================================================
// Features - Spectrogram
// ============================================================================

/**
 * Short-time Fourier transform.
 *
 * @returns Magnitude and power matrices whose rows are `nFft / 2 + 1` bins and cols are frames.
 */
export function stft(request: StftRequest): StftResult;
export function stft(
  samples: Float32Array,
  sampleRate: number,
  options?: OptionsOf<StftRequest>,
): StftResult;
export function stft(...args: unknown[]): StftResult {
  const r = sampleOneShot<StftRequest>('stft', args);
  assertAudioInput('stft', r.samples, r.sampleRate, r);
  const fft = resolveFftOptions('stft', r.nFft, r.hopLength);
  const native = requireModule().stft(r.samples, r.sampleRate, fft.nFft, fft.hopLength);
  return {
    nFft: native.nFft,
    hopLength: native.hopLength,
    sampleRate: native.sampleRate,
    magnitude: toMatrix2D(native.magnitude, native.nBins, native.nFrames),
    power: toMatrix2D(native.power, native.nBins, native.nFrames),
  };
}

/**
 * STFT magnitude in decibels.
 *
 * @returns {@link Matrix2D} whose rows are `nFft / 2 + 1` bins and cols are frames.
 */
export function stftDb(request: StftDbRequest): Matrix2D;
export function stftDb(
  samples: Float32Array,
  sampleRate: number,
  options?: OptionsOf<StftDbRequest>,
): Matrix2D;
export function stftDb(...args: unknown[]): Matrix2D {
  const r = sampleOneShot<StftDbRequest>('stftDb', args);
  assertAudioInput('stftDb', r.samples, r.sampleRate, r);
  const fft = resolveFftOptions('stftDb', r.nFft, r.hopLength);
  const native = requireModule().stftDb(r.samples, r.sampleRate, fft.nFft, fft.hopLength);
  return toMatrix2D(native.db, native.nBins, native.nFrames);
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
  const r = sampleOneShot<ChromaCensRequest>('chromaCens', args);
  assertAudioInput('chromaCens', r.samples, r.sampleRate, r);
  const hopLength = r.hopLength ?? 512;
  const nChroma = r.nChroma ?? 12;
  const binsPerOctave = r.binsPerOctave ?? 36;
  validatePositiveIntegers('chromaCens', { hopLength, nChroma, binsPerOctave });
  if (binsPerOctave % nChroma !== 0) {
    throw new RangeError('chromaCens: binsPerOctave must be a multiple of nChroma');
  }
  return chromaResult(
    requireModule().chromaCens(r.samples, r.sampleRate, hopLength, nChroma, binsPerOctave),
  );
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
  const r = sampleOneShot<ChromaCqtRequest>('chromaCqt', args);
  assertAudioInput('chromaCqt', r.samples, r.sampleRate, r);
  const hopLength = r.hopLength ?? 512;
  const nChroma = r.nChroma ?? 12;
  const binsPerOctave = r.binsPerOctave ?? 36;
  validatePositiveIntegers('chromaCqt', { hopLength, nChroma, binsPerOctave });
  if (binsPerOctave % nChroma !== 0) {
    throw new RangeError('chromaCqt: binsPerOctave must be a multiple of nChroma');
  }
  return chromaResult(
    requireModule().chromaCqt(r.samples, r.sampleRate, hopLength, nChroma, binsPerOctave),
  );
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
  const r = sampleOneShot<BassChromaRequest>('bassChroma', args);
  assertAudioInput('bassChroma', r.samples, r.sampleRate, r);
  const hopLength = r.hopLength ?? 512;
  const nChroma = r.nChroma ?? 12;
  validatePositiveIntegers('bassChroma', { hopLength, nChroma });
  return chromaResult(requireModule().bassChroma(r.samples, r.sampleRate, hopLength, nChroma));
}

// ============================================================================
// Features - Mel Spectrogram
// ============================================================================

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
  const r = sampleOneShot<MelSpectrogramRequest>('melSpectrogram', args);
  assertAudioInput('melSpectrogram', r.samples, r.sampleRate, r);
  const fft = resolveFftOptions('melSpectrogram', r.nFft, r.hopLength);
  const nMels = r.nMels ?? 128;
  validatePositiveIntegers('melSpectrogram', { nMels });
  const fminHz = r.fminHz ?? 0;
  const fmaxHz = r.fmaxHz ?? 0;
  assertFloatFields('melSpectrogram', { fminHz, fmaxHz });
  // An omitted fmaxHz is the core's Nyquist sentinel 0; a given one is a frequency.
  if (r.fmaxHz !== undefined) {
    assertPositiveFloatValue('melSpectrogram', fmaxHz, 'fmaxHz');
  }
  const native = requireModule().melSpectrogram(
    r.samples,
    r.sampleRate,
    fft.nFft,
    fft.hopLength,
    nMels,
    fminHz,
    fmaxHz,
    r.htk ?? false,
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
  const r = sampleOneShot<MfccRequest>('mfcc', args);
  assertAudioInput('mfcc', r.samples, r.sampleRate, r);
  const fft = resolveFftOptions('mfcc', r.nFft, r.hopLength);
  const nMels = r.nMels ?? 128;
  const nMfcc = r.nMfcc ?? 20;
  validatePositiveIntegers('mfcc', { nMels, nMfcc });
  const fminHz = r.fminHz ?? 0;
  const fmaxHz = r.fmaxHz ?? 0;
  assertFloatFields('mfcc', { fminHz, fmaxHz });
  // An omitted fmaxHz is the core's Nyquist sentinel 0; a given one is a frequency.
  if (r.fmaxHz !== undefined) {
    assertPositiveFloatValue('mfcc', fmaxHz, 'fmaxHz');
  }
  const native = requireModule().mfcc(
    r.samples,
    r.sampleRate,
    fft.nFft,
    fft.hopLength,
    nMels,
    nMfcc,
    fminHz,
    fmaxHz,
    r.htk ?? false,
    r.lifter ?? 0,
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
}

/**
 * First-order regression delta of a feature matrix.
 *
 * @returns {@link Matrix2D} of the input's shape: rows are features, cols are frames.
 */
export function melDelta(request: MelDeltaRequest): Matrix2D;
export function melDelta(...args: unknown[]): Matrix2D {
  const r = requestOnly<MelDeltaRequest>('melDelta', args);
  const features = assertMatrix2D('melDelta', r.features, 'features');
  const width = r.width ?? 9;
  assertPositiveInteger('melDelta', width, 'width');
  if (width < 3 || width % 2 === 0) {
    throw new RangeError('melDelta: width must be an odd integer of at least 3');
  }
  const out = requireModule().melDelta(features.data, features.rows, features.cols, width);
  return toMatrix2D(out, features.rows, features.cols);
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
  const r = sampleOneShot<ReassignedSpectrogramRequest>('reassignedSpectrogram', args);
  assertAudioInput('reassignedSpectrogram', r.samples, r.sampleRate, r);
  const nFft = r.nFft ?? 2048;
  const hopLength = r.hopLength ?? 512;
  const refPower = r.refPower ?? 1e-6;
  assertPositiveInteger('reassignedSpectrogram', nFft, 'nFft');
  assertPositiveInteger('reassignedSpectrogram', hopLength, 'hopLength');
  assertFiniteScalar('reassignedSpectrogram', refPower, 'refPower');
  if (refPower < 0) {
    throw new RangeError('reassignedSpectrogram: refPower must be non-negative');
  }
  const native = requireModule().reassignedSpectrogram(
    r.samples,
    r.sampleRate,
    nFft,
    hopLength,
    refPower,
    r.fillNan ?? false,
  );
  return {
    magnitude: toMatrix2D(native.magnitude, native.nBins, native.nFrames),
    times: toMatrix2D(native.times, native.nBins, native.nFrames),
    frequencies: toMatrix2D(native.frequencies, native.nBins, native.nFrames),
  };
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
  const r = sampleOneShot<ChromaRequest>('chroma', args);
  assertAudioInput('chroma', r.samples, r.sampleRate, r);
  const fft = resolveFftOptions('chroma', r.nFft, r.hopLength);
  return chromaResult(requireModule().chroma(r.samples, r.sampleRate, fft.nFft, fft.hopLength));
}
