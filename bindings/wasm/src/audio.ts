import type { VoiceChangeOptions } from './effects_mastering';
import {
  harmonic,
  hpss,
  masterAudio,
  mastering,
  masteringChain,
  masteringProcess,
  normalize,
  noteStretch,
  percussive,
  pitchCorrectToMidi,
  pitchShift,
  timeStretch,
  voiceChange,
} from './effects_mastering';
import {
  chroma,
  lufs,
  melSpectrogram,
  mfcc,
  momentaryLufs,
  nnlsChroma,
  onsetEnvelope,
  pitchPyin,
  pitchYin,
  resample,
  rmsEnergy,
  shortTermLufs,
  spectralBandwidth,
  spectralCentroid,
  spectralFlatness,
  spectralRolloff,
  stft,
  stftDb,
  trim,
  zeroCrossingRate,
} from './features';
import { getSonareModule } from './module_state';
import type {
  AnalysisResult,
  ChordAnalysisResult,
  ChordDetectionOptions,
  ChromaResult,
  HpssResult,
  Key,
  KeyCandidate,
  KeyDetectionOptions,
  LufsResult,
  MasteringChainConfig,
  MasteringChainResult,
  MasteringOptions,
  MasteringPreset,
  MasteringProcessorParams,
  MasteringResult,
  MelSpectrogramResult,
  MfccResult,
  Mode,
  NoteStretchOptions,
  PitchClass,
  PitchResult,
  SoloProcessor,
  StftResult,
} from './public_types';
import {
  analyze,
  analyzeWithProgress,
  chordFunctionalAnalysis,
  detectBeats,
  detectBpm,
  detectChords,
  detectDownbeats,
  detectKey,
  detectKeyCandidates,
  detectOnsets,
} from './quick_analysis';
import type { ProgressCallback, WasmNnlsChromaResult } from './sonare.js';

// ============================================================================
// Audio Class
// ============================================================================

type BrowserDecodeContext = Pick<BaseAudioContext, 'decodeAudioData' | 'sampleRate'>;

export interface BrowserAudioDecodeOptions {
  /**
   * AudioContext/OfflineAudioContext used for browser codec fallback. Its
   * `sampleRate` becomes the returned Audio sample rate.
   */
  audioContext?: BrowserDecodeContext;
  /**
   * Factory used when `audioContext` is omitted. `targetSampleRate` is passed
   * through so browsers that honor AudioContextOptions decode directly at that
   * rate.
   */
  createAudioContext?: (options?: AudioContextOptions) => BrowserDecodeContext;
  /**
   * Requested fallback decode rate when this helper creates the context. If the
   * browser ignores it or a context is supplied, no extra resampling is applied.
   */
  targetSampleRate?: number;
}

function encodedBytesToArrayBuffer(bytes: Uint8Array): ArrayBuffer {
  const copy = new Uint8Array(bytes.byteLength);
  copy.set(bytes);
  return copy.buffer;
}

function getBrowserAudioContextFactory():
  | ((options?: AudioContextOptions) => BrowserDecodeContext)
  | undefined {
  const root = globalThis as typeof globalThis & {
    AudioContext?: new (options?: AudioContextOptions) => BaseAudioContext;
    webkitAudioContext?: new (options?: AudioContextOptions) => BaseAudioContext;
  };
  const Ctor = root.AudioContext ?? root.webkitAudioContext;
  return Ctor ? (options?: AudioContextOptions) => new Ctor(options) : undefined;
}

function audioBufferToMono(buffer: AudioBuffer): Float32Array {
  const samples = new Float32Array(buffer.length);
  if (buffer.numberOfChannels <= 0) {
    return samples;
  }
  if (buffer.numberOfChannels === 1) {
    samples.set(buffer.getChannelData(0));
    return samples;
  }
  for (let channel = 0; channel < buffer.numberOfChannels; channel++) {
    const data = buffer.getChannelData(channel);
    for (let i = 0; i < buffer.length; i++) {
      samples[i] += data[i] / buffer.numberOfChannels;
    }
  }
  return samples;
}

async function closeCreatedContext(context: BrowserDecodeContext): Promise<void> {
  const maybeClosable = context as BrowserDecodeContext & { close?: () => Promise<void> };
  if (maybeClosable.close) {
    await maybeClosable.close();
  }
}

/**
 * Wrapper around audio data that exposes all analysis and feature functions as instance methods.
 *
 * @example
 * ```typescript
 * import { init, Audio } from '@libraz/libsonare';
 *
 * await init();
 *
 * const audio = Audio.fromBuffer(samples, 44100);
 * console.log('BPM:', audio.detectBpm());
 * console.log('Key:', audio.detectKey().name);
 *
 * const mel = audio.melSpectrogram();
 * ```
 */
export class Audio {
  private _samples: Float32Array;
  private _sampleRate: number;

  private constructor(samples: Float32Array, sampleRate: number) {
    this._samples = samples;
    this._sampleRate = sampleRate;
  }

  /**
   * Create an Audio instance from raw sample data.
   *
   * @param samples - Mono float samples.
   * @param sampleRate - Sample rate in Hz (default `48000`, matching the
   *   Node/Python surfaces).
   */
  static fromBuffer(samples: Float32Array, sampleRate = 48000): Audio {
    return new Audio(samples, sampleRate);
  }

  /**
   * Create an Audio instance by decoding audio bytes in memory.
   *
   * @param bytes - Encoded audio bytes such as WAV or MP3.
   */
  static fromMemory(bytes: Uint8Array): Audio {
    const decoded = getSonareModule().audioFromMemory(bytes);
    return new Audio(decoded.samples, decoded.sampleRate);
  }

  /**
   * Decode audio bytes with the native WASM decoder first, then fall back to the
   * browser codec stack (`AudioContext.decodeAudioData`) for formats such as
   * AAC, OGG, and FLAC when available. Browser-decoded multi-channel audio is
   * mixed down to mono to match the `Audio` wrapper contract.
   */
  static async fromMemoryWithBrowserFallback(
    bytes: Uint8Array,
    options: BrowserAudioDecodeOptions = {},
  ): Promise<Audio> {
    try {
      return Audio.fromMemory(bytes);
    } catch (nativeError) {
      let createdContext = false;
      const contextFactory = options.createAudioContext ?? getBrowserAudioContextFactory();
      const context =
        options.audioContext ??
        contextFactory?.(
          options.targetSampleRate ? { sampleRate: options.targetSampleRate } : undefined,
        );

      if (!context) {
        throw new Error(
          `Audio.fromMemory failed and browser decodeAudioData is unavailable: ${
            nativeError instanceof Error ? nativeError.message : String(nativeError)
          }`,
        );
      }

      createdContext = !options.audioContext;
      try {
        const decoded = await context.decodeAudioData(encodedBytesToArrayBuffer(bytes));
        return new Audio(audioBufferToMono(decoded), decoded.sampleRate || context.sampleRate);
      } catch (fallbackError) {
        throw new Error(
          `Audio.fromMemory failed and browser decodeAudioData fallback failed: ${
            fallbackError instanceof Error ? fallbackError.message : String(fallbackError)
          }`,
        );
      } finally {
        if (createdContext) {
          await closeCreatedContext(context);
        }
      }
    }
  }

  /** The raw audio samples. */
  get data(): Float32Array {
    return this._samples;
  }

  /** Number of samples. */
  get length(): number {
    return this._samples.length;
  }

  /** Sample rate in Hz. */
  get sampleRate(): number {
    return this._sampleRate;
  }

  /** Duration in seconds. */
  get duration(): number {
    return this._samples.length / this._sampleRate;
  }

  // -- Analysis --

  detectBpm(): number {
    return detectBpm(this._samples, this._sampleRate);
  }

  detectKey(options: KeyDetectionOptions = {}): Key {
    return detectKey(this._samples, this._sampleRate, options);
  }

  detectKeyCandidates(options: KeyDetectionOptions = {}): KeyCandidate[] {
    return detectKeyCandidates(this._samples, this._sampleRate, options);
  }

  detectOnsets(): Float32Array {
    return detectOnsets(this._samples, this._sampleRate);
  }

  detectBeats(): Float32Array {
    return detectBeats(this._samples, this._sampleRate);
  }

  detectDownbeats(): Float32Array {
    return detectDownbeats(this._samples, this._sampleRate);
  }

  detectChords(options: ChordDetectionOptions = {}): ChordAnalysisResult {
    return detectChords(this._samples, this._sampleRate, options);
  }

  chordFunctionalAnalysis(
    keyRoot: PitchClass,
    keyMode: Mode,
    options: ChordDetectionOptions = {},
  ): string[] {
    return chordFunctionalAnalysis(this._samples, keyRoot, keyMode, this._sampleRate, options);
  }

  analyze(): AnalysisResult {
    return analyze(this._samples, this._sampleRate);
  }

  analyzeWithProgress(onProgress: ProgressCallback): AnalysisResult {
    return analyzeWithProgress(this._samples, this._sampleRate, onProgress);
  }

  // -- Effects --

  hpss(kernelHarmonic = 31, kernelPercussive = 31): HpssResult {
    return hpss(this._samples, this._sampleRate, kernelHarmonic, kernelPercussive);
  }

  harmonic(): Float32Array {
    return harmonic(this._samples, this._sampleRate);
  }

  percussive(): Float32Array {
    return percussive(this._samples, this._sampleRate);
  }

  timeStretch(rate: number): Float32Array {
    return timeStretch(this._samples, this._sampleRate, rate);
  }

  pitchShift(semitones: number): Float32Array {
    return pitchShift(this._samples, this._sampleRate, semitones);
  }

  pitchCorrectToMidi(currentMidi = 69.0, targetMidi = 69.0): Float32Array {
    return pitchCorrectToMidi(this._samples, this._sampleRate, currentMidi, targetMidi);
  }

  noteStretch(options: NoteStretchOptions = {}): Float32Array {
    return noteStretch(this._samples, this._sampleRate, options);
  }

  voiceChange(options: VoiceChangeOptions = {}): Float32Array {
    return voiceChange(this._samples, this._sampleRate, options);
  }

  normalize(targetDb = 0.0): Float32Array {
    return normalize(this._samples, this._sampleRate, targetDb);
  }

  mastering(options: MasteringOptions = {}): MasteringResult {
    return mastering(this._samples, this._sampleRate, options);
  }

  masteringChain(config: MasteringChainConfig): MasteringChainResult {
    return masteringChain(this._samples, this._sampleRate, config);
  }

  masterAudio(
    presetName: MasteringPreset = 'pop',
    overrides: Record<string, number | boolean> | null = null,
  ): MasteringChainResult {
    return masterAudio(this._samples, this._sampleRate, presetName, overrides ?? {});
  }

  masteringProcess(
    processorName: SoloProcessor,
    params: MasteringProcessorParams = {},
  ): MasteringResult {
    return masteringProcess(processorName, this._samples, this._sampleRate, params);
  }

  trim(thresholdDb = -60.0): Float32Array {
    return trim(this._samples, this._sampleRate, thresholdDb);
  }

  // -- Features --

  stft(nFft = 2048, hopLength = 512): StftResult {
    return stft(this._samples, this._sampleRate, nFft, hopLength);
  }

  stftDb(nFft = 2048, hopLength = 512): { nBins: number; nFrames: number; db: Float32Array } {
    return stftDb(this._samples, this._sampleRate, nFft, hopLength);
  }

  melSpectrogram(
    nFft = 2048,
    hopLength = 512,
    nMels = 128,
    fmin = 0,
    fmax = 0,
    htk = false,
  ): MelSpectrogramResult {
    return melSpectrogram(this._samples, this._sampleRate, nFft, hopLength, nMels, fmin, fmax, htk);
  }

  mfcc(
    nFft = 2048,
    hopLength = 512,
    nMels = 128,
    nMfcc = 20,
    fmin = 0,
    fmax = 0,
    htk = false,
  ): MfccResult {
    return mfcc(this._samples, this._sampleRate, nFft, hopLength, nMels, nMfcc, fmin, fmax, htk);
  }

  chroma(nFft = 2048, hopLength = 512): ChromaResult {
    return chroma(this._samples, this._sampleRate, nFft, hopLength);
  }

  nnlsChroma(): WasmNnlsChromaResult {
    return nnlsChroma(this._samples, this._sampleRate);
  }

  onsetEnvelope(nFft = 2048, hopLength = 512, nMels = 128): Float32Array {
    return onsetEnvelope(this._samples, this._sampleRate, nFft, hopLength, nMels);
  }

  lufs(): LufsResult {
    return lufs(this._samples, this._sampleRate);
  }

  momentaryLufs(): Float32Array {
    return momentaryLufs(this._samples, this._sampleRate);
  }

  shortTermLufs(): Float32Array {
    return shortTermLufs(this._samples, this._sampleRate);
  }

  spectralCentroid(nFft = 2048, hopLength = 512): Float32Array {
    return spectralCentroid(this._samples, this._sampleRate, nFft, hopLength);
  }

  spectralBandwidth(nFft = 2048, hopLength = 512): Float32Array {
    return spectralBandwidth(this._samples, this._sampleRate, nFft, hopLength);
  }

  spectralRolloff(nFft = 2048, hopLength = 512, rollPercent = 0.85): Float32Array {
    return spectralRolloff(this._samples, this._sampleRate, nFft, hopLength, rollPercent);
  }

  spectralFlatness(nFft = 2048, hopLength = 512): Float32Array {
    return spectralFlatness(this._samples, this._sampleRate, nFft, hopLength);
  }

  zeroCrossingRate(frameLength = 2048, hopLength = 512): Float32Array {
    return zeroCrossingRate(this._samples, this._sampleRate, frameLength, hopLength);
  }

  rmsEnergy(frameLength = 2048, hopLength = 512): Float32Array {
    return rmsEnergy(this._samples, this._sampleRate, frameLength, hopLength);
  }

  pitchYin(
    frameLength = 2048,
    hopLength = 512,
    fmin = 65.0,
    fmax = 2093.0,
    threshold = 0.3,
    fillNa = false,
  ): PitchResult {
    return pitchYin(
      this._samples,
      this._sampleRate,
      frameLength,
      hopLength,
      fmin,
      fmax,
      threshold,
      fillNa,
    );
  }

  pitchPyin(
    frameLength = 2048,
    hopLength = 512,
    fmin = 65.0,
    fmax = 2093.0,
    threshold = 0.3,
    fillNa = false,
  ): PitchResult {
    return pitchPyin(
      this._samples,
      this._sampleRate,
      frameLength,
      hopLength,
      fmin,
      fmax,
      threshold,
      fillNa,
    );
  }

  resample(targetSr: number): Float32Array {
    return resample(this._samples, this._sampleRate, targetSr);
  }
}
