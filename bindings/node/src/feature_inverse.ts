import { refuse, refuseType, withEntry } from './internal/entry.js';
import { assertMatrix2D, toMatrix2D } from './internal/feature_matrix.js';
import { resolveIntegerOption, resolvePositiveIntegerOption } from './internal/feature_options.js';
import { resolveEffectFftOptions, resolveResynthesisHop } from './internal/fft_options.js';
import { isAbsent } from './internal/value_guards.js';
import { addon } from './native.js';
import type { Matrix2D } from './types.js';
import type { OptionsOf } from './validation.js';
import {
  assertAudioInput,
  assertCqtHopLength,
  assertFiniteScalar,
  assertFloat32Array,
  assertFloatFields,
  assertInt32,
  assertPositiveFloatValue,
  assertSafeInteger,
  assertSampleRate,
  requestOnly,
  sampleOneShot,
} from './validation.js';

/** Request for {@link cqtToAudio}. */
export interface CqtToAudioRequest {
  /** CQT magnitude: rows = CQT bins, cols = frames. */
  magnitude: Matrix2D;
  /** Sample rate of the audio the magnitude came from, in Hz. */
  sampleRate: number;
  /** Hop length in samples. Default `512`. */
  hopLength?: number;
  /** Lowest bin frequency in Hz. Default C1 (32.70 Hz). */
  fminHz?: number;
  /** CQT bins per octave. Default `12`. */
  binsPerOctave?: number;
  /** Griffin-Lim iterations. Default 32. */
  nIter?: number;
}

/** Request for {@link vqtToAudio}. `magnitude` is rows = VQT bins, cols = frames. */
export interface VqtToAudioRequest extends CqtToAudioRequest {
  /** Bandwidth offset. Absent selects the automatic ERB-derived value; 0 is the constant-Q transform. */
  gamma?: number;
}

/** Request for {@link melToStft}. */
export interface MelToStftRequest {
  /** Mel power: rows = mel bands, cols = frames. */
  power: Matrix2D;
  /** Default 22050. */
  sampleRate?: number;
  /** FFT size of the forward transform. Default 2048. */
  nFft?: number;
  /** Lower band edge in Hz. Default 0. */
  fminHz?: number;
  /** Upper band edge in Hz. Absent means the Nyquist; an explicit 0 is refused (`RangeError`). */
  fmaxHz?: number;
  /** Use the HTK mel formula instead of Slaney. Default `false`. */
  htk?: boolean;
}

/** Request for {@link melToAudio}. */
export interface MelToAudioRequest extends MelToStftRequest {
  /** Hop length in samples. Default `nFft / 4`, rounded down. */
  hopLength?: number;
  /** Griffin-Lim iterations. Default 32. */
  nIter?: number;
}

/** Request for {@link griffinLim}. */
export interface GriffinLimRequest {
  /** STFT magnitude: rows = `nFft / 2 + 1` bins, cols = frames. */
  magnitude: Matrix2D;
  /** Sample rate of the audio the magnitude came from, in Hz. */
  sampleRate: number;
  /** FFT size the magnitude came from. Omitted, it is inferred as `(rows - 1) * 2`; a size that disagrees with the bin count is a `RangeError`. */
  nFft?: number;
  /** Hop length in samples. Default `nFft / 4`, rounded down. */
  hopLength?: number;
  /** Griffin-Lim iterations. Default 32. */
  nIter?: number;
  /** Fast Griffin-Lim momentum. Default 0.99. */
  momentum?: number;
}

/** Request for {@link istft}. */
export interface IstftRequest {
  /** STFT magnitude matrix: rows = bins, cols = frames. */
  magnitude: Matrix2D;
  /** STFT phase matrix in radians, with the same shape as `magnitude`. */
  phase: Matrix2D;
  /** FFT size; omitted, it is inferred as `(magnitude.rows - 1) * 2`. */
  nFft?: number;
  /** Positive Hann-COLA hop in samples. Defaults to `nFft / 4`, rounded down and at least 1. */
  hopLength?: number;
  /** Output length in samples, up to 500,000,000. Omitted uses natural length; explicit 0 is refused. */
  lengthSamples?: number;
}

/** Request for {@link mfccToMel}. */
export interface MfccToMelRequest {
  /** Coefficients: rows = coefficients, cols = frames. */
  coefficients: Matrix2D;
  /** Mel bands to reconstruct. Default 128. */
  nMels?: number;
  /** Lifter used by the forward MFCC transform; zero means no liftering. */
  lifter?: number;
}

/** Request for {@link mfccToAudio}. */
export interface MfccToAudioRequest extends MfccToMelRequest {
  /** Default 22050. */
  sampleRate?: number;
  /** FFT size in samples. Default `2048`. */
  nFft?: number;
  /** Hop length in samples. Default `nFft / 4`, rounded down. */
  hopLength?: number;
  /** Lower mel band edge in Hz. Default `0`. */
  fminHz?: number;
  /** Upper mel band edge in Hz. Absent means the Nyquist; an explicit 0 is refused (`RangeError`). */
  fmaxHz?: number;
  /** Griffin-Lim iterations. Default 32. */
  nIter?: number;
  /** Use the HTK mel formula instead of Slaney. Default `false`. */
  htk?: boolean;
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

/** Request for {@link tone}. */
export interface ToneRequest {
  /** Default 440. */
  frequencyHz?: number;
  /** Default 22050. */
  sampleRate?: number;
  /** Default 1. */
  durationSec?: number;
  /** Starting phase in radians. Default `0`. */
  phase?: number;
  /** Peak amplitude. Default `1`. */
  amplitude?: number;
}

/** Request for {@link chirp}. */
export interface ChirpRequest {
  /** Start frequency in Hz. Default 440. */
  fminHz?: number;
  /** End frequency in Hz. Default 880. */
  fmaxHz?: number;
  /** Output sample rate in Hz. Default `22050`. */
  sampleRate?: number;
  /** Default 1. */
  durationSec?: number;
  /** Linear sweep instead of the default exponential one. Default false. */
  linear?: boolean;
}

/** Request for {@link clicks}. */
export interface ClicksRequest {
  /** Click times in seconds. */
  times: Float32Array;
  /** Output sample rate in Hz. Default `22050`. */
  sampleRate?: number;
  /** Output length in samples. Absent derives it from the last click; an explicit 0 is refused. */
  length?: number;
  /** Click tone frequency in Hz. Default 1000. */
  frequencyHz?: number;
  /** Click decay length in seconds. Default 0.1. */
  clickDurationSec?: number;
}

const DEFAULT_CQT_FMIN_HZ = 32.70319566257483;

/** Reconstruct mono audio from a CQT magnitude via Griffin-Lim. */
export function cqtToAudio(request: CqtToAudioRequest): Float32Array;
export function cqtToAudio(...args: unknown[]): Float32Array {
  return withEntry('cqtToAudio', () => {
    const r = requestOnly<CqtToAudioRequest>('cqtToAudio', args);
    assertMatrix2D(r.magnitude, 'magnitude');
    const sampleRate = r.sampleRate;
    assertSampleRate(sampleRate);
    const hopLength = r.hopLength ?? 512;
    assertCqtHopLength(hopLength);
    const nIter = resolveIntegerOption('nIter', r.nIter, 32);
    const fminHz = r.fminHz ?? DEFAULT_CQT_FMIN_HZ;
    assertPositiveFloatValue(fminHz, 'fminHz');
    return addon.cqtToAudio(
      r.magnitude.data,
      r.magnitude.rows,
      r.magnitude.cols,
      sampleRate,
      hopLength,
      fminHz,
      r.binsPerOctave ?? 12,
      nIter,
    );
  });
}

/** Reconstruct mono audio from a VQT magnitude via Griffin-Lim. */
export function vqtToAudio(request: VqtToAudioRequest): Float32Array;
export function vqtToAudio(...args: unknown[]): Float32Array {
  return withEntry('vqtToAudio', () => {
    const r = requestOnly<VqtToAudioRequest>('vqtToAudio', args);
    assertMatrix2D(r.magnitude, 'magnitude');
    const sampleRate = r.sampleRate;
    assertSampleRate(sampleRate);
    const hopLength = r.hopLength ?? 512;
    assertCqtHopLength(hopLength);
    const nIter = resolveIntegerOption('nIter', r.nIter, 32);
    const fminHz = r.fminHz ?? DEFAULT_CQT_FMIN_HZ;
    assertPositiveFloatValue(fminHz, 'fminHz');
    return addon.vqtToAudio(
      r.magnitude.data,
      r.magnitude.rows,
      r.magnitude.cols,
      sampleRate,
      hopLength,
      fminHz,
      r.binsPerOctave ?? 12,
      r.gamma ?? undefined,
      nIter,
    );
  });
}

/** Reconstruct STFT magnitude from a mel spectrogram. Returns rows = `nFft / 2 + 1` bins, cols = frames. */
export function melToStft(request: MelToStftRequest): Matrix2D;
export function melToStft(...args: unknown[]): Matrix2D {
  return withEntry('melToStft', () => {
    const r = requestOnly<MelToStftRequest>('melToStft', args);
    assertMatrix2D(r.power, 'power');
    const sampleRate = r.sampleRate ?? 22050;
    const nFft = r.nFft ?? 2048;
    const fminHz = r.fminHz ?? 0;
    const fmaxHz = r.fmaxHz ?? 0;
    assertFloatFields({ fminHz, fmaxHz });
    // An omitted fmaxHz is the core's Nyquist sentinel 0; a given one is a frequency.
    if (r.fmaxHz !== undefined) {
      assertPositiveFloatValue(fmaxHz, 'fmaxHz');
    }
    const out = addon.melToStft(
      r.power.data,
      r.power.rows,
      r.power.cols,
      sampleRate,
      nFft,
      fminHz,
      fmaxHz,
      r.htk ?? false,
    );
    return toMatrix2D(out.magnitude, out.nBins, out.nFrames);
  });
}

/** Reconstruct audio from a mel spectrogram via Griffin-Lim. */
export function melToAudio(request: MelToAudioRequest): Float32Array;
export function melToAudio(...args: unknown[]): Float32Array {
  return withEntry('melToAudio', () => {
    const r = requestOnly<MelToAudioRequest>('melToAudio', args);
    assertMatrix2D(r.power, 'power');
    const sampleRate = r.sampleRate ?? 22050;
    const nFft = r.nFft ?? 2048;
    const hopLength = resolveResynthesisHop(r.hopLength);
    const nIter = resolveIntegerOption('nIter', r.nIter, 32);
    const fminHz = r.fminHz ?? 0;
    const fmaxHz = r.fmaxHz ?? 0;
    assertFloatFields({ fminHz, fmaxHz });
    // An omitted fmaxHz is the core's Nyquist sentinel 0; a given one is a frequency.
    if (r.fmaxHz !== undefined) {
      assertPositiveFloatValue(fmaxHz, 'fmaxHz');
    }
    return addon.melToAudio(
      r.power.data,
      r.power.rows,
      r.power.cols,
      sampleRate,
      nFft,
      hopLength,
      fminHz,
      fmaxHz,
      nIter,
      r.htk ?? false,
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

/** Reconstruct audio from an STFT magnitude matrix via Griffin-Lim. */
export function griffinLim(request: GriffinLimRequest): Float32Array;
export function griffinLim(...args: unknown[]): Float32Array {
  return withEntry('griffinLim', () => {
    const r = requestOnly<GriffinLimRequest>('griffinLim', args);
    assertMatrix2D(r.magnitude, 'magnitude');
    const sampleRate = r.sampleRate;
    assertSampleRate(sampleRate);
    const nBins = r.magnitude.rows;
    const nFft = resolvePositiveIntegerOption('nFft', r.nFft, (nBins - 1) * 2);
    assertGriffinLimFftSize(nBins, nFft);
    const hopLength = resolveResynthesisHop(r.hopLength);
    const nIter = resolveIntegerOption('nIter', r.nIter, 32);
    return addon.griffinLim(
      r.magnitude.data,
      r.magnitude.rows,
      r.magnitude.cols,
      sampleRate,
      nFft,
      hopLength,
      nIter,
      r.momentum ?? 0.99,
    );
  });
}

/** Reconstruct centered audio from an STFT magnitude and phase. */
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
    assertMatrix2D(r.magnitude, 'magnitude', false);
    assertMatrix2D(r.phase, 'phase', false);
    if (r.magnitude.rows !== r.phase.rows || r.magnitude.cols !== r.phase.cols) {
      throw refuse('magnitude and phase must have the same shape');
    }

    const inferredNFft = (r.magnitude.rows - 1) * 2;
    const nFft = isAbsent(r.nFft) ? inferredNFft : r.nFft;
    assertInt32(nFft, 'nFft');
    const hopLength = isAbsent(r.hopLength) ? Math.max(1, Math.floor(nFft / 4)) : r.hopLength;
    assertSafeInteger(hopLength, 'hopLength');
    if (!isAbsent(r.lengthSamples)) {
      assertSafeInteger(r.lengthSamples, 'lengthSamples');
      if (r.lengthSamples === 0) {
        throw refuse('lengthSamples must not be 0; omit it for the natural length');
      }
    }

    return addon.istft(
      r.magnitude.data,
      r.magnitude.rows,
      r.magnitude.cols,
      r.phase.data,
      r.phase.rows,
      r.phase.cols,
      nFft,
      hopLength,
      isAbsent(r.lengthSamples) ? undefined : r.lengthSamples,
    ) as Float32Array;
  });
}

/** Reconstruct a mel power spectrogram from MFCCs. Returns rows = mel bands, cols = frames. */
export function mfccToMel(request: MfccToMelRequest): Matrix2D;
export function mfccToMel(...args: unknown[]): Matrix2D {
  return withEntry('mfccToMel', () => {
    const r = requestOnly<MfccToMelRequest>('mfccToMel', args);
    assertMatrix2D(r.coefficients, 'coefficients');
    const out = addon.mfccToMel(
      r.coefficients.data,
      r.coefficients.rows,
      r.coefficients.cols,
      r.nMels ?? 128,
      r.lifter ?? 0,
    );
    return toMatrix2D(out.power, out.nMels, out.nFrames);
  });
}

/** Reconstruct audio from MFCCs via Griffin-Lim. */
export function mfccToAudio(request: MfccToAudioRequest): Float32Array;
export function mfccToAudio(...args: unknown[]): Float32Array {
  return withEntry('mfccToAudio', () => {
    const r = requestOnly<MfccToAudioRequest>('mfccToAudio', args);
    assertMatrix2D(r.coefficients, 'coefficients');
    const sampleRate = r.sampleRate ?? 22050;
    const nFft = r.nFft ?? 2048;
    const hopLength = resolveResynthesisHop(r.hopLength);
    const nIter = resolveIntegerOption('nIter', r.nIter, 32);
    const fminHz = r.fminHz ?? 0;
    const fmaxHz = r.fmaxHz ?? 0;
    assertFloatFields({ fminHz, fmaxHz });
    // An omitted fmaxHz is the core's Nyquist sentinel 0; a given one is a frequency.
    if (r.fmaxHz !== undefined) {
      assertPositiveFloatValue(fmaxHz, 'fmaxHz');
    }
    return addon.mfccToAudio(
      r.coefficients.data,
      r.coefficients.rows,
      r.coefficients.cols,
      r.nMels ?? 128,
      sampleRate,
      nFft,
      hopLength,
      fminHz,
      fmaxHz,
      nIter,
      r.htk ?? false,
      r.lifter ?? 0,
    );
  });
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
    const request = sampleOneShot<PhaseVocoderRequest>('phaseVocoder', args);
    if (typeof request.speedRatio !== 'number') {
      throw refuseType('speedRatio must be a number');
    }
    assertFiniteScalar(request.speedRatio, 'speedRatio');
    assertPositiveFloatValue(request.speedRatio, 'speedRatio');
    assertAudioInput(request.samples, request.sampleRate, request);
    const { nFft, hopLength } = resolveEffectFftOptions(request.nFft, request.hopLength);
    return addon.phaseVocoder(
      request.samples,
      request.sampleRate,
      request.speedRatio,
      nFft,
      hopLength,
    );
  });
}

/** Generate a sine tone. */
export function tone(request: ToneRequest): Float32Array;
export function tone(...args: unknown[]): Float32Array {
  return withEntry('tone', () => {
    const r = requestOnly<ToneRequest>('tone', args);
    const sampleRate = r.sampleRate ?? 22050;
    const frequencyHz = r.frequencyHz ?? 440;
    const durationSec = r.durationSec ?? 1;
    assertFloatFields({ frequencyHz, durationSec });
    return addon.tone(frequencyHz, sampleRate, durationSec, r.phase ?? 0, r.amplitude ?? 1);
  });
}

/** Generate an exponential (default) or linear chirp. */
export function chirp(request: ChirpRequest): Float32Array;
export function chirp(...args: unknown[]): Float32Array {
  return withEntry('chirp', () => {
    const r = requestOnly<ChirpRequest>('chirp', args);
    const sampleRate = r.sampleRate ?? 22050;
    const fminHz = r.fminHz ?? 440;
    const fmaxHz = r.fmaxHz ?? 880;
    const durationSec = r.durationSec ?? 1;
    assertFloatFields({ fminHz, fmaxHz, durationSec });
    return addon.chirp(fminHz, fmaxHz, sampleRate, durationSec, r.linear ?? false);
  });
}

/** Generate a decaying sine click track at times in seconds. */
export function clicks(request: ClicksRequest): Float32Array;
export function clicks(...args: unknown[]): Float32Array {
  return withEntry('clicks', () => {
    const r = requestOnly<ClicksRequest>('clicks', args);
    assertFloat32Array(r.times, 'times');
    const sampleRate = r.sampleRate ?? 22050;
    // An omitted length derives it from the last click.
    const length = r.length;
    const frequencyHz = r.frequencyHz ?? 1000;
    const clickDurationSec = r.clickDurationSec ?? 0.1;
    assertFloatFields({ frequencyHz, clickDurationSec });
    return addon.clicks(r.times, sampleRate, length, frequencyHz, clickDurationSec);
  });
}
