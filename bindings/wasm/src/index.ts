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
import type {
  CapabilityCatalog,
  RealtimeVoiceChangerPodConfig,
  SonareCapabilities,
  VoicePresetId,
} from './public_types';
import type {
  SonareModule,
  WasmDecomposeResult,
  WasmHpssWithResidualResult,
  WasmMatrix2dResult,
} from './sonare.js';

export type { BrowserAudioDecodeOptions } from './audio';
export { Audio } from './audio';
export type {
  ClipPageStreamerEngine,
  ClipPageStreamerOptions,
  ClipPageStreamerRequest,
  ClipPageStreamSource,
  OpfsClipStream,
  OpfsClipStreamOptions,
  WorkletOpfsClipStreamHost,
} from './clip_page_streamer';
export { attachOpfsClipStream, ClipPageStreamer } from './clip_page_streamer';
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
  MasteringAssistantParamsRequest,
  MasteringAssistantStereoParamsRequest,
  MasteringChannelPolicy,
  MasteringDynamicsCompressorRequest,
  MasteringDynamicsGateRequest,
  MasteringDynamicsTransientShaperRequest,
  MasteringInsertParamInfo,
  MasteringPairAnalyzeRequest,
  MasteringPairProcessRequest,
  MasteringProcessorCatalogEntry,
  MasteringProcessRequest,
  MasteringProcessStereoRequest,
  MasteringRealtimeCost,
  MasteringRepairDeclickRequest,
  MasteringRepairDeclipRequest,
  MasteringRepairDecrackleRequest,
  MasteringRepairDehumRequest,
  MasteringRepairDenoiseClassicalRequest,
  MasteringRepairDereverbClassicalRequest,
  MasteringRepairTrimSilenceRequest,
  MasteringSamplesParamsRequest,
  MasteringStereoAnalyzeRequest,
  MasteringStereoParamsRequest,
  MasteringStreamingPreviewRequest,
  MasteringStreamingPreviewStereoRequest,
  MixStereoRequest,
  TransientShaperOptions,
  TrimSilenceMode,
  TrimSilenceOptions,
  VoiceChangeOptions,
  VoiceChangeRealtimeOptions,
  VoiceChangeRealtimeRequest,
  VoiceChangeRequest,
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
  masteringAssistantSuggestStereo,
  masteringAudioProfile,
  masteringAudioProfileStereo,
  masteringChain,
  masteringChainStereo,
  masteringChainStereoWithProgress,
  masteringChainWithProgress,
  masteringDynamicsCompressor,
  masteringDynamicsGate,
  masteringDynamicsTransientShaper,
  masteringInsertNames,
  masteringInsertParamInfo,
  masteringInsertParamNames,
  masteringPairAnalysisNames,
  masteringPairAnalyze,
  masteringPairProcess,
  masteringPairProcessorNames,
  masteringPlatformNames,
  masteringPresetNames,
  masteringProcess,
  masteringProcessorCatalog,
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
  masteringStreamingPreviewStereo,
  mixingScenePresetJson,
  mixingScenePresetNames,
  mixStereo,
  normalize,
  noteMove,
  noteStretch,
  percussive,
  pitchCorrectTimevarying,
  pitchCorrectToMidi,
  pitchCorrectToMidiTimevarying,
  pitchShift,
  spectralEdit,
  timeStretch,
  voiceChange,
  voiceChangeRealtime,
} from './effects_mastering';
export type {
  HarmonicRequest,
  HpssRequest,
  NormalizeMode,
  NormalizeRequest,
  NoteMoveRequest,
  NoteStretchRequest,
  PercussiveRequest,
  PitchCorrectTimevaryingRequest,
  PitchCorrectToMidiRequest,
  PitchCorrectToMidiTimevaryingRequest,
  PitchShiftRequest,
  SpectralEditRequest,
  TimeStretchRequest,
} from './effects_transform';
export { ErrorCode, isSonareError, SonareError } from './errors';
export type {
  ChirpRequest,
  ClicksRequest,
  CyclicTempogramRequest,
  DbConversionRequest,
  EmphasisRequest,
  FixFramesRequest,
  FixLengthRequest,
  FrameSignalRequest,
  OnsetBacktrackRequest,
  PadCenterRequest,
  PcenRequest,
  PeakPickRequest,
  PlpRequest,
  SilenceRequest,
  TempogramRequest,
  ToneRequest,
  TonnetzRequest,
  VectorNormalizeRequest,
} from './feature_core';
export type {
  AnalyzeMelodyRequest,
  AnalyzeSectionsRequest,
  CqtRequest,
  CqtToAudioRequest,
  FourierTempogramRequest,
  LufsRequest,
  MelodyOptions,
  NnlsChromaRequest,
  OnsetEnvelopeRequest,
  OnsetStrengthMultiRequest,
  TempogramRatioRequest,
  VqtRequest,
  VqtToAudioRequest,
} from './feature_music';
export type {
  NoteSegmentsRequest,
  PiptrackRequest,
  PitchPyinRequest,
  PitchYinRequest,
} from './feature_pitch';
export type { ResampleRequest } from './feature_resample';
export type {
  DecomposeRequest,
  DecomposeStemsRequest,
  DecomposeStemsResult,
  DecomposeWithInitRequest,
  Ebur128LoudnessRangeRequest,
  EstimateTuningRequest,
  HpssWithResidualRequest,
  LufsInterleavedRequest,
  NnFilterRequest,
  PhaseVocoderRequest,
  PitchTuningRequest,
  PolyFeaturesRequest,
  RemixRequest,
  SegmentAgglomerativeRequest,
  SegmentCrossSimilarityRequest,
  SegmentLagToRecurrenceRequest,
  SegmentPathEnhanceRequest,
  SegmentRecurrenceMatrixRequest,
  SegmentRecurrenceToLagRequest,
  SegmentSubsegmentRequest,
  SpectralContrastRequest,
  SpectralFrameRequest,
  SpectralRolloffRequest,
  ZeroCrossingRateRequest,
  ZeroCrossingsRequest,
} from './feature_spectral';
export type {
  BassChromaSpectrogramRequest,
  ChromaSpectrogramRequest,
  GriffinLimRequest,
  MelDeltaRequest,
  MelSpectrogramRequest,
  MelToAudioRequest,
  MelToStftRequest,
  MfccRequest,
  MfccToAudioRequest,
  MfccToMelRequest,
  ReassignedSpectrogramRequest,
  SpectrogramRequest,
  TrimRequest,
} from './feature_spectrogram';
export {
  amplitudeToDb,
  analyzeMelody,
  analyzeSections,
  bassChroma,
  chirp,
  chroma,
  chromaCens,
  chromaCqt,
  clicks,
  cqt,
  cqtToAudio,
  cyclicTempogram,
  dbToAmplitude,
  dbToPower,
  decompose,
  decomposeStems,
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
  griffinLim,
  hpssWithResidual,
  hybridCqt,
  hzToMel,
  hzToMidi,
  hzToNote,
  lufs,
  lufsInterleaved,
  melDelta,
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
  noteSegments,
  noteToHz,
  onsetBacktrack,
  onsetEnvelope,
  onsetStrengthMulti,
  padCenter,
  pcen,
  peakPick,
  phaseVocoder,
  piptrack,
  pitchPyin,
  pitchTuning,
  pitchYin,
  plp,
  polyFeatures,
  powerToDb,
  preemphasis,
  pseudoCqt,
  reassignedSpectrogram,
  remix,
  remixAlignedIntervals,
  resample,
  rmsEnergy,
  samplesToFrames,
  segmentAgglomerative,
  segmentCrossSimilarity,
  segmentLagToRecurrence,
  segmentPathEnhance,
  segmentRecurrenceMatrix,
  segmentRecurrenceToLag,
  segmentSubsegment,
  shortTermLufs,
  spectralBandwidth,
  spectralCentroid,
  spectralContrast,
  spectralFlatness,
  spectralFlux,
  spectralRolloff,
  splitSilence,
  stft,
  stftDb,
  tempogram,
  tempogramRatio,
  timeToFrames,
  tone,
  tonnetz,
  trim,
  trimSilence,
  vectorNormalize,
  vqt,
  vqtToAudio,
  zeroCrossingRate,
  zeroCrossings,
} from './features';
export type { BindMicrophoneInputOptions, MicrophoneInputBinding } from './live_audio';
export { bindMicrophoneInput } from './live_audio';
export type {
  MasterAudioRequest,
  MasterAudioStereoRequest,
  MasteringChainRequest,
  MasteringChainStereoRequest,
} from './mastering_chain';
export type { MasteringRequest } from './mastering_core';
export type {
  ClippingRegion,
  ClippingReport,
  DynamicRangeReport,
  MeteringDetectClippingOptions,
  MeteringDetectClippingRequest,
  MeteringDynamicRangeOptions,
  MeteringDynamicRangeRequest,
  MeteringSamplesRequest,
  MeteringSilenceRatioRequest,
  MeteringSpectrumFrameRequest,
  MeteringSpectrumRequest,
  MeteringStereoDecimatedRequest,
  MeteringStereoRequest,
  MeteringTruePeakRequest,
  PhaseScopeReport,
  SpectrumOptions,
  SpectrumReport,
  VectorscopeReport,
  WaveformPeakPyramidOptions,
  WaveformPeakPyramidRequest,
  WaveformPeaksOptions,
  WaveformPeaksReport,
  WaveformPeaksRequest,
} from './metering';
export {
  meteringCrestFactorDb,
  meteringCrestFactorDbStereo,
  meteringDcOffset,
  meteringDetectClipping,
  meteringDynamicRange,
  meteringPeakDb,
  meteringPhaseScope,
  meteringPhaseScopeDecimated,
  meteringRmsDb,
  meteringSilenceRatio,
  meteringSpectrum,
  meteringSpectrumFrame,
  meteringStereoCorrelation,
  meteringStereoWidth,
  meteringTruePeakDb,
  meteringVectorscope,
  meteringVectorscopeDecimated,
  waveformPeakPyramid,
  waveformPeaks,
} from './metering';
export type { SuggestMixSceneRequest } from './mixing_assistant';
export {
  mixSourceClassFromName,
  mixSourceClassNames,
  suggestMixScene,
  suggestMixSceneJson,
} from './mixing_assistant';
export type {
  OpfsClipPageProviderBinding,
  OpfsClipPageProviderOptions,
} from './opfs_clip_pages';
export {
  createOpfsClipPageProvider,
  createOpfsClipPageWorker,
  opfsClipPageWorkerSource,
} from './opfs_clip_pages';
export type {
  BuiltinSynthBinding,
  BuiltinSynthConfig,
  BuiltinSynthWaveform,
  ExternalSeparatedStem,
  ExternalSeparatedStemImportRequest,
  ExternalSeparatedStemImportResult,
  MidiCcLearnOptions,
  ProjectAssistSidecar,
  ProjectAssistSidecarInput,
  ProjectAutomationCurve,
  ProjectAutomationLaneDesc,
  ProjectAutomationPoint,
  ProjectAutomationTargetKind,
  ProjectBounceOptions,
  ProjectChordSymbol,
  ProjectClip,
  ProjectClipCompSegment,
  ProjectClipDesc,
  ProjectClipFade,
  ProjectClipTake,
  ProjectCompileResult,
  ProjectFadeCurve,
  ProjectKeySegment,
  ProjectLoopMode,
  ProjectLoopRecordingDesc,
  ProjectLoopRecordingResult,
  ProjectMarker,
  ProjectMidiClipResult,
  ProjectMidiEvent,
  ProjectMidiFxBakeRequest,
  ProjectMidiFxBakeResult,
  ProjectMidiFxPreviewRequest,
  ProjectNotePairValidation,
  ProjectSource,
  ProjectTempoCandidate,
  ProjectTempoOptions,
  ProjectTempoSegment,
  ProjectTimeSignatureSegment,
  ProjectTrack,
  ProjectTrackDesc,
  ProjectTrackKind,
  ProjectWarpAnchor,
  ProjectWarpMapDesc,
  Sf2InstrumentConfig,
  Sf2ProgramStatus,
  SourceBackend,
  SynthBodyType,
  SynthEngineMode,
  SynthEnumTables,
  SynthFilterModel,
  SynthFilterOutput,
  SynthModDestination,
  SynthModRouting,
  SynthModSource,
  SynthOscWaveform,
  SynthPatch,
} from './project';
export {
  AutomationTargetKind,
  BUILTIN_SYNTH_WAVEFORMS,
  EXPECTED_PROJECT_ABI_VERSION,
  MarkerKind,
  PROJECT_AUTOMATION_TARGET_OPAQUE,
  PROJECT_AUTOMATION_TARGET_TRACK_FADER_DB,
  PROJECT_AUTOMATION_TARGET_TRACK_PAN,
  Project,
  projectAbiVersion,
  SYNTH_BODY_TYPES,
  SYNTH_ENGINE_MODES,
  SYNTH_FILTER_MODELS,
  SYNTH_FILTER_OUTPUTS,
  SYNTH_MOD_DESTINATIONS,
  SYNTH_MOD_SOURCES,
  SYNTH_OSC_WAVEFORMS,
  synthEnumTables,
  synthPresetNames,
  synthPresetPatch,
} from './project';
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
  BpmHypothesis,
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
  MasteringAssistantParams,
  MasteringChainConfig,
  MasteringChainResult,
  MasteringChainStereoResult,
  MasteringLoudnessSummary,
  MasteringOptions,
  MasteringPreset,
  MasteringProcessorParams,
  MasteringReport,
  MasteringResult,
  MasteringStereoChainResult,
  MasteringStereoResult,
  MelodyPoint,
  MelodyResult,
  MelPowerResult,
  MelSpectrogramResult,
  MeterTap,
  MfccResult,
  MixAnalysisBand,
  MixAssistantMixProfile,
  MixAssistantOptions,
  MixAssistantResult,
  MixAssistantTrack,
  MixAssistantTrackProfile,
  MixBandDominance,
  MixBandOccupancy,
  MixCrowdedBand,
  MixerProcessResult,
  MixMeterSnapshot,
  MixMonoRisk,
  MixOptions,
  MixResult,
  MixTrackAlignment,
  NoteSegment,
  NoteStretchOptions,
  PairAnalysis,
  PairProcessor,
  PanLaw,
  PanLawInput,
  PanLawName,
  PanMode,
  PitchCorrectOptions,
  PitchResult,
  RealtimeVoiceChangerConfigInput,
  RealtimeVoiceChangerPodConfig,
  RhythmFeatures,
  RirDiagnostic,
  RirResult,
  RirSynthOptions,
  RoomEstimateOptions,
  RoomEstimateResult,
  RoomGeometryOptions,
  RoomMorphOptions,
  Section,
  SegmentMatrix,
  SendTiming,
  SoloProcessor,
  SonareCapabilities,
  SpectralEditMode,
  SpectralEditOptions,
  SpectralEditWindow,
  SpectralRegionOp,
  StageGainReduction,
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
  VoicedFlags,
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
  AnalyzeBpmRequest,
  AnalyzeDynamicsRequest,
  AnalyzeImpulseResponseRequest,
  AnalyzeRhythmRequest,
  AnalyzeTimbreRequest,
  AnalyzeWithProgressRequest,
  BpmAnalysisResult,
  BpmCandidate,
  ChordFunctionalAnalysisRequest,
  DetectAcousticRequest,
  DetectChordsRequest,
  DetectKeyRequest,
  DetectOnsetsRequest,
  DynamicsAnalysisResult,
  EstimateMeterRequest,
  EstimateRoomRequest,
  MusicAnalyzeRequest,
  RhythmAnalysisResult,
  RoomMorphRequest,
  SamplesRequest,
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
  estimateMeter,
  estimateRoom,
  hasFfmpegSupport,
  roomMorph,
  synthesizeRir,
} from './quick_analysis';
export type {
  ClipPageRequest,
  EngineAutomationPoint,
  EngineBounceOptions,
  EngineBounceResult,
  EngineBus,
  EngineCapabilities,
  EngineCaptureSource,
  EngineCaptureStatus,
  EngineClip,
  EngineFreezeOptions,
  EngineFreezeResult,
  EngineGraphSpec,
  EngineMarker,
  EngineMeterTelemetry,
  EngineMeterTelemetryWide,
  EngineMetronomeConfig,
  EngineMidiClipSchedule,
  EngineMidiEvent,
  EngineParameterInfo,
  EngineScopeTelemetry,
  EngineTelemetry,
  EngineTempoSegment,
  EngineTimeSignatureSegment,
  EngineTrackLane,
  EngineTrackMonitorMode,
  EngineTrackSend,
  EngineTransportState,
  ExternalMidiEvent,
  MidiCcBindOptions,
  TrackMonitorMode,
} from './realtime_engine';
export {
  ClipPageProvider,
  EXPECTED_ENGINE_ABI_VERSION,
  engineCapabilities,
  RealtimeEngine,
} from './realtime_engine';
export { scaleCorrectionSemitones, scalePitchClassEnabled, scaleQuantizeMidi } from './scale';
export type { ProgressCallback } from './sonare.js';
export { StreamAnalyzer, streamAnalyzerConfigDefaults } from './stream_analyzer';
export type {
  AnalyzerStats,
  BarChord,
  ChordChange,
  FrameBuffer,
  PatternScore,
  ProgressiveEstimate,
  StreamConfig,
  StreamConfigDefaults,
  StreamFramesI16,
  StreamFramesU8,
  StreamQuantizeConfig,
} from './stream_types';
export type {
  MixerMeterSnapshot,
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
export type {
  BindWebMidiOptions,
  WebMidiBinding,
  WebMidiCcBinding,
  WebMidiInputInfo,
} from './web_midi';
export { bindWebMidi, isWebMidiAvailable } from './web_midi';
export type {
  OfflineWorker,
  OfflineWorkerCallOptions,
  OfflineWorkerClientOptions,
  OfflineWorkerProgress,
} from './worker_client';
export { OfflineWorkerClient, OfflineWorkerTask } from './worker_client';

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
  wasmBinary?: ArrayBuffer | Uint8Array;
  moduleFactory?: (options?: {
    locateFile?: (path: string, prefix: string) => string;
    wasmBinary?: ArrayBuffer | Uint8Array;
  }) => Promise<SonareModule>;
}): Promise<void> {
  if (module) {
    return;
  }

  if (initPromise) {
    return initPromise;
  }

  initPromise = (async () => {
    try {
      const createModule = options?.moduleFactory ?? (await import('./sonare.js')).default;
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

/**
 * Return the capabilities of the loaded WASM build.
 *
 * This is synchronous and only describes the already-initialized module.
 */
export function capabilities(): SonareCapabilities {
  if (!module) {
    throw new Error('Module not initialized. Call init() first.');
  }
  return module.capabilities();
}

/** Return the initialized module's processors, parameters, and presets. */
export function capabilityCatalog(): CapabilityCatalog {
  if (!module) {
    throw new Error('Module not initialized. Call init() first.');
  }
  return JSON.parse(module.capabilityCatalog()) as CapabilityCatalog;
}

/**
 * Aggregate native ABI version: the per-subsystem ABI macros folded into one
 * 32-bit value. It bumps whenever any flat C POD layout changes, so callers can
 * detect an incompatible prebuilt binary. Matches the Node/Python `abiVersion()`.
 */
export function abiVersion(): number {
  if (!module) {
    throw new Error('Module not initialized. Call init() first.');
  }
  return module.abiVersion();
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
    if (!Number.isSafeInteger(preset) || preset < 0 || preset >= VOICE_PRESET_ORDINALS.length) {
      throw new RangeError(`Unknown voice-character preset ordinal: ${String(preset)}`);
    }
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
 * string (e.g. `'bright-idol'`). Unknown numeric ordinals return `null`;
 * unknown preset ids throw.
 */
export function voiceCharacterPresetId(preset: VoicePresetId | number): VoicePresetId | null {
  if (!module) {
    throw new Error('Module not initialized. Call init() first.');
  }
  if (
    typeof preset === 'number' &&
    (!Number.isSafeInteger(preset) || preset < 0 || preset >= VOICE_PRESET_ORDINALS.length)
  ) {
    return null;
  }
  return module.voiceCharacterPresetId(resolveVoicePresetOrdinal(preset)) as VoicePresetId;
}

/**
 * Return the canonical (normalized) flat POD config for a built-in voice
 * preset, skipping the JSON round-trip. Accepts a canonical preset id or its
 * integer ordinal. Invalid ordinals throw.
 */
export function realtimeVoiceChangerPresetConfig(
  preset: VoicePresetId | number,
): RealtimeVoiceChangerPodConfig {
  if (!module) {
    throw new Error('Module not initialized. Call init() first.');
  }
  return module.realtimeVoiceChangerPresetConfig(resolveVoicePresetOrdinal(preset));
}

// ============================================================================
// Re-exports
// ============================================================================

export { PitchClass as Pitch } from './public_types';
