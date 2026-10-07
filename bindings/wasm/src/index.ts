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

import './lifetime.js';
import {
  assertAbiCompatible,
  assertSameInitOptions,
  notInitializedError,
  setSonareModule,
} from './module_state.js';
import type {
  CapabilityCatalog,
  RealtimeVoiceChangerPodConfig,
  SonareCapabilities,
  VoicePresetId,
} from './public_types.js';
import type {
  SonareModule,
  WasmDecomposeResult,
  WasmHpssWithResidualResult,
  WasmMatrix2dResult,
} from './sonare.js';

export { alignTakeToReference } from './align_take.js';
export type {
  BrowserAudioDecodeOptions,
  ChannelLayout,
  DecodedChannels,
} from './audio.js';
export { Audio, decodeChannels, downmix } from './audio.js';
export type {
  ClipPageStreamerEngine,
  ClipPageStreamerOptions,
  ClipPageStreamerRequest,
  ClipPageStreamSource,
  OpfsClipStream,
  OpfsClipStreamOptions,
  WorkletOpfsClipStreamHost,
} from './clip_page_streamer.js';
export { attachOpfsClipStream, ClipPageStreamer } from './clip_page_streamer.js';
export type {
  CompressorDetector,
  CompressorOptions,
  DeclickOptions,
  DeclipOptions,
  DecrackleMode,
  DecrackleOptions,
  DehumMode,
  DehumOptions,
  DenoiseClassicalMode,
  DenoiseClassicalNoiseEstimator,
  DenoiseClassicalOptions,
  DereverbClassicalOptions,
  DynamicsProcessorResult,
  GateOptions,
  MasteringAbMatchLoudnessRequest,
  MasteringAbMatchLoudnessStereoRequest,
  MasteringAmpPresetCatalogEntry,
  MasteringAssistantParamsRequest,
  MasteringAssistantStereoParamsRequest,
  MasteringChannelPolicy,
  MasteringDynamicsCompressorRequest,
  MasteringDynamicsGateRequest,
  MasteringDynamicsTransientShaperRequest,
  MasteringInsertParamChoice,
  MasteringInsertParamInfo,
  MasteringInsertSlot,
  MasteringInsertTiming,
  MasteringPairAnalyzeRequest,
  MasteringPairProcessRequest,
  MasteringPairProcessStereoRequest,
  MasteringProcessorCatalogEntry,
  MasteringProcessorCategory,
  MasteringProcessRequest,
  MasteringProcessStereoRequest,
  MasteringRealtimeCost,
  MasteringRepairDeclickRequest,
  MasteringRepairDeclickStereoRequest,
  MasteringRepairDeclipRequest,
  MasteringRepairDeclipStereoRequest,
  MasteringRepairDecrackleRequest,
  MasteringRepairDecrackleStereoRequest,
  MasteringRepairDehumRequest,
  MasteringRepairDehumStereoRequest,
  MasteringRepairDenoiseClassicalLinkedRequest,
  MasteringRepairDenoiseClassicalRequest,
  MasteringRepairDenoiseClassicalStereoRequest,
  MasteringRepairDereverbClassicalLinkedRequest,
  MasteringRepairDereverbClassicalRequest,
  MasteringRepairDereverbClassicalStereoRequest,
  MasteringRepairDereverbConfigForRoomRequest,
  MasteringRepairDetectClicksRequest,
  MasteringRepairDetectClippingRequest,
  MasteringRepairDetectCrackleRequest,
  MasteringRepairDetectHumRequest,
  MasteringRepairDetectNoiseFloorRequest,
  MasteringRepairDetectReverbRequest,
  MasteringRepairDetectTrimRangeRequest,
  MasteringRepairDetectTrimRangeStereoRequest,
  MasteringRepairNoiseBandBinsRequest,
  MasteringRepairTrimSilenceRequest,
  MasteringRepairTrimSilenceStereoRequest,
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
} from './effects_mastering.js';
export {
  assignNoteTargets,
  decomposeNotePitch,
  extractNotes,
  extractPercussiveEvents,
  harmonic,
  hpss,
  masterAudio,
  masterAudioStereo,
  masterAudioStereoWithProgress,
  masterAudioWithProgress,
  mastering,
  masteringAbMatchLoudness,
  masteringAbMatchLoudnessStereo,
  masteringAmpPresetCatalog,
  masteringAssistantSuggest,
  masteringAssistantSuggestChain,
  masteringAssistantSuggestChainStereo,
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
  masteringInsertTiming,
  masteringPairAnalysisNames,
  masteringPairAnalyze,
  masteringPairProcess,
  masteringPairProcessorNames,
  masteringPairProcessStereo,
  masteringPlatformNames,
  masteringPresetNames,
  masteringPresetParams,
  masteringProcess,
  masteringProcessorCatalog,
  masteringProcessorNames,
  masteringProcessStereo,
  masteringRepairDeclick,
  masteringRepairDeclickStereo,
  masteringRepairDeclip,
  masteringRepairDeclipStereo,
  masteringRepairDecrackle,
  masteringRepairDecrackleStereo,
  masteringRepairDehum,
  masteringRepairDehumStereo,
  masteringRepairDenoiseClassical,
  masteringRepairDenoiseClassicalLinked,
  masteringRepairDenoiseClassicalStereo,
  masteringRepairDereverbClassical,
  masteringRepairDereverbClassicalLinked,
  masteringRepairDereverbClassicalStereo,
  masteringRepairDereverbConfigForRoom,
  masteringRepairDetectClicks,
  masteringRepairDetectClipping,
  masteringRepairDetectCrackle,
  masteringRepairDetectHum,
  masteringRepairDetectNoiseFloor,
  masteringRepairDetectReverb,
  masteringRepairDetectTrimRange,
  masteringRepairDetectTrimRangeStereo,
  masteringRepairNoiseBandBins,
  masteringRepairTrimSilence,
  masteringRepairTrimSilenceStereo,
  masteringStereoAnalysisNames,
  masteringStereoAnalyze,
  masteringStreamingPreview,
  masteringStreamingPreviewStereo,
  mergeNotes,
  mixingScenePresetJson,
  mixingScenePresetNames,
  mixStereo,
  normalize,
  normalizeStereo,
  noteMove,
  noteStretch,
  noteTargetsFromSmf,
  percussive,
  pitchCorrectTimevarying,
  pitchCorrectToMidi,
  pitchCorrectToMidiTimevarying,
  pitchShift,
  renderNotes,
  renderPercussiveEvents,
  spectralEdit,
  splitNote,
  timeStretch,
  voiceChange,
  voiceChangeRealtime,
} from './effects_mastering.js';
export type {
  AssignNoteTargetsRequest,
  DecomposeNotePitchRequest,
  ExtractNotesRequest,
  MergeNotesRequest,
  NoteMoveRequest,
  NoteSetRequest,
  NoteStretchRequest,
  NoteTargetsFromSmfRequest,
  RenderNotesRequest,
  SplitNoteRequest,
} from './effects_note_ops.js';
export type {
  ExtractPercussiveEventsRequest,
  PercussiveSeparationOptions,
  RenderPercussiveEventsRequest,
} from './effects_percussive.js';
export type { HarmonicRequest, HpssRequest, PercussiveRequest } from './effects_separation.js';
export type { SpectralEditRequest } from './effects_spectral.js';
export type {
  PitchCorrectTimevaryingRequest,
  PitchCorrectToMidiRequest,
  PitchCorrectToMidiTimevaryingRequest,
  PitchShiftRequest,
  TimeStretchRequest,
} from './effects_timepitch.js';
export { ErrorCode, isSonareError, SonareError } from './errors.js';
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
  SilenceCommonReport,
  SilenceRequest,
  SplitSilenceCommonRequest,
  SplitSilenceCommonWithReportResult,
  TempogramRequest,
  ToneRequest,
  TonnetzRequest,
  VectorNormalizeRequest,
} from './feature_core.js';
export type {
  DecomposeRequest,
  DecomposeStemsLinkedRequest,
  DecomposeStemsLinkedResult,
  DecomposeStemsRequest,
  DecomposeStemsResult,
  DecomposeWithInitRequest,
  HpssWithResidualRequest,
  NnFilterRequest,
  RemixRequest,
  SegmentAgglomerativeRequest,
  SegmentCrossSimilarityRequest,
  SegmentLagToRecurrenceRequest,
  SegmentPathEnhanceRequest,
  SegmentRecurrenceMatrixRequest,
  SegmentRecurrenceToLagRequest,
  SegmentSubsegmentRequest,
} from './feature_decompose.js';
export type {
  GriffinLimRequest,
  MelToAudioRequest,
  MelToStftRequest,
  MfccToAudioRequest,
  MfccToMelRequest,
  PhaseVocoderRequest,
} from './feature_inverse.js';
export type {
  Ebur128LoudnessRangeRequest,
  LufsInterleavedRequest,
  LufsSeriesInterleavedRequest,
} from './feature_loudness.js';
export type {
  AnalyzeMelodyRequest,
  AnalyzeSectionsRequest,
  CqtRequest,
  CqtToAudioRequest,
  DetectBoundariesRequest,
  FourierTempogramRequest,
  LufsRequest,
  MelodyOptions,
  NnlsChromaRequest,
  OnsetEnvelopeRequest,
  OnsetStrengthMultiRequest,
  TempogramRatioRequest,
  VqtRequest,
  VqtToAudioRequest,
} from './feature_music.js';
export type {
  EstimateTuningRequest,
  NoteSegmentsRequest,
  PiptrackRequest,
  PitchPyinRequest,
  PitchTuningRequest,
  PitchYinRequest,
} from './feature_pitch.js';
export type { ResampleRequest } from './feature_resample.js';
export type {
  PolyFeaturesRequest,
  SpectralContrastRequest,
  SpectralFrameRequest,
  SpectralRolloffRequest,
  ZeroCrossingRateRequest,
  ZeroCrossingsRequest,
} from './feature_spectral.js';
export type {
  BassChromaSpectrogramRequest,
  ChromaSpectrogramRequest,
  MelDeltaRequest,
  MelSpectrogramRequest,
  MfccRequest,
  ReassignedSpectrogramRequest,
  SpectrogramRequest,
  TrimRequest,
} from './feature_spectrogram.js';
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
  decomposeStemsLinked,
  decomposeWithInit,
  deemphasis,
  detectBoundaries,
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
  lufsSeriesInterleaved,
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
  splitSilenceCommon,
  splitSilenceCommonWithReport,
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
} from './features.js';
export type { BindMicrophoneInputOptions, MicrophoneInputBinding } from './live_audio.js';
export { bindMicrophoneInput } from './live_audio.js';
export type {
  MasterAudioRequest,
  MasterAudioStereoRequest,
  MasteringChainRequest,
  MasteringChainStereoRequest,
  NormalizeMode,
  NormalizeRequest,
  NormalizeStereoRequest,
  NormalizeStereoResult,
} from './mastering_chain.js';
export type { MasteringRequest } from './mastering_core.js';
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
} from './metering.js';
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
} from './metering.js';
export type { SuggestMixSceneRequest } from './mixing_assistant.js';
export {
  mixSourceClassFromName,
  mixSourceClassNames,
  suggestMixScene,
  suggestMixSceneJson,
} from './mixing_assistant.js';
export type {
  OpfsClipImportResult,
  OpfsClipPageProviderBinding,
  OpfsClipPageProviderOptions,
  OpfsClipWriteOptions,
  OpfsClipWriteResult,
} from './opfs_clip_pages.js';
export {
  createOpfsClipPageProvider,
  createOpfsClipPageWorker,
  importOpfsClip,
  opfsClipPageWorkerSource,
  writeOpfsClip,
} from './opfs_clip_pages.js';
export {
  HrtfSet,
  PlaybackLoudnessMeter,
  PlaybackRenderer,
  renderPlayback,
} from './playback_renderer.js';
export type { AnalyzePolyphonicRequest } from './polyphony.js';
export { analyzePolyphonic, PolyphonicAnalysis } from './polyphony.js';
export type {
  AlignTakeToReferenceRequest,
  AlignTakeToReferenceResult,
  Articulation,
  BuiltinSynthBinding,
  BuiltinSynthConfig,
  BuiltinSynthWaveform,
  ControllerAxis,
  ControllerBinding,
  ControllerInput,
  ExternalInstrument,
  ExternalInstrumentEvent,
  ExternalSeparatedStem,
  ExternalSeparatedStemImportRequest,
  ExternalSeparatedStemImportResult,
  MidiCcLearnOptions,
  MpeDimension,
  NoteTracking,
  PartRig,
  PartRigEntry,
  PartRigInsert,
  PartRigKey,
  PartRigMode,
  PartRigRequest,
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
  ProjectCompileTimelineResult,
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
  ProjectTranscribeRequest,
  ProjectWarpAnchor,
  ProjectWarpMapDesc,
  SampleDesc,
  SampleDescLoopMode,
  SampleKeyTrack,
  SampleLoopMode,
  SampleZoneDesc,
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
  SynthRetrigger,
  TakeAlignment,
  TranscribeOptions,
  TranscribeResult,
} from './project.js';
export {
  ARTICULATIONS,
  AutomationTargetKind,
  BUILTIN_SYNTH_WAVEFORMS,
  CONTROLLER_AXES,
  CONTROLLER_INPUTS,
  controllerProfileNames,
  EXPECTED_PROJECT_ABI_VERSION,
  MarkerKind,
  MPE_DIMENSIONS,
  NOTE_TRACKINGS,
  PART_RIG_ALL_PARTS,
  PART_RIG_MODES,
  PROJECT_AUTOMATION_TARGET_OPAQUE,
  PROJECT_AUTOMATION_TARGET_TRACK_FADER_DB,
  PROJECT_AUTOMATION_TARGET_TRACK_PAN,
  Project,
  ProjectTimeline,
  projectAbiVersion,
  SAMPLE_KEY_TRACKS,
  SAMPLE_LOOP_MODES,
  SampleBank,
  SYNTH_BODY_TYPES,
  SYNTH_ENGINE_MODES,
  SYNTH_FILTER_MODELS,
  SYNTH_FILTER_OUTPUTS,
  SYNTH_MOD_DESTINATIONS,
  SYNTH_MOD_SOURCES,
  SYNTH_OSC_WAVEFORMS,
  SYNTH_RETRIGGERS,
  synthEnumTables,
  synthGsDrumKitIsVoicedApart,
  synthGsDrumKitName,
  synthGsVariationIsVoicedApart,
  synthPresetNames,
  synthPresetPatch,
} from './project.js';
export type {
  AcousticOptions,
  AcousticResult,
  AnalysisChord,
  AnalysisResult,
  AnalyzeBpmOptions,
  AnalyzeDynamicsOptions,
  AnalyzeRhythmOptions,
  AnalyzeSectionsOptions,
  AnalyzeTimbreOptions,
  AutomationCurve,
  Beat,
  Boundary,
  BoundaryOptions,
  BoundaryResult,
  BpmHypothesis,
  CapabilityCatalog,
  CapabilityCatalogMasteringPreset,
  CapabilityCatalogParameter,
  CapabilityCatalogPresets,
  CapabilityCatalogProcessor,
  Chord,
  ChordAnalysisResult,
  ChordDetectionOptions,
  ChromaResult,
  ClickDetection,
  ClipDetection,
  CqtResult,
  CrackleDetection,
  DeclickReport,
  DeclipReport,
  DecrackleReport,
  DehumReport,
  DenoiseReport,
  DereverbReport,
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
  HumDetection,
  Key,
  KeyCandidate,
  KeyDetectionOptions,
  KeyProfileName,
  LoudnessMatchResult,
  LoudnessMatchStereoResult,
  LufsResult,
  LufsSeriesResult,
  MasteringAssistantParams,
  MasteringChainConfig,
  MasteringChainResult,
  MasteringChainStereoResult,
  MasteringInsertParamDependency,
  MasteringInsertParamRelation,
  MasteringInsertParamScale,
  MasteringInsertParamUnit,
  MasteringLoudnessSummary,
  MasteringOptions,
  MasteringPreset,
  MasteringProcessorParams,
  MasteringRepairDeclickStereoResult,
  MasteringRepairDeclipStereoResult,
  MasteringRepairDecrackleStereoResult,
  MasteringRepairDehumStereoResult,
  MasteringRepairDenoiseClassicalLinkedResult,
  MasteringRepairDenoiseClassicalStereoResult,
  MasteringRepairDereverbClassicalLinkedResult,
  MasteringRepairDereverbClassicalStereoResult,
  MasteringRepairTrimSilenceStereoResult,
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
  MixSceneBus,
  MixSceneConnection,
  MixSceneDocument,
  MixSceneInsert,
  MixSceneSend,
  MixSceneStrip,
  MixSceneVcaGroup,
  MixTrackAlignment,
  NoiseDetection,
  NoteEdit,
  NoteEditInput,
  NoteExtractorOptions,
  NoteObject,
  NoteObjectInput,
  NoteSegment,
  NoteSetEntry,
  NoteStretchOptions,
  NoteTarget,
  NoteTargetAssignResult,
  NoteTargetUnmatchedPolicy,
  PairAnalysis,
  PairProcessor,
  PanLaw,
  PanLawInput,
  PanLawName,
  PanMode,
  PercussiveEvent,
  PercussiveEventEdit,
  PercussiveEventEditInput,
  PercussiveEventInput,
  PitchCorrectOptions,
  PitchDecompositionResult,
  PitchResult,
  PlaybackBassManagementConfig,
  PlaybackChannelRole,
  PlaybackDiagnostics,
  PlaybackHeadTrackingConfig,
  PlaybackInputConfig,
  PlaybackLoudnessConfig,
  PlaybackNightModeConfig,
  PlaybackOutputLimiterConfig,
  PlaybackRendererConfig,
  PlaybackRendererOptions,
  PlaybackRoomConfig,
  PlaybackSpeakerConfig,
  PlaybackStageLatency,
  PlaybackStageName,
  PlaybackTargetConfig,
  PlaybackUpmixConfig,
  PolyphonicAnalysisOptions,
  PolyphonicRenderOptions,
  RealtimeVoiceChangerConfigInput,
  RealtimeVoiceChangerPodConfig,
  RenderPlaybackRequest,
  RenderPlaybackResult,
  ReverbDetection,
  RhythmFeatures,
  RirDiagnostic,
  RirResult,
  RirSynthOptions,
  RoomEstimateOptions,
  RoomEstimateResult,
  RoomGeometryOptions,
  RoomMorphOptions,
  RoomMorphResult,
  Section,
  SegmentMatrix,
  SendTiming,
  SidechainCheck,
  SidechainRefusal,
  SidechainSourceKind,
  SoloProcessor,
  SonareCapabilities,
  SpectralEditMode,
  SpectralEditOptions,
  SpectralEditWindow,
  SpectralRegionOp,
  StageGainReduction,
  StereoAnalysis,
  StereoPairProcessor,
  StftPowerResult,
  StftResult,
  StreamingEqualizerConfig,
  StreamingMasteringChainConfig,
  StreamingPlatform,
  StreamingRetuneConfig,
  TempogramMode,
  Timbre,
  TimeSignature,
  TrimRange,
  TrimReport,
  UmpWords,
  VoicedFlags,
  VoicePresetId,
} from './public_types.js';
export {
  ChordQuality,
  KeyProfile,
  Mode,
  PitchClass,
  SectionType,
} from './public_types.js';
export type * from './public_types_vocal_edit.js';
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
  DynamicsResult,
  EstimateMeterRequest,
  EstimateRoomRequest,
  MusicAnalyzeRequest,
  RhythmAnalysisResult,
  RoomMorphRequest,
  SamplesRequest,
  TimbreAnalysisResult,
  TimbreFrame,
} from './quick_analysis.js';
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
} from './quick_analysis.js';
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
  RenderOfflineRequest,
  TrackMonitorMode,
} from './realtime_engine.js';
export {
  ClipPageProvider,
  EXPECTED_ENGINE_ABI_VERSION,
  engineCapabilities,
  RealtimeEngine,
} from './realtime_engine.js';
export { scaleCorrectionSemitones, scalePitchClassEnabled, scaleQuantizeMidi } from './scale.js';
export type { ProgressCallback } from './sonare.js';
export { StreamAnalyzer, streamAnalyzerConfigDefaults } from './stream_analyzer.js';
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
} from './stream_types.js';
export type {
  MixerMeterSnapshot,
  MixerRealtimeBuffer,
  RealtimeVoiceChangerInterleavedBuffer,
  RealtimeVoiceChangerMonoBuffer,
  RealtimeVoiceChangerPlanarBuffer,
  StripMeteringOptions,
} from './streaming_mixing.js';
export {
  Mixer,
  RealtimeVoiceChanger,
  realtimeVoiceChangerPresetJson,
  realtimeVoiceChangerPresetNames,
  StreamingEqualizer,
  StreamingMasteringChain,
  StreamingRetune,
  validateRealtimeVoiceChangerPresetJson,
} from './streaming_mixing.js';
export type { TranscribeRequest } from './transcribe.js';
export { transcribe } from './transcribe.js';
export type { ValidateOptions } from './validation.js';
export {
  createVocalEditSession,
  restoreVocalEditSession,
  VocalEditDraft,
  VocalEditSession,
  VocalRenderJob,
  VocalRenderSnapshot,
  vocalEditApiVersion,
  vocalEditAvailable,
} from './vocal_edit.js';
export type {
  VocalEditWorker,
  VocalEditWorkerCallOptions,
  VocalEditWorkerClientOptions,
  VocalEditWorkerTransferOptions,
} from './vocal_edit_worker_client.js';
export {
  VocalEditWorkerClient,
  VocalEditWorkerSession,
  VocalEditWorkerStaleResultError,
  VocalEditWorkerTask,
} from './vocal_edit_worker_client.js';
export type * from './vocal_edit_worker_protocol.js';
export type {
  BindWebMidiOptions,
  WebMidiBinding,
  WebMidiCcBinding,
  WebMidiInputInfo,
} from './web_midi.js';
export { bindWebMidi, isWebMidiAvailable } from './web_midi.js';
export type {
  OfflineWorker,
  OfflineWorkerCallOptions,
  OfflineWorkerClientOptions,
  OfflineWorkerProgress,
} from './worker_client.js';
export { OfflineWorkerClient, OfflineWorkerTask } from './worker_client.js';

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
let firstInitOptions: object | undefined;

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
    assertSameInitOptions(firstInitOptions, options);
    return;
  }
  if (initPromise) {
    assertSameInitOptions(firstInitOptions, options);
    return initPromise;
  }
  firstInitOptions = options && { ...options };

  initPromise = (async () => {
    try {
      const createModule = options?.moduleFactory ?? (await import('./sonare.js')).default;
      const created = await createModule(options);
      assertAbiCompatible(created);
      module = created;
      setSonareModule(module);
    } catch (error) {
      initPromise = null;
      firstInitOptions = undefined;
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
    throw notInitializedError();
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
    throw notInitializedError();
  }
  return module.capabilities();
}

/** Return the initialized module's processors, parameters, and presets. */
export function capabilityCatalog(): CapabilityCatalog {
  if (!module) {
    throw notInitializedError();
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
    throw notInitializedError();
  }
  return module.abiVersion();
}

export function engineAbiVersion(): number {
  if (!module) {
    throw notInitializedError();
  }
  return module.engineAbiVersion();
}

export function voiceChangerAbiVersion(): number {
  if (!module) {
    throw notInitializedError();
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
    throw notInitializedError();
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
    throw notInitializedError();
  }
  return module.realtimeVoiceChangerPresetConfig(resolveVoicePresetOrdinal(preset));
}

// ============================================================================
// Re-exports
// ============================================================================

export { PitchClass as Pitch } from './public_types.js';

export type * from './public_types_vocal_project.js';
