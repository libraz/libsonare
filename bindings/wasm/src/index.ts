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

import { setSonareModule } from './module_state';
import type { RealtimeVoiceChangerPodConfig, VoicePresetId } from './public_types';
import type {
  SonareModule,
  WasmDecomposeResult,
  WasmHpssWithResidualResult,
  WasmMatrix2dResult,
} from './sonare.js';

export { Audio } from './audio';
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
  VoiceChangeOptions,
  VoiceChangeRealtimeOptions,
} from './effects_mastering';
export {
  harmonic,
  hpss,
  masterAudio,
  masterAudioStereo,
  masterAudioStereoWithProgress,
  masterAudioWithProgress,
  mastering,
  masteringAssistantSuggest,
  masteringAudioProfile,
  masteringChain,
  masteringChainStereo,
  masteringChainStereoWithProgress,
  masteringChainWithProgress,
  masteringDynamicsCompressor,
  masteringDynamicsGate,
  masteringDynamicsTransientShaper,
  masteringInsertNames,
  masteringPairAnalysisNames,
  masteringPairAnalyze,
  masteringPairProcess,
  masteringPairProcessorNames,
  masteringPresetNames,
  masteringProcess,
  masteringProcessorNames,
  masteringProcessStereo,
  masteringRepairDeclick,
  masteringRepairDeclip,
  masteringRepairDecrackle,
  masteringRepairDehum,
  masteringRepairDenoiseClassical,
  masteringRepairDereverbClassical,
  masteringRepairTrimSilence,
  masteringStereoAnalysisNames,
  masteringStereoAnalyze,
  masteringStreamingPreview,
  mixingScenePresetJson,
  mixingScenePresetNames,
  mixStereo,
  normalize,
  noteStretch,
  percussive,
  pitchCorrectToMidi,
  pitchCorrectToMidiTimevarying,
  pitchShift,
  timeStretch,
  voiceChange,
  voiceChangeRealtime,
} from './effects_mastering';
export type { MelodyOptions } from './feature_music';
export {
  amplitudeToDb,
  analyzeMelody,
  analyzeSections,
  bassChroma,
  chroma,
  chromaCens,
  cqt,
  cyclicTempogram,
  dbToAmplitude,
  dbToPower,
  decompose,
  decomposeWithInit,
  deemphasis,
  ebur128LoudnessRange,
  estimateTuning,
  fixFrames,
  fixLength,
  fourierTempogram,
  frameSignal,
  framesToSamples,
  framesToTime,
  hpssWithResidual,
  hybridCqt,
  hzToMel,
  hzToMidi,
  hzToNote,
  lufs,
  lufsInterleaved,
  melSpectrogram,
  melToAudio,
  melToHz,
  melToStft,
  mfcc,
  mfccToAudio,
  mfccToMel,
  midiToHz,
  momentaryLufs,
  nnFilter,
  nnlsChroma,
  noteToHz,
  onsetEnvelope,
  onsetStrengthMulti,
  padCenter,
  pcen,
  peakPick,
  phaseVocoder,
  pitchPyin,
  pitchTuning,
  pitchYin,
  plp,
  polyFeatures,
  powerToDb,
  preemphasis,
  pseudoCqt,
  remix,
  resample,
  rmsEnergy,
  samplesToFrames,
  shortTermLufs,
  spectralBandwidth,
  spectralCentroid,
  spectralContrast,
  spectralFlatness,
  spectralRolloff,
  splitSilence,
  stft,
  stftDb,
  tempogram,
  tempogramRatio,
  timeToFrames,
  tonnetz,
  trim,
  trimSilence,
  vectorNormalize,
  vqt,
  zeroCrossingRate,
  zeroCrossings,
} from './features';
export type {
  ClippingRegion,
  ClippingReport,
  DynamicRangeReport,
  MeteringDetectClippingOptions,
  MeteringDynamicRangeOptions,
  PhaseScopeReport,
  SpectrumOptions,
  SpectrumReport,
  VectorscopeReport,
} from './metering';
export {
  meteringCrestFactorDb,
  meteringDcOffset,
  meteringDetectClipping,
  meteringDynamicRange,
  meteringPeakDb,
  meteringPhaseScope,
  meteringPhaseScopeDecimated,
  meteringRmsDb,
  meteringSpectrum,
  meteringSpectrumFrame,
  meteringStereoCorrelation,
  meteringStereoWidth,
  meteringTruePeakDb,
  meteringVectorscope,
  meteringVectorscopeDecimated,
} from './metering';
export type {
  BuiltinSynthBinding,
  BuiltinSynthConfig,
  BuiltinSynthWaveform,
  MidiCcLearnOptions,
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
  ProjectNotePairValidation,
  ProjectTrackDesc,
  ProjectTrackKind,
  ProjectWarpAnchor,
  ProjectWarpMapDesc,
  Sf2InstrumentConfig,
  Sf2ProgramStatus,
  SourceBackend,
} from './project';
export { EXPECTED_PROJECT_ABI_VERSION, Project, projectAbiVersion } from './project';
export type {
  AcousticOptions,
  AcousticResult,
  AnalysisResult,
  AnalyzeBpmOptions,
  AnalyzeDynamicsOptions,
  AnalyzeRhythmOptions,
  AnalyzeSectionsOptions,
  AnalyzeTimbreOptions,
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
  MasteringOptions,
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
  NoteStretchOptions,
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
  StreamingMasteringChainConfig,
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
export type {
  BpmAnalysisResult,
  BpmCandidate,
  DynamicsAnalysisResult,
  RhythmAnalysisResult,
  TimbreAnalysisResult,
  TimbreFrame,
} from './quick_analysis';
export {
  analyze,
  analyzeBpm,
  analyzeDynamics,
  analyzeImpulseResponse,
  analyzeRhythm,
  analyzeTimbre,
  analyzeWithProgress,
  chordFunctionalAnalysis,
  detectAcoustic,
  detectBeats,
  detectBpm,
  detectChords,
  detectDownbeats,
  detectKey,
  detectKeyCandidates,
  detectOnsets,
  estimateRoom,
  hasFfmpegSupport,
  roomMorph,
  synthesizeRir,
} from './quick_analysis';
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
  MidiCcBindOptions,
} from './realtime_engine';
export { EXPECTED_ENGINE_ABI_VERSION, engineCapabilities, RealtimeEngine } from './realtime_engine';
export { scaleCorrectionSemitones, scalePitchClassEnabled, scaleQuantizeMidi } from './scale';
export type { ProgressCallback } from './sonare.js';
export { StreamAnalyzer } from './stream_analyzer';
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
  StreamQuantizeConfig,
} from './stream_types';
export type {
  MixerRealtimeBuffer,
  RealtimeVoiceChangerInterleavedBuffer,
  RealtimeVoiceChangerMonoBuffer,
  RealtimeVoiceChangerPlanarBuffer,
} from './streaming_mixing';
export {
  Mixer,
  RealtimeVoiceChanger,
  realtimeVoiceChangerPresetJson,
  realtimeVoiceChangerPresetNames,
  StreamingEqualizer,
  StreamingMasteringChain,
  StreamingRetune,
  validateRealtimeVoiceChangerPresetJson,
} from './streaming_mixing';
export type { ValidateOptions } from './validation';

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
