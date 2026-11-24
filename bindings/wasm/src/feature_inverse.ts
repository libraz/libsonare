/**
 * Inverse transforms: reconstructing a linear spectrogram or audio from a mel
 * or MFCC representation, and the phase reconstruction they share.
 */

import { refuse, refuseType, withEntry } from './internal/entry.js';
import {
  resolveEffectFftOptions,
  resolveFftOptions,
  resolveResynthesisHop,
} from './internal/fft_options.js';
import { MAX_OFFLINE_AUDIO_SAMPLES, withHeapNeed } from './internal/memory_budget.js';
import { isAbsent } from './internal/value_guards.js';
import { getSonareModule } from './module_state.js';
import type { Matrix2D } from './public_types.js';
import type { OptionsOf } from './validation.js';
import {
  assertAudioInput,
  assertFiniteScalar,
  assertInt32,
  assertIntegerValue,
  assertMatrix2D,
  assertNumber,
  assertOptionalFmaxNotZero,
  assertPositiveFloatValue,
  assertSampleRate,
  requestOnly,
  sampleOneShot,
  toMatrix2D,
} from './validation.js';

/** The `nFft` / `hopLength` pair of a Griffin-Lim inverse: an omitted hop is left to the core. */
function resolveInverseFftOptions(
  nFft: unknown,
  hopLength: unknown,
): { nFft: number; hopLength: number } {
  assertIntegerValue(nFft, 'nFft');
  return {
    nFft: resolveFftOptions(nFft, undefined).nFft,
    hopLength: resolveResynthesisHop(hopLength),
  };
}

/** The core's frame-count ceiling for a mel inversion, and its largest `mel_to_stft` FFT size. */
const MAX_MEL_INVERSE_FRAMES = 4096;
const MAX_MEL_INVERSE_N_FFT = 8192;

/** Samples a Griffin-Lim reconstruction returns for `frames` frames; an automatic hop is a quarter of `nFft`. */
function reconstructedSamples(frames: number, nFft: number, hopLength: number): number {
  const hop = hopLength !== 0 ? hopLength : Math.max(1, Math.floor(nFft / 4));
  return Math.max(0, frames - 1) * hop;
}

/** Mel power to linear STFT magnitude request. */
export interface MelToStftRequest {
  /** Rows are mel bands, cols are frames. */
  power: Matrix2D;
  /** Rate of the source audio, in Hz. Default 22050. */
  sampleRate?: number;
  /** FFT size of the target spectrogram. Default 2048. */
  nFft?: number;
  /** Lower mel band edge in Hz. Default 0. */
  fminHz?: number;
  /** Upper mel band edge in Hz. Omit to use Nyquist; a supplied value must be positive and at most Nyquist. */
  fmaxHz?: number;
  /** Use the HTK mel formula instead of Slaney. Default `false`. */
  htk?: boolean;
}

/** Griffin-Lim reconstruction from mel power request. */
export interface MelToAudioRequest {
  /** Rows are mel bands, cols are frames. */
  power: Matrix2D;
  /** Rate of the source audio, in Hz. Default 22050. */
  sampleRate?: number;
  /** FFT size. Default 2048. */
  nFft?: number;
  /** Hop length in samples. Default `nFft / 4`, rounded down. */
  hopLength?: number;
  /** Lower mel band edge in Hz. Default 0. */
  fminHz?: number;
  /** Upper mel band edge in Hz. Omit to use Nyquist; a supplied value must be positive and at most Nyquist. */
  fmaxHz?: number;
  /** Griffin-Lim iterations. Default 32. */
  nIter?: number;
  /** Use the HTK mel formula instead of Slaney. Default `false`. */
  htk?: boolean;
}

/** MFCC to mel power request. */
export interface MfccToMelRequest {
  /** Rows are coefficients, cols are frames. */
  coefficients: Matrix2D;
  /** Mel bands to reconstruct. Default 128. */
  nMels?: number;
  /** Lifter used by the forward MFCC transform; zero means no liftering. */
  lifter?: number;
}

/** Griffin-Lim reconstruction from MFCC request. */
export interface MfccToAudioRequest {
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
  /** Hop length in samples. Default `nFft / 4`, rounded down. */
  hopLength?: number;
  /** Lower mel band edge in Hz. Default 0. */
  fminHz?: number;
  /** Upper mel band edge in Hz. Omit to use Nyquist; a supplied value must be positive and at most Nyquist. */
  fmaxHz?: number;
  /** Griffin-Lim iterations. Default 32. */
  nIter?: number;
  /** Use the HTK mel formula instead of Slaney. Default `false`. */
  htk?: boolean;
}

/** Griffin-Lim reconstruction from an STFT magnitude request. */
export interface GriffinLimRequest {
  /** Rows are `nFft / 2 + 1` bins, cols are frames. */
  magnitude: Matrix2D;
  /** Rate of the source audio, in Hz. */
  sampleRate: number;
  /** FFT size the magnitude came from. Omitted, it is inferred as `(rows - 1) * 2`; a size that disagrees with the bin count is a `RangeError`. */
  nFft?: number;
  /** Hop length in samples. Default `nFft / 4`, rounded down. */
  hopLength?: number;
  /** Iterations. Default 32. */
  nIter?: number;
  /** Fast Griffin-Lim momentum. Default 0.99. */
  momentum?: number;
}

/** Request for {@link istft}. */
export interface IstftRequest {
  /** Magnitude matrix from `stft`; rows are bins and cols are frames. */
  magnitude: Matrix2D;
  /** Phase matrix in radians, with the same shape as `magnitude`. */
  phase: Matrix2D;
  /** FFT size of the source transform. Omitted, it is inferred from the bin count. */
  nFft?: number;
  /** Hop length in samples. Omitted, it is one quarter of `nFft`, rounded down and at least 1. */
  hopLength?: number;
  /** Output length in samples, from 1 through 500,000,000. Omit for the natural length. */
  lengthSamples?: number;
}

/**
 * Approximate inverse of a Mel filterbank: Mel power spectrogram -> STFT
 * magnitude spectrogram (not squared). Mirrors `feature::mel_to_stft`.
 *
 * @returns {@link Matrix2D} whose rows are `nFft / 2 + 1` bins and cols are frames.
 */
export function melToStft(request: MelToStftRequest): Matrix2D;
export function melToStft(...args: unknown[]): Matrix2D {
  return withEntry('melToStft', () => {
    const r = requestOnly<MelToStftRequest>('melToStft', args);
    const sampleRate = r.sampleRate ?? 22050;
    const power = assertMatrix2D(r.power, 'power', false);
    // Not on the shared FFT rule: this inverts a filterbank rather than running a
    // transform, so `nFft` only sizes the output to `nFft / 2 + 1` bins and an odd
    // size is accepted by the core and by the Node surface alike.
    const nFft = r.nFft ?? 2048;
    const fminHz = r.fminHz ?? 0;
    const fmaxHz = r.fmaxHz ?? 0;
    assertNumber(fminHz, 'fminHz');
    assertOptionalFmaxNotZero(r.fmaxHz);
    const call = () =>
      getSonareModule().melToStft(
        power.data,
        power.rows,
        power.cols,
        sampleRate,
        nFft,
        fminHz,
        fmaxHz,
        r.htk ?? false,
      );
    const inDomain =
      Number.isInteger(nFft) &&
      nFft >= 1 &&
      nFft <= MAX_MEL_INVERSE_N_FFT &&
      power.cols <= MAX_MEL_INVERSE_FRAMES;
    const native = inDomain
      ? withHeapNeed(
          {
            subject: 'power',
            size: power.cols,
            bytes: (n) => 4 * power.rows * n + 4 * (Math.floor(nFft / 2) + 1) * n,
            monotone: true,
          },
          call,
        )
      : call();
    return toMatrix2D(native.magnitude, native.nBins, native.nFrames);
  });
}

/**
 * Reconstruct audio from a Mel power spectrogram via Griffin-Lim. Mirrors
 * `feature::mel_to_audio`.
 *
 * @returns Reconstructed audio samples (mono, float32)
 */
export function melToAudio(request: MelToAudioRequest): Float32Array;
export function melToAudio(...args: unknown[]): Float32Array {
  return withEntry('melToAudio', () => {
    const r = requestOnly<MelToAudioRequest>('melToAudio', args);
    const sampleRate = r.sampleRate ?? 22050;
    const power = assertMatrix2D(r.power, 'power', false);
    const fft = resolveInverseFftOptions(r.nFft ?? 2048, r.hopLength);
    const nIter = r.nIter ?? 32;
    assertInt32(nIter, 'nIter');
    const fminHz = r.fminHz ?? 0;
    const fmaxHz = r.fmaxHz ?? 0;
    assertNumber(fminHz, 'fminHz');
    assertOptionalFmaxNotZero(r.fmaxHz);
    const call = () =>
      getSonareModule().melToAudio(
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
    if (!Number.isInteger(fft.nFft) || fft.nFft < 1 || power.cols > MAX_MEL_INVERSE_FRAMES) {
      return call();
    }
    return withHeapNeed(
      {
        subject: 'power',
        size: power.cols,
        bytes: (n) => 4 * power.rows * n + 4 * reconstructedSamples(n, fft.nFft, fft.hopLength),
        monotone: true,
      },
      call,
    );
  });
}

/**
 * A Griffin-Lim magnitude has `nFft / 2 + 1` rows, so an explicit size must agree with them.
 * The relation is asked only of an even size: the core refuses any other by its own rule first.
 */
function assertGriffinLimFftSize(nBins: number, nFft: number): void {
  if (nFft % 2 === 0 && nFft !== (nBins - 1) * 2) {
    throw refuse(`nFft must equal (magnitude.rows - 1) * 2, which is ${(nBins - 1) * 2}`);
  }
}

/** Reconstruct audio from STFT magnitude and phase. */
export function istft(request: IstftRequest): Float32Array;
export function istft(...args: unknown[]): Float32Array {
  return withEntry('istft', () => {
    const r = requestOnly<IstftRequest>('istft', args);
    if (isAbsent(r.magnitude)) {
      throw refuseType('magnitude is required');
    }
    if (isAbsent(r.phase)) {
      throw refuseType('phase is required');
    }
    const magnitude = assertMatrix2D(r.magnitude, 'magnitude', false);
    const phase = assertMatrix2D(r.phase, 'phase', false);
    if (magnitude.rows !== phase.rows || magnitude.cols !== phase.cols) {
      throw refuse('magnitude and phase must have the same shape');
    }
    const nFft = r.nFft ?? (magnitude.rows - 1) * 2;
    assertInt32(nFft, 'nFft');
    const hopLength = r.hopLength ?? Math.max(1, Math.floor(nFft / 4));
    assertIntegerValue(hopLength, 'hopLength');
    const lengthSamples = r.lengthSamples ?? undefined;
    if (lengthSamples !== undefined) {
      assertIntegerValue(lengthSamples, 'lengthSamples');
    }
    const outputLength = lengthSamples ?? Math.max(0, (magnitude.cols - 1) * hopLength);
    const call = () => {
      const module = getSonareModule();
      return module.istft(
        magnitude.data,
        magnitude.rows,
        magnitude.cols,
        phase.data,
        phase.rows,
        phase.cols,
        nFft,
        hopLength,
        lengthSamples,
      );
    };
    if (
      nFft <= 0 ||
      hopLength <= 0 ||
      lengthSamples === 0 ||
      !Number.isSafeInteger(outputLength) ||
      outputLength < 0 ||
      outputLength > MAX_OFFLINE_AUDIO_SAMPLES
    ) {
      return call();
    }
    return withHeapNeed(
      {
        subject: 'magnitude',
        size: magnitude.data.length,
        bytes: () => 16 * magnitude.data.length + 8 * outputLength,
      },
      call,
    );
  });
}

/** Reconstruct audio from an STFT magnitude matrix via Griffin-Lim. */
export function griffinLim(request: GriffinLimRequest): Float32Array;
export function griffinLim(...args: unknown[]): Float32Array {
  return withEntry('griffinLim', () => {
    const r = requestOnly<GriffinLimRequest>('griffinLim', args);
    const sampleRate = r.sampleRate;
    assertSampleRate(sampleRate);
    const magnitude = assertMatrix2D(r.magnitude, 'magnitude');
    const fft = resolveInverseFftOptions(r.nFft ?? (magnitude.rows - 1) * 2, r.hopLength);
    assertGriffinLimFftSize(magnitude.rows, fft.nFft);
    const nIter = r.nIter ?? 32;
    const call = () =>
      getSonareModule().griffinLim(
        magnitude.data,
        magnitude.rows,
        magnitude.cols,
        sampleRate,
        fft.nFft,
        fft.hopLength,
        nIter,
        r.momentum ?? 0.99,
      );
    if (!Number.isInteger(fft.nFft) || fft.nFft < 1) {
      return call();
    }
    return withHeapNeed(
      {
        subject: 'magnitude',
        size: magnitude.cols,
        bytes: (n) => 4 * magnitude.rows * n + 4 * reconstructedSamples(n, fft.nFft, fft.hopLength),
        monotone: true,
      },
      call,
    );
  });
}

/**
 * Invert MFCC coefficients back to a Mel power spectrogram. Mirrors
 * `feature::mfcc_to_mel`.
 *
 * @returns {@link Matrix2D} whose rows are mel bands and cols are frames.
 */
export function mfccToMel(request: MfccToMelRequest): Matrix2D;
export function mfccToMel(...args: unknown[]): Matrix2D {
  return withEntry('mfccToMel', () => {
    const r = requestOnly<MfccToMelRequest>('mfccToMel', args);
    const coefficients = assertMatrix2D(r.coefficients, 'coefficients');
    const nMels = r.nMels ?? 128;
    assertInt32(nMels, 'nMels');
    const call = () =>
      getSonareModule().mfccToMel(
        coefficients.data,
        coefficients.rows,
        coefficients.cols,
        nMels,
        r.lifter ?? 0,
      );
    const native =
      nMels >= 1
        ? withHeapNeed(
            {
              subject: 'coefficients',
              size: coefficients.cols,
              bytes: (n) => 4 * coefficients.rows * n + 4 * nMels * n,
              monotone: true,
            },
            call,
          )
        : call();
    return toMatrix2D(native.power, native.nMels, native.nFrames);
  });
}

/**
 * Reconstruct audio directly from MFCC coefficients via Griffin-Lim. Mirrors
 * `feature::mfcc_to_audio`.
 *
 * @returns Reconstructed audio samples (mono, float32)
 */
export function mfccToAudio(request: MfccToAudioRequest): Float32Array;
export function mfccToAudio(...args: unknown[]): Float32Array {
  return withEntry('mfccToAudio', () => {
    const r = requestOnly<MfccToAudioRequest>('mfccToAudio', args);
    const sampleRate = r.sampleRate ?? 22050;
    const coefficients = assertMatrix2D(r.coefficients, 'coefficients', false);
    const fft = resolveInverseFftOptions(r.nFft ?? 2048, r.hopLength);
    const nMels = r.nMels ?? 128;
    const nIter = r.nIter ?? 32;
    assertInt32(nMels, 'nMels');
    assertInt32(nIter, 'nIter');
    const fminHz = r.fminHz ?? 0;
    const fmaxHz = r.fmaxHz ?? 0;
    assertNumber(fminHz, 'fminHz');
    assertOptionalFmaxNotZero(r.fmaxHz);
    const call = () =>
      getSonareModule().mfccToAudio(
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
    if (!Number.isInteger(fft.nFft) || fft.nFft < 1 || coefficients.cols > MAX_MEL_INVERSE_FRAMES) {
      return call();
    }
    return withHeapNeed(
      {
        subject: 'coefficients',
        size: coefficients.cols,
        bytes: (n) =>
          4 * coefficients.rows * n + 4 * reconstructedSamples(n, fft.nFft, fft.hopLength),
        monotone: true,
      },
      call,
    );
  });
}

/** Request for {@link phaseVocoder}. */
export interface PhaseVocoderRequest {
  /** Mono audio samples. */
  samples: Float32Array;
  /** Sample rate in Hz, an integer in `[8000, 384000]`; there is no default. */
  sampleRate: number;
  /**
   * Playback speed as a ratio, finite: above 1 is faster and shorter, below 1
   * slower and longer, the opposite sense of a note's `stretchRatio`. Required.
   */
  speedRatio: number;
  /** FFT size: an even integer in `[4, 2147483646]`. Default 2048. */
  nFft?: number;
  /** Hop in samples, in `(0, nFft / 2]`. Default `nFft / 4`, rounded down. */
  hopLength?: number;
}

/**
 * Phase-vocoder time-scale modification by `speedRatio`.
 *
 * @throws {TypeError} when `sampleRate` or `speedRatio` is missing
 * @throws {RangeError} on audio out of domain, an out-of-range `sampleRate`, a non-finite `speedRatio`,
 *   an `nFft` outside the even integers in `[4, 2147483646]` or a `hopLength` that is not positive
 */
export function phaseVocoder(request: PhaseVocoderRequest): Float32Array;
export function phaseVocoder(
  samples: Float32Array,
  sampleRate: number,
  options: OptionsOf<PhaseVocoderRequest>,
): Float32Array;
export function phaseVocoder(...args: unknown[]): Float32Array {
  return withEntry('phaseVocoder', () => {
    const r = sampleOneShot<PhaseVocoderRequest>('phaseVocoder', args);
    assertAudioInput(r.samples, r.sampleRate, r);
    assertNumber(r.speedRatio, 'speedRatio');
    // Matches the addon. A finite value too wide for a float is not covered here
    // and cannot be — it is still finite to Number.isFinite; the core's own guard
    // refuses the infinity it becomes.
    assertFiniteScalar(r.speedRatio, 'speedRatio');
    assertPositiveFloatValue(r.speedRatio, 'speedRatio');
    const { nFft, hopLength } = resolveEffectFftOptions(r.nFft, r.hopLength);
    const call = () =>
      getSonareModule().phaseVocoder(r.samples, r.sampleRate, r.speedRatio, nFft, hopLength);
    const rate = Math.fround(r.speedRatio);
    // The output is ceil(N / rate) samples; the factor keeps float rounding from overcounting.
    const length = r.samples.length;
    const outSamples = Math.floor((length / rate) * (1 - 1e-6));
    // A rate whose output passes the offline cap is the native rate check's refusal.
    if (!(rate > 0) || !Number.isFinite(rate) || outSamples > MAX_OFFLINE_AUDIO_SAMPLES) {
      return call();
    }
    return withHeapNeed(
      {
        subject: 'speedRatio',
        size: r.speedRatio,
        bytes: () => 4 * length + 4 * outSamples,
      },
      call,
    );
  });
}
