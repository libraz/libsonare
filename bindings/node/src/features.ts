import { addon } from './native.js';
import type { ValidateOptions } from './validation.js';
import { assertSamples } from './validation.js';
import type {
  ChromaResult,
  CqtResult,
  InverseMelResult,
  InverseStftResult,
  LufsResult,
  Matrix2D,
  MelSpectrogramResult,
  MfccResult,
  PitchResult,
  StftDbResult,
  StftResult,
  TempogramMode,
} from './types.js';

export function trim(samples: Float32Array, sampleRate = 22050, thresholdDb = -60.0): Float32Array {
  return addon.trim(samples, sampleRate, thresholdDb);
}

// -- Features --

export function stft(
  samples: Float32Array,
  sampleRate = 22050,
  nFft = 2048,
  hopLength = 512,
): StftResult {
  return addon.stft(samples, sampleRate, nFft, hopLength);
}

export function stftDb(
  samples: Float32Array,
  sampleRate = 22050,
  nFft = 2048,
  hopLength = 512,
): StftDbResult {
  return addon.stftDb(samples, sampleRate, nFft, hopLength);
}

export function melSpectrogram(
  samples: Float32Array,
  sampleRate = 22050,
  nFft = 2048,
  hopLength = 512,
  nMels = 128,
  fmin = 0,
  fmax = 0,
  htk = false,
): MelSpectrogramResult {
  return addon.melSpectrogram(samples, sampleRate, nFft, hopLength, nMels, fmin, fmax, htk);
}

export function mfcc(
  samples: Float32Array,
  sampleRate = 22050,
  nFft = 2048,
  hopLength = 512,
  nMels = 128,
  nMfcc = 20,
  fmin = 0,
  fmax = 0,
  htk = false,
): MfccResult {
  return addon.mfcc(samples, sampleRate, nFft, hopLength, nMels, nMfcc, fmin, fmax, htk);
}

export function chroma(
  samples: Float32Array,
  sampleRate = 22050,
  nFft = 2048,
  hopLength = 512,
): ChromaResult {
  return addon.chroma(samples, sampleRate, nFft, hopLength);
}

/** Compute the Constant-Q Transform magnitude. */
export function cqt(
  samples: Float32Array,
  sampleRate = 22050,
  hopLength = 512,
  fmin = 32.70319566257483,
  nBins = 84,
  binsPerOctave = 12,
): CqtResult {
  return addon.cqt(samples, sampleRate, hopLength, fmin, nBins, binsPerOctave);
}

/** Compute the Variable-Q Transform magnitude (`gamma` controls Q). */
export function vqt(
  samples: Float32Array,
  sampleRate = 22050,
  hopLength = 512,
  fmin = 32.70319566257483,
  nBins = 84,
  binsPerOctave = 12,
  gamma = 0.0,
): CqtResult {
  return addon.vqt(samples, sampleRate, hopLength, fmin, nBins, binsPerOctave, gamma);
}

/** Reconstruct a linear STFT magnitude from a mel spectrogram. */
export function melToStft(
  mel: Float32Array,
  nMels: number,
  nFrames: number,
  sampleRate = 22050,
  nFft = 2048,
  fmin = 0,
  fmax = 0,
): InverseStftResult {
  return addon.melToStft(mel, nMels, nFrames, sampleRate, nFft, fmin, fmax);
}

/** Reconstruct audio from a mel spectrogram via Griffin-Lim. */
export function melToAudio(
  mel: Float32Array,
  nMels: number,
  nFrames: number,
  sampleRate = 22050,
  nFft = 2048,
  hopLength = 512,
  fmin = 0,
  fmax = 0,
  nIter = 32,
): Float32Array {
  return addon.melToAudio(mel, nMels, nFrames, sampleRate, nFft, hopLength, fmin, fmax, nIter);
}

/** Reconstruct a mel spectrogram from MFCCs (`nMels` mel bands, dB scale). */
export function mfccToMel(
  mfcc: Float32Array,
  nMfcc: number,
  nFrames: number,
  nMels = 128,
): InverseMelResult {
  return addon.mfccToMel(mfcc, nMfcc, nFrames, nMels);
}

/** Reconstruct audio from MFCCs via Griffin-Lim. */
export function mfccToAudio(
  mfcc: Float32Array,
  nMfcc: number,
  nFrames: number,
  nMels = 128,
  sampleRate = 22050,
  nFft = 2048,
  hopLength = 512,
  fmin = 0,
  fmax = 0,
  nIter = 32,
): Float32Array {
  return addon.mfccToAudio(
    mfcc,
    nMfcc,
    nFrames,
    nMels,
    sampleRate,
    nFft,
    hopLength,
    fmin,
    fmax,
    nIter,
  );
}

export function spectralCentroid(
  samples: Float32Array,
  sampleRate = 22050,
  nFft = 2048,
  hopLength = 512,
): Float32Array {
  return addon.spectralCentroid(samples, sampleRate, nFft, hopLength);
}

/** Spectral contrast (librosa.feature.spectral_contrast); (nBands+1) x nFrames. */
export function spectralContrast(
  samples: Float32Array,
  sampleRate = 22050,
  nFft = 2048,
  hopLength = 512,
  nBands = 6,
  fmin = 200.0,
  quantile = 0.02,
): Matrix2D {
  return addon.spectralContrast(samples, sampleRate, nFft, hopLength, nBands, fmin, quantile);
}

/** Per-frame polynomial coefficients (librosa.feature.poly_features); (order+1) x nFrames. */
export function polyFeatures(
  samples: Float32Array,
  sampleRate = 22050,
  nFft = 2048,
  hopLength = 512,
  order = 1,
): Matrix2D {
  return addon.polyFeatures(samples, sampleRate, nFft, hopLength, order);
}

/** Zero-crossing indices of a signal (librosa.zero_crossings). */
export function zeroCrossings(
  samples: Float32Array,
  threshold = 1e-10,
  refMagnitude = false,
  pad = true,
  zeroPos = true,
): Int32Array {
  return addon.zeroCrossings(samples, threshold, refMagnitude, pad, zeroPos);
}

/** Global tuning offset from a set of frequencies (librosa.pitch_tuning). */
export function pitchTuning(
  frequencies: Float32Array,
  resolution = 0.01,
  binsPerOctave = 12,
): number {
  return addon.pitchTuning(frequencies, resolution, binsPerOctave);
}

/** Tuning offset of an audio signal (librosa.estimate_tuning). */
export function estimateTuning(
  samples: Float32Array,
  sampleRate = 22050,
  nFft = 2048,
  hopLength = 512,
  resolution = 0.01,
  binsPerOctave = 12,
): number {
  return addon.estimateTuning(samples, sampleRate, nFft, hopLength, resolution, binsPerOctave);
}

/**
 * NMF of a flattened [nFeatures x nFrames] spectrogram (librosa.decompose.decompose).
 *
 * `init` selects the initialiser: `'random'` (default, deterministic seed) or
 * `'nndsvd'` (SVD-based warm start, which tends to converge in fewer iterations).
 */
export function decompose(
  s: Float32Array,
  nFeatures: number,
  nFrames: number,
  nComponents: number,
  nIter = 50,
  beta = 2.0,
  init: 'random' | 'nndsvd' = 'random',
): { w: Matrix2D; h: Matrix2D } {
  return addon.decompose(s, nFeatures, nFrames, nComponents, nIter, beta, init);
}

/** Nearest-neighbour filtering of a flattened [nFeatures x nFrames] spectrogram. */
export function nnFilter(
  s: Float32Array,
  nFeatures: number,
  nFrames: number,
  aggregate = 'mean',
  k = 7,
  width = 1,
): Matrix2D {
  return addon.nnFilter(s, nFeatures, nFrames, aggregate, k, width);
}

/** Reorder/concatenate a signal by (start,end) interval slices (librosa.effects.remix). */
export function remix(
  samples: Float32Array,
  intervals: Int32Array,
  sampleRate = 22050,
  alignZeros = false,
): Float32Array {
  return addon.remix(samples, intervals, sampleRate, alignZeros);
}

/** Phase-vocoder time-scale modification (rate > 1 faster, < 1 slower). */
export function phaseVocoder(
  samples: Float32Array,
  rate: number,
  sampleRate = 22050,
  nFft = 2048,
  hopLength = 512,
): Float32Array {
  if (typeof rate !== 'number' || !Number.isFinite(rate)) {
    throw new TypeError('phaseVocoder: rate must be a finite number');
  }
  return addon.phaseVocoder(samples, sampleRate, rate, nFft, hopLength);
}

/** HPSS into harmonic / percussive / residual signals. */
export function hpssWithResidual(
  samples: Float32Array,
  sampleRate = 22050,
  kernelHarmonic = 31,
  kernelPercussive = 31,
): {
  harmonic: Float32Array;
  percussive: Float32Array;
  residual: Float32Array;
  sampleRate: number;
} {
  return addon.hpssWithResidual(samples, sampleRate, kernelHarmonic, kernelPercussive);
}

/**
 * Channel-weighted multichannel loudness + LRA (BS.1770 / EBU R128) from an
 * interleaved buffer of `frames * channels` samples. The per-channel frame
 * count is derived from the buffer length and `channels`.
 */
export function lufsInterleaved(
  samples: Float32Array,
  channels: number,
  sampleRate = 22050,
): LufsResult {
  return addon.lufsInterleaved(samples, channels, sampleRate);
}

/** Standards-compliant EBU R128 loudness range (LRA) in LU. */
export function ebur128LoudnessRange(samples: Float32Array, sampleRate = 22050): number {
  return addon.ebur128LoudnessRange(samples, sampleRate);
}

export function spectralBandwidth(
  samples: Float32Array,
  sampleRate = 22050,
  nFft = 2048,
  hopLength = 512,
): Float32Array {
  return addon.spectralBandwidth(samples, sampleRate, nFft, hopLength);
}

export function spectralRolloff(
  samples: Float32Array,
  sampleRate = 22050,
  nFft = 2048,
  hopLength = 512,
  rollPercent = 0.85,
): Float32Array {
  return addon.spectralRolloff(samples, sampleRate, nFft, hopLength, rollPercent);
}

export function spectralFlatness(
  samples: Float32Array,
  sampleRate = 22050,
  nFft = 2048,
  hopLength = 512,
): Float32Array {
  return addon.spectralFlatness(samples, sampleRate, nFft, hopLength);
}

export function zeroCrossingRate(
  samples: Float32Array,
  sampleRate = 22050,
  frameLength = 2048,
  hopLength = 512,
): Float32Array {
  return addon.zeroCrossingRate(samples, sampleRate, frameLength, hopLength);
}

export function rmsEnergy(
  samples: Float32Array,
  sampleRate = 22050,
  frameLength = 2048,
  hopLength = 512,
): Float32Array {
  return addon.rmsEnergy(samples, sampleRate, frameLength, hopLength);
}

export function pitchYin(
  samples: Float32Array,
  sampleRate = 22050,
  frameLength = 2048,
  hopLength = 512,
  fmin = 65.0,
  fmax = 2093.0,
  threshold = 0.3,
  fillNa = false,
): PitchResult {
  return addon.pitchYin(samples, sampleRate, frameLength, hopLength, fmin, fmax, threshold, fillNa);
}

export function pitchPyin(
  samples: Float32Array,
  sampleRate = 22050,
  frameLength = 2048,
  hopLength = 512,
  fmin = 65.0,
  fmax = 2093.0,
  threshold = 0.3,
  fillNa = false,
): PitchResult {
  return addon.pitchPyin(
    samples,
    sampleRate,
    frameLength,
    hopLength,
    fmin,
    fmax,
    threshold,
    fillNa,
  );
}

// -- Core --

export function hzToMel(hz: number): number {
  return addon.hzToMel(hz);
}

export function melToHz(mel: number): number {
  return addon.melToHz(mel);
}

export function hzToMidi(hz: number): number {
  return addon.hzToMidi(hz);
}

export function midiToHz(midi: number): number {
  return addon.midiToHz(midi);
}

export function hzToNote(hz: number): string {
  return addon.hzToNote(hz);
}

export function noteToHz(note: string): number {
  return addon.noteToHz(note);
}

export function framesToTime(frames: number, sr = 22050, hopLength = 512): number {
  return addon.framesToTime(frames, sr, hopLength);
}

export function timeToFrames(time: number, sr = 22050, hopLength = 512): number {
  return addon.timeToFrames(time, sr, hopLength);
}

export function framesToSamples(frames: number, hopLength = 512, nFft = 0): number {
  return addon.framesToSamples(frames, hopLength, nFft);
}

export function samplesToFrames(samples: number, hopLength = 512, nFft = 0): number {
  return addon.samplesToFrames(samples, hopLength, nFft);
}

export function powerToDb(
  values: Float32Array,
  ref = 1.0,
  amin = 1e-10,
  topDb = 80.0,
): Float32Array {
  return addon.powerToDb(values, ref, amin, topDb);
}

export function amplitudeToDb(
  values: Float32Array,
  ref = 1.0,
  amin = 1e-5,
  topDb = 80.0,
): Float32Array {
  return addon.amplitudeToDb(values, ref, amin, topDb);
}

export function dbToPower(values: Float32Array, ref = 1.0): Float32Array {
  return addon.dbToPower(values, ref);
}

export function dbToAmplitude(values: Float32Array, ref = 1.0): Float32Array {
  return addon.dbToAmplitude(values, ref);
}

export function preemphasis(samples: Float32Array, coef = 0.97, zi?: number): Float32Array {
  return zi === undefined ? addon.preemphasis(samples, coef) : addon.preemphasis(samples, coef, zi);
}

export function deemphasis(samples: Float32Array, coef = 0.97, zi?: number): Float32Array {
  return zi === undefined ? addon.deemphasis(samples, coef) : addon.deemphasis(samples, coef, zi);
}

export function trimSilence(
  samples: Float32Array,
  topDb = 60.0,
  frameLength = 2048,
  hopLength = 512,
): { audio: Float32Array; startSample: number; endSample: number } {
  return addon.trimSilence(samples, topDb, frameLength, hopLength);
}

export function splitSilence(
  samples: Float32Array,
  topDb = 60.0,
  frameLength = 2048,
  hopLength = 512,
): Int32Array {
  return addon.splitSilence(samples, topDb, frameLength, hopLength);
}

export function frameSignal(
  samples: Float32Array,
  frameLength: number,
  hopLength: number,
): { nFrames: number; frames: Float32Array } {
  return addon.frameSignal(samples, frameLength, hopLength);
}

export function padCenter(values: Float32Array, targetSize: number, padValue = 0.0): Float32Array {
  return addon.padCenter(values, targetSize, padValue);
}

export function fixLength(values: Float32Array, targetSize: number, padValue = 0.0): Float32Array {
  return addon.fixLength(values, targetSize, padValue);
}

export function fixFrames(
  frames: Int32Array | number[],
  xMin = 0,
  xMax = -1,
  pad = true,
): Int32Array {
  return addon.fixFrames(frames, xMin, xMax, pad);
}

export function peakPick(
  values: Float32Array,
  preMax: number,
  postMax: number,
  preAvg: number,
  postAvg: number,
  delta: number,
  wait: number,
): Int32Array {
  return addon.peakPick(values, preMax, postMax, preAvg, postAvg, delta, wait);
}

export function vectorNormalize(values: Float32Array, normType = 0, threshold = 0.0): Float32Array {
  return addon.vectorNormalize(values, normType, threshold);
}

/**
 * Tuning parameters for {@link pcen} (per-channel energy normalization). All
 * fields are optional; omitted keys fall back to librosa-compatible defaults.
 */
export interface PcenOptions {
  /** Sample rate used to derive the smoothing time constant (default 22050). */
  sampleRate?: number;
  /** Hop length used to derive the smoothing time constant (default 512). */
  hopLength?: number;
  /** Smoothing filter time constant in seconds (default 0.4). */
  timeConstant?: number;
  /** Gain exponent applied to the smoothed energy (default 0.98). */
  gain?: number;
  /** Bias added before the power compression (default 2.0). */
  bias?: number;
  /** Power exponent of the final compression (default 0.5). */
  power?: number;
  /** Numerical floor to avoid division by zero (default 1e-6). */
  eps?: number;
}

export function pcen(
  values: Float32Array,
  nBins: number,
  nFrames: number,
  options: PcenOptions = {},
): Float32Array {
  return addon.pcen(values, nBins, nFrames, options);
}

export function tonnetz(chromagram: Float32Array, nChroma: number, nFrames: number): Float32Array {
  return addon.tonnetz(chromagram, nChroma, nFrames);
}

export function tempogram(
  onsetEnvelope: Float32Array,
  sampleRate = 22050,
  hopLength = 512,
  winLength = 384,
  mode: TempogramMode = 'autocorrelation',
): { nFrames: number; winLength: number; data: Float32Array } {
  return addon.tempogram(onsetEnvelope, sampleRate, hopLength, winLength, mode);
}

export function cyclicTempogram(
  onsetEnvelope: Float32Array,
  sampleRate = 22050,
  hopLength = 512,
  winLength = 384,
  bpmMin = 60.0,
  nBins = 60,
): { nFrames: number; nBins: number; data: Float32Array } {
  return addon.cyclicTempogram(onsetEnvelope, sampleRate, hopLength, winLength, bpmMin, nBins);
}

export function plp(
  onsetEnvelope: Float32Array,
  sampleRate = 22050,
  hopLength = 512,
  tempoMin = 30.0,
  tempoMax = 300.0,
  winLength = 384,
): Float32Array {
  return addon.plp(onsetEnvelope, sampleRate, hopLength, tempoMin, tempoMax, winLength);
}

export function onsetEnvelope(
  samples: Float32Array,
  sampleRate = 22050,
  nFft = 2048,
  hopLength = 512,
  nMels = 128,
): Float32Array {
  return addon.onsetEnvelope(samples, sampleRate, nFft, hopLength, nMels);
}

export function fourierTempogram(
  onsetEnvelope: Float32Array,
  sampleRate = 22050,
  hopLength = 512,
  winLength = 384,
): { nBins: number; nFrames: number; data: Float32Array } {
  return addon.fourierTempogram(onsetEnvelope, sampleRate, hopLength, winLength);
}

export function tempogramRatio(
  tempogramData: Float32Array,
  winLength = 384,
  sampleRate = 22050,
  hopLength = 512,
  factors?: Float32Array | number[],
): Float32Array {
  return addon.tempogramRatio(tempogramData, winLength, sampleRate, hopLength, factors);
}

export function nnlsChroma(
  samples: Float32Array,
  sampleRate = 22050,
): { nChroma: number; nFrames: number; data: Float32Array } {
  return addon.nnlsChroma(samples, sampleRate);
}

export function lufs(
  samples: Float32Array,
  sampleRate = 22050,
  options: ValidateOptions = {},
): LufsResult {
  assertSamples('lufs', samples, options.validate !== false);
  return addon.lufs(samples, sampleRate);
}

export function momentaryLufs(
  samples: Float32Array,
  sampleRate = 22050,
  options: ValidateOptions = {},
): Float32Array {
  assertSamples('momentaryLufs', samples, options.validate !== false);
  return addon.momentaryLufs(samples, sampleRate);
}

export function shortTermLufs(
  samples: Float32Array,
  sampleRate = 22050,
  options: ValidateOptions = {},
): Float32Array {
  assertSamples('shortTermLufs', samples, options.validate !== false);
  return addon.shortTermLufs(samples, sampleRate);
}
