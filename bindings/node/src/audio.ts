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
import { ErrorCode, SonareError } from './errors.js';
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
import type {
  ClippingReport,
  DynamicRangeReport,
  MeteringDetectClippingOptions,
  MeteringDynamicRangeOptions,
  SpectrumOptions,
  SpectrumReport,
} from './metering.js';
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
  ChordFunctionsKey,
  ChromaResult,
  DetectKeyOptions,
  DynamicsResult,
  FunctionalChord,
  HpssResult,
  KeyCandidate,
  KeyDetection,
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
import { assertPositiveInteger } from './validation.js';

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
   * Load one channel of a file, leaving the others out of it.
   *
   * An {@link Audio} carries a single channel, so {@link fromFile} downmixes a
   * multi-channel source into it. This loads the requested channel instead,
   * which is what rendering a stereo result needs: pair it with
   * {@link fileChannelCount} and load each channel in turn.
   *
   * The file is decoded once per call, so a stereo load costs two decodes. The
   * format set and the decoded-buffer contract are {@link fromFile}'s.
   *
   * @param path - Source file path.
   * @param channelIndex - Zero-based channel to load.
   * @throws RangeError when
   *   `channelIndex` is negative or names no channel the file has, and the same
   *   load errors {@link fromFile} raises.
   *
   * @example
   * ```ts
   * const planes = Array.from({ length: Audio.fileChannelCount(path) }, (_, i) =>
   *   Audio.fromFileChannel(path, i),
   * );
   * ```
   */
  static fromFileChannel(path: string, channelIndex: number): Audio {
    return new Audio(addon.Audio.fromFileChannel(path, channelIndex));
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
      throw new SonareError(ErrorCode.InvalidState, 'Audio has been destroyed');
    }
  }

  /**
   * The decoded samples. The first read copies them out of the native handle
   * once; every later read returns that same array, which is shared with this
   * Audio's own methods and must not be mutated (copy it with `slice()` first).
   *
   * @throws SonareError `InvalidState` after {@link destroy}.
   */
  get data(): Float32Array {
    this.requireAlive();
    const cached = this.dataCache;
    if (cached !== undefined) {
      return cached;
    }
    const data = this.native.getData();
    this.dataCache = data;
    return data;
  }

  /**
   * Number of samples.
   *
   * @throws SonareError `InvalidState` after {@link destroy}.
   */
  get length(): number {
    this.requireAlive();
    return this.native.getLength();
  }

  /**
   * Sample rate in Hz.
   *
   * @throws SonareError `InvalidState` after {@link destroy}.
   */
  get sampleRate(): number {
    this.requireAlive();
    return this.native.getSampleRate();
  }

  /**
   * Duration in seconds.
   *
   * @throws SonareError `InvalidState` after {@link destroy}.
   */
  get durationSec(): number {
    this.requireAlive();
    return this.native.getDuration();
  }

  /** Releases the native handle; later reads throw `SonareError` `InvalidState`. A second call does nothing. */
  destroy(): void {
    if (this.disposed) {
      return;
    }
    this.disposed = true;
    this.dataCache = undefined;
    this.native.destroy();
  }

  /**
   * Same as {@link destroy}; lets `using` free the handle (needs TypeScript 5.2+
   * or a runtime with native explicit resource management).
   */
  [Symbol.dispose](): void {
    this.destroy();
  }

  // -- Analysis --

  // The methods below stay on the native handle rather than delegating to their
  // standalone facade counterparts: the handle reads its own buffer in native
  // memory, so routing through `data` would copy it out first. Nothing is given
  // up by not delegating — those facades are plain forwarders with no guard of
  // their own. The metering group below analyze() additionally skips the
  // per-call finiteness scan and defensive copy the standalone metering
  // facades pay, because audio_ passed both once at construction.

  detectBpm(): number {
    this.requireAlive();
    return this.native.detectBpm();
  }

  detectKey(options: DetectKeyOptions = {}): KeyDetection {
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

  // -- Metering --

  peakDb(): number {
    this.requireAlive();
    return this.native.peakDb();
  }

  rmsDb(): number {
    this.requireAlive();
    return this.native.rmsDb();
  }

  dcOffset(): number {
    this.requireAlive();
    return this.native.dcOffset();
  }

  crestFactorDb(): number {
    this.requireAlive();
    return this.native.crestFactorDb();
  }

  silenceRatio(thresholdDb = -45, frameLength = 1024, hopLength = 256): number {
    this.requireAlive();
    // Positivity only, as the free meteringSilenceRatio: the same C entry, which
    // requires both positive and bounds neither.
    assertPositiveInteger('silenceRatio', frameLength, 'frameLength');
    assertPositiveInteger('silenceRatio', hopLength, 'hopLength');
    return this.native.silenceRatio(thresholdDb, frameLength, hopLength);
  }

  /**
   * Inter-sample (true) peak in dBFS. `oversampleFactor` must be a power of two
   * in [1, 16]; pass 0 to use the library default (4).
   */
  truePeakDb(oversampleFactor = 4): number {
    this.requireAlive();
    return this.native.truePeakDb(oversampleFactor);
  }

  detectClipping(options: MeteringDetectClippingOptions = {}): ClippingReport {
    this.requireAlive();
    return this.native.detectClipping(options);
  }

  dynamicRange(options: MeteringDynamicRangeOptions = {}): DynamicRangeReport {
    this.requireAlive();
    return this.native.dynamicRange(options);
  }

  spectrum(options: SpectrumOptions = {}): SpectrumReport {
    this.requireAlive();
    return this.native.spectrum(options);
  }

  /**
   * True single-frame magnitude / power / dB spectrum starting at `frameOffset`.
   * See {@link meteringSpectrumFrame} for the frame-validation contract.
   */
  spectrumFrame(frameOffset = 0, options: SpectrumOptions = {}): SpectrumReport {
    this.requireAlive();
    return this.native.spectrumFrame(frameOffset, options);
  }

  ebur128LoudnessRange(): number {
    this.requireAlive();
    return this.native.ebur128LoudnessRange();
  }

  analyzeBpm(options: AnalyzeBpmOptions = {}): BpmAnalysisResult {
    return analyzeBpmFn(this.data, this.sampleRate, options);
  }

  analyzeImpulseResponse(nOctaveBands = 6): AcousticResult {
    return analyzeImpulseResponseFn(this.data, this.sampleRate, nOctaveBands);
  }

  detectAcoustic(options: AcousticOptions = {}): AcousticResult {
    return detectAcousticFn(this.data, this.sampleRate, options);
  }

  analyzeRhythm(options: AnalyzeRhythmOptions = {}): RhythmResult {
    return analyzeRhythmFn(this.data, this.sampleRate, options);
  }

  analyzeDynamics(options: AnalyzeDynamicsOptions = {}): DynamicsResult {
    return analyzeDynamicsFn(this.data, this.sampleRate, options);
  }

  analyzeTimbre(options: AnalyzeTimbreOptions = {}): TimbreResult {
    return analyzeTimbreFn(this.data, this.sampleRate, options);
  }

  detectChords(options: ChordDetectionOptions = {}): ChordAnalysisResult {
    return detectChordsFn(this.data, this.sampleRate, options);
  }

  chordFunctionalAnalysis(
    keyRoot: ChordFunctionsKey['root'],
    keyMode: ChordFunctionsKey['mode'] = 0,
    options: ChordDetectionOptions = {},
  ): FunctionalChord[] {
    return chordFunctionalAnalysisFn(this.data, keyRoot, keyMode, this.sampleRate, options);
  }

  // -- Effects --

  hpss(kernelHarmonic = 31, kernelPercussive = 31): HpssResult {
    return hpssFn(this.data, this.sampleRate, kernelHarmonic, kernelPercussive);
  }

  harmonic(): Float32Array {
    return harmonicFn(this.data, this.sampleRate);
  }

  percussive(): Float32Array {
    return percussiveFn(this.data, this.sampleRate);
  }

  timeStretch(rate: number): Float32Array {
    return timeStretchFn(this.data, this.sampleRate, rate);
  }

  pitchShift(semitones: number): Float32Array {
    return pitchShiftFn(this.data, this.sampleRate, semitones);
  }

  pitchCorrectToMidi(currentMidi = 69.0, targetMidi = 69.0): Float32Array {
    return pitchCorrectToMidiFn(this.data, this.sampleRate, currentMidi, targetMidi);
  }

  noteStretch(options: NoteStretchOptions = {}): Float32Array {
    return noteStretchFn(this.data, this.sampleRate, options);
  }

  noteMove(options: import('./types_notes.js').NoteMoveOptions = {}): Float32Array {
    return noteMoveFn(this.data, this.sampleRate, options);
  }

  voiceChange(options: VoiceChangeOptions = {}): Float32Array {
    return voiceChangeFn(this.data, this.sampleRate, options);
  }

  normalize(targetDb = 0.0): Float32Array {
    return normalizeFn(this.data, this.sampleRate, targetDb);
  }

  mastering(options: MasteringOptions = {}): MasteringResult {
    return masteringFn(this.data, this.sampleRate, options);
  }

  masteringProcess(
    processorName: SoloProcessor,
    params: Record<string, number | boolean> = {},
  ): MasteringResult {
    return masteringProcessFn(processorName, this.data, this.sampleRate, params);
  }

  masteringChain(
    config: MasteringChainConfig = {},
    onProgress?: ProgressCallback,
  ): MasteringChainResult {
    return masteringChainFn(this.data, this.sampleRate, config, onProgress);
  }

  masterAudio(
    preset: MasteringPreset = 'pop',
    overrides: MasteringChainConfig = {},
    onProgress?: ProgressCallback,
  ): MasteringChainResult {
    return masterAudioFn(this.data, this.sampleRate, preset, overrides, onProgress);
  }

  trim(thresholdDb = -60.0): Float32Array {
    return trimFn(this.data, this.sampleRate, thresholdDb);
  }

  // -- Features --

  stft(nFft = 2048, hopLength = 512): StftResult {
    return stftFn(this.data, this.sampleRate, nFft, hopLength);
  }

  stftDb(nFft = 2048, hopLength = 512): StftDbResult {
    return stftDbFn(this.data, this.sampleRate, nFft, hopLength);
  }

  melSpectrogram(
    nFft = 2048,
    hopLength = 512,
    nMels = 128,
    fmin = 0,
    fmax = 0,
    htk = false,
  ): MelSpectrogramResult {
    return melSpectrogramFn(this.data, this.sampleRate, nFft, hopLength, nMels, fmin, fmax, htk);
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
    return mfccFn(this.data, this.sampleRate, nFft, hopLength, nMels, nMfcc, fmin, fmax, htk);
  }

  chroma(nFft = 2048, hopLength = 512): ChromaResult {
    return chromaFn(this.data, this.sampleRate, nFft, hopLength);
  }

  spectralCentroid(nFft = 2048, hopLength = 512): Float32Array {
    return spectralCentroidFn(this.data, this.sampleRate, nFft, hopLength);
  }

  spectralBandwidth(nFft = 2048, hopLength = 512): Float32Array {
    return spectralBandwidthFn(this.data, this.sampleRate, nFft, hopLength);
  }

  spectralRolloff(nFft = 2048, hopLength = 512, rollPercent = 0.85): Float32Array {
    return spectralRolloffFn(this.data, this.sampleRate, nFft, hopLength, rollPercent);
  }

  spectralFlatness(nFft = 2048, hopLength = 512): Float32Array {
    return spectralFlatnessFn(this.data, this.sampleRate, nFft, hopLength);
  }

  zeroCrossingRate(frameLength = 2048, hopLength = 512): Float32Array {
    return zeroCrossingRateFn(this.data, this.sampleRate, frameLength, hopLength);
  }

  rmsEnergy(frameLength = 2048, hopLength = 512): Float32Array {
    return rmsEnergyFn(this.data, this.sampleRate, frameLength, hopLength);
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
      this.data,
      this.sampleRate,
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
      this.data,
      this.sampleRate,
      frameLength,
      hopLength,
      fmin,
      fmax,
      threshold,
      fillNa,
    );
  }

  resample(targetSr: number): Float32Array {
    return resampleFn(this.data, this.sampleRate, targetSr);
  }

  onsetEnvelope(nFft = 2048, hopLength = 512, nMels = 128): Float32Array {
    return onsetEnvelopeFn(this.data, this.sampleRate, nFft, hopLength, nMels);
  }

  nnlsChroma(): { nChroma: number; nFrames: number; data: Float32Array } {
    return nnlsChromaFn(this.data, this.sampleRate);
  }

  lufs(options: ValidateOptions = {}): LufsResult {
    return lufsFn(this.data, this.sampleRate, options);
  }

  momentaryLufs(options: ValidateOptions = {}): Float32Array {
    return momentaryLufsFn(this.data, this.sampleRate, options);
  }

  shortTermLufs(options: ValidateOptions = {}): Float32Array {
    return shortTermLufsFn(this.data, this.sampleRate, options);
  }
}
