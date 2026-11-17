/**
 * Inverse transforms: reconstructing a linear spectrogram or audio from a mel
 * or MFCC representation, and the phase reconstruction they share.
 */

import type { GuardedOptions } from './internal/feature_validation.js';
import { validatePositiveIntegers } from './internal/feature_validation.js';
import { resolveFftOptions } from './internal/fft_options.js';
import { getSonareModule } from './module_state.js';
import type { Matrix2D } from './public_types.js';
import {
  assertAudioInput,
  assertFiniteScalar,
  assertFloatFields,
  assertMatrix2D,
  assertSampleRate,
  requestObject,
  requestOnly,
  toMatrix2D,
} from './validation.js';

function requireModule() {
  return getSonareModule();
}

/** Mel power to linear STFT magnitude request. */
export interface MelToStftRequest extends GuardedOptions {
  /** Rows are mel bands, cols are frames. */
  power: Matrix2D;
  /** Rate of the source audio, in Hz. Default 22050. */
  sampleRate?: number;
  /** FFT size of the target spectrogram. Default 2048. */
  nFft?: number;
  /** Lower mel band edge in Hz. Default 0. */
  fminHz?: number;
  /** Upper mel band edge in Hz; absent or 0 is the Nyquist. */
  fmaxHz?: number;
  /** Use the HTK mel formula instead of Slaney. Default `false`. */
  htk?: boolean;
}

/** Griffin-Lim reconstruction from mel power request. */
export interface MelToAudioRequest extends GuardedOptions {
  /** Rows are mel bands, cols are frames. */
  power: Matrix2D;
  /** Rate of the source audio, in Hz. Default 22050. */
  sampleRate?: number;
  /** FFT size. Default 2048. */
  nFft?: number;
  /** Hop in samples. Default 512. */
  hopLength?: number;
  /** Lower mel band edge in Hz. Default 0. */
  fminHz?: number;
  /** Upper mel band edge in Hz; absent or 0 is the Nyquist. */
  fmaxHz?: number;
  /** Griffin-Lim iterations. Default 32. */
  nIter?: number;
  /** Use the HTK mel formula instead of Slaney. Default `false`. */
  htk?: boolean;
}

/** MFCC to mel power request. */
export interface MfccToMelRequest extends GuardedOptions {
  /** Rows are coefficients, cols are frames. */
  coefficients: Matrix2D;
  /** Mel bands to reconstruct. Default 128. */
  nMels?: number;
  /** Lifter used by the forward MFCC transform; zero means no liftering. */
  lifter?: number;
}

/** Griffin-Lim reconstruction from MFCC request. */
export interface MfccToAudioRequest extends GuardedOptions {
  /** Rows are coefficients, cols are frames. */
  coefficients: Matrix2D;
  /** Mel bands to reconstruct. Default 128. */
  nMels?: number;
  /** Lifter used by the forward MFCC transform; zero means no liftering. */
  lifter?: number;
  /** Rate of the source audio, in Hz. Default 22050. */
  sampleRate?: number;
  /** FFT size. Default 2048. */
  nFft?: number;
  /** Hop in samples. Default 512. */
  hopLength?: number;
  /** Lower mel band edge in Hz. Default 0. */
  fminHz?: number;
  /** Upper mel band edge in Hz; absent or 0 is the Nyquist. */
  fmaxHz?: number;
  /** Griffin-Lim iterations. Default 32. */
  nIter?: number;
  /** Use the HTK mel formula instead of Slaney. Default `false`. */
  htk?: boolean;
}

/** Griffin-Lim reconstruction from an STFT magnitude request. */
export interface GriffinLimRequest extends GuardedOptions {
  /** Rows are `nFft / 2 + 1` bins, cols are frames. */
  magnitude: Matrix2D;
  /** Rate of the source audio, in Hz. Default 22050. */
  sampleRate?: number;
  /** FFT size. Default 2048. */
  nFft?: number;
  /** Hop in samples. Default 512. */
  hopLength?: number;
  /** Iterations. Default 32. */
  nIter?: number;
  /** Fast Griffin-Lim momentum. Default 0.99. */
  momentum?: number;
}

/**
 * Approximate inverse of a Mel filterbank: Mel power spectrogram -> STFT
 * magnitude spectrogram (not squared). Mirrors `feature::mel_to_stft`.
 *
 * @returns {@link Matrix2D} whose rows are `nFft / 2 + 1` bins and cols are frames.
 */
export function melToStft(request: MelToStftRequest): Matrix2D;
export function melToStft(...args: unknown[]): Matrix2D {
  const r = requestOnly<MelToStftRequest>('melToStft', args);
  const sampleRate = r.sampleRate ?? 22050;
  assertSampleRate('melToStft', sampleRate);
  const power = assertMatrix2D('melToStft', r.power, 'power', r.validate !== false);
  // Not on the shared FFT rule: this inverts a filterbank rather than running a
  // transform, so `nFft` only sizes the output to `nFft / 2 + 1` bins and an odd
  // size is accepted by the core and by the Node surface alike.
  const nFft = r.nFft ?? 2048;
  validatePositiveIntegers('melToStft', { nFft });
  const fminHz = r.fminHz ?? 0;
  const fmaxHz = r.fmaxHz ?? 0;
  assertFloatFields('melToStft', { fminHz, fmaxHz });
  const native = requireModule().melToStft(
    power.data,
    power.rows,
    power.cols,
    sampleRate,
    nFft,
    fminHz,
    fmaxHz,
    r.htk ?? false,
  );
  return toMatrix2D(native.magnitude, native.nBins, native.nFrames);
}

/**
 * Reconstruct audio from a Mel power spectrogram via Griffin-Lim. Mirrors
 * `feature::mel_to_audio`.
 *
 * @returns Reconstructed audio samples (mono, float32)
 */
export function melToAudio(request: MelToAudioRequest): Float32Array;
export function melToAudio(...args: unknown[]): Float32Array {
  const r = requestOnly<MelToAudioRequest>('melToAudio', args);
  const sampleRate = r.sampleRate ?? 22050;
  assertSampleRate('melToAudio', sampleRate);
  const power = assertMatrix2D('melToAudio', r.power, 'power', r.validate !== false);
  const fft = resolveFftOptions('melToAudio', r.nFft, r.hopLength);
  const nIter = r.nIter ?? 32;
  validatePositiveIntegers('melToAudio', { nIter });
  const fminHz = r.fminHz ?? 0;
  const fmaxHz = r.fmaxHz ?? 0;
  assertFloatFields('melToAudio', { fminHz, fmaxHz });
  return requireModule().melToAudio(
    power.data,
    power.rows,
    power.cols,
    sampleRate,
    fft.nFft,
    fft.hopLength,
    fminHz,
    fmaxHz,
    nIter,
    r.htk ?? false,
  );
}

/** Reconstruct audio from an STFT magnitude matrix via Griffin-Lim. */
export function griffinLim(request: GriffinLimRequest): Float32Array;
export function griffinLim(...args: unknown[]): Float32Array {
  const r = requestOnly<GriffinLimRequest>('griffinLim', args);
  const sampleRate = r.sampleRate ?? 22050;
  assertSampleRate('griffinLim', sampleRate);
  const magnitude = assertMatrix2D('griffinLim', r.magnitude, 'magnitude', r.validate !== false);
  const fft = resolveFftOptions('griffinLim', r.nFft, r.hopLength);
  const nIter = r.nIter ?? 32;
  validatePositiveIntegers('griffinLim', { nIter });
  return requireModule().griffinLim(
    magnitude.data,
    magnitude.rows,
    magnitude.cols,
    sampleRate,
    fft.nFft,
    fft.hopLength,
    nIter,
    r.momentum ?? 0.99,
  );
}

/**
 * Invert MFCC coefficients back to a Mel power spectrogram. Mirrors
 * `feature::mfcc_to_mel`.
 *
 * @returns {@link Matrix2D} whose rows are mel bands and cols are frames.
 */
export function mfccToMel(request: MfccToMelRequest): Matrix2D;
export function mfccToMel(...args: unknown[]): Matrix2D {
  const r = requestOnly<MfccToMelRequest>('mfccToMel', args);
  const coefficients = assertMatrix2D(
    'mfccToMel',
    r.coefficients,
    'coefficients',
    r.validate !== false,
  );
  const nMels = r.nMels ?? 128;
  validatePositiveIntegers('mfccToMel', { nMels });
  const native = requireModule().mfccToMel(
    coefficients.data,
    coefficients.rows,
    coefficients.cols,
    nMels,
    r.lifter ?? 0,
  );
  return toMatrix2D(native.power, native.nMels, native.nFrames);
}

/**
 * Reconstruct audio directly from MFCC coefficients via Griffin-Lim. Mirrors
 * `feature::mfcc_to_audio`.
 *
 * @returns Reconstructed audio samples (mono, float32)
 */
export function mfccToAudio(request: MfccToAudioRequest): Float32Array;
export function mfccToAudio(...args: unknown[]): Float32Array {
  const r = requestOnly<MfccToAudioRequest>('mfccToAudio', args);
  const sampleRate = r.sampleRate ?? 22050;
  assertSampleRate('mfccToAudio', sampleRate);
  const coefficients = assertMatrix2D(
    'mfccToAudio',
    r.coefficients,
    'coefficients',
    r.validate !== false,
  );
  const fft = resolveFftOptions('mfccToAudio', r.nFft, r.hopLength);
  const nMels = r.nMels ?? 128;
  const nIter = r.nIter ?? 32;
  validatePositiveIntegers('mfccToAudio', { nMels, nIter });
  const fminHz = r.fminHz ?? 0;
  const fmaxHz = r.fmaxHz ?? 0;
  assertFloatFields('mfccToAudio', { fminHz, fmaxHz });
  return requireModule().mfccToAudio(
    coefficients.data,
    coefficients.rows,
    coefficients.cols,
    nMels,
    sampleRate,
    fft.nFft,
    fft.hopLength,
    fminHz,
    fmaxHz,
    nIter,
    r.htk ?? false,
    r.lifter ?? 0,
  );
}

export interface PhaseVocoderRequest extends GuardedOptions {
  samples: Float32Array;
  sampleRate?: number;
  nFft?: number;
  hopLength?: number;
  rate: number;
}

/**
 * Phase-vocoder time-scale modification. `rate` is a speed (> 1 faster and
 * shorter, < 1 slower and longer), the opposite sense of a note's `stretchRatio`.
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
  rate = Number.NaN,
  nFft = 2048,
  hopLength = 512,
): Float32Array {
  const request =
    samples instanceof Float32Array
      ? { samples, sampleRate, rate, nFft, hopLength }
      : requestObject('phaseVocoder', samples);
  const resolvedSampleRate = request.sampleRate ?? 22050;
  assertAudioInput('phaseVocoder', request.samples, resolvedSampleRate, request);
  // Matches the addon. A finite value too wide for a float is not covered here
  // and cannot be — it is still finite to Number.isFinite; the core's own guard
  // refuses the infinity it becomes.
  assertFiniteScalar('phaseVocoder', request.rate, 'rate');
  return requireModule().phaseVocoder(
    request.samples,
    resolvedSampleRate,
    request.rate,
    request.nFft ?? 2048,
    request.hopLength ?? 512,
  );
}
