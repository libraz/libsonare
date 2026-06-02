import { createRequire } from 'node:module';
import type {
  AcousticResult,
  AnalysisProgressCallback,
  AnalysisResult,
  AutomationCurve,
  BpmAnalysisResult,
  ChordAnalysisResult,
  ChordChromaMethod,
  ChromaResult,
  CqtResult,
  DynamicsResult,
  EngineAutomationPoint,
  EngineBounceOptions,
  EngineBounceResult,
  EngineCaptureStatus,
  EngineClip,
  EngineFreezeOptions,
  EngineFreezeResult,
  EngineGraphSpec,
  EngineMarker,
  EngineMeterTelemetry,
  EngineMetronomeConfig,
  EngineParameterInfo,
  EngineTelemetry,
  EngineTransportState,
  EqBandInput,
  EqSpectrumSnapshot,
  GoniometerPoint,
  HpssResult,
  InverseMelResult,
  InverseStftResult,
  Key,
  KeyCandidate,
  KeyDetectionOptions,
  LufsResult,
  MasteringChainResult,
  MasteringChainStereoResult,
  MasteringPreset,
  MasteringResult,
  MasteringStereoResult,
  Matrix2D,
  MelodyResult,
  MelSpectrogramResult,
  MeterTap,
  MfccResult,
  MixerProcessResult,
  MixMeterSnapshot,
  MixOptions,
  MixResult,
  PairAnalysis,
  PairProcessor,
  PanLaw,
  PanMode,
  PitchResult,
  RealtimeVoiceChangerConfig,
  RealtimeVoiceChangerConfigInput,
  RealtimeVoiceChangerOptions,
  RhythmResult,
  Section,
  SendTiming,
  SoloProcessor,
  StereoAnalysis,
  StftDbResult,
  StftResult,
  StreamAnalyzerConfig,
  StreamAnalyzerStats,
  StreamFramesI16,
  StreamFramesSoa,
  StreamFramesU8,
  StreamingPlatform,
  StripRef,
  TempogramMode,
  TimbreResult,
  VoicePresetId,
} from './types.js';

const require = createRequire(import.meta.url);
const addon = require('../build/Release/sonare-node.node');

/**
 * Per-call validation options accepted by guarded wrappers. Empty-buffer
 * checks are always performed; pass `{ validate: false }` to opt out of the
 * O(n) NaN/Inf scan on hot paths where the caller already controls the data.
 */
export interface ValidateOptions {
  validate?: boolean;
}

function assertNonEmptySamples(
  fnName: string,
  samples: ArrayLike<number>,
  argName = 'samples',
): void {
  if (samples.length === 0) {
    throw new RangeError(`${fnName}: ${argName} must not be empty`);
  }
}

function assertFiniteSamples(
  fnName: string,
  samples: ArrayLike<number>,
  validate: boolean,
  argName = 'samples',
): void {
  if (!validate) {
    return;
  }
  for (let i = 0; i < samples.length; i++) {
    const v = samples[i] as number;
    if (!Number.isFinite(v)) {
      throw new RangeError(`${fnName}: ${argName} contains NaN or Inf at index ${i}`);
    }
  }
}

function assertSamples(
  fnName: string,
  samples: ArrayLike<number>,
  validate: boolean,
  argName = 'samples',
): void {
  assertNonEmptySamples(fnName, samples, argName);
  assertFiniteSamples(fnName, samples, validate, argName);
}

function assertFiniteScalar(fnName: string, value: number, argName: string): void {
  if (!Number.isFinite(value)) {
    throw new RangeError(`${fnName}: ${argName} must be a finite number`);
  }
}

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

  pitchCorrectToMidi(currentMidi = 69.0, targetMidi = 69.0): Float32Array {
    return addon.pitchCorrectToMidi(this.getData(), this.getSampleRate(), currentMidi, targetMidi);
  }

  noteStretch(onsetSample = 0, offsetSample = 0, stretchRatio = 1.0): Float32Array {
    return addon.noteStretch(
      this.getData(),
      this.getSampleRate(),
      onsetSample,
      offsetSample,
      stretchRatio,
    );
  }

  voiceChange(
    pitchSemitones = 0.0,
    formantFactor = 1.0,
    options: ValidateOptions = {},
  ): Float32Array {
    const data = this.getData();
    assertSamples('voiceChange', data, options.validate !== false);
    return addon.voiceChange(data, this.getSampleRate(), pitchSemitones, formantFactor);
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

  mfcc(nFft = 2048, hopLength = 512, nMels = 128, nMfcc = 20): MfccResult {
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
    threshold = 0.3,
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

export class RealtimeVoiceChanger {
  private native: InstanceType<typeof addon.RealtimeVoiceChanger>;

  constructor(options: RealtimeVoiceChangerOptions) {
    this.native = new addon.RealtimeVoiceChanger(options.preset ?? 'neutral-monitor');
    this.native.prepare(options.sampleRate, options.maxBlockSize ?? 128, options.channels ?? 1);
  }

  reset(): void {
    this.native.reset();
  }

  setConfig(config: RealtimeVoiceChangerConfigInput): void {
    this.native.setConfig(config);
  }

  configJson(): string {
    return this.native.configJson();
  }

  latencySamples(): number {
    return this.native.latencySamples();
  }

  processMono(input: Float32Array): Float32Array {
    return this.native.processMono(input);
  }

  processMonoInto(input: Float32Array, output: Float32Array): void {
    this.native.processMonoInto(input, output);
  }

  processInterleaved(input: Float32Array, channels: 1 | 2): Float32Array {
    return this.native.processInterleaved(input, channels);
  }

  processInterleavedInto(input: Float32Array, channels: 1 | 2, output: Float32Array): void {
    this.native.processInterleavedInto(input, channels, output);
  }

  /**
   * Process a block of planar (non-interleaved) stereo audio in place. The
   * `left` and `right` buffers must have equal length and are mutated with the
   * processed output. Requires the changer to have been prepared with at least
   * 2 channels.
   */
  processPlanarStereo(left: Float32Array, right: Float32Array): void {
    this.native.processPlanarStereo(left, right);
  }

  destroy(): void {
    // N-API ObjectWrap instances do not have a `.delete` method, so this guard
    // is purely defensive in case the native binding ever exposes one. The
    // real lifecycle is GC-driven via the C++ destructor.
    if (typeof this.native.delete === 'function') {
      this.native.delete();
    }
  }
}

export class RealtimeEngine {
  private native: InstanceType<typeof addon.RealtimeEngine>;

  constructor(
    sampleRate = 48000,
    maxBlockSize = 128,
    commandCapacity = 1024,
    telemetryCapacity = 1024,
  ) {
    this.native = new addon.RealtimeEngine(
      sampleRate,
      maxBlockSize,
      commandCapacity,
      telemetryCapacity,
    );
  }

  prepare(
    sampleRate: number,
    maxBlockSize: number,
    commandCapacity = 1024,
    telemetryCapacity = 1024,
  ): void {
    this.native.prepare(sampleRate, maxBlockSize, commandCapacity, telemetryCapacity);
  }

  play(renderFrame = -1): void {
    this.native.play(renderFrame);
  }

  stop(renderFrame = -1): void {
    this.native.stop(renderFrame);
  }

  seekSample(timelineSample: number, renderFrame = -1): void {
    this.native.seekSample(timelineSample, renderFrame);
  }

  seekPpq(ppq: number, renderFrame = -1): void {
    this.native.seekPpq(ppq, renderFrame);
  }

  setTempo(bpm: number): void {
    this.native.setTempo(bpm);
  }

  setTimeSignature(numerator: number, denominator: number): void {
    this.native.setTimeSignature(numerator, denominator);
  }

  setLoop(startPpq: number, endPpq: number, enabled = true): void {
    this.native.setLoop(startPpq, endPpq, enabled);
  }

  addParameter(info: EngineParameterInfo): void {
    this.native.addParameter(info);
  }

  parameterCount(): number {
    return this.native.parameterCount();
  }

  parameterInfoByIndex(index: number): EngineParameterInfo {
    return this.native.parameterInfoByIndex(index);
  }

  parameterInfo(id: number): EngineParameterInfo {
    return this.native.parameterInfo(id);
  }

  setAutomationLane(paramId: number, points: EngineAutomationPoint[]): void {
    this.native.setAutomationLane(paramId, points);
  }

  automationLaneCount(): number {
    return this.native.automationLaneCount();
  }

  setMarkers(markers: EngineMarker[]): void {
    this.native.setMarkers(markers);
  }

  markerCount(): number {
    return this.native.markerCount();
  }

  markerByIndex(index: number): EngineMarker {
    return this.native.markerByIndex(index);
  }

  marker(id: number): EngineMarker {
    return this.native.marker(id);
  }

  seekMarker(markerId: number, renderFrame = -1): void {
    this.native.seekMarker(markerId, renderFrame);
  }

  setLoopFromMarkers(startMarkerId: number, endMarkerId: number): void {
    this.native.setLoopFromMarkers(startMarkerId, endMarkerId);
  }

  setMetronome(config: EngineMetronomeConfig): void {
    this.native.setMetronome(config);
  }

  metronome(): Required<EngineMetronomeConfig> {
    return this.native.metronome();
  }

  countInEndSample(startSample: number, bars: number): number {
    return this.native.countInEndSample(startSample, bars);
  }

  setClips(clips: EngineClip[]): void {
    this.native.setClips(clips);
  }

  clipCount(): number {
    return this.native.clipCount();
  }

  setCaptureBuffer(channels: Float32Array[]): void {
    this.native.setCaptureBuffer(channels);
  }

  armCapture(armed = true): void {
    this.native.armCapture(armed);
  }

  setCapturePunch(startSample: number, endSample: number, enabled = true): void {
    this.native.setCapturePunch(startSample, endSample, enabled);
  }

  resetCapture(): void {
    this.native.resetCapture();
  }

  captureStatus(): EngineCaptureStatus {
    return this.native.captureStatus();
  }

  /**
   * Read the recorded samples out of the capture buffer.
   *
   * Returns one `Float32Array` per capture channel, each sliced to the number
   * of frames recorded so far (see {@link captureStatus}). Call after capture
   * to retrieve the audio written into the buffers passed to
   * {@link setCaptureBuffer}.
   */
  capturedAudio(): Float32Array[] {
    return this.native.capturedAudio();
  }

  setGraph(spec: EngineGraphSpec): void {
    this.native.setGraph(spec);
  }

  graphNodeCount(): number {
    return this.native.graphNodeCount();
  }

  graphConnectionCount(): number {
    return this.native.graphConnectionCount();
  }

  process(channels: Float32Array[]): Float32Array[] {
    return this.native.process(channels);
  }

  processWithMonitor(channels: Float32Array[]): {
    output: Float32Array[];
    monitor: Float32Array[];
  } {
    return this.native.processWithMonitor(channels);
  }

  renderOffline(channels: Float32Array[], blockSize = 128): Float32Array[] {
    return this.native.renderOffline(channels, blockSize);
  }

  bounceOffline(options: EngineBounceOptions): EngineBounceResult {
    return this.native.bounceOffline(options);
  }

  freezeOffline(options: EngineFreezeOptions): EngineFreezeResult {
    return this.native.freezeOffline(options);
  }

  drainTelemetry(maxRecords = 1024): EngineTelemetry[] {
    return this.native.drainTelemetry(maxRecords);
  }

  /** Drain pending meter telemetry records published by the engine's meter tap. */
  drainMeterTelemetry(maxRecords = 1024): EngineMeterTelemetry[] {
    return this.native.drainMeterTelemetry(maxRecords);
  }

  /**
   * Push a live parameter value to the engine (immediate jump).
   *
   * @param paramId - Target parameter id
   * @param value - New value
   * @param renderFrame - Render-frame time to apply, or `-1` for immediate
   */
  setParameter(paramId: number, value: number, renderFrame = -1): void {
    this.native.setParameter(paramId, value, renderFrame);
  }

  /** Push a live parameter value to the engine using a smoothed ramp. */
  setParameterSmoothed(paramId: number, value: number, renderFrame = -1): void {
    this.native.setParameterSmoothed(paramId, value, renderFrame);
  }

  /** Read the current engine transport state (playing/position/ppq/tempo). */
  getTransportState(): EngineTransportState {
    return this.native.getTransportState();
  }

  destroy(): void {
    this.native.destroy();
  }
}

export function engineAbiVersion(): number {
  return addon.engineAbiVersion();
}

export function voiceChangerAbiVersion(): number {
  return addon.voiceChangerAbiVersion();
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

/**
 * Asynchronous variant of {@link analyze}. Runs the DSP pipeline on a libuv
 * worker thread so the JS event loop is never blocked. The returned promise
 * resolves with the same shape as the synchronous version.
 */
export function analyzeAsync(samples: Float32Array, sampleRate = 22050): Promise<AnalysisResult> {
  return addon.analyzeAsync(samples, sampleRate);
}

/**
 * Run the full music analysis, reporting per-stage progress.
 *
 * The progress callback is invoked synchronously during analysis with a
 * normalized progress value in `[0, 1]` and the current stage name. The result
 * shape matches {@link analyze}.
 */
export function analyzeWithProgress(
  samples: Float32Array,
  sampleRate = 22050,
  onProgress: AnalysisProgressCallback,
): AnalysisResult {
  return addon.analyzeWithProgress(samples, sampleRate, onProgress);
}

/** Detect song-structure sections (intro/verse/chorus/...). */
export function analyzeSections(
  samples: Float32Array,
  sampleRate = 22050,
  nFft = 2048,
  hopLength = 512,
  minSectionSec = 4.0,
): Section[] {
  return addon.analyzeSections(samples, sampleRate, nFft, hopLength, minSectionSec);
}

/** Extract the melody contour from monophonic audio via YIN. */
export function analyzeMelody(
  samples: Float32Array,
  sampleRate = 22050,
  fmin = 65.0,
  fmax = 2093.0,
  frameLength = 2048,
  hopLength = 256,
  threshold = 0.1,
): MelodyResult {
  return addon.analyzeMelody(samples, sampleRate, fmin, fmax, frameLength, hopLength, threshold);
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

export function timeStretch(samples: Float32Array, rate: number, sampleRate = 22050): Float32Array {
  if (typeof rate !== 'number' || !Number.isFinite(rate)) {
    throw new TypeError('timeStretch: rate must be a finite number');
  }
  return addon.timeStretch(samples, sampleRate, rate);
}

export function pitchShift(
  samples: Float32Array,
  semitones: number,
  sampleRate = 22050,
): Float32Array {
  if (typeof semitones !== 'number' || !Number.isFinite(semitones)) {
    throw new TypeError('pitchShift: semitones must be a finite number');
  }
  return addon.pitchShift(samples, sampleRate, semitones);
}

export function pitchCorrectToMidi(
  samples: Float32Array,
  sampleRate = 22050,
  currentMidi = 69.0,
  targetMidi = 69.0,
): Float32Array {
  return addon.pitchCorrectToMidi(samples, sampleRate, currentMidi, targetMidi);
}

export function noteStretch(
  samples: Float32Array,
  sampleRate = 22050,
  onsetSample = 0,
  offsetSample = 0,
  stretchRatio = 1.0,
): Float32Array {
  return addon.noteStretch(samples, sampleRate, onsetSample, offsetSample, stretchRatio);
}

export function voiceChange(
  samples: Float32Array,
  sampleRate = 22050,
  pitchSemitones = 0.0,
  formantFactor = 1.0,
  options: ValidateOptions = {},
): Float32Array {
  const validate = options.validate !== false;
  assertSamples('voiceChange', samples, validate);
  return addon.voiceChange(samples, sampleRate, pitchSemitones, formantFactor);
}

export interface VoiceChangeRealtimeOptions extends ValidateOptions {
  /** Channel count: 1 = mono, 2 = interleaved stereo (L0,R0,L1,R1,...). */
  channels?: 1 | 2;
}

export function voiceChangeRealtime(
  samples: Float32Array,
  sampleRate = 48000,
  preset: RealtimeVoiceChangerConfigInput = 'neutral-monitor',
  options: VoiceChangeRealtimeOptions = {},
): Float32Array {
  const validate = options.validate !== false;
  assertSamples('voiceChangeRealtime', samples, validate);
  const channels = options.channels ?? 1;
  if (channels !== 1 && channels !== 2) {
    throw new Error('voiceChangeRealtime: channels must be 1 or 2.');
  }
  const block = 512;
  const changer = new RealtimeVoiceChanger({
    sampleRate,
    maxBlockSize: block,
    channels,
    preset,
  });
  const output = new Float32Array(samples.length);
  try {
    if (channels === 1) {
      for (let pos = 0; pos < samples.length; pos += block) {
        const inputBlock = samples.subarray(pos, Math.min(samples.length, pos + block));
        const outputBlock = output.subarray(pos, pos + inputBlock.length);
        changer.processMonoInto(inputBlock, outputBlock);
      }
    } else {
      // Interleaved stereo: each block carries `block` frames = block*2 samples,
      // mirroring the WASM voiceChangeRealtime stereo path.
      const frameStride = block * 2;
      for (let pos = 0; pos < samples.length; pos += frameStride) {
        const inputBlock = samples.subarray(pos, Math.min(samples.length, pos + frameStride));
        const outputBlock = output.subarray(pos, pos + inputBlock.length);
        changer.processInterleavedInto(inputBlock, 2, outputBlock);
      }
    }
  } finally {
    changer.destroy();
  }
  return output;
}

export function realtimeVoiceChangerPresetNames(): VoicePresetId[] {
  return addon.realtimeVoiceChangerPresetNames() as VoicePresetId[];
}

export function realtimeVoiceChangerPresetJson(name: VoicePresetId): string {
  return addon.realtimeVoiceChangerPresetJson(name);
}

export function validateRealtimeVoiceChangerPresetJson(json: string): {
  ok: boolean;
  normalizedJson?: string;
  error?: string;
} {
  return addon.validateRealtimeVoiceChangerPresetJson(json);
}

// Ordinals mirror the SonareVoiceCharacterPreset enum (sonare_c_types.h).
const VOICE_PRESET_ORDINALS: Record<VoicePresetId, number> = {
  'neutral-monitor': 0,
  'bright-idol': 1,
  'soft-whisper': 2,
  'deep-narrator': 3,
  'robot-mascot': 4,
  'dark-villain': 5,
};

function resolveVoicePresetOrdinal(preset: VoicePresetId | number): number {
  if (typeof preset === 'number') {
    return preset;
  }
  const ordinal = VOICE_PRESET_ORDINALS[preset];
  if (ordinal === undefined) {
    // Mirror the WASM/Python bindings: an unknown preset name is an error, not
    // a silent `undefined` ordinal that would corrupt the native call.
    throw new Error(`Unknown voice character preset: ${preset}`);
  }
  return ordinal;
}

/**
 * Returns the canonical preset id for a voice-character preset ordinal (or id),
 * or `null` when the ordinal is out of range.
 */
export function voiceCharacterPresetId(preset: VoicePresetId | number): VoicePresetId | null {
  return addon.voiceCharacterPresetId(resolveVoicePresetOrdinal(preset)) as VoicePresetId | null;
}

/**
 * Returns the flat (normalized) realtime-voice-changer config for a built-in
 * preset, skipping the JSON round-trip.
 */
export function realtimeVoiceChangerPresetConfig(
  preset: VoicePresetId | number,
): RealtimeVoiceChangerConfig {
  return addon.realtimeVoiceChangerPresetConfig(
    resolveVoicePresetOrdinal(preset),
  ) as RealtimeVoiceChangerConfig;
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

/**
 * Stateful real-time / streaming music analyzer.
 *
 * Feed mono blocks with {@link process}; drain analysis frames with
 * {@link readFramesSoa} (or quantized variants) and query the running musical
 * estimate (BPM/key/chord/pattern) with {@link stats}.
 *
 * @example
 * ```typescript
 * const analyzer = new StreamAnalyzer({ sampleRate: 44100 });
 * analyzer.process(block);
 * const frames = analyzer.readFramesSoa(analyzer.availableFrames());
 * const { estimate } = analyzer.stats();
 * ```
 */
export class StreamAnalyzer {
  private native: InstanceType<typeof addon.StreamAnalyzer>;

  constructor(config: StreamAnalyzerConfig = {}) {
    this.native = new addon.StreamAnalyzer(config);
  }

  /** Feed a mono block of samples. */
  process(samples: Float32Array): void {
    this.native.process(samples);
  }

  /** Feed a mono block anchored at an absolute sample offset. */
  processWithOffset(samples: Float32Array, sampleOffset: number): void {
    this.native.processWithOffset(samples, sampleOffset);
  }

  /** Number of analysis frames ready to read. */
  availableFrames(): number {
    return this.native.availableFrames();
  }

  /** Drain up to `maxFrames` frames as float32 structure-of-arrays. */
  readFramesSoa(maxFrames: number): StreamFramesSoa {
    return this.native.readFramesSoa(maxFrames);
  }

  /** Drain up to `maxFrames` frames as uint8-quantized arrays. */
  readFramesU8(maxFrames: number): StreamFramesU8 {
    return this.native.readFramesU8(maxFrames);
  }

  /** Drain up to `maxFrames` frames as int16-quantized arrays. */
  readFramesI16(maxFrames: number): StreamFramesI16 {
    return this.native.readFramesI16(maxFrames);
  }

  /** Reset analyzer state; optionally re-anchor to a base sample offset. */
  reset(baseOffset = 0): void {
    this.native.reset(baseOffset);
  }

  /** Current progressive musical estimate and totals. */
  stats(): StreamAnalyzerStats {
    return this.native.stats();
  }

  /** Total frames processed so far. */
  frameCount(): number {
    return this.native.frameCount();
  }

  /** Current analysis time in seconds. */
  currentTime(): number {
    return this.native.currentTime();
  }

  /** Configured sample rate in Hz. */
  sampleRate(): number {
    return this.native.sampleRate();
  }

  /** Hint the expected total duration (seconds) to tune progressive estimates. */
  setExpectedDuration(seconds: number): void {
    this.native.setExpectedDuration(seconds);
  }

  /** Set a normalization gain applied to incoming samples. */
  setNormalizationGain(gain: number): void {
    this.native.setNormalizationGain(gain);
  }

  /** Set the tuning reference frequency (Hz) for key/chroma analysis. */
  setTuningRefHz(hz: number): void {
    this.native.setTuningRefHz(hz);
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

  /** Set all-band EQ gain scale as a 0.0..2.0 multiplier. */
  setGainScale(scale: number): void {
    this.native.setGainScale(scale);
  }

  /** Set post-EQ output gain in dB. */
  setOutputGainDb(gainDb: number): void {
    this.native.setOutputGainDb(gainDb);
  }

  /** Set post-EQ stereo balance in -1.0..1.0; mono input ignores pan. */
  setOutputPan(pan: number): void {
    this.native.setOutputPan(pan);
  }

  /** Set a mono external key for dynamic bands with `externalSidechain` enabled. */
  setSidechainMono(samples: Float32Array): void {
    this.native.setSidechainMono(samples);
  }

  /** Set a stereo external key for dynamic bands with `externalSidechain` enabled. */
  setSidechainStereo(left: Float32Array, right: Float32Array): void {
    this.native.setSidechainStereo(left, right);
  }

  /** Clear any pending external key before the next process call. */
  clearSidechain(): void {
    this.native.clearSidechain();
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
  presetName: MasteringPreset = 'pop',
  overrides: Record<string, number | boolean> = {},
  onProgress?: (progress: number, stage: string) => void,
): MasteringChainResult {
  if (onProgress) {
    return addon.masterAudioWithProgress(presetName, samples, sampleRate, overrides, onProgress);
  }
  return addon.masterAudio(presetName, samples, sampleRate, overrides);
}

/**
 * Asynchronous variant of {@link masterAudio}. Runs the full chain on a libuv
 * worker thread; the returned promise resolves with the same shape as the
 * synchronous version. Progress reporting is not available on the async path
 * (use the synchronous `masterAudio` with `onProgress` if you need it, or
 * spin up multiple async calls in parallel).
 */
export function masterAudioAsync(
  samples: Float32Array,
  sampleRate = 22050,
  presetName: MasteringPreset = 'pop',
  overrides: Record<string, number | boolean> = {},
): Promise<MasteringChainResult> {
  return addon.masterAudioAsync(presetName, samples, sampleRate, overrides);
}

export function masterAudioStereo(
  left: Float32Array,
  right: Float32Array,
  sampleRate = 22050,
  presetName: MasteringPreset = 'pop',
  overrides: Record<string, number | boolean> = {},
  onProgress?: (progress: number, stage: string) => void,
): MasteringChainStereoResult {
  if (onProgress) {
    return addon.masterAudioStereoWithProgress(
      presetName,
      left,
      right,
      sampleRate,
      overrides,
      onProgress,
    );
  }
  return addon.masterAudioStereo(presetName, left, right, sampleRate, overrides);
}

/**
 * Asynchronous variant of {@link masterAudioStereo}.
 */
export function masterAudioStereoAsync(
  left: Float32Array,
  right: Float32Array,
  sampleRate = 22050,
  presetName: MasteringPreset = 'pop',
  overrides: Record<string, number | boolean> = {},
): Promise<MasteringChainStereoResult> {
  return addon.masterAudioStereoAsync(presetName, left, right, sampleRate, overrides);
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

export function masteringAssistantSuggest(
  samples: Float32Array,
  sampleRate = 22050,
  params: Record<string, number | boolean> = {},
): string {
  return addon.masteringAssistantSuggest(samples, sampleRate, params);
}

export function masteringAudioProfile(
  samples: Float32Array,
  sampleRate = 22050,
  params: Record<string, number | boolean> = {},
): string {
  return addon.masteringAudioProfile(samples, sampleRate, params);
}

export function masteringStreamingPreview(
  samples: Float32Array,
  sampleRate = 22050,
  platforms: StreamingPlatform[] = [],
): string {
  return addon.masteringStreamingPreview(samples, sampleRate, platforms);
}

/** Options for `masteringRepairDeclick`. */
export interface DeclickOptions {
  threshold?: number;
  neighborRatio?: number;
  maxClickSamples?: number;
  lpcOrder?: number;
  residualRatio?: number;
}

/** Algorithms accepted by `masteringRepairDenoiseClassical`. */
export type DenoiseClassicalMode = 'logMmse' | 'mmseStsa' | 'spectralSubtraction';

/** Noise PSD estimators accepted by `masteringRepairDenoiseClassical`. */
export type DenoiseClassicalNoiseEstimator = 'quantile' | 'mcra' | 'imcra';

/** Options for `masteringRepairDenoiseClassical`. */
export interface DenoiseClassicalOptions {
  mode?: DenoiseClassicalMode;
  noiseEstimator?: DenoiseClassicalNoiseEstimator;
  nFft?: number;
  hopLength?: number;
  ddAlpha?: number;
  gainFloor?: number;
  overSubtraction?: number;
  spectralFloor?: number;
  noiseEstimationQuantile?: number;
  speechPresenceGain?: boolean;
  gainSmoothing?: boolean;
}

/** Offline LPC-based declicker. */
export function masteringRepairDeclick(
  samples: Float32Array,
  sampleRate = 22050,
  options: DeclickOptions = {},
): Float32Array {
  return addon.masteringRepairDeclick(samples, sampleRate, options);
}

/** Offline STFT-domain classical denoiser (LogMMSE / MMSE-STSA / SpectralSubtraction). */
export function masteringRepairDenoiseClassical(
  samples: Float32Array,
  sampleRate = 22050,
  options: DenoiseClassicalOptions = {},
): Float32Array {
  return addon.masteringRepairDenoiseClassical(samples, sampleRate, options);
}

/** Options for `masteringRepairDeclip`. */
export interface DeclipOptions {
  clipThreshold?: number;
  lpcOrder?: number;
  iterations?: number;
  lpcBlend?: number;
}

/** Algorithms accepted by `masteringRepairDecrackle`. */
export type DecrackleMode = 'median' | 'waveletShrinkage';

/** Options for `masteringRepairDecrackle`. */
export interface DecrackleOptions {
  threshold?: number;
  mode?: DecrackleMode;
  levels?: number;
}

/** Options for `masteringRepairDehum`. */
export interface DehumOptions {
  fundamentalHz?: number;
  harmonics?: number;
  q?: number;
  adaptive?: boolean;
  searchRangeHz?: number;
  adaptation?: number;
  frameSize?: number;
  pllBandwidth?: number;
}

/** Options for `masteringRepairDereverbClassical`. */
export interface DereverbClassicalOptions {
  threshold?: number;
  attenuation?: number;
  nFft?: number;
  hopLength?: number;
  t60Sec?: number;
  lateDelayMs?: number;
  overSubtraction?: number;
  spectralFloor?: number;
  wpeEnabled?: boolean;
  wpeIterations?: number;
  wpeTaps?: number;
  wpeStrength?: number;
}

/** Trimming modes accepted by `masteringRepairTrimSilence`. */
export type TrimSilenceMode = 'peak' | 'lufsGated';

/** Options for `masteringRepairTrimSilence`. */
export interface TrimSilenceOptions {
  threshold?: number;
  paddingSamples?: number;
  mode?: TrimSilenceMode;
  gateLufs?: number;
  windowMs?: number;
}

/** Offline LPC-based declipper. */
export function masteringRepairDeclip(
  samples: Float32Array,
  sampleRate = 22050,
  options: DeclipOptions = {},
): Float32Array {
  return addon.masteringRepairDeclip(samples, sampleRate, options);
}

/** Offline crackle suppressor (median or wavelet-shrinkage). */
export function masteringRepairDecrackle(
  samples: Float32Array,
  sampleRate = 22050,
  options: DecrackleOptions = {},
): Float32Array {
  return addon.masteringRepairDecrackle(samples, sampleRate, options);
}

/** Offline mains-hum remover. */
export function masteringRepairDehum(
  samples: Float32Array,
  sampleRate = 22050,
  options: DehumOptions = {},
): Float32Array {
  return addon.masteringRepairDehum(samples, sampleRate, options);
}

/** Offline classical dereverberator (spectral subtraction + optional WPE). */
export function masteringRepairDereverbClassical(
  samples: Float32Array,
  sampleRate = 22050,
  options: DereverbClassicalOptions = {},
): Float32Array {
  return addon.masteringRepairDereverbClassical(samples, sampleRate, options);
}

/** Offline silence trimmer (peak threshold or LUFS-gated). */
export function masteringRepairTrimSilence(
  samples: Float32Array,
  sampleRate = 22050,
  options: TrimSilenceOptions = {},
): Float32Array {
  return addon.masteringRepairTrimSilence(samples, sampleRate, options);
}

/** Detector mode for `masteringDynamicsCompressor`. */
export type CompressorDetector = 'peak' | 'rms' | 'log_rms' | 'logRms' | 0 | 1 | 2;

/** Options for `masteringDynamicsCompressor`. */
export interface CompressorOptions extends ValidateOptions {
  thresholdDb?: number;
  ratio?: number;
  attackMs?: number;
  releaseMs?: number;
  kneeDb?: number;
  makeupGainDb?: number;
  autoMakeup?: boolean;
  detector?: CompressorDetector;
  sidechainHpfEnabled?: boolean;
  sidechainHpfHz?: number;
  pdrTimeMs?: number;
  pdrReleaseScale?: number;
}

/** Options for `masteringDynamicsGate`. */
export interface GateOptions extends ValidateOptions {
  thresholdDb?: number;
  attackMs?: number;
  releaseMs?: number;
  rangeDb?: number;
  holdMs?: number;
  closeThresholdDb?: number;
  keyHpfHz?: number;
}

/** Options for `masteringDynamicsTransientShaper`. */
export interface TransientShaperOptions extends ValidateOptions {
  attackGainDb?: number;
  sustainGainDb?: number;
  fastAttackMs?: number;
  fastReleaseMs?: number;
  slowAttackMs?: number;
  slowReleaseMs?: number;
  sensitivity?: number;
  maxGainDb?: number;
  gainSmoothingMs?: number;
  lookaheadMs?: number;
}

/** Result of an offline dynamics processor call. */
export interface DynamicsProcessorResult {
  samples: Float32Array;
  latencySamples: number;
}

/** Offline feed-forward compressor (soft-knee, optional makeup, sidechain HPF, PDR). */
export function masteringDynamicsCompressor(
  samples: Float32Array,
  sampleRate = 22050,
  options: CompressorOptions = {},
): DynamicsProcessorResult {
  assertSamples('masteringDynamicsCompressor', samples, options.validate !== false);
  return addon.masteringDynamicsCompressor(samples, sampleRate, options);
}

/** Offline noise gate with hysteresis, hold, and optional key HPF. */
export function masteringDynamicsGate(
  samples: Float32Array,
  sampleRate = 22050,
  options: GateOptions = {},
): DynamicsProcessorResult {
  assertSamples('masteringDynamicsGate', samples, options.validate !== false);
  return addon.masteringDynamicsGate(samples, sampleRate, options);
}

/** Offline transient shaper (envelope-difference attack/sustain control). */
export function masteringDynamicsTransientShaper(
  samples: Float32Array,
  sampleRate = 22050,
  options: TransientShaperOptions = {},
): DynamicsProcessorResult {
  assertSamples('masteringDynamicsTransientShaper', samples, options.validate !== false);
  return addon.masteringDynamicsTransientShaper(samples, sampleRate, options);
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
  nMfcc = 20,
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

/** NMF of a flattened [nFeatures x nFrames] spectrogram (librosa.decompose.decompose). */
export function decompose(
  s: Float32Array,
  nFeatures: number,
  nFrames: number,
  nComponents: number,
  nIter = 50,
  beta = 2.0,
): { w: Matrix2D; h: Matrix2D } {
  return addon.decompose(s, nFeatures, nFrames, nComponents, nIter, beta);
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

/** One contiguous run of clipped samples reported by `meteringDetectClipping`. */
export interface ClippingRegion {
  startSample: number;
  endSample: number;
  length: number;
  peak: number;
}

/** Aggregated clipping report (mirrors C SonareClippingResult). */
export interface ClippingReport {
  clippedSamples: number;
  clippingRatio: number;
  maxClippedPeak: number;
  regions: ClippingRegion[];
}

/** Sliding-window dynamic range report (mirrors C SonareDynamicRangeResult). */
export interface DynamicRangeReport {
  dynamicRangeDb: number;
  lowPercentileDb: number;
  highPercentileDb: number;
  windowRmsDb: Float32Array;
}

export function meteringPeakDb(
  samples: Float32Array,
  sampleRate = 22050,
  options: ValidateOptions = {},
): number {
  assertSamples('meteringPeakDb', samples, options.validate !== false);
  return addon.meteringPeakDb(samples, sampleRate);
}

export function meteringRmsDb(
  samples: Float32Array,
  sampleRate = 22050,
  options: ValidateOptions = {},
): number {
  assertSamples('meteringRmsDb', samples, options.validate !== false);
  return addon.meteringRmsDb(samples, sampleRate);
}

export function meteringCrestFactorDb(
  samples: Float32Array,
  sampleRate = 22050,
  options: ValidateOptions = {},
): number {
  assertSamples('meteringCrestFactorDb', samples, options.validate !== false);
  return addon.meteringCrestFactorDb(samples, sampleRate);
}

export function meteringDcOffset(
  samples: Float32Array,
  sampleRate = 22050,
  options: ValidateOptions = {},
): number {
  assertSamples('meteringDcOffset', samples, options.validate !== false);
  return addon.meteringDcOffset(samples, sampleRate);
}

/**
 * Inter-sample (true) peak in dBFS. `oversampleFactor` must be a power of two
 * in [1, 16]; pass 0 to use the library default (4).
 */
export function meteringTruePeakDb(
  samples: Float32Array,
  sampleRate = 22050,
  oversampleFactor = 4,
  options: ValidateOptions = {},
): number {
  assertSamples('meteringTruePeakDb', samples, options.validate !== false);
  return addon.meteringTruePeakDb(samples, sampleRate, oversampleFactor);
}

/**
 * Detect contiguous runs of clipped samples.
 *
 * @param threshold Linear absolute threshold (default 0.999).
 * @param minRegionSamples Minimum run length to report (default 1).
 */
export function meteringDetectClipping(
  samples: Float32Array,
  sampleRate = 22050,
  threshold = 0.999,
  minRegionSamples = 1,
  options: ValidateOptions = {},
): ClippingReport {
  assertSamples('meteringDetectClipping', samples, options.validate !== false);
  return addon.meteringDetectClipping(samples, sampleRate, threshold, minRegionSamples);
}

/**
 * Sliding-window dynamic range (high_percentile_db - low_percentile_db).
 * Pass 0 for any parameter to use the library default
 * (window=3 s, hop=1 s, low=0.10, high=0.95).
 */
export function meteringDynamicRange(
  samples: Float32Array,
  sampleRate = 22050,
  windowSec = 0,
  hopSec = 0,
  lowPercentile = 0,
  highPercentile = 0,
  options: ValidateOptions = {},
): DynamicRangeReport {
  assertSamples('meteringDynamicRange', samples, options.validate !== false);
  return addon.meteringDynamicRange(
    samples,
    sampleRate,
    windowSec,
    hopSec,
    lowPercentile,
    highPercentile,
  );
}

/** Mid/side vectorscope point series for a (left, right) stereo pair. */
export interface VectorscopeReport {
  mid: Float32Array;
  side: Float32Array;
}

/** Phase-scope (Lissajous) point series plus summary stats. */
export interface PhaseScopeReport {
  mid: Float32Array;
  side: Float32Array;
  radius: Float32Array;
  angleRad: Float32Array;
  correlation: number;
  averageAbsAngleRad: number;
  maxRadius: number;
}

/** Options for `meteringSpectrum`. */
export interface SpectrumOptions {
  /** FFT size. Pass 0 / omit for the library default (2048). */
  nFft?: number;
  /** Apply fractional-octave smoothing to magnitude. */
  applyOctaveSmoothing?: boolean;
  /** Smoothing fraction (e.g. 3 = 1/3-octave). 0 / omit = library default (3). */
  octaveFraction?: number;
  /** Linear reference for the dB conversion. 0 / omit = 1.0. */
  dbRef?: number;
  /** Linear floor used to avoid log(0). 0 / omit = library default. */
  dbAmin?: number;
}

/** Single-frame magnitude / power / dB spectrum returned by `meteringSpectrum`. */
export interface SpectrumReport {
  frequencies: Float32Array;
  magnitude: Float32Array;
  power: Float32Array;
  db: Float32Array;
  nFft: number;
  sampleRate: number;
}

/** Pearson correlation in [-1, 1] between two equal-length channels. */
export function meteringStereoCorrelation(
  left: Float32Array,
  right: Float32Array,
  sampleRate = 22050,
  options: ValidateOptions = {},
): number {
  const validate = options.validate !== false;
  assertSamples('meteringStereoCorrelation', left, validate, 'left');
  assertSamples('meteringStereoCorrelation', right, validate, 'right');
  return addon.meteringStereoCorrelation(left, right, sampleRate);
}

/** Side / mid energy ratio: 0 = pure mono, ~1 = wide stereo. */
export function meteringStereoWidth(
  left: Float32Array,
  right: Float32Array,
  sampleRate = 22050,
  options: ValidateOptions = {},
): number {
  const validate = options.validate !== false;
  assertSamples('meteringStereoWidth', left, validate, 'left');
  assertSamples('meteringStereoWidth', right, validate, 'right');
  return addon.meteringStereoWidth(left, right, sampleRate);
}

/** Per-sample mid/side point series (one entry per input frame). */
export function meteringVectorscope(
  left: Float32Array,
  right: Float32Array,
  sampleRate = 22050,
  options: ValidateOptions = {},
): VectorscopeReport {
  const validate = options.validate !== false;
  assertSamples('meteringVectorscope', left, validate, 'left');
  assertSamples('meteringVectorscope', right, validate, 'right');
  return addon.meteringVectorscope(left, right, sampleRate);
}

/** Phase-scope point series plus summary stats. */
export function meteringPhaseScope(
  left: Float32Array,
  right: Float32Array,
  sampleRate = 22050,
  options: ValidateOptions = {},
): PhaseScopeReport {
  const validate = options.validate !== false;
  assertSamples('meteringPhaseScope', left, validate, 'left');
  assertSamples('meteringPhaseScope', right, validate, 'right');
  return addon.meteringPhaseScope(left, right, sampleRate);
}

/** Single-frame spectrum view (uses the first `nFft` samples of `samples`). */
export function meteringSpectrum(
  samples: Float32Array,
  sampleRate = 22050,
  options?: SpectrumOptions & ValidateOptions,
): SpectrumReport {
  const validate = options?.validate !== false;
  assertSamples('meteringSpectrum', samples, validate);
  return addon.meteringSpectrum(samples, sampleRate, options ?? {});
}

/**
 * Snap a MIDI value to the nearest pitch class enabled by `modeMask`.
 *
 * `modeMask` is a 12-bit mask. For natural C major use `0b101010110101`.
 * `referenceMidi` defaults to A4 (69) when passed as 0.
 */
export function scaleQuantizeMidi(
  root: number,
  modeMask: number,
  midi: number,
  referenceMidi = 0,
): number {
  assertFiniteScalar('scaleQuantizeMidi', midi, 'midi');
  assertFiniteScalar('scaleQuantizeMidi', referenceMidi, 'referenceMidi');
  return addon.scaleQuantizeMidi(root, modeMask, midi, referenceMidi);
}

export function scaleCorrectionSemitones(
  root: number,
  modeMask: number,
  midi: number,
  referenceMidi = 0,
): number {
  assertFiniteScalar('scaleCorrectionSemitones', midi, 'midi');
  assertFiniteScalar('scaleCorrectionSemitones', referenceMidi, 'referenceMidi');
  return addon.scaleCorrectionSemitones(root, modeMask, midi, referenceMidi);
}

export function scalePitchClassEnabled(
  root: number,
  modeMask: number,
  pitchClass: number,
): boolean {
  return addon.scalePitchClassEnabled(root, modeMask, pitchClass);
}

export function resample(samples: Float32Array, srcSr: number, targetSr: number): Float32Array {
  return addon.resample(samples, srcSr, targetSr);
}

export function mixingScenePresetNames(): string[] {
  return addon.mixingScenePresetNames();
}

export function mixingScenePresetJson(presetName: string): string {
  return addon.mixingScenePresetJson(presetName);
}

const PAN_LAW_VALUES: Record<PanLaw, number> = {
  const3dB: 0,
  'const4.5dB': 1,
  const6dB: 2,
  linear0dB: 3,
};

const METER_TAP_VALUES: Record<MeterTap, number> = {
  preFader: 0,
  postFader: 1,
};

const SEND_TIMING_VALUES: Record<SendTiming, number> = {
  preFader: 0,
  postFader: 1,
};

function automationCurveValue(curve: AutomationCurve): number {
  if (curve === 'linear') {
    return 0;
  }
  if (curve === 'exponential') {
    return 1;
  }
  if (curve === 'hold') {
    return 2;
  }
  if (curve === 's-curve') {
    return 3;
  }
  throw new Error(`Invalid automation curve: ${curve}`);
}

function panLawValue(panLaw: PanLaw | number): number {
  if (typeof panLaw === 'number') {
    return panLaw;
  }
  const value = PAN_LAW_VALUES[panLaw];
  if (value === undefined) {
    throw new Error(`Invalid pan law: ${panLaw}`);
  }
  return value;
}

function panModeValue(panMode: PanMode): number {
  if (typeof panMode === 'number') {
    return panMode;
  }
  const mode = panMode.replace(/_/g, '-').toLowerCase();
  if (mode === 'stereo-pan' || mode === 'stereopan' || mode === 'pan') {
    return 1; // SONARE_PAN_MODE_STEREO_PAN
  }
  if (mode === 'dual-pan' || mode === 'dualpan') {
    return 2; // SONARE_PAN_MODE_DUAL_PAN
  }
  return 0; // SONARE_PAN_MODE_BALANCE
}

function meterTapValue(tap: MeterTap | number): number {
  if (typeof tap === 'number') {
    return tap;
  }
  const value = METER_TAP_VALUES[tap];
  if (value === undefined) {
    throw new Error(`Invalid meter tap: ${tap}`);
  }
  return value;
}

function sendTimingValue(timing: SendTiming | number): number {
  if (typeof timing === 'number') {
    return timing;
  }
  const value = SEND_TIMING_VALUES[timing];
  if (value === undefined) {
    throw new Error(`Invalid send timing: ${timing}`);
  }
  return value;
}

/**
 * Scene-based persistent stereo mixer. Built from a scene JSON string, it routes
 * per-strip stereo blocks through a compiled routing graph (sends, buses,
 * inserts) into a stereo master. Strips are addressed by 0-based index or by
 * their string id; the underlying strip handles are never exposed.
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
      automationCurveValue(curve),
    );
  }

  /** Resolve a strip id to its 0-based index, or `null` if not found. */
  stripById(id: string): number | null {
    return this.native.stripById(id);
  }

  /**
   * Add a bus to the mixer topology.
   *
   * @param id - Unique bus id
   * @param role - Bus role (`'master'` | `'aux'` | `'submix'`); defaults to `'aux'`
   *
   * Marks the routing graph dirty; call {@link compile} (or process) to rebuild.
   */
  addBus(id: string, role?: 'master' | 'aux' | 'submix' | string): void {
    this.native.addBus(id, role);
  }

  /** Remove a bus by id. */
  removeBus(id: string): void {
    this.native.removeBus(id);
  }

  /** Number of buses in the mixer topology. */
  busCount(): number {
    return this.native.busCount();
  }

  /**
   * Add a VCA group with the given id and gain offset.
   *
   * @param id - Unique group id
   * @param gainDb - Group gain offset in dB
   * @param members - Strip ids that belong to the group
   */
  addVcaGroup(id: string, gainDb = 0.0, members: string[] = []): void {
    this.native.addVcaGroup(id, gainDb, members);
  }

  /** Remove a VCA group by id. */
  removeVcaGroup(id: string): void {
    this.native.removeVcaGroup(id);
  }

  /** Number of VCA groups in the mixer topology. */
  vcaGroupCount(): number {
    return this.native.vcaGroupCount();
  }

  /** Set a strip's input trim in dB (applied before the channel processing). */
  setInputTrimDb(strip: StripRef, db: number): void {
    this.native.setInputTrimDb(strip, db);
  }

  /** Set a strip's fader gain in dB. */
  setFaderDb(strip: StripRef, db: number): void {
    this.native.setFaderDb(strip, db);
  }

  /** Set a strip's pan position (-1..1) with an optional pan mode. */
  setPan(strip: StripRef, pan: number, panMode: PanMode = 0): void {
    this.native.setPan(strip, pan, panModeValue(panMode));
  }

  /** Set a strip's stereo width (0 = mono, 1 = original, >1 = widened). */
  setWidth(strip: StripRef, width: number): void {
    this.native.setWidth(strip, width);
  }

  /** Set a strip's mute state. */
  setMuted(strip: StripRef, muted: boolean): void {
    this.native.setMuted(strip, muted);
  }

  /** Set a strip's solo state. Takes effect on the next process (no recompile). */
  setSoloed(strip: StripRef, soloed: boolean): void {
    this.native.setSoloed(strip, soloed);
  }

  /** Mark a strip solo-safe so it is never implied-muted by another strip's solo. */
  setSoloSafe(strip: StripRef, soloSafe: boolean): void {
    this.native.setSoloSafe(strip, soloSafe);
  }

  /** Invert the polarity of a strip's left and/or right channel. */
  setPolarityInvert(strip: StripRef, invertLeft: boolean, invertRight: boolean): void {
    this.native.setPolarityInvert(strip, invertLeft, invertRight);
  }

  /** Set a strip's pan law (`'const3dB'` | `'const4.5dB'` | `'const6dB'` | `'linear0dB'`). */
  setPanLaw(strip: StripRef, panLaw: PanLaw | number): void {
    this.native.setPanLaw(strip, panLawValue(panLaw));
  }

  /** Set a per-strip channel delay in samples (recompiled at the next {@link compile}). */
  setChannelDelaySamples(strip: StripRef, delaySamples: number): void {
    this.native.setChannelDelaySamples(strip, delaySamples);
  }

  /** Set a strip's live VCA gain offset in dB (not persisted to the scene JSON). */
  setVcaOffsetDb(strip: StripRef, offsetDb: number): void {
    this.native.setVcaOffsetDb(strip, offsetDb);
  }

  /** Set a strip's independent left/right pan positions (dual-pan mode). */
  setDualPan(strip: StripRef, leftPan: number, rightPan: number): void {
    this.native.setDualPan(strip, leftPan, rightPan);
  }

  /**
   * Add a post-construction send from a strip to a destination bus.
   *
   * @returns The 0-based index of the new send (use with {@link setSendDb} /
   *   {@link scheduleSendAutomation}).
   */
  addSend(
    strip: StripRef,
    sendId: string,
    destinationBusId: string,
    sendDb = 0.0,
    timing: SendTiming | number = 'postFader',
  ): number {
    return this.native.addSend(strip, sendId, destinationBusId, sendDb, sendTimingValue(timing));
  }

  /** Set the send level (dB) of a strip's send addressed by add-order index. */
  setSendDb(strip: StripRef, sendIndex: number, sendDb: number): void {
    this.native.setSendDb(strip, sendIndex, sendDb);
  }

  /** Read a strip's current (post-fader) meter snapshot. */
  stripMeter(strip: StripRef): MixMeterSnapshot {
    return this.native.stripMeter(strip);
  }

  /** Read a strip's meter snapshot at the given tap point (`'preFader'` | `'postFader'`). */
  meterTap(strip: StripRef, tap: MeterTap | number = 'postFader'): MixMeterSnapshot {
    return this.native.meterTap(strip, meterTapValue(tap));
  }

  /** Read up to `maxPoints` of the latest goniometer samples for a strip. */
  readGoniometerLatest(strip: StripRef, maxPoints: number): GoniometerPoint[] {
    return this.native.readGoniometerLatest(strip, maxPoints);
  }

  /** Schedule sample-accurate fader (dB) automation on a strip. */
  scheduleFaderAutomation(
    strip: StripRef,
    samplePos: number,
    faderDb: number,
    curve: AutomationCurve = 'linear',
  ): void {
    this.native.scheduleFaderAutomation(strip, samplePos, faderDb, automationCurveValue(curve));
  }

  /** Schedule sample-accurate pan automation on a strip. */
  schedulePanAutomation(
    strip: StripRef,
    samplePos: number,
    pan: number,
    curve: AutomationCurve = 'linear',
  ): void {
    this.native.schedulePanAutomation(strip, samplePos, pan, automationCurveValue(curve));
  }

  /** Schedule sample-accurate width automation on a strip. */
  scheduleWidthAutomation(
    strip: StripRef,
    samplePos: number,
    width: number,
    curve: AutomationCurve = 'linear',
  ): void {
    this.native.scheduleWidthAutomation(strip, samplePos, width, automationCurveValue(curve));
  }

  /** Schedule sample-accurate send-level (dB) automation on a strip's send. */
  scheduleSendAutomation(
    strip: StripRef,
    sendIndex: number,
    samplePos: number,
    db: number,
    curve: AutomationCurve = 'linear',
  ): void {
    this.native.scheduleSendAutomation(
      strip,
      sendIndex,
      samplePos,
      db,
      automationCurveValue(curve),
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
  AnalysisProgressCallback,
  AnalysisResult,
  AutomationCurve,
  BpmAnalysisResult,
  BpmCandidate,
  Chord,
  ChordAnalysisResult,
  ChordChromaMethod,
  ChromaResult,
  CqtResult,
  DynamicsResult,
  EngineAutomationPoint,
  EngineAutomationPointCurve,
  EngineCaptureStatus,
  EngineClip,
  EngineGraphConnection,
  EngineGraphMix,
  EngineGraphNode,
  EngineGraphNodeType,
  EngineGraphParameterBinding,
  EngineGraphSpec,
  EngineMarker,
  EngineMeterTelemetry,
  EngineMetronomeConfig,
  EngineParameterInfo,
  EngineTelemetry,
  EngineTelemetryError,
  EngineTelemetryType,
  EngineTransportState,
  EqBandInput,
  EqSpectrumSnapshot,
  GoniometerPoint,
  HpssResult,
  InverseMelResult,
  InverseStftResult,
  Key,
  KeyCandidate,
  KeyDetectionOptions,
  KeyMode,
  LufsResult,
  MasteringChainResult,
  MasteringChainStereoResult,
  MasteringResult,
  MasteringStereoResult,
  MelodyPoint,
  MelodyResult,
  MelSpectrogramResult,
  MeterTap,
  MfccResult,
  MixerProcessResult,
  MixMeterSnapshot,
  MixOptions,
  MixResult,
  PanLaw,
  PanMode,
  PitchResult,
  RhythmResult,
  Section,
  SectionTypeOrdinal,
  SendTiming,
  StftDbResult,
  StftResult,
  StreamAnalyzerConfig,
  StreamAnalyzerStats,
  StreamFramesI16,
  StreamFramesSoa,
  StreamFramesU8,
  StreamingPlatform,
  StripRef,
  TimbreFrame,
  TimbreResult,
  TimeSignature,
} from './types.js';
