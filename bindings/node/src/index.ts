import { createRequire } from 'node:module';
import type {
  AnalysisResult,
  ChromaResult,
  HpssResult,
  Key,
  MelSpectrogramResult,
  MfccResult,
  PitchResult,
  StftDbResult,
  StftResult,
} from './types.js';

const require = createRequire(import.meta.url);
const addon = require('../build/Release/sonare-node.node');

/**
 * Audio object wrapping decoded audio samples.
 */
export class Audio {
  private native: InstanceType<typeof addon.Audio>;

  private constructor(native: InstanceType<typeof addon.Audio>) {
    this.native = native;
  }

  static fromFile(path: string): Audio {
    return new Audio(addon.Audio.fromFile(path));
  }

  static fromBuffer(samples: Float32Array, sampleRate = 22050): Audio {
    return new Audio(addon.Audio.fromBuffer(samples, sampleRate));
  }

  static fromMemory(data: Buffer | Uint8Array): Audio {
    return new Audio(addon.Audio.fromMemory(data));
  }

  getData(): Float32Array {
    return this.native.getData();
  }

  getLength(): number {
    return this.native.getLength();
  }

  getSampleRate(): number {
    return this.native.getSampleRate();
  }

  getDuration(): number {
    return this.native.getDuration();
  }

  destroy(): void {
    this.native.destroy();
  }

  // -- Analysis --

  detectBpm(): number {
    return addon.detectBpm(this.getData(), this.getSampleRate());
  }

  detectKey(): Key {
    return addon.detectKey(this.getData(), this.getSampleRate());
  }

  detectBeats(): Float32Array {
    return addon.detectBeats(this.getData(), this.getSampleRate());
  }

  detectOnsets(): Float32Array {
    return addon.detectOnsets(this.getData(), this.getSampleRate());
  }

  analyze(): AnalysisResult {
    return addon.analyze(this.getData(), this.getSampleRate());
  }

  // -- Effects --

  hpss(kernelHarmonic = 31, kernelPercussive = 31): HpssResult {
    return addon.hpss(this.getData(), this.getSampleRate(), kernelHarmonic, kernelPercussive);
  }

  harmonic(): Float32Array {
    return addon.harmonic(this.getData(), this.getSampleRate());
  }

  percussive(): Float32Array {
    return addon.percussive(this.getData(), this.getSampleRate());
  }

  timeStretch(rate: number): Float32Array {
    return addon.timeStretch(this.getData(), this.getSampleRate(), rate);
  }

  pitchShift(semitones: number): Float32Array {
    return addon.pitchShift(this.getData(), this.getSampleRate(), semitones);
  }

  normalize(targetDb = 0.0): Float32Array {
    return addon.normalize(this.getData(), this.getSampleRate(), targetDb);
  }

  trim(thresholdDb = -60.0): Float32Array {
    return addon.trim(this.getData(), this.getSampleRate(), thresholdDb);
  }

  // -- Features --

  stft(nFft = 2048, hopLength = 512): StftResult {
    return addon.stft(this.getData(), this.getSampleRate(), nFft, hopLength);
  }

  stftDb(nFft = 2048, hopLength = 512): StftDbResult {
    return addon.stftDb(this.getData(), this.getSampleRate(), nFft, hopLength);
  }

  melSpectrogram(nFft = 2048, hopLength = 512, nMels = 128): MelSpectrogramResult {
    return addon.melSpectrogram(this.getData(), this.getSampleRate(), nFft, hopLength, nMels);
  }

  mfcc(nFft = 2048, hopLength = 512, nMels = 128, nMfcc = 13): MfccResult {
    return addon.mfcc(this.getData(), this.getSampleRate(), nFft, hopLength, nMels, nMfcc);
  }

  chroma(nFft = 2048, hopLength = 512): ChromaResult {
    return addon.chroma(this.getData(), this.getSampleRate(), nFft, hopLength);
  }

  spectralCentroid(nFft = 2048, hopLength = 512): Float32Array {
    return addon.spectralCentroid(this.getData(), this.getSampleRate(), nFft, hopLength);
  }

  spectralBandwidth(nFft = 2048, hopLength = 512): Float32Array {
    return addon.spectralBandwidth(this.getData(), this.getSampleRate(), nFft, hopLength);
  }

  spectralRolloff(nFft = 2048, hopLength = 512, rollPercent = 0.85): Float32Array {
    return addon.spectralRolloff(
      this.getData(),
      this.getSampleRate(),
      nFft,
      hopLength,
      rollPercent,
    );
  }

  spectralFlatness(nFft = 2048, hopLength = 512): Float32Array {
    return addon.spectralFlatness(this.getData(), this.getSampleRate(), nFft, hopLength);
  }

  zeroCrossingRate(frameLength = 2048, hopLength = 512): Float32Array {
    return addon.zeroCrossingRate(this.getData(), this.getSampleRate(), frameLength, hopLength);
  }

  rmsEnergy(frameLength = 2048, hopLength = 512): Float32Array {
    return addon.rmsEnergy(this.getData(), this.getSampleRate(), frameLength, hopLength);
  }

  pitchYin(
    frameLength = 2048,
    hopLength = 512,
    fmin = 65.0,
    fmax = 2093.0,
    threshold = 0.3,
  ): PitchResult {
    return addon.pitchYin(
      this.getData(),
      this.getSampleRate(),
      frameLength,
      hopLength,
      fmin,
      fmax,
      threshold,
    );
  }

  pitchPyin(
    frameLength = 2048,
    hopLength = 512,
    fmin = 65.0,
    fmax = 2093.0,
    threshold = 0.3,
  ): PitchResult {
    return addon.pitchPyin(
      this.getData(),
      this.getSampleRate(),
      frameLength,
      hopLength,
      fmin,
      fmax,
      threshold,
    );
  }

  resample(targetSr: number): Float32Array {
    return addon.resample(this.getData(), this.getSampleRate(), targetSr);
  }
}

// ============================================================================
// Standalone functions
// ============================================================================

// -- Analysis --

export function detectBpm(samples: Float32Array, sampleRate = 22050): number {
  return addon.detectBpm(samples, sampleRate);
}

export function detectKey(samples: Float32Array, sampleRate = 22050): Key {
  return addon.detectKey(samples, sampleRate);
}

export function detectBeats(samples: Float32Array, sampleRate = 22050): Float32Array {
  return addon.detectBeats(samples, sampleRate);
}

export function detectOnsets(samples: Float32Array, sampleRate = 22050): Float32Array {
  return addon.detectOnsets(samples, sampleRate);
}

export function analyze(samples: Float32Array, sampleRate = 22050): AnalysisResult {
  return addon.analyze(samples, sampleRate);
}

export function version(): string {
  return addon.version();
}

// -- Effects --

export function hpss(
  samples: Float32Array,
  sampleRate = 22050,
  kernelHarmonic = 31,
  kernelPercussive = 31,
): HpssResult {
  return addon.hpss(samples, sampleRate, kernelHarmonic, kernelPercussive);
}

export function harmonic(samples: Float32Array, sampleRate = 22050): Float32Array {
  return addon.harmonic(samples, sampleRate);
}

export function percussive(samples: Float32Array, sampleRate = 22050): Float32Array {
  return addon.percussive(samples, sampleRate);
}

export function timeStretch(samples: Float32Array, sampleRate = 22050, rate: number): Float32Array {
  return addon.timeStretch(samples, sampleRate, rate);
}

export function pitchShift(
  samples: Float32Array,
  sampleRate = 22050,
  semitones: number,
): Float32Array {
  return addon.pitchShift(samples, sampleRate, semitones);
}

export function normalize(samples: Float32Array, sampleRate = 22050, targetDb = 0.0): Float32Array {
  return addon.normalize(samples, sampleRate, targetDb);
}

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
): MelSpectrogramResult {
  return addon.melSpectrogram(samples, sampleRate, nFft, hopLength, nMels);
}

export function mfcc(
  samples: Float32Array,
  sampleRate = 22050,
  nFft = 2048,
  hopLength = 512,
  nMels = 128,
  nMfcc = 13,
): MfccResult {
  return addon.mfcc(samples, sampleRate, nFft, hopLength, nMels, nMfcc);
}

export function chroma(
  samples: Float32Array,
  sampleRate = 22050,
  nFft = 2048,
  hopLength = 512,
): ChromaResult {
  return addon.chroma(samples, sampleRate, nFft, hopLength);
}

export function spectralCentroid(
  samples: Float32Array,
  sampleRate = 22050,
  nFft = 2048,
  hopLength = 512,
): Float32Array {
  return addon.spectralCentroid(samples, sampleRate, nFft, hopLength);
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
): PitchResult {
  return addon.pitchYin(samples, sampleRate, frameLength, hopLength, fmin, fmax, threshold);
}

export function pitchPyin(
  samples: Float32Array,
  sampleRate = 22050,
  frameLength = 2048,
  hopLength = 512,
  fmin = 65.0,
  fmax = 2093.0,
  threshold = 0.3,
): PitchResult {
  return addon.pitchPyin(samples, sampleRate, frameLength, hopLength, fmin, fmax, threshold);
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

export function framesToTime(frames: number, sr: number, hopLength: number): number {
  return addon.framesToTime(frames, sr, hopLength);
}

export function timeToFrames(time: number, sr: number, hopLength: number): number {
  return addon.timeToFrames(time, sr, hopLength);
}

export function resample(samples: Float32Array, srcSr: number, targetSr: number): Float32Array {
  return addon.resample(samples, srcSr, targetSr);
}

export type {
  AnalysisResult,
  ChromaResult,
  HpssResult,
  Key,
  MelSpectrogramResult,
  MfccResult,
  PitchResult,
  StftDbResult,
  StftResult,
  TimeSignature,
} from './types.js';
