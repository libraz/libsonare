import { createRequire } from 'node:module';
import type {
  AcousticResult,
  AnalysisResult,
  AutomationCurve,
  BpmAnalysisResult,
  ChordAnalysisResult,
  ChordChromaMethod,
  ChromaResult,
  DynamicsResult,
  EqBandInput,
  EqSpectrumSnapshot,
  HpssResult,
  Key,
  KeyCandidate,
  KeyDetectionOptions,
  LufsResult,
  MasteringChainResult,
  MasteringChainStereoResult,
  MasteringPreset,
  MasteringResult,
  MasteringStereoResult,
  MelSpectrogramResult,
  MfccResult,
  MixerProcessResult,
  MixOptions,
  MixResult,
  PairAnalysis,
  PairProcessor,
  PitchResult,
  RhythmResult,
  SoloProcessor,
  StereoAnalysis,
  StftDbResult,
  StftResult,
  TimbreResult,
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
    return this.native.detectBpm();
  }

  detectKey(options: KeyDetectionOptions = {}): Key {
    return addon.detectKey(this.getData(), this.getSampleRate(), options);
  }

  detectKeyCandidates(options: KeyDetectionOptions = {}): KeyCandidate[] {
    return addon.detectKeyCandidates(this.getData(), this.getSampleRate(), options);
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

  analyzeBpm(
    bpmMin = 30.0,
    bpmMax = 300.0,
    startBpm = 120.0,
    nFft = 2048,
    hopLength = 512,
    maxCandidates = 5,
  ): BpmAnalysisResult {
    return addon.analyzeBpm(
      this.getData(),
      this.getSampleRate(),
      bpmMin,
      bpmMax,
      startBpm,
      nFft,
      hopLength,
      maxCandidates,
    );
  }

  analyzeImpulseResponse(nOctaveBands = 6): AcousticResult {
    return addon.analyzeImpulseResponse(this.getData(), this.getSampleRate(), nOctaveBands);
  }

  detectAcoustic(
    nOctaveBands = 6,
    nThirdOctaveSubbands = 24,
    minDecayDb = 30.0,
    noiseFloorMarginDb = 10.0,
  ): AcousticResult {
    return addon.detectAcoustic(
      this.getData(),
      this.getSampleRate(),
      nOctaveBands,
      nThirdOctaveSubbands,
      minDecayDb,
      noiseFloorMarginDb,
    );
  }

  analyzeRhythm(
    bpmMin = 60.0,
    bpmMax = 200.0,
    startBpm = 120.0,
    nFft = 2048,
    hopLength = 512,
  ): RhythmResult {
    return addon.analyzeRhythm(
      this.getData(),
      this.getSampleRate(),
      bpmMin,
      bpmMax,
      startBpm,
      nFft,
      hopLength,
    );
  }

  analyzeDynamics(windowSec = 0.4, hopLength = 512, compressionThreshold = 6.0): DynamicsResult {
    return addon.analyzeDynamics(
      this.getData(),
      this.getSampleRate(),
      windowSec,
      hopLength,
      compressionThreshold,
    );
  }

  analyzeTimbre(
    nFft = 2048,
    hopLength = 512,
    nMels = 128,
    nMfcc = 13,
    windowSec = 0.5,
  ): TimbreResult {
    return addon.analyzeTimbre(
      this.getData(),
      this.getSampleRate(),
      nFft,
      hopLength,
      nMels,
      nMfcc,
      windowSec,
    );
  }

  detectChords(
    minDuration = 0.3,
    smoothingWindow = 2.0,
    threshold = 0.5,
    useTriadsOnly = false,
    nFft = 2048,
    hopLength = 512,
    useBeatSync = true,
    useHmm = false,
    hmmBeamWidth = 24,
    useKeyContext = false,
    keyRoot = 0,
    keyMode = 0,
    detectInversions = false,
    chromaMethod: ChordChromaMethod = 'stft',
  ): ChordAnalysisResult {
    return addon.detectChords(
      this.getData(),
      this.getSampleRate(),
      minDuration,
      smoothingWindow,
      threshold,
      useTriadsOnly,
      nFft,
      hopLength,
      useBeatSync,
      useHmm,
      hmmBeamWidth,
      useKeyContext,
      keyRoot,
      keyMode,
      detectInversions,
      chordChromaMethodValue(chromaMethod),
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

  normalize(targetDb = 0.0): Float32Array {
    return addon.normalize(this.getData(), this.getSampleRate(), targetDb);
  }

  mastering(targetLufs = -14.0, ceilingDb = -1.0, truePeakOversample = 4): MasteringResult {
    return addon.mastering(
      this.getData(),
      this.getSampleRate(),
      targetLufs,
      ceilingDb,
      truePeakOversample,
    );
  }

  masteringProcess(
    processorName: SoloProcessor,
    params: Record<string, number | boolean> = {},
  ): MasteringResult {
    return addon.masteringProcess(processorName, this.getData(), this.getSampleRate(), params);
  }

  masteringChain(
    config: Record<string, number | boolean> = {},
    onProgress?: (progress: number, stage: string) => void,
  ): MasteringChainResult {
    if (onProgress) {
      return addon.masteringChainWithProgress(
        this.getData(),
        this.getSampleRate(),
        config,
        onProgress,
      );
    }
    return addon.masteringChain(this.getData(), this.getSampleRate(), config);
  }

  masterAudio(
    preset: MasteringPreset = 'pop',
    overrides: Record<string, number | boolean> = {},
    onProgress?: (progress: number, stage: string) => void,
  ): MasteringChainResult {
    if (onProgress) {
      return addon.masterAudioWithProgress(
        preset,
        this.getData(),
        this.getSampleRate(),
        overrides,
        onProgress,
      );
    }
    return addon.masterAudio(preset, this.getData(), this.getSampleRate(), overrides);
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

  onsetEnvelope(nFft = 2048, hopLength = 512, nMels = 128): Float32Array {
    return addon.onsetEnvelope(this.getData(), this.getSampleRate(), nFft, hopLength, nMels);
  }

  nnlsChroma(): { nChroma: number; nFrames: number; data: Float32Array } {
    return addon.nnlsChroma(this.getData(), this.getSampleRate());
  }

  lufs(): LufsResult {
    return addon.lufs(this.getData(), this.getSampleRate());
  }

  momentaryLufs(): Float32Array {
    return addon.momentaryLufs(this.getData(), this.getSampleRate());
  }

  shortTermLufs(): Float32Array {
    return addon.shortTermLufs(this.getData(), this.getSampleRate());
  }
}

// ============================================================================
// Standalone functions
// ============================================================================

// -- Analysis --

export function detectBpm(samples: Float32Array, sampleRate = 22050): number {
  return addon.detectBpm(samples, sampleRate);
}

export function detectKey(
  samples: Float32Array,
  sampleRate = 22050,
  options: KeyDetectionOptions = {},
): Key {
  return addon.detectKey(samples, sampleRate, options);
}

export function detectKeyCandidates(
  samples: Float32Array,
  sampleRate = 22050,
  options: KeyDetectionOptions = {},
): KeyCandidate[] {
  return addon.detectKeyCandidates(samples, sampleRate, options);
}

export function detectBeats(samples: Float32Array, sampleRate = 22050): Float32Array {
  return addon.detectBeats(samples, sampleRate);
}

export function detectDownbeats(samples: Float32Array, sampleRate = 22050): Float32Array {
  return addon.detectDownbeats(samples, sampleRate);
}

export function detectOnsets(samples: Float32Array, sampleRate = 22050): Float32Array {
  return addon.detectOnsets(samples, sampleRate);
}

export function analyze(samples: Float32Array, sampleRate = 22050): AnalysisResult {
  return addon.analyze(samples, sampleRate);
}

export function analyzeBpm(
  samples: Float32Array,
  sampleRate = 22050,
  bpmMin = 30.0,
  bpmMax = 300.0,
  startBpm = 120.0,
  nFft = 2048,
  hopLength = 512,
  maxCandidates = 5,
): BpmAnalysisResult {
  return addon.analyzeBpm(
    samples,
    sampleRate,
    bpmMin,
    bpmMax,
    startBpm,
    nFft,
    hopLength,
    maxCandidates,
  );
}

export function analyzeRhythm(
  samples: Float32Array,
  sampleRate = 22050,
  bpmMin = 60.0,
  bpmMax = 200.0,
  startBpm = 120.0,
  nFft = 2048,
  hopLength = 512,
): RhythmResult {
  return addon.analyzeRhythm(samples, sampleRate, bpmMin, bpmMax, startBpm, nFft, hopLength);
}

export function analyzeDynamics(
  samples: Float32Array,
  sampleRate = 22050,
  windowSec = 0.4,
  hopLength = 512,
  compressionThreshold = 6.0,
): DynamicsResult {
  return addon.analyzeDynamics(samples, sampleRate, windowSec, hopLength, compressionThreshold);
}

export function analyzeImpulseResponse(
  samples: Float32Array,
  sampleRate = 48000,
  nOctaveBands = 6,
): AcousticResult {
  return addon.analyzeImpulseResponse(samples, sampleRate, nOctaveBands);
}

export function detectAcoustic(
  samples: Float32Array,
  sampleRate = 48000,
  nOctaveBands = 6,
  nThirdOctaveSubbands = 24,
  minDecayDb = 30.0,
  noiseFloorMarginDb = 10.0,
): AcousticResult {
  return addon.detectAcoustic(
    samples,
    sampleRate,
    nOctaveBands,
    nThirdOctaveSubbands,
    minDecayDb,
    noiseFloorMarginDb,
  );
}

export function analyzeTimbre(
  samples: Float32Array,
  sampleRate = 22050,
  nFft = 2048,
  hopLength = 512,
  nMels = 128,
  nMfcc = 13,
  windowSec = 0.5,
): TimbreResult {
  return addon.analyzeTimbre(samples, sampleRate, nFft, hopLength, nMels, nMfcc, windowSec);
}

export function detectChords(
  samples: Float32Array,
  sampleRate = 22050,
  minDuration = 0.3,
  smoothingWindow = 2.0,
  threshold = 0.5,
  useTriadsOnly = false,
  nFft = 2048,
  hopLength = 512,
  useBeatSync = true,
  useHmm = false,
  hmmBeamWidth = 24,
  useKeyContext = false,
  keyRoot = 0,
  keyMode = 0,
  detectInversions = false,
  chromaMethod: ChordChromaMethod = 'stft',
): ChordAnalysisResult {
  return addon.detectChords(
    samples,
    sampleRate,
    minDuration,
    smoothingWindow,
    threshold,
    useTriadsOnly,
    nFft,
    hopLength,
    useBeatSync,
    useHmm,
    hmmBeamWidth,
    useKeyContext,
    keyRoot,
    keyMode,
    detectInversions,
    chordChromaMethodValue(chromaMethod),
  );
}

function chordChromaMethodValue(method: ChordChromaMethod): number {
  if (method === 'stft') {
    return 0;
  }
  if (method === 'nnls') {
    return 1;
  }
  throw new Error(`Invalid chord chroma method: ${method}`);
}

export function version(): string {
  return addon.version();
}

/**
 * Returns whether the loaded native binding was compiled with FFmpeg support.
 *
 * When `true`, `Audio.fromFile` / `Audio.fromMemory` can decode M4A, AAC,
 * FLAC, OGG, Opus, etc. (anything libavformat handles). When `false`, only
 * WAV and MP3 are supported and other formats throw an actionable error.
 */
export function hasFfmpegSupport(): boolean {
  return addon.hasFfmpegSupport();
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

export function mastering(
  samples: Float32Array,
  sampleRate = 22050,
  targetLufs = -14.0,
  ceilingDb = -1.0,
  truePeakOversample = 4,
): MasteringResult {
  return addon.mastering(samples, sampleRate, targetLufs, ceilingDb, truePeakOversample);
}

export function masteringProcess(
  processorName: SoloProcessor,
  samples: Float32Array,
  sampleRate = 22050,
  params: Record<string, number | boolean> = {},
): MasteringResult {
  return addon.masteringProcess(processorName, samples, sampleRate, params);
}

export function masteringProcessStereo(
  processorName: SoloProcessor,
  left: Float32Array,
  right: Float32Array,
  sampleRate = 22050,
  params: Record<string, number | boolean> = {},
): MasteringStereoResult {
  return addon.masteringProcessStereo(processorName, left, right, sampleRate, params);
}

export function masteringChain(
  samples: Float32Array,
  sampleRate = 22050,
  config: Record<string, number | boolean> = {},
  onProgress?: (progress: number, stage: string) => void,
): MasteringChainResult {
  if (onProgress) {
    return addon.masteringChainWithProgress(samples, sampleRate, config, onProgress);
  }
  return addon.masteringChain(samples, sampleRate, config);
}

export function masteringChainStereo(
  left: Float32Array,
  right: Float32Array,
  sampleRate = 22050,
  config: Record<string, number | boolean> = {},
  onProgress?: (progress: number, stage: string) => void,
): MasteringChainStereoResult {
  if (onProgress) {
    return addon.masteringChainStereoWithProgress(left, right, sampleRate, config, onProgress);
  }
  return addon.masteringChainStereo(left, right, sampleRate, config);
}

/**
 * Block-by-block streaming variant of {@link masteringChain}.
 *
 * Maintains processor state across {@link processMono}/{@link processStereo}
 * calls. Only ProcessorBase-backed stages (`eq.tilt`, `dynamics.compressor`,
 * `saturation.tape`, `saturation.exciter`, `spectral.airBand`, `stereo.imager`,
 * `stereo.monoMaker`, `maximizer.truePeakLimiter`) are supported. Constructing
 * with `repair.denoise` or `loudness` enabled throws an Error.
 *
 * @example
 * ```typescript
 * const chain = new StreamingMasteringChain({ eq: { tiltDb: 1.0 } });
 * chain.prepare(44100, 512, 1);
 * const out = chain.processMono(blockSamples);
 * chain.reset();
 * ```
 */
export class StreamingMasteringChain {
  private native: InstanceType<typeof addon.StreamingMasteringChain>;

  constructor(config: Record<string, unknown> = {}) {
    this.native = new addon.StreamingMasteringChain(config);
  }

  /**
   * Initialize processors for the given sample rate and block layout.
   * Stereo-only stages are skipped when ``numChannels`` is 1.
   */
  prepare(sampleRate: number, maxBlockSize: number, numChannels: number): void {
    this.native.prepare(sampleRate, maxBlockSize, numChannels);
  }

  /** Process one mono block; returns the processed samples (same length). */
  processMono(samples: Float32Array): Float32Array {
    return this.native.processMono(samples);
  }

  /** Process one stereo block; returns the processed channels. */
  processStereo(
    left: Float32Array,
    right: Float32Array,
  ): { left: Float32Array; right: Float32Array } {
    return this.native.processStereo(left, right);
  }

  /** Reset all processor state without rebuilding. */
  reset(): void {
    this.native.reset();
  }

  /** Total reported latency in samples across all active processors. */
  latencySamples(): number {
    return this.native.latencySamples();
  }

  /** Ordered stage names that will run (e.g. ``"eq.tilt"``). */
  stageNames(): string[] {
    return this.native.stageNames();
  }
}

const EQ_PHASE_MODES: Record<string, number> = {
  zero: 1,
  'zero-latency': 1,
  zero_latency: 1,
  natural: 2,
  'natural-phase': 2,
  natural_phase: 2,
  linear: 3,
  'linear-phase': 3,
  linear_phase: 3,
};

/**
 * Block-by-block unified equalizer (zero-latency / natural / linear phase).
 *
 * Wraps the native `EqualizerProcessor`; state persists across
 * {@link processMono}/{@link processStereo} calls.
 *
 * @example
 * ```typescript
 * const eq = new StreamingEqualizer({ sampleRate: 48000, maxBlockSize: 512 });
 * eq.setBand(0, { type: 'HighShelf', frequencyHz: 8000, gainDb: 6, enabled: true });
 * const { left, right } = eq.processStereo(blockLeft, blockRight);
 * ```
 */
export class StreamingEqualizer {
  private native: InstanceType<typeof addon.StreamingEqualizer>;

  constructor(config: { sampleRate?: number; maxBlockSize?: number } = {}) {
    this.native = new addon.StreamingEqualizer(config);
  }

  /** Configure one EQ band (0-based index). */
  setBand(index: number, band: EqBandInput): void {
    this.native.setBand(index, band);
  }

  /** Disable all bands. */
  clear(): void {
    this.native.clear();
  }

  /** Set the global phase mode: ``'zero'`` | ``'natural'`` | ``'linear'`` or 1/2/3. */
  setPhaseMode(mode: 'zero' | 'natural' | 'linear' | number): void {
    const value = typeof mode === 'number' ? mode : EQ_PHASE_MODES[mode.toLowerCase()];
    if (value === undefined) {
      throw new Error(`unknown EQ phase mode: ${mode}`);
    }
    this.native.setPhaseMode(value);
  }

  /** Enable or disable output auto-gain compensation. */
  setAutoGain(enabled: boolean): void {
    this.native.setAutoGain(enabled);
  }

  /** Last applied auto-gain in dB (0 when disabled). */
  lastAutoGainDb(): number {
    return this.native.lastAutoGainDb();
  }

  /** Reported processing latency in samples. */
  latencySamples(): number {
    return this.native.latencySamples();
  }

  /** Process one mono block; returns the processed samples (same length). */
  processMono(samples: Float32Array): Float32Array {
    return this.native.processMono(samples);
  }

  /** Process one stereo block; returns the processed channels. */
  processStereo(
    left: Float32Array,
    right: Float32Array,
  ): { left: Float32Array; right: Float32Array } {
    return this.native.processStereo(left, right);
  }

  /** Latest realtime-safe spectrum snapshot. */
  spectrum(): EqSpectrumSnapshot {
    return this.native.spectrum();
  }

  /** Configure bands to match a reference spectrum (offline analysis). */
  match(
    source: Float32Array,
    reference: Float32Array,
    options: { sampleRate?: number; maxBands?: number } = {},
  ): void {
    this.native.match(source, reference, options);
  }
}

export function masteringPresetNames(): MasteringPreset[] {
  return addon.masteringPresetNames();
}

export function masterAudio(
  samples: Float32Array,
  sampleRate = 22050,
  preset: MasteringPreset = 'pop',
  overrides: Record<string, number | boolean> = {},
  onProgress?: (progress: number, stage: string) => void,
): MasteringChainResult {
  if (onProgress) {
    return addon.masterAudioWithProgress(preset, samples, sampleRate, overrides, onProgress);
  }
  return addon.masterAudio(preset, samples, sampleRate, overrides);
}

export function masterAudioStereo(
  left: Float32Array,
  right: Float32Array,
  sampleRate = 22050,
  preset: MasteringPreset = 'pop',
  overrides: Record<string, number | boolean> = {},
  onProgress?: (progress: number, stage: string) => void,
): MasteringChainStereoResult {
  if (onProgress) {
    return addon.masterAudioStereoWithProgress(
      preset,
      left,
      right,
      sampleRate,
      overrides,
      onProgress,
    );
  }
  return addon.masterAudioStereo(preset, left, right, sampleRate, overrides);
}

export function masteringProcessorNames(): SoloProcessor[] {
  return addon.masteringProcessorNames();
}

export function masteringPairProcessorNames(): PairProcessor[] {
  return addon.masteringPairProcessorNames();
}

export function masteringPairAnalysisNames(): PairAnalysis[] {
  return addon.masteringPairAnalysisNames();
}

export function masteringStereoAnalysisNames(): StereoAnalysis[] {
  return addon.masteringStereoAnalysisNames();
}

export function masteringPairProcess(
  processorName: PairProcessor,
  source: Float32Array,
  reference: Float32Array,
  sampleRate = 22050,
  params: Record<string, number | boolean> = {},
): MasteringResult {
  return addon.masteringPairProcess(processorName, source, reference, sampleRate, params);
}

export function masteringPairAnalyze(
  analysisName: PairAnalysis,
  source: Float32Array,
  reference: Float32Array,
  sampleRate = 22050,
  params: Record<string, number | boolean> = {},
): string {
  return addon.masteringPairAnalyze(analysisName, source, reference, sampleRate, params);
}

export function masteringStereoAnalyze(
  analysisName: StereoAnalysis,
  left: Float32Array,
  right: Float32Array,
  sampleRate = 22050,
  params: Record<string, number | boolean> = {},
): string {
  return addon.masteringStereoAnalyze(analysisName, left, right, sampleRate, params);
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

export function padCenter(values: Float32Array, size: number, padValue = 0.0): Float32Array {
  return addon.padCenter(values, size, padValue);
}

export function fixLength(values: Float32Array, size: number, padValue = 0.0): Float32Array {
  return addon.fixLength(values, size, padValue);
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

export function pcen(
  values: Float32Array,
  nBins: number,
  nFrames: number,
  options: Record<string, number> = {},
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
): { nFrames: number; winLength: number; data: Float32Array } {
  return addon.tempogram(onsetEnvelope, sampleRate, hopLength, winLength);
}

export function cyclicTempogram(
  onsetEnvelope: Float32Array,
  sampleRate: number,
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
): Float32Array {
  return addon.tempogramRatio(tempogramData, winLength, sampleRate, hopLength);
}

export function nnlsChroma(
  samples: Float32Array,
  sampleRate = 22050,
): { nChroma: number; nFrames: number; data: Float32Array } {
  return addon.nnlsChroma(samples, sampleRate);
}

export function lufs(samples: Float32Array, sampleRate = 22050): LufsResult {
  return addon.lufs(samples, sampleRate);
}

export function momentaryLufs(samples: Float32Array, sampleRate = 22050): Float32Array {
  return addon.momentaryLufs(samples, sampleRate);
}

export function shortTermLufs(samples: Float32Array, sampleRate = 22050): Float32Array {
  return addon.shortTermLufs(samples, sampleRate);
}

export function resample(samples: Float32Array, srcSr: number, targetSr: number): Float32Array {
  return addon.resample(samples, srcSr, targetSr);
}

export function mixingScenePresetNames(): string[] {
  return addon.mixingScenePresetNames();
}

export function mixingScenePresetJson(preset: string): string {
  return addon.mixingScenePresetJson(preset);
}

/**
 * Scene-based persistent stereo mixer. Built from a scene JSON string, it routes
 * per-strip stereo blocks through a compiled routing graph (sends, buses,
 * inserts) into a stereo master. Insert-parameter automation is scheduled by
 * strip index; the underlying strip handles are never exposed.
 */
export class Mixer {
  private native: InstanceType<typeof addon.Mixer>;

  private constructor(native: InstanceType<typeof addon.Mixer>) {
    this.native = native;
  }

  /** Build a mixer from a scene JSON string (see {@link mixingScenePresetJson}). */
  static fromSceneJson(json: string, sampleRate = 48000, blockSize = 512): Mixer {
    return new Mixer(new addon.Mixer(json, sampleRate, blockSize));
  }

  /** Rebuild and compile the routing graph from the current scene topology. */
  compile(): void {
    this.native.compile();
  }

  /** Number of strips in the mixer. */
  stripCount(): number {
    return this.native.stripCount();
  }

  /**
   * Mix one block of per-strip stereo audio into the stereo master.
   *
   * @param leftChannels - `leftChannels[i]` is the left channel of strip `i`
   * @param rightChannels - `rightChannels[i]` is the right channel of strip `i`
   */
  processStereo(leftChannels: Float32Array[], rightChannels: Float32Array[]): MixerProcessResult {
    if (leftChannels.length !== rightChannels.length) {
      throw new Error('leftChannels and rightChannels must have the same length.');
    }
    return this.native.processStereo(leftChannels, rightChannels);
  }

  /**
   * Schedule a sample-accurate insert-parameter automation event.
   *
   * @param stripIndex - Strip index in `[0, stripCount())`
   * @param insertIndex - Index into the strip's combined [pre... post...] inserts
   * @param paramId - Processor-specific parameter id
   * @param samplePos - Absolute sample position from the start of processing
   * @param value - Target parameter value
   * @param curve - Interpolation curve toward the value (default `'linear'`)
   */
  scheduleInsertAutomation(
    stripIndex: number,
    insertIndex: number,
    paramId: number,
    samplePos: number,
    value: number,
    curve: AutomationCurve = 'linear',
  ): void {
    this.native.scheduleInsertAutomation(
      stripIndex,
      insertIndex,
      paramId,
      samplePos,
      value,
      curve === 'exponential' ? 1 : 0,
    );
  }

  /** Serialize the current scene (strips, buses, sends, connections) to JSON. */
  toSceneJson(): string {
    return this.native.toSceneJson();
  }

  /** Release the underlying native mixer. Safe to call only once. */
  destroy(): void {
    this.native.destroy();
  }

  /** Alias for {@link destroy}, provided for cross-binding (WASM) compatibility. */
  delete(): void {
    this.destroy();
  }
}

export function mixStereo(
  leftChannels: Float32Array[],
  rightChannels: Float32Array[],
  sampleRate = 48000,
  options: MixOptions = {},
): MixResult {
  return addon.mixStereo(leftChannels, rightChannels, sampleRate, options);
}

export type {
  AcousticResult,
  AnalysisResult,
  AutomationCurve,
  BpmAnalysisResult,
  BpmCandidate,
  Chord,
  ChordAnalysisResult,
  ChordChromaMethod,
  ChromaResult,
  DynamicsResult,
  EqBandInput,
  EqSpectrumSnapshot,
  HpssResult,
  Key,
  KeyCandidate,
  KeyDetectionOptions,
  KeyMode,
  LufsResult,
  MasteringChainResult,
  MasteringChainStereoResult,
  MasteringResult,
  MasteringStereoResult,
  MelSpectrogramResult,
  MfccResult,
  MixerProcessResult,
  MixMeterSnapshot,
  MixOptions,
  MixResult,
  PanMode,
  PitchResult,
  RhythmResult,
  StftDbResult,
  StftResult,
  TimbreResult,
  TimeSignature,
} from './types.js';
