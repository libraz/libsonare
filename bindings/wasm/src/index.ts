/**
 * sonare - Audio Analysis Library
 *
 * @example
 * ```ts
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

import { ErrorCode } from './errors.js';
import { codedError, withEntry } from './internal/entry.js';
import './lifetime.js';
import { initModule, loadedModule, notInitializedError } from './module_state.js';
import type { Capabilities, CapabilityCatalog, InitOptions, SystemInfo } from './public_types.js';

export { alignTakeToReference } from './align_take.js';
export type {
  BrowserAudioDecodeOptions,
  ChannelLayout,
  DecodedChannels,
  DownmixOptions,
} from './audio.js';
export { Audio, decodeChannels, decodeChannelsWithBrowserFallback, downmix } from './audio.js';
export type { ChordSymbolFields } from './chord_annotation.js';
export { chordSymbolFromAnalysis, keyModeFromAnalysis } from './chord_annotation.js';
export type {
  ClipPagePageRequest,
  ClipPageSampleRequest,
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
  FormantMode,
  GateOptions,
  MasteringAbMatchLoudnessRequest,
  MasteringAbMatchLoudnessStereoRequest,
  MasteringAmpPresetCatalogEntry,
  MasteringAnalyzePairRequest,
  MasteringAnalyzeStereoRequest,
  MasteringApplyPairProcessorRequest,
  MasteringApplyPairProcessorStereoRequest,
  MasteringApplyProcessorRequest,
  MasteringApplyProcessorStereoRequest,
  MasteringAssistantSettings,
  MasteringAssistantSuggestRequest,
  MasteringAssistantSuggestStereoRequest,
  MasteringAudioProfileRequest,
  MasteringAudioProfileStereoRequest,
  MasteringChannelPolicy,
  MasteringDynamicsCompressorRequest,
  MasteringDynamicsGateRequest,
  MasteringDynamicsTransientShaperRequest,
  MasteringInsertParamChoice,
  MasteringInsertParamInfo,
  MasteringInsertParamInfoRequest,
  MasteringInsertSlot,
  MasteringInsertTiming,
  MasteringInsertTimingRequest,
  MasteringNormalizeLoudnessRequest,
  MasteringProcessorCatalogEntry,
  MasteringProcessorCategory,
  MasteringRealtimeCost,
  MasteringRepairAnalysis,
  MasteringRepairAnalyzeRequest,
  MasteringRepairApplyRequest,
  MasteringRepairApplyResult,
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
  MasteringRepairStage,
  MasteringRepairStageName,
  MasteringRepairStageReports,
  MasteringRepairTrimSilenceRequest,
  MasteringRepairTrimSilenceStereoRequest,
  MasteringStreamingPreviewRequest,
  MasteringStreamingPreviewStereoRequest,
  MixStereoRequest,
  MixStereoStrip,
  RealtimeVoiceChangerRenderRequest,
  TransientShaperOptions,
  TrimSilenceMode,
  TrimSilenceOptions,
  VoiceChangeOptions,
  VoiceChangeRequest,
} from './effects_mastering.js';
export {
  assignNoteTargets,
  autoTune,
  decomposeNotePitch,
  extractNotes,
  extractPercussiveEvents,
  harmonic,
  hpss,
  masterAudio,
  masterAudioStereo,
  masteringAbMatchLoudness,
  masteringAbMatchLoudnessStereo,
  masteringAmpPresetCatalog,
  masteringAnalyzePair,
  masteringAnalyzeStereo,
  masteringApplyPairProcessor,
  masteringApplyPairProcessorStereo,
  masteringApplyProcessor,
  masteringApplyProcessorStereo,
  masteringAssistantSuggest,
  masteringAssistantSuggestStereo,
  masteringAudioProfile,
  masteringAudioProfileStereo,
  masteringChainConfigFromJson,
  masteringDynamicsCompressor,
  masteringDynamicsGate,
  masteringDynamicsTransientShaper,
  masteringInsertNames,
  masteringInsertParamInfo,
  masteringInsertParamNames,
  masteringInsertTiming,
  masteringNormalizeLoudness,
  masteringPairAnalysisNames,
  masteringPairProcessorNames,
  masteringPlatformNames,
  masteringPresetNames,
  masteringPresetParams,
  masteringProcessorCatalog,
  masteringProcessorNames,
  masteringRepairAnalyze,
  masteringRepairApply,
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
  masteringStreamingPreview,
  masteringStreamingPreviewStereo,
  mergeNotes,
  mixingScenePreset,
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
  pitchShift,
  realtimeVoiceChangerRender,
  renderNotes,
  renderPercussiveEvents,
  spectralEdit,
  splitNote,
  streamingLoudnessGain,
  streamingLoudnessGainStereo,
  timeStretch,
  voiceChange,
} from './effects_mastering.js';
export type {
  AssignNoteTargetsRequest,
  DecomposeNotePitchRequest,
  ExtractNotesRequest,
  MergeNotesRequest,
  NoteMoveRequest,
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
  AutoTuneRequest,
  PitchCorrectTimevaryingRequest,
  PitchCorrectToMidiRequest,
  PitchShiftRequest,
  TimeStretchRequest,
} from './effects_timepitch.js';
export type { ErrorCodeName, SonareErrorOptions } from './errors.js';
export { ErrorCode, isSonareError, SonareError } from './errors.js';
export type {
  AmplitudeToDbRequest,
  ChirpRequest,
  ClicksRequest,
  CyclicTempogramRequest,
  DbToAmplitudeRequest,
  DbToPowerRequest,
  DeemphasisRequest,
  FixFramesRequest,
  FixLengthRequest,
  FrameSignalRequest,
  OnsetBacktrackRequest,
  PadCenterRequest,
  PcenRequest,
  PeakPickRequest,
  PlpRequest,
  PowerToDbRequest,
  PreemphasisRequest,
  SilenceCommonReport,
  SplitSilenceCommonRequest,
  SplitSilenceCommonWithReportRequest,
  SplitSilenceCommonWithReportResult,
  SplitSilenceRequest,
  TempogramRequest,
  ToneRequest,
  TonnetzRequest,
  TrimSilenceRequest,
  VectorNormalizeRequest,
} from './feature_core.js';
export type {
  DecomposeRequest,
  DecomposeResult,
  DecomposeStemsLinkedRequest,
  DecomposeStemsLinkedResult,
  DecomposeStemsRequest,
  DecomposeStemsResult,
  NmfInit,
  NnFilterAggregate,
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
  Ebur128LoudnessRangeLuRequest,
  LufsMultichannelRequest,
  LufsSeriesMultichannelRequest,
} from './feature_loudness.js';
export type {
  AnalyzeMelodyRequest,
  AnalyzeSectionsRequest,
  CqtRequest,
  CqtToAudioRequest,
  DetectBoundariesRequest,
  FourierTempogramRequest,
  HybridCqtRequest,
  LufsRequest,
  MelodyOptions,
  MomentaryLufsRequest,
  NnlsChromaRequest,
  OnsetStrengthMultiRequest,
  OnsetStrengthRequest,
  PseudoCqtRequest,
  ShortTermLufsRequest,
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
  RmsEnergyRequest,
  SpectralBandwidthRequest,
  SpectralCentroidRequest,
  SpectralContrastRequest,
  SpectralFlatnessRequest,
  SpectralFluxRequest,
  SpectralRolloffRequest,
  ZeroCrossingRateRequest,
  ZeroCrossingsRequest,
} from './feature_spectral.js';
export type {
  BassChromaRequest,
  BiquadFilterRequest,
  BiquadFilterType,
  ChromaCensRequest,
  ChromaCqtRequest,
  ChromaRequest,
  FadeRequest,
  MelDeltaRequest,
  MelFilterbankRequest,
  MelFrequenciesRequest,
  MelSpectrogramRequest,
  MfccRequest,
  ReassignedSpectrogramRequest,
  StftRequest,
  TrimRequest,
  TrimResult,
} from './feature_spectrogram.js';
export {
  amplitudeToDb,
  analyzeMelody,
  analyzeSections,
  bassChroma,
  biquadFilter,
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
  deemphasis,
  detectBoundaries,
  ebur128LoudnessRangeLu,
  estimateTuning,
  fade,
  fixFrames,
  fixLength,
  fourierTempogram,
  frameSignal,
  framesToSamples,
  framesToTime,
  griffinLim,
  hybridCqt,
  hzToMel,
  hzToMidi,
  hzToNote,
  lufs,
  lufsMultichannel,
  lufsSeriesMultichannel,
  melDelta,
  melFilterbank,
  melFrequencies,
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
  onsetStrength,
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
  referenceHzToTuning,
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
  tempogram,
  tempogramRatio,
  timeToFrames,
  tone,
  tonnetz,
  trim,
  trimSilence,
  tuningToReferenceHz,
  vectorNormalize,
  vqt,
  vqtToAudio,
  zeroCrossingRate,
  zeroCrossings,
} from './features.js';
export type { SynthParamCondition, SynthParamInfo } from './instrument_types.js';
export type { BindMicrophoneInputOptions, MicrophoneInputBinding } from './live_audio.js';
export { bindMicrophoneInput } from './live_audio.js';
export type {
  MasterAudioRequest,
  MasterAudioStereoRequest,
  NormalizeMode,
  NormalizeRequest,
  NormalizeStereoRequest,
  NormalizeStereoResult,
  StreamingLoudnessGainRequest,
  StreamingLoudnessGainStereoRequest,
} from './mastering_chain.js';
export type {
  ClippingRegion,
  ClippingReport,
  DynamicRangeReport,
  MeteringCrestFactorDbRequest,
  MeteringCrestFactorDbStereoRequest,
  MeteringDcOffsetRequest,
  MeteringDetectClippingOptions,
  MeteringDetectClippingRequest,
  MeteringDynamicRangeOptions,
  MeteringDynamicRangeRequest,
  MeteringPeakDbRequest,
  MeteringPhaseScopeRequest,
  MeteringRmsDbRequest,
  MeteringSilenceRatioRequest,
  MeteringSpectrumFrameRequest,
  MeteringSpectrumRequest,
  MeteringStereoCorrelationRequest,
  MeteringStereoWidthRequest,
  MeteringTruePeakDbtpRequest,
  MeteringVectorscopeRequest,
  PhaseScopeReport,
  SpectrumOptions,
  SpectrumReport,
  VectorscopeReport,
  WaveformPeakPyramidRequest,
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
  meteringRmsDb,
  meteringSilenceRatio,
  meteringSpectrum,
  meteringSpectrumFrame,
  meteringStereoCorrelation,
  meteringStereoWidth,
  meteringTruePeakDbtp,
  meteringVectorscope,
  waveformPeakPyramid,
  waveformPeaks,
} from './metering.js';
export {
  gm2DrumName,
  gm2DrumSetName,
  gm2InstrumentName,
  gmDrumName,
  gmDrumNoteForName,
  gmFamilyFirstProgram,
  gmFamilyName,
  gmInstrumentName,
  gmProgramForName,
  midi2AssignableController,
  midi2AssignablePerNoteController,
  midi2Cc,
  midi2ChannelPressure,
  midi2ControlToMidi1,
  midi2ControlToMidi1Control14,
  midi2NoteOff,
  midi2NoteOn,
  midi2NoteOnVelocityToMidi1,
  midi2PerNoteManagement,
  midi2PerNotePitchBend,
  midi2PitchBend,
  midi2PitchBendToMidi1,
  midi2PolyPressure,
  midi2Program,
  midi2RegisteredController,
  midi2RegisteredPerNoteController,
  midi2RelativeAssignableController,
  midi2RelativeRegisteredController,
  midi2VelocityToMidi1,
  midiBankProgram,
  midiCc,
  midiCcIndexForName,
  midiCcLearn,
  midiCcName,
  midiCcToBreakpoint,
  midiChannelPressure,
  midiControl14ToMidi2,
  midiControlToMidi2,
  midiNoteOff,
  midiNoteOn,
  midiNoteOnVelocityToMidi2,
  midiParamToCc,
  midiPitchBend,
  midiPitchBendToMidi2,
  midiPolyPressure,
  midiProgram,
  midiRouteEvents,
  midiVelocityToMidi2,
  perNoteControllerName,
} from './midi_messages.js';
export type {
  FaderAutomationPoint,
  InsertAutomationPoint,
  MixBusRole,
  MixerAutomationPointBase,
  MixerConfig,
  MixerJsonOptions,
  MixerMeterOptions,
  MixerSend,
  PanAutomationPoint,
  PolarityInvert,
  SendAutomationPoint,
  StripRef,
  WidthAutomationPoint,
} from './mixer.js';
export { MeterScratchField } from './mixer.js';
export type { MixingAssistantSuggestRequest } from './mixing_assistant.js';
export {
  mixingAssistantSourceClassFromName,
  mixingAssistantSourceClassNames,
  mixingAssistantSuggest,
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
  importOpfsClip,
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
  MpeDimension,
  NoteTracking,
  PartRig,
  PartRigEntry,
  PartRigInsert,
  PartRigKey,
  PartRigMode,
  PartRigRequest,
  ProjectAnalyzeTempoRequest,
  ProjectAssistSidecar,
  ProjectAssistSidecarInput,
  ProjectAudioSourceMetadataInput,
  ProjectAutomationCurve,
  ProjectAutomationLaneDesc,
  ProjectAutomationPoint,
  ProjectAutomationTargetKind,
  ProjectAutoTempoOptions,
  ProjectAutoTempoRequest,
  ProjectBounceOptions,
  ProjectBounceResult,
  ProjectChordQuality,
  ProjectChordSymbol,
  ProjectClip,
  ProjectClipCompSegment,
  ProjectClipDesc,
  ProjectClipFadeInput,
  ProjectClipLoopInput,
  ProjectClipTake,
  ProjectClipTakesOptions,
  ProjectCompileOptions,
  ProjectCompileResult,
  ProjectDeserializeResult,
  ProjectDiagnosticCode,
  ProjectFadeCurve,
  ProjectInstrumentBinding,
  ProjectKeyMode,
  ProjectKeySegment,
  ProjectLoopMode,
  ProjectLoopRecordingCommon,
  ProjectLoopRecordingDesc,
  ProjectLoopRecordingResult,
  ProjectMarker,
  ProjectMarkerInput,
  ProjectMarkerKind,
  ProjectMidiCcBinding,
  ProjectMidiCcBindingKind,
  ProjectMidiClipResult,
  ProjectMidiEvent,
  ProjectMidiFxBakeRequest,
  ProjectMidiFxBakeResult,
  ProjectMidiFxPreviewRequest,
  ProjectMidiRouteConfig,
  ProjectMidiRouteResult,
  ProjectMoveClipOptions,
  ProjectNotePairValidation,
  ProjectOverlapPolicy,
  ProjectProgramOptions,
  ProjectSource,
  ProjectSourceAudioInput,
  ProjectSourceKind,
  ProjectStemLayout,
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
  ProjectWarpMode,
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
  SynthLfoShape,
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
  BUILTIN_SYNTH_WAVEFORMS,
  CONTROLLER_AXES,
  CONTROLLER_INPUTS,
  controllerProfileNames,
  MPE_DIMENSIONS,
  NOTE_TRACKINGS,
  PART_RIG_ALL_PARTS,
  PART_RIG_MODES,
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
  SYNTH_LFO_SHAPES,
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
export { synthEngineParamInfo, synthPatchParamInfo } from './project_synth.js';
export type {
  AcousticModeName,
  AcousticOptions,
  AcousticResult,
  AnalysisBpmCandidate,
  AnalysisChord,
  AnalysisDynamics,
  AnalysisMelody,
  AnalysisResult,
  AnalysisRhythm,
  AnalyzeBpmOptions,
  AnalyzeDynamicsOptions,
  AnalyzeRhythmOptions,
  AnalyzeSectionsOptions,
  AnalyzeTimbreOptions,
  AssignNoteTargetsResult,
  AutoGainOptions,
  AutomationCurve,
  AutoTuneKey,
  AutoTuneOptions,
  AutoTuneResult,
  Beat,
  BeatObservations,
  Boundary,
  BoundaryOptions,
  BoundaryResult,
  BpmCandidate,
  BpmCandidateRelation,
  Capabilities,
  CapabilityCatalog,
  CapabilityCatalogChain,
  CapabilityCatalogChainList,
  CapabilityCatalogChainStage,
  CapabilityCatalogPresets,
  CharacterType,
  Chord,
  ChordAnalysisResult,
  ChordChromaMethod,
  ChordDetectionOptions,
  ChordFunction,
  ChordFunctionsInput,
  ChordFunctionsKey,
  ChordQuality,
  ChromaResult,
  ClickDetection,
  ClipDetection,
  CqtResult,
  CrackleDetection,
  DeclickReport,
  DeclickResult,
  DeclipReport,
  DeclipResult,
  DecrackleReport,
  DecrackleResult,
  DehumReport,
  DehumResult,
  DenoiseReport,
  DenoiseResult,
  DereverbReport,
  DereverbResult,
  DynamicsResult,
  EqBandInput,
  EqBandPhase,
  EqBandType,
  EqCoeffMode,
  EqMatchOptions,
  EqSpectrumSnapshot,
  EqStereoPlacement,
  FunctionalChord,
  GenreHint,
  GoniometerPoint,
  GrooveType,
  HpssResult,
  HumDetection,
  InitOptions,
  Key,
  KeyCandidate,
  KeyDetection,
  KeyDetectionOptions,
  KeyMode,
  KeyProfile,
  LoudnessMatchResult,
  LoudnessMatchStereoResult,
  LufsResult,
  LufsSeriesResult,
  MasteringAssistantProfile,
  MasteringAssistantResult,
  MasteringAudioProfile,
  MasteringAudioProfileDefects,
  MasteringAudioProfileDynamics,
  MasteringAudioProfileLoudness,
  MasteringAudioProfileSpectral,
  MasteringChainConfig,
  MasteringChainConfigInput,
  MasteringChainResult,
  MasteringChainStereoResult,
  MasteringInsertName,
  MasteringInsertParamDependency,
  MasteringInsertParamRelation,
  MasteringInsertParamScale,
  MasteringInsertParamUnit,
  MasteringLoudnessSummary,
  MasteringNormalizeLoudnessOptions,
  MasteringNormalizeLoudnessResult,
  MasteringPairParams,
  MasteringPlatform,
  MasteringPreset,
  MasteringPresetCatalogEntry,
  MasteringProcessorParamMap,
  MasteringProcessorParams,
  MasteringRepairDeclickResult,
  MasteringRepairDeclipResult,
  MasteringRepairDecrackleResult,
  MasteringRepairDehumResult,
  MasteringRepairDenoiseClassicalResult,
  MasteringRepairDereverbClassicalResult,
  MasteringRepairTrimSilenceResult,
  MasteringReport,
  MasteringResult,
  MasteringStereoResult,
  MasteringStreamingPreviewPlatform,
  MasteringStreamingPreviewResult,
  MatchEqCurveResult,
  MatchEstimateReferenceDelaySamplesResult,
  MatchReferenceLoudnessResult,
  MatchTonalBalanceBand,
  MatchTonalBalanceResult,
  MaterialPresetName,
  Matrix2D,
  MelNorm,
  MelodyPoint,
  MelodyResult,
  MelSpectrogramResult,
  MeterEstimate,
  MeterTap,
  MfccResult,
  MidiFxConfig,
  MidiRelease,
  MixAnalysisBand,
  MixAssistantMixProfile,
  MixAssistantResult,
  MixAssistantTrack,
  MixAssistantTrackProfile,
  MixBandDominance,
  MixBandOccupancy,
  MixCrowdedBand,
  MixerProcessResult,
  MixMeterSnapshot,
  MixMonoRisk,
  MixResult,
  MixSceneBus,
  MixSceneBusInput,
  MixSceneConnection,
  MixSceneDocument,
  MixSceneDocumentInput,
  MixSceneInsert,
  MixScenePreset,
  MixSceneSend,
  MixSceneSendInput,
  MixSceneStrip,
  MixSceneStripInput,
  MixSceneVcaGroup,
  MixSceneVcaGroupInput,
  MixSourceClass,
  MixTrackAlignment,
  ModuleFactory,
  ModuleFactoryOptions,
  ModuleHandle,
  MultibandCompressorBand,
  NoiseDetection,
  NormType,
  NoteDivision,
  NoteEdit,
  NoteEditInput,
  NoteExtractorOptions,
  NoteMoveOptions,
  NoteObject,
  NoteObjectInput,
  NoteSegment,
  NoteSetEntry,
  NoteStretchOptions,
  NoteTarget,
  NoteTargetUnmatchedPolicy,
  PairAnalysis,
  PairAnalysisResultMap,
  PairProcessor,
  PanLaw,
  PanMode,
  PercussiveEvent,
  PercussiveEventEdit,
  PercussiveEventEditInput,
  PercussiveEventInput,
  PiptrackResult,
  PitchCorrectionMode,
  PitchCorrectOptions,
  PitchDecompositionResult,
  PitchResult,
  PitchTargetMode,
  PlaybackBassManagementConfig,
  PlaybackChannelRole,
  PlaybackDiagnostics,
  PlaybackHeadTrackingConfig,
  PlaybackInputConfig,
  PlaybackLoudnessConfig,
  PlaybackLoudnessMeterOptions,
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
  RealtimeVoiceChangerConfig,
  RealtimeVoiceChangerConfigInput,
  ReassignedSpectrogramResult,
  RenderPlaybackRequest,
  RenderPlaybackResult,
  ReverbDetection,
  ReverbVariation,
  RhythmResult,
  RirDiagnostic,
  RirResult,
  RoomEstimateOptions,
  RoomEstimateResult,
  RoomGeometry,
  RoomMorphOptions,
  RoomMorphResult,
  RoomMorphStereoResult,
  RoomPlacement,
  ScaleKind,
  Section,
  SectionAnalysis,
  SectionType,
  SendTiming,
  SidechainCheck,
  SidechainRefusal,
  SidechainSourceKind,
  SoloProcessor,
  SpectralEditMode,
  SpectralEditOptions,
  SpectralRegionOp,
  StageGainReduction,
  StereoAnalysis,
  StereoAnalysisResultMap,
  StereoMonoCompatBand,
  StereoMonoCompatCheckLogBandsResult,
  StereoMonoCompatCheckResult,
  StereoPairProcessor,
  StftResult,
  StreamingEqualizerConfig,
  StreamingLoudnessGainResult,
  StreamingMasteringChainConfig,
  StreamingPlatform,
  StreamingRetuneAppliedConfig,
  StreamingRetuneConfig,
  SurroundPan,
  SynthesizeRirRequest,
  SystemInfo,
  TempogramMode,
  TimbreFrame,
  TimbreResult,
  TimeSignature,
  TrimRange,
  TrimReport,
  TrimSilenceResult,
  UmpWords,
  VocoderCarrierSource,
  VocoderCarrierWave,
  VoicedFlags,
  VoiceEngine,
  VoicePresetId,
  WindowType,
} from './public_types.js';
export {
  CHORD_QUALITIES,
  MASTERING_INSERT_NAMES,
  MIX_SCENE_PRESETS,
  PitchClass,
  SECTION_TYPES,
  SOLO_PROCESSORS,
} from './public_types.js';
export type {
  Gm2DrumNameRequest,
  Gm2InstrumentNameRequest,
  Midi2AssignableControllerRequest,
  Midi2AssignablePerNoteControllerRequest,
  Midi2CcRequest,
  Midi2ChannelPressureRequest,
  Midi2NoteOffRequest,
  Midi2NoteOnRequest,
  Midi2PerNoteManagementRequest,
  Midi2PerNotePitchBendRequest,
  Midi2PitchBendRequest,
  Midi2PolyPressureRequest,
  Midi2ProgramRequest,
  Midi2RegisteredControllerRequest,
  Midi2RegisteredPerNoteControllerRequest,
  Midi2RelativeAssignableControllerRequest,
  Midi2RelativeRegisteredControllerRequest,
  MidiBank,
  MidiBankProgramRequest,
  MidiCcLearnRequest,
  MidiCcRequest,
  MidiCcToBreakpointRequest,
  MidiChannelPressureRequest,
  MidiNoteOffRequest,
  MidiNoteOnRequest,
  MidiParamToCcRequest,
  MidiPitchBendRequest,
  MidiPolyPressureRequest,
  MidiProgramRequest,
  MidiRouteEventsRequest,
} from './public_types_midi_messages.js';
export type * from './public_types_vocal_edit.js';
export type {
  AnalyzeBpmRequest,
  AnalyzeDynamicsRequest,
  AnalyzeImpulseResponseRequest,
  AnalyzeRequest,
  AnalyzeRhythmRequest,
  AnalyzeTimbreRequest,
  BpmAnalysisResult,
  ChordFunctionalAnalysisRequest,
  ChordFunctionsRequest,
  DetectAcousticRequest,
  DetectBeatsRequest,
  DetectBpmRequest,
  DetectChordsRequest,
  DetectDownbeatsRequest,
  DetectKeyCandidatesRequest,
  DetectKeyRequest,
  DetectOnsetsRequest,
  EstimateMeterRequest,
  EstimateRoomRequest,
  MusicAnalyzeOptions,
  OnsetDetectOptions,
  RoomGeometryEstimate,
  RoomGeometryFromEstimateRequest,
  RoomMorphRequest,
  RoomMorphStereoRequest,
  TempoRangeOptions,
} from './quick_analysis.js';
export {
  analyze,
  analyzeBpm,
  analyzeDynamics,
  analyzeImpulseResponse,
  analyzeRhythm,
  analyzeTimbre,
  chordFunctionalAnalysis,
  chordFunctions,
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
  roomGeometryFromEstimate,
  roomMorph,
  roomMorphStereo,
  synthesizeRir,
} from './quick_analysis.js';
export type {
  CaptureArmState,
  CaptureOptions,
  CapturePunch,
  ClipPageProviderOptions,
  EngineAutomationPoint,
  EngineBounceOptions,
  EngineBounceResult,
  EngineBus,
  EngineCaptureSource,
  EngineCaptureStatus,
  EngineClip,
  EngineDither,
  EngineFreezeOptions,
  EngineFreezeResult,
  EngineGraphConnection,
  EngineGraphMix,
  EngineGraphNode,
  EngineGraphNodeType,
  EngineGraphParameterBinding,
  EngineGraphSpec,
  EngineInputMonitor,
  EngineInputMonitorInput,
  EngineLoop,
  EngineMarker,
  EngineMarkerInput,
  EngineMeterTelemetry,
  EngineMetronomeConfig,
  EngineMidiClipSchedule,
  EngineMidiEvent,
  EngineParameterInfo,
  EngineProcessOptions,
  EngineProcessWithMonitorResult,
  EngineScopeTelemetry,
  EngineSetParameterOptions,
  EngineTelemetry,
  EngineTelemetryError,
  EngineTelemetryType,
  EngineTempoSegment,
  EngineTimeSignatureSegment,
  EngineTrackLane,
  EngineTrackMonitorMode,
  EngineTrackSend,
  EngineTransportState,
  ExternalMidiEvent,
  InsertBypassState,
  InsertParamValue,
  MarkerKind,
  RealtimeEngineConfig,
  RenderOfflineRequest,
  SidechainSource,
  TrackSoloMuteState,
} from './realtime_engine.js';
export {
  ClipPageProvider,
  MAX_TRACK_LANES,
  RealtimeEngine,
} from './realtime_engine.js';
export type { DereverbClassicalConfig } from './repair_dereverb.js';
export {
  scaleCorrectionSemitones,
  scaleMaskForMode,
  scalePitchClassEnabled,
  scaleQuantizeMidi,
} from './scale.js';
export type { ProgressCallback } from './sonare.js';
export { StreamAnalyzer } from './stream_analyzer.js';
export type {
  StreamBarChord,
  StreamChordChange,
  StreamConfig,
  StreamFrames,
  StreamFramesI16,
  StreamFramesU8,
  StreamPatternScore,
  StreamProgressiveEstimate,
  StreamQuantizeConfig,
  StreamStats,
} from './stream_types.js';
export type {
  MixerRealtimeBuffer,
  RealtimeVoiceChangerInterleavedBuffer,
  RealtimeVoiceChangerMonoBuffer,
  RealtimeVoiceChangerOptions,
  RealtimeVoiceChangerPlanarBuffer,
  RealtimeVoiceChangerValidatePresetJsonResult,
  StripMeteringOptions,
} from './streaming_mixing.js';
export {
  Mixer,
  RealtimeVoiceChanger,
  realtimeVoiceChangerPresetConfig,
  realtimeVoiceChangerPresetJson,
  realtimeVoiceChangerPresetNames,
  realtimeVoiceChangerValidatePresetJson,
  StreamingEqualizer,
  StreamingMasteringChain,
  StreamingRetune,
  voiceCharacterPresetId,
} from './streaming_mixing.js';
export type { EqPhaseMode, StereoBlock } from './streaming_processors.js';
export type { TranscribeRequest } from './transcribe.js';
export { transcribe } from './transcribe.js';
export type { OptionsOf } from './validation.js';
export {
  createVocalEditSession,
  restoreVocalEditSession,
  VocalEditDraft,
  VocalEditSession,
  VocalRenderJob,
  VocalRenderSnapshot,
  vocalEditAbiVersion,
} from './vocal_edit.js';
export type {
  VocalEditWorker,
  VocalEditWorkerCallOptions,
  VocalEditWorkerClientOptions,
  VocalEditWorkerCreated,
  VocalEditWorkerTransferOptions,
} from './vocal_edit_worker_client.js';
export {
  VocalEditWorkerClient,
  VocalEditWorkerDraft,
  VocalEditWorkerSession,
} from './vocal_edit_worker_client.js';
export type {
  BindWebMidiOptions,
  WebMidiAudioContext,
  WebMidiBinding,
  WebMidiEngine,
  WebMidiEngineInputOptions,
  WebMidiEventContextTimeOptions,
  WebMidiInputInfo,
  WebMidiMessage,
} from './web_midi.js';
export { bindWebMidi, isWebMidiAvailable, webMidiEventContextTime } from './web_midi.js';
export type {
  OfflineWorker,
  OfflineWorkerCallOptions,
  OfflineWorkerClientOptions,
} from './worker_client.js';

import { parseCatalogJson } from './internal/record_json.js';

export { OfflineWorkerClient } from './worker_client.js';

// ============================================================================
// Module State
// ============================================================================

// ============================================================================
// Initialization
// ============================================================================

/**
 * Initialize the WASM module.
 * Must be called before using any analysis functions.
 *
 * One module per realm, shared by every entry point of this build; the
 * AudioWorklet realm initialises its own. Options apply on the first call; a
 * later call passing different options is refused.
 *
 * @param options - Optional module configuration
 * @returns Promise that resolves when initialization is complete
 */
export async function init(options?: InitOptions): Promise<void> {
  return withEntry('init', () =>
    initModule('full', options, async () => {
      // A worklet realm has no fetch, so the bundled loader could only fail later and obscurely.
      if ('AudioWorkletGlobalScope' in globalThis) {
        throw codedError(
          ErrorCode.NotSupported,
          'the module cannot be loaded inside an AudioWorkletGlobalScope; call ' +
            'init({ moduleFactory, wasmBinary }) with the default export of ' +
            "'@libraz/libsonare/module' as moduleFactory, or, for a processor registered from " +
            "'@libraz/libsonare/worklet', set globalThis.SonareEmbindModuleFactory to that " +
            'export and pass wasmBinary in its processorOptions',
        );
      }
      return (await import('./sonare.js')).default;
    }),
  );
}

/**
 * Check if this realm's module is initialized, through any entry point of this
 * build.
 */
export function isInitialized(): boolean {
  return loadedModule('full') !== null;
}

/**
 * Get the library version.
 */
export function version(): string {
  return withEntry('version', () => {
    const module = loadedModule('full');
    if (!module) {
      throw notInitializedError();
    }
    return module.version();
  });
}

/**
 * Return the capabilities of the loaded WASM build. Feature presence is read
 * here (`capabilities().features.ffmpeg`, `capabilities().features.vocalEdit`,
 * ...); there is no separate probe per feature.
 *
 * @throws SonareError `InvalidState` before {@link init} has resolved.
 */
export function capabilities(): Capabilities {
  return withEntry('capabilities', () => {
    const module = loadedModule('full');
    if (!module) {
      throw notInitializedError();
    }
    return module.capabilities();
  });
}

/** Parses a native count: `undefined` stays absent. */
function optionalFact(value: unknown): number | undefined {
  return typeof value === 'number' ? value : undefined;
}

/**
 * Return the host's core counts, memory sizes and how the library parallelizes on it. A fact
 * the platform cannot report is absent from its object, never 0.
 *
 * @throws SonareError `InvalidState` before {@link init} has resolved.
 */
export function systemInfo(): SystemInfo {
  return withEntry('systemInfo', () => {
    const module = loadedModule('full');
    if (!module) {
      throw notInitializedError();
    }
    const raw = JSON.parse(module.systemInfo());
    const cpu: { logicalCores?: number; physicalCores?: number } = {};
    const memory: { totalGb?: number; availableGb?: number } = {};
    const logicalCores = optionalFact(raw.cpu?.logical_cores);
    const physicalCores = optionalFact(raw.cpu?.physical_cores);
    const totalGb = optionalFact(raw.memory?.total_gb);
    const availableGb = optionalFact(raw.memory?.available_gb);
    if (logicalCores !== undefined) {
      cpu.logicalCores = logicalCores;
    }
    if (physicalCores !== undefined) {
      cpu.physicalCores = physicalCores;
    }
    if (totalGb !== undefined) {
      memory.totalGb = totalGb;
    }
    if (availableGb !== undefined) {
      memory.availableGb = availableGb;
    }
    return {
      cpu,
      memory,
      parallel: {
        enabled: raw.parallel.enabled === true,
        workers: raw.parallel.workers,
        strategy: raw.parallel.strategy,
      },
    };
  });
}

/**
 * Return the initialized module's processors, parameters, and presets. The
 * processor and parameter entries are the same records
 * {@link masteringProcessorCatalog} and {@link masteringInsertParamInfo} return.
 *
 * @throws SonareError `InvalidState` before {@link init} has resolved.
 */
export function capabilityCatalog(): CapabilityCatalog {
  return withEntry('capabilityCatalog', () => {
    const module = loadedModule('full');
    if (!module) {
      throw notInitializedError();
    }
    return parseCatalogJson(module.capabilityCatalog()) as CapabilityCatalog;
  });
}

/**
 * Aggregate native ABI version: the per-subsystem ABI macros folded into one
 * 32-bit value. It bumps whenever any flat C POD layout changes, so callers can
 * detect an incompatible prebuilt binary. Matches the Node/Python `abiVersion()`.
 */
export function abiVersion(): number {
  return withEntry('abiVersion', () => {
    const module = loadedModule('full');
    if (!module) {
      throw notInitializedError();
    }
    return module.abiVersion();
  });
}

/** ABI version of the realtime engine command/telemetry layout in the loaded module. */
export function engineAbiVersion(): number {
  return withEntry('engineAbiVersion', () => {
    const module = loadedModule('full');
    if (!module) {
      throw notInitializedError();
    }
    return module.engineAbiVersion();
  });
}

/** ABI version of the realtime voice-changer config layout in the loaded module. */
export function voiceChangerAbiVersion(): number {
  return withEntry('voiceChangerAbiVersion', () => {
    const module = loadedModule('full');
    if (!module) {
      throw notInitializedError();
    }
    return module.voiceChangerAbiVersion();
  });
}

// ============================================================================
// Re-exports
// ============================================================================

export type * from './public_types_vocal_project.js';
