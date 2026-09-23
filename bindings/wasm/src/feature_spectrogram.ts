/**
 * Spectrogram and chroma representations, and the silence trim that shares
 * their input checks.
 */

import type { GuardedOptions } from './_feature_validation';
import { validateMelFrequencyRange, validatePositiveIntegers } from './_feature_validation';
import { resolveFftOptions } from './_fft_options';
import { getSonareModule } from './module_state';
import type {
  ChromaResult,
  MelSpectrogramResult,
  MfccResult,
  ReassignedSpectrogramResult,
  StftResult,
} from './public_types';
import {
  assertFiniteScalar,
  assertPositiveInteger,
  assertSampleRate,
  assertSamples,
} from './validation';

function requireModule() {
  return getSonareModule();
}

/** Canonical request form for basic frame-based spectrogram features. */
export interface SpectrogramRequest extends GuardedOptions {
  samples: Float32Array;
  sampleRate?: number;
  nFft?: number;
  hopLength?: number;
}

/**
 * Options for the constant-Q chroma variants.
 *
 * Carries no `nFft`: these are built on a constant-Q transform, which resolves
 * frequency through per-bin filter lengths rather than a framed FFT, so there
 * is no FFT size to set. Use `chroma` for the STFT-framed chromagram.
 */
export interface ChromaSpectrogramRequest extends GuardedOptions {
  samples: Float32Array;
  sampleRate?: number;
  hopLength?: number;
  nChroma?: number;
  binsPerOctave?: number;
}

/**
 * Options for the bass-focused chroma.
 *
 * Unlike {@link ChromaSpectrogramRequest} this carries no `binsPerOctave`: the
 * bass chroma fixes its bin count and its lowest frequency together, so the
 * resolution is not independently settable through this entry point.
 */
export interface BassChromaSpectrogramRequest extends GuardedOptions {
  samples: Float32Array;
  sampleRate?: number;
  hopLength?: number;
  nChroma?: number;
}

/**
 * Compile-time guard for the two request shapes above. A field the entry point
 * cannot forward is worse than a missing one: it type-checks, runs, and returns
 * the default silently. These aliases fail to compile if either field appears,
 * so adding one has to be a deliberate act.
 */
type AbsentKey<T extends never> = T;

type _ChromaRequestHasNoFftSize = AbsentKey<Extract<keyof ChromaSpectrogramRequest, 'nFft'>>;

type _BassChromaRequestHasNoResolutionControls = AbsentKey<
  Extract<keyof BassChromaSpectrogramRequest, 'nFft' | 'binsPerOctave'>
>;

export interface MelSpectrogramRequest extends SpectrogramRequest {
  nMels?: number;
  fmin?: number;
  fmax?: number;
  htk?: boolean;
}

export interface MfccRequest extends MelSpectrogramRequest {
  nMfcc?: number;
  lifter?: number;
}

export interface MelDeltaRequest extends GuardedOptions {
  features: Float32Array;
  nFeatures: number;
  nFrames: number;
  width?: number;
}

export interface ReassignedSpectrogramRequest extends SpectrogramRequest {
  refPower?: number;
  fillNan?: boolean;
}

export interface TrimRequest extends GuardedOptions {
  samples: Float32Array;
  sampleRate: number;
  thresholdDb?: number;
  frameLength?: number;
  hopLength?: number;
}

function validateSpectrogramSamples(
  fnName: string,
  samples: Float32Array,
  sampleRate: number,
  options: GuardedOptions = {},
): void {
  assertSampleRate(fnName, sampleRate);
  assertSamples(fnName, samples, options.validate !== false);
}

/**
 * Trim silence from beginning and end of audio.
 *
 * @param samples - Audio samples (mono, float32)
 * @param sampleRate - Sample rate in Hz
 * @param thresholdDb - Silence threshold in dB (default: -60 dB)
 * @returns Trimmed audio
 */
export function trim(request: TrimRequest): Float32Array;
export function trim(
  samples: Float32Array,
  sampleRate: number,
  thresholdDb?: number,
  options?: GuardedOptions,
): Float32Array;
export function trim(
  samples: Float32Array,
  sampleRate: number,
  thresholdDb?: number,
  frameLength?: number,
  hopLength?: number,
  options?: GuardedOptions,
): Float32Array;
export function trim(
  samples: Float32Array | TrimRequest,
  sampleRate = 22050,
  thresholdDb = -60.0,
  frameLengthOrOptions?: number | GuardedOptions,
  hopLength?: number,
  options: GuardedOptions = {},
): Float32Array {
  if (!(samples instanceof Float32Array)) {
    const r = samples;
    return trim(r.samples, r.sampleRate, r.thresholdDb, r.frameLength, r.hopLength, r);
  }
  if (frameLengthOrOptions === null) {
    throw new TypeError('trim: frameLength must be an integer or options object');
  }
  if (
    frameLengthOrOptions !== undefined &&
    typeof frameLengthOrOptions !== 'number' &&
    typeof frameLengthOrOptions !== 'object'
  ) {
    throw new TypeError('trim: frameLength must be an integer or options object');
  }
  const positionalOptions =
    typeof frameLengthOrOptions === 'object' && frameLengthOrOptions !== null
      ? frameLengthOrOptions
      : options;
  const positionalFrameLength =
    typeof frameLengthOrOptions === 'number' ? frameLengthOrOptions : undefined;
  const resolvedFrameLength = positionalFrameLength ?? 2048;
  const resolvedHopLength = hopLength === undefined ? 512 : hopLength;
  validateSpectrogramSamples('trim', samples, sampleRate, positionalOptions);
  assertFiniteScalar('trim', thresholdDb, 'thresholdDb');
  assertPositiveInteger('trim', resolvedFrameLength, 'frameLength');
  assertPositiveInteger('trim', resolvedHopLength, 'hopLength');
  if (resolvedFrameLength > 2 ** 31 - 1 || resolvedHopLength > 2 ** 31 - 1) {
    throw new RangeError('trim: frameLength and hopLength must fit in a signed 32-bit integer');
  }
  return requireModule().trimEx(
    samples,
    sampleRate,
    thresholdDb,
    resolvedFrameLength,
    resolvedHopLength,
  );
}

// ============================================================================
// Features - Spectrogram
// ============================================================================

/**
 * Compute Short-Time Fourier Transform (STFT).
 *
 * @param samples - Audio samples (mono, float32)
 * @param sampleRate - Sample rate in Hz (default: 22050)
 * @param nFft - FFT size (default: 2048)
 * @param hopLength - Hop length (default: 512)
 * @returns STFT result with magnitude and power spectrograms
 */
export function stft(request: SpectrogramRequest): StftResult;
export function stft(
  samples: Float32Array,
  sampleRate?: number,
  nFft?: number,
  hopLength?: number,
  options?: GuardedOptions,
): StftResult;
export function stft(
  samples: Float32Array | SpectrogramRequest,
  sampleRate = 22050,
  nFft = 2048,
  hopLength = 512,
  options: GuardedOptions = {},
): StftResult {
  if (!(samples instanceof Float32Array)) {
    const request = samples;
    return stft(request.samples, request.sampleRate, request.nFft, request.hopLength, request);
  }
  validateSpectrogramSamples('stft', samples, sampleRate, options);
  const fft = resolveFftOptions('stft', nFft, hopLength);
  return requireModule().stft(samples, sampleRate, fft.nFft, fft.hopLength);
}

/**
 * Compute STFT and return magnitude in decibels.
 *
 * @param samples - Audio samples (mono, float32)
 * @param sampleRate - Sample rate in Hz (default: 22050)
 * @param nFft - FFT size (default: 2048)
 * @param hopLength - Hop length (default: 512)
 * @returns STFT result with dB values
 */
export function stftDb(request: SpectrogramRequest): {
  nBins: number;
  nFrames: number;
  db: Float32Array;
};
export function stftDb(
  samples: Float32Array,
  sampleRate?: number,
  nFft?: number,
  hopLength?: number,
  options?: GuardedOptions,
): { nBins: number; nFrames: number; db: Float32Array };
export function stftDb(
  samples: Float32Array | SpectrogramRequest,
  sampleRate = 22050,
  nFft = 2048,
  hopLength = 512,
  options: GuardedOptions = {},
): { nBins: number; nFrames: number; db: Float32Array } {
  if (!(samples instanceof Float32Array)) {
    const request = samples;
    return stftDb(request.samples, request.sampleRate, request.nFft, request.hopLength, request);
  }
  validateSpectrogramSamples('stftDb', samples, sampleRate, options);
  const fft = resolveFftOptions('stftDb', nFft, hopLength);
  return requireModule().stftDb(samples, sampleRate, fft.nFft, fft.hopLength);
}

/**
 * Compute Chroma Energy Normalized Statistics.
 *
 * @param samples - Audio samples (mono, float32)
 * @param sampleRate - Sample rate in Hz (default: 22050)
 * @param hopLength - Hop length (default: 512)
 * @param nChroma - Number of chroma bins (default: 12)
 * @returns Chroma result
 */
export function chromaCens(request: ChromaSpectrogramRequest): ChromaResult;
export function chromaCens(
  samples: Float32Array,
  sampleRate?: number,
  hopLength?: number,
  nChroma?: number,
  binsPerOctave?: number,
  options?: GuardedOptions,
): ChromaResult;
export function chromaCens(
  samples: Float32Array | ChromaSpectrogramRequest,
  sampleRate = 22050,
  hopLength = 512,
  nChroma = 12,
  binsPerOctave = 36,
  options: GuardedOptions = {},
): ChromaResult {
  if (!(samples instanceof Float32Array)) {
    const request = samples;
    return chromaCens(
      request.samples,
      request.sampleRate,
      request.hopLength,
      request.nChroma,
      request.binsPerOctave,
      request,
    );
  }
  validateSpectrogramSamples('chromaCens', samples, sampleRate, options);
  validatePositiveIntegers('chromaCens', { hopLength, nChroma, binsPerOctave });
  if (binsPerOctave % nChroma !== 0) {
    throw new RangeError('chromaCens: binsPerOctave must be a multiple of nChroma');
  }
  return requireModule().chromaCens(samples, sampleRate, hopLength, nChroma, binsPerOctave);
}

/**
 * Compute a constant-Q chromagram (librosa.feature.chroma_cqt).
 *
 * @param samples - Audio samples (mono, float32)
 * @param sampleRate - Sample rate in Hz (default: 22050)
 * @param hopLength - Hop length (default: 512)
 * @param nChroma - Number of chroma bins (default: 12)
 * @returns Chroma result
 */
export function chromaCqt(request: ChromaSpectrogramRequest): ChromaResult;
export function chromaCqt(
  samples: Float32Array,
  sampleRate?: number,
  hopLength?: number,
  nChroma?: number,
  binsPerOctave?: number,
  options?: GuardedOptions,
): ChromaResult;
export function chromaCqt(
  samples: Float32Array | ChromaSpectrogramRequest,
  sampleRate = 22050,
  hopLength = 512,
  nChroma = 12,
  binsPerOctave = 36,
  options: GuardedOptions = {},
): ChromaResult {
  if (!(samples instanceof Float32Array)) {
    const request = samples;
    return chromaCqt(
      request.samples,
      request.sampleRate,
      request.hopLength,
      request.nChroma,
      request.binsPerOctave,
      request,
    );
  }
  validateSpectrogramSamples('chromaCqt', samples, sampleRate, options);
  validatePositiveIntegers('chromaCqt', { hopLength, nChroma, binsPerOctave });
  if (binsPerOctave % nChroma !== 0) {
    throw new RangeError('chromaCqt: binsPerOctave must be a multiple of nChroma');
  }
  return requireModule().chromaCqt(samples, sampleRate, hopLength, nChroma, binsPerOctave);
}

/**
 * Compute low-frequency bass chroma.
 *
 * @param samples - Audio samples (mono, float32)
 * @param sampleRate - Sample rate in Hz (default: 22050)
 * @param hopLength - Hop length (default: 512)
 * @param nChroma - Number of chroma bins (default: 12)
 * @returns Chroma result
 */
export function bassChroma(request: BassChromaSpectrogramRequest): ChromaResult;
export function bassChroma(
  samples: Float32Array,
  sampleRate?: number,
  hopLength?: number,
  nChroma?: number,
  options?: GuardedOptions,
): ChromaResult;
export function bassChroma(
  samples: Float32Array | BassChromaSpectrogramRequest,
  sampleRate = 22050,
  hopLength = 512,
  nChroma = 12,
  options: GuardedOptions = {},
): ChromaResult {
  if (!(samples instanceof Float32Array)) {
    const request = samples;
    return bassChroma(
      request.samples,
      request.sampleRate,
      request.hopLength,
      request.nChroma,
      request,
    );
  }
  validateSpectrogramSamples('bassChroma', samples, sampleRate, options);
  validatePositiveIntegers('bassChroma', { hopLength, nChroma });
  return requireModule().bassChroma(samples, sampleRate, hopLength, nChroma);
}

// ============================================================================
// Features - Mel Spectrogram
// ============================================================================

/**
 * Compute Mel spectrogram.
 *
 * @param samples - Audio samples (mono, float32)
 * @param sampleRate - Sample rate in Hz (default: 22050)
 * @param nFft - FFT size (default: 2048)
 * @param hopLength - Hop length (default: 512)
 * @param nMels - Number of Mel bands (default: 128)
 * @param fmin - Minimum Mel frequency in Hz (default: 0 = librosa default).
 *   Set with `fmax` to round-trip with `melToStft` / `melToAudio`.
 * @param fmax - Maximum Mel frequency in Hz (default: 0 = sampleRate / 2)
 * @param htk - Use the HTK Mel formula instead of Slaney (default: false)
 * @returns Mel spectrogram result
 */
export function melSpectrogram(request: MelSpectrogramRequest): MelSpectrogramResult;
export function melSpectrogram(
  samples: Float32Array,
  sampleRate?: number,
  nFft?: number,
  hopLength?: number,
  nMels?: number,
  fmin?: number,
  fmax?: number,
  htk?: boolean,
  options?: GuardedOptions,
): MelSpectrogramResult;
export function melSpectrogram(
  samples: Float32Array | MelSpectrogramRequest,
  sampleRate = 22050,
  nFft = 2048,
  hopLength = 512,
  nMels = 128,
  fmin = 0,
  fmax = 0,
  htk = false,
  options: GuardedOptions = {},
): MelSpectrogramResult {
  if (!(samples instanceof Float32Array)) {
    const request = samples;
    return melSpectrogram(
      request.samples,
      request.sampleRate,
      request.nFft,
      request.hopLength,
      request.nMels,
      request.fmin,
      request.fmax,
      request.htk,
      request,
    );
  }
  validateSpectrogramSamples('melSpectrogram', samples, sampleRate, options);
  const fft = resolveFftOptions('melSpectrogram', nFft, hopLength);
  validatePositiveIntegers('melSpectrogram', { nMels });
  validateMelFrequencyRange('melSpectrogram', fmin, fmax, sampleRate);
  return requireModule().melSpectrogram(
    samples,
    sampleRate,
    fft.nFft,
    fft.hopLength,
    nMels,
    fmin,
    fmax,
    htk,
  );
}

/**
 * Compute MFCC (Mel-Frequency Cepstral Coefficients).
 *
 * @param samples - Audio samples (mono, float32)
 * @param sampleRate - Sample rate in Hz (default: 22050)
 * @param nFft - FFT size (default: 2048)
 * @param hopLength - Hop length (default: 512)
 * @param nMels - Number of Mel bands (default: 128)
 * @param nMfcc - Number of MFCC coefficients (default: 20)
 * @param fmin - Minimum Mel frequency in Hz (default: 0 = librosa default)
 * @param fmax - Maximum Mel frequency in Hz (default: 0 = sampleRate / 2)
 * @param htk - Use the HTK Mel formula instead of Slaney (default: false)
 * @param lifter - Cepstral liftering coefficient (default: 0 = no liftering)
 * @returns MFCC result
 */
export function mfcc(request: MfccRequest): MfccResult;
export function mfcc(
  samples: Float32Array,
  sampleRate?: number,
  nFft?: number,
  hopLength?: number,
  nMels?: number,
  nMfcc?: number,
  fmin?: number,
  fmax?: number,
  htk?: boolean,
  lifter?: number,
  options?: GuardedOptions,
): MfccResult;
export function mfcc(
  samples: Float32Array | MfccRequest,
  sampleRate = 22050,
  nFft = 2048,
  hopLength = 512,
  nMels = 128,
  nMfcc = 20,
  fmin = 0,
  fmax = 0,
  htk = false,
  lifter = 0,
  options: GuardedOptions = {},
): MfccResult {
  if (!(samples instanceof Float32Array)) {
    const request = samples;
    return mfcc(
      request.samples,
      request.sampleRate,
      request.nFft,
      request.hopLength,
      request.nMels,
      request.nMfcc,
      request.fmin,
      request.fmax,
      request.htk,
      request.lifter,
      request,
    );
  }
  validateSpectrogramSamples('mfcc', samples, sampleRate, options);
  const fft = resolveFftOptions('mfcc', nFft, hopLength);
  validatePositiveIntegers('mfcc', { nMels, nMfcc });
  validateMelFrequencyRange('mfcc', fmin, fmax, sampleRate);
  return requireModule().mfcc(
    samples,
    sampleRate,
    fft.nFft,
    fft.hopLength,
    nMels,
    nMfcc,
    fmin,
    fmax,
    htk,
    lifter,
  );
}

/** First-order regression delta of a row-major feature matrix. */
export function melDelta(request: MelDeltaRequest): Float32Array;
export function melDelta(
  features: Float32Array,
  nFeatures: number,
  nFrames: number,
  width?: number,
): Float32Array;
export function melDelta(
  features: Float32Array | MelDeltaRequest,
  nFeatures?: number,
  nFrames?: number,
  width = 9,
): Float32Array {
  const request: MelDeltaRequest =
    features instanceof Float32Array
      ? { features, nFeatures: nFeatures ?? 0, nFrames: nFrames ?? 0, width }
      : features;
  assertPositiveInteger('melDelta', request.nFeatures, 'nFeatures');
  assertPositiveInteger('melDelta', request.nFrames, 'nFrames');
  assertPositiveInteger('melDelta', request.width ?? 9, 'width');
  if ((request.width ?? 9) < 3 || (request.width ?? 9) % 2 === 0) {
    throw new RangeError('melDelta: width must be an odd integer of at least 3');
  }
  if (request.features.length !== request.nFeatures * request.nFrames) {
    throw new RangeError('melDelta: feature matrix length must equal nFeatures * nFrames');
  }
  return requireModule().melDelta(
    request.features,
    request.nFeatures,
    request.nFrames,
    request.width ?? 9,
  );
}

/** Auger-Flandrin reassigned spectrogram (row-major [nBins x nFrames] arrays). */
export function reassignedSpectrogram(
  request: ReassignedSpectrogramRequest,
): ReassignedSpectrogramResult;
export function reassignedSpectrogram(
  samples: Float32Array,
  sampleRate?: number,
  nFft?: number,
  hopLength?: number,
  refPower?: number,
  fillNan?: boolean,
): ReassignedSpectrogramResult;
export function reassignedSpectrogram(
  samples: Float32Array | ReassignedSpectrogramRequest,
  sampleRate = 22050,
  nFft = 2048,
  hopLength = 512,
  refPower = 1e-6,
  fillNan = false,
): ReassignedSpectrogramResult {
  if (!(samples instanceof Float32Array)) {
    const request = samples;
    return reassignedSpectrogram(
      request.samples,
      request.sampleRate,
      request.nFft,
      request.hopLength,
      request.refPower,
      request.fillNan,
    );
  }
  assertSamples('reassignedSpectrogram', samples, true);
  assertSampleRate('reassignedSpectrogram', sampleRate);
  assertPositiveInteger('reassignedSpectrogram', nFft, 'nFft');
  assertPositiveInteger('reassignedSpectrogram', hopLength, 'hopLength');
  assertFiniteScalar('reassignedSpectrogram', refPower, 'refPower');
  if (refPower < 0) {
    throw new RangeError('reassignedSpectrogram: refPower must be non-negative');
  }
  return requireModule().reassignedSpectrogram(
    samples,
    sampleRate,
    nFft,
    hopLength,
    refPower,
    fillNan,
  );
}

// ============================================================================
// Features - Chroma
// ============================================================================

/**
 * Compute STFT chromagram (librosa.feature.chroma_stft).
 *
 * The chroma filterbank uses a fixed tuning of 0 (concert A440). Unlike
 * librosa.feature.chroma_stft — which estimates tuning from the signal when none
 * is given — this does NOT auto-estimate and takes no tuning argument. A
 * tuning offset from {@link estimateTuning} is applied through `analyze`'s
 * `tuning` option (and to chords through `detectChords`).
 *
 * @param samples - Audio samples (mono, float32)
 * @param sampleRate - Sample rate in Hz (default: 22050)
 * @param nFft - FFT size (default: 2048)
 * @param hopLength - Hop length (default: 512)
 * @returns Chroma features result
 */
export function chroma(request: SpectrogramRequest): ChromaResult;
export function chroma(
  samples: Float32Array,
  sampleRate?: number,
  nFft?: number,
  hopLength?: number,
  options?: GuardedOptions,
): ChromaResult;
export function chroma(
  samples: Float32Array | SpectrogramRequest,
  sampleRate = 22050,
  nFft = 2048,
  hopLength = 512,
  options: GuardedOptions = {},
): ChromaResult {
  if (!(samples instanceof Float32Array)) {
    const request = samples;
    return chroma(request.samples, request.sampleRate, request.nFft, request.hopLength, request);
  }
  validateSpectrogramSamples('chroma', samples, sampleRate, options);
  const fft = resolveFftOptions('chroma', nFft, hopLength);
  return requireModule().chroma(samples, sampleRate, fft.nFft, fft.hopLength);
}
