/**
 * Inverse transforms: reconstructing a linear spectrogram or audio from a mel
 * or MFCC representation, and the phase reconstruction they share.
 */

import type { GuardedOptions } from './_feature_validation';
import { validateMelFrequencyRange, validatePositiveIntegers } from './_feature_validation';
import { resolveFftOptions } from './_fft_options';
import type { SpectralFrameRequest } from './feature_spectral';
import { getSonareModule } from './module_state';
import type { MelPowerResult, StftPowerResult } from './public_types';
import { assertFiniteScalar, assertSampleRate, assertSamples } from './validation';

function requireModule() {
  return getSonareModule();
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
  // Not on the shared FFT rule: this inverts a filterbank rather than running a
  // transform, so `nFft` only sizes the output to `nFft / 2 + 1` bins and an odd
  // size is accepted by the core and by the Node surface alike.
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
  const fft = resolveFftOptions('melToAudio', nFft, hopLength);
  validatePositiveIntegers('melToAudio', { nIter });
  validateMelFrequencyRange('melToAudio', fmin, fmax, sampleRate);
  return requireModule().melToAudio(
    melPower,
    nMels,
    nFrames,
    sampleRate,
    fft.nFft,
    fft.hopLength,
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
  const fft = resolveFftOptions('griffinLim', nFft, hopLength);
  validatePositiveIntegers('griffinLim', { nIter });
  return requireModule().griffinLim(
    magnitude,
    nBins,
    nFrames,
    sampleRate,
    fft.nFft,
    fft.hopLength,
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
  const fft = resolveFftOptions('mfccToAudio', nFft, hopLength);
  validatePositiveIntegers('mfccToAudio', { nMels, nIter });
  validateMelFrequencyRange('mfccToAudio', fmin, fmax, sampleRate);
  return requireModule().mfccToAudio(
    mfccCoefficients,
    nMfcc,
    nFrames,
    nMels,
    sampleRate,
    fft.nFft,
    fft.hopLength,
    fmin,
    fmax,
    nIter,
    htk,
    lifter,
  );
}

export interface PhaseVocoderRequest extends SpectralFrameRequest {
  rate: number;
}

/**
 * Phase-vocoder time-scale modification (rate > 1 faster, < 1 slower).
 */
export function phaseVocoder(request: PhaseVocoderRequest): Float32Array;
export function phaseVocoder(
  samples: Float32Array,
  sampleRate: number,
  rate: number,
  nFft?: number,
  hopLength?: number,
): Float32Array;
export function phaseVocoder(
  samples: Float32Array | PhaseVocoderRequest,
  sampleRate = 22050,
  rate = 1,
  nFft = 2048,
  hopLength = 512,
): Float32Array {
  if (!(samples instanceof Float32Array)) {
    const r = samples;
    return phaseVocoder(r.samples, r.sampleRate ?? 22050, r.rate, r.nFft, r.hopLength);
  }
  // Matches the addon. A finite value too wide for a float is not covered here
  // and cannot be — it is still finite to Number.isFinite; the core's own guard
  // refuses the infinity it becomes.
  assertFiniteScalar('phaseVocoder', rate, 'rate');
  return requireModule().phaseVocoder(samples, sampleRate, rate, nFft, hopLength);
}
