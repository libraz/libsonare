/**
 * Inverse transforms: reconstructing a linear spectrogram or audio from a mel
 * or MFCC representation, and the phase reconstruction they share.
 */

import type { GuardedOptions } from './_feature_validation.js';
import { validateMelFrequencyRange, validatePositiveIntegers } from './_feature_validation.js';
import { resolveFftOptions } from './_fft_options.js';
import type { SpectralFrameRequest } from './feature_spectral.js';
import { getSonareModule } from './module_state.js';
import type {
  MelPowerResult,
  MelSpectrogramResult,
  MfccResult,
  StftPowerResult,
} from './public_types.js';
import { assertFiniteScalar, assertSampleRate, assertSamples } from './validation.js';

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

/**
 * Request form of {@link melToStft} that takes the forward result and reads the
 * Mel matrix and every parameter from it. A field set here as well must agree
 * with the result's value, or the call is refused with a `RangeError`; a result
 * in dB is refused too, with a message naming `dbToPower`.
 */
export interface MelResultToStftRequest extends GuardedOptions {
  result: MelSpectrogramResult;
  nMels?: number;
  nFrames?: number;
  sampleRate?: number;
  nFft?: number;
  fmin?: number;
  fmax?: number;
  htk?: boolean;
}

/** Request form of {@link melToAudio} over a forward result; see {@link MelResultToStftRequest}. */
export interface MelResultToAudioRequest extends MelResultToStftRequest {
  /** Hop of the reconstruction; must agree with the result's when set. */
  hopLength?: number;
  /** Griffin-Lim iterations. Default 32. */
  nIter?: number;
}

/**
 * Request form of {@link mfccToMel} that takes the forward result and reads the
 * coefficients, Mel band count and lifter from it. A field set here as well must
 * agree with the result's value, or the call is refused with a `RangeError`.
 */
export interface MfccResultToMelRequest extends GuardedOptions {
  result: MfccResult;
  nMfcc?: number;
  nFrames?: number;
  nMels?: number;
  lifter?: number;
}

/** Request form of {@link mfccToAudio} over a forward result; see {@link MfccResultToMelRequest}. */
export interface MfccResultToAudioRequest extends MfccResultToMelRequest {
  sampleRate?: number;
  nFft?: number;
  hopLength?: number;
  fmin?: number;
  fmax?: number;
  htk?: boolean;
  /** Griffin-Lim iterations. Default 32. */
  nIter?: number;
}

// Refuses a field the caller set that disagrees with the value the result carries.
// An fmax of 0 means the Nyquist in the inverse's own vocabulary, so it is compared resolved.
function requireAgreesWithResult(
  fnName: string,
  request: object,
  result: object,
  keys: readonly string[],
): void {
  const given = request as Record<string, unknown>;
  const carried = result as Record<string, unknown>;
  for (const key of keys) {
    const value = given[key];
    if (value === undefined) {
      continue;
    }
    const comparable = key === 'fmax' && value === 0 ? (carried.sampleRate as number) / 2 : value;
    if (comparable !== carried[key]) {
      throw new RangeError(
        `${fnName}: ${key} ${String(value)} disagrees with the result's ${String(carried[key])}`,
      );
    }
  }
}

// The fields a result must carry to be inverted, so a hand-built result that lacks one is
// named rather than inverted with an undefined.
function requireResultFields(fnName: string, result: object, keys: readonly string[]): void {
  const carried = result as Record<string, unknown>;
  for (const key of keys) {
    if (carried[key] === undefined) {
      throw new TypeError(`${fnName}: result.${key} is missing`);
    }
  }
}

const MEL_RESULT_FIELDS = [
  'nMels',
  'nFrames',
  'sampleRate',
  'hopLength',
  'nFft',
  'fmin',
  'fmax',
  'htk',
  'power',
] as const;
const MFCC_RESULT_FIELDS = [
  'nMfcc',
  'nFrames',
  'sampleRate',
  'hopLength',
  'nFft',
  'nMels',
  'fmin',
  'fmax',
  'htk',
  'lifter',
  'coefficients',
] as const;
const MEL_TO_STFT_CARRIED = ['nMels', 'nFrames', 'sampleRate', 'nFft', 'fmin', 'fmax', 'htk'];
const MEL_TO_AUDIO_CARRIED = [...MEL_TO_STFT_CARRIED, 'hopLength'];
const MFCC_TO_MEL_CARRIED = ['nMfcc', 'nFrames', 'nMels', 'lifter'];
const MFCC_TO_AUDIO_CARRIED = [
  ...MFCC_TO_MEL_CARRIED,
  'sampleRate',
  'nFft',
  'hopLength',
  'fmin',
  'fmax',
  'htk',
];

// Reads a Mel result request into the request form its inverse takes.
function melRequestFromResult(
  fnName: string,
  request: MelResultToAudioRequest,
  carriedKeys: readonly string[],
): MelToAudioRequest {
  const { result } = request;
  if (result === null || typeof result !== 'object') {
    throw new TypeError(`${fnName}: result must be a Mel spectrogram result`);
  }
  if (result.isDb === true) {
    throw new RangeError(
      `${fnName}: result is in dB; convert it to power with dbToPower before inverting it`,
    );
  }
  requireResultFields(fnName, result, MEL_RESULT_FIELDS);
  requireAgreesWithResult(fnName, request, result, carriedKeys);
  return {
    melPower: result.power,
    nMels: result.nMels,
    nFrames: result.nFrames,
    sampleRate: result.sampleRate,
    nFft: result.nFft,
    hopLength: result.hopLength,
    fmin: result.fmin,
    fmax: result.fmax,
    htk: result.htk,
    nIter: request.nIter,
    validate: request.validate,
  };
}

// Reads an MFCC result request into the request form its inverse takes.
function mfccRequestFromResult(
  fnName: string,
  request: MfccResultToAudioRequest,
  carriedKeys: readonly string[],
): MfccToAudioRequest {
  const { result } = request;
  if (result === null || typeof result !== 'object') {
    throw new TypeError(`${fnName}: result must be an MFCC result`);
  }
  requireResultFields(fnName, result, MFCC_RESULT_FIELDS);
  requireAgreesWithResult(fnName, request, result, carriedKeys);
  return {
    mfccCoefficients: result.coefficients,
    nMfcc: result.nMfcc,
    nFrames: result.nFrames,
    nMels: result.nMels,
    lifter: result.lifter,
    sampleRate: result.sampleRate,
    nFft: result.nFft,
    hopLength: result.hopLength,
    fmin: result.fmin,
    fmax: result.fmax,
    htk: result.htk,
    nIter: request.nIter,
    validate: request.validate,
  };
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
export function melToStft(request: MelToStftRequest | MelResultToStftRequest): StftPowerResult;
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
  melPower: Float32Array | MelToStftRequest | MelResultToStftRequest,
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
    const request =
      'result' in melPower
        ? melRequestFromResult('melToStft', melPower, MEL_TO_STFT_CARRIED)
        : melPower;
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
export function melToAudio(request: MelToAudioRequest | MelResultToAudioRequest): Float32Array;
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
  melPower: Float32Array | MelToAudioRequest | MelResultToAudioRequest,
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
    const request =
      'result' in melPower
        ? melRequestFromResult('melToAudio', melPower, MEL_TO_AUDIO_CARRIED)
        : melPower;
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
export function mfccToMel(request: MfccToMelRequest | MfccResultToMelRequest): MelPowerResult;
export function mfccToMel(
  mfccCoefficients: Float32Array,
  nMfcc: number,
  nFrames: number,
  nMels?: number,
  lifter?: number,
  options?: GuardedOptions,
): MelPowerResult;
export function mfccToMel(
  mfccCoefficients: Float32Array | MfccToMelRequest | MfccResultToMelRequest,
  nMfcc = 0,
  nFrames = 0,
  nMels = 128,
  lifter = 0,
  options: GuardedOptions = {},
): MelPowerResult {
  if (!(mfccCoefficients instanceof Float32Array)) {
    const request =
      'result' in mfccCoefficients
        ? mfccRequestFromResult('mfccToMel', mfccCoefficients, MFCC_TO_MEL_CARRIED)
        : mfccCoefficients;
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
export function mfccToAudio(request: MfccToAudioRequest | MfccResultToAudioRequest): Float32Array;
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
  mfccCoefficients: Float32Array | MfccToAudioRequest | MfccResultToAudioRequest,
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
    const request =
      'result' in mfccCoefficients
        ? mfccRequestFromResult('mfccToAudio', mfccCoefficients, MFCC_TO_AUDIO_CARRIED)
        : mfccCoefficients;
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
