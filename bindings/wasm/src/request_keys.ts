/**
 * Declared keys of every request-object entry point, checked against the request
 * types at compile time. `requestObject` refuses any other key by name.
 */

import type { ClipPageStreamerOptions } from './clip_page_streamer.js';
import type {
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
import type {
  ExtractPercussiveEventsRequest,
  RenderPercussiveEventsRequest,
} from './effects_percussive.js';
import type { HarmonicRequest, HpssRequest, PercussiveRequest } from './effects_separation.js';
import type { SpectralEditRequest } from './effects_spectral.js';
import type {
  AutoTuneRequest,
  PitchCorrectTimevaryingRequest,
  PitchCorrectToMidiRequest,
  PitchShiftRequest,
  TimeStretchRequest,
} from './effects_timepitch.js';
import type { VoiceChangeRealtimeRequest, VoiceChangeRequest } from './effects_voice_change.js';
import type {
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
  SplitSilenceCommonRequest,
  SplitSilenceCommonWithReportRequest,
  SplitSilenceRequest,
  TempogramRequest,
  ToneRequest,
  TonnetzRequest,
  TrimSilenceRequest,
  VectorNormalizeRequest,
} from './feature_core.js';
import type {
  DecomposeRequest,
  DecomposeStemsLinkedRequest,
  DecomposeStemsRequest,
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
import type {
  GriffinLimRequest,
  MelToAudioRequest,
  MelToStftRequest,
  MfccToAudioRequest,
  MfccToMelRequest,
  PhaseVocoderRequest,
} from './feature_inverse.js';
import type {
  Ebur128LoudnessRangeLuRequest,
  LufsMultichannelRequest,
  LufsSeriesMultichannelRequest,
} from './feature_loudness.js';
import type {
  AnalyzeMelodyRequest,
  AnalyzeSectionsRequest,
  CqtRequest,
  CqtToAudioRequest,
  DetectBoundariesRequest,
  FourierTempogramRequest,
  HybridCqtRequest,
  LufsRequest,
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
import type {
  EstimateTuningRequest,
  NoteSegmentsRequest,
  PiptrackRequest,
  PitchPyinRequest,
  PitchTuningRequest,
  PitchYinRequest,
} from './feature_pitch.js';
import type { ResampleRequest } from './feature_resample.js';
import type {
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
import type {
  BassChromaRequest,
  ChromaCensRequest,
  ChromaCqtRequest,
  ChromaRequest,
  MelDeltaRequest,
  MelSpectrogramRequest,
  MfccRequest,
  ReassignedSpectrogramRequest,
  StftDbRequest,
  StftRequest,
  TrimRequest,
} from './feature_spectrogram.js';
import type { PartRigRequest } from './instrument_types.js';
import type {
  MasterAudioRequest,
  MasterAudioStereoRequest,
  NormalizeRequest,
  NormalizeStereoRequest,
  StreamingLoudnessGainRequest,
  StreamingLoudnessGainStereoRequest,
} from './mastering_chain.js';
import type {
  MasteringAbMatchLoudnessRequest,
  MasteringAbMatchLoudnessStereoRequest,
  MasteringAnalyzePairRequest,
  MasteringAnalyzeStereoRequest,
  MasteringApplyPairProcessorRequest,
  MasteringApplyPairProcessorStereoRequest,
  MasteringApplyProcessorRequest,
  MasteringApplyProcessorStereoRequest,
  MasteringAssistantSuggestRequest,
  MasteringAssistantSuggestStereoRequest,
  MasteringAudioProfileRequest,
  MasteringAudioProfileStereoRequest,
  MasteringInsertParamInfoRequest,
  MasteringInsertTimingRequest,
  MasteringNormalizeLoudnessRequest,
  MasteringStreamingPreviewRequest,
  MasteringStreamingPreviewStereoRequest,
} from './mastering_core.js';
import type {
  MasteringDynamicsCompressorRequest,
  MasteringDynamicsGateRequest,
  MasteringDynamicsTransientShaperRequest,
} from './mastering_dynamics.js';
import type {
  MeteringCrestFactorDbRequest,
  MeteringCrestFactorDbStereoRequest,
  MeteringDcOffsetRequest,
  MeteringDetectClippingRequest,
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
  WaveformPeakPyramidRequest,
  WaveformPeaksRequest,
} from './metering.js';
import type {
  FaderAutomationPoint,
  InsertAutomationPoint,
  MixerConfig,
  MixerJsonOptions,
  MixerMeterOptions,
  MixerSend,
  PanAutomationPoint,
  PolarityInvert,
  SendAutomationPoint,
  WidthAutomationPoint,
} from './mixer.js';
import type { MixingAssistantSuggestRequest } from './mixing_assistant.js';
import type { MixStereoRequest } from './mixing_oneshot.js';
import type { AnalyzePolyphonicRequest } from './polyphony.js';
import type {
  AlignTakeToReferenceRequest,
  ExternalSeparatedStemImportRequest,
  ProjectAnalyzeTempoRequest,
  ProjectAssistSidecarInput,
  ProjectAudioSourceMetadataInput,
  ProjectAutoTempoRequest,
  ProjectBounceOptions,
  ProjectClipDesc,
  ProjectClipFadeInput,
  ProjectClipLoopInput,
  ProjectClipTakesOptions,
  ProjectMarkerInput,
  ProjectMidiFxBakeRequest,
  ProjectMidiFxPreviewRequest,
  ProjectMoveClipOptions,
  ProjectProgramInput,
  ProjectSourceAudioInput,
  ProjectTranscribeRequest,
} from './project_types.js';
import type { RoomPlacement, SynthesizeRirRequest } from './public_types_acoustic.js';
import type {
  CaptureOptions,
  CapturePunch,
  ClipPageProviderOptions,
  EngineAutomationPoint,
  EngineLoop,
  EngineMetronomeConfig,
  RealtimeEngineConfig,
  RenderOfflineRequest,
  SidechainSource,
} from './public_types_engine.js';
import type {
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
import type { ChordFunctionsInput } from './public_types_music.js';
import type {
  PlaybackLoudnessMeterOptions,
  PlaybackRendererOptions,
  RenderPlaybackRequest,
} from './public_types_playback.js';
import type { AutoTuneKey } from './public_types_spectral.js';
import type {
  VocalAnalysisInput,
  VocalApplyRequest,
  VocalCreateRequest,
  VocalRenderRequest,
  VocalRestoreRequest,
  VocalSessionLimits,
} from './public_types_vocal_edit.js';
import type { ProjectVocalEditApplyRequest } from './public_types_vocal_project.js';
import type {
  SonarePlaybackNodeOptions,
  SonarePlaybackWorkletProcessorConstructorOptions,
  SonarePlaybackWorkletProcessorOptions,
  SonareRealtimeEngineNodeOptions,
  SonareRealtimeEngineWorkletProcessorConstructorOptions,
  SonareRealtimeEngineWorkletProcessorOptions,
  SonareRealtimeVoiceChangerWorkletProcessorOptions,
  SonareStreamAnalyzerNodeOptions,
  SonareStreamAnalyzerWorkletProcessorConstructorOptions,
  SonareStreamAnalyzerWorkletProcessorOptions,
  SonareWorkletProcessorConstructorOptions,
  SonareWorkletProcessorOptions,
} from './public_types_worklet.js';
import type {
  AnalyzeBpmRequest,
  AnalyzeDynamicsRequest,
  AnalyzeImpulseResponseRequest,
  AnalyzeRequest,
  AnalyzeRhythmRequest,
  AnalyzeTimbreRequest,
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
  RoomGeometryFromEstimateRequest,
  RoomMorphRequest,
  RoomMorphStereoRequest,
} from './quick_analysis.js';
import type {
  MasteringRepairDereverbClassicalLinkedRequest,
  MasteringRepairDereverbClassicalRequest,
  MasteringRepairDereverbClassicalStereoRequest,
  MasteringRepairDereverbConfigForRoomRequest,
  MasteringRepairDetectReverbRequest,
} from './repair_dereverb.js';
import type {
  MasteringRepairDeclickRequest,
  MasteringRepairDeclickStereoRequest,
  MasteringRepairDeclipRequest,
  MasteringRepairDeclipStereoRequest,
  MasteringRepairDecrackleRequest,
  MasteringRepairDecrackleStereoRequest,
  MasteringRepairDetectClicksRequest,
  MasteringRepairDetectClippingRequest,
  MasteringRepairDetectCrackleRequest,
} from './repair_impulsive.js';
import type {
  MasteringRepairDehumRequest,
  MasteringRepairDehumStereoRequest,
  MasteringRepairDenoiseClassicalLinkedRequest,
  MasteringRepairDenoiseClassicalRequest,
  MasteringRepairDenoiseClassicalStereoRequest,
  MasteringRepairDetectHumRequest,
  MasteringRepairDetectNoiseFloorRequest,
  MasteringRepairNoiseBandBinsRequest,
} from './repair_noise.js';
import type {
  MasteringRepairAnalyzeRequest,
  MasteringRepairApplyRequest,
} from './repair_session.js';
import type {
  MasteringRepairDetectTrimRangeRequest,
  MasteringRepairDetectTrimRangeStereoRequest,
  MasteringRepairTrimSilenceRequest,
  MasteringRepairTrimSilenceStereoRequest,
} from './repair_trim.js';
import type { TranscribeRequest } from './transcribe.js';
import type { SonareEngineRenderOfflineRequest } from './worklet/engine-export.js';

/** Declared keys of `T`, without the index-signature widening. */
type KnownKeys<T> = keyof {
  [K in keyof T as Record<never, never> extends Record<K, 1> ? never : K]: T[K];
} &
  string;

/**
 * Lists every key of `T`: an extra name or a missing one fails to compile, so
 * the runtime list cannot drift from the request type.
 */
const keysOf =
  <T>() =>
  <const K extends readonly KnownKeys<T>[]>(
    keys: K & ([Exclude<KnownKeys<T>, K[number]>] extends [never] ? unknown : never),
  ): readonly string[] =>
    keys;

/**
 * The keys one request-object entry point accepts. `nested` names an options
 * object forwarded to native code, whose own keys are refused too; `hints`
 * names an object whose keys the native layer already validates, used only to
 * point a misplaced key at its field.
 */
export interface RequestShape {
  readonly keys: readonly string[];
  readonly nested?: Readonly<Record<string, readonly string[]>>;
  readonly hints?: Readonly<Record<string, readonly string[]>>;
}

const AlignTakeToReferenceRequestKeys = keysOf<AlignTakeToReferenceRequest>()([
  'reference',
  'take',
  'sampleRate',
  'takeSampleRate',
  'hopLength',
  'binsPerOctave',
]);
const AmplitudeToDbRequestKeys = keysOf<AmplitudeToDbRequest>()(['values', 'ref', 'amin', 'topDb']);
const PowerToDbRequestKeys = keysOf<PowerToDbRequest>()(['values', 'ref', 'amin', 'topDb']);
const DbToPowerRequestKeys = keysOf<DbToPowerRequest>()(['values', 'ref']);
const DbToAmplitudeRequestKeys = keysOf<DbToAmplitudeRequest>()(['values', 'ref']);
const PreemphasisRequestKeys = keysOf<PreemphasisRequest>()([
  'samples',
  'sampleRate',
  'coef',
  'zi',
  'validate',
]);
const DeemphasisRequestKeys = keysOf<DeemphasisRequest>()([
  'samples',
  'sampleRate',
  'coef',
  'zi',
  'validate',
]);
const TrimSilenceRequestKeys = keysOf<TrimSilenceRequest>()([
  'samples',
  'sampleRate',
  'topDb',
  'frameLength',
  'hopLength',
  'validate',
]);
const SplitSilenceRequestKeys = keysOf<SplitSilenceRequest>()([
  'samples',
  'sampleRate',
  'topDb',
  'frameLength',
  'hopLength',
  'validate',
]);
const SplitSilenceCommonWithReportRequestKeys = keysOf<SplitSilenceCommonWithReportRequest>()([
  'signals',
  'topDb',
  'frameLength',
  'hopLength',
]);
const StftRequestKeys = keysOf<StftRequest>()([
  'samples',
  'sampleRate',
  'nFft',
  'hopLength',
  'validate',
]);
const StftDbRequestKeys = keysOf<StftDbRequest>()([
  'samples',
  'sampleRate',
  'nFft',
  'hopLength',
  'validate',
]);
const ChromaRequestKeys = keysOf<ChromaRequest>()([
  'samples',
  'sampleRate',
  'nFft',
  'hopLength',
  'validate',
]);
const ChromaCensRequestKeys = keysOf<ChromaCensRequest>()([
  'samples',
  'sampleRate',
  'hopLength',
  'nChroma',
  'binsPerOctave',
  'validate',
]);
const ChromaCqtRequestKeys = keysOf<ChromaCqtRequest>()([
  'samples',
  'sampleRate',
  'hopLength',
  'nChroma',
  'binsPerOctave',
  'validate',
]);
const BassChromaRequestKeys = keysOf<BassChromaRequest>()([
  'samples',
  'sampleRate',
  'hopLength',
  'nChroma',
  'validate',
]);
const SpectralCentroidRequestKeys = keysOf<SpectralCentroidRequest>()([
  'samples',
  'sampleRate',
  'nFft',
  'hopLength',
  'validate',
]);
const SpectralFlatnessRequestKeys = keysOf<SpectralFlatnessRequest>()([
  'samples',
  'sampleRate',
  'nFft',
  'hopLength',
  'validate',
]);
const RmsEnergyRequestKeys = keysOf<RmsEnergyRequest>()([
  'samples',
  'sampleRate',
  'frameLength',
  'hopLength',
  'validate',
]);
const DetectBpmRequestKeys = keysOf<DetectBpmRequest>()([
  'samples',
  'sampleRate',
  'bpmMin',
  'bpmMax',
  'validate',
]);
const DetectBeatsRequestKeys = keysOf<DetectBeatsRequest>()([
  'samples',
  'sampleRate',
  'bpmMin',
  'bpmMax',
  'validate',
]);
const DetectDownbeatsRequestKeys = keysOf<DetectDownbeatsRequest>()([
  'samples',
  'sampleRate',
  'validate',
]);
const MomentaryLufsRequestKeys = keysOf<MomentaryLufsRequest>()([
  'samples',
  'sampleRate',
  'validate',
]);
const ShortTermLufsRequestKeys = keysOf<ShortTermLufsRequest>()([
  'samples',
  'sampleRate',
  'validate',
]);
const OnsetStrengthRequestKeys = keysOf<OnsetStrengthRequest>()([
  'nFft',
  'hopLength',
  'nMels',
  'samples',
  'sampleRate',
  'validate',
]);
const PseudoCqtRequestKeys = keysOf<PseudoCqtRequest>()([
  'hopLength',
  'fminHz',
  'nBins',
  'binsPerOctave',
  'samples',
  'sampleRate',
  'validate',
]);
const HybridCqtRequestKeys = keysOf<HybridCqtRequest>()([
  'hopLength',
  'fminHz',
  'nBins',
  'binsPerOctave',
  'samples',
  'sampleRate',
  'validate',
]);
const AnalyzeRequestKeys = keysOf<AnalyzeRequest>()([
  'samples',
  'sampleRate',
  'validate',
  'nFft',
  'hopLength',
  'bpmMin',
  'bpmMax',
  'startBpm',
  'useTriadsOnly',
  'useHpss',
  'chromaHighPassHz',
  'useBassWeighted',
  'chromaHopMultiplier',
  'useChordHmm',
  'useChordKeyContext',
  'chordHmmBeamWidth',
  'detectChordInversions',
  'adaptiveTempo',
  'tempoUpdateIntervalBeats',
  'computeTempoCurve',
  'meterCandidateNumerators',
  'meterDenominator',
  'tuningCents',
  'onProgress',
  'cancel',
]);
const AnalyzeBpmRequestKeys = keysOf<AnalyzeBpmRequest>()([
  'bpmMin',
  'bpmMax',
  'startBpm',
  'nFft',
  'hopLength',
  'maxCandidates',
  'validate',
  'samples',
  'sampleRate',
]);
const AnalyzeDynamicsRequestKeys = keysOf<AnalyzeDynamicsRequest>()([
  'windowSec',
  'hopLength',
  'compressionThresholdDb',
  'validate',
  'samples',
  'sampleRate',
]);
const AnalyzeImpulseResponseRequestKeys = keysOf<AnalyzeImpulseResponseRequest>()([
  'nOctaveBands',
  'minDecayDb',
  'samples',
  'sampleRate',
  'validate',
]);
const AnalyzeMelodyRequestKeys = keysOf<AnalyzeMelodyRequest>()([
  'samples',
  'sampleRate',
  'frameLength',
  'hopLength',
  'threshold',
  'usePyin',
  'center',
  'validate',
  'f0MinHz',
  'f0MaxHz',
]);
const AnalyzePolyphonicRequestKeys = keysOf<AnalyzePolyphonicRequest>()([
  'samples',
  'sampleRate',
  'nFft',
  'hopLength',
  'winLength',
  'centRefHz',
  'centsPerBin',
  'centMaxHz',
  'tonality',
  'salienceHarmonics',
  'f0MinHz',
  'f0MaxHz',
  'salienceAlphaHz',
  'salienceBetaHz',
  'salienceInharmonicity',
  'maxPolyphony',
  'minFramePeakRatio',
  'minSeparationCents',
  'subtractionFactor',
  'maxJumpCents',
  'minRidgePeakRatio',
  'minRidgeDurationMs',
  'reattackRatio',
  'maskHarmonics',
  'claimLobes',
  'inharmonicity',
  'estimateInharmonicity',
  'inharmonicityMinPartials',
  'inharmonicityMaxResidualBins',
  'inharmonicityMaxStretch',
  'windowFrames',
  'minPartialSeparation',
  'maxFitResidual',
  'maxWeightModulus',
  'maxRefineHz',
  'f0ToleranceCents',
  'segmentationThresholdCents',
  'minNoteMs',
  'referenceHz',
  'validate',
]);
const AnalyzeRhythmRequestKeys = keysOf<AnalyzeRhythmRequest>()([
  'bpmMin',
  'bpmMax',
  'startBpm',
  'nFft',
  'hopLength',
  'validate',
  'samples',
  'sampleRate',
]);
const AnalyzeSectionsRequestKeys = keysOf<AnalyzeSectionsRequest>()([
  'samples',
  'sampleRate',
  'nFft',
  'hopLength',
  'minSectionSec',
  'threshold',
  'validate',
]);
const AnalyzeTimbreRequestKeys = keysOf<AnalyzeTimbreRequest>()([
  'nFft',
  'hopLength',
  'nMels',
  'nMfcc',
  'windowSec',
  'validate',
  'samples',
  'sampleRate',
]);
const AssignNoteTargetsRequestKeys = keysOf<AssignNoteTargetsRequest>()([
  'notes',
  'sampleRate',
  'targets',
  'unmatchedPolicy',
  'minOverlapRatio',
  'maxCorrectionSemitones',
]);
const AutoTuneRequestKeys = keysOf<AutoTuneRequest>()([
  'samples',
  'sampleRate',
  'key',
  'strength',
  'retuneSpeedMs',
  'vibratoThresholdCents',
  'maxCorrectionSemitones',
  'scaleReferenceMidi',
  'validate',
]);
const AutoTuneKeyKeys = keysOf<AutoTuneKey>()(['root', 'mode']);
const ChirpRequestKeys = keysOf<ChirpRequest>()([
  'fminHz',
  'fmaxHz',
  'sampleRate',
  'durationSec',
  'linear',
]);
const ChordFunctionalAnalysisRequestKeys = keysOf<ChordFunctionalAnalysisRequest>()([
  'keyRoot',
  'keyMode',
  'minDurationSec',
  'smoothingWindowSec',
  'threshold',
  'useTriadsOnly',
  'nFft',
  'hopLength',
  'useBeatSync',
  'useHmm',
  'hmmBeamWidth',
  'useKeyContext',
  'detectInversions',
  'chromaMethod',
  'tuningCents',
  'validate',
  'samples',
  'sampleRate',
]);
const ChordFunctionsRequestKeys = keysOf<ChordFunctionsRequest<ChordFunctionsInput>>()([
  'chords',
  'key',
]);
const ClicksRequestKeys = keysOf<ClicksRequest>()([
  'times',
  'sampleRate',
  'length',
  'frequencyHz',
  'clickDurationSec',
]);
const CqtRequestKeys = keysOf<CqtRequest>()([
  'samples',
  'sampleRate',
  'hopLength',
  'fminHz',
  'nBins',
  'binsPerOctave',
  'validate',
]);
const CqtToAudioRequestKeys = keysOf<CqtToAudioRequest>()([
  'magnitude',
  'sampleRate',
  'hopLength',
  'fminHz',
  'binsPerOctave',
  'nIter',
  'validate',
]);
const VocalCreateRequestKeys = keysOf<VocalCreateRequest>()([
  'samples',
  'sampleRate',
  'outputLengthSamples',
  'edgeFadeMs',
  'vibratoCutoffHz',
  'segmentationThresholdCents',
  'minNoteMs',
  'frameLengthSamples',
  'hopLengthSamples',
  'fminHz',
  'fmaxHz',
  'yinThreshold',
  'voicedThreshold',
  'centered',
  'referenceHz',
  'limits',
  'analysis',
  'validate',
]);
const VocalSessionLimitsKeys = keysOf<VocalSessionLimits>()([
  'maxHistoryBytes',
  'maxCacheBytes',
  'maxUndoDepth',
  'maxRenderJobs',
]);
const VocalAnalysisInputKeys = keysOf<VocalAnalysisInput>()([
  'frameOriginSample',
  'samplesPerFrame',
  'frameLengthSamples',
  'f0Hz',
  'voiced',
  'algorithmId',
  'algorithmVersion',
  'fminHz',
  'fmaxHz',
  'yinThreshold',
  'voicedThreshold',
  'centered',
  'segmentationThresholdCents',
  'minNoteMs',
  'referenceHz',
]);
const CyclicTempogramRequestKeys = keysOf<CyclicTempogramRequest>()([
  'onsetEnvelope',
  'sampleRate',
  'hopLength',
  'winLength',
  'bpmMin',
  'nBins',
]);
const DecomposeRequestKeys = keysOf<DecomposeRequest>()([
  's',
  'nComponents',
  'nIter',
  'beta',
  'init',
]);
const DecomposeNotePitchRequestKeys = keysOf<DecomposeNotePitchRequest>()([
  'f0Hz',
  'voiced',
  'frameRate',
  'medianHz',
  'vibratoCutoffHz',
]);
const DecomposeStemsRequestKeys = keysOf<DecomposeStemsRequest>()([
  'samples',
  'sampleRate',
  'nComponents',
  'nFft',
  'hopLength',
  'nIter',
  'beta',
  'init',
  'maskPower',
  'validate',
]);
const DecomposeStemsLinkedRequestKeys = keysOf<DecomposeStemsLinkedRequest>()([
  'channels',
  'sampleRate',
  'nComponents',
  'nFft',
  'hopLength',
  'nIter',
  'beta',
  'init',
  'maskPower',
  'validate',
]);
const DetectAcousticRequestKeys = keysOf<DetectAcousticRequest>()([
  'nOctaveBands',
  'nThirdOctaveSubbands',
  'minDecayDb',
  'noiseFloorMarginDb',
  'validate',
  'samples',
  'sampleRate',
]);
const DetectBoundariesRequestKeys = keysOf<DetectBoundariesRequest>()([
  'samples',
  'sampleRate',
  'nFft',
  'hopLength',
  'kernelSize',
  'threshold',
  'absoluteThreshold',
  'nMfcc',
  'nChroma',
  'peakDistanceSec',
  'referenceWindowSec',
  'useMfcc',
  'useChroma',
  'validate',
]);
const DetectChordsRequestKeys = keysOf<DetectChordsRequest>()([
  'minDurationSec',
  'smoothingWindowSec',
  'threshold',
  'useTriadsOnly',
  'nFft',
  'hopLength',
  'useBeatSync',
  'useHmm',
  'hmmBeamWidth',
  'useKeyContext',
  'keyRoot',
  'keyMode',
  'detectInversions',
  'chromaMethod',
  'tuningCents',
  'validate',
  'samples',
  'sampleRate',
]);
const DetectKeyRequestKeys = keysOf<DetectKeyRequest>()([
  'tuningCents',
  'nFft',
  'hopLength',
  'useHpss',
  'loudnessWeighted',
  'modes',
  'profile',
  'genreHint',
  'validate',
  'samples',
  'sampleRate',
  'chromaHighPassHz',
]);
const DetectKeyCandidatesRequestKeys = keysOf<DetectKeyCandidatesRequest>()([
  'tuningCents',
  'nFft',
  'hopLength',
  'useHpss',
  'loudnessWeighted',
  'modes',
  'profile',
  'genreHint',
  'validate',
  'samples',
  'sampleRate',
  'chromaHighPassHz',
]);
const DetectOnsetsRequestKeys = keysOf<DetectOnsetsRequest>()([
  'samples',
  'sampleRate',
  'validate',
  'nFft',
  'hopLength',
  'threshold',
  'preMax',
  'postMax',
  'preAvg',
  'postAvg',
  'delta',
  'wait',
  'backtrack',
  'backtrackRange',
]);
const Ebur128LoudnessRangeLuRequestKeys = keysOf<Ebur128LoudnessRangeLuRequest>()([
  'samples',
  'sampleRate',
  'validate',
]);
const EstimateMeterRequestKeys = keysOf<EstimateMeterRequest>()([
  'beats',
  'candidateNumerators',
  'denominator',
  'downbeatWeight',
  'measureWeight',
  'subdivisionWeight',
  'compoundSubdivisionThreshold',
]);
const EstimateRoomRequestKeys = keysOf<EstimateRoomRequest>()([
  'aspectHintLw',
  'aspectHintLh',
  'referenceAbsorption',
  'preferEyring',
  'nOctaveBands',
  'mode',
  'minDecayDb',
  'noiseFloorMarginDb',
  'samples',
  'sampleRate',
  'validate',
]);
const EstimateTuningRequestKeys = keysOf<EstimateTuningRequest>()([
  'resolution',
  'binsPerOctave',
  'samples',
  'sampleRate',
  'nFft',
  'hopLength',
  'validate',
]);
const ExtractNotesRequestKeys = keysOf<ExtractNotesRequest>()([
  'samples',
  'sampleRate',
  'f0Hz',
  'frameRate',
  'voiced',
  'voicedProb',
  'segmentationThresholdCents',
  'minNoteMs',
  'referenceHz',
  'voicedThreshold',
  'validate',
]);
const ExtractPercussiveEventsRequestKeys = keysOf<ExtractPercussiveEventsRequest>()([
  'samples',
  'sampleRate',
  'onsetWait',
  'onsetDelta',
  'maxEventMs',
  'minPercussiveRatio',
  'nFft',
  'hopLength',
  'hpssKernelHarmonic',
  'hpssKernelPercussive',
  'validate',
]);
const FixFramesRequestKeys = keysOf<FixFramesRequest>()(['frames', 'xMin', 'xMax', 'pad']);
const FixLengthRequestKeys = keysOf<FixLengthRequest>()(['values', 'targetSize', 'padValue']);
const FourierTempogramRequestKeys = keysOf<FourierTempogramRequest>()([
  'onsetEnvelope',
  'sampleRate',
  'hopLength',
  'winLength',
  'center',
  'validate',
]);
const FrameSignalRequestKeys = keysOf<FrameSignalRequest>()([
  'samples',
  'frameLength',
  'hopLength',
  'sampleRate',
  'validate',
]);
const GriffinLimRequestKeys = keysOf<GriffinLimRequest>()([
  'magnitude',
  'sampleRate',
  'nFft',
  'hopLength',
  'nIter',
  'momentum',
  'validate',
]);
const HarmonicRequestKeys = keysOf<HarmonicRequest>()(['samples', 'sampleRate', 'validate']);
const HpssRequestKeys = keysOf<HpssRequest>()([
  'samples',
  'sampleRate',
  'kernelHarmonic',
  'kernelPercussive',
  'nFft',
  'hopLength',
  'hardMask',
  'validate',
]);
const HpssWithResidualRequestKeys = keysOf<HpssWithResidualRequest>()([
  'samples',
  'sampleRate',
  'kernelHarmonic',
  'kernelPercussive',
  'nFft',
  'hopLength',
  'hardMask',
  'validate',
]);
const LufsRequestKeys = keysOf<LufsRequest>()(['samples', 'sampleRate', 'validate']);
const LufsMultichannelRequestKeys = keysOf<LufsMultichannelRequest>()([
  'channels',
  'sampleRate',
  'validate',
]);
const LufsSeriesMultichannelRequestKeys = keysOf<LufsSeriesMultichannelRequest>()([
  'channels',
  'sampleRate',
  'validate',
]);
const MasterAudioRequestKeys = keysOf<MasterAudioRequest>()([
  'samples',
  'sampleRate',
  'preset',
  'overrides',
  'onProgress',
  'cancel',
  'validate',
]);
const MasterAudioStereoRequestKeys = keysOf<MasterAudioStereoRequest>()([
  'left',
  'right',
  'sampleRate',
  'preset',
  'overrides',
  'onProgress',
  'cancel',
  'validate',
]);
const MasteringNormalizeLoudnessRequestKeys = keysOf<MasteringNormalizeLoudnessRequest>()([
  'samples',
  'sampleRate',
  'targetLufs',
  'ceilingDbtp',
  'truePeakOversample',
  'releaseMs',
  'applyGainAtInputRate',
  'validate',
]);
const MasteringAbMatchLoudnessRequestKeys = keysOf<MasteringAbMatchLoudnessRequest>()([
  'source',
  'reference',
  'sampleRate',
  'referenceSampleRate',
  'validate',
]);
const MasteringAbMatchLoudnessStereoRequestKeys = keysOf<MasteringAbMatchLoudnessStereoRequest>()([
  'sourceLeft',
  'sourceRight',
  'referenceLeft',
  'referenceRight',
  'sampleRate',
  'referenceSampleRate',
  'validate',
]);
const MasteringAssistantSuggestRequestKeys = keysOf<MasteringAssistantSuggestRequest>()([
  'samples',
  'sampleRate',
  'targetLufs',
  'ceilingDbtp',
  'enableRepair',
  'preferStreamingSafe',
  'speechMonoAmount',
  'targetPlatform',
  'preset',
  'validate',
]);
const MasteringAssistantSuggestStereoRequestKeys = keysOf<MasteringAssistantSuggestStereoRequest>()(
  [
    'left',
    'right',
    'sampleRate',
    'targetLufs',
    'ceilingDbtp',
    'enableRepair',
    'preferStreamingSafe',
    'speechMonoAmount',
    'targetPlatform',
    'preset',
    'validate',
  ],
);
const MasteringAudioProfileRequestKeys = keysOf<MasteringAudioProfileRequest>()([
  'samples',
  'sampleRate',
  'nFft',
  'hopLength',
  'truePeakOversample',
  'detectDefects',
  'validate',
]);
const MasteringAudioProfileStereoRequestKeys = keysOf<MasteringAudioProfileStereoRequest>()([
  'left',
  'right',
  'sampleRate',
  'nFft',
  'hopLength',
  'truePeakOversample',
  'detectDefects',
  'validate',
]);
const MasteringDynamicsCompressorRequestKeys = keysOf<MasteringDynamicsCompressorRequest>()([
  'samples',
  'sampleRate',
  'thresholdDb',
  'ratio',
  'attackMs',
  'releaseMs',
  'kneeDb',
  'makeupGainDb',
  'autoMakeup',
  'detector',
  'sidechainHighPassHz',
  'pdrTimeMs',
  'pdrReleaseScale',
  'validate',
]);
const MasteringDynamicsGateRequestKeys = keysOf<MasteringDynamicsGateRequest>()([
  'samples',
  'sampleRate',
  'thresholdDb',
  'attackMs',
  'releaseMs',
  'rangeDb',
  'holdMs',
  'closeThresholdDb',
  'keyHighPassHz',
  'validate',
]);
const MasteringDynamicsTransientShaperRequestKeys =
  keysOf<MasteringDynamicsTransientShaperRequest>()([
    'samples',
    'sampleRate',
    'attackGainDb',
    'sustainGainDb',
    'fastAttackMs',
    'fastReleaseMs',
    'slowAttackMs',
    'slowReleaseMs',
    'sensitivity',
    'maxGainDb',
    'gainSmoothingMs',
    'lookaheadMs',
    'validate',
  ]);
const MasteringAnalyzePairRequestKeys = keysOf<MasteringAnalyzePairRequest>()([
  'analysis',
  'source',
  'reference',
  'sampleRate',
  'referenceSampleRate',
  'params',
  'validate',
]);
const MasteringApplyPairProcessorRequestKeys = keysOf<MasteringApplyPairProcessorRequest>()([
  'processor',
  'source',
  'reference',
  'sampleRate',
  'referenceSampleRate',
  'params',
  'validate',
]);
const MasteringApplyPairProcessorStereoRequestKeys =
  keysOf<MasteringApplyPairProcessorStereoRequest>()([
    'processor',
    'sourceLeft',
    'sourceRight',
    'referenceLeft',
    'referenceRight',
    'sampleRate',
    'referenceSampleRate',
    'params',
    'validate',
  ]);
const MasteringApplyProcessorRequestKeys = keysOf<MasteringApplyProcessorRequest>()([
  'processor',
  'samples',
  'sampleRate',
  'params',
  'validate',
]);
const MasteringApplyProcessorStereoRequestKeys = keysOf<MasteringApplyProcessorStereoRequest>()([
  'processor',
  'left',
  'right',
  'sampleRate',
  'params',
  'validate',
]);
const MasteringRepairAnalyzeRequestKeys = keysOf<MasteringRepairAnalyzeRequest>()([
  'channels',
  'sampleRate',
  'preferStreamingSafe',
  'validate',
]);
const MasteringRepairApplyRequestKeys = keysOf<MasteringRepairApplyRequest>()([
  'channels',
  'sampleRate',
  'stages',
  'onProgress',
  'cancel',
  'validate',
]);
const MasteringRepairDeclickRequestKeys = keysOf<MasteringRepairDeclickRequest>()([
  'samples',
  'sampleRate',
  'threshold',
  'neighborRatio',
  'maxClickSamples',
  'lpcOrder',
  'residualRatio',
  'validate',
]);
const MasteringRepairDeclickStereoRequestKeys = keysOf<MasteringRepairDeclickStereoRequest>()([
  'left',
  'right',
  'sampleRate',
  'threshold',
  'neighborRatio',
  'maxClickSamples',
  'lpcOrder',
  'residualRatio',
  'validate',
]);
const MasteringRepairDeclipRequestKeys = keysOf<MasteringRepairDeclipRequest>()([
  'samples',
  'sampleRate',
  'clipThresholdDb',
  'lpcOrder',
  'iterations',
  'lpcBlend',
  'validate',
]);
const MasteringRepairDeclipStereoRequestKeys = keysOf<MasteringRepairDeclipStereoRequest>()([
  'left',
  'right',
  'sampleRate',
  'clipThresholdDb',
  'lpcOrder',
  'iterations',
  'lpcBlend',
  'validate',
]);
const MasteringRepairDecrackleRequestKeys = keysOf<MasteringRepairDecrackleRequest>()([
  'samples',
  'sampleRate',
  'threshold',
  'mode',
  'levels',
  'validate',
]);
const MasteringRepairDecrackleStereoRequestKeys = keysOf<MasteringRepairDecrackleStereoRequest>()([
  'left',
  'right',
  'sampleRate',
  'threshold',
  'mode',
  'levels',
  'validate',
]);
const MasteringRepairDehumRequestKeys = keysOf<MasteringRepairDehumRequest>()([
  'samples',
  'sampleRate',
  'fundamentalHz',
  'harmonics',
  'q',
  'adaptive',
  'searchRangeHz',
  'adaptation',
  'frameSize',
  'pllBandwidth',
  'mode',
  'validate',
]);
const MasteringRepairDehumStereoRequestKeys = keysOf<MasteringRepairDehumStereoRequest>()([
  'left',
  'right',
  'sampleRate',
  'fundamentalHz',
  'harmonics',
  'q',
  'adaptive',
  'searchRangeHz',
  'adaptation',
  'frameSize',
  'pllBandwidth',
  'mode',
  'validate',
]);
const MasteringRepairDenoiseClassicalRequestKeys = keysOf<MasteringRepairDenoiseClassicalRequest>()(
  [
    'samples',
    'sampleRate',
    'mode',
    'noiseEstimator',
    'nFft',
    'hopLength',
    'ddAlpha',
    'reductionDb',
    'overSubtraction',
    'spectralFloor',
    'noiseEstimationQuantile',
    'speechPresenceGain',
    'gainSmoothing',
    'validate',
  ],
);
const MasteringRepairDenoiseClassicalLinkedRequestKeys =
  keysOf<MasteringRepairDenoiseClassicalLinkedRequest>()([
    'channels',
    'sampleRate',
    'mode',
    'noiseEstimator',
    'nFft',
    'hopLength',
    'ddAlpha',
    'reductionDb',
    'overSubtraction',
    'spectralFloor',
    'noiseEstimationQuantile',
    'speechPresenceGain',
    'gainSmoothing',
    'validate',
  ]);
const MasteringRepairDenoiseClassicalStereoRequestKeys =
  keysOf<MasteringRepairDenoiseClassicalStereoRequest>()([
    'left',
    'right',
    'sampleRate',
    'mode',
    'noiseEstimator',
    'nFft',
    'hopLength',
    'ddAlpha',
    'reductionDb',
    'overSubtraction',
    'spectralFloor',
    'noiseEstimationQuantile',
    'speechPresenceGain',
    'gainSmoothing',
    'validate',
  ]);
const MasteringRepairDereverbClassicalRequestKeys =
  keysOf<MasteringRepairDereverbClassicalRequest>()([
    'samples',
    'sampleRate',
    'threshold',
    'attenuation',
    'nFft',
    'hopLength',
    't60Sec',
    'lateDelayMs',
    'overSubtraction',
    'spectralFloor',
    'wpeEnabled',
    'wpeIterations',
    'wpeTaps',
    'wpeStrength',
    'validate',
  ]);
const MasteringRepairDereverbClassicalLinkedRequestKeys =
  keysOf<MasteringRepairDereverbClassicalLinkedRequest>()([
    'channels',
    'sampleRate',
    'threshold',
    'attenuation',
    'nFft',
    'hopLength',
    't60Sec',
    'lateDelayMs',
    'overSubtraction',
    'spectralFloor',
    'wpeEnabled',
    'wpeIterations',
    'wpeTaps',
    'wpeStrength',
    'validate',
  ]);
const MasteringRepairDereverbClassicalStereoRequestKeys =
  keysOf<MasteringRepairDereverbClassicalStereoRequest>()([
    'left',
    'right',
    'sampleRate',
    'threshold',
    'attenuation',
    'nFft',
    'hopLength',
    't60Sec',
    'lateDelayMs',
    'overSubtraction',
    'spectralFloor',
    'wpeEnabled',
    'wpeIterations',
    'wpeTaps',
    'wpeStrength',
    'validate',
  ]);
const MasteringRepairDetectClicksRequestKeys = keysOf<MasteringRepairDetectClicksRequest>()([
  'samples',
  'sampleRate',
  'threshold',
  'neighborRatio',
  'maxClickSamples',
  'lpcOrder',
  'residualRatio',
  'validate',
]);
const MasteringRepairDetectClippingRequestKeys = keysOf<MasteringRepairDetectClippingRequest>()([
  'samples',
  'sampleRate',
  'clipThresholdDb',
  'lpcOrder',
  'iterations',
  'lpcBlend',
  'validate',
]);
const MasteringRepairDetectCrackleRequestKeys = keysOf<MasteringRepairDetectCrackleRequest>()([
  'samples',
  'sampleRate',
  'threshold',
  'mode',
  'levels',
  'validate',
]);
const MasteringRepairDetectHumRequestKeys = keysOf<MasteringRepairDetectHumRequest>()([
  'samples',
  'sampleRate',
  'fundamentalHz',
  'harmonics',
  'q',
  'adaptive',
  'searchRangeHz',
  'adaptation',
  'frameSize',
  'pllBandwidth',
  'mode',
  'validate',
]);
const MasteringRepairDetectNoiseFloorRequestKeys = keysOf<MasteringRepairDetectNoiseFloorRequest>()(
  [
    'samples',
    'sampleRate',
    'mode',
    'noiseEstimator',
    'nFft',
    'hopLength',
    'ddAlpha',
    'reductionDb',
    'overSubtraction',
    'spectralFloor',
    'noiseEstimationQuantile',
    'speechPresenceGain',
    'gainSmoothing',
    'validate',
  ],
);
const MasteringRepairDetectReverbRequestKeys = keysOf<MasteringRepairDetectReverbRequest>()([
  'samples',
  'sampleRate',
  'threshold',
  'attenuation',
  'nFft',
  'hopLength',
  't60Sec',
  'lateDelayMs',
  'overSubtraction',
  'spectralFloor',
  'wpeEnabled',
  'wpeIterations',
  'wpeTaps',
  'wpeStrength',
  'validate',
]);
const MasteringRepairDetectTrimRangeRequestKeys = keysOf<MasteringRepairDetectTrimRangeRequest>()([
  'samples',
  'sampleRate',
  'thresholdDb',
  'paddingSamples',
  'paddingSec',
  'mode',
  'gateLufs',
  'windowMs',
  'validate',
]);
const MasteringRepairDetectTrimRangeStereoRequestKeys =
  keysOf<MasteringRepairDetectTrimRangeStereoRequest>()([
    'left',
    'right',
    'sampleRate',
    'thresholdDb',
    'paddingSamples',
    'paddingSec',
    'mode',
    'gateLufs',
    'windowMs',
    'validate',
  ]);
const MasteringRepairTrimSilenceRequestKeys = keysOf<MasteringRepairTrimSilenceRequest>()([
  'samples',
  'sampleRate',
  'thresholdDb',
  'paddingSamples',
  'paddingSec',
  'mode',
  'gateLufs',
  'windowMs',
  'validate',
]);
const MasteringRepairTrimSilenceStereoRequestKeys =
  keysOf<MasteringRepairTrimSilenceStereoRequest>()([
    'left',
    'right',
    'sampleRate',
    'thresholdDb',
    'paddingSamples',
    'paddingSec',
    'mode',
    'gateLufs',
    'windowMs',
    'validate',
  ]);
const MasteringAnalyzeStereoRequestKeys = keysOf<MasteringAnalyzeStereoRequest>()([
  'analysis',
  'left',
  'right',
  'sampleRate',
  'params',
  'validate',
]);
const MasteringInsertParamInfoRequestKeys = keysOf<MasteringInsertParamInfoRequest>()([
  'insert',
  'sampleRate',
]);
const MasteringInsertTimingRequestKeys = keysOf<MasteringInsertTimingRequest>()([
  'insert',
  'params',
  'sampleRate',
]);
const MasteringStreamingPreviewRequestKeys = keysOf<MasteringStreamingPreviewRequest>()([
  'samples',
  'sampleRate',
  'platforms',
  'validate',
]);
const MasteringStreamingPreviewStereoRequestKeys = keysOf<MasteringStreamingPreviewStereoRequest>()(
  ['left', 'right', 'sampleRate', 'platforms', 'validate'],
);
const MelToStftRequestKeys = keysOf<MelToStftRequest>()([
  'power',
  'sampleRate',
  'nFft',
  'fminHz',
  'fmaxHz',
  'htk',
  'validate',
]);
const MelToAudioRequestKeys = keysOf<MelToAudioRequest>()([
  'power',
  'sampleRate',
  'nFft',
  'hopLength',
  'fminHz',
  'fmaxHz',
  'htk',
  'nIter',
  'validate',
]);
const MfccToMelRequestKeys = keysOf<MfccToMelRequest>()([
  'coefficients',
  'nMels',
  'lifter',
  'validate',
]);
const MfccToAudioRequestKeys = keysOf<MfccToAudioRequest>()([
  'coefficients',
  'nMels',
  'lifter',
  'sampleRate',
  'nFft',
  'hopLength',
  'fminHz',
  'fmaxHz',
  'htk',
  'nIter',
  'validate',
]);
const MasteringRepairDereverbConfigForRoomRequestKeys =
  keysOf<MasteringRepairDereverbConfigForRoomRequest>()([
    'estimate',
    'threshold',
    'attenuation',
    'nFft',
    'hopLength',
    't60Sec',
    'lateDelayMs',
    'overSubtraction',
    'spectralFloor',
    'wpeEnabled',
    'wpeIterations',
    'wpeTaps',
    'wpeStrength',
  ]);
const MasteringRepairNoiseBandBinsRequestKeys = keysOf<MasteringRepairNoiseBandBinsRequest>()([
  'nFft',
  'sampleRate',
]);
const MelDeltaRequestKeys = keysOf<MelDeltaRequest>()(['features', 'width', 'validate']);
const MelSpectrogramRequestKeys = keysOf<MelSpectrogramRequest>()([
  'nMels',
  'fminHz',
  'fmaxHz',
  'htk',
  'samples',
  'sampleRate',
  'nFft',
  'hopLength',
  'validate',
]);
const MergeNotesRequestKeys = keysOf<MergeNotesRequest>()([
  'first',
  'last',
  'samples',
  'sampleRate',
  'f0Hz',
  'frameRate',
  'voiced',
  'voicedProb',
  'notes',
  'segmentationThresholdCents',
  'minNoteMs',
  'referenceHz',
  'voicedThreshold',
  'validate',
]);
const MeteringPeakDbRequestKeys = keysOf<MeteringPeakDbRequest>()([
  'samples',
  'sampleRate',
  'validate',
]);
const MeteringRmsDbRequestKeys = keysOf<MeteringRmsDbRequest>()([
  'samples',
  'sampleRate',
  'validate',
]);
const MeteringCrestFactorDbRequestKeys = keysOf<MeteringCrestFactorDbRequest>()([
  'samples',
  'sampleRate',
  'validate',
]);
const MeteringDcOffsetRequestKeys = keysOf<MeteringDcOffsetRequest>()([
  'samples',
  'sampleRate',
  'validate',
]);
const MeteringCrestFactorDbStereoRequestKeys = keysOf<MeteringCrestFactorDbStereoRequest>()([
  'left',
  'right',
  'sampleRate',
  'validate',
]);
const MeteringStereoCorrelationRequestKeys = keysOf<MeteringStereoCorrelationRequest>()([
  'left',
  'right',
  'sampleRate',
  'validate',
]);
const MeteringStereoWidthRequestKeys = keysOf<MeteringStereoWidthRequest>()([
  'left',
  'right',
  'sampleRate',
  'validate',
]);
const MeteringDetectClippingRequestKeys = keysOf<MeteringDetectClippingRequest>()([
  'samples',
  'sampleRate',
  'thresholdDb',
  'minRegionSamples',
  'validate',
]);
const MeteringDynamicRangeRequestKeys = keysOf<MeteringDynamicRangeRequest>()([
  'samples',
  'sampleRate',
  'windowSec',
  'hopSec',
  'lowPercentile',
  'highPercentile',
  'validate',
]);
const MeteringVectorscopeRequestKeys = keysOf<MeteringVectorscopeRequest>()([
  'left',
  'right',
  'sampleRate',
  'maxPoints',
  'validate',
]);
const MeteringPhaseScopeRequestKeys = keysOf<MeteringPhaseScopeRequest>()([
  'left',
  'right',
  'sampleRate',
  'maxPoints',
  'validate',
]);
const MeteringSilenceRatioRequestKeys = keysOf<MeteringSilenceRatioRequest>()([
  'samples',
  'sampleRate',
  'thresholdDb',
  'frameLength',
  'hopLength',
  'validate',
]);
const MeteringSpectrumRequestKeys = keysOf<MeteringSpectrumRequest>()([
  'samples',
  'sampleRate',
  'nFft',
  'applyOctaveSmoothing',
  'octaveFraction',
  'dbRef',
  'dbAmin',
  'validate',
]);
const MeteringSpectrumFrameRequestKeys = keysOf<MeteringSpectrumFrameRequest>()([
  'samples',
  'sampleRate',
  'startSample',
  'nFft',
  'applyOctaveSmoothing',
  'octaveFraction',
  'dbRef',
  'dbAmin',
  'validate',
]);
const MeteringTruePeakDbtpRequestKeys = keysOf<MeteringTruePeakDbtpRequest>()([
  'samples',
  'sampleRate',
  'oversampleFactor',
  'validate',
]);
const MfccRequestKeys = keysOf<MfccRequest>()([
  'nMfcc',
  'lifter',
  'nMels',
  'fminHz',
  'fmaxHz',
  'htk',
  'samples',
  'sampleRate',
  'nFft',
  'hopLength',
  'validate',
]);
const MixStereoRequestKeys = keysOf<MixStereoRequest>()(['strips', 'sampleRate', 'validate']);
const NnFilterRequestKeys = keysOf<NnFilterRequest>()(['s', 'aggregate', 'k', 'width']);
const NnlsChromaRequestKeys = keysOf<NnlsChromaRequest>()([
  'samples',
  'sampleRate',
  'enableStftBlend',
  'stftBlendWeight',
  'stftBlendNFft',
  'hopLength',
  'validate',
]);
const NormalizeRequestKeys = keysOf<NormalizeRequest>()([
  'samples',
  'sampleRate',
  'targetDb',
  'mode',
  'validate',
]);
const NormalizeStereoRequestKeys = keysOf<NormalizeStereoRequest>()([
  'left',
  'right',
  'sampleRate',
  'targetDb',
  'mode',
  'validate',
]);
const NoteMoveRequestKeys = keysOf<NoteMoveRequest>()([
  'samples',
  'sampleRate',
  'onsetSample',
  'onsetSec',
  'offsetSample',
  'offsetSec',
  'targetOnsetSample',
  'targetOnsetSec',
  'validate',
]);
const NoteSegmentsRequestKeys = keysOf<NoteSegmentsRequest>()([
  'f0Hz',
  'voicedProb',
  'voiced',
  'frameRate',
  'segmentationThresholdCents',
  'minNoteMs',
  'referenceHz',
  'voicedThreshold',
]);
const NoteStretchRequestKeys = keysOf<NoteStretchRequest>()([
  'samples',
  'sampleRate',
  'onsetSample',
  'onsetSec',
  'offsetSample',
  'offsetSec',
  'stretchRatio',
  'validate',
]);
const NoteTargetsFromSmfRequestKeys = keysOf<NoteTargetsFromSmfRequest>()(['data', 'trackIndex']);
const OnsetBacktrackRequestKeys = keysOf<OnsetBacktrackRequest>()(['events', 'energy']);
const OnsetStrengthMultiRequestKeys = keysOf<OnsetStrengthMultiRequest>()([
  'nBands',
  'samples',
  'sampleRate',
  'nFft',
  'hopLength',
  'nMels',
  'validate',
]);
const PadCenterRequestKeys = keysOf<PadCenterRequest>()(['values', 'targetSize', 'padValue']);
const PcenRequestKeys = keysOf<PcenRequest>()([
  'values',
  'sampleRate',
  'hopLength',
  'timeConstant',
  'gain',
  'bias',
  'power',
  'eps',
]);
const PeakPickRequestKeys = keysOf<PeakPickRequest>()([
  'values',
  'preMax',
  'postMax',
  'preAvg',
  'postAvg',
  'delta',
  'wait',
]);
const PercussiveRequestKeys = keysOf<PercussiveRequest>()(['samples', 'sampleRate', 'validate']);
const PhaseVocoderRequestKeys = keysOf<PhaseVocoderRequest>()([
  'speedRatio',
  'samples',
  'sampleRate',
  'nFft',
  'hopLength',
  'validate',
]);
const PiptrackRequestKeys = keysOf<PiptrackRequest>()([
  'samples',
  'sampleRate',
  'nFft',
  'hopLength',
  'fminHz',
  'fmaxHz',
  'threshold',
  'validate',
]);
const PitchCorrectTimevaryingRequestKeys = keysOf<PitchCorrectTimevaryingRequest>()([
  'samples',
  'f0Hz',
  'sampleRate',
  'hopLength',
  'targetMode',
  'targetMidi',
  'scaleRoot',
  'scaleModeMask',
  'scaleReferenceMidi',
  'retuneAmount',
  'maxCorrectionSemitones',
  'retuneSpeedMs',
  'vibratoThresholdCents',
  'voiced',
  'voicedProb',
  'validate',
]);
const PitchCorrectToMidiRequestKeys = keysOf<PitchCorrectToMidiRequest>()([
  'samples',
  'sampleRate',
  'currentMidi',
  'targetMidi',
  'validate',
]);
const PitchPyinRequestKeys = keysOf<PitchPyinRequest>()([
  'samples',
  'sampleRate',
  'frameLength',
  'hopLength',
  'threshold',
  'fillNa',
  'f0MinHz',
  'f0MaxHz',
  'validate',
]);
const PitchShiftRequestKeys = keysOf<PitchShiftRequest>()([
  'samples',
  'sampleRate',
  'pitchSemitones',
  'nFft',
  'hopLength',
  'validate',
]);
const PitchTuningRequestKeys = keysOf<PitchTuningRequest>()([
  'frequencies',
  'resolution',
  'binsPerOctave',
]);
const PitchYinRequestKeys = keysOf<PitchYinRequest>()([
  'samples',
  'sampleRate',
  'frameLength',
  'hopLength',
  'threshold',
  'f0MinHz',
  'f0MaxHz',
  'validate',
]);
const PlpRequestKeys = keysOf<PlpRequest>()([
  'onsetEnvelope',
  'sampleRate',
  'hopLength',
  'winLength',
  'bpmMin',
  'bpmMax',
]);
const PolyFeaturesRequestKeys = keysOf<PolyFeaturesRequest>()([
  'order',
  'samples',
  'sampleRate',
  'nFft',
  'hopLength',
  'validate',
]);
const ReassignedSpectrogramRequestKeys = keysOf<ReassignedSpectrogramRequest>()([
  'refPower',
  'fillNan',
  'samples',
  'sampleRate',
  'nFft',
  'hopLength',
  'validate',
]);
const RemixRequestKeys = keysOf<RemixRequest>()([
  'samples',
  'intervals',
  'sampleRate',
  'alignZeros',
  'validate',
]);
const RenderNotesRequestKeys = keysOf<RenderNotesRequest>()([
  'samples',
  'sampleRate',
  'notes',
  'fadeMs',
  'f0Hz',
  'voiced',
  'frameRate',
  'vibratoCutoffHz',
  'validate',
]);
const RenderPercussiveEventsRequestKeys = keysOf<RenderPercussiveEventsRequest>()([
  'samples',
  'sampleRate',
  'events',
  'fadeMs',
  'nFft',
  'hopLength',
  'hpssKernelHarmonic',
  'hpssKernelPercussive',
  'validate',
]);
const RenderPlaybackRequestKeys = keysOf<RenderPlaybackRequest>()([
  'samples',
  'numChannels',
  'sampleRate',
  'config',
  'hrtf',
  'validate',
]);
const PlaybackRendererOptionsKeys = keysOf<PlaybackRendererOptions>()([
  'config',
  'hrtf',
  'sampleRate',
  'maxBlockSize',
]);
const ClipPageProviderOptionsKeys = keysOf<ClipPageProviderOptions>()([
  'numChannels',
  'numSamples',
  'pageFrames',
]);
const PlaybackLoudnessMeterOptionsKeys = keysOf<PlaybackLoudnessMeterOptions>()([
  'numChannels',
  'sampleRate',
]);
const ResampleRequestKeys = keysOf<ResampleRequest>()([
  'samples',
  'sampleRate',
  'targetSampleRate',
]);
const VocalRestoreRequestKeys = keysOf<VocalRestoreRequest>()([
  'samples',
  'sampleRate',
  'state',
  'limits',
  'validate',
]);
const ZeroCrossingRateRequestKeys = keysOf<ZeroCrossingRateRequest>()([
  'samples',
  'sampleRate',
  'frameLength',
  'hopLength',
  'validate',
]);
const RoomGeometryFromEstimateRequestKeys = keysOf<RoomGeometryFromEstimateRequest>()([
  'estimate',
  'source',
  'listener',
]);
const RoomPlacementKeys = keysOf<RoomPlacement>()(['x', 'y', 'z']);
const RoomMorphRequestKeys = keysOf<RoomMorphRequest>()([
  'samples',
  'sampleRate',
  'wet',
  'sourceTailSuppression',
  'preferEyring',
  'mixingTimeMs',
  'crossfadeMs',
  'lengthM',
  'widthM',
  'heightM',
  'absorption',
  'bandAbsorption',
  'bandScattering',
  'materialPreset',
  'source',
  'listener',
  'ismOrder',
  'seed',
  'maxSec',
  'airAbsorptionEnabled',
  'airTemperatureC',
  'airHumidityPercent',
  'validate',
]);
const RoomMorphStereoRequestKeys = keysOf<RoomMorphStereoRequest>()([
  'left',
  'right',
  'sampleRate',
  'receiverSpacingM',
  'wet',
  'sourceTailSuppression',
  'preferEyring',
  'mixingTimeMs',
  'crossfadeMs',
  'lengthM',
  'widthM',
  'heightM',
  'absorption',
  'bandAbsorption',
  'bandScattering',
  'materialPreset',
  'source',
  'listener',
  'ismOrder',
  'seed',
  'maxSec',
  'airAbsorptionEnabled',
  'airTemperatureC',
  'airHumidityPercent',
  'validate',
]);
const SegmentAgglomerativeRequestKeys = keysOf<SegmentAgglomerativeRequest>()([
  'data',
  'k',
  'linkage',
]);
const SegmentCrossSimilarityRequestKeys = keysOf<SegmentCrossSimilarityRequest>()([
  'x',
  'y',
  'k',
  'metric',
  'mode',
]);
const SegmentLagToRecurrenceRequestKeys = keysOf<SegmentLagToRecurrenceRequest>()(['lag']);
const SegmentPathEnhanceRequestKeys = keysOf<SegmentPathEnhanceRequest>()([
  'recurrence',
  'win',
  'maxRatio',
  'minRatio',
  'nFilters',
]);
const SegmentRecurrenceMatrixRequestKeys = keysOf<SegmentRecurrenceMatrixRequest>()([
  'data',
  'k',
  'width',
  'sym',
  'metric',
  'mode',
]);
const SegmentRecurrenceToLagRequestKeys = keysOf<SegmentRecurrenceToLagRequest>()([
  'recurrence',
  'pad',
]);
const SegmentSubsegmentRequestKeys = keysOf<SegmentSubsegmentRequest>()([
  'data',
  'boundaries',
  'nSegments',
]);
const SpectralBandwidthRequestKeys = keysOf<SpectralBandwidthRequest>()([
  'samples',
  'sampleRate',
  'nFft',
  'hopLength',
  'p',
  'validate',
]);
const SpectralContrastRequestKeys = keysOf<SpectralContrastRequest>()([
  'nBands',
  'fminHz',
  'quantile',
  'samples',
  'sampleRate',
  'nFft',
  'hopLength',
  'validate',
]);
const SpectralEditRequestKeys = keysOf<SpectralEditRequest>()([
  'samples',
  'sampleRate',
  'ops',
  'nFft',
  'hopLength',
  'window',
  'healRadiusFrames',
  'validate',
]);
const SpectralFluxRequestKeys = keysOf<SpectralFluxRequest>()([
  'samples',
  'sampleRate',
  'nFft',
  'hopLength',
  'lag',
  'validate',
]);
const SpectralRolloffRequestKeys = keysOf<SpectralRolloffRequest>()([
  'rollPercent',
  'samples',
  'sampleRate',
  'nFft',
  'hopLength',
  'validate',
]);
const SplitNoteRequestKeys = keysOf<SplitNoteRequest>()([
  'index',
  'frame',
  'frameSec',
  'samples',
  'sampleRate',
  'f0Hz',
  'frameRate',
  'voiced',
  'voicedProb',
  'notes',
  'segmentationThresholdCents',
  'minNoteMs',
  'referenceHz',
  'voicedThreshold',
  'validate',
]);
const SplitSilenceCommonRequestKeys = keysOf<SplitSilenceCommonRequest>()([
  'signals',
  'topDb',
  'frameLength',
  'hopLength',
]);
const StreamingLoudnessGainRequestKeys = keysOf<StreamingLoudnessGainRequest>()([
  'samples',
  'sampleRate',
  'config',
  'validate',
]);
const StreamingLoudnessGainStereoRequestKeys = keysOf<StreamingLoudnessGainStereoRequest>()([
  'left',
  'right',
  'sampleRate',
  'config',
  'validate',
]);
const MixingAssistantSuggestRequestKeys = keysOf<MixingAssistantSuggestRequest>()([
  'tracks',
  'sampleRate',
  'targetTrackLufs',
  'suggestionStrength',
  'eqMaxCutDb',
  'mixBusHeadroomDbtp',
  'tempoBpm',
  'enableStructure',
  'enableGain',
  'enableBalance',
  'enableEq',
  'enableDynamics',
  'enableImage',
  'enableHighPass',
  'nFft',
  'hopLength',
  'validate',
]);
const SynthesizeRirRequestKeys = keysOf<SynthesizeRirRequest>()([
  'sampleRate',
  'preferEyring',
  'mixingTimeMs',
  'crossfadeMs',
  'lengthM',
  'widthM',
  'heightM',
  'absorption',
  'bandAbsorption',
  'bandScattering',
  'materialPreset',
  'source',
  'listener',
  'ismOrder',
  'seed',
  'maxSec',
  'airAbsorptionEnabled',
  'airTemperatureC',
  'airHumidityPercent',
]);
const TempogramRequestKeys = keysOf<TempogramRequest>()([
  'onsetEnvelope',
  'sampleRate',
  'hopLength',
  'winLength',
  'mode',
  'center',
  'norm',
]);
const TempogramRatioRequestKeys = keysOf<TempogramRatioRequest>()([
  'tempogram',
  'sampleRate',
  'hopLength',
  'factors',
  'validate',
]);
const TimeStretchRequestKeys = keysOf<TimeStretchRequest>()([
  'samples',
  'sampleRate',
  'speedRatio',
  'nFft',
  'hopLength',
  'validate',
]);
const ToneRequestKeys = keysOf<ToneRequest>()([
  'frequencyHz',
  'sampleRate',
  'durationSec',
  'phase',
  'amplitude',
]);
const TonnetzRequestKeys = keysOf<TonnetzRequest>()(['chromagram']);
const TranscribeRequestKeys = keysOf<TranscribeRequest>()([
  'samples',
  'sampleRate',
  'tempoBpm',
  'polyphonic',
  'referenceHz',
  'minNoteMs',
  'minNoteDivision',
  'segmentationThresholdCents',
  'monophonicModel',
  'velocityFloorDb',
  'fixedVelocity',
  'maxPolyphony',
  'minFramePeakRatio',
  'minRidgePeakRatio',
  'reattackRatio',
  'group',
  'channel',
  'f0MinHz',
  'f0MaxHz',
  'validate',
]);
const TrimRequestKeys = keysOf<TrimRequest>()([
  'samples',
  'sampleRate',
  'thresholdDb',
  'frameLength',
  'hopLength',
  'validate',
]);
const VectorNormalizeRequestKeys = keysOf<VectorNormalizeRequest>()([
  'values',
  'norm',
  'threshold',
]);
const VoiceChangeRequestKeys = keysOf<VoiceChangeRequest>()([
  'samples',
  'sampleRate',
  'pitchSemitones',
  'formantFactor',
  'formantMode',
  'validate',
]);
const VoiceChangeRealtimeRequestKeys = keysOf<VoiceChangeRealtimeRequest>()([
  'channels',
  'sampleRate',
  'config',
  'validate',
]);
const VqtRequestKeys = keysOf<VqtRequest>()([
  'gamma',
  'samples',
  'sampleRate',
  'hopLength',
  'fminHz',
  'nBins',
  'binsPerOctave',
  'validate',
]);
const VqtToAudioRequestKeys = keysOf<VqtToAudioRequest>()([
  'gamma',
  'magnitude',
  'sampleRate',
  'hopLength',
  'fminHz',
  'binsPerOctave',
  'nIter',
  'validate',
]);
const WaveformPeakPyramidRequestKeys = keysOf<WaveformPeakPyramidRequest>()([
  'channels',
  'samplesPerBucketLevels',
  'validate',
]);
const WaveformPeaksRequestKeys = keysOf<WaveformPeaksRequest>()([
  'channels',
  'samplesPerBucket',
  'validate',
]);
const ZeroCrossingsRequestKeys = keysOf<ZeroCrossingsRequest>()([
  'samples',
  'threshold',
  'refMagnitude',
  'pad',
  'zeroPos',
  'sampleRate',
  'validate',
]);

const SetPartRigRequestKeys = keysOf<PartRigRequest>()([
  'destinationId',
  'part',
  'mode',
  'inserts',
]);
const RenderOfflineRequestKeys = keysOf<RenderOfflineRequest>()([
  'channels',
  'blockSize',
  'finalize',
  'validate',
]);
const EngineConfigRequestKeys = keysOf<RealtimeEngineConfig>()([
  'sampleRate',
  'maxBlockSize',
  'commandCapacity',
  'telemetryCapacity',
  'maxChannels',
]);
const SidechainSourceRequestKeys = keysOf<SidechainSource>()(['kind', 'id']);
const InsertBypassRequestKeys = keysOf<{ bypassed: boolean; resetOnBypass?: boolean }>()([
  'bypassed',
  'resetOnBypass',
]);
const InsertParamRequestKeys = keysOf<{ name: string; value: number }>()(['name', 'value']);
const ConfigureCaptureRequestKeys = keysOf<CaptureOptions>()([
  'capacityFrames',
  'numChannels',
  'source',
  'recordOffsetSamples',
  'inputMonitor',
  'punch',
]);
const CaptureInputMonitorKeys = keysOf<NonNullable<CaptureOptions['inputMonitor']>>()([
  'enabled',
  'gain',
]);
const CapturePunchKeys = keysOf<CapturePunch>()([
  'startPpq',
  'startSample',
  'startSec',
  'endPpq',
  'endSample',
  'endSec',
]);
const ArmCaptureRequestKeys = keysOf<{ armed: boolean }>()(['armed']);
const ScheduleParamRequestKeys = keysOf<EngineAutomationPoint>()(['ppq', 'value', 'curveToNext']);
const SonareEngineRenderOfflineRequestKeys = keysOf<SonareEngineRenderOfflineRequest>()([
  'totalFrames',
  'blockSize',
  'finalize',
  'validate',
]);
const SetLoopRequestKeys = keysOf<EngineLoop>()(['startPpq', 'endPpq', 'enabled']);
const SetMetronomeRequestKeys = keysOf<EngineMetronomeConfig>()([
  'enabled',
  'beatGain',
  'accentGain',
  'clickSamples',
  'clickSec',
]);
const SetTrackSoloMuteRequestKeys = keysOf<{ solo: boolean; mute: boolean }>()(['solo', 'mute']);
const MidiBankRequestKeys = keysOf<{ msb: number; lsb: number }>()(['msb', 'lsb']);
const MixerConfigRequestKeys = keysOf<MixerConfig>()(['scene', 'sampleRate', 'maxBlockSize']);
const MixerJsonOptionsRequestKeys = keysOf<MixerJsonOptions>()(['sampleRate', 'maxBlockSize']);
const ConfigureMeterRequestKeys = keysOf<MixerMeterOptions>()([
  'enabled',
  'truePeakOversample',
  'measureLufs',
  'measureTruePeak',
]);
const SetPolarityInvertRequestKeys = keysOf<PolarityInvert>()(['left', 'right']);
const MixerAddSendRequestKeys = keysOf<MixerSend>()(['id', 'destinationBusId', 'sendDb', 'timing']);
const ScheduleInsertAutomationRequestKeys = keysOf<InsertAutomationPoint>()([
  'paramId',
  'samplePos',
  'value',
  'curve',
]);
const ScheduleFaderAutomationRequestKeys = keysOf<FaderAutomationPoint>()([
  'samplePos',
  'faderDb',
  'curve',
]);
const SchedulePanAutomationRequestKeys = keysOf<PanAutomationPoint>()([
  'samplePos',
  'pan',
  'curve',
]);
const ScheduleWidthAutomationRequestKeys = keysOf<WidthAutomationPoint>()([
  'samplePos',
  'width',
  'curve',
]);
const ScheduleSendAutomationRequestKeys = keysOf<SendAutomationPoint>()([
  'samplePos',
  'sendDb',
  'curve',
]);
const ImportExternalSeparatedStemRequestKeys = keysOf<ExternalSeparatedStemImportRequest>()([
  'sampleRate',
  'stems',
]);
const BakeMidiFxRequestKeys = keysOf<ProjectMidiFxBakeRequest>()([
  'clipId',
  'config',
  'withSourceIndex',
]);
const PreviewMidiFxRequestKeys = keysOf<ProjectMidiFxPreviewRequest>()(['clipId', 'config']);

const Gm2DrumNameRequestKeys = keysOf<Gm2DrumNameRequest>()(['bankLsb', 'note']);

const Gm2InstrumentNameRequestKeys = keysOf<Gm2InstrumentNameRequest>()(['bankLsb', 'program']);

const Midi2AssignableControllerRequestKeys = keysOf<Midi2AssignableControllerRequest>()([
  'ppq',
  'group',
  'channel',
  'bank',
  'index',
  'value32',
]);

const Midi2AssignablePerNoteControllerRequestKeys =
  keysOf<Midi2AssignablePerNoteControllerRequest>()([
    'ppq',
    'group',
    'channel',
    'note',
    'index',
    'value32',
  ]);

const Midi2CcRequestKeys = keysOf<Midi2CcRequest>()([
  'ppq',
  'group',
  'channel',
  'controller',
  'value32',
]);

const Midi2ChannelPressureRequestKeys = keysOf<Midi2ChannelPressureRequest>()([
  'ppq',
  'group',
  'channel',
  'pressure32',
]);

const Midi2NoteOffRequestKeys = keysOf<Midi2NoteOffRequest>()([
  'ppq',
  'group',
  'channel',
  'note',
  'velocity16',
]);

const Midi2NoteOnRequestKeys = keysOf<Midi2NoteOnRequest>()([
  'ppq',
  'group',
  'channel',
  'note',
  'velocity16',
  'attributeType',
  'attributeData',
]);

const Midi2PerNoteManagementRequestKeys = keysOf<Midi2PerNoteManagementRequest>()([
  'ppq',
  'group',
  'channel',
  'note',
  'detach',
  'reset',
]);

const Midi2PerNotePitchBendRequestKeys = keysOf<Midi2PerNotePitchBendRequest>()([
  'ppq',
  'group',
  'channel',
  'note',
  'bend32',
]);

const Midi2PitchBendRequestKeys = keysOf<Midi2PitchBendRequest>()([
  'ppq',
  'group',
  'channel',
  'bend32',
]);

const Midi2PolyPressureRequestKeys = keysOf<Midi2PolyPressureRequest>()([
  'ppq',
  'group',
  'channel',
  'note',
  'pressure32',
]);

const Midi2ProgramRequestKeys = keysOf<Midi2ProgramRequest>()([
  'ppq',
  'group',
  'channel',
  'program',
  'bank',
]);

const Midi2RegisteredControllerRequestKeys = keysOf<Midi2RegisteredControllerRequest>()([
  'ppq',
  'group',
  'channel',
  'bank',
  'index',
  'value32',
]);

const Midi2RegisteredPerNoteControllerRequestKeys =
  keysOf<Midi2RegisteredPerNoteControllerRequest>()([
    'ppq',
    'group',
    'channel',
    'note',
    'index',
    'value32',
  ]);

const Midi2RelativeAssignableControllerRequestKeys =
  keysOf<Midi2RelativeAssignableControllerRequest>()([
    'ppq',
    'group',
    'channel',
    'bank',
    'index',
    'delta32',
  ]);

const Midi2RelativeRegisteredControllerRequestKeys =
  keysOf<Midi2RelativeRegisteredControllerRequest>()([
    'ppq',
    'group',
    'channel',
    'bank',
    'index',
    'delta32',
  ]);

const MidiBankProgramRequestKeys = keysOf<MidiBankProgramRequest>()([
  'ppq',
  'group',
  'channel',
  'program',
  'bank',
]);

const MidiCcRequestKeys = keysOf<MidiCcRequest>()([
  'ppq',
  'group',
  'channel',
  'controller',
  'value',
]);

const MidiChannelPressureRequestKeys = keysOf<MidiChannelPressureRequest>()([
  'ppq',
  'group',
  'channel',
  'pressure',
]);

const MidiNoteOffRequestKeys = keysOf<MidiNoteOffRequest>()([
  'ppq',
  'group',
  'channel',
  'note',
  'velocity',
]);

const MidiNoteOnRequestKeys = keysOf<MidiNoteOnRequest>()([
  'ppq',
  'group',
  'channel',
  'note',
  'velocity',
]);

const MidiParamToCcRequestKeys = keysOf<MidiParamToCcRequest>()([
  'bindings',
  'paramId',
  'unitValue',
  'group',
  'ppq',
]);

const MidiRouteEventsRequestKeys = keysOf<MidiRouteEventsRequest>()([
  'events',
  'filterGroup',
  'filterChannel',
  'remapChannel',
  'thru',
]);

const MidiCcLearnRequestKeys = keysOf<MidiCcLearnRequest>()([
  'events',
  'paramId',
  'minValue',
  'maxValue',
  'minMovement',
]);

const MidiCcToBreakpointRequestKeys = keysOf<MidiCcToBreakpointRequest>()(['bindings', 'event']);

const MidiPitchBendRequestKeys = keysOf<MidiPitchBendRequest>()([
  'ppq',
  'group',
  'channel',
  'bend',
]);

const MidiPolyPressureRequestKeys = keysOf<MidiPolyPressureRequest>()([
  'ppq',
  'group',
  'channel',
  'note',
  'pressure',
]);

const MidiProgramRequestKeys = keysOf<MidiProgramRequest>()(['ppq', 'group', 'channel', 'program']);
const ProjectAddClipRequestKeys = keysOf<ProjectClipDesc>()([
  'trackId',
  'isMidi',
  'startPpq',
  'lengthPpq',
  'sourceOffsetPpq',
  'gain',
  'channels',
  'sampleRate',
  'sourceUri',
]);
const ProjectSetMarkerRequestKeys = keysOf<ProjectMarkerInput>()([
  'id',
  'ppq',
  'name',
  'kind',
  'keyFifths',
  'keyMinor',
]);
const ProjectMoveClipRequestKeys = keysOf<ProjectMoveClipOptions>()(['trackId']);
const ProjectSetClipTakesRequestKeys = keysOf<ProjectClipTakesOptions>()(['activeTakeId']);
const ProjectSetClipFadeRequestKeys = keysOf<ProjectClipFadeInput>()([
  'fadeInPpq',
  'fadeInCurve',
  'fadeOutPpq',
  'fadeOutCurve',
]);
const ProjectSetClipLoopRequestKeys = keysOf<ProjectClipLoopInput>()([
  'loopMode',
  'loopLengthPpq',
  'loopCrossfadePpq',
]);
const ProjectSetProgramOnChannelRequestKeys = keysOf<ProjectProgramInput>()([
  'group',
  'channel',
  'program',
  'bank',
]);
const ProjectSetSourceAudioRequestKeys = keysOf<ProjectSourceAudioInput>()([
  'sourceId',
  'channels',
  'sampleRate',
]);
const ProjectSetAudioSourceMetadataRequestKeys = keysOf<ProjectAudioSourceMetadataInput>()([
  'sourceId',
  'contentHash',
  'externalStemRole',
]);
const ProjectSetAssistSidecarRequestKeys = keysOf<ProjectAssistSidecarInput>()([
  'moduleId',
  'schemaVersion',
  'targetTrackId',
  'regionStartPpq',
  'regionEndPpq',
  'payload',
]);
const ProjectBounceRequestKeys = keysOf<ProjectBounceOptions>()([
  'totalFrames',
  'blockSize',
  'numChannels',
  'instrumentLatencySamples',
]);
const ProjectAutoTempoRequestKeys = keysOf<ProjectAutoTempoRequest>()([
  'samples',
  'sampleRate',
  'candidateIndex',
  'applyTimeSignatures',
  'adaptiveTempo',
  'tempoUpdateIntervalBeats',
  'rampThreshold',
  'includeOctaveCandidates',
]);
const ProjectAnalyzeTempoRequestKeys = keysOf<ProjectAnalyzeTempoRequest>()([
  'samples',
  'sampleRate',
  'adaptiveTempo',
  'tempoUpdateIntervalBeats',
  'rampThreshold',
  'includeOctaveCandidates',
]);
const TranscribeToClipRequestKeys = keysOf<ProjectTranscribeRequest>()([
  'clipId',
  'samples',
  'sampleRate',
  'polyphonic',
  'referenceHz',
  'minNoteMs',
  'minNoteDivision',
  'segmentationThresholdCents',
  'monophonicModel',
  'velocityFloorDb',
  'fixedVelocity',
  'maxPolyphony',
  'minFramePeakRatio',
  'minRidgePeakRatio',
  'reattackRatio',
  'group',
  'channel',
  'f0MinHz',
  'f0MaxHz',
]);
const ApplyProjectVocalEditRequestKeys = keysOf<ProjectVocalEditApplyRequest>()([
  'clipId',
  'takeId',
  'expectedSourceId',
  'expectedSourceSampleRate',
  'expectedSourceSampleCount',
  'expectedSourceSha256',
  'expectedClipLengthPpq',
  'expectedSourceOffsetPpq',
  'renderedMono',
  'renderedSampleRate',
  'renderToken',
  'vocalState',
]);
const ClipPageStreamerOptionsKeys = keysOf<ClipPageStreamerOptions>()([
  'engine',
  'readAheadPages',
  'retainBehindPages',
  'maxRequestsPerPump',
]);
const VocalApplyRequestKeys = keysOf<VocalApplyRequest>()(['expectedGeneration', 'operations']);
const VocalRenderRequestKeys = keysOf<VocalRenderRequest>()(['range', 'requestId', 'cancel']);

/** Request shape of every entry point that takes a request object, by entry-point name. */
export const REQUEST_SHAPES = {
  setPartRig: { keys: SetPartRigRequestKeys },
  renderOffline: { keys: RenderOfflineRequestKeys },
  RealtimeEngine: { keys: EngineConfigRequestKeys },
  ClipPageStreamer: { keys: ClipPageStreamerOptionsKeys },
  prepare: { keys: EngineConfigRequestKeys },
  setBusSidechain: { keys: SidechainSourceRequestKeys },
  setStripSidechain: { keys: SidechainSourceRequestKeys },
  canSetBusSidechain: { keys: SidechainSourceRequestKeys },
  canSetStripSidechain: { keys: SidechainSourceRequestKeys },
  setStripInsertBypassed: { keys: InsertBypassRequestKeys },
  setBusInsertBypassed: { keys: InsertBypassRequestKeys },
  setStripInsertParam: { keys: InsertParamRequestKeys },
  setBusInsertParam: { keys: InsertParamRequestKeys },
  applyStripInsertParamNow: { keys: InsertParamRequestKeys },
  applyBusInsertParamNow: { keys: InsertParamRequestKeys },
  restoreStripInsertParam: { keys: InsertParamRequestKeys },
  restoreBusInsertParam: { keys: InsertParamRequestKeys },
  configureCapture: {
    keys: ConfigureCaptureRequestKeys,
    nested: { inputMonitor: CaptureInputMonitorKeys, punch: CapturePunchKeys },
  },
  armCapture: { keys: ArmCaptureRequestKeys },
  scheduleParam: { keys: ScheduleParamRequestKeys },
  'SonareEngine.renderOffline': { keys: SonareEngineRenderOfflineRequestKeys },
  setLoop: { keys: SetLoopRequestKeys },
  setMetronome: { keys: SetMetronomeRequestKeys },
  setTrackSoloMute: { keys: SetTrackSoloMuteRequestKeys },
  pushMidiProgram: { keys: MidiBankRequestKeys },
  pushMidiInputProgram: { keys: MidiBankRequestKeys },
  Mixer: { keys: MixerConfigRequestKeys },
  PlaybackRenderer: { keys: PlaybackRendererOptionsKeys },
  PlaybackLoudnessMeter: { keys: PlaybackLoudnessMeterOptionsKeys },
  ClipPageProvider: { keys: ClipPageProviderOptionsKeys },
  'Mixer.fromJson': { keys: MixerJsonOptionsRequestKeys },
  configureMeter: { keys: ConfigureMeterRequestKeys },
  setPolarityInvert: { keys: SetPolarityInvertRequestKeys },
  addSend: { keys: MixerAddSendRequestKeys },
  scheduleInsertAutomation: { keys: ScheduleInsertAutomationRequestKeys },
  scheduleFaderAutomation: { keys: ScheduleFaderAutomationRequestKeys },
  schedulePanAutomation: { keys: SchedulePanAutomationRequestKeys },
  scheduleWidthAutomation: { keys: ScheduleWidthAutomationRequestKeys },
  scheduleSendAutomation: { keys: ScheduleSendAutomationRequestKeys },
  importExternalSeparatedStem: { keys: ImportExternalSeparatedStemRequestKeys },
  bakeMidiFx: { keys: BakeMidiFxRequestKeys },
  previewMidiFx: { keys: PreviewMidiFxRequestKeys },
  gm2DrumName: { keys: Gm2DrumNameRequestKeys },
  gm2InstrumentName: { keys: Gm2InstrumentNameRequestKeys },
  midi2AssignableController: { keys: Midi2AssignableControllerRequestKeys },
  midi2AssignablePerNoteController: { keys: Midi2AssignablePerNoteControllerRequestKeys },
  midi2Cc: { keys: Midi2CcRequestKeys },
  midi2ChannelPressure: { keys: Midi2ChannelPressureRequestKeys },
  midi2NoteOff: { keys: Midi2NoteOffRequestKeys },
  midi2NoteOn: { keys: Midi2NoteOnRequestKeys },
  midi2PerNoteManagement: { keys: Midi2PerNoteManagementRequestKeys },
  midi2PerNotePitchBend: { keys: Midi2PerNotePitchBendRequestKeys },
  midi2PitchBend: { keys: Midi2PitchBendRequestKeys },
  midi2PolyPressure: { keys: Midi2PolyPressureRequestKeys },
  midi2Program: { keys: Midi2ProgramRequestKeys, nested: { bank: ['msb', 'lsb'] } },
  midi2RegisteredController: { keys: Midi2RegisteredControllerRequestKeys },
  midi2RegisteredPerNoteController: { keys: Midi2RegisteredPerNoteControllerRequestKeys },
  midi2RelativeAssignableController: { keys: Midi2RelativeAssignableControllerRequestKeys },
  midi2RelativeRegisteredController: { keys: Midi2RelativeRegisteredControllerRequestKeys },
  midiBankProgram: { keys: MidiBankProgramRequestKeys, nested: { bank: ['msb', 'lsb'] } },
  midiCc: { keys: MidiCcRequestKeys },
  midiChannelPressure: { keys: MidiChannelPressureRequestKeys },
  midiNoteOff: { keys: MidiNoteOffRequestKeys },
  midiNoteOn: { keys: MidiNoteOnRequestKeys },
  midiParamToCc: { keys: MidiParamToCcRequestKeys },
  midiRouteEvents: { keys: MidiRouteEventsRequestKeys },
  midiCcLearn: { keys: MidiCcLearnRequestKeys },
  midiCcToBreakpoint: { keys: MidiCcToBreakpointRequestKeys },
  midiPitchBend: { keys: MidiPitchBendRequestKeys },
  midiPolyPressure: { keys: MidiPolyPressureRequestKeys },
  midiProgram: { keys: MidiProgramRequestKeys },
  'Project.addClip': { keys: ProjectAddClipRequestKeys },
  'Project.setMarker': { keys: ProjectSetMarkerRequestKeys },
  'Project.moveClip': { keys: ProjectMoveClipRequestKeys },
  'Project.setClipTakes': { keys: ProjectSetClipTakesRequestKeys },
  'Project.setClipFade': { keys: ProjectSetClipFadeRequestKeys },
  'Project.setClipLoop': { keys: ProjectSetClipLoopRequestKeys },
  'Project.setProgramOnChannel': {
    keys: ProjectSetProgramOnChannelRequestKeys,
    nested: { bank: ['msb', 'lsb'] },
  },
  'Project.setSourceAudio': { keys: ProjectSetSourceAudioRequestKeys },
  'Project.setAudioSourceMetadata': { keys: ProjectSetAudioSourceMetadataRequestKeys },
  'Project.setAssistSidecar': { keys: ProjectSetAssistSidecarRequestKeys },
  'Project.bounce': { keys: ProjectBounceRequestKeys },
  'Project.autoTempo': { keys: ProjectAutoTempoRequestKeys },
  'Project.analyzeTempo': { keys: ProjectAnalyzeTempoRequestKeys },
  transcribeToClip: { keys: TranscribeToClipRequestKeys },
  applyProjectVocalEdit: { keys: ApplyProjectVocalEditRequestKeys },
  vocalApply: { keys: VocalApplyRequestKeys },
  vocalRender: { keys: VocalRenderRequestKeys },
  alignTakeToReference: { keys: AlignTakeToReferenceRequestKeys },
  amplitudeToDb: { keys: AmplitudeToDbRequestKeys },
  analyze: { keys: AnalyzeRequestKeys },
  analyzeBpm: { keys: AnalyzeBpmRequestKeys },
  analyzeDynamics: { keys: AnalyzeDynamicsRequestKeys },
  analyzeImpulseResponse: { keys: AnalyzeImpulseResponseRequestKeys },
  analyzeMelody: { keys: AnalyzeMelodyRequestKeys },
  analyzePolyphonic: { keys: AnalyzePolyphonicRequestKeys },
  analyzeRhythm: { keys: AnalyzeRhythmRequestKeys },
  analyzeSections: { keys: AnalyzeSectionsRequestKeys },
  analyzeTimbre: { keys: AnalyzeTimbreRequestKeys },
  assignNoteTargets: { keys: AssignNoteTargetsRequestKeys },
  autoTune: { keys: AutoTuneRequestKeys, hints: { key: AutoTuneKeyKeys } },
  bassChroma: { keys: BassChromaRequestKeys },
  chirp: { keys: ChirpRequestKeys },
  chordFunctionalAnalysis: { keys: ChordFunctionalAnalysisRequestKeys },
  chordFunctions: { keys: ChordFunctionsRequestKeys },
  chroma: { keys: ChromaRequestKeys },
  chromaCens: { keys: ChromaCensRequestKeys },
  chromaCqt: { keys: ChromaCqtRequestKeys },
  clicks: { keys: ClicksRequestKeys },
  cqt: { keys: CqtRequestKeys },
  cqtToAudio: { keys: CqtToAudioRequestKeys },
  createVocalEditSession: {
    keys: VocalCreateRequestKeys,
    nested: { limits: VocalSessionLimitsKeys },
    hints: { analysis: VocalAnalysisInputKeys },
  },
  cyclicTempogram: { keys: CyclicTempogramRequestKeys },
  decompose: { keys: DecomposeRequestKeys },
  decomposeNotePitch: { keys: DecomposeNotePitchRequestKeys },
  decomposeStems: { keys: DecomposeStemsRequestKeys },
  decomposeStemsLinked: { keys: DecomposeStemsLinkedRequestKeys },
  deemphasis: { keys: DeemphasisRequestKeys },
  detectAcoustic: { keys: DetectAcousticRequestKeys },
  detectBeats: { keys: DetectBeatsRequestKeys },
  detectBoundaries: { keys: DetectBoundariesRequestKeys },
  dbToAmplitude: { keys: DbToAmplitudeRequestKeys },
  dbToPower: { keys: DbToPowerRequestKeys },
  detectBpm: { keys: DetectBpmRequestKeys },
  detectChords: { keys: DetectChordsRequestKeys },
  detectDownbeats: { keys: DetectDownbeatsRequestKeys },
  detectKey: { keys: DetectKeyRequestKeys },
  detectKeyCandidates: { keys: DetectKeyCandidatesRequestKeys },
  detectOnsets: { keys: DetectOnsetsRequestKeys },
  ebur128LoudnessRangeLu: { keys: Ebur128LoudnessRangeLuRequestKeys },
  estimateMeter: { keys: EstimateMeterRequestKeys },
  estimateRoom: { keys: EstimateRoomRequestKeys },
  estimateTuning: { keys: EstimateTuningRequestKeys },
  extractNotes: { keys: ExtractNotesRequestKeys },
  extractPercussiveEvents: { keys: ExtractPercussiveEventsRequestKeys },
  fixFrames: { keys: FixFramesRequestKeys },
  fixLength: { keys: FixLengthRequestKeys },
  fourierTempogram: { keys: FourierTempogramRequestKeys },
  frameSignal: { keys: FrameSignalRequestKeys },
  griffinLim: { keys: GriffinLimRequestKeys },
  harmonic: { keys: HarmonicRequestKeys },
  hpss: { keys: HpssRequestKeys },
  hpssWithResidual: { keys: HpssWithResidualRequestKeys },
  hybridCqt: { keys: HybridCqtRequestKeys },
  lufs: { keys: LufsRequestKeys },
  lufsMultichannel: { keys: LufsMultichannelRequestKeys },
  lufsSeriesMultichannel: { keys: LufsSeriesMultichannelRequestKeys },
  masterAudio: { keys: MasterAudioRequestKeys },
  masterAudioStereo: { keys: MasterAudioStereoRequestKeys },
  masteringAbMatchLoudness: { keys: MasteringAbMatchLoudnessRequestKeys },
  masteringAbMatchLoudnessStereo: { keys: MasteringAbMatchLoudnessStereoRequestKeys },
  masteringNormalizeLoudness: { keys: MasteringNormalizeLoudnessRequestKeys },
  masteringAnalyzePair: { keys: MasteringAnalyzePairRequestKeys },
  masteringAnalyzeStereo: { keys: MasteringAnalyzeStereoRequestKeys },
  masteringApplyPairProcessor: { keys: MasteringApplyPairProcessorRequestKeys },
  masteringApplyPairProcessorStereo: { keys: MasteringApplyPairProcessorStereoRequestKeys },
  masteringApplyProcessor: { keys: MasteringApplyProcessorRequestKeys },
  masteringApplyProcessorStereo: { keys: MasteringApplyProcessorStereoRequestKeys },
  masteringAssistantSuggest: { keys: MasteringAssistantSuggestRequestKeys },
  masteringAssistantSuggestChain: { keys: MasteringAssistantSuggestRequestKeys },
  masteringAssistantSuggestChainStereo: { keys: MasteringAssistantSuggestStereoRequestKeys },
  masteringAssistantSuggestStereo: { keys: MasteringAssistantSuggestStereoRequestKeys },
  masteringAudioProfile: { keys: MasteringAudioProfileRequestKeys },
  masteringAudioProfileStereo: { keys: MasteringAudioProfileStereoRequestKeys },
  masteringDynamicsCompressor: { keys: MasteringDynamicsCompressorRequestKeys },
  masteringDynamicsGate: { keys: MasteringDynamicsGateRequestKeys },
  masteringDynamicsTransientShaper: { keys: MasteringDynamicsTransientShaperRequestKeys },
  masteringInsertParamInfo: { keys: MasteringInsertParamInfoRequestKeys },
  masteringInsertTiming: { keys: MasteringInsertTimingRequestKeys },
  masteringRepairAnalyze: { keys: MasteringRepairAnalyzeRequestKeys },
  masteringRepairApply: { keys: MasteringRepairApplyRequestKeys },
  masteringRepairDeclick: { keys: MasteringRepairDeclickRequestKeys },
  masteringRepairDeclickStereo: { keys: MasteringRepairDeclickStereoRequestKeys },
  masteringRepairDeclip: { keys: MasteringRepairDeclipRequestKeys },
  masteringRepairDeclipStereo: { keys: MasteringRepairDeclipStereoRequestKeys },
  masteringRepairDecrackle: { keys: MasteringRepairDecrackleRequestKeys },
  masteringRepairDecrackleStereo: { keys: MasteringRepairDecrackleStereoRequestKeys },
  masteringRepairDehum: { keys: MasteringRepairDehumRequestKeys },
  masteringRepairDehumStereo: { keys: MasteringRepairDehumStereoRequestKeys },
  masteringRepairDenoiseClassical: { keys: MasteringRepairDenoiseClassicalRequestKeys },
  masteringRepairDenoiseClassicalLinked: { keys: MasteringRepairDenoiseClassicalLinkedRequestKeys },
  masteringRepairDenoiseClassicalStereo: { keys: MasteringRepairDenoiseClassicalStereoRequestKeys },
  masteringRepairDereverbClassical: { keys: MasteringRepairDereverbClassicalRequestKeys },
  masteringRepairDereverbClassicalLinked: {
    keys: MasteringRepairDereverbClassicalLinkedRequestKeys,
  },
  masteringRepairDereverbClassicalStereo: {
    keys: MasteringRepairDereverbClassicalStereoRequestKeys,
  },
  masteringRepairDereverbConfigForRoom: {
    keys: MasteringRepairDereverbConfigForRoomRequestKeys,
  },
  masteringRepairDetectClicks: { keys: MasteringRepairDetectClicksRequestKeys },
  masteringRepairDetectClipping: { keys: MasteringRepairDetectClippingRequestKeys },
  masteringRepairDetectCrackle: { keys: MasteringRepairDetectCrackleRequestKeys },
  masteringRepairDetectHum: { keys: MasteringRepairDetectHumRequestKeys },
  masteringRepairDetectNoiseFloor: { keys: MasteringRepairDetectNoiseFloorRequestKeys },
  masteringRepairDetectReverb: { keys: MasteringRepairDetectReverbRequestKeys },
  masteringRepairDetectTrimRange: { keys: MasteringRepairDetectTrimRangeRequestKeys },
  masteringRepairDetectTrimRangeStereo: { keys: MasteringRepairDetectTrimRangeStereoRequestKeys },
  masteringRepairNoiseBandBins: { keys: MasteringRepairNoiseBandBinsRequestKeys },
  masteringRepairTrimSilence: { keys: MasteringRepairTrimSilenceRequestKeys },
  masteringRepairTrimSilenceStereo: { keys: MasteringRepairTrimSilenceStereoRequestKeys },
  masteringStreamingPreview: { keys: MasteringStreamingPreviewRequestKeys },
  masteringStreamingPreviewStereo: { keys: MasteringStreamingPreviewStereoRequestKeys },
  melDelta: { keys: MelDeltaRequestKeys },
  melSpectrogram: { keys: MelSpectrogramRequestKeys },
  melToAudio: { keys: MelToAudioRequestKeys },
  melToStft: { keys: MelToStftRequestKeys },
  mergeNotes: { keys: MergeNotesRequestKeys },
  meteringCrestFactorDb: { keys: MeteringCrestFactorDbRequestKeys },
  meteringCrestFactorDbStereo: { keys: MeteringCrestFactorDbStereoRequestKeys },
  meteringDcOffset: { keys: MeteringDcOffsetRequestKeys },
  meteringDetectClipping: { keys: MeteringDetectClippingRequestKeys },
  meteringDynamicRange: { keys: MeteringDynamicRangeRequestKeys },
  meteringPeakDb: { keys: MeteringPeakDbRequestKeys },
  meteringPhaseScope: { keys: MeteringPhaseScopeRequestKeys },
  meteringRmsDb: { keys: MeteringRmsDbRequestKeys },
  meteringSilenceRatio: { keys: MeteringSilenceRatioRequestKeys },
  meteringSpectrum: { keys: MeteringSpectrumRequestKeys },
  meteringSpectrumFrame: { keys: MeteringSpectrumFrameRequestKeys },
  meteringStereoCorrelation: { keys: MeteringStereoCorrelationRequestKeys },
  meteringStereoWidth: { keys: MeteringStereoWidthRequestKeys },
  meteringTruePeakDbtp: { keys: MeteringTruePeakDbtpRequestKeys },
  meteringVectorscope: { keys: MeteringVectorscopeRequestKeys },
  mfcc: { keys: MfccRequestKeys },
  mfccToAudio: { keys: MfccToAudioRequestKeys },
  mfccToMel: { keys: MfccToMelRequestKeys },
  mixStereo: { keys: MixStereoRequestKeys },
  mixingAssistantSuggest: { keys: MixingAssistantSuggestRequestKeys },
  momentaryLufs: { keys: MomentaryLufsRequestKeys },
  nnFilter: { keys: NnFilterRequestKeys },
  nnlsChroma: { keys: NnlsChromaRequestKeys },
  normalize: { keys: NormalizeRequestKeys },
  normalizeStereo: { keys: NormalizeStereoRequestKeys },
  noteMove: { keys: NoteMoveRequestKeys },
  noteSegments: { keys: NoteSegmentsRequestKeys },
  noteStretch: { keys: NoteStretchRequestKeys },
  noteTargetsFromSmf: { keys: NoteTargetsFromSmfRequestKeys },
  onsetBacktrack: { keys: OnsetBacktrackRequestKeys },
  onsetStrength: { keys: OnsetStrengthRequestKeys },
  onsetStrengthMulti: { keys: OnsetStrengthMultiRequestKeys },
  padCenter: { keys: PadCenterRequestKeys },
  pcen: { keys: PcenRequestKeys },
  peakPick: { keys: PeakPickRequestKeys },
  percussive: { keys: PercussiveRequestKeys },
  phaseVocoder: { keys: PhaseVocoderRequestKeys },
  piptrack: { keys: PiptrackRequestKeys },
  pitchCorrectTimevarying: { keys: PitchCorrectTimevaryingRequestKeys },
  pitchCorrectToMidi: { keys: PitchCorrectToMidiRequestKeys },
  pitchPyin: { keys: PitchPyinRequestKeys },
  pitchShift: { keys: PitchShiftRequestKeys },
  pitchTuning: { keys: PitchTuningRequestKeys },
  pitchYin: { keys: PitchYinRequestKeys },
  plp: { keys: PlpRequestKeys },
  polyFeatures: { keys: PolyFeaturesRequestKeys },
  powerToDb: { keys: PowerToDbRequestKeys },
  preemphasis: { keys: PreemphasisRequestKeys },
  pseudoCqt: { keys: PseudoCqtRequestKeys },
  reassignedSpectrogram: { keys: ReassignedSpectrogramRequestKeys },
  remix: { keys: RemixRequestKeys },
  remixAlignedIntervals: { keys: RemixRequestKeys },
  renderNotes: { keys: RenderNotesRequestKeys },
  renderPercussiveEvents: { keys: RenderPercussiveEventsRequestKeys },
  renderPlayback: { keys: RenderPlaybackRequestKeys },
  resample: { keys: ResampleRequestKeys },
  restoreVocalEditSession: {
    keys: VocalRestoreRequestKeys,
    nested: { limits: VocalSessionLimitsKeys },
  },
  rmsEnergy: { keys: RmsEnergyRequestKeys },
  roomGeometryFromEstimate: {
    keys: RoomGeometryFromEstimateRequestKeys,
    hints: { source: RoomPlacementKeys, listener: RoomPlacementKeys },
  },
  roomMorph: { keys: RoomMorphRequestKeys },
  roomMorphStereo: { keys: RoomMorphStereoRequestKeys },
  segmentAgglomerative: { keys: SegmentAgglomerativeRequestKeys },
  segmentCrossSimilarity: { keys: SegmentCrossSimilarityRequestKeys },
  segmentLagToRecurrence: { keys: SegmentLagToRecurrenceRequestKeys },
  segmentPathEnhance: { keys: SegmentPathEnhanceRequestKeys },
  segmentRecurrenceMatrix: { keys: SegmentRecurrenceMatrixRequestKeys },
  segmentRecurrenceToLag: { keys: SegmentRecurrenceToLagRequestKeys },
  segmentSubsegment: { keys: SegmentSubsegmentRequestKeys },
  shortTermLufs: { keys: ShortTermLufsRequestKeys },
  spectralBandwidth: { keys: SpectralBandwidthRequestKeys },
  spectralCentroid: { keys: SpectralCentroidRequestKeys },
  spectralContrast: { keys: SpectralContrastRequestKeys },
  spectralEdit: { keys: SpectralEditRequestKeys },
  spectralFlatness: { keys: SpectralFlatnessRequestKeys },
  spectralFlux: { keys: SpectralFluxRequestKeys },
  spectralRolloff: { keys: SpectralRolloffRequestKeys },
  splitNote: { keys: SplitNoteRequestKeys },
  splitSilence: { keys: SplitSilenceRequestKeys },
  splitSilenceCommon: { keys: SplitSilenceCommonRequestKeys },
  splitSilenceCommonWithReport: { keys: SplitSilenceCommonWithReportRequestKeys },
  stft: { keys: StftRequestKeys },
  stftDb: { keys: StftDbRequestKeys },
  streamingLoudnessGain: { keys: StreamingLoudnessGainRequestKeys },
  streamingLoudnessGainStereo: { keys: StreamingLoudnessGainStereoRequestKeys },
  synthesizeRir: { keys: SynthesizeRirRequestKeys },
  tempogram: { keys: TempogramRequestKeys },
  tempogramRatio: { keys: TempogramRatioRequestKeys },
  timeStretch: { keys: TimeStretchRequestKeys },
  tone: { keys: ToneRequestKeys },
  tonnetz: { keys: TonnetzRequestKeys },
  transcribe: { keys: TranscribeRequestKeys },
  trim: { keys: TrimRequestKeys },
  trimSilence: { keys: TrimSilenceRequestKeys },
  vectorNormalize: { keys: VectorNormalizeRequestKeys },
  voiceChange: { keys: VoiceChangeRequestKeys },
  voiceChangeRealtime: { keys: VoiceChangeRealtimeRequestKeys },
  vqt: { keys: VqtRequestKeys },
  vqtToAudio: { keys: VqtToAudioRequestKeys },
  waveformPeakPyramid: { keys: WaveformPeakPyramidRequestKeys },
  waveformPeaks: { keys: WaveformPeaksRequestKeys },
  zeroCrossingRate: { keys: ZeroCrossingRateRequestKeys },
  zeroCrossings: { keys: ZeroCrossingsRequestKeys },
} as const satisfies Readonly<Record<string, RequestShape>>;

/**
 * Declared `processorOptions` keys of each AudioWorklet processor, the option
 * keys of the main-thread factory that builds its node, and the keys of the
 * processor's own constructor, which adds the port or transport a structured
 * clone cannot carry. Checked by `assertKnownProcessorOptions`.
 */
export const PROCESSOR_OPTION_KEYS = {
  engineProcessor: keysOf<SonareRealtimeEngineWorkletProcessorOptions>()([
    'wasmBinary',
    'initialSyncMessages',
    'initialCommands',
    'sampleRate',
    'maxBlockSize',
    'numChannels',
    'meterIntervalFrames',
    'commandSharedBuffer',
    'commandRingCapacity',
    'telemetrySharedBuffer',
    'telemetryRingCapacity',
    'meterSharedBuffer',
    'meterRingCapacity',
    'scopeIntervalFrames',
    'scopeBands',
    'scopeSharedBuffer',
    'scopeRingCapacity',
    'clipPageRequestSharedBuffer',
    'clipPageRequestRingCapacity',
    'externalMidiSharedBuffer',
    'externalMidiRingCapacity',
    'cueOutput',
  ]),
  engineProcessorConstructor: keysOf<SonareRealtimeEngineWorkletProcessorConstructorOptions>()([
    'wasmBinary',
    'initialSyncMessages',
    'initialCommands',
    'sampleRate',
    'maxBlockSize',
    'numChannels',
    'meterIntervalFrames',
    'commandSharedBuffer',
    'commandRingCapacity',
    'telemetrySharedBuffer',
    'telemetryRingCapacity',
    'meterSharedBuffer',
    'meterRingCapacity',
    'scopeIntervalFrames',
    'scopeBands',
    'scopeSharedBuffer',
    'scopeRingCapacity',
    'clipPageRequestSharedBuffer',
    'clipPageRequestRingCapacity',
    'externalMidiSharedBuffer',
    'externalMidiRingCapacity',
    'cueOutput',
    'transport',
  ]),
  engineNode: keysOf<SonareRealtimeEngineNodeOptions>()([
    'wasmBinary',
    'initialSyncMessages',
    'initialCommands',
    'sampleRate',
    'maxBlockSize',
    'numChannels',
    'meterIntervalFrames',
    'commandSharedBuffer',
    'commandRingCapacity',
    'telemetrySharedBuffer',
    'telemetryRingCapacity',
    'meterSharedBuffer',
    'meterRingCapacity',
    'scopeIntervalFrames',
    'scopeBands',
    'scopeSharedBuffer',
    'scopeRingCapacity',
    'clipPageRequestSharedBuffer',
    'clipPageRequestRingCapacity',
    'externalMidiSharedBuffer',
    'externalMidiRingCapacity',
    'cueOutput',
    'processorName',
    'moduleUrl',
    'mode',
    'engineAbiVersion',
    'expectedEngineAbiVersion',
    'requireAbiCompatible',
    'nodeFactory',
  ]),
  mixerProcessor: keysOf<SonareWorkletProcessorOptions>()([
    'wasmBinary',
    'sceneJson',
    'sampleRate',
    'maxBlockSize',
    'stripCount',
    'meterIntervalFrames',
    'meterSharedBuffer',
    'meterRingCapacity',
    'spectrumIntervalFrames',
    'spectrumBands',
    'spectrumSharedBuffer',
    'spectrumRingCapacity',
  ]),
  mixerProcessorConstructor: keysOf<SonareWorkletProcessorConstructorOptions>()([
    'wasmBinary',
    'sceneJson',
    'sampleRate',
    'maxBlockSize',
    'stripCount',
    'meterIntervalFrames',
    'meterSharedBuffer',
    'meterRingCapacity',
    'spectrumIntervalFrames',
    'spectrumBands',
    'spectrumSharedBuffer',
    'spectrumRingCapacity',
    'transport',
  ]),
  voiceChangerProcessor: keysOf<SonareRealtimeVoiceChangerWorkletProcessorOptions>()([
    'config',
    'sampleRate',
    'maxBlockSize',
    'numChannels',
    'wasmBinary',
  ]),
  playbackProcessor: keysOf<SonarePlaybackWorkletProcessorOptions>()([
    'config',
    'hrtf',
    'maxBlockSize',
    'sampleRate',
    'wasmBinary',
  ]),
  playbackProcessorConstructor: keysOf<SonarePlaybackWorkletProcessorConstructorOptions>()([
    'config',
    'hrtf',
    'maxBlockSize',
    'sampleRate',
    'wasmBinary',
    'port',
  ]),
  playbackNode: keysOf<SonarePlaybackNodeOptions>()([
    'config',
    'hrtf',
    'maxBlockSize',
    'sampleRate',
    'wasmBinary',
    'processorName',
    'moduleUrl',
    'nodeFactory',
  ]),
  streamAnalyzerProcessor: keysOf<SonareStreamAnalyzerWorkletProcessorOptions>()(['chunkFrames']),
  streamAnalyzerProcessorConstructor:
    keysOf<SonareStreamAnalyzerWorkletProcessorConstructorOptions>()(['chunkFrames', 'port']),
  streamAnalyzerNode: keysOf<SonareStreamAnalyzerNodeOptions>()([
    'chunkFrames',
    'config',
    'processorName',
    'moduleUrl',
    'nodeFactory',
  ]),
} as const;

/** Name of an entry point that declares a request shape. */
export type RequestFn = keyof typeof REQUEST_SHAPES;
