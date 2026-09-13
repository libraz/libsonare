import type { MusicAnalyzeOptions } from './analysis.js';
import {
  analyzeBpm as analyzeBpmFn,
  analyzeDynamics as analyzeDynamicsFn,
  analyzeImpulseResponse as analyzeImpulseResponseFn,
  analyzeRhythm as analyzeRhythmFn,
  analyzeTimbre as analyzeTimbreFn,
  chordFunctionalAnalysis as chordFunctionalAnalysisFn,
  detectAcoustic as detectAcousticFn,
  detectChords as detectChordsFn,
} from './analysis.js';
import type { VoiceChangeOptions } from './effects_mastering.js';
import {
  harmonic as harmonicFn,
  hpss as hpssFn,
  masterAudio as masterAudioFn,
  masteringChain as masteringChainFn,
  mastering as masteringFn,
  masteringProcess as masteringProcessFn,
  normalize as normalizeFn,
  noteMove as noteMoveFn,
  noteStretch as noteStretchFn,
  percussive as percussiveFn,
  pitchCorrectToMidi as pitchCorrectToMidiFn,
  pitchShift as pitchShiftFn,
  timeStretch as timeStretchFn,
  voiceChange as voiceChangeFn,
} from './effects_mastering.js';
import {
  chroma as chromaFn,
  lufs as lufsFn,
  melSpectrogram as melSpectrogramFn,
  mfcc as mfccFn,
  momentaryLufs as momentaryLufsFn,
  nnlsChroma as nnlsChromaFn,
  onsetEnvelope as onsetEnvelopeFn,
  pitchPyin as pitchPyinFn,
  pitchYin as pitchYinFn,
  rmsEnergy as rmsEnergyFn,
  shortTermLufs as shortTermLufsFn,
  spectralBandwidth as spectralBandwidthFn,
  spectralCentroid as spectralCentroidFn,
  spectralFlatness as spectralFlatnessFn,
  spectralRolloff as spectralRolloffFn,
  stftDb as stftDbFn,
  stft as stftFn,
  trim as trimFn,
  zeroCrossingRate as zeroCrossingRateFn,
} from './features.js';
import { resample as resampleFn } from './mixer.js';
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

  // The seven methods below stay on the native handle rather than delegating to
  // their standalone facade counterparts: the handle reads its own buffer in
  // native memory, so routing through data() would copy it out first. Nothing is
  // given up by not delegating — those facades are plain forwarders with no
  // guard of their own, and none of these methods takes an integer positionally.

  detectBpm(): number {
    this.requireAlive();
    return this.native.detectBpm();
  }

  detectKey(options: KeyDetectionOptions = {}): Key {
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
    return analyzeImpulseResponseFn(this.data(), this.getSampleRate(), nOctaveBands);
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
    return hpssFn(this.data(), this.getSampleRate(), kernelHarmonic, kernelPercussive);
  }

  harmonic(): Float32Array {
    return harmonicFn(this.data(), this.getSampleRate());
  }

  percussive(): Float32Array {
    return percussiveFn(this.data(), this.getSampleRate());
  }

  timeStretch(rate: number): Float32Array {
    return timeStretchFn(this.data(), this.getSampleRate(), rate);
  }

  pitchShift(semitones: number): Float32Array {
    return pitchShiftFn(this.data(), this.getSampleRate(), semitones);
  }

  pitchCorrectToMidi(currentMidi = 69.0, targetMidi = 69.0): Float32Array {
    return pitchCorrectToMidiFn(this.data(), this.getSampleRate(), currentMidi, targetMidi);
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
    return normalizeFn(this.data(), this.getSampleRate(), targetDb);
  }

  mastering(options: MasteringOptions = {}): MasteringResult {
    return masteringFn(this.data(), this.getSampleRate(), options);
  }

  masteringProcess(
    processorName: SoloProcessor,
    params: Record<string, number | boolean> = {},
  ): MasteringResult {
    return masteringProcessFn(processorName, this.data(), this.getSampleRate(), params);
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
    return trimFn(this.data(), this.getSampleRate(), thresholdDb);
  }

  // -- Features --

  stft(nFft = 2048, hopLength = 512): StftResult {
    return stftFn(this.data(), this.getSampleRate(), nFft, hopLength);
  }

  stftDb(nFft = 2048, hopLength = 512): StftDbResult {
    return stftDbFn(this.data(), this.getSampleRate(), nFft, hopLength);
  }

  melSpectrogram(
    nFft = 2048,
    hopLength = 512,
    nMels = 128,
    fmin = 0,
    fmax = 0,
    htk = false,
  ): MelSpectrogramResult {
    return melSpectrogramFn(
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
    return mfccFn(
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
    return chromaFn(this.data(), this.getSampleRate(), nFft, hopLength);
  }

  spectralCentroid(nFft = 2048, hopLength = 512): Float32Array {
    return spectralCentroidFn(this.data(), this.getSampleRate(), nFft, hopLength);
  }

  spectralBandwidth(nFft = 2048, hopLength = 512): Float32Array {
    return spectralBandwidthFn(this.data(), this.getSampleRate(), nFft, hopLength);
  }

  spectralRolloff(nFft = 2048, hopLength = 512, rollPercent = 0.85): Float32Array {
    return spectralRolloffFn(this.data(), this.getSampleRate(), nFft, hopLength, rollPercent);
  }

  spectralFlatness(nFft = 2048, hopLength = 512): Float32Array {
    return spectralFlatnessFn(this.data(), this.getSampleRate(), nFft, hopLength);
  }

  zeroCrossingRate(frameLength = 2048, hopLength = 512): Float32Array {
    return zeroCrossingRateFn(this.data(), this.getSampleRate(), frameLength, hopLength);
  }

  rmsEnergy(frameLength = 2048, hopLength = 512): Float32Array {
    return rmsEnergyFn(this.data(), this.getSampleRate(), frameLength, hopLength);
  }

  pitchYin(
    frameLength = 2048,
    hopLength = 512,
    fmin = 65.0,
    fmax = 2093.0,
    threshold = 0.1,
    fillNa = false,
  ): PitchResult {
    return pitchYinFn(
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
    return pitchPyinFn(
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
    return resampleFn(this.data(), this.getSampleRate(), targetSr);
  }

  onsetEnvelope(nFft = 2048, hopLength = 512, nMels = 128): Float32Array {
    return onsetEnvelopeFn(this.data(), this.getSampleRate(), nFft, hopLength, nMels);
  }

  nnlsChroma(): { nChroma: number; nFrames: number; data: Float32Array } {
    return nnlsChromaFn(this.data(), this.getSampleRate());
  }

  lufs(options: ValidateOptions = {}): LufsResult {
    return lufsFn(this.data(), this.getSampleRate(), options);
  }

  momentaryLufs(options: ValidateOptions = {}): Float32Array {
    return momentaryLufsFn(this.data(), this.getSampleRate(), options);
  }

  shortTermLufs(options: ValidateOptions = {}): Float32Array {
    return shortTermLufsFn(this.data(), this.getSampleRate(), options);
  }
}
