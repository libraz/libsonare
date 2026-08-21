import type { MusicAnalyzeOptions } from './analysis.js';
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

  static fileChannelCount(path: string): number {
    return addon.Audio.fileChannelCount(path);
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

  private requireAlive(): void {
    if (this.disposed) {
      throw new Error('Audio has been destroyed');
    }
  }

  /**
   * The cached decoded buffer, without copying.
   *
   * Every facade method below reads through here. The native calls they feed it
   * to treat samples as read-only, so a defensive copy per call bought nothing
   * and cost the whole buffer: a five-minute mono file is 57.6 MB, and
   * `analyzeBpm()` then `masterAudio()` then `mfcc()` allocated and discarded
   * that three times over. Only {@link getData}, which hands the buffer to a
   * caller who may mutate it, copies.
   */
  private data(): Float32Array {
    this.requireAlive();
    const cached = this.dataCache;
    if (cached !== undefined) {
      return cached;
    }
    const data = this.native.getData();
    this.dataCache = data;
    return data;
  }

  getData(): Float32Array {
    // Keep an immutable internal snapshot for facade operations, while callers
    // receive their own mutable copy. Otherwise a natural in-place edit to the
    // returned Float32Array silently changes every later facade calculation.
    return this.data().slice();
  }

  getLength(): number {
    this.requireAlive();
    return this.native.getLength();
  }

  getSampleRate(): number {
    this.requireAlive();
    return this.native.getSampleRate();
  }

  getDuration(): number {
    this.requireAlive();
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
    this.requireAlive();
    return this.native.detectBpm();
  }

  detectKey(options: KeyDetectionOptions = {}): Key {
    // Native instance method reads the handle's buffer directly (same options
    // and result shape as the standalone addon.detectKey); routing through
    // getData() would copy the whole buffer out of native memory first.
    this.requireAlive();
    return this.native.detectKey(options);
  }

  detectKeyCandidates(options: KeyDetectionOptions = {}): KeyCandidate[] {
    this.requireAlive();
    return this.native.detectKeyCandidates(options);
  }

  detectBeats(): Float32Array {
    this.requireAlive();
    return this.native.detectBeats();
  }

  detectDownbeats(): Float32Array {
    this.requireAlive();
    return this.native.detectDownbeats();
  }

  detectOnsets(): Float32Array {
    this.requireAlive();
    return this.native.detectOnsets();
  }

  analyze(options: MusicAnalyzeOptions = {}): AnalysisResult {
    this.requireAlive();
    return this.native.analyze(options);
  }

  analyzeBpm(options: AnalyzeBpmOptions = {}): BpmAnalysisResult {
    return analyzeBpmFn(this.data(), this.getSampleRate(), options);
  }

  analyzeImpulseResponse(nOctaveBands = 6): AcousticResult {
    return addon.analyzeImpulseResponse(this.data(), this.getSampleRate(), nOctaveBands);
  }

  detectAcoustic(options: AcousticOptions = {}): AcousticResult {
    return detectAcousticFn(this.data(), this.getSampleRate(), options);
  }

  analyzeRhythm(options: AnalyzeRhythmOptions = {}): RhythmResult {
    return analyzeRhythmFn(this.data(), this.getSampleRate(), options);
  }

  analyzeDynamics(options: AnalyzeDynamicsOptions = {}): DynamicsResult {
    return analyzeDynamicsFn(this.data(), this.getSampleRate(), options);
  }

  analyzeTimbre(options: AnalyzeTimbreOptions = {}): TimbreResult {
    return analyzeTimbreFn(this.data(), this.getSampleRate(), options);
  }

  detectChords(options: ChordDetectionOptions = {}): ChordAnalysisResult {
    return detectChordsFn(this.data(), this.getSampleRate(), options);
  }

  chordFunctionalAnalysis(
    keyRoot: number,
    keyMode = 0,
    options: ChordDetectionOptions = {},
  ): string[] {
    return chordFunctionalAnalysisFn(this.data(), keyRoot, keyMode, this.getSampleRate(), options);
  }

  // -- Effects --

  hpss(kernelHarmonic = 31, kernelPercussive = 31): HpssResult {
    return addon.hpss(this.data(), this.getSampleRate(), kernelHarmonic, kernelPercussive);
  }

  harmonic(): Float32Array {
    return addon.harmonic(this.data(), this.getSampleRate());
  }

  percussive(): Float32Array {
    return addon.percussive(this.data(), this.getSampleRate());
  }

  timeStretch(rate: number): Float32Array {
    return addon.timeStretch(this.data(), this.getSampleRate(), rate);
  }

  pitchShift(semitones: number): Float32Array {
    return addon.pitchShift(this.data(), this.getSampleRate(), semitones);
  }

  pitchCorrectToMidi(currentMidi = 69.0, targetMidi = 69.0): Float32Array {
    return addon.pitchCorrectToMidi(this.data(), this.getSampleRate(), currentMidi, targetMidi);
  }

  noteStretch(options: NoteStretchOptions = {}): Float32Array {
    return noteStretchFn(this.data(), this.getSampleRate(), options);
  }

  noteMove(options: import('./types_mastering.js').NoteMoveOptions = {}): Float32Array {
    return noteMoveFn(this.data(), this.getSampleRate(), options);
  }

  voiceChange(options: VoiceChangeOptions = {}): Float32Array {
    return voiceChangeFn(this.data(), this.getSampleRate(), options);
  }

  normalize(targetDb = 0.0): Float32Array {
    return addon.normalize(this.data(), this.getSampleRate(), targetDb);
  }

  mastering(options: MasteringOptions = {}): MasteringResult {
    return masteringFn(this.data(), this.getSampleRate(), options);
  }

  masteringProcess(
    processorName: SoloProcessor,
    params: Record<string, number | boolean> = {},
  ): MasteringResult {
    return addon.masteringProcess(processorName, this.data(), this.getSampleRate(), params);
  }

  masteringChain(
    config: MasteringChainConfig = {},
    onProgress?: ProgressCallback,
  ): MasteringChainResult {
    return masteringChainFn(this.data(), this.getSampleRate(), config, onProgress);
  }

  masterAudio(
    preset: MasteringPreset = 'pop',
    overrides: MasteringChainConfig = {},
    onProgress?: ProgressCallback,
  ): MasteringChainResult {
    return masterAudioFn(this.data(), this.getSampleRate(), preset, overrides, onProgress);
  }

  trim(thresholdDb = -60.0): Float32Array {
    return addon.trim(this.data(), this.getSampleRate(), thresholdDb);
  }

  // -- Features --

  stft(nFft = 2048, hopLength = 512): StftResult {
    return addon.stft(this.data(), this.getSampleRate(), nFft, hopLength);
  }

  stftDb(nFft = 2048, hopLength = 512): StftDbResult {
    return addon.stftDb(this.data(), this.getSampleRate(), nFft, hopLength);
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
      this.data(),
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
      this.data(),
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
    return addon.chroma(this.data(), this.getSampleRate(), nFft, hopLength);
  }

  spectralCentroid(nFft = 2048, hopLength = 512): Float32Array {
    return addon.spectralCentroid(this.data(), this.getSampleRate(), nFft, hopLength);
  }

  spectralBandwidth(nFft = 2048, hopLength = 512): Float32Array {
    return addon.spectralBandwidth(this.data(), this.getSampleRate(), nFft, hopLength);
  }

  spectralRolloff(nFft = 2048, hopLength = 512, rollPercent = 0.85): Float32Array {
    return addon.spectralRolloff(this.data(), this.getSampleRate(), nFft, hopLength, rollPercent);
  }

  spectralFlatness(nFft = 2048, hopLength = 512): Float32Array {
    return addon.spectralFlatness(this.data(), this.getSampleRate(), nFft, hopLength);
  }

  zeroCrossingRate(frameLength = 2048, hopLength = 512): Float32Array {
    return addon.zeroCrossingRate(this.data(), this.getSampleRate(), frameLength, hopLength);
  }

  rmsEnergy(frameLength = 2048, hopLength = 512): Float32Array {
    return addon.rmsEnergy(this.data(), this.getSampleRate(), frameLength, hopLength);
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
      this.data(),
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
      this.data(),
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
    return addon.resample(this.data(), this.getSampleRate(), targetSr);
  }

  onsetEnvelope(nFft = 2048, hopLength = 512, nMels = 128): Float32Array {
    return addon.onsetEnvelope(this.data(), this.getSampleRate(), nFft, hopLength, nMels);
  }

  nnlsChroma(): { nChroma: number; nFrames: number; data: Float32Array } {
    return addon.nnlsChroma(this.data(), this.getSampleRate());
  }

  lufs(options: ValidateOptions = {}): LufsResult {
    const data = this.data();
    assertSamples('lufs', data, options.validate !== false);
    return addon.lufs(data, this.getSampleRate());
  }

  momentaryLufs(options: ValidateOptions = {}): Float32Array {
    const data = this.data();
    assertSamples('momentaryLufs', data, options.validate !== false);
    return addon.momentaryLufs(data, this.getSampleRate());
  }

  shortTermLufs(options: ValidateOptions = {}): Float32Array {
    const data = this.data();
    assertSamples('shortTermLufs', data, options.validate !== false);
    return addon.shortTermLufs(data, this.getSampleRate());
  }
}
