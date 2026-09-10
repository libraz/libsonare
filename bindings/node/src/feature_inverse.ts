import type { FeatureSamplesRequest } from './feature_spectral.js';
import { addon } from './native.js';
import type { InverseMelResult, InverseStftResult } from './types.js';

export interface CqtToAudioRequest {
  magnitude: Float32Array;
  nBins: number;
  nFrames: number;
  sampleRate?: number;
  hopLength?: number;
  fmin?: number;
  binsPerOctave?: number;
  nIter?: number;
}
export interface VqtToAudioRequest extends CqtToAudioRequest {
  gamma?: number;
}
export interface MelToStftRequest {
  mel: Float32Array;
  nMels: number;
  nFrames: number;
  sampleRate?: number;
  nFft?: number;
  fmin?: number;
  fmax?: number;
  htk?: boolean;
}
export interface MelToAudioRequest extends MelToStftRequest {
  hopLength?: number;
  nIter?: number;
}
export interface GriffinLimRequest {
  magnitude: Float32Array;
  nBins: number;
  nFrames: number;
  sampleRate?: number;
  nFft?: number;
  hopLength?: number;
  nIter?: number;
  momentum?: number;
}
export interface MfccToMelRequest {
  mfcc: Float32Array;
  nMfcc: number;
  nFrames: number;
  nMels?: number;
  /** Lifter used by the forward MFCC transform; zero means no liftering. */
  lifter?: number;
}
export interface MfccToAudioRequest extends MfccToMelRequest {
  sampleRate?: number;
  nFft?: number;
  hopLength?: number;
  fmin?: number;
  fmax?: number;
  nIter?: number;
  htk?: boolean;
}

export interface PhaseVocoderRequest extends FeatureSamplesRequest {
  rate: number;
  nFft?: number;
  hopLength?: number;
}

export interface ToneRequest {
  frequency?: number;
  sampleRate?: number;
  duration?: number;
  phase?: number;
  amplitude?: number;
}
export interface ChirpRequest {
  fmin?: number;
  fmax?: number;
  sampleRate?: number;
  duration?: number;
  linear?: boolean;
}
export interface ClicksRequest {
  times: Float32Array;
  sampleRate?: number;
  length?: number;
  frequency?: number;
  clickDuration?: number;
}

/** Reconstruct mono audio from a row-major CQT magnitude via Griffin-Lim. */
export function cqtToAudio(request: CqtToAudioRequest): Float32Array;
export function cqtToAudio(
  magnitude: Float32Array,
  nBins?: number,
  nFrames?: number,
  sampleRate?: number,
  hopLength?: number,
  fmin?: number,
  binsPerOctave?: number,
  nIter?: number,
): Float32Array;
export function cqtToAudio(
  magnitude: Float32Array | CqtToAudioRequest,
  nBins = 0,
  nFrames = 0,
  sampleRate = 22050,
  hopLength = 512,
  fmin = 32.70319566257483,
  binsPerOctave = 12,
  nIter = 32,
): Float32Array {
  const request =
    magnitude instanceof Float32Array
      ? { magnitude, nBins, nFrames, sampleRate, hopLength, fmin, binsPerOctave, nIter }
      : magnitude;
  return addon.cqtToAudio(
    request.magnitude,
    request.nBins,
    request.nFrames,
    request.sampleRate ?? 22050,
    request.hopLength ?? 512,
    request.fmin ?? 32.70319566257483,
    request.binsPerOctave ?? 12,
    request.nIter ?? 32,
  );
}

/** Reconstruct mono audio from a row-major VQT magnitude via Griffin-Lim. */
export function vqtToAudio(request: VqtToAudioRequest): Float32Array;
export function vqtToAudio(
  magnitude: Float32Array,
  nBins?: number,
  nFrames?: number,
  sampleRate?: number,
  hopLength?: number,
  fmin?: number,
  binsPerOctave?: number,
  gamma?: number,
  nIter?: number,
): Float32Array;
export function vqtToAudio(
  magnitude: Float32Array | VqtToAudioRequest,
  nBins = 0,
  nFrames = 0,
  sampleRate = 22050,
  hopLength = 512,
  fmin = 32.70319566257483,
  binsPerOctave = 12,
  gamma = -1,
  nIter = 32,
): Float32Array {
  // The request form delegates to the positional form so the defaults live in
  // exactly one place: `gamma` in particular must reach the core as the
  // automatic-VQT sentinel (-1), not as the constant-Q value (0).
  if (!(magnitude instanceof Float32Array)) {
    return vqtToAudio(
      magnitude.magnitude,
      magnitude.nBins,
      magnitude.nFrames,
      magnitude.sampleRate,
      magnitude.hopLength,
      magnitude.fmin,
      magnitude.binsPerOctave,
      magnitude.gamma,
      magnitude.nIter,
    );
  }
  return addon.vqtToAudio(
    magnitude,
    nBins,
    nFrames,
    sampleRate,
    hopLength,
    fmin,
    binsPerOctave,
    gamma,
    nIter,
  );
}

/** Reconstruct STFT power from a mel spectrogram. */
export function melToStft(request: MelToStftRequest): InverseStftResult;
export function melToStft(
  mel: Float32Array,
  nMels?: number,
  nFrames?: number,
  sampleRate?: number,
  nFft?: number,
  fmin?: number,
  fmax?: number,
  htk?: boolean,
): InverseStftResult;
export function melToStft(
  mel: Float32Array | MelToStftRequest,
  nMels = 0,
  nFrames = 0,
  sampleRate = 22050,
  nFft = 2048,
  fmin = 0,
  fmax = 0,
  htk = false,
): InverseStftResult {
  const request =
    mel instanceof Float32Array ? { mel, nMels, nFrames, sampleRate, nFft, fmin, fmax, htk } : mel;
  return addon.melToStft(
    request.mel,
    request.nMels,
    request.nFrames,
    request.sampleRate ?? 22050,
    request.nFft ?? 2048,
    request.fmin ?? 0,
    request.fmax ?? 0,
    request.htk ?? false,
  );
}

/** Reconstruct audio from a mel spectrogram via Griffin-Lim. */
export function melToAudio(request: MelToAudioRequest): Float32Array;
export function melToAudio(
  mel: Float32Array,
  nMels?: number,
  nFrames?: number,
  sampleRate?: number,
  nFft?: number,
  hopLength?: number,
  fmin?: number,
  fmax?: number,
  nIter?: number,
  htk?: boolean,
): Float32Array;
export function melToAudio(
  mel: Float32Array | MelToAudioRequest,
  nMels = 0,
  nFrames = 0,
  sampleRate = 22050,
  nFft = 2048,
  hopLength = 512,
  fmin = 0,
  fmax = 0,
  nIter = 32,
  htk = false,
): Float32Array {
  const request =
    mel instanceof Float32Array
      ? { mel, nMels, nFrames, sampleRate, nFft, hopLength, fmin, fmax, nIter, htk }
      : mel;
  return addon.melToAudio(
    request.mel,
    request.nMels,
    request.nFrames,
    request.sampleRate ?? 22050,
    request.nFft ?? 2048,
    request.hopLength ?? 512,
    request.fmin ?? 0,
    request.fmax ?? 0,
    request.nIter ?? 32,
    request.htk ?? false,
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
): Float32Array {
  const request =
    magnitude instanceof Float32Array
      ? { magnitude, nBins, nFrames, sampleRate, nFft, hopLength, nIter, momentum }
      : magnitude;
  return addon.griffinLim(
    request.magnitude,
    request.nBins,
    request.nFrames,
    request.sampleRate ?? 22050,
    request.nFft ?? 2048,
    request.hopLength ?? 512,
    request.nIter ?? 32,
    request.momentum ?? 0.99,
  );
}

/** Reconstruct a mel power spectrogram from MFCCs (`nMels` mel bands). */
export function mfccToMel(request: MfccToMelRequest): InverseMelResult;
export function mfccToMel(
  mfcc: Float32Array,
  nMfcc?: number,
  nFrames?: number,
  nMels?: number,
  lifter?: number,
): InverseMelResult;
export function mfccToMel(
  mfcc: Float32Array | MfccToMelRequest,
  nMfcc = 0,
  nFrames = 0,
  nMels = 128,
  lifter = 0,
): InverseMelResult {
  const request = mfcc instanceof Float32Array ? { mfcc, nMfcc, nFrames, nMels, lifter } : mfcc;
  return addon.mfccToMel(
    request.mfcc,
    request.nMfcc,
    request.nFrames,
    request.nMels ?? 128,
    request.lifter ?? 0,
  );
}

/** Reconstruct audio from MFCCs via Griffin-Lim. */
export function mfccToAudio(request: MfccToAudioRequest): Float32Array;
export function mfccToAudio(
  mfcc: Float32Array,
  nMfcc?: number,
  nFrames?: number,
  nMels?: number,
  sampleRate?: number,
  nFft?: number,
  hopLength?: number,
  fmin?: number,
  fmax?: number,
  nIter?: number,
  htk?: boolean,
  lifter?: number,
): Float32Array;
export function mfccToAudio(
  mfcc: Float32Array | MfccToAudioRequest,
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
): Float32Array {
  const request =
    mfcc instanceof Float32Array
      ? { mfcc, nMfcc, nFrames, nMels, sampleRate, nFft, hopLength, fmin, fmax, nIter, htk, lifter }
      : mfcc;
  return addon.mfccToAudio(
    request.mfcc,
    request.nMfcc,
    request.nFrames,
    request.nMels ?? 128,
    request.sampleRate ?? 22050,
    request.nFft ?? 2048,
    request.hopLength ?? 512,
    request.fmin ?? 0,
    request.fmax ?? 0,
    request.nIter ?? 32,
    request.htk ?? false,
    request.lifter ?? 0,
  );
}

/** Phase-vocoder time-scale modification (rate > 1 faster, < 1 slower). */
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
    samples instanceof Float32Array ? { samples, sampleRate, rate, nFft, hopLength } : samples;
  if (typeof request.rate !== 'number' || !Number.isFinite(request.rate)) {
    throw new TypeError('phaseVocoder: rate must be a finite number');
  }
  return addon.phaseVocoder(
    request.samples,
    request.sampleRate ?? 22050,
    request.rate,
    request.nFft ?? 2048,
    request.hopLength ?? 512,
  );
}

/** Generate a sine tone. */
export function tone(request?: ToneRequest): Float32Array;
export function tone(
  frequency?: number,
  sampleRate?: number,
  duration?: number,
  phase?: number,
  amplitude?: number,
): Float32Array;
export function tone(
  frequency: number | ToneRequest = 440,
  sampleRate = 22050,
  duration = 1,
  phase = 0,
  amplitude = 1,
): Float32Array {
  const request =
    typeof frequency === 'number'
      ? { frequency, sampleRate, duration, phase, amplitude }
      : frequency;
  return addon.tone(
    request.frequency ?? 440,
    request.sampleRate ?? 22050,
    request.duration ?? 1,
    request.phase ?? 0,
    request.amplitude ?? 1,
  );
}

/** Generate a linear or exponential chirp. */
export function chirp(request?: ChirpRequest): Float32Array;
export function chirp(
  fmin?: number,
  fmax?: number,
  sampleRate?: number,
  duration?: number,
  linear?: boolean,
): Float32Array;
export function chirp(
  fmin: number | ChirpRequest = 440,
  fmax = 880,
  sampleRate = 22050,
  duration = 1,
  linear = true,
): Float32Array {
  const request = typeof fmin === 'number' ? { fmin, fmax, sampleRate, duration, linear } : fmin;
  return addon.chirp(
    request.fmin ?? 440,
    request.fmax ?? 880,
    request.sampleRate ?? 22050,
    request.duration ?? 1,
    request.linear ?? true,
  );
}

/** Generate a decaying sine click track at times in seconds. */
export function clicks(request: ClicksRequest): Float32Array;
export function clicks(
  times: Float32Array,
  sampleRate?: number,
  length?: number,
  frequency?: number,
  clickDuration?: number,
): Float32Array;
export function clicks(
  times: Float32Array | ClicksRequest,
  sampleRate = 22050,
  length = 0,
  frequency = 1000,
  clickDuration = 0.1,
): Float32Array {
  const request =
    times instanceof Float32Array ? { times, sampleRate, length, frequency, clickDuration } : times;
  return addon.clicks(
    request.times,
    request.sampleRate ?? 22050,
    request.length ?? 0,
    request.frequency ?? 1000,
    request.clickDuration ?? 0.1,
  );
}
