import { getSonareModule } from './module_state';
import type {
  ChromaResult,
  MelPowerResult,
  MelSpectrogramResult,
  MfccResult,
  ReassignedSpectrogramResult,
  StftPowerResult,
  StftResult,
} from './public_types';
import type { ValidateOptions } from './validation';
import {
  assertFiniteScalar,
  assertPositiveInteger,
  assertSampleRate,
  assertSamples,
} from './validation';

function requireModule() {
  return getSonareModule();
}

type GuardedOptions = ValidateOptions;

/** Canonical request form for basic frame-based spectrogram features. */
export interface SpectrogramRequest extends GuardedOptions {
  samples: Float32Array;
  sampleRate?: number;
  nFft?: number;
  hopLength?: number;
}

export interface ChromaSpectrogramRequest extends GuardedOptions {
  samples: Float32Array;
  sampleRate?: number;
  hopLength?: number;
  nChroma?: number;
  binsPerOctave?: number;
}

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

export interface MfccToMelRequest extends GuardedOptions {
  mfccCoefficients: Float32Array;
  nMfcc: number;
  nFrames: number;
  nMels?: number;
  /** Lifter used by the forward MFCC transform; zero means no liftering. */
  lifter?: number;
}

/** Canonical request form for reconstruction from a Mel power spectrogram. */
export interface MelToStftRequest extends GuardedOptions {
  melPower: Float32Array;
  nMels: number;
  nFrames: number;
  sampleRate?: number;
  nFft?: number;
  fmin?: number;
  fmax?: number;
  htk?: boolean;
}

/** Canonical request form for Griffin-Lim reconstruction from Mel power. */
export interface MelToAudioRequest extends MelToStftRequest {
  hopLength?: number;
  nIter?: number;
}
export interface GriffinLimRequest extends GuardedOptions {
  magnitude: Float32Array;
  nBins: number;
  nFrames: number;
  sampleRate?: number;
  nFft?: number;
  hopLength?: number;
  nIter?: number;
  momentum?: number;
}

/** Canonical request form for Griffin-Lim reconstruction from MFCCs. */
export interface MfccToAudioRequest extends MfccToMelRequest {
  sampleRate?: number;
  nFft?: number;
  hopLength?: number;
  fmin?: number;
  fmax?: number;
  nIter?: number;
  htk?: boolean;
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

function validatePositiveIntegers(fnName: string, values: Record<string, number>): void {
  for (const [name, value] of Object.entries(values)) {
    assertPositiveInteger(fnName, value, name);
  }
}

function validateMelFrequencyRange(
  fnName: string,
  fmin: number,
  fmax: number,
  sampleRate: number,
): void {
  assertFiniteScalar(fnName, fmin, 'fmin');
  assertFiniteScalar(fnName, fmax, 'fmax');
  if (fmin < 0) {
    throw new RangeError(`${fnName}: fmin must be non-negative`);
  }
  if (fmax < 0) {
    throw new RangeError(`${fnName}: fmax must be non-negative`);
  }
  const effectiveFmax = fmax === 0 ? sampleRate / 2 : fmax;
  if (effectiveFmax <= fmin) {
    throw new RangeError(`${fnName}: fmax must be greater than fmin`);
  }
}

function validateMatrix(
  fnName: string,
  data: Float32Array,
  rows: number,
  frames: number,
  dataName: string,
  rowName: string,
  options: GuardedOptions = {},
): void {
  validatePositiveIntegers(fnName, { [rowName]: rows, nFrames: frames });
  assertSamples(fnName, data, options.validate !== false, dataName);
  const expectedLength = rows * frames;
  if (!Number.isSafeInteger(expectedLength) || data.length !== expectedLength) {
    throw new RangeError(`${fnName}: ${dataName} length must equal ${rowName} * nFrames`);
  }
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
  validatePositiveIntegers('stft', { nFft, hopLength });
  return requireModule().stft(samples, sampleRate, nFft, hopLength);
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
  validatePositiveIntegers('stftDb', { nFft, hopLength });
  return requireModule().stftDb(samples, sampleRate, nFft, hopLength);
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
export function bassChroma(request: ChromaSpectrogramRequest): ChromaResult;
export function bassChroma(
  samples: Float32Array,
  sampleRate?: number,
  hopLength?: number,
  nChroma?: number,
  options?: GuardedOptions,
): ChromaResult;
export function bassChroma(
  samples: Float32Array | ChromaSpectrogramRequest,
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
  validatePositiveIntegers('melSpectrogram', { nFft, hopLength, nMels });
  validateMelFrequencyRange('melSpectrogram', fmin, fmax, sampleRate);
  return requireModule().melSpectrogram(
    samples,
    sampleRate,
    nFft,
    hopLength,
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
  validatePositiveIntegers('mfcc', { nFft, hopLength, nMels, nMfcc });
  validateMelFrequencyRange('mfcc', fmin, fmax, sampleRate);
  return requireModule().mfcc(
    samples,
    sampleRate,
    nFft,
    hopLength,
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
// Features - Inverse reconstruction
// ============================================================================

/**
 * Approximate inverse of a Mel filterbank: Mel power spectrogram -> STFT power
 * spectrogram. Mirrors `feature::mel_to_stft`.
 *
 * @param melPower - Mel power spectrogram [nMels x nFrames] row-major
 * @param nMels - Number of Mel bands
 * @param nFrames - Number of time frames
 * @param sampleRate - Sample rate in Hz
 * @param nFft - FFT size (default: 2048)
 * @param fmin - Lower Mel band edge in Hz (default: 0)
 * @param fmax - Upper Mel band edge in Hz (default: sr/2 when 0)
 * @param htk - Use the HTK Mel formula instead of Slaney (default: false)
 * @returns STFT power spectrogram result
 */
export function melToStft(request: MelToStftRequest): StftPowerResult;
export function melToStft(
  melPower: Float32Array,
  nMels: number,
  nFrames: number,
  sampleRate?: number,
  nFft?: number,
  fmin?: number,
  fmax?: number,
  htk?: boolean,
  options?: GuardedOptions,
): StftPowerResult;
export function melToStft(
  melPower: Float32Array | MelToStftRequest,
  nMels = 0,
  nFrames = 0,
  sampleRate = 22050,
  nFft = 2048,
  fmin = 0,
  fmax = 0,
  htk = false,
  options: GuardedOptions = {},
): StftPowerResult {
  if (!(melPower instanceof Float32Array)) {
    const request = melPower;
    return melToStft(
      request.melPower,
      request.nMels,
      request.nFrames,
      request.sampleRate,
      request.nFft,
      request.fmin,
      request.fmax,
      request.htk,
      request,
    );
  }
  assertSampleRate('melToStft', sampleRate);
  validateMatrix('melToStft', melPower, nMels, nFrames, 'melPower', 'nMels', options);
  validatePositiveIntegers('melToStft', { nFft });
  validateMelFrequencyRange('melToStft', fmin, fmax, sampleRate);
  return requireModule().melToStft(melPower, nMels, nFrames, sampleRate, nFft, fmin, fmax, htk);
}

/**
 * Reconstruct audio from a Mel power spectrogram via Griffin-Lim. Mirrors
 * `feature::mel_to_audio`.
 *
 * @param melPower - Mel power spectrogram [nMels x nFrames] row-major
 * @param nMels - Number of Mel bands
 * @param nFrames - Number of time frames
 * @param sampleRate - Sample rate in Hz
 * @param nFft - FFT size (default: 2048)
 * @param hopLength - Hop length (default: 512)
 * @param fmin - Minimum Mel frequency in Hz (default: 0)
 * @param fmax - Maximum Mel frequency in Hz (default: 0 = sr/2)
 * @param nIter - Griffin-Lim iterations (default: 32)
 * @param htk - Use the HTK Mel formula instead of Slaney (default: false)
 * @returns Reconstructed audio samples (mono, float32)
 */
export function melToAudio(request: MelToAudioRequest): Float32Array;
export function melToAudio(
  melPower: Float32Array,
  nMels: number,
  nFrames: number,
  sampleRate?: number,
  nFft?: number,
  hopLength?: number,
  fmin?: number,
  fmax?: number,
  nIter?: number,
  htk?: boolean,
  options?: GuardedOptions,
): Float32Array;
export function melToAudio(
  melPower: Float32Array | MelToAudioRequest,
  nMels = 0,
  nFrames = 0,
  sampleRate = 22050,
  nFft = 2048,
  hopLength = 512,
  fmin = 0,
  fmax = 0,
  nIter = 32,
  htk = false,
  options: GuardedOptions = {},
): Float32Array {
  if (!(melPower instanceof Float32Array)) {
    const request = melPower;
    return melToAudio(
      request.melPower,
      request.nMels,
      request.nFrames,
      request.sampleRate,
      request.nFft,
      request.hopLength,
      request.fmin,
      request.fmax,
      request.nIter,
      request.htk,
      request,
    );
  }
  assertSampleRate('melToAudio', sampleRate);
  validateMatrix('melToAudio', melPower, nMels, nFrames, 'melPower', 'nMels', options);
  validatePositiveIntegers('melToAudio', { nFft, hopLength, nIter });
  validateMelFrequencyRange('melToAudio', fmin, fmax, sampleRate);
  return requireModule().melToAudio(
    melPower,
    nMels,
    nFrames,
    sampleRate,
    nFft,
    hopLength,
    fmin,
    fmax,
    nIter,
    htk,
  );
}

/** Reconstruct audio from an STFT magnitude matrix via Griffin-Lim. */
export function griffinLim(request: GriffinLimRequest): Float32Array;
export function griffinLim(
  magnitude: Float32Array,
  nBins: number,
  nFrames: number,
  sampleRate?: number,
  nFft?: number,
  hopLength?: number,
  nIter?: number,
  momentum?: number,
  options?: GuardedOptions,
): Float32Array;
export function griffinLim(
  magnitude: Float32Array | GriffinLimRequest,
  nBins = 0,
  nFrames = 0,
  sampleRate = 22050,
  nFft = 2048,
  hopLength = 512,
  nIter = 32,
  momentum = 0.99,
  options: GuardedOptions = {},
): Float32Array {
  if (!(magnitude instanceof Float32Array)) {
    const request = magnitude;
    return griffinLim(
      request.magnitude,
      request.nBins,
      request.nFrames,
      request.sampleRate,
      request.nFft,
      request.hopLength,
      request.nIter,
      request.momentum,
      request,
    );
  }
  assertSampleRate('griffinLim', sampleRate);
  validateMatrix('griffinLim', magnitude, nBins, nFrames, 'magnitude', 'nBins', options);
  validatePositiveIntegers('griffinLim', { nFft, hopLength, nIter });
  return requireModule().griffinLim(
    magnitude,
    nBins,
    nFrames,
    sampleRate,
    nFft,
    hopLength,
    nIter,
    momentum,
  );
}

/**
 * Invert MFCC coefficients back to a Mel power spectrogram. Mirrors
 * `feature::mfcc_to_mel`.
 *
 * @param mfccCoefficients - MFCC matrix [nMfcc x nFrames] row-major
 * @param nMfcc - Number of MFCC coefficients
 * @param nFrames - Number of time frames
 * @param nMels - Number of Mel bins to reconstruct (default: 128)
 * @returns Mel power spectrogram result
 */
export function mfccToMel(request: MfccToMelRequest): MelPowerResult;
export function mfccToMel(
  mfccCoefficients: Float32Array,
  nMfcc: number,
  nFrames: number,
  nMels?: number,
  lifter?: number,
  options?: GuardedOptions,
): MelPowerResult;
export function mfccToMel(
  mfccCoefficients: Float32Array | MfccToMelRequest,
  nMfcc = 0,
  nFrames = 0,
  nMels = 128,
  lifter = 0,
  options: GuardedOptions = {},
): MelPowerResult {
  if (!(mfccCoefficients instanceof Float32Array)) {
    const request = mfccCoefficients;
    return mfccToMel(
      request.mfccCoefficients,
      request.nMfcc,
      request.nFrames,
      request.nMels,
      request.lifter,
      request,
    );
  }
  validateMatrix(
    'mfccToMel',
    mfccCoefficients,
    nMfcc,
    nFrames,
    'mfccCoefficients',
    'nMfcc',
    options,
  );
  validatePositiveIntegers('mfccToMel', { nMels });
  return requireModule().mfccToMel(mfccCoefficients, nMfcc, nFrames, nMels, lifter);
}

/**
 * Reconstruct audio directly from MFCC coefficients via Griffin-Lim. Mirrors
 * `feature::mfcc_to_audio`.
 *
 * @param mfccCoefficients - MFCC matrix [nMfcc x nFrames] row-major
 * @param nMfcc - Number of MFCC coefficients
 * @param nFrames - Number of time frames
 * @param nMels - Number of Mel bins (default: 128)
 * @param sampleRate - Sample rate in Hz (default: 22050)
 * @param nFft - FFT size (default: 2048)
 * @param hopLength - Hop length (default: 512)
 * @param fmin - Minimum Mel frequency in Hz (default: 0)
 * @param fmax - Maximum Mel frequency in Hz (default: 0 = sr/2)
 * @param nIter - Griffin-Lim iterations (default: 32)
 * @param htk - Use the HTK Mel formula instead of Slaney (default: false)
 * @returns Reconstructed audio samples (mono, float32)
 */
export function mfccToAudio(request: MfccToAudioRequest): Float32Array;
export function mfccToAudio(
  mfccCoefficients: Float32Array,
  nMfcc: number,
  nFrames: number,
  nMels?: number,
  sampleRate?: number,
  nFft?: number,
  hopLength?: number,
  fmin?: number,
  fmax?: number,
  nIter?: number,
  htk?: boolean,
  lifter?: number,
  options?: GuardedOptions,
): Float32Array;
export function mfccToAudio(
  mfccCoefficients: Float32Array | MfccToAudioRequest,
  nMfcc = 0,
  nFrames = 0,
  nMels = 128,
  sampleRate = 22050,
  nFft = 2048,
  hopLength = 512,
  fmin = 0,
  fmax = 0,
  nIter = 32,
  htk = false,
  lifter = 0,
  options: GuardedOptions = {},
): Float32Array {
  if (!(mfccCoefficients instanceof Float32Array)) {
    const request = mfccCoefficients;
    return mfccToAudio(
      request.mfccCoefficients,
      request.nMfcc,
      request.nFrames,
      request.nMels,
      request.sampleRate,
      request.nFft,
      request.hopLength,
      request.fmin,
      request.fmax,
      request.nIter,
      request.htk,
      request.lifter,
      request,
    );
  }
  assertSampleRate('mfccToAudio', sampleRate);
  validateMatrix(
    'mfccToAudio',
    mfccCoefficients,
    nMfcc,
    nFrames,
    'mfccCoefficients',
    'nMfcc',
    options,
  );
  validatePositiveIntegers('mfccToAudio', { nMels, nFft, hopLength, nIter });
  validateMelFrequencyRange('mfccToAudio', fmin, fmax, sampleRate);
  return requireModule().mfccToAudio(
    mfccCoefficients,
    nMfcc,
    nFrames,
    nMels,
    sampleRate,
    nFft,
    hopLength,
    fmin,
    fmax,
    nIter,
    htk,
    lifter,
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
 * is given — this does NOT auto-estimate and exposes no tuning argument, so
 * sharp/flat (non-A440) recordings smear across pitch classes. Estimate tuning
 * separately via {@link estimateTuning} if a non-A440 reference matters.
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
  validatePositiveIntegers('chroma', { nFft, hopLength });
  return requireModule().chroma(samples, sampleRate, nFft, hopLength);
}
