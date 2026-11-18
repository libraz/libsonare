import { assertMatrix2D, toMatrix2D } from './internal/feature_matrix.js';
import { resolveIntegerOption, resolvePositiveIntegerOption } from './internal/feature_options.js';
import { quarterHop } from './internal/fft_options.js';
import { addon } from './native.js';
import type { Matrix2D } from './types.js';
import type { OptionsOf, ValidateOptions } from './validation.js';
import {
  assertAudioInput,
  assertFiniteScalar,
  assertFloat32Array,
  assertFloatFields,
  assertIntegralSampleRate,
  assertPositiveFloatValue,
  assertPositiveInteger,
  assertSampleRate,
  requestOnly,
  resolveOptionalNonNegative,
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
  /** `false` skips the NaN/Inf pre-scan of the matrix. */
  validate?: boolean;
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
  /** `false` skips the NaN/Inf pre-scan of the matrix. Default `true`. */
  validate?: boolean;
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
  /** `false` skips the NaN/Inf pre-scan of the matrix. Default `true`. */
  validate?: boolean;
}

/** Request for {@link mfccToMel}. */
export interface MfccToMelRequest {
  /** Coefficients: rows = coefficients, cols = frames. */
  coefficients: Matrix2D;
  /** Mel bands to reconstruct. Default 128. */
  nMels?: number;
  /** Lifter used by the forward MFCC transform; zero means no liftering. */
  lifter?: number;
  /** `false` skips the NaN/Inf pre-scan of the matrix. Default `true`. */
  validate?: boolean;
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
export interface PhaseVocoderRequest extends ValidateOptions {
  /** Mono audio samples. */
  samples: Float32Array;
  /** Sample rate in Hz, an integer in `[8000, 384000]`; there is no default. */
  sampleRate: number;
  /**
   * Playback speed as a ratio, finite: above 1 is faster and shorter, below 1
   * slower and longer, the opposite sense of a note's `stretchRatio`. Required.
   */
  speedRatio: number;
  /** FFT size in samples. Default 2048. */
  nFft?: number;
  /** Hop in samples. Default 512. */
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
  const r = requestOnly<CqtToAudioRequest>('cqtToAudio', args);
  assertMatrix2D('cqtToAudio', r.magnitude, 'magnitude', r.validate);
  const sampleRate = r.sampleRate;
  assertSampleRate('cqtToAudio', sampleRate);
  // Positivity only: the core takes any positive hop and any positive iteration
  // count, and names the Griffin-Lim ceiling itself when one exceeds it.
  const hopLength = resolvePositiveIntegerOption('cqtToAudio', 'hopLength', r.hopLength, 512);
  const nIter = resolvePositiveIntegerOption('cqtToAudio', 'nIter', r.nIter, 32);
  const fminHz = r.fminHz ?? DEFAULT_CQT_FMIN_HZ;
  assertPositiveFloatValue('cqtToAudio', fminHz, 'fminHz');
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
}

/** Reconstruct mono audio from a VQT magnitude via Griffin-Lim. */
export function vqtToAudio(request: VqtToAudioRequest): Float32Array;
export function vqtToAudio(...args: unknown[]): Float32Array {
  const r = requestOnly<VqtToAudioRequest>('vqtToAudio', args);
  assertMatrix2D('vqtToAudio', r.magnitude, 'magnitude', r.validate);
  const sampleRate = r.sampleRate;
  assertSampleRate('vqtToAudio', sampleRate);
  const hopLength = resolvePositiveIntegerOption('vqtToAudio', 'hopLength', r.hopLength, 512);
  const nIter = resolvePositiveIntegerOption('vqtToAudio', 'nIter', r.nIter, 32);
  const fminHz = r.fminHz ?? DEFAULT_CQT_FMIN_HZ;
  assertPositiveFloatValue('vqtToAudio', fminHz, 'fminHz');
  return addon.vqtToAudio(
    r.magnitude.data,
    r.magnitude.rows,
    r.magnitude.cols,
    sampleRate,
    hopLength,
    fminHz,
    r.binsPerOctave ?? 12,
    resolveOptionalNonNegative('vqtToAudio', r.gamma, 'gamma'),
    nIter,
  );
}

/** Reconstruct STFT magnitude from a mel spectrogram. Returns rows = `nFft / 2 + 1` bins, cols = frames. */
export function melToStft(request: MelToStftRequest): Matrix2D;
export function melToStft(...args: unknown[]): Matrix2D {
  const r = requestOnly<MelToStftRequest>('melToStft', args);
  assertMatrix2D('melToStft', r.power, 'power', r.validate);
  const sampleRate = r.sampleRate ?? 22050;
  // No range bound here: the core only requires sample_rate > 0 for this
  // reconstruction, unlike the [8000, 384000] audio-analysis bound.
  assertPositiveInteger('melToStft', sampleRate, 'sampleRate');
  // Positivity only: the filterbank inverse takes any positive size, so the
  // even-size rule the STFT entries carry would refuse sizes this one answers.
  const nFft = resolvePositiveIntegerOption('melToStft', 'nFft', r.nFft, 2048);
  const fminHz = r.fminHz ?? 0;
  const fmaxHz = r.fmaxHz ?? 0;
  assertFloatFields('melToStft', { fminHz, fmaxHz });
  // An omitted fmaxHz is the core's Nyquist sentinel 0; a given one is a frequency.
  if (r.fmaxHz !== undefined) {
    assertPositiveFloatValue('melToStft', fmaxHz, 'fmaxHz');
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
}

/** Reconstruct audio from a mel spectrogram via Griffin-Lim. */
export function melToAudio(request: MelToAudioRequest): Float32Array;
export function melToAudio(...args: unknown[]): Float32Array {
  const r = requestOnly<MelToAudioRequest>('melToAudio', args);
  assertMatrix2D('melToAudio', r.power, 'power', r.validate);
  const sampleRate = r.sampleRate ?? 22050;
  assertPositiveInteger('melToAudio', sampleRate, 'sampleRate');
  // Positivity only, as melToStft: the geometry rule this reconstruction carries
  // is that all three are positive.
  const nFft = resolvePositiveIntegerOption('melToAudio', 'nFft', r.nFft, 2048);
  const hopLength = resolvePositiveIntegerOption(
    'melToAudio',
    'hopLength',
    r.hopLength,
    quarterHop(nFft),
  );
  const nIter = resolveIntegerOption('melToAudio', 'nIter', r.nIter, 32);
  const fminHz = r.fminHz ?? 0;
  const fmaxHz = r.fmaxHz ?? 0;
  assertFloatFields('melToAudio', { fminHz, fmaxHz });
  // An omitted fmaxHz is the core's Nyquist sentinel 0; a given one is a frequency.
  if (r.fmaxHz !== undefined) {
    assertPositiveFloatValue('melToAudio', fmaxHz, 'fmaxHz');
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
}

/** A Griffin-Lim magnitude has `nFft / 2 + 1` rows, so an explicit size must agree with them. */
function assertGriffinLimFftSize(nBins: number, nFft: number): void {
  if (nFft !== (nBins - 1) * 2) {
    throw new RangeError(
      `griffinLim: nFft must equal (nBins - 1) * 2 (got ${nFft} for ${nBins} bins)`,
    );
  }
}

/** Reconstruct audio from an STFT magnitude matrix via Griffin-Lim. */
export function griffinLim(request: GriffinLimRequest): Float32Array;
export function griffinLim(...args: unknown[]): Float32Array {
  const r = requestOnly<GriffinLimRequest>('griffinLim', args);
  assertMatrix2D('griffinLim', r.magnitude, 'magnitude', r.validate);
  const sampleRate = r.sampleRate;
  assertSampleRate('griffinLim', sampleRate);
  const nBins = r.magnitude.rows;
  const nFft = resolvePositiveIntegerOption('griffinLim', 'nFft', r.nFft, (nBins - 1) * 2);
  assertGriffinLimFftSize(nBins, nFft);
  const hopLength = resolvePositiveIntegerOption(
    'griffinLim',
    'hopLength',
    r.hopLength,
    quarterHop(nFft),
  );
  const nIter = resolvePositiveIntegerOption('griffinLim', 'nIter', r.nIter, 32);
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
}

/** Reconstruct a mel power spectrogram from MFCCs. Returns rows = mel bands, cols = frames. */
export function mfccToMel(request: MfccToMelRequest): Matrix2D;
export function mfccToMel(...args: unknown[]): Matrix2D {
  const r = requestOnly<MfccToMelRequest>('mfccToMel', args);
  assertMatrix2D('mfccToMel', r.coefficients, 'coefficients', r.validate);
  const out = addon.mfccToMel(
    r.coefficients.data,
    r.coefficients.rows,
    r.coefficients.cols,
    r.nMels ?? 128,
    r.lifter ?? 0,
  );
  return toMatrix2D(out.power, out.nMels, out.nFrames);
}

/** Reconstruct audio from MFCCs via Griffin-Lim. */
export function mfccToAudio(request: MfccToAudioRequest): Float32Array;
export function mfccToAudio(...args: unknown[]): Float32Array {
  const r = requestOnly<MfccToAudioRequest>('mfccToAudio', args);
  assertMatrix2D('mfccToAudio', r.coefficients, 'coefficients', r.validate);
  const sampleRate = r.sampleRate ?? 22050;
  assertPositiveInteger('mfccToAudio', sampleRate, 'sampleRate');
  // Positivity only, as melToAudio: the same reconstruction behind one more
  // inverse transform, under the same geometry rule.
  const nFft = resolvePositiveIntegerOption('mfccToAudio', 'nFft', r.nFft, 2048);
  const hopLength = resolvePositiveIntegerOption(
    'mfccToAudio',
    'hopLength',
    r.hopLength,
    quarterHop(nFft),
  );
  const nIter = resolveIntegerOption('mfccToAudio', 'nIter', r.nIter, 32);
  const fminHz = r.fminHz ?? 0;
  const fmaxHz = r.fmaxHz ?? 0;
  assertFloatFields('mfccToAudio', { fminHz, fmaxHz });
  // An omitted fmaxHz is the core's Nyquist sentinel 0; a given one is a frequency.
  if (r.fmaxHz !== undefined) {
    assertPositiveFloatValue('mfccToAudio', fmaxHz, 'fmaxHz');
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
}

/**
 * Phase-vocoder time-scale modification by `speedRatio`.
 *
 * @throws {TypeError} when `sampleRate` or `speedRatio` is missing
 * @throws {RangeError} on audio out of domain, an out-of-range `sampleRate` or a non-finite `speedRatio`
 */
export function phaseVocoder(request: PhaseVocoderRequest): Float32Array;
export function phaseVocoder(
  samples: Float32Array,
  sampleRate: number,
  options: OptionsOf<PhaseVocoderRequest>,
): Float32Array;
export function phaseVocoder(...args: unknown[]): Float32Array {
  const request = sampleOneShot<PhaseVocoderRequest>('phaseVocoder', args);
  if (typeof request.speedRatio !== 'number') {
    throw new TypeError('phaseVocoder: speedRatio must be a number');
  }
  assertFiniteScalar('phaseVocoder', request.speedRatio, 'speedRatio');
  assertPositiveFloatValue('phaseVocoder', request.speedRatio, 'speedRatio');
  assertAudioInput('phaseVocoder', request.samples, request.sampleRate, request);
  // Positivity only: the analysis geometry is an StftConfig, which requires a
  // positive size and hop and reports its own size ceiling by name.
  const nFft = resolvePositiveIntegerOption('phaseVocoder', 'nFft', request.nFft, 2048);
  const hopLength = resolvePositiveIntegerOption(
    'phaseVocoder',
    'hopLength',
    request.hopLength,
    512,
  );
  return addon.phaseVocoder(
    request.samples,
    request.sampleRate,
    request.speedRatio,
    nFft,
    hopLength,
  );
}

/** Generate a sine tone. */
export function tone(request: ToneRequest): Float32Array;
export function tone(...args: unknown[]): Float32Array {
  const r = requestOnly<ToneRequest>('tone', args);
  // Integrality only: a generator's rate has no domain in the core, so the
  // audio-analysis bound would refuse rates it renders correctly today.
  const sampleRate = r.sampleRate ?? 22050;
  assertIntegralSampleRate('tone', sampleRate);
  const frequencyHz = r.frequencyHz ?? 440;
  const durationSec = r.durationSec ?? 1;
  assertFloatFields('tone', { frequencyHz, durationSec });
  return addon.tone(frequencyHz, sampleRate, durationSec, r.phase ?? 0, r.amplitude ?? 1);
}

/** Generate an exponential (default) or linear chirp. */
export function chirp(request: ChirpRequest): Float32Array;
export function chirp(...args: unknown[]): Float32Array {
  const r = requestOnly<ChirpRequest>('chirp', args);
  // Integrality only, as `tone`: a generator's rate has no domain in the core.
  const sampleRate = r.sampleRate ?? 22050;
  assertIntegralSampleRate('chirp', sampleRate);
  const fminHz = r.fminHz ?? 440;
  const fmaxHz = r.fmaxHz ?? 880;
  const durationSec = r.durationSec ?? 1;
  assertFloatFields('chirp', { fminHz, fmaxHz, durationSec });
  return addon.chirp(fminHz, fmaxHz, sampleRate, durationSec, r.linear ?? false);
}

/** Generate a decaying sine click track at times in seconds. */
export function clicks(request: ClicksRequest): Float32Array;
export function clicks(...args: unknown[]): Float32Array {
  const r = requestOnly<ClicksRequest>('clicks', args);
  assertFloat32Array('clicks', r.times, 'times');
  // Integrality only, as `tone`: a generator's rate has no domain in the core.
  const sampleRate = r.sampleRate ?? 22050;
  assertIntegralSampleRate('clicks', sampleRate);
  // The native 0 means "derive from the last click"; the facade spells that as absence.
  let length = 0;
  if (r.length !== undefined && r.length !== null) {
    if (typeof r.length !== 'number') {
      throw new TypeError('clicks: length must be a number');
    }
    assertPositiveInteger('clicks', r.length, 'length');
    length = r.length;
  }
  const frequencyHz = r.frequencyHz ?? 1000;
  const clickDurationSec = r.clickDurationSec ?? 0.1;
  assertFloatFields('clicks', { frequencyHz, clickDurationSec });
  return addon.clicks(r.times, sampleRate, length, frequencyHz, clickDurationSec);
}
