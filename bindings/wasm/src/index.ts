/**
 * sonare - Audio Analysis Library
 *
 * @example
 * ```typescript
 * import { init, detectBpm, detectKey, analyze } from '@libraz/libsonare';
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

import type { RealtimeVoiceChangerPodConfig, VoicePresetId } from './public_types';
import type {
  SonareModule,
  WasmDecomposeResult,
  WasmHpssWithResidualResult,
  WasmMatrix2dResult,
} from './sonare.js';
import { setSonareModule } from './module_state';

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
  PanMode,
  PitchResult,
  RealtimeVoiceChangerConfigInput,
  RealtimeVoiceChangerPodConfig,
  RhythmFeatures,
  RirResult,
  RirSynthOptions,
  RoomEstimateOptions,
  RoomEstimateResult,
  RoomGeometryOptions,
  RoomMorphOptions,
  Section,
  SendTiming,
  SoloProcessor,
  StereoAnalysis,
  StftPowerResult,
  StftResult,
  StreamingEqualizerConfig,
  StreamingPlatform,
  StreamingRetuneConfig,
  TempogramMode,
  Timbre,
  TimeSignature,
  VoicePresetId,
} from './public_types';
export {
  ChordQuality,
  KeyProfile,
  Mode,
  PitchClass,
  SectionType,
} from './public_types';
export type { ProgressCallback } from './sonare.js';
export { EXPECTED_PROJECT_ABI_VERSION, Project, projectAbiVersion } from './project';
export { Audio } from './audio';
export { EXPECTED_ENGINE_ABI_VERSION, engineCapabilities, RealtimeEngine } from './realtime_engine';
export type {
  EngineAutomationPoint,
  EngineBounceOptions,
  EngineBounceResult,
  EngineCapabilities,
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
} from './realtime_engine';
export {
  detectBpm,
  detectKey,
  detectKeyCandidates,
  detectOnsets,
  detectBeats,
  detectDownbeats,
  detectChords,
  chordFunctionalAnalysis,
  analyze,
  analyzeImpulseResponse,
  detectAcoustic,
  synthesizeRir,
  estimateRoom,
  roomMorph,
  analyzeWithProgress,
  analyzeBpm,
  analyzeRhythm,
  analyzeDynamics,
  analyzeTimbre,
  hasFfmpegSupport,
} from './quick_analysis';
export type {
  BpmCandidate,
  BpmAnalysisResult,
  RhythmAnalysisResult,
  DynamicsAnalysisResult,
  TimbreFrame,
  TimbreAnalysisResult,
} from './quick_analysis';
export {
  hpss,
  harmonic,
  percussive,
  timeStretch,
  pitchShift,
  pitchCorrectToMidi,
  noteStretch,
  voiceChange,
  voiceChangeRealtime,
  normalize,
  mastering,
  masteringProcessorNames,
  masteringInsertNames,
  masteringPairProcessorNames,
  masteringPairAnalysisNames,
  masteringStereoAnalysisNames,
  masteringProcess,
  masteringProcessStereo,
  masteringPairProcess,
  masteringPairAnalyze,
  masteringStereoAnalyze,
  masteringAssistantSuggest,
  masteringAudioProfile,
  masteringStreamingPreview,
  masteringRepairDeclick,
  masteringRepairDenoiseClassical,
  masteringRepairDeclip,
  masteringRepairDecrackle,
  masteringRepairDehum,
  masteringRepairDereverbClassical,
  masteringRepairTrimSilence,
  masteringDynamicsCompressor,
  masteringDynamicsGate,
  masteringDynamicsTransientShaper,
  masteringChain,
  masteringChainStereo,
  masteringChainWithProgress,
  masteringChainStereoWithProgress,
  masteringPresetNames,
  masterAudio,
  masterAudioStereo,
  masterAudioWithProgress,
  masterAudioStereoWithProgress,
  mixingScenePresetNames,
  mixingScenePresetJson,
  mixStereo,
} from './effects_mastering';
export type {
  CompressorDetector,
  CompressorOptions,
  DeclickOptions,
  DeclipOptions,
  DecrackleMode,
  DecrackleOptions,
  DehumOptions,
  DenoiseClassicalMode,
  DenoiseClassicalNoiseEstimator,
  DenoiseClassicalOptions,
  DereverbClassicalOptions,
  DynamicsResult,
  GateOptions,
  TransientShaperOptions,
  TrimSilenceMode,
  TrimSilenceOptions,
  VoiceChangeRealtimeOptions,
} from './effects_mastering';
export {
  trim,
  stft,
  stftDb,
  melSpectrogram,
  mfcc,
  melToStft,
  melToAudio,
  mfccToMel,
  mfccToAudio,
  chroma,
  spectralCentroid,
  spectralContrast,
  polyFeatures,
  zeroCrossings,
  pitchTuning,
  estimateTuning,
  decompose,
  nnFilter,
  remix,
  phaseVocoder,
  hpssWithResidual,
  lufsInterleaved,
  ebur128LoudnessRange,
  spectralBandwidth,
  spectralRolloff,
  spectralFlatness,
  zeroCrossingRate,
  rmsEnergy,
  pitchYin,
  pitchPyin,
  hzToMel,
  melToHz,
  hzToMidi,
  midiToHz,
  hzToNote,
  noteToHz,
  framesToTime,
  timeToFrames,
  framesToSamples,
  samplesToFrames,
  powerToDb,
  amplitudeToDb,
  dbToPower,
  dbToAmplitude,
  preemphasis,
  deemphasis,
  trimSilence,
  splitSilence,
  frameSignal,
  padCenter,
  fixLength,
  fixFrames,
  peakPick,
  vectorNormalize,
  pcen,
  tonnetz,
  tempogram,
  cyclicTempogram,
  plp,
  nnlsChroma,
  cqt,
  vqt,
  analyzeSections,
  analyzeMelody,
  onsetEnvelope,
  fourierTempogram,
  tempogramRatio,
  lufs,
  momentaryLufs,
  shortTermLufs,
  resample,
} from './features';
export { StreamAnalyzer } from './stream_analyzer';
export {
  Mixer,
  realtimeVoiceChangerPresetJson,
  realtimeVoiceChangerPresetNames,
  StreamingEqualizer,
  StreamingMasteringChain,
  StreamingRetune,
  validateRealtimeVoiceChangerPresetJson,
} from './streaming_mixing';
export { RealtimeVoiceChanger } from './streaming_mixing';
export type {
  MixerRealtimeBuffer,
  RealtimeVoiceChangerInterleavedBuffer,
  RealtimeVoiceChangerMonoBuffer,
  RealtimeVoiceChangerPlanarBuffer,
} from './streaming_mixing';
export {
  meteringCrestFactorDb,
  meteringDcOffset,
  meteringDetectClipping,
  meteringDynamicRange,
  meteringPeakDb,
  meteringPhaseScope,
  meteringRmsDb,
  meteringSpectrum,
  meteringStereoCorrelation,
  meteringStereoWidth,
  meteringTruePeakDb,
  meteringVectorscope,
} from './metering';
export type {
  ClippingRegion,
  ClippingReport,
  DynamicRangeReport,
  PhaseScopeReport,
  SpectrumOptions,
  SpectrumReport,
  VectorscopeReport,
} from './metering';
export { scaleCorrectionSemitones, scalePitchClassEnabled, scaleQuantizeMidi } from './scale';
export type {
  BuiltinSynthBinding,
  BuiltinSynthWaveform,
  ProjectAssistSidecar,
  ProjectAutomationCurve,
  ProjectAutomationLaneDesc,
  ProjectAutomationPoint,
  ProjectBounceOptions,
  ProjectChordSymbol,
  ProjectClipDesc,
  ProjectClipFade,
  ProjectCompileResult,
  ProjectFadeCurve,
  ProjectKeySegment,
  ProjectLoopMode,
  ProjectMidiClipResult,
  ProjectMidiEvent,
  ProjectTrackDesc,
  ProjectTrackKind,
} from './project';
export type { ValidateOptions } from './validation';
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

/** Row-major 2-D matrix as a flat buffer plus its dimensions. */
export type Matrix2dResult = WasmMatrix2dResult;
/** NMF factor matrices { w, h } from {@link decompose}. */
export type DecomposeResult = WasmDecomposeResult;
/** Harmonic / percussive / residual signals from {@link hpssWithResidual}. */
export type HpssWithResidualResult = WasmHpssWithResidualResult;

// ============================================================================
// Module State
// ============================================================================

let module: SonareModule | null = null;
let initPromise: Promise<void> | null = null;

function requireModule(): SonareModule {
  if (!module) {
    throw new Error('Module not initialized. Call init() first.');
  }
  return module;
}

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
      setSonareModule(module);
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

export function voiceChangerAbiVersion(): number {
  if (!module) {
    throw new Error('Module not initialized. Call init() first.');
  }
  return module.voiceChangerAbiVersion();
}

// Canonical ordinal order of the built-in voice-character presets, matching the
// C ABI SonareVoiceCharacterPreset enum and SONARE_REALTIME_VOICE_CHANGER_PRESET_IDS.
const VOICE_PRESET_ORDINALS: readonly VoicePresetId[] = [
  'neutral-monitor',
  'bright-idol',
  'soft-whisper',
  'deep-narrator',
  'robot-mascot',
  'dark-villain',
];

function resolveVoicePresetOrdinal(preset: VoicePresetId | number): number {
  if (typeof preset === 'number') {
    return preset;
  }
  const ordinal = VOICE_PRESET_ORDINALS.indexOf(preset);
  if (ordinal < 0) {
    throw new Error(`Unknown voice character preset: ${preset}`);
  }
  return ordinal;
}

/**
 * Map a voice-character preset ordinal (or canonical id) to its canonical id
 * string (e.g. `'bright-idol'`). Returns `null` for an out-of-range ordinal.
 */
export function voiceCharacterPresetId(preset: VoicePresetId | number): string | null {
  if (!module) {
    throw new Error('Module not initialized. Call init() first.');
  }
  return module.voiceCharacterPresetId(resolveVoicePresetOrdinal(preset));
}

/**
 * Return the canonical (normalized) flat POD config for a built-in voice
 * preset, skipping the JSON round-trip. Accepts a canonical preset id or its
 * integer ordinal. Returns `null` for an out-of-range ordinal.
 */
export function realtimeVoiceChangerPresetConfig(
  preset: VoicePresetId | number,
): RealtimeVoiceChangerPodConfig | null {
  if (!module) {
    throw new Error('Module not initialized. Call init() first.');
  }
  return module.realtimeVoiceChangerPresetConfig(resolveVoicePresetOrdinal(preset));
}

// ============================================================================
// Re-exports
// ============================================================================

export { PitchClass as Pitch } from './public_types';
