/**
 * sonare - Audio Analysis Library
 *
 * @example
 * ```typescript
 * import { init, detectBpm, detectKey, analyze } from '@libraz/sonare';
 *
 * await init();
 *
 * // Detect BPM from audio samples
 * const bpm = detectBpm(samples, sampleRate);
 *
 * // Detect musical key
 * const key = detectKey(samples, sampleRate);
 *
 * // Full analysis
 * const result = analyze(samples, sampleRate);
 * ```
 */

import type {
  AcousticResult,
  AnalysisResult,
  AutomationCurve,
  ChordAnalysisResult,
  ChordDetectionOptions,
  ChordQuality,
  ChromaResult,
  CqtResult,
  EqBand,
  EqMatchOptions,
  EqSpectrumSnapshot,
  GoniometerPoint,
  HpssResult,
  Key,
  KeyCandidate,
  KeyDetectionOptions,
  KeyProfileName,
  LufsResult,
  MasteringChainConfig,
  MasteringChainResult,
  MasteringPreset,
  MasteringProcessorParams,
  MasteringResult,
  MasteringStereoChainResult,
  MasteringStereoResult,
  MelodyResult,
  MelPowerResult,
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
  PitchResult,
  Section,
  SectionType,
  SendTiming,
  SoloProcessor,
  StereoAnalysis,
  StftPowerResult,
  StftResult,
  StreamingPlatform,
  StreamingEqualizerConfig,
  TempogramMode,
} from './public_types';
import { KeyProfile as KeyProfileValues, Mode, PitchClass } from './public_types';
import type {
  AnalyzerStats,
  FrameBuffer,
  StreamConfig,
  StreamFramesI16,
  StreamFramesU8,
} from './stream_types';
import type {
  ProgressCallback,
  SonareModule,
  WasmAcousticResult,
  WasmAnalysisResult,
  WasmChordAnalysisResult,
  WasmCyclicTempogramResult,
  WasmEngineAutomationPoint,
  WasmEngineBounceOptions,
  WasmEngineBounceResult,
  WasmEngineCaptureStatus,
  WasmEngineClip,
  WasmEngineFreezeOptions,
  WasmEngineFreezeResult,
  WasmEngineGraphSpec,
  WasmEngineMarker,
  WasmEngineMeterTelemetry,
  WasmEngineMetronomeConfig,
  WasmEngineParameterInfo,
  WasmEngineProcessWithMonitorResult,
  WasmEngineTelemetry,
  WasmEngineTransportState,
  WasmFourierTempogramResult,
  WasmFrameResult,
  WasmKeyCandidateResult,
  WasmNnlsChromaResult,
  WasmRealtimeEngine,
  WasmStreamAnalyzer,
  WasmTempogramResult,
  WasmTrimResult,
} from './wasm_types';

export type {
  AcousticResult,
  AnalysisResult,
  AutomationCurve,
  Beat,
  Chord,
  ChordAnalysisResult,
  ChordDetectionOptions,
  ChromaResult,
  CqtResult,
  Dynamics,
  EqBand,
  EqBandPhase,
  EqBandType,
  EqCoeffMode,
  EqMatchOptions,
  EqSpectrumSnapshot,
  EqStereoPlacement,
  GoniometerPoint,
  HpssResult,
  Key,
  KeyCandidate,
  KeyDetectionOptions,
  KeyProfileName,
  LufsResult,
  MasteringChainConfig,
  MasteringChainResult,
  MasteringPreset,
  MasteringProcessorParams,
  MasteringResult,
  MasteringStereoChainResult,
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
  PairAnalysis,
  PairProcessor,
  PanLaw,
  PanMode,
  PitchResult,
  RhythmFeatures,
  Section,
  SendTiming,
  SoloProcessor,
  StereoAnalysis,
  StftResult,
  StreamingPlatform,
  StreamingEqualizerConfig,
  Timbre,
  TimeSignature,
} from './public_types';
export {
  ChordQuality,
  KeyProfile,
  Mode,
  PitchClass,
  SectionType,
} from './public_types';
export type {
  AnalyzerStats,
  BarChord,
  ChordChange,
  FrameBuffer,
  PatternScore,
  ProgressiveEstimate,
  StreamConfig,
  StreamFramesI16,
  StreamFramesU8,
} from './stream_types';
export type { ProgressCallback } from './wasm_types';

export type EngineClip = WasmEngineClip;
export type EngineParameterInfo = WasmEngineParameterInfo;
export type EngineAutomationPoint = WasmEngineAutomationPoint;
export type EngineMarker = WasmEngineMarker;
export type EngineMetronomeConfig = WasmEngineMetronomeConfig;
export type EngineGraphSpec = WasmEngineGraphSpec;
export type EngineCaptureStatus = WasmEngineCaptureStatus;
export type EngineBounceOptions = WasmEngineBounceOptions;
export type EngineBounceResult = WasmEngineBounceResult;
export type EngineFreezeOptions = WasmEngineFreezeOptions;
export type EngineFreezeResult = WasmEngineFreezeResult;
export type EngineTelemetry = WasmEngineTelemetry;
export type EngineMeterTelemetry = WasmEngineMeterTelemetry;
export type EngineTransportState = WasmEngineTransportState;

export const EXPECTED_ENGINE_ABI_VERSION = 2;

export interface EngineCapabilities {
  engineAbiVersion: number;
  expectedEngineAbiVersion: number;
  abiCompatible: boolean;
  sharedArrayBuffer: boolean;
  atomics: boolean;
  audioWorklet: boolean;
  mode: 'sab' | 'postMessage';
}

export interface MixerRealtimeBuffer {
  leftInputs: Float32Array[];
  rightInputs: Float32Array[];
  outLeft: Float32Array;
  outRight: Float32Array;
  process: (numSamples?: number) => void;
}

function automationCurveCode(curve: AutomationCurve): number {
  switch (curve) {
    case 'linear':
      return 0;
    case 'exponential':
      return 1;
    case 'hold':
      return 2;
    case 's-curve':
      return 3;
    default:
      throw new Error(`Invalid automation curve: ${curve}`);
  }
}

function panLawCode(panLaw: PanLaw | number): number {
  if (typeof panLaw === 'number') {
    return panLaw;
  }
  switch (panLaw) {
    case 'const4.5dB':
      return 1;
    case 'const6dB':
      return 2;
    case 'linear0dB':
      return 3;
    default:
      return 0;
  }
}

function meterTapCode(tap: MeterTap | number): number {
  return tap === 'preFader' || tap === 0 ? 0 : 1;
}

function sendTimingCode(timing: SendTiming | number): number {
  return timing === 'preFader' || timing === 0 ? 0 : 1;
}

// ============================================================================
// Module State
// ============================================================================

let module: SonareModule | null = null;
let initPromise: Promise<void> | null = null;

// ============================================================================
// Initialization
// ============================================================================

/**
 * Initialize the WASM module.
 * Must be called before using any analysis functions.
 *
 * @param options - Optional module configuration
 * @returns Promise that resolves when initialization is complete
 */
export async function init(options?: {
  locateFile?: (path: string, prefix: string) => string;
}): Promise<void> {
  if (module) {
    return;
  }

  if (initPromise) {
    return initPromise;
  }

  initPromise = (async () => {
    try {
      const createModule = (await import('./sonare.js')).default;
      module = await createModule(options);
    } catch (error) {
      initPromise = null;
      throw error;
    }
  })();

  return initPromise;
}

/**
 * Check if the module is initialized.
 */
export function isInitialized(): boolean {
  return module !== null;
}

/**
 * Get the library version.
 */
export function version(): string {
  if (!module) {
    throw new Error('Module not initialized. Call init() first.');
  }
  return module.version();
}

export function engineAbiVersion(): number {
  if (!module) {
    throw new Error('Module not initialized. Call init() first.');
  }
  return module.engineAbiVersion();
}

export function engineCapabilities(): EngineCapabilities {
  const abiVersion = engineAbiVersion();
  const sharedArrayBuffer = typeof globalThis.SharedArrayBuffer === 'function';
  const atomics = typeof globalThis.Atomics === 'object';
  const audioWorklet =
    typeof AudioWorkletNode !== 'undefined' ||
    typeof (globalThis as typeof globalThis & { AudioWorkletProcessor?: unknown })
      .AudioWorkletProcessor !== 'undefined';
  return {
    engineAbiVersion: abiVersion,
    expectedEngineAbiVersion: EXPECTED_ENGINE_ABI_VERSION,
    abiCompatible: abiVersion === EXPECTED_ENGINE_ABI_VERSION,
    sharedArrayBuffer,
    atomics,
    audioWorklet,
    mode: sharedArrayBuffer && atomics ? 'sab' : 'postMessage',
  };
}

export class RealtimeEngine {
  private native: WasmRealtimeEngine;

  constructor(
    sampleRate = 48000,
    maxBlockSize = 128,
    commandCapacity = 1024,
    telemetryCapacity = 1024,
  ) {
    if (!module) {
      throw new Error('Module not initialized. Call init() first.');
    }
    const wasmModule = module;
    const capabilities = engineCapabilities();
    if (!capabilities.abiCompatible) {
      throw new Error(
        `Engine ABI mismatch: wasm=${capabilities.engineAbiVersion}, expected=${capabilities.expectedEngineAbiVersion}`,
      );
    }
    this.native = new module.RealtimeEngine(
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

  /** Queue a sample-accurate parameter change (engine kSetParam). */
  setParameter(paramId: number, value: number, renderFrame = -1): void {
    this.native.setParameter(paramId, value, renderFrame);
  }

  /** Queue a smoothed parameter change (engine kSetParamSmoothed). */
  setParameterSmoothed(paramId: number, value: number, renderFrame = -1): void {
    this.native.setParameterSmoothed(paramId, value, renderFrame);
  }

  /** Read back the current transport state snapshot. */
  getTransportState(): EngineTransportState {
    return this.native.getTransportState();
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
    return Number(this.native.countInEndSample(startSample, bars));
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

  setClips(clips: EngineClip[]): void {
    this.native.setClips(clips);
  }

  clipCount(): number {
    return this.native.clipCount();
  }

  setCaptureBuffer(numChannels: number, capacityFrames: number): void {
    this.native.setCaptureBuffer(numChannels, capacityFrames);
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

  capturedAudio(): Float32Array[] {
    return this.native.capturedAudio();
  }

  process(channels: Float32Array[]): Float32Array[] {
    return this.native.process(channels);
  }

  processWithMonitor(channels: Float32Array[]): WasmEngineProcessWithMonitorResult {
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

  drainMeterTelemetry(maxRecords = 1024): EngineMeterTelemetry[] {
    return this.native.drainMeterTelemetry(maxRecords);
  }

  destroy(): void {
    this.native.delete();
  }
}

// ============================================================================
// Quick API (High-level Analysis)
// ============================================================================

/**
 * Detect BPM from audio samples.
 *
 * @param samples - Audio samples (mono, float32)
 * @param sampleRate - Sample rate in Hz
 * @returns Detected BPM
 */
export function detectBpm(samples: Float32Array, sampleRate: number): number {
  if (!module) {
    throw new Error('Module not initialized. Call init() first.');
  }
  return module.detectBpm(samples, sampleRate);
}

/**
 * Detect musical key from audio samples.
 *
 * @param samples - Audio samples (mono, float32)
 * @param sampleRate - Sample rate in Hz
 * @returns Detected key
 */
export function detectKey(
  samples: Float32Array,
  sampleRate: number,
  options: KeyDetectionOptions = {},
): Key {
  if (!module) {
    throw new Error('Module not initialized. Call init() first.');
  }
  const result = module._detectKeyWithOptions(
    samples,
    sampleRate,
    options.nFft ?? 4096,
    options.hopLength ?? 512,
    options.useHpss ?? false,
    options.loudnessWeighted ?? false,
    options.highPassHz ?? 0,
    keyModeValues(options.modes),
    keyProfileValue(options.profile),
    options.genreHint ?? '',
  );
  return {
    root: result.root as PitchClass,
    mode: result.mode as Mode,
    confidence: result.confidence,
    name: result.name,
    shortName: result.shortName,
  };
}

function convertKeyCandidate(wasm: WasmKeyCandidateResult): KeyCandidate {
  return {
    key: {
      root: wasm.key.root as PitchClass,
      mode: wasm.key.mode as Mode,
      confidence: wasm.key.confidence,
      name: wasm.key.name,
      shortName: wasm.key.shortName,
    },
    correlation: wasm.correlation,
  };
}

function keyModeValues(modes: KeyDetectionOptions['modes'] | undefined): number[] {
  if (!modes) {
    return [];
  }
  if (modes === 'major-minor') {
    return [Mode.Major, Mode.Minor];
  }
  if (modes === 'all' || modes === 'modal') {
    return [
      Mode.Major,
      Mode.Minor,
      Mode.Dorian,
      Mode.Phrygian,
      Mode.Lydian,
      Mode.Mixolydian,
      Mode.Locrian,
    ];
  }
  const names = {
    major: Mode.Major,
    minor: Mode.Minor,
    dorian: Mode.Dorian,
    phrygian: Mode.Phrygian,
    lydian: Mode.Lydian,
    mixolydian: Mode.Mixolydian,
    locrian: Mode.Locrian,
  } as const;
  return modes.map((mode) => (typeof mode === 'number' ? mode : names[mode]));
}

function keyProfileValue(profile: KeyDetectionOptions['profile'] | undefined): number {
  if (profile === undefined) {
    return -1;
  }
  if (typeof profile === 'number') {
    return profile;
  }
  const names: Record<KeyProfileName, number> = {
    ks: KeyProfileValues.KrumhanslSchmuckler,
    krumhansl: KeyProfileValues.KrumhanslSchmuckler,
    temperley: KeyProfileValues.Temperley,
    shaath: KeyProfileValues.Shaath,
    keyfinder: KeyProfileValues.Shaath,
    'faraldo-edmt': KeyProfileValues.FaraldoEDMT,
    edmt: KeyProfileValues.FaraldoEDMT,
    'faraldo-edma': KeyProfileValues.FaraldoEDMA,
    edma: KeyProfileValues.FaraldoEDMA,
    'faraldo-edmm': KeyProfileValues.FaraldoEDMM,
    edmm: KeyProfileValues.FaraldoEDMM,
    'bellman-budge': KeyProfileValues.BellmanBudge,
    bellman: KeyProfileValues.BellmanBudge,
  };
  return names[profile];
}

export function detectKeyCandidates(
  samples: Float32Array,
  sampleRate: number,
  options: KeyDetectionOptions = {},
): KeyCandidate[] {
  if (!module) {
    throw new Error('Module not initialized. Call init() first.');
  }
  return module
    ._detectKeyCandidates(
      samples,
      sampleRate,
      options.nFft ?? 4096,
      options.hopLength ?? 512,
      options.useHpss ?? false,
      options.loudnessWeighted ?? false,
      options.highPassHz ?? 0,
      keyModeValues(options.modes),
      keyProfileValue(options.profile),
      options.genreHint ?? '',
    )
    .map(convertKeyCandidate);
}

/**
 * Detect onset times from audio samples.
 *
 * @param samples - Audio samples (mono, float32)
 * @param sampleRate - Sample rate in Hz
 * @returns Array of onset times in seconds
 */
export function detectOnsets(samples: Float32Array, sampleRate: number): Float32Array {
  if (!module) {
    throw new Error('Module not initialized. Call init() first.');
  }
  return module.detectOnsets(samples, sampleRate);
}

/**
 * Detect beat times from audio samples.
 *
 * @param samples - Audio samples (mono, float32)
 * @param sampleRate - Sample rate in Hz
 * @returns Array of beat times in seconds
 */
export function detectBeats(samples: Float32Array, sampleRate: number): Float32Array {
  if (!module) {
    throw new Error('Module not initialized. Call init() first.');
  }
  return module.detectBeats(samples, sampleRate);
}

/**
 * Detect downbeat times from audio samples.
 *
 * @param samples - Audio samples (mono, float32)
 * @param sampleRate - Sample rate in Hz
 * @returns Array of downbeat times in seconds
 */
export function detectDownbeats(samples: Float32Array, sampleRate: number): Float32Array {
  if (!module) {
    throw new Error('Module not initialized. Call init() first.');
  }
  return module.detectDownbeats(samples, sampleRate);
}

function convertChordAnalysisResult(wasm: WasmChordAnalysisResult): ChordAnalysisResult {
  return {
    chords: wasm.chords.map((c) => ({
      root: c.root as PitchClass,
      bass: c.bass as PitchClass,
      quality: c.quality as ChordQuality,
      start: c.start,
      end: c.end,
      confidence: c.confidence,
      name: c.name,
    })),
  };
}

/**
 * Detect chords from audio samples.
 *
 * @param samples - Audio samples (mono, float32)
 * @param sampleRate - Sample rate in Hz
 * @param options - Optional chord detection settings
 * @returns Detected chord segments
 */
export function detectChords(
  samples: Float32Array,
  sampleRate: number,
  options: ChordDetectionOptions = {},
): ChordAnalysisResult {
  if (!module) {
    throw new Error('Module not initialized. Call init() first.');
  }
  const result = module.detectChords(
    samples,
    sampleRate,
    options.minDuration ?? 0.3,
    options.smoothingWindow ?? 2.0,
    options.threshold ?? 0.5,
    options.useTriadsOnly ?? false,
    options.nFft ?? 2048,
    options.hopLength ?? 512,
    options.useBeatSync ?? true,
    options.useHmm ?? false,
    options.hmmBeamWidth ?? 24,
    options.useKeyContext ?? false,
    options.keyRoot ?? PitchClass.C,
    options.keyMode ?? Mode.Major,
    options.detectInversions ?? false,
    chordChromaMethodValue(options.chromaMethod ?? 'stft'),
  );
  return convertChordAnalysisResult(result);
}

function chordChromaMethodValue(method: 'stft' | 'nnls'): number {
  if (method === 'stft') {
    return 0;
  }
  if (method === 'nnls') {
    return 1;
  }
  throw new Error(`Invalid chord chroma method: ${method}`);
}

// Helper to convert WASM result to typed result
function convertAnalysisResult(wasm: WasmAnalysisResult): AnalysisResult {
  const beatTimes = new Float32Array(wasm.beats.length);
  for (let i = 0; i < wasm.beats.length; i++) {
    beatTimes[i] = wasm.beats[i].time;
  }
  return {
    bpm: wasm.bpm,
    bpmConfidence: wasm.bpmConfidence,
    key: {
      root: wasm.key.root as PitchClass,
      mode: wasm.key.mode as Mode,
      confidence: wasm.key.confidence,
      name: wasm.key.name,
      shortName: wasm.key.shortName,
    },
    timeSignature: wasm.timeSignature,
    beatTimes,
    beats: wasm.beats,
    chords: wasm.chords.map((c) => ({
      root: c.root as PitchClass,
      bass: c.bass as PitchClass,
      quality: c.quality as ChordQuality,
      start: c.start,
      end: c.end,
      confidence: c.confidence,
      name: c.name,
    })),
    sections: wasm.sections.map((s) => ({
      type: s.type as SectionType,
      start: s.start,
      end: s.end,
      energyLevel: s.energyLevel,
      confidence: s.confidence,
      name: s.name,
    })),
    timbre: wasm.timbre,
    dynamics: wasm.dynamics,
    rhythm: wasm.rhythm,
    form: wasm.form,
  };
}

/**
 * Perform complete music analysis.
 *
 * @param samples - Audio samples (mono, float32)
 * @param sampleRate - Sample rate in Hz
 * @returns Complete analysis result
 */
export function analyze(samples: Float32Array, sampleRate: number): AnalysisResult {
  if (!module) {
    throw new Error('Module not initialized. Call init() first.');
  }
  const result = module.analyze(samples, sampleRate);
  return convertAnalysisResult(result);
}

export function analyzeImpulseResponse(
  samples: Float32Array,
  sampleRate: number,
  nOctaveBands = 6,
): AcousticResult {
  if (!module) {
    throw new Error('Module not initialized. Call init() first.');
  }
  const result: WasmAcousticResult = module.analyzeImpulseResponse(
    samples,
    sampleRate,
    nOctaveBands,
  );
  return result;
}

export function detectAcoustic(
  samples: Float32Array,
  sampleRate: number,
  nOctaveBands = 6,
  nThirdOctaveSubbands = 24,
  minDecayDb = 30.0,
  noiseFloorMarginDb = 10.0,
): AcousticResult {
  if (!module) {
    throw new Error('Module not initialized. Call init() first.');
  }
  const result: WasmAcousticResult = module.detectAcoustic(
    samples,
    sampleRate,
    nOctaveBands,
    nThirdOctaveSubbands,
    minDecayDb,
    noiseFloorMarginDb,
  );
  return result;
}

/**
 * Perform complete music analysis with progress reporting.
 *
 * @param samples - Audio samples (mono, float32)
 * @param sampleRate - Sample rate in Hz
 * @param onProgress - Progress callback (progress: 0-1, stage: string)
 * @returns Complete analysis result
 */
export function analyzeWithProgress(
  samples: Float32Array,
  sampleRate: number,
  onProgress: ProgressCallback,
): AnalysisResult {
  if (!module) {
    throw new Error('Module not initialized. Call init() first.');
  }
  const result = module.analyzeWithProgress(samples, sampleRate, onProgress);
  return convertAnalysisResult(result);
}

// ============================================================================
// Effects
// ============================================================================

/**
 * Perform Harmonic-Percussive Source Separation (HPSS).
 *
 * @param samples - Audio samples (mono, float32)
 * @param sampleRate - Sample rate in Hz
 * @param kernelHarmonic - Horizontal median filter size for harmonic (default: 31)
 * @param kernelPercussive - Vertical median filter size for percussive (default: 31)
 * @returns Separated harmonic and percussive components
 */
export function hpss(
  samples: Float32Array,
  sampleRate: number,
  kernelHarmonic = 31,
  kernelPercussive = 31,
): HpssResult {
  if (!module) {
    throw new Error('Module not initialized. Call init() first.');
  }
  return module.hpss(samples, sampleRate, kernelHarmonic, kernelPercussive);
}

/**
 * Extract harmonic component from audio.
 *
 * @param samples - Audio samples (mono, float32)
 * @param sampleRate - Sample rate in Hz
 * @returns Harmonic component
 */
export function harmonic(samples: Float32Array, sampleRate: number): Float32Array {
  if (!module) {
    throw new Error('Module not initialized. Call init() first.');
  }
  return module.harmonic(samples, sampleRate);
}

/**
 * Extract percussive component from audio.
 *
 * @param samples - Audio samples (mono, float32)
 * @param sampleRate - Sample rate in Hz
 * @returns Percussive component
 */
export function percussive(samples: Float32Array, sampleRate: number): Float32Array {
  if (!module) {
    throw new Error('Module not initialized. Call init() first.');
  }
  return module.percussive(samples, sampleRate);
}

/**
 * Time-stretch audio without changing pitch.
 *
 * @param samples - Audio samples (mono, float32)
 * @param sampleRate - Sample rate in Hz
 * @param rate - Time stretch rate (0.5 = double duration, 2.0 = half duration)
 * @returns Time-stretched audio
 */
export function timeStretch(samples: Float32Array, sampleRate: number, rate: number): Float32Array {
  if (!module) {
    throw new Error('Module not initialized. Call init() first.');
  }
  return module.timeStretch(samples, sampleRate, rate);
}

/**
 * Pitch-shift audio without changing duration.
 *
 * @param samples - Audio samples (mono, float32)
 * @param sampleRate - Sample rate in Hz
 * @param semitones - Pitch shift in semitones (+12 = one octave up, -12 = one octave down)
 * @returns Pitch-shifted audio
 */
export function pitchShift(
  samples: Float32Array,
  sampleRate: number,
  semitones: number,
): Float32Array {
  if (!module) {
    throw new Error('Module not initialized. Call init() first.');
  }
  return module.pitchShift(samples, sampleRate, semitones);
}

/**
 * Pitch-correct audio from a current MIDI note to a target MIDI note.
 *
 * @param samples - Audio samples (mono, float32)
 * @param sampleRate - Sample rate in Hz
 * @param currentMidi - Detected/current MIDI note number
 * @param targetMidi - Desired MIDI note number
 * @returns Pitch-corrected audio
 */
export function pitchCorrectToMidi(
  samples: Float32Array,
  sampleRate: number,
  currentMidi: number,
  targetMidi: number,
): Float32Array {
  if (!module) {
    throw new Error('Module not initialized. Call init() first.');
  }
  return module.pitchCorrectToMidi(samples, sampleRate, currentMidi, targetMidi);
}

/**
 * Time-stretch a note region between two sample offsets without changing pitch.
 *
 * @param samples - Audio samples (mono, float32)
 * @param sampleRate - Sample rate in Hz
 * @param onsetSample - Note onset position in samples
 * @param offsetSample - Note offset position in samples
 * @param stretchRatio - Stretch ratio (0.5 = double duration, 2.0 = half duration)
 * @returns Audio with the note region stretched
 */
export function noteStretch(
  samples: Float32Array,
  sampleRate: number,
  onsetSample: number,
  offsetSample: number,
  stretchRatio: number,
): Float32Array {
  if (!module) {
    throw new Error('Module not initialized. Call init() first.');
  }
  return module.noteStretch(samples, sampleRate, onsetSample, offsetSample, stretchRatio);
}

/**
 * Apply a voice change by shifting pitch and formants independently.
 *
 * @param samples - Audio samples (mono, float32)
 * @param sampleRate - Sample rate in Hz
 * @param pitchSemitones - Pitch shift in semitones
 * @param formantFactor - Formant scaling factor (1.0 = unchanged)
 * @returns Voice-changed audio
 */
export function voiceChange(
  samples: Float32Array,
  sampleRate: number,
  pitchSemitones: number,
  formantFactor: number,
): Float32Array {
  if (!module) {
    throw new Error('Module not initialized. Call init() first.');
  }
  return module.voiceChange(samples, sampleRate, pitchSemitones, formantFactor);
}

/**
 * Normalize audio to target peak level.
 *
 * @param samples - Audio samples (mono, float32)
 * @param sampleRate - Sample rate in Hz
 * @param targetDb - Target peak level in dB (default: 0 dB = full scale)
 * @returns Normalized audio
 */
export function normalize(samples: Float32Array, sampleRate: number, targetDb = 0.0): Float32Array {
  if (!module) {
    throw new Error('Module not initialized. Call init() first.');
  }
  return module.normalize(samples, sampleRate, targetDb);
}

/**
 * Apply mastering loudness normalization with a true-peak ceiling.
 *
 * @param samples - Audio samples (mono, float32)
 * @param sampleRate - Sample rate in Hz
 * @param targetLufs - Target integrated LUFS (default: -14)
 * @param ceilingDb - True/sample peak ceiling in dBFS (default: -1)
 * @param truePeakOversample - Oversampling factor used for peak estimation
 * @returns Processed audio and loudness metadata
 */
export function mastering(
  samples: Float32Array,
  sampleRate: number,
  targetLufs = -14.0,
  ceilingDb = -1.0,
  truePeakOversample = 4,
): MasteringResult {
  if (!module) {
    throw new Error('Module not initialized. Call init() first.');
  }
  return module.mastering(samples, sampleRate, targetLufs, ceilingDb, truePeakOversample);
}

export function masteringProcessorNames(): SoloProcessor[] {
  if (!module) {
    throw new Error('Module not initialized. Call init() first.');
  }
  return module.masteringProcessorNames() as SoloProcessor[];
}

export function masteringPairProcessorNames(): PairProcessor[] {
  if (!module) {
    throw new Error('Module not initialized. Call init() first.');
  }
  return module.masteringPairProcessorNames() as PairProcessor[];
}

export function masteringPairAnalysisNames(): PairAnalysis[] {
  if (!module) {
    throw new Error('Module not initialized. Call init() first.');
  }
  return module.masteringPairAnalysisNames() as PairAnalysis[];
}

export function masteringStereoAnalysisNames(): StereoAnalysis[] {
  if (!module) {
    throw new Error('Module not initialized. Call init() first.');
  }
  return module.masteringStereoAnalysisNames() as StereoAnalysis[];
}

export function masteringProcess(
  processorName: SoloProcessor,
  samples: Float32Array,
  sampleRate: number,
  params: MasteringProcessorParams = {},
): MasteringResult {
  if (!module) {
    throw new Error('Module not initialized. Call init() first.');
  }
  return module.masteringProcess(processorName, samples, sampleRate, params);
}

export function masteringProcessStereo(
  processorName: SoloProcessor,
  left: Float32Array,
  right: Float32Array,
  sampleRate: number,
  params: MasteringProcessorParams = {},
): MasteringStereoResult {
  if (!module) {
    throw new Error('Module not initialized. Call init() first.');
  }
  if (left.length !== right.length) {
    throw new Error('Stereo channel lengths must match.');
  }
  return module.masteringProcessStereo(processorName, left, right, sampleRate, params);
}

export function masteringPairProcess(
  processorName: PairProcessor,
  source: Float32Array,
  reference: Float32Array,
  sampleRate: number,
  params: MasteringProcessorParams = {},
): MasteringResult {
  if (!module) {
    throw new Error('Module not initialized. Call init() first.');
  }
  return module.masteringPairProcess(processorName, source, reference, sampleRate, params);
}

export function masteringPairAnalyze(
  analysisName: PairAnalysis,
  source: Float32Array,
  reference: Float32Array,
  sampleRate: number,
  params: MasteringProcessorParams = {},
): string {
  if (!module) {
    throw new Error('Module not initialized. Call init() first.');
  }
  return module.masteringPairAnalyze(analysisName, source, reference, sampleRate, params);
}

export function masteringStereoAnalyze(
  analysisName: StereoAnalysis,
  left: Float32Array,
  right: Float32Array,
  sampleRate: number,
  params: MasteringProcessorParams = {},
): string {
  if (!module) {
    throw new Error('Module not initialized. Call init() first.');
  }
  return module.masteringStereoAnalyze(analysisName, left, right, sampleRate, params);
}

export function masteringAssistantSuggest(
  samples: Float32Array,
  sampleRate: number,
  params: MasteringProcessorParams = {},
): string {
  if (!module) {
    throw new Error('Module not initialized. Call init() first.');
  }
  return module.masteringAssistantSuggest(samples, sampleRate, params);
}

export function masteringAudioProfile(
  samples: Float32Array,
  sampleRate: number,
  params: MasteringProcessorParams = {},
): string {
  if (!module) {
    throw new Error('Module not initialized. Call init() first.');
  }
  return module.masteringAudioProfile(samples, sampleRate, params);
}

export function masteringStreamingPreview(
  samples: Float32Array,
  sampleRate: number,
  platforms: StreamingPlatform[] = [],
): string {
  if (!module) {
    throw new Error('Module not initialized. Call init() first.');
  }
  return module.masteringStreamingPreview(samples, sampleRate, platforms);
}

/**
 * Apply a configurable mastering chain in WASM.
 *
 * @param samples - Audio samples (mono, float32)
 * @param sampleRate - Sample rate in Hz
 * @param config - Chain stage configuration
 * @returns Processed audio, loudness metadata, and applied stage names
 */
export function masteringChain(
  samples: Float32Array,
  sampleRate: number,
  config: MasteringChainConfig,
): MasteringChainResult {
  if (!module) {
    throw new Error('Module not initialized. Call init() first.');
  }
  return module.masteringChain(samples, sampleRate, config as Record<string, unknown>);
}

/**
 * Apply a configurable stereo mastering chain in WASM.
 *
 * @param left - Left channel samples
 * @param right - Right channel samples
 * @param sampleRate - Sample rate in Hz
 * @param config - Chain stage configuration
 * @returns Processed stereo audio, loudness metadata, and applied stage names
 */
export function masteringChainStereo(
  left: Float32Array,
  right: Float32Array,
  sampleRate: number,
  config: MasteringChainConfig,
): MasteringStereoChainResult {
  if (!module) {
    throw new Error('Module not initialized. Call init() first.');
  }
  if (left.length !== right.length) {
    throw new Error('Stereo channel lengths must match.');
  }
  return module.masteringChainStereo(left, right, sampleRate, config as Record<string, unknown>);
}

/**
 * Apply a configurable mastering chain in WASM with progress reporting.
 *
 * @param samples - Audio samples (mono, float32)
 * @param sampleRate - Sample rate in Hz
 * @param config - Chain stage configuration
 * @param onProgress - Progress callback (progress: 0-1, stage: string)
 * @returns Processed audio, loudness metadata, and applied stage names
 */
export function masteringChainWithProgress(
  samples: Float32Array,
  sampleRate: number,
  config: MasteringChainConfig,
  onProgress: ProgressCallback,
): MasteringChainResult {
  if (!module) {
    throw new Error('Module not initialized. Call init() first.');
  }
  return module.masteringChainWithProgress(
    samples,
    sampleRate,
    config as Record<string, unknown>,
    onProgress,
  );
}

/**
 * Apply a configurable stereo mastering chain in WASM with progress reporting.
 *
 * @param left - Left channel samples
 * @param right - Right channel samples
 * @param sampleRate - Sample rate in Hz
 * @param config - Chain stage configuration
 * @param onProgress - Progress callback (progress: 0-1, stage: string)
 * @returns Processed stereo audio, loudness metadata, and applied stage names
 */
export function masteringChainStereoWithProgress(
  left: Float32Array,
  right: Float32Array,
  sampleRate: number,
  config: MasteringChainConfig,
  onProgress: ProgressCallback,
): MasteringStereoChainResult {
  if (!module) {
    throw new Error('Module not initialized. Call init() first.');
  }
  if (left.length !== right.length) {
    throw new Error('Stereo channel lengths must match.');
  }
  return module.masteringChainStereoWithProgress(
    left,
    right,
    sampleRate,
    config as Record<string, unknown>,
    onProgress,
  );
}

/**
 * List built-in mastering preset identifiers.
 *
 * @returns Preset names in display order (e.g. "pop", "edm", "aiMusic")
 */
export function masteringPresetNames(): MasteringPreset[] {
  if (!module) {
    throw new Error('Module not initialized. Call init() first.');
  }
  return module.masteringPresetNames() as MasteringPreset[];
}

/**
 * Apply a named mastering preset chain to mono audio.
 *
 * @param samples - Audio samples (mono, float32)
 * @param sampleRate - Sample rate in Hz
 * @param presetName - Preset identifier from {@link masteringPresetNames}
 * @param overrides - Optional flat overrides (dot-notation, e.g. `'loudness.targetLufs'`) applied on top of the preset. Pass `null` for preset defaults.
 * @returns Processed audio, loudness metadata, and applied stage names
 */
export function masterAudio(
  samples: Float32Array,
  sampleRate: number,
  presetName: MasteringPreset,
  overrides: Record<string, number | boolean> | null = null,
): MasteringChainResult {
  if (!module) {
    throw new Error('Module not initialized. Call init() first.');
  }
  return module.masterAudio(presetName, samples, sampleRate, overrides);
}

/**
 * Apply a named mastering preset chain to stereo audio.
 *
 * @param left - Left channel samples
 * @param right - Right channel samples
 * @param sampleRate - Sample rate in Hz
 * @param presetName - Preset identifier from {@link masteringPresetNames}
 * @param overrides - Optional flat overrides (dot-notation, e.g. `'loudness.targetLufs'`) applied on top of the preset. Pass `null` for preset defaults.
 * @returns Processed stereo audio, loudness metadata, and applied stage names
 */
export function masterAudioStereo(
  left: Float32Array,
  right: Float32Array,
  sampleRate: number,
  presetName: MasteringPreset,
  overrides: Record<string, number | boolean> | null = null,
): MasteringStereoChainResult {
  if (!module) {
    throw new Error('Module not initialized. Call init() first.');
  }
  if (left.length !== right.length) {
    throw new Error('Stereo channel lengths must match.');
  }
  return module.masterAudioStereo(presetName, left, right, sampleRate, overrides);
}

export function mixingScenePresetNames(): string[] {
  if (!module) {
    throw new Error('Module not initialized. Call init() first.');
  }
  return module.mixingScenePresetNames();
}

/**
 * Get a built-in mixing scene preset serialized as JSON. This is the canonical
 * name shared with the Node and Python bindings; the returned JSON loads
 * directly into a {@link Mixer} via {@link Mixer.fromSceneJson}.
 *
 * @param preset - Preset name (see {@link mixingScenePresetNames})
 * @returns Scene JSON string
 */
export function mixingScenePresetJson(preset: string): string {
  if (!module) {
    throw new Error('Module not initialized. Call init() first.');
  }
  return module.mixingScenePresetJson(preset);
}

export function mixStereo(
  leftChannels: Float32Array[],
  rightChannels: Float32Array[],
  sampleRate = 48000,
  options: MixOptions = {},
): MixResult {
  if (!module) {
    throw new Error('Module not initialized. Call init() first.');
  }
  if (leftChannels.length === 0 || leftChannels.length !== rightChannels.length) {
    throw new Error('leftChannels and rightChannels must have the same non-zero length.');
  }
  return module.mixStereo(
    leftChannels,
    rightChannels,
    sampleRate,
    options as Record<string, unknown>,
  );
}

// ============================================================================
// StreamingMasteringChain Class
// ============================================================================

/**
 * Block-by-block streaming variant of {@link masteringChain}.
 *
 * Maintains processor state across {@link processMono}/{@link processStereo}
 * calls. Only ProcessorBase-backed stages are supported. Configurations that
 * enable `repair.denoise` or `loudness` throw at construction.
 *
 * Call {@link delete} (or use a `try/finally`) to release the underlying WASM
 * object — the embind handle is not garbage-collected automatically.
 *
 * @example
 * ```typescript
 * const chain = new StreamingMasteringChain({ eq: { tiltDb: 1.0 } });
 * try {
 *   chain.prepare(44100, 512, 1);
 *   const out = chain.processMono(blockSamples);
 * } finally {
 *   chain.delete();
 * }
 * ```
 */
export class StreamingMasteringChain {
  private chain: import('./wasm_types').WasmStreamingMasteringChain;

  constructor(config: MasteringChainConfig) {
    if (!module) {
      throw new Error('Module not initialized. Call init() first.');
    }
    this.chain = module.createStreamingMasteringChain(config as Record<string, unknown>);
  }

  /**
   * Initialize processors for the given sample rate and block layout.
   *
   * @param sampleRate - Sample rate in Hz
   * @param maxBlockSize - Maximum block size per process call
   * @param numChannels - 1 (mono) or 2 (stereo)
   */
  prepare(sampleRate: number, maxBlockSize: number, numChannels: number): void {
    this.chain.prepare(sampleRate, maxBlockSize, numChannels);
  }

  /**
   * Process one mono block, returning the processed samples (same length).
   */
  processMono(samples: Float32Array): Float32Array {
    return this.chain.processMono(samples);
  }

  /**
   * Process one stereo block, returning the processed channels.
   */
  processStereo(
    left: Float32Array,
    right: Float32Array,
  ): { left: Float32Array; right: Float32Array } {
    if (left.length !== right.length) {
      throw new Error('Stereo channel lengths must match.');
    }
    return this.chain.processStereo(left, right);
  }

  /** Reset all processor state without rebuilding. */
  reset(): void {
    this.chain.reset();
  }

  /** Total reported latency in samples across all active processors. */
  latencySamples(): number {
    return this.chain.latencySamples();
  }

  /** Ordered stage names that will run (e.g. `"eq.tilt"`). */
  stageNames(): string[] {
    return this.chain.stageNames();
  }

  /** Release the underlying WASM object. Safe to call only once. */
  delete(): void {
    this.chain.delete();
  }
}

// ============================================================================
// StreamingEqualizer Class
// ============================================================================

/**
 * Block-by-block streaming equalizer wrapping the unified C++
 * `EqualizerProcessor` (up to 24 bands, RBJ/Vicanek biquads, dynamic EQ,
 * linear-phase FIR, mid/side processing, and auto-gain).
 *
 * State is maintained across {@link processMono}/{@link processStereo} calls.
 * Call {@link delete} (or use `try/finally`) to release the underlying WASM
 * object — the embind handle is not garbage-collected automatically.
 *
 * @example
 * ```typescript
 * const eq = new StreamingEqualizer({ sampleRate: 48000, maxBlockSize: 512 });
 * try {
 *   eq.setBand(0, { type: 'HighShelf', frequencyHz: 8000, gainDb: 6, enabled: true });
 *   const out = eq.processStereo(left, right);
 *   const snapshot = eq.spectrum();
 * } finally {
 *   eq.delete();
 * }
 * ```
 */
export class StreamingEqualizer {
  private eq: import('./wasm_types').WasmStreamingEqualizer;

  constructor(config: StreamingEqualizerConfig = {}) {
    if (!module) {
      throw new Error('Module not initialized. Call init() first.');
    }
    this.eq = module.createEqualizer(config as Record<string, unknown>);
  }

  /**
   * Configure the band at `index` (0..23). Omitted fields use C++ defaults.
   */
  setBand(index: number, band: EqBand): void {
    this.eq.setBand(index, band as Record<string, unknown>);
  }

  /** Disable and reset every band. */
  clear(): void {
    this.eq.clear();
  }

  /**
   * Set the global phase mode: 1=ZeroLatency, 2=NaturalPhase, 3=LinearPhase.
   */
  setPhaseMode(mode: number): void {
    this.eq.setPhaseMode(mode);
  }

  /** Enable or disable output auto-gain compensation. */
  setAutoGain(enabled: boolean): void {
    this.eq.setAutoGain(enabled);
  }

  /** Set all-band EQ gain scale as a 0.0..2.0 multiplier. */
  setGainScale(scale: number): void {
    this.eq.setGainScale(scale);
  }

  /** Set post-EQ output gain in dB. */
  setOutputGainDb(gainDb: number): void {
    this.eq.setOutputGainDb(gainDb);
  }

  /** Set post-EQ stereo balance in -1.0..1.0; mono input ignores pan. */
  setOutputPan(pan: number): void {
    this.eq.setOutputPan(pan);
  }

  /**
   * Provide a mono external sidechain key for dynamic bands that opt into
   * `external_sidechain`. The samples are copied into an owned buffer.
   */
  setSidechainMono(samples: Float32Array): void {
    this.eq.setSidechainMono(samples);
  }

  /**
   * Provide a stereo external sidechain key. Both channels must match length.
   */
  setSidechainStereo(left: Float32Array, right: Float32Array): void {
    if (left.length !== right.length) {
      throw new Error('Sidechain channel lengths must match.');
    }
    this.eq.setSidechainStereo(left, right);
  }

  /** Release any borrowed external sidechain buffers. */
  clearSidechain(): void {
    this.eq.clearSidechain();
  }

  /** Auto-gain applied on the most recent block, in dB. */
  lastAutoGainDb(): number {
    return this.eq.lastAutoGainDb();
  }

  /** Reported processing latency in samples (non-zero for linear-phase bands). */
  latencySamples(): number {
    return this.eq.latencySamples();
  }

  /**
   * Process one mono block, returning the equalized samples (same length).
   */
  processMono(samples: Float32Array): Float32Array {
    return this.eq.processMono(samples);
  }

  /**
   * Process one stereo block, returning the equalized channels.
   */
  processStereo(
    left: Float32Array,
    right: Float32Array,
  ): { left: Float32Array; right: Float32Array } {
    if (left.length !== right.length) {
      throw new Error('Stereo channel lengths must match.');
    }
    return this.eq.processStereo(left, right);
  }

  /**
   * Read the latest pre/post spectrum snapshot for metering. `seq` increments
   * each time a new snapshot is published.
   */
  spectrum(): EqSpectrumSnapshot {
    return this.eq.spectrum();
  }

  /**
   * Configure bands so the source spectrum matches the reference spectrum.
   *
   * @param source - Source audio (mono samples)
   * @param reference - Reference audio (mono samples)
   * @param options - `sampleRate` (default 48000) and `maxBands` (default 8)
   */
  match(source: Float32Array, reference: Float32Array, options: EqMatchOptions = {}): void {
    this.eq.match(source, reference, options as Record<string, unknown>);
  }

  /** Release the underlying WASM object. Safe to call only once. */
  delete(): void {
    this.eq.delete();
  }
}

// ============================================================================
// Mixer Class (scene-based persistent mixer)
// ============================================================================

/**
 * Get a built-in mixing scene preset serialized as JSON, normalized through the
 * C mixer API (the same path {@link Mixer.fromSceneJson} uses to load it).
 *
 * @deprecated Use {@link mixingScenePresetJson}, the canonical name shared with
 * the Node and Python bindings. This alias is retained for backwards
 * compatibility and may be removed in a future release. Both functions return a
 * scene JSON string that loads cleanly into a {@link Mixer}.
 *
 * @param preset - Preset name (see {@link mixingScenePresetNames})
 * @returns Scene JSON string
 */
export function mixerScenePresetJson(preset: string): string {
  if (!module) {
    throw new Error('Module not initialized. Call init() first.');
  }
  return module.mixerPresetJson(preset);
}

/**
 * Persistent, scene-based stereo mixer.
 *
 * Build one from a scene JSON string (e.g. {@link mixerScenePresetJson} or a
 * hand-authored scene), then feed per-strip stereo blocks through
 * {@link processStereo} to get the routed stereo master. Strips, sends, buses,
 * and inserts are described entirely by the scene; the routing graph is
 * compiled lazily on the first {@link processStereo} call (or eagerly via
 * {@link compile}).
 *
 * Call {@link delete} (or use a `try/finally`) to release the underlying WASM
 * object — the embind handle is not garbage-collected automatically.
 *
 * @example
 * ```typescript
 * const mixer = Mixer.fromSceneJson(mixerScenePresetJson('basicStereo'), 48000, 512);
 * try {
 *   const out = mixer.processStereo([stripL], [stripR]);
 * } finally {
 *   mixer.delete();
 * }
 * ```
 */
export class Mixer {
  private mixer: import('./wasm_types').WasmMixer;

  private constructor(mixer: import('./wasm_types').WasmMixer) {
    this.mixer = mixer;
  }

  /**
   * Build a mixer from a scene JSON string.
   *
   * @param json - Scene JSON (strips, buses, sends, connections, inserts)
   * @param sampleRate - Sample rate in Hz (default: 48000)
   * @param blockSize - Maximum block size per {@link processStereo} call (default: 512)
   */
  static fromSceneJson(json: string, sampleRate = 48000, blockSize = 512): Mixer {
    if (!module) {
      throw new Error('Module not initialized. Call init() first.');
    }
    return new Mixer(module.createMixerFromSceneJson(json, sampleRate, blockSize));
  }

  /** Rebuild and compile the routing graph from the current scene topology. */
  compile(): void {
    this.mixer.compile();
  }

  /**
   * Mix one block of per-strip stereo audio into the stereo master.
   *
   * @param leftChannels - `leftChannels[i]` is the left channel of strip `i`
   * @param rightChannels - `rightChannels[i]` is the right channel of strip `i`
   * @returns Mixed stereo master (`left`, `right`, `sampleRate`)
   */
  processStereo(leftChannels: Float32Array[], rightChannels: Float32Array[]): MixerProcessResult {
    if (leftChannels.length !== rightChannels.length) {
      throw new Error('leftChannels and rightChannels must have the same length.');
    }
    return this.mixer.processStereo(leftChannels, rightChannels);
  }

  /**
   * Mix one block into caller-owned output arrays.
   *
   * This avoids allocating the result object and result `Float32Array`s. It is
   * intended for realtime bridges such as AudioWorklet; the input channel count
   * must match the scene strip count and all arrays must have the same length.
   */
  processStereoInto(
    leftChannels: Float32Array[],
    rightChannels: Float32Array[],
    outLeft: Float32Array,
    outRight: Float32Array,
  ): void {
    if (leftChannels.length !== rightChannels.length) {
      throw new Error('leftChannels and rightChannels must have the same length.');
    }
    if (outLeft.length !== outRight.length) {
      throw new Error('outLeft and outRight must have the same length.');
    }
    this.mixer.processStereoInto(leftChannels, rightChannels, outLeft, outRight);
  }

  /**
   * Create reusable WASM-heap input/output views for realtime-style processing.
   *
   * Fill `leftInputs[i]` / `rightInputs[i]`, call `process()`, then read
   * `outLeft` / `outRight`. The views are owned by this mixer and become invalid
   * after {@link delete}.
   */
  createRealtimeBuffer(): MixerRealtimeBuffer {
    const stripCount = this.stripCount();
    const leftInputs: Float32Array[] = [];
    const rightInputs: Float32Array[] = [];
    for (let index = 0; index < stripCount; index++) {
      leftInputs.push(this.mixer.inputLeftView(index));
      rightInputs.push(this.mixer.inputRightView(index));
    }
    const outLeft = this.mixer.outputLeftView();
    const outRight = this.mixer.outputRightView();
    return {
      leftInputs,
      rightInputs,
      outLeft,
      outRight,
      process: (numSamples = outLeft.length) => this.mixer.processPreparedStereo(numSamples),
    };
  }

  /** Number of strips in the mixer (e.g. strips loaded from the scene). */
  stripCount(): number {
    return this.mixer.stripCount();
  }

  /**
   * Schedule sample-accurate insert-parameter automation on a strip's insert.
   *
   * @param stripIndex - Strip index in `[0, stripCount())`
   * @param insertIndex - Index into the strip's combined insert sequence
   *   (`[pre-inserts... post-inserts...]`)
   * @param paramId - Processor-specific parameter id
   * @param samplePos - Absolute samples from the start of processing (the mixer
   *   advances an internal position from 0 on the first {@link processStereo}
   *   call; recompiling resets it to 0)
   * @param value - Target parameter value
   * @param curve - Interpolation curve (default: `'linear'`)
   * @throws If the strip index is out of range or the schedule call fails
   *   (unknown curve, out-of-range insert index, or full event lane)
   */
  scheduleInsertAutomation(
    stripIndex: number,
    insertIndex: number,
    paramId: number,
    samplePos: number,
    value: number,
    curve: AutomationCurve = 'linear',
  ): void {
    this.mixer.scheduleInsertAutomation(
      stripIndex,
      insertIndex,
      paramId,
      samplePos,
      value,
      automationCurveCode(curve),
    );
  }

  /**
   * Resolve a strip's index in `[0, stripCount())` from its scene id, or `null`
   * when no strip with that id exists (matches the Node binding's `number | null`).
   */
  stripById(id: string): number | null {
    const index = this.mixer.stripById(id);
    return index < 0 ? null : index;
  }

  /**
   * Add a bus to the mixer topology. `role` is one of `'master'`, `'aux'`, or
   * `'submix'` (defaults to `'aux'`). Marks the routing graph dirty; call
   * {@link compile} (or {@link processStereo}) to rebuild.
   */
  addBus(id: string, role = 'aux'): void {
    this.mixer.addBus(id, role);
  }

  /** Remove a bus by id. Marks the routing graph dirty. */
  removeBus(id: string): void {
    this.mixer.removeBus(id);
  }

  /** Number of buses in the mixer topology. */
  busCount(): number {
    return this.mixer.busCount();
  }

  /**
   * Add a VCA group with the given gain offset (dB). `members` is a list of
   * strip ids governed by the group (may be empty).
   */
  addVcaGroup(id: string, gainDb = 0.0, members: string[] = []): void {
    this.mixer.addVcaGroup(id, gainDb, members);
  }

  /** Remove a VCA group by id. */
  removeVcaGroup(id: string): void {
    this.mixer.removeVcaGroup(id);
  }

  /** Number of VCA groups in the mixer topology. */
  vcaGroupCount(): number {
    return this.mixer.vcaGroupCount();
  }

  /**
   * Set a strip's solo state. Takes effect on the next process without a
   * graph recompile.
   */
  setSoloed(stripIndex: number, soloed: boolean): void {
    this.mixer.setSoloed(stripIndex, soloed);
  }

  /**
   * Mark a strip solo-safe so it is never implied-muted by another strip's
   * solo. Takes effect on the next process without a graph recompile.
   */
  setSoloSafe(stripIndex: number, soloSafe: boolean): void {
    this.mixer.setSoloSafe(stripIndex, soloSafe);
  }

  /** Invert the polarity of the left and/or right channel of a strip. */
  setPolarityInvert(stripIndex: number, invertLeft: boolean, invertRight: boolean): void {
    this.mixer.setPolarityInvert(stripIndex, invertLeft, invertRight);
  }

  /** Set the strip's pan law. */
  setPanLaw(stripIndex: number, panLaw: PanLaw | number): void {
    this.mixer.setPanLaw(stripIndex, panLawCode(panLaw));
  }

  /**
   * Set a per-strip channel delay in samples. This changes the strip's reported
   * latency; recompile to re-run latency compensation.
   */
  setChannelDelaySamples(stripIndex: number, delaySamples: number): void {
    this.mixer.setChannelDelaySamples(stripIndex, delaySamples);
  }

  /** Set the strip's live VCA gain offset in dB (not persisted to the scene). */
  setVcaOffsetDb(stripIndex: number, offsetDb: number): void {
    this.mixer.setVcaOffsetDb(stripIndex, offsetDb);
  }

  /** Set independent left/right pan positions (dual-pan mode). */
  setDualPan(stripIndex: number, leftPan: number, rightPan: number): void {
    this.mixer.setDualPan(stripIndex, leftPan, rightPan);
  }

  /**
   * Add a send to a strip after construction.
   *
   * @param stripIndex - Strip index in `[0, stripCount())`
   * @param id - Send id
   * @param destinationBusId - Destination bus id
   * @param sendDb - Initial send level in dB
   * @param timing - `'preFader'` or `'postFader'` (default: `'postFader'`)
   * @returns The new send's index
   */
  addSend(
    stripIndex: number,
    id: string,
    destinationBusId: string,
    sendDb: number,
    timing: SendTiming | number = 'postFader',
  ): number {
    return this.mixer.addSend(stripIndex, id, destinationBusId, sendDb, sendTimingCode(timing));
  }

  /** Set the send level (in dB) for an existing send by index. */
  setSendDb(stripIndex: number, sendIndex: number, sendDb: number): void {
    this.mixer.setSendDb(stripIndex, sendIndex, sendDb);
  }

  /**
   * Read a strip's meter snapshot at the given tap point.
   *
   * @param stripIndex - Strip index in `[0, stripCount())`
   * @param tap - `'preFader'` or `'postFader'` (default: `'postFader'`)
   */
  meterTap(stripIndex: number, tap: MeterTap = 'postFader'): MixMeterSnapshot {
    return this.mixer.meterTap(stripIndex, meterTapCode(tap));
  }

  /**
   * Read a strip's meter snapshot. Alias of {@link meterTap}, provided for
   * cross-binding (Node/Python) parity.
   *
   * @param stripIndex - Strip index in `[0, stripCount())`
   * @param tap - `'preFader'` or `'postFader'` (default: `'postFader'`)
   */
  stripMeter(stripIndex: number, tap: MeterTap = 'postFader'): MixMeterSnapshot {
    return this.mixer.stripMeter(stripIndex, meterTapCode(tap));
  }

  /**
   * Schedule sample-accurate fader automation on a strip.
   *
   * @param stripIndex - Strip index in `[0, stripCount())`
   * @param samplePos - Absolute samples from the start of processing
   * @param faderDb - Target fader level in dB
   * @param curve - Interpolation curve (default: `'linear'`)
   */
  scheduleFaderAutomation(
    stripIndex: number,
    samplePos: number,
    faderDb: number,
    curve: AutomationCurve = 'linear',
  ): void {
    this.mixer.scheduleFaderAutomation(stripIndex, samplePos, faderDb, automationCurveCode(curve));
  }

  /**
   * Schedule sample-accurate pan automation on a strip.
   *
   * @param stripIndex - Strip index in `[0, stripCount())`
   * @param samplePos - Absolute samples from the start of processing
   * @param pan - Target pan position
   * @param curve - Interpolation curve (default: `'linear'`)
   */
  schedulePanAutomation(
    stripIndex: number,
    samplePos: number,
    pan: number,
    curve: AutomationCurve = 'linear',
  ): void {
    this.mixer.schedulePanAutomation(stripIndex, samplePos, pan, automationCurveCode(curve));
  }

  /**
   * Schedule sample-accurate width automation on a strip.
   *
   * @param stripIndex - Strip index in `[0, stripCount())`
   * @param samplePos - Absolute samples from the start of processing
   * @param width - Target stereo width
   * @param curve - Interpolation curve (default: `'linear'`)
   */
  scheduleWidthAutomation(
    stripIndex: number,
    samplePos: number,
    width: number,
    curve: AutomationCurve = 'linear',
  ): void {
    this.mixer.scheduleWidthAutomation(stripIndex, samplePos, width, automationCurveCode(curve));
  }

  /**
   * Schedule sample-accurate send-level automation on a strip's send.
   *
   * @param stripIndex - Strip index in `[0, stripCount())`
   * @param sendIndex - Send index in the strip's add order
   * @param samplePos - Absolute samples from the start of processing
   * @param db - Target send level in dB
   * @param curve - Interpolation curve (default: `'linear'`)
   */
  scheduleSendAutomation(
    stripIndex: number,
    sendIndex: number,
    samplePos: number,
    db: number,
    curve: AutomationCurve = 'linear',
  ): void {
    this.mixer.scheduleSendAutomation(
      stripIndex,
      sendIndex,
      samplePos,
      db,
      automationCurveCode(curve),
    );
  }

  /**
   * Read up to `maxPoints` of a strip's most recent goniometer samples
   * (oldest to newest).
   */
  readGoniometerLatest(stripIndex: number, maxPoints: number): GoniometerPoint[] {
    return this.mixer.readGoniometerLatest(stripIndex, maxPoints);
  }

  /** Serialize the current scene (strips, buses, sends, connections) to JSON. */
  toSceneJson(): string {
    return this.mixer.toSceneJson();
  }

  /** Release the underlying WASM object. Safe to call only once. */
  delete(): void {
    this.mixer.delete();
  }

  /** Alias for {@link delete}, provided for cross-binding (Node) compatibility. */
  destroy(): void {
    this.delete();
  }
}

/**
 * Trim silence from beginning and end of audio.
 *
 * @param samples - Audio samples (mono, float32)
 * @param sampleRate - Sample rate in Hz
 * @param thresholdDb - Silence threshold in dB (default: -60 dB)
 * @returns Trimmed audio
 */
export function trim(samples: Float32Array, sampleRate: number, thresholdDb = -60.0): Float32Array {
  if (!module) {
    throw new Error('Module not initialized. Call init() first.');
  }
  return module.trim(samples, sampleRate, thresholdDb);
}

// ============================================================================
// Features - Spectrogram
// ============================================================================

/**
 * Compute Short-Time Fourier Transform (STFT).
 *
 * @param samples - Audio samples (mono, float32)
 * @param sampleRate - Sample rate in Hz
 * @param nFft - FFT size (default: 2048)
 * @param hopLength - Hop length (default: 512)
 * @returns STFT result with magnitude and power spectrograms
 */
export function stft(
  samples: Float32Array,
  sampleRate: number,
  nFft = 2048,
  hopLength = 512,
): StftResult {
  if (!module) {
    throw new Error('Module not initialized. Call init() first.');
  }
  return module.stft(samples, sampleRate, nFft, hopLength);
}

/**
 * Compute STFT and return magnitude in decibels.
 *
 * @param samples - Audio samples (mono, float32)
 * @param sampleRate - Sample rate in Hz
 * @param nFft - FFT size (default: 2048)
 * @param hopLength - Hop length (default: 512)
 * @returns STFT result with dB values
 */
export function stftDb(
  samples: Float32Array,
  sampleRate: number,
  nFft = 2048,
  hopLength = 512,
): { nBins: number; nFrames: number; db: Float32Array } {
  if (!module) {
    throw new Error('Module not initialized. Call init() first.');
  }
  return module.stftDb(samples, sampleRate, nFft, hopLength);
}

// ============================================================================
// Features - Mel Spectrogram
// ============================================================================

/**
 * Compute Mel spectrogram.
 *
 * @param samples - Audio samples (mono, float32)
 * @param sampleRate - Sample rate in Hz
 * @param nFft - FFT size (default: 2048)
 * @param hopLength - Hop length (default: 512)
 * @param nMels - Number of Mel bands (default: 128)
 * @returns Mel spectrogram result
 */
export function melSpectrogram(
  samples: Float32Array,
  sampleRate: number,
  nFft = 2048,
  hopLength = 512,
  nMels = 128,
): MelSpectrogramResult {
  if (!module) {
    throw new Error('Module not initialized. Call init() first.');
  }
  return module.melSpectrogram(samples, sampleRate, nFft, hopLength, nMels);
}

/**
 * Compute MFCC (Mel-Frequency Cepstral Coefficients).
 *
 * @param samples - Audio samples (mono, float32)
 * @param sampleRate - Sample rate in Hz
 * @param nFft - FFT size (default: 2048)
 * @param hopLength - Hop length (default: 512)
 * @param nMels - Number of Mel bands (default: 128)
 * @param nMfcc - Number of MFCC coefficients (default: 13)
 * @returns MFCC result
 */
export function mfcc(
  samples: Float32Array,
  sampleRate: number,
  nFft = 2048,
  hopLength = 512,
  nMels = 128,
  nMfcc = 13,
): MfccResult {
  if (!module) {
    throw new Error('Module not initialized. Call init() first.');
  }
  return module.mfcc(samples, sampleRate, nFft, hopLength, nMels, nMfcc);
}

// ============================================================================
// Features - Inverse reconstruction
// ============================================================================

/**
 * Approximate inverse of a Mel filterbank: Mel power spectrogram -> STFT power
 * spectrogram. Mirrors `feature::mel_to_stft`.
 *
 * @param melPower - Mel power spectrogram [nMels x nFrames] row-major
 * @param nMels - Number of Mel bands
 * @param nFrames - Number of time frames
 * @param sampleRate - Sample rate in Hz
 * @param nFft - FFT size (default: 2048)
 * @param hopLength - Hop length (default: 512)
 * @returns STFT power spectrogram result
 */
export function melToStft(
  melPower: Float32Array,
  nMels: number,
  nFrames: number,
  sampleRate: number,
  nFft = 2048,
  hopLength = 512,
  fmin = 0,
  fmax = 0,
): StftPowerResult {
  if (!module) {
    throw new Error('Module not initialized. Call init() first.');
  }
  return module.melToStft(melPower, nMels, nFrames, sampleRate, nFft, hopLength, fmin, fmax);
}

/**
 * Reconstruct audio from a Mel power spectrogram via Griffin-Lim. Mirrors
 * `feature::mel_to_audio`.
 *
 * @param melPower - Mel power spectrogram [nMels x nFrames] row-major
 * @param nMels - Number of Mel bands
 * @param nFrames - Number of time frames
 * @param sampleRate - Sample rate in Hz
 * @param nFft - FFT size (default: 2048)
 * @param hopLength - Hop length (default: 512)
 * @param nIter - Griffin-Lim iterations (default: 32)
 * @returns Reconstructed audio samples (mono, float32)
 */
export function melToAudio(
  melPower: Float32Array,
  nMels: number,
  nFrames: number,
  sampleRate: number,
  nFft = 2048,
  hopLength = 512,
  nIter = 32,
  fmin = 0,
  fmax = 0,
): Float32Array {
  if (!module) {
    throw new Error('Module not initialized. Call init() first.');
  }
  return module.melToAudio(
    melPower,
    nMels,
    nFrames,
    sampleRate,
    nFft,
    hopLength,
    nIter,
    fmin,
    fmax,
  );
}

/**
 * Invert MFCC coefficients back to a Mel power spectrogram. Mirrors
 * `feature::mfcc_to_mel`.
 *
 * @param mfccCoefficients - MFCC matrix [nMfcc x nFrames] row-major
 * @param nMfcc - Number of MFCC coefficients
 * @param nFrames - Number of time frames
 * @param nMels - Number of Mel bins to reconstruct (default: 128)
 * @returns Mel power spectrogram result
 */
export function mfccToMel(
  mfccCoefficients: Float32Array,
  nMfcc: number,
  nFrames: number,
  nMels = 128,
): MelPowerResult {
  if (!module) {
    throw new Error('Module not initialized. Call init() first.');
  }
  return module.mfccToMel(mfccCoefficients, nMfcc, nFrames, nMels);
}

/**
 * Reconstruct audio directly from MFCC coefficients via Griffin-Lim. Mirrors
 * `feature::mfcc_to_audio`.
 *
 * @param mfccCoefficients - MFCC matrix [nMfcc x nFrames] row-major
 * @param nMfcc - Number of MFCC coefficients
 * @param nFrames - Number of time frames
 * @param nMels - Number of Mel bins (default: 128)
 * @param sampleRate - Sample rate in Hz
 * @param nFft - FFT size (default: 2048)
 * @param hopLength - Hop length (default: 512)
 * @param nIter - Griffin-Lim iterations (default: 32)
 * @returns Reconstructed audio samples (mono, float32)
 */
export function mfccToAudio(
  mfccCoefficients: Float32Array,
  nMfcc: number,
  nFrames: number,
  nMels: number,
  sampleRate: number,
  nFft = 2048,
  hopLength = 512,
  nIter = 32,
  fmin = 0,
  fmax = 0,
): Float32Array {
  if (!module) {
    throw new Error('Module not initialized. Call init() first.');
  }
  return module.mfccToAudio(
    mfccCoefficients,
    nMfcc,
    nFrames,
    nMels,
    sampleRate,
    nFft,
    hopLength,
    nIter,
    fmin,
    fmax,
  );
}

// ============================================================================
// Features - Chroma
// ============================================================================

/**
 * Compute chromagram (pitch class distribution).
 *
 * @param samples - Audio samples (mono, float32)
 * @param sampleRate - Sample rate in Hz
 * @param nFft - FFT size (default: 2048)
 * @param hopLength - Hop length (default: 512)
 * @returns Chroma features result
 */
export function chroma(
  samples: Float32Array,
  sampleRate: number,
  nFft = 2048,
  hopLength = 512,
): ChromaResult {
  if (!module) {
    throw new Error('Module not initialized. Call init() first.');
  }
  return module.chroma(samples, sampleRate, nFft, hopLength);
}

// ============================================================================
// Features - Spectral
// ============================================================================

/**
 * Compute spectral centroid (center of mass of spectrum).
 *
 * @param samples - Audio samples (mono, float32)
 * @param sampleRate - Sample rate in Hz
 * @param nFft - FFT size (default: 2048)
 * @param hopLength - Hop length (default: 512)
 * @returns Spectral centroid in Hz for each frame
 */
export function spectralCentroid(
  samples: Float32Array,
  sampleRate: number,
  nFft = 2048,
  hopLength = 512,
): Float32Array {
  if (!module) {
    throw new Error('Module not initialized. Call init() first.');
  }
  return module.spectralCentroid(samples, sampleRate, nFft, hopLength);
}

/**
 * Compute spectral bandwidth.
 *
 * @param samples - Audio samples (mono, float32)
 * @param sampleRate - Sample rate in Hz
 * @param nFft - FFT size (default: 2048)
 * @param hopLength - Hop length (default: 512)
 * @returns Spectral bandwidth in Hz for each frame
 */
export function spectralBandwidth(
  samples: Float32Array,
  sampleRate: number,
  nFft = 2048,
  hopLength = 512,
): Float32Array {
  if (!module) {
    throw new Error('Module not initialized. Call init() first.');
  }
  return module.spectralBandwidth(samples, sampleRate, nFft, hopLength);
}

/**
 * Compute spectral rolloff frequency.
 *
 * @param samples - Audio samples (mono, float32)
 * @param sampleRate - Sample rate in Hz
 * @param nFft - FFT size (default: 2048)
 * @param hopLength - Hop length (default: 512)
 * @param rollPercent - Percentage threshold (default: 0.85)
 * @returns Rolloff frequency in Hz for each frame
 */
export function spectralRolloff(
  samples: Float32Array,
  sampleRate: number,
  nFft = 2048,
  hopLength = 512,
  rollPercent = 0.85,
): Float32Array {
  if (!module) {
    throw new Error('Module not initialized. Call init() first.');
  }
  return module.spectralRolloff(samples, sampleRate, nFft, hopLength, rollPercent);
}

/**
 * Compute spectral flatness.
 *
 * @param samples - Audio samples (mono, float32)
 * @param sampleRate - Sample rate in Hz
 * @param nFft - FFT size (default: 2048)
 * @param hopLength - Hop length (default: 512)
 * @returns Spectral flatness for each frame (0 = tonal, 1 = noise-like)
 */
export function spectralFlatness(
  samples: Float32Array,
  sampleRate: number,
  nFft = 2048,
  hopLength = 512,
): Float32Array {
  if (!module) {
    throw new Error('Module not initialized. Call init() first.');
  }
  return module.spectralFlatness(samples, sampleRate, nFft, hopLength);
}

/**
 * Compute zero crossing rate.
 *
 * @param samples - Audio samples (mono, float32)
 * @param sampleRate - Sample rate in Hz
 * @param frameLength - Frame length (default: 2048)
 * @param hopLength - Hop length (default: 512)
 * @returns Zero crossing rate for each frame
 */
export function zeroCrossingRate(
  samples: Float32Array,
  sampleRate: number,
  frameLength = 2048,
  hopLength = 512,
): Float32Array {
  if (!module) {
    throw new Error('Module not initialized. Call init() first.');
  }
  return module.zeroCrossingRate(samples, sampleRate, frameLength, hopLength);
}

/**
 * Compute RMS energy.
 *
 * @param samples - Audio samples (mono, float32)
 * @param sampleRate - Sample rate in Hz
 * @param frameLength - Frame length (default: 2048)
 * @param hopLength - Hop length (default: 512)
 * @returns RMS energy for each frame
 */
export function rmsEnergy(
  samples: Float32Array,
  sampleRate: number,
  frameLength = 2048,
  hopLength = 512,
): Float32Array {
  if (!module) {
    throw new Error('Module not initialized. Call init() first.');
  }
  return module.rmsEnergy(samples, sampleRate, frameLength, hopLength);
}

// ============================================================================
// Features - Pitch
// ============================================================================

/**
 * Detect pitch using YIN algorithm.
 *
 * @param samples - Audio samples (mono, float32)
 * @param sampleRate - Sample rate in Hz
 * @param frameLength - Frame length (default: 2048)
 * @param hopLength - Hop length (default: 512)
 * @param fmin - Minimum frequency in Hz (default: 65)
 * @param fmax - Maximum frequency in Hz (default: 2093)
 * @param threshold - YIN threshold (default: 0.3)
 * @returns Pitch detection result
 */
export function pitchYin(
  samples: Float32Array,
  sampleRate: number,
  frameLength = 2048,
  hopLength = 512,
  fmin = 65.0,
  fmax = 2093.0,
  threshold = 0.3,
): PitchResult {
  if (!module) {
    throw new Error('Module not initialized. Call init() first.');
  }
  return module.pitchYin(samples, sampleRate, frameLength, hopLength, fmin, fmax, threshold);
}

/**
 * Detect pitch using pYIN algorithm (probabilistic YIN with HMM smoothing).
 *
 * @param samples - Audio samples (mono, float32)
 * @param sampleRate - Sample rate in Hz
 * @param frameLength - Frame length (default: 2048)
 * @param hopLength - Hop length (default: 512)
 * @param fmin - Minimum frequency in Hz (default: 65)
 * @param fmax - Maximum frequency in Hz (default: 2093)
 * @param threshold - YIN threshold (default: 0.3)
 * @returns Pitch detection result
 */
export function pitchPyin(
  samples: Float32Array,
  sampleRate: number,
  frameLength = 2048,
  hopLength = 512,
  fmin = 65.0,
  fmax = 2093.0,
  threshold = 0.3,
): PitchResult {
  if (!module) {
    throw new Error('Module not initialized. Call init() first.');
  }
  return module.pitchPyin(samples, sampleRate, frameLength, hopLength, fmin, fmax, threshold);
}

// ============================================================================
// Core - Unit Conversion
// ============================================================================

/**
 * Convert frequency in Hz to Mel scale.
 *
 * @param hz - Frequency in Hz
 * @returns Mel frequency
 */
export function hzToMel(hz: number): number {
  if (!module) {
    throw new Error('Module not initialized. Call init() first.');
  }
  return module.hzToMel(hz);
}

/**
 * Convert Mel scale to frequency in Hz.
 *
 * @param mel - Mel frequency
 * @returns Frequency in Hz
 */
export function melToHz(mel: number): number {
  if (!module) {
    throw new Error('Module not initialized. Call init() first.');
  }
  return module.melToHz(mel);
}

/**
 * Convert frequency in Hz to MIDI note number.
 *
 * @param hz - Frequency in Hz
 * @returns MIDI note number (A4 = 440 Hz = 69)
 */
export function hzToMidi(hz: number): number {
  if (!module) {
    throw new Error('Module not initialized. Call init() first.');
  }
  return module.hzToMidi(hz);
}

/**
 * Convert MIDI note number to frequency in Hz.
 *
 * @param midi - MIDI note number
 * @returns Frequency in Hz
 */
export function midiToHz(midi: number): number {
  if (!module) {
    throw new Error('Module not initialized. Call init() first.');
  }
  return module.midiToHz(midi);
}

/**
 * Convert frequency in Hz to note name.
 *
 * @param hz - Frequency in Hz
 * @returns Note name (e.g., "A4", "C#5")
 */
export function hzToNote(hz: number): string {
  if (!module) {
    throw new Error('Module not initialized. Call init() first.');
  }
  return module.hzToNote(hz);
}

/**
 * Convert note name to frequency in Hz.
 *
 * @param note - Note name (e.g., "A4", "C#5")
 * @returns Frequency in Hz
 */
export function noteToHz(note: string): number {
  if (!module) {
    throw new Error('Module not initialized. Call init() first.');
  }
  return module.noteToHz(note);
}

/**
 * Convert frame index to time in seconds.
 *
 * @param frames - Frame index
 * @param sr - Sample rate in Hz
 * @param hopLength - Hop length in samples
 * @returns Time in seconds
 */
export function framesToTime(frames: number, sr: number, hopLength: number): number {
  if (!module) {
    throw new Error('Module not initialized. Call init() first.');
  }
  return module.framesToTime(frames, sr, hopLength);
}

/**
 * Convert time in seconds to frame index.
 *
 * @param time - Time in seconds
 * @param sr - Sample rate in Hz
 * @param hopLength - Hop length in samples
 * @returns Frame index
 */
export function timeToFrames(time: number, sr: number, hopLength: number): number {
  if (!module) {
    throw new Error('Module not initialized. Call init() first.');
  }
  return module.timeToFrames(time, sr, hopLength);
}

export function framesToSamples(frames: number, hopLength = 512, nFft = 0): number {
  if (!module) {
    throw new Error('Module not initialized. Call init() first.');
  }
  return module.framesToSamples(frames, hopLength, nFft);
}

export function samplesToFrames(samples: number, hopLength = 512, nFft = 0): number {
  if (!module) {
    throw new Error('Module not initialized. Call init() first.');
  }
  return module.samplesToFrames(samples, hopLength, nFft);
}

export function powerToDb(
  values: Float32Array,
  ref = 1.0,
  amin = 1e-10,
  topDb = 80.0,
): Float32Array {
  if (!module) {
    throw new Error('Module not initialized. Call init() first.');
  }
  return module.powerToDb(values, ref, amin, topDb);
}

export function amplitudeToDb(
  values: Float32Array,
  ref = 1.0,
  amin = 1e-5,
  topDb = 80.0,
): Float32Array {
  if (!module) {
    throw new Error('Module not initialized. Call init() first.');
  }
  return module.amplitudeToDb(values, ref, amin, topDb);
}

export function dbToPower(values: Float32Array, ref = 1.0): Float32Array {
  if (!module) {
    throw new Error('Module not initialized. Call init() first.');
  }
  return module.dbToPower(values, ref);
}

export function dbToAmplitude(values: Float32Array, ref = 1.0): Float32Array {
  if (!module) {
    throw new Error('Module not initialized. Call init() first.');
  }
  return module.dbToAmplitude(values, ref);
}

export function preemphasis(samples: Float32Array, coef = 0.97, zi?: number): Float32Array {
  if (!module) {
    throw new Error('Module not initialized. Call init() first.');
  }
  return module.preemphasis(samples, coef, zi ?? null);
}

export function deemphasis(samples: Float32Array, coef = 0.97, zi?: number): Float32Array {
  if (!module) {
    throw new Error('Module not initialized. Call init() first.');
  }
  return module.deemphasis(samples, coef, zi ?? null);
}

export function trimSilence(
  samples: Float32Array,
  topDb = 60.0,
  frameLength = 2048,
  hopLength = 512,
): WasmTrimResult {
  if (!module) {
    throw new Error('Module not initialized. Call init() first.');
  }
  return module.trimSilence(samples, topDb, frameLength, hopLength);
}

export function splitSilence(
  samples: Float32Array,
  topDb = 60.0,
  frameLength = 2048,
  hopLength = 512,
): Int32Array {
  if (!module) {
    throw new Error('Module not initialized. Call init() first.');
  }
  return module.splitSilence(samples, topDb, frameLength, hopLength);
}

export function frameSignal(
  samples: Float32Array,
  frameLength: number,
  hopLength: number,
): WasmFrameResult {
  if (!module) {
    throw new Error('Module not initialized. Call init() first.');
  }
  return module.frameSignal(samples, frameLength, hopLength);
}

export function padCenter(values: Float32Array, size: number, padValue = 0.0): Float32Array {
  if (!module) {
    throw new Error('Module not initialized. Call init() first.');
  }
  return module.padCenter(values, size, padValue);
}

export function fixLength(values: Float32Array, size: number, padValue = 0.0): Float32Array {
  if (!module) {
    throw new Error('Module not initialized. Call init() first.');
  }
  return module.fixLength(values, size, padValue);
}

export function fixFrames(frames: Int32Array, xMin = 0, xMax = -1, pad = true): Int32Array {
  if (!module) {
    throw new Error('Module not initialized. Call init() first.');
  }
  return module.fixFrames(frames, xMin, xMax, pad);
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
  if (!module) {
    throw new Error('Module not initialized. Call init() first.');
  }
  return module.peakPick(values, preMax, postMax, preAvg, postAvg, delta, wait);
}

export function vectorNormalize(
  values: Float32Array,
  normType = 0,
  threshold = 1e-12,
): Float32Array {
  if (!module) {
    throw new Error('Module not initialized. Call init() first.');
  }
  return module.vectorNormalize(values, normType, threshold);
}

export function pcen(
  values: Float32Array,
  nBins: number,
  nFrames: number,
  options: Record<string, number> = {},
): Float32Array {
  if (!module) {
    throw new Error('Module not initialized. Call init() first.');
  }
  return module.pcen(values, nBins, nFrames, options);
}

export function tonnetz(chromagram: Float32Array, nChroma: number, nFrames: number): Float32Array {
  if (!module) {
    throw new Error('Module not initialized. Call init() first.');
  }
  return module.tonnetz(chromagram, nChroma, nFrames);
}

export function tempogram(
  onsetEnvelope: Float32Array,
  sampleRate: number,
  hopLength = 512,
  winLength = 384,
  mode: TempogramMode = 'autocorrelation',
): WasmTempogramResult {
  if (!module) {
    throw new Error('Module not initialized. Call init() first.');
  }
  return module.tempogram(onsetEnvelope, sampleRate, hopLength, winLength, mode);
}

export function cyclicTempogram(
  onsetEnvelope: Float32Array,
  sampleRate: number,
  hopLength = 512,
  winLength = 384,
  bpmMin = 60.0,
  nBins = 60,
): WasmCyclicTempogramResult {
  if (!module) {
    throw new Error('Module not initialized. Call init() first.');
  }
  return module.cyclicTempogram(onsetEnvelope, sampleRate, hopLength, winLength, bpmMin, nBins);
}

export function plp(
  onsetEnvelope: Float32Array,
  sampleRate: number,
  hopLength = 512,
  tempoMin = 30.0,
  tempoMax = 300.0,
  winLength = 384,
): Float32Array {
  if (!module) {
    throw new Error('Module not initialized. Call init() first.');
  }
  return module.plp(onsetEnvelope, sampleRate, hopLength, tempoMin, tempoMax, winLength);
}

/**
 * Compute NNLS (non-negative least squares) chromagram.
 *
 * @param samples - Audio samples (mono, float32)
 * @param sampleRate - Sample rate in Hz (default: 22050)
 * @returns NNLS chroma result
 */
export function nnlsChroma(samples: Float32Array, sampleRate = 22050): WasmNnlsChromaResult {
  if (!module) {
    throw new Error('Module not initialized. Call init() first.');
  }
  return module.nnlsChroma(samples, sampleRate);
}

/**
 * Compute the Constant-Q Transform magnitude.
 *
 * @param samples - Audio samples (mono, float32)
 * @param sampleRate - Sample rate in Hz (default: 22050)
 * @param hopLength - Hop length (default: 512)
 * @param fmin - Minimum frequency in Hz (default: 32.70319566257483, C1)
 * @param nBins - Number of frequency bins (default: 84)
 * @param binsPerOctave - Bins per octave (default: 12)
 * @returns CQT magnitude result
 */
export function cqt(
  samples: Float32Array,
  sampleRate = 22050,
  hopLength = 512,
  fmin = 32.70319566257483,
  nBins = 84,
  binsPerOctave = 12,
): CqtResult {
  if (!module) {
    throw new Error('Module not initialized. Call init() first.');
  }
  return module.cqt(samples, sampleRate, hopLength, fmin, nBins, binsPerOctave);
}

/**
 * Compute the Variable-Q Transform magnitude (gamma controls Q).
 *
 * @param samples - Audio samples (mono, float32)
 * @param sampleRate - Sample rate in Hz (default: 22050)
 * @param hopLength - Hop length (default: 512)
 * @param fmin - Minimum frequency in Hz (default: 32.70319566257483, C1)
 * @param nBins - Number of frequency bins (default: 84)
 * @param binsPerOctave - Bins per octave (default: 12)
 * @param gamma - Bandwidth offset; 0 is equivalent to CQT (default: 0)
 * @returns VQT magnitude result (same shape as CQT)
 */
export function vqt(
  samples: Float32Array,
  sampleRate = 22050,
  hopLength = 512,
  fmin = 32.70319566257483,
  nBins = 84,
  binsPerOctave = 12,
  gamma = 0,
): CqtResult {
  if (!module) {
    throw new Error('Module not initialized. Call init() first.');
  }
  return module.vqt(samples, sampleRate, hopLength, fmin, nBins, binsPerOctave, gamma);
}

/**
 * Detect song-structure sections (intro/verse/chorus/...).
 *
 * @param samples - Audio samples (mono, float32)
 * @param sampleRate - Sample rate in Hz (default: 22050)
 * @param nFft - FFT size (default: 2048)
 * @param hopLength - Hop length (default: 512)
 * @param minSectionSec - Minimum section duration in seconds (default: 8.0)
 * @returns Array of detected sections
 */
export function analyzeSections(
  samples: Float32Array,
  sampleRate = 22050,
  nFft = 2048,
  hopLength = 512,
  minSectionSec = 8.0,
): Section[] {
  if (!module) {
    throw new Error('Module not initialized. Call init() first.');
  }
  return module
    .analyzeSections(samples, sampleRate, nFft, hopLength, minSectionSec)
    .map((s) => ({ ...s, type: s.type as SectionType }));
}

/**
 * Extract the melody contour from monophonic audio via YIN.
 *
 * @param samples - Audio samples (mono, float32)
 * @param sampleRate - Sample rate in Hz (default: 22050)
 * @param fmin - Minimum frequency in Hz (default: 65.0)
 * @param fmax - Maximum frequency in Hz (default: 2093.0)
 * @param frameLength - Frame length in samples (default: 2048)
 * @param hopLength - Hop length (default: 512)
 * @param threshold - YIN threshold; lower is stricter (default: 0.1)
 * @returns Melody contour with per-frame pitch points and summary stats
 */
export function analyzeMelody(
  samples: Float32Array,
  sampleRate = 22050,
  fmin = 65.0,
  fmax = 2093.0,
  frameLength = 2048,
  hopLength = 512,
  threshold = 0.1,
): MelodyResult {
  if (!module) {
    throw new Error('Module not initialized. Call init() first.');
  }
  return module.analyzeMelody(samples, sampleRate, fmin, fmax, frameLength, hopLength, threshold);
}

/**
 * Compute the onset strength envelope.
 *
 * @param samples - Audio samples (mono, float32)
 * @param sampleRate - Sample rate in Hz (default: 22050)
 * @param nFft - FFT size (default: 2048)
 * @param hopLength - Hop length (default: 512)
 * @param nMels - Number of Mel bands (default: 128)
 * @returns Onset envelope for each frame
 */
export function onsetEnvelope(
  samples: Float32Array,
  sampleRate = 22050,
  nFft = 2048,
  hopLength = 512,
  nMels = 128,
): Float32Array {
  if (!module) {
    throw new Error('Module not initialized. Call init() first.');
  }
  return module.onsetEnvelope(samples, sampleRate, nFft, hopLength, nMels);
}

/**
 * Compute the Fourier tempogram from an onset envelope.
 *
 * @param onsetEnvelope - Onset strength envelope (float32)
 * @param sampleRate - Sample rate in Hz (default: 22050)
 * @param hopLength - Hop length (default: 512)
 * @param winLength - Window length in frames (default: 384)
 * @returns Fourier tempogram result
 */
export function fourierTempogram(
  onsetEnvelope: Float32Array,
  sampleRate = 22050,
  hopLength = 512,
  winLength = 384,
): WasmFourierTempogramResult {
  if (!module) {
    throw new Error('Module not initialized. Call init() first.');
  }
  return module.fourierTempogram(onsetEnvelope, sampleRate, hopLength, winLength);
}

/**
 * Compute tempogram ratio features.
 *
 * @param tempogramData - Tempogram data (float32)
 * @param winLength - Window length in frames (default: 384)
 * @param sampleRate - Sample rate in Hz (default: 22050)
 * @param hopLength - Hop length (default: 512)
 * @returns Tempogram ratio features
 */
export function tempogramRatio(
  tempogramData: Float32Array,
  winLength = 384,
  sampleRate = 22050,
  hopLength = 512,
): Float32Array {
  if (!module) {
    throw new Error('Module not initialized. Call init() first.');
  }
  return module.tempogramRatio(tempogramData, winLength, sampleRate, hopLength);
}

/**
 * Measure loudness (EBU R128 / ITU-R BS.1770).
 *
 * @param samples - Audio samples (mono, float32)
 * @param sampleRate - Sample rate in Hz (default: 22050)
 * @returns Loudness measurement result
 */
export function lufs(samples: Float32Array, sampleRate = 22050): LufsResult {
  if (!module) {
    throw new Error('Module not initialized. Call init() first.');
  }
  return module.lufs(samples, sampleRate);
}

/**
 * Compute the momentary loudness (LUFS) over time.
 *
 * @param samples - Audio samples (mono, float32)
 * @param sampleRate - Sample rate in Hz (default: 22050)
 * @returns Momentary LUFS values over time
 */
export function momentaryLufs(samples: Float32Array, sampleRate = 22050): Float32Array {
  if (!module) {
    throw new Error('Module not initialized. Call init() first.');
  }
  return module.momentaryLufs(samples, sampleRate);
}

/**
 * Compute the short-term loudness (LUFS) over time.
 *
 * @param samples - Audio samples (mono, float32)
 * @param sampleRate - Sample rate in Hz (default: 22050)
 * @returns Short-term LUFS values over time
 */
export function shortTermLufs(samples: Float32Array, sampleRate = 22050): Float32Array {
  if (!module) {
    throw new Error('Module not initialized. Call init() first.');
  }
  return module.shortTermLufs(samples, sampleRate);
}

// ============================================================================
// Core - Resample
// ============================================================================

/**
 * Resample audio to a different sample rate.
 *
 * @param samples - Audio samples (mono, float32)
 * @param srcSr - Source sample rate in Hz
 * @param targetSr - Target sample rate in Hz
 * @returns Resampled audio
 */
export function resample(samples: Float32Array, srcSr: number, targetSr: number): Float32Array {
  if (!module) {
    throw new Error('Module not initialized. Call init() first.');
  }
  return module.resample(samples, srcSr, targetSr);
}

// ============================================================================
// Audio Class
// ============================================================================

/**
 * Wrapper around audio data that exposes all analysis and feature functions as instance methods.
 *
 * @example
 * ```typescript
 * import { init, Audio } from '@libraz/sonare';
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

  /** Create an Audio instance from raw sample data. */
  static fromBuffer(samples: Float32Array, sampleRate: number): Audio {
    return new Audio(samples, sampleRate);
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

  pitchCorrectToMidi(currentMidi: number, targetMidi: number): Float32Array {
    return pitchCorrectToMidi(this._samples, this._sampleRate, currentMidi, targetMidi);
  }

  noteStretch(onsetSample: number, offsetSample: number, stretchRatio: number): Float32Array {
    return noteStretch(this._samples, this._sampleRate, onsetSample, offsetSample, stretchRatio);
  }

  voiceChange(pitchSemitones: number, formantFactor: number): Float32Array {
    return voiceChange(this._samples, this._sampleRate, pitchSemitones, formantFactor);
  }

  normalize(targetDb = 0.0): Float32Array {
    return normalize(this._samples, this._sampleRate, targetDb);
  }

  mastering(targetLufs = -14.0, ceilingDb = -1.0, truePeakOversample = 4): MasteringResult {
    return mastering(this._samples, this._sampleRate, targetLufs, ceilingDb, truePeakOversample);
  }

  masteringChain(config: MasteringChainConfig): MasteringChainResult {
    return masteringChain(this._samples, this._sampleRate, config);
  }

  masterAudio(
    presetName: MasteringPreset,
    overrides: Record<string, number | boolean> | null = null,
  ): MasteringChainResult {
    return masterAudio(this._samples, this._sampleRate, presetName, overrides);
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

  melSpectrogram(nFft = 2048, hopLength = 512, nMels = 128): MelSpectrogramResult {
    return melSpectrogram(this._samples, this._sampleRate, nFft, hopLength, nMels);
  }

  mfcc(nFft = 2048, hopLength = 512, nMels = 128, nMfcc = 13): MfccResult {
    return mfcc(this._samples, this._sampleRate, nFft, hopLength, nMels, nMfcc);
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
  ): PitchResult {
    return pitchYin(this._samples, this._sampleRate, frameLength, hopLength, fmin, fmax, threshold);
  }

  pitchPyin(
    frameLength = 2048,
    hopLength = 512,
    fmin = 65.0,
    fmax = 2093.0,
    threshold = 0.3,
  ): PitchResult {
    return pitchPyin(
      this._samples,
      this._sampleRate,
      frameLength,
      hopLength,
      fmin,
      fmax,
      threshold,
    );
  }

  resample(targetSr: number): Float32Array {
    return resample(this._samples, this._sampleRate, targetSr);
  }
}

// ============================================================================
// StreamAnalyzer Class
// ============================================================================

/**
 * Real-time streaming audio analyzer.
 *
 * @example
 * ```typescript
 * import { init, StreamAnalyzer } from '@libraz/sonare';
 *
 * await init();
 *
 * const analyzer = new StreamAnalyzer({ sampleRate: 44100 });
 *
 * // In audio processing callback
 * analyzer.process(samples);
 *
 * // Get current analysis state
 * const stats = analyzer.stats();
 * console.log('BPM:', stats.estimate.bpm);
 * console.log('Key:', stats.estimate.key);
 * console.log('Chord progression:', stats.estimate.chordProgression);
 * ```
 */
export class StreamAnalyzer {
  private analyzer: WasmStreamAnalyzer;

  /**
   * Create a new StreamAnalyzer.
   *
   * @param config - Configuration options
   */
  constructor(config: StreamConfig) {
    if (!module) {
      throw new Error('Module not initialized. Call init() first.');
    }
    const wasmModule = module;
    const args = [
      config.sampleRate,
      config.nFft ?? 2048,
      config.hopLength ?? 512,
      config.nMels ?? 128,
      config.fmin ?? 0,
      config.fmax ?? 0,
      config.tuningRefHz ?? 440,
      config.computeMagnitude ?? true,
      config.computeMel ?? true,
      config.computeChroma ?? true,
      config.computeOnset ?? true,
      config.computeSpectral ?? true,
      config.emitEveryNFrames ?? 1,
      config.magnitudeDownsample ?? 1,
      config.keyUpdateIntervalSec ?? 5,
      config.bpmUpdateIntervalSec ?? 10,
      config.window ?? 0,
      config.outputFormat ?? 0,
    ] as const;
    const isArityError = (error: unknown): boolean => {
      const message = String((error as { message?: unknown } | null)?.message ?? error);
      return message.includes('invalid number of parameters');
    };
    const createLegacy = (): WasmStreamAnalyzer => {
      const LegacyStreamAnalyzer = wasmModule.StreamAnalyzer as unknown as new (
        sampleRate: number,
        nFft: number,
        hopLength: number,
        nMels: number,
        computeMel: boolean,
        computeChroma: boolean,
        computeOnset: boolean,
        emitEveryNFrames: number,
      ) => WasmStreamAnalyzer;
      return new LegacyStreamAnalyzer(
        args[0],
        args[1],
        args[2],
        args[3],
        args[8],
        args[9],
        args[10],
        args[12],
      );
    };
    const hasExtendedConfig =
      config.fmin !== undefined ||
      config.fmax !== undefined ||
      config.tuningRefHz !== undefined ||
      config.computeMagnitude !== undefined ||
      config.computeSpectral !== undefined ||
      config.magnitudeDownsample !== undefined ||
      config.keyUpdateIntervalSec !== undefined ||
      config.bpmUpdateIntervalSec !== undefined ||
      config.window !== undefined ||
      config.outputFormat !== undefined;
    if (hasExtendedConfig) {
      try {
        this.analyzer = new wasmModule.StreamAnalyzer(...args);
      } catch (error) {
        if (!isArityError(error)) throw error;
        this.analyzer = createLegacy();
      }
    } else {
      try {
        this.analyzer = createLegacy();
      } catch (error) {
        if (!isArityError(error)) throw error;
        this.analyzer = new wasmModule.StreamAnalyzer(...args);
      }
    }
  }

  /**
   * Process audio samples.
   *
   * @param samples - Audio samples (mono, float32)
   */
  process(samples: Float32Array): void {
    this.analyzer.process(samples);
  }

  /**
   * Process audio samples with explicit sample offset.
   *
   * @param samples - Audio samples (mono, float32)
   * @param sampleOffset - Cumulative sample count at start of this chunk
   */
  processWithOffset(samples: Float32Array, sampleOffset: number): void {
    this.analyzer.processWithOffset(samples, sampleOffset);
  }

  /**
   * Get the number of frames available to read.
   */
  availableFrames(): number {
    return this.analyzer.availableFrames();
  }

  /**
   * Read processed frames as Structure of Arrays.
   *
   * @param maxFrames - Maximum number of frames to read
   * @returns Frame buffer with analysis results
   */
  readFrames(maxFrames: number): FrameBuffer {
    return this.analyzer.readFramesSoa(maxFrames);
  }

  readFramesU8(maxFrames: number): StreamFramesU8 {
    return this.analyzer.readFramesU8(maxFrames) as StreamFramesU8;
  }

  readFramesI16(maxFrames: number): StreamFramesI16 {
    return this.analyzer.readFramesI16(maxFrames) as StreamFramesI16;
  }

  /**
   * Reset the analyzer state.
   *
   * @param baseSampleOffset - Starting sample offset (default 0)
   */
  reset(baseSampleOffset = 0): void {
    this.analyzer.reset(baseSampleOffset);
  }

  /**
   * Get current statistics and progressive estimates.
   *
   * @returns Analyzer statistics including BPM, key, and chord progression
   */
  stats(): AnalyzerStats {
    const s = this.analyzer.stats();
    return {
      totalFrames: s.totalFrames,
      totalSamples: s.totalSamples,
      durationSeconds: s.durationSeconds,
      estimate: {
        bpm: s.estimate.bpm,
        bpmConfidence: s.estimate.bpmConfidence,
        bpmCandidateCount: s.estimate.bpmCandidateCount,
        key: s.estimate.key as PitchClass,
        keyMinor: s.estimate.keyMinor,
        keyConfidence: s.estimate.keyConfidence,
        chordRoot: s.estimate.chordRoot as PitchClass,
        chordQuality: s.estimate.chordQuality as ChordQuality,
        chordConfidence: s.estimate.chordConfidence,
        chordStartTime: s.estimate.chordStartTime,
        chordProgression: s.estimate.chordProgression.map((c) => ({
          root: c.root as PitchClass,
          quality: c.quality as ChordQuality,
          startTime: c.startTime,
          confidence: c.confidence,
        })),
        barChordProgression: s.estimate.barChordProgression.map((c) => ({
          barIndex: c.barIndex,
          root: c.root as PitchClass,
          quality: c.quality as ChordQuality,
          startTime: c.startTime,
          confidence: c.confidence,
        })),
        currentBar: s.estimate.currentBar,
        barDuration: s.estimate.barDuration,
        votedPattern: (s.estimate.votedPattern || []).map((c) => ({
          barIndex: c.barIndex,
          root: c.root as PitchClass,
          quality: c.quality as ChordQuality,
          startTime: c.startTime,
          confidence: c.confidence,
        })),
        patternLength: s.estimate.patternLength,
        detectedPatternName: s.estimate.detectedPatternName || '',
        detectedPatternScore: s.estimate.detectedPatternScore || 0,
        allPatternScores: (s.estimate.allPatternScores || []).map((p) => ({
          name: p.name,
          score: p.score,
        })),
        accumulatedSeconds: s.estimate.accumulatedSeconds,
        usedFrames: s.estimate.usedFrames,
        updated: s.estimate.updated,
      },
    };
  }

  /**
   * Get total frames processed.
   */
  frameCount(): number {
    return this.analyzer.frameCount();
  }

  /**
   * Get current time position in seconds.
   */
  currentTime(): number {
    return this.analyzer.currentTime();
  }

  /**
   * Get the sample rate.
   */
  sampleRate(): number {
    return this.analyzer.sampleRate();
  }

  /**
   * Set the expected total duration for pattern lock timing.
   *
   * @param durationSeconds - Total duration in seconds
   */
  setExpectedDuration(durationSeconds: number): void {
    this.analyzer.setExpectedDuration(durationSeconds);
  }

  /**
   * Set normalization gain for loud/compressed audio.
   *
   * @param gain - Gain factor to apply (e.g., 0.5 for -6dB reduction)
   */
  setNormalizationGain(gain: number): void {
    this.analyzer.setNormalizationGain(gain);
  }

  /**
   * Set tuning reference frequency for non-standard tuning.
   *
   * @param refHz - Reference frequency for A4 (default 440 Hz)
   * @example
   * // If audio is 1 semitone sharp (A4 = 466.16 Hz)
   * analyzer.setTuningRefHz(466.16);
   * // If audio is 1 semitone flat (A4 = 415.30 Hz)
   * analyzer.setTuningRefHz(415.30);
   */
  setTuningRefHz(refHz: number): void {
    this.analyzer.setTuningRefHz(refHz);
  }

  /**
   * Release resources. Call when done using the analyzer.
   */
  dispose(): void {
    this.analyzer.delete();
  }
}

// ============================================================================
// Re-exports
// ============================================================================

export { PitchClass as Pitch } from './public_types';
