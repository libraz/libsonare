import {
  analyzeBpm as analyzeBpmFn,
  analyzeDynamics as analyzeDynamicsFn,
  analyzeRhythm as analyzeRhythmFn,
  analyzeTimbre as analyzeTimbreFn,
  chordFunctionalAnalysis as chordFunctionalAnalysisFn,
  detectAcoustic as detectAcousticFn,
  detectChords as detectChordsFn,
} from './analysis.js';
import type { VoiceChangeOptions } from './effects_mastering.js';
import {
  masterAudio as masterAudioFn,
  masteringChain as masteringChainFn,
  mastering as masteringFn,
  noteMove as noteMoveFn,
  noteStretch as noteStretchFn,
  voiceChange as voiceChangeFn,
} from './effects_mastering.js';
import { addon } from './native.js';
import type {
  AcousticOptions,
  AcousticResult,
  AnalysisResult,
  AnalyzeBpmOptions,
  AnalyzeDynamicsOptions,
  AnalyzeRhythmOptions,
  AnalyzeTimbreOptions,
  BpmAnalysisResult,
  ChordAnalysisResult,
  ChordDetectionOptions,
  ChromaResult,
  DynamicsResult,
  HpssResult,
  Key,
  KeyCandidate,
  KeyDetectionOptions,
  LufsResult,
  MasteringChainConfig,
  MasteringChainResult,
  MasteringOptions,
  MasteringPreset,
  MasteringResult,
  MelSpectrogramResult,
  MfccResult,
  NoteStretchOptions,
  PitchResult,
  ProgressCallback,
  RhythmResult,
  SoloProcessor,
  StftDbResult,
  StftResult,
  TimbreResult,
} from './types.js';
import type { ValidateOptions } from './validation.js';
import { assertSamples } from './validation.js';

export class Audio {
  private native: InstanceType<typeof addon.Audio>;
  private disposed = false;
  private dataCache?: Float32Array;

  private constructor(native: InstanceType<typeof addon.Audio>) {
    this.native = native;
  }

  static fromFile(path: string): Audio {
    return new Audio(addon.Audio.fromFile(path));
  }

  /**
   * Wrap raw mono float samples as an {@link Audio}. `sampleRate` defaults to
   * `48000` (the project default) when omitted.
   */
  static fromBuffer(samples: Float32Array, sampleRate = 48000): Audio {
    return new Audio(addon.Audio.fromBuffer(samples, sampleRate));
  }

  static fromMemory(data: Buffer | Uint8Array): Audio {
    return new Audio(addon.Audio.fromMemory(data));
  }

  getData(): Float32Array {
    // The native Audio handle is immutable. Cache its one JS snapshot so the
    // feature/effect convenience methods below do not copy the full PCM buffer
    // across N-API on every call.
    const data = this.dataCache ?? this.native.getData();
    this.dataCache = data;
    return data;
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
    if (this.disposed) {
      return;
    }
    this.disposed = true;
    this.dataCache = undefined;
    this.native.destroy();
  }

  /** Releases the native handle; lets `using` (Node 22+) free it automatically. */
  [Symbol.dispose](): void {
    this.destroy();
  }

  // -- Analysis --

  detectBpm(): number {
    return this.native.detectBpm();
  }

  detectKey(options: KeyDetectionOptions = {}): Key {
    // Native instance method reads the handle's buffer directly (same options
    // and result shape as the standalone addon.detectKey); routing through
    // getData() would copy the whole buffer out of native memory first.
    return this.native.detectKey(options);
  }

  detectKeyCandidates(options: KeyDetectionOptions = {}): KeyCandidate[] {
    return this.native.detectKeyCandidates(options);
  }

  detectBeats(): Float32Array {
    return this.native.detectBeats();
  }

  detectDownbeats(): Float32Array {
    return this.native.detectDownbeats();
  }

  detectOnsets(): Float32Array {
    return this.native.detectOnsets();
  }

  analyze(): AnalysisResult {
    return this.native.analyze();
  }

  analyzeBpm(options: AnalyzeBpmOptions = {}): BpmAnalysisResult {
    return analyzeBpmFn(this.getData(), this.getSampleRate(), options);
  }

  analyzeImpulseResponse(nOctaveBands = 6): AcousticResult {
    return addon.analyzeImpulseResponse(this.getData(), this.getSampleRate(), nOctaveBands);
  }

  detectAcoustic(options: AcousticOptions = {}): AcousticResult {
    return detectAcousticFn(this.getData(), this.getSampleRate(), options);
  }

  analyzeRhythm(options: AnalyzeRhythmOptions = {}): RhythmResult {
    return analyzeRhythmFn(this.getData(), this.getSampleRate(), options);
  }

  analyzeDynamics(options: AnalyzeDynamicsOptions = {}): DynamicsResult {
    return analyzeDynamicsFn(this.getData(), this.getSampleRate(), options);
  }

  analyzeTimbre(options: AnalyzeTimbreOptions = {}): TimbreResult {
    return analyzeTimbreFn(this.getData(), this.getSampleRate(), options);
  }

  detectChords(options: ChordDetectionOptions = {}): ChordAnalysisResult {
    return detectChordsFn(this.getData(), this.getSampleRate(), options);
  }

  chordFunctionalAnalysis(
    keyRoot: number,
    keyMode = 0,
    options: ChordDetectionOptions = {},
  ): string[] {
    return chordFunctionalAnalysisFn(
      this.getData(),
      keyRoot,
      keyMode,
      this.getSampleRate(),
      options,
    );
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

  pitchCorrectToMidi(currentMidi = 69.0, targetMidi = 69.0): Float32Array {
    return addon.pitchCorrectToMidi(this.getData(), this.getSampleRate(), currentMidi, targetMidi);
  }

  noteStretch(options: NoteStretchOptions = {}): Float32Array {
    return noteStretchFn(this.getData(), this.getSampleRate(), options);
  }

  noteMove(options: import('./types_mastering.js').NoteMoveOptions = {}): Float32Array {
    return noteMoveFn(this.getData(), this.getSampleRate(), options);
  }

  voiceChange(options: VoiceChangeOptions = {}): Float32Array {
    return voiceChangeFn(this.getData(), this.getSampleRate(), options);
  }

  normalize(targetDb = 0.0): Float32Array {
    return addon.normalize(this.getData(), this.getSampleRate(), targetDb);
  }

  mastering(options: MasteringOptions = {}): MasteringResult {
    return masteringFn(this.getData(), this.getSampleRate(), options);
  }

  masteringProcess(
    processorName: SoloProcessor,
    params: Record<string, number | boolean> = {},
  ): MasteringResult {
    return addon.masteringProcess(processorName, this.getData(), this.getSampleRate(), params);
  }

  masteringChain(
    config: MasteringChainConfig = {},
    onProgress?: ProgressCallback,
  ): MasteringChainResult {
    return masteringChainFn(this.getData(), this.getSampleRate(), config, onProgress);
  }

  masterAudio(
    preset: MasteringPreset = 'pop',
    overrides: MasteringChainConfig = {},
    onProgress?: ProgressCallback,
  ): MasteringChainResult {
    return masterAudioFn(this.getData(), this.getSampleRate(), preset, overrides, onProgress);
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

  melSpectrogram(
    nFft = 2048,
    hopLength = 512,
    nMels = 128,
    fmin = 0,
    fmax = 0,
    htk = false,
  ): MelSpectrogramResult {
    return addon.melSpectrogram(
      this.getData(),
      this.getSampleRate(),
      nFft,
      hopLength,
      nMels,
      fmin,
      fmax,
      htk,
    );
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
    return addon.mfcc(
      this.getData(),
      this.getSampleRate(),
      nFft,
      hopLength,
      nMels,
      nMfcc,
      fmin,
      fmax,
      htk,
    );
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
    threshold = 0.1,
    fillNa = false,
  ): PitchResult {
    return addon.pitchYin(
      this.getData(),
      this.getSampleRate(),
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
    threshold = 0.1,
    fillNa = false,
  ): PitchResult {
    return addon.pitchPyin(
      this.getData(),
      this.getSampleRate(),
      frameLength,
      hopLength,
      fmin,
      fmax,
      threshold,
      fillNa,
    );
  }

  resample(targetSr: number): Float32Array {
    return addon.resample(this.getData(), this.getSampleRate(), targetSr);
  }

  onsetEnvelope(nFft = 2048, hopLength = 512, nMels = 128): Float32Array {
    return addon.onsetEnvelope(this.getData(), this.getSampleRate(), nFft, hopLength, nMels);
  }

  nnlsChroma(): { nChroma: number; nFrames: number; data: Float32Array } {
    return addon.nnlsChroma(this.getData(), this.getSampleRate());
  }

  lufs(options: ValidateOptions = {}): LufsResult {
    const data = this.getData();
    assertSamples('lufs', data, options.validate !== false);
    return addon.lufs(data, this.getSampleRate());
  }

  momentaryLufs(options: ValidateOptions = {}): Float32Array {
    const data = this.getData();
    assertSamples('momentaryLufs', data, options.validate !== false);
    return addon.momentaryLufs(data, this.getSampleRate());
  }

  shortTermLufs(options: ValidateOptions = {}): Float32Array {
    const data = this.getData();
    assertSamples('shortTermLufs', data, options.validate !== false);
    return addon.shortTermLufs(data, this.getSampleRate());
  }
}
