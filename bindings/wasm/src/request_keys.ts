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
import type {
  RealtimeVoiceChangerRenderRequest,
  VoiceChangeRequest,
} from './effects_voice_change.js';
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
  BiquadFilterRequest,
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
  ProjectCompileOptions,
  ProjectMarkerInput,
  ProjectMidiFxBakeRequest,
  ProjectMidiFxPreviewRequest,
  ProjectMoveClipOptions,
  ProjectProgramOptions,
  ProjectSourceAudioInput,
  ProjectTranscribeRequest,
} from './project_types.js';
import type { RoomPlacement, SynthesizeRirRequest } from './public_types_acoustic.js';
import type {
  CaptureOptions,
  CapturePunch,
  ClipPageProviderOptions,
  EngineAutomationPoint,
  EngineBounceOptions,
  EngineFreezeOptions,
  EngineGraphConnection,
  EngineGraphNode,
  EngineGraphParameterBinding,
  EngineGraphSpec,
  EngineInputMonitorInput,
  EngineLoop,
  EngineMetronomeConfig,
  EngineProcessOptions,
  EngineSetParameterOptions,
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
import type { SurroundPan } from './public_types_mixing.js';
import type { ChordFunctionsInput } from './public_types_music.js';
import type {
  PlaybackLoudnessMeterOptions,
  PlaybackRendererOptions,
  RenderPlaybackRequest,
} from './public_types_playback.js';
import type { StreamingRetuneConfig } from './public_types_realtime.js';
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
  SonareRealtimeVoiceChangerNodeOptions,
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
import type { BindWebMidiOptions, WebMidiEventContextTimeOptions } from './web_midi.js';
import type { SonareEngineRenderOfflineRequest } from './worklet/engine-export.js';
import type { SonareEngineMidiTimingOptions } from './worklet/engine-midi-input.js';
import type { SonareEngineSetParamOptions } from './worklet/engine-parameter-facade.js';

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
]);
const DeemphasisRequestKeys = keysOf<DeemphasisRequest>()(['samples', 'sampleRate', 'coef', 'zi']);
const TrimSilenceRequestKeys = keysOf<TrimSilenceRequest>()([
  'samples',
  'sampleRate',
  'topDb',
  'frameLength',
  'hopLength',
]);
const SplitSilenceRequestKeys = keysOf<SplitSilenceRequest>()([
  'samples',
  'sampleRate',
  'topDb',
  'frameLength',
  'hopLength',
]);
const SplitSilenceCommonWithReportRequestKeys = keysOf<SplitSilenceCommonWithReportRequest>()([
  'signals',
  'topDb',
  'frameLength',
  'hopLength',
]);
const StftRequestKeys = keysOf<StftRequest>()(['samples', 'sampleRate', 'nFft', 'hopLength', 'db']);
const ChromaRequestKeys = keysOf<ChromaRequest>()(['samples', 'sampleRate', 'nFft', 'hopLength']);
const ChromaCensRequestKeys = keysOf<ChromaCensRequest>()([
  'samples',
  'sampleRate',
  'hopLength',
  'nChroma',
  'binsPerOctave',
]);
const ChromaCqtRequestKeys = keysOf<ChromaCqtRequest>()([
  'samples',
  'sampleRate',
  'hopLength',
  'nChroma',
  'binsPerOctave',
]);
const BassChromaRequestKeys = keysOf<BassChromaRequest>()([
  'samples',
  'sampleRate',
  'hopLength',
  'nChroma',
]);
const SpectralCentroidRequestKeys = keysOf<SpectralCentroidRequest>()([
  'samples',
  'sampleRate',
  'nFft',
  'hopLength',
]);
const SpectralFlatnessRequestKeys = keysOf<SpectralFlatnessRequest>()([
  'samples',
  'sampleRate',
  'nFft',
  'hopLength',
]);
const RmsEnergyRequestKeys = keysOf<RmsEnergyRequest>()([
  'samples',
  'sampleRate',
  'frameLength',
  'hopLength',
]);
const DetectBpmRequestKeys = keysOf<DetectBpmRequest>()([
  'samples',
  'sampleRate',
  'bpmMin',
  'bpmMax',
]);
const DetectBeatsRequestKeys = keysOf<DetectBeatsRequest>()([
  'samples',
  'sampleRate',
  'bpmMin',
  'bpmMax',
]);
const DetectDownbeatsRequestKeys = keysOf<DetectDownbeatsRequest>()(['samples', 'sampleRate']);
const MomentaryLufsRequestKeys = keysOf<MomentaryLufsRequest>()(['samples', 'sampleRate']);
const ShortTermLufsRequestKeys = keysOf<ShortTermLufsRequest>()(['samples', 'sampleRate']);
const OnsetStrengthRequestKeys = keysOf<OnsetStrengthRequest>()([
  'nFft',
  'hopLength',
  'nMels',
  'samples',
  'sampleRate',
]);
const PseudoCqtRequestKeys = keysOf<PseudoCqtRequest>()([
  'hopLength',
  'fminHz',
  'nBins',
  'binsPerOctave',
  'samples',
  'sampleRate',
  'filterScale',
]);
const HybridCqtRequestKeys = keysOf<HybridCqtRequest>()([
  'hopLength',
  'fminHz',
  'nBins',
  'binsPerOctave',
  'samples',
  'sampleRate',
  'filterScale',
]);
const AnalyzeRequestKeys = keysOf<AnalyzeRequest>()([
  'samples',
  'sampleRate',
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
  'samples',
  'sampleRate',
]);
const AnalyzeDynamicsRequestKeys = keysOf<AnalyzeDynamicsRequest>()([
  'windowSec',
  'hopLength',
  'compressionThresholdDb',
  'samples',
  'sampleRate',
]);
const AnalyzeImpulseResponseRequestKeys = keysOf<AnalyzeImpulseResponseRequest>()([
  'nOctaveBands',
  'minDecayDb',
  'samples',
  'sampleRate',
]);
const AnalyzeMelodyRequestKeys = keysOf<AnalyzeMelodyRequest>()([
  'samples',
  'sampleRate',
  'frameLength',
  'hopLength',
  'threshold',
  'usePyin',
  'center',
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
]);
const AnalyzeRhythmRequestKeys = keysOf<AnalyzeRhythmRequest>()([
  'bpmMin',
  'bpmMax',
  'startBpm',
  'nFft',
  'hopLength',
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
]);
const AnalyzeTimbreRequestKeys = keysOf<AnalyzeTimbreRequest>()([
  'nFft',
  'hopLength',
  'nMels',
  'nMfcc',
  'windowSec',
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
  'retuneAmount',
  'retuneSpeedMs',
  'vibratoThresholdCents',
  'maxCorrectionSemitones',
  'scaleReferenceMidi',
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
  'filterScale',
]);
const CqtToAudioRequestKeys = keysOf<CqtToAudioRequest>()([
  'magnitude',
  'sampleRate',
  'hopLength',
  'fminHz',
  'binsPerOctave',
  'nIter',
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
]);
const DetectAcousticRequestKeys = keysOf<DetectAcousticRequest>()([
  'nOctaveBands',
  'nThirdOctaveSubbands',
  'minDecayDb',
  'noiseFloorMarginDb',
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
  'samples',
  'sampleRate',
  'chromaHighPassHz',
]);
const DetectOnsetsRequestKeys = keysOf<DetectOnsetsRequest>()([
  'samples',
  'sampleRate',
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
]);
const EstimateTuningRequestKeys = keysOf<EstimateTuningRequest>()([
  'resolution',
  'binsPerOctave',
  'samples',
  'sampleRate',
  'nFft',
  'hopLength',
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
]);
const FixFramesRequestKeys = keysOf<FixFramesRequest>()(['frames', 'xMin', 'xMax', 'pad']);
const FixLengthRequestKeys = keysOf<FixLengthRequest>()(['values', 'targetSize', 'padValue']);
const FourierTempogramRequestKeys = keysOf<FourierTempogramRequest>()([
  'onsetEnvelope',
  'sampleRate',
  'hopLength',
  'winLength',
  'center',
]);
const FrameSignalRequestKeys = keysOf<FrameSignalRequest>()([
  'samples',
  'frameLength',
  'hopLength',
  'sampleRate',
]);
const GriffinLimRequestKeys = keysOf<GriffinLimRequest>()([
  'magnitude',
  'sampleRate',
  'nFft',
  'hopLength',
  'nIter',
  'momentum',
]);
const HarmonicRequestKeys = keysOf<HarmonicRequest>()(['samples', 'sampleRate']);
const HpssRequestKeys = keysOf<HpssRequest>()([
  'samples',
  'sampleRate',
  'kernelHarmonic',
  'kernelPercussive',
  'nFft',
  'hopLength',
  'hardMask',
  'withResidual',
]);
const LufsRequestKeys = keysOf<LufsRequest>()(['samples', 'sampleRate']);
const LufsMultichannelRequestKeys = keysOf<LufsMultichannelRequest>()(['channels', 'sampleRate']);
const LufsSeriesMultichannelRequestKeys = keysOf<LufsSeriesMultichannelRequest>()([
  'channels',
  'sampleRate',
]);
const MasterAudioRequestKeys = keysOf<MasterAudioRequest>()([
  'samples',
  'sampleRate',
  'preset',
  'overrides',
  'onProgress',
  'cancel',
]);
const MasterAudioStereoRequestKeys = keysOf<MasterAudioStereoRequest>()([
  'left',
  'right',
  'sampleRate',
  'preset',
  'overrides',
  'onProgress',
  'cancel',
]);
const MasteringNormalizeLoudnessRequestKeys = keysOf<MasteringNormalizeLoudnessRequest>()([
  'samples',
  'sampleRate',
  'targetLufs',
  'ceilingDbtp',
  'truePeakOversample',
  'releaseMs',
  'applyGainAtInputRate',
]);
const MasteringAbMatchLoudnessRequestKeys = keysOf<MasteringAbMatchLoudnessRequest>()([
  'source',
  'reference',
  'sampleRate',
  'referenceSampleRate',
]);
const MasteringAbMatchLoudnessStereoRequestKeys = keysOf<MasteringAbMatchLoudnessStereoRequest>()([
  'sourceLeft',
  'sourceRight',
  'referenceLeft',
  'referenceRight',
  'sampleRate',
  'referenceSampleRate',
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
  ],
);
const MasteringAudioProfileRequestKeys = keysOf<MasteringAudioProfileRequest>()([
  'samples',
  'sampleRate',
  'nFft',
  'hopLength',
  'truePeakOversample',
  'detectDefects',
]);
const MasteringAudioProfileStereoRequestKeys = keysOf<MasteringAudioProfileStereoRequest>()([
  'left',
  'right',
  'sampleRate',
  'nFft',
  'hopLength',
  'truePeakOversample',
  'detectDefects',
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
  ]);
const MasteringAnalyzePairRequestKeys = keysOf<MasteringAnalyzePairRequest>()([
  'analysis',
  'source',
  'reference',
  'sampleRate',
  'referenceSampleRate',
  'params',
]);
const MasteringApplyPairProcessorRequestKeys = keysOf<MasteringApplyPairProcessorRequest>()([
  'processor',
  'source',
  'reference',
  'sampleRate',
  'referenceSampleRate',
  'params',
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
  ]);
const MasteringApplyProcessorRequestKeys = keysOf<MasteringApplyProcessorRequest>()([
  'processor',
  'samples',
  'sampleRate',
  'params',
]);
const MasteringApplyProcessorStereoRequestKeys = keysOf<MasteringApplyProcessorStereoRequest>()([
  'processor',
  'left',
  'right',
  'sampleRate',
  'params',
]);
const MasteringRepairAnalyzeRequestKeys = keysOf<MasteringRepairAnalyzeRequest>()([
  'channels',
  'sampleRate',
  'preferStreamingSafe',
]);
const MasteringRepairApplyRequestKeys = keysOf<MasteringRepairApplyRequest>()([
  'channels',
  'sampleRate',
  'stages',
  'onProgress',
  'cancel',
]);
const MasteringRepairDeclickRequestKeys = keysOf<MasteringRepairDeclickRequest>()([
  'samples',
  'sampleRate',
  'threshold',
  'neighborRatio',
  'maxClickSamples',
  'lpcOrder',
  'residualRatio',
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
]);
const MasteringRepairDeclipRequestKeys = keysOf<MasteringRepairDeclipRequest>()([
  'samples',
  'sampleRate',
  'clipThresholdDb',
  'lpcOrder',
  'iterations',
]);
const MasteringRepairDeclipStereoRequestKeys = keysOf<MasteringRepairDeclipStereoRequest>()([
  'left',
  'right',
  'sampleRate',
  'clipThresholdDb',
  'lpcOrder',
  'iterations',
  'linkChannels',
]);
const MasteringRepairDecrackleRequestKeys = keysOf<MasteringRepairDecrackleRequest>()([
  'samples',
  'sampleRate',
  'threshold',
  'mode',
  'levels',
]);
const MasteringRepairDecrackleStereoRequestKeys = keysOf<MasteringRepairDecrackleStereoRequest>()([
  'left',
  'right',
  'sampleRate',
  'threshold',
  'mode',
  'levels',
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
  ]);
const MasteringRepairDetectClicksRequestKeys = keysOf<MasteringRepairDetectClicksRequest>()([
  'samples',
  'sampleRate',
  'threshold',
  'neighborRatio',
  'maxClickSamples',
  'lpcOrder',
  'residualRatio',
]);
const MasteringRepairDetectClippingRequestKeys = keysOf<MasteringRepairDetectClippingRequest>()([
  'samples',
  'sampleRate',
  'clipThresholdDb',
  'lpcOrder',
  'iterations',
]);
const MasteringRepairDetectCrackleRequestKeys = keysOf<MasteringRepairDetectCrackleRequest>()([
  'samples',
  'sampleRate',
  'threshold',
  'mode',
  'levels',
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
  ]);
const MasteringAnalyzeStereoRequestKeys = keysOf<MasteringAnalyzeStereoRequest>()([
  'analysis',
  'left',
  'right',
  'sampleRate',
  'params',
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
]);
const MasteringStreamingPreviewStereoRequestKeys = keysOf<MasteringStreamingPreviewStereoRequest>()(
  ['left', 'right', 'sampleRate', 'platforms'],
);
const MelToStftRequestKeys = keysOf<MelToStftRequest>()([
  'power',
  'sampleRate',
  'nFft',
  'fminHz',
  'fmaxHz',
  'htk',
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
]);
const MfccToMelRequestKeys = keysOf<MfccToMelRequest>()(['coefficients', 'nMels', 'lifter']);
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
const MelDeltaRequestKeys = keysOf<MelDeltaRequest>()(['features', 'width']);
const BindWebMidiRequestKeys = keysOf<BindWebMidiOptions>()([
  'destinationId',
  'group',
  'inputIds',
  'sysex',
  'software',
  'ccBindings',
  'timestampToOffsetInBlockSamples',
  'context',
  'scheduleAheadSec',
  'onInputsChanged',
  'filter',
]);
const WebMidiEventContextTimeRequestKeys = keysOf<WebMidiEventContextTimeOptions>()([
  'scheduleAheadSec',
]);
const MelFilterbankRequestKeys = keysOf<MelFilterbankRequest>()([
  'sampleRate',
  'nFft',
  'nMels',
  'fminHz',
  'fmaxHz',
  'htk',
  'norm',
]);
const MelFrequenciesRequestKeys = keysOf<MelFrequenciesRequest>()([
  'nMels',
  'fminHz',
  'fmaxHz',
  'htk',
]);
const MelSpectrogramRequestKeys = keysOf<MelSpectrogramRequest>()([
  'nMels',
  'fminHz',
  'fmaxHz',
  'htk',
  'samples',
  'sampleRate',
  'nFft',
  'hopLength',
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
]);
const MeteringPeakDbRequestKeys = keysOf<MeteringPeakDbRequest>()(['samples', 'sampleRate']);
const MeteringRmsDbRequestKeys = keysOf<MeteringRmsDbRequest>()(['samples', 'sampleRate']);
const MeteringCrestFactorDbRequestKeys = keysOf<MeteringCrestFactorDbRequest>()([
  'samples',
  'sampleRate',
]);
const MeteringDcOffsetRequestKeys = keysOf<MeteringDcOffsetRequest>()(['samples', 'sampleRate']);
const MeteringCrestFactorDbStereoRequestKeys = keysOf<MeteringCrestFactorDbStereoRequest>()([
  'left',
  'right',
  'sampleRate',
]);
const MeteringStereoCorrelationRequestKeys = keysOf<MeteringStereoCorrelationRequest>()([
  'left',
  'right',
  'sampleRate',
]);
const MeteringStereoWidthRequestKeys = keysOf<MeteringStereoWidthRequest>()([
  'left',
  'right',
  'sampleRate',
]);
const MeteringDetectClippingRequestKeys = keysOf<MeteringDetectClippingRequest>()([
  'samples',
  'sampleRate',
  'thresholdDb',
  'minRegionSamples',
]);
const MeteringDynamicRangeRequestKeys = keysOf<MeteringDynamicRangeRequest>()([
  'samples',
  'sampleRate',
  'windowSec',
  'hopSec',
  'lowPercentile',
  'highPercentile',
]);
const MeteringVectorscopeRequestKeys = keysOf<MeteringVectorscopeRequest>()([
  'left',
  'right',
  'sampleRate',
  'maxPoints',
]);
const MeteringPhaseScopeRequestKeys = keysOf<MeteringPhaseScopeRequest>()([
  'left',
  'right',
  'sampleRate',
  'maxPoints',
]);
const MeteringSilenceRatioRequestKeys = keysOf<MeteringSilenceRatioRequest>()([
  'samples',
  'sampleRate',
  'thresholdDb',
  'frameLength',
  'hopLength',
]);
const MeteringSpectrumRequestKeys = keysOf<MeteringSpectrumRequest>()([
  'samples',
  'sampleRate',
  'nFft',
  'applyOctaveSmoothing',
  'octaveFraction',
  'dbRef',
  'dbAmin',
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
]);
const MeteringTruePeakDbtpRequestKeys = keysOf<MeteringTruePeakDbtpRequest>()([
  'samples',
  'sampleRate',
  'oversampleFactor',
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
]);
const MixStereoRequestKeys = keysOf<MixStereoRequest>()(['strips', 'sampleRate']);
const NnFilterRequestKeys = keysOf<NnFilterRequest>()(['s', 'aggregate', 'k', 'width']);
const NnlsChromaRequestKeys = keysOf<NnlsChromaRequest>()([
  'samples',
  'sampleRate',
  'enableStftBlend',
  'stftBlendWeight',
  'stftBlendNFft',
  'hopLength',
]);
const NormalizeRequestKeys = keysOf<NormalizeRequest>()([
  'samples',
  'sampleRate',
  'targetDb',
  'mode',
]);
const NormalizeStereoRequestKeys = keysOf<NormalizeStereoRequest>()([
  'left',
  'right',
  'sampleRate',
  'targetDb',
  'mode',
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
const PercussiveRequestKeys = keysOf<PercussiveRequest>()(['samples', 'sampleRate']);
const PhaseVocoderRequestKeys = keysOf<PhaseVocoderRequest>()([
  'speedRatio',
  'samples',
  'sampleRate',
  'nFft',
  'hopLength',
]);
const PiptrackRequestKeys = keysOf<PiptrackRequest>()([
  'samples',
  'sampleRate',
  'nFft',
  'hopLength',
  'fminHz',
  'fmaxHz',
  'threshold',
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
]);
const PitchCorrectToMidiRequestKeys = keysOf<PitchCorrectToMidiRequest>()([
  'samples',
  'sampleRate',
  'currentMidi',
  'targetMidi',
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
]);
const PitchShiftRequestKeys = keysOf<PitchShiftRequest>()([
  'samples',
  'sampleRate',
  'pitchSemitones',
  'nFft',
  'hopLength',
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
]);
const ReassignedSpectrogramRequestKeys = keysOf<ReassignedSpectrogramRequest>()([
  'refPower',
  'fillNan',
  'samples',
  'sampleRate',
  'nFft',
  'hopLength',
]);
const RemixRequestKeys = keysOf<RemixRequest>()([
  'samples',
  'intervals',
  'sampleRate',
  'alignZeros',
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
]);
const RenderPlaybackRequestKeys = keysOf<RenderPlaybackRequest>()([
  'samples',
  'numChannels',
  'sampleRate',
  'config',
  'hrtf',
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
]);
const ZeroCrossingRateRequestKeys = keysOf<ZeroCrossingRateRequest>()([
  'samples',
  'sampleRate',
  'frameLength',
  'hopLength',
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
]);
const SpectralContrastRequestKeys = keysOf<SpectralContrastRequest>()([
  'nBands',
  'fminHz',
  'quantile',
  'samples',
  'sampleRate',
  'nFft',
  'hopLength',
]);
const SpectralEditRequestKeys = keysOf<SpectralEditRequest>()([
  'samples',
  'sampleRate',
  'ops',
  'nFft',
  'hopLength',
  'window',
  'healRadiusFrames',
]);
const SpectralFluxRequestKeys = keysOf<SpectralFluxRequest>()([
  'samples',
  'sampleRate',
  'nFft',
  'hopLength',
  'lag',
]);
const SpectralRolloffRequestKeys = keysOf<SpectralRolloffRequest>()([
  'rollPercent',
  'samples',
  'sampleRate',
  'nFft',
  'hopLength',
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
]);
const StreamingLoudnessGainStereoRequestKeys = keysOf<StreamingLoudnessGainStereoRequest>()([
  'left',
  'right',
  'sampleRate',
  'config',
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
]);
const TimeStretchRequestKeys = keysOf<TimeStretchRequest>()([
  'samples',
  'sampleRate',
  'speedRatio',
  'nFft',
  'hopLength',
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
]);
const FadeRequestKeys = keysOf<FadeRequest>()(['samples', 'sampleRate', 'fadeInSec', 'fadeOutSec']);
const BiquadFilterRequestKeys = keysOf<BiquadFilterRequest>()([
  'samples',
  'sampleRate',
  'type',
  'order',
  'zeroPhase',
  'cutoffHz',
  'centerHz',
  'bandwidthHz',
]);
const TrimRequestKeys = keysOf<TrimRequest>()([
  'samples',
  'sampleRate',
  'thresholdDb',
  'frameLength',
  'hopLength',
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
]);
const RealtimeVoiceChangerRenderRequestKeys = keysOf<RealtimeVoiceChangerRenderRequest>()([
  'channels',
  'sampleRate',
  'config',
]);
const VqtRequestKeys = keysOf<VqtRequest>()([
  'gamma',
  'samples',
  'sampleRate',
  'hopLength',
  'fminHz',
  'nBins',
  'binsPerOctave',
  'filterScale',
]);
const VqtToAudioRequestKeys = keysOf<VqtToAudioRequest>()([
  'gamma',
  'magnitude',
  'sampleRate',
  'hopLength',
  'fminHz',
  'binsPerOctave',
  'nIter',
]);
const WaveformPeakPyramidRequestKeys = keysOf<WaveformPeakPyramidRequest>()([
  'channels',
  'samplesPerBucketLevels',
]);
const WaveformPeaksRequestKeys = keysOf<WaveformPeaksRequest>()(['channels', 'samplesPerBucket']);
const ZeroCrossingsRequestKeys = keysOf<ZeroCrossingsRequest>()([
  'samples',
  'threshold',
  'refMagnitude',
  'pad',
  'zeroPos',
  'sampleRate',
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
]);
const StreamingRetuneConfigKeys = keysOf<StreamingRetuneConfig>()([
  'pitchSemitones',
  'mix',
  'grainSizeSamples',
]);
const EngineConfigRequestKeys = keysOf<RealtimeEngineConfig>()([
  'sampleRate',
  'maxBlockSize',
  'commandCapacity',
  'telemetryCapacity',
  'maxChannels',
]);
const SidechainSourceRequestKeys = keysOf<SidechainSource>()(['kind', 'id']);
const SetGraphRequestKeys = keysOf<EngineGraphSpec>()([
  'nodes',
  'connections',
  'inputNode',
  'outputNode',
  'numChannels',
  'parameterBindings',
]);
const EngineGraphNodeKeys = keysOf<EngineGraphNode>()(['id', 'type', 'gainDb', 'numPorts']);
const EngineGraphConnectionKeys = keysOf<EngineGraphConnection>()([
  'sourceNode',
  'sourcePort',
  'destNode',
  'destPort',
  'mix',
]);
const EngineGraphParameterBindingKeys = keysOf<EngineGraphParameterBinding>()([
  'paramId',
  'nodeId',
]);
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
  'punch',
]);
const CapturePunchKeys = keysOf<CapturePunch>()([
  'startPpq',
  'startSample',
  'startSec',
  'endPpq',
  'endSample',
  'endSec',
]);
const EngineProcessRequestKeys = keysOf<EngineProcessOptions>()(['withMonitor']);
const ArmCaptureRequestKeys = keysOf<{ armed: boolean }>()(['armed']);
const ScheduleParamRequestKeys = keysOf<EngineAutomationPoint>()(['ppq', 'value', 'curveToNext']);
const SonareEngineMidiTimingRequestKeys = keysOf<SonareEngineMidiTimingOptions>()([
  'contextTimeSec',
]);
const SonareEngineSetParamRequestKeys = keysOf<SonareEngineSetParamOptions>()([
  'smoothed',
  'renderFrame',
]);
const SonareEngineRenderOfflineRequestKeys = keysOf<SonareEngineRenderOfflineRequest>()([
  'totalFrames',
  'blockSize',
  'finalize',
]);
const BounceOfflineRequestKeys = keysOf<EngineBounceOptions>()([
  'totalFrames',
  'blockSize',
  'numChannels',
  'targetSampleRate',
  'sourceSampleRate',
  'normalizeLufs',
  'targetLufs',
  'dither',
  'ditherBits',
  'ditherSeed',
]);
const FreezeOfflineRequestKeys = keysOf<EngineFreezeOptions>()([
  'totalFrames',
  'blockSize',
  'numChannels',
  'clipId',
  'startPpq',
  'gain',
]);
const SetParameterRequestKeys = keysOf<EngineSetParameterOptions>()(['smoothed']);
const SetLoopRequestKeys = keysOf<EngineLoop>()(['startPpq', 'endPpq', 'enabled']);
const SetMetronomeRequestKeys = keysOf<EngineMetronomeConfig>()([
  'enabled',
  'beatGain',
  'accentGain',
  'clickSamples',
  'clickSec',
]);
const SetInputMonitorRequestKeys = keysOf<EngineInputMonitorInput>()(['enabled', 'gain']);
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
const SurroundPanRequestKeys = keysOf<SurroundPan>()(['azimuth', 'divergence', 'lfe']);
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
const ProjectSetProgramRequestKeys = keysOf<ProjectProgramOptions>()(['bank', 'group', 'channel']);
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
  'builtinInstruments',
  'synthInstruments',
  'sf2Instruments',
  'instruments',
]);
const ProjectCompileRequestKeys = keysOf<ProjectCompileOptions>()(['withTimeline']);
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
  StreamingRetune: { keys: StreamingRetuneConfigKeys },
  ClipPageStreamer: { keys: ClipPageStreamerOptionsKeys },
  prepare: { keys: EngineConfigRequestKeys },
  setBusSidechain: { keys: SidechainSourceRequestKeys },
  setStripSidechain: { keys: SidechainSourceRequestKeys },
  setGraph: {
    keys: SetGraphRequestKeys,
    nested: {
      nodes: EngineGraphNodeKeys,
      connections: EngineGraphConnectionKeys,
      parameterBindings: EngineGraphParameterBindingKeys,
    },
  },
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
    nested: { punch: CapturePunchKeys },
  },
  armCapture: { keys: ArmCaptureRequestKeys },
  process: { keys: EngineProcessRequestKeys },
  processPrepared: { keys: EngineProcessRequestKeys },
  scheduleParam: { keys: ScheduleParamRequestKeys },
  'SonareEngine.renderOffline': { keys: SonareEngineRenderOfflineRequestKeys },
  'SonareEngine.pushMidiChannelPressure': { keys: SonareEngineMidiTimingRequestKeys },
  'SonareEngine.pushMidiCc': { keys: SonareEngineMidiTimingRequestKeys },
  'SonareEngine.pushMidiNoteOff': { keys: SonareEngineMidiTimingRequestKeys },
  'SonareEngine.pushMidiNoteOn': { keys: SonareEngineMidiTimingRequestKeys },
  'SonareEngine.pushMidiPitchBend': { keys: SonareEngineMidiTimingRequestKeys },
  'SonareEngine.pushMidiPolyPressure': { keys: SonareEngineMidiTimingRequestKeys },
  'SonareEngine.pushMidiProgram': { keys: SonareEngineMidiTimingRequestKeys },
  'SonareEngine.pushMidiSysex': { keys: SonareEngineMidiTimingRequestKeys },
  'SonareEngine.pushMidiUmp': { keys: SonareEngineMidiTimingRequestKeys },
  'SonareEngine.pushMidiInputChannelPressure': { keys: SonareEngineMidiTimingRequestKeys },
  'SonareEngine.pushMidiInputCc': { keys: SonareEngineMidiTimingRequestKeys },
  'SonareEngine.pushMidiInputNoteOff': { keys: SonareEngineMidiTimingRequestKeys },
  'SonareEngine.pushMidiInputNoteOn': { keys: SonareEngineMidiTimingRequestKeys },
  'SonareEngine.pushMidiInputPitchBend': { keys: SonareEngineMidiTimingRequestKeys },
  'SonareEngine.pushMidiInputPolyPressure': { keys: SonareEngineMidiTimingRequestKeys },
  'SonareEngine.pushMidiInputProgram': { keys: SonareEngineMidiTimingRequestKeys },
  'SonareEngine.pushMidiInputUmp': { keys: SonareEngineMidiTimingRequestKeys },
  'SonareEngine.setParam': { keys: SonareEngineSetParamRequestKeys },
  bounceOffline: { keys: BounceOfflineRequestKeys },
  freezeOffline: { keys: FreezeOfflineRequestKeys },
  setParameter: { keys: SetParameterRequestKeys },
  setLoop: { keys: SetLoopRequestKeys },
  setMetronome: { keys: SetMetronomeRequestKeys },
  setInputMonitor: { keys: SetInputMonitorRequestKeys },
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
  setSurroundPan: { keys: SurroundPanRequestKeys },
  setStripSurroundPan: { keys: SurroundPanRequestKeys },
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
  'Project.setProgram': {
    keys: ProjectSetProgramRequestKeys,
    nested: { bank: ['msb', 'lsb'] },
  },
  'Project.setSourceAudio': { keys: ProjectSetSourceAudioRequestKeys },
  'Project.setAudioSourceMetadata': { keys: ProjectSetAudioSourceMetadataRequestKeys },
  'Project.setAssistSidecar': { keys: ProjectSetAssistSidecarRequestKeys },
  'Project.bounce': { keys: ProjectBounceRequestKeys },
  'Project.compile': { keys: ProjectCompileRequestKeys },
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
  biquadFilter: { keys: BiquadFilterRequestKeys },
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
  fade: { keys: FadeRequestKeys },
  fixFrames: { keys: FixFramesRequestKeys },
  fixLength: { keys: FixLengthRequestKeys },
  fourierTempogram: { keys: FourierTempogramRequestKeys },
  frameSignal: { keys: FrameSignalRequestKeys },
  griffinLim: { keys: GriffinLimRequestKeys },
  harmonic: { keys: HarmonicRequestKeys },
  hpss: { keys: HpssRequestKeys },
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
  melFilterbank: { keys: MelFilterbankRequestKeys },
  melFrequencies: { keys: MelFrequenciesRequestKeys },
  bindWebMidi: { keys: BindWebMidiRequestKeys },
  webMidiEventContextTime: { keys: WebMidiEventContextTimeRequestKeys },
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
  realtimeVoiceChangerRender: { keys: RealtimeVoiceChangerRenderRequestKeys },
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
  voiceChangerNode: keysOf<SonareRealtimeVoiceChangerNodeOptions>()([
    'config',
    'sampleRate',
    'maxBlockSize',
    'numChannels',
    'wasmBinary',
    'processorName',
    'moduleUrl',
    'nodeFactory',
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
