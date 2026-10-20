/**
 * Declared keys of every request-object entry point, checked against the request
 * types at compile time. `requestObject` refuses any other key by name.
 */
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
  PitchCorrectToMidiTimevaryingRequest,
  PitchShiftRequest,
  TimeStretchRequest,
} from './effects_timepitch.js';
import type { VoiceChangeRealtimeRequest, VoiceChangeRequest } from './effects_voice_change.js';
import type {
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
  SplitSilenceCommonRequest,
  TempogramRequest,
  ToneRequest,
  TonnetzRequest,
  VectorNormalizeRequest,
} from './feature_core.js';
import type {
  DecomposeRequest,
  DecomposeStemsLinkedRequest,
  DecomposeStemsRequest,
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
import type {
  GriffinLimRequest,
  MelResultToAudioRequest,
  MelResultToStftRequest,
  MelToAudioRequest,
  MelToStftRequest,
  MfccResultToAudioRequest,
  MfccResultToMelRequest,
  MfccToAudioRequest,
  MfccToMelRequest,
  PhaseVocoderRequest,
} from './feature_inverse.js';
import type {
  Ebur128LoudnessRangeRequest,
  LufsInterleavedRequest,
  LufsSeriesInterleavedRequest,
} from './feature_loudness.js';
import type {
  AnalyzeMelodyRequest,
  AnalyzeSectionsRequest,
  CqtRequest,
  CqtToAudioRequest,
  DetectBoundariesRequest,
  FourierTempogramRequest,
  LufsRequest,
  NnlsChromaRequest,
  OnsetEnvelopeRequest,
  OnsetStrengthMultiRequest,
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
  SpectralContrastRequest,
  SpectralFrameRequest,
  SpectralRolloffRequest,
  ZeroCrossingRateRequest,
  ZeroCrossingsRequest,
} from './feature_spectral.js';
import type {
  BassChromaSpectrogramRequest,
  ChromaSpectrogramRequest,
  MelDeltaRequest,
  MelSpectrogramRequest,
  MfccRequest,
  ReassignedSpectrogramRequest,
  SpectrogramRequest,
  TrimRequest,
} from './feature_spectrogram.js';
import type {
  MasterAudioRequest,
  MasterAudioStereoRequest,
  MasteringChainRequest,
  MasteringChainStereoRequest,
  NormalizeRequest,
  NormalizeStereoRequest,
  StreamingLoudnessGainRequest,
  StreamingLoudnessGainStereoRequest,
} from './mastering_chain.js';
import type {
  MasteringAbMatchLoudnessRequest,
  MasteringAbMatchLoudnessStereoRequest,
  MasteringAssistantParamsRequest,
  MasteringAssistantStereoParamsRequest,
  MasteringPairAnalyzeRequest,
  MasteringPairProcessRequest,
  MasteringPairProcessStereoRequest,
  MasteringProcessRequest,
  MasteringProcessStereoRequest,
  MasteringRequest,
  MasteringSamplesParamsRequest,
  MasteringStereoAnalyzeRequest,
  MasteringStereoParamsRequest,
  MasteringStreamingPreviewRequest,
  MasteringStreamingPreviewStereoRequest,
} from './mastering_core.js';
import type {
  MasteringDynamicsCompressorRequest,
  MasteringDynamicsGateRequest,
  MasteringDynamicsTransientShaperRequest,
} from './mastering_dynamics.js';
import type {
  MeteringDetectClippingRequest,
  MeteringDynamicRangeRequest,
  MeteringSamplesRequest,
  MeteringSilenceRatioRequest,
  MeteringSpectrumFrameRequest,
  MeteringSpectrumRequest,
  MeteringStereoDecimatedRequest,
  MeteringStereoRequest,
  MeteringTruePeakRequest,
  WaveformPeakPyramidRequest,
  WaveformPeaksRequest,
} from './metering.js';
import type { SuggestMixSceneRequest } from './mixing_assistant.js';
import type { MixStereoRequest } from './mixing_oneshot.js';
import type { AnalyzePolyphonicRequest } from './polyphony.js';
import type { AlignTakeToReferenceRequest } from './project_types.js';
import type { RirSynthOptions, RoomPlacement } from './public_types_acoustic.js';
import type {
  MasteringChainConfig,
  PairAnalysis,
  StereoAnalysis,
} from './public_types_mastering.js';
import type { MixAssistantOptions } from './public_types_mixing.js';
import type { ChordFunctionsInput } from './public_types_music.js';
import type { PlaybackRendererConfig, RenderPlaybackRequest } from './public_types_playback.js';
import type { AutoTuneKey } from './public_types_spectral.js';
import type {
  VocalAnalysisInput,
  VocalCreateRequest,
  VocalRestoreRequest,
  VocalSessionLimits,
} from './public_types_vocal_edit.js';
import type {
  AnalyzeBpmRequest,
  AnalyzeDynamicsRequest,
  AnalyzeImpulseResponseRequest,
  AnalyzeRhythmRequest,
  AnalyzeTimbreRequest,
  AnalyzeWithProgressRequest,
  ChordFunctionalAnalysisRequest,
  ChordFunctionsRequest,
  DetectAcousticRequest,
  DetectChordsRequest,
  DetectKeyCandidatesRequest,
  DetectKeyRequest,
  DetectOnsetsRequest,
  EstimateMeterRequest,
  EstimateRoomRequest,
  MusicAnalyzeOptions,
  MusicAnalyzeRequest,
  RoomGeometryFromEstimateRequest,
  RoomMorphRequest,
  RoomMorphStereoRequest,
  SamplesRequest,
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
const DbConversionRequestKeys = keysOf<DbConversionRequest>()(['values', 'ref', 'amin', 'topDb']);
const MusicAnalyzeRequestKeys = keysOf<MusicAnalyzeRequest>()([
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
  'chromaHighpassHz',
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
  'tuning',
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
  'compressionThreshold',
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
  'fmin',
  'fmax',
  'frameLength',
  'hopLength',
  'threshold',
  'usePyin',
  'center',
  'validate',
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
  'tonalityOff',
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
const AnalyzeWithProgressRequestKeys = keysOf<AnalyzeWithProgressRequest>()([
  'onProgress',
  'cancel',
  'options',
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
  'chromaHighpassHz',
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
  'tuning',
]);
const MusicAnalyzeOptionsKeys = keysOf<MusicAnalyzeOptions>()([
  'nFft',
  'hopLength',
  'bpmMin',
  'bpmMax',
  'startBpm',
  'useTriadsOnly',
  'useHpss',
  'chromaHighpassHz',
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
  'tuning',
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
  'referenceMidi',
  'validate',
]);
const AutoTuneKeyKeys = keysOf<AutoTuneKey>()(['root', 'mode']);
const BassChromaSpectrogramRequestKeys = keysOf<BassChromaSpectrogramRequest>()([
  'samples',
  'sampleRate',
  'hopLength',
  'nChroma',
  'validate',
]);
const ChirpRequestKeys = keysOf<ChirpRequest>()([
  'fmin',
  'fmax',
  'sampleRate',
  'duration',
  'linear',
]);
const ChordFunctionalAnalysisRequestKeys = keysOf<ChordFunctionalAnalysisRequest>()([
  'keyRoot',
  'keyMode',
  'minDuration',
  'smoothingWindow',
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
  'tuning',
  'validate',
  'samples',
  'sampleRate',
]);
const Anon19Keys = keysOf<ChordFunctionsRequest<ChordFunctionsInput>>()(['chords', 'key']);
const SpectrogramRequestKeys = keysOf<SpectrogramRequest>()([
  'samples',
  'sampleRate',
  'nFft',
  'hopLength',
  'validate',
]);
const ChromaSpectrogramRequestKeys = keysOf<ChromaSpectrogramRequest>()([
  'samples',
  'sampleRate',
  'hopLength',
  'nChroma',
  'binsPerOctave',
  'validate',
]);
const ClicksRequestKeys = keysOf<ClicksRequest>()([
  'times',
  'sampleRate',
  'length',
  'frequency',
  'clickDuration',
]);
const CqtRequestKeys = keysOf<CqtRequest>()([
  'samples',
  'sampleRate',
  'hopLength',
  'fmin',
  'nBins',
  'binsPerOctave',
  'validate',
]);
const CqtToAudioRequestKeys = keysOf<CqtToAudioRequest>()([
  'magnitude',
  'nBins',
  'nFrames',
  'sampleRate',
  'hopLength',
  'fmin',
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
  'nFeatures',
  'nFrames',
  'nComponents',
  'nIter',
  'beta',
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
const DecomposeWithInitRequestKeys = keysOf<DecomposeWithInitRequest>()([
  'init',
  's',
  'nFeatures',
  'nFrames',
  'nComponents',
  'nIter',
  'beta',
]);
const EmphasisRequestKeys = keysOf<EmphasisRequest>()(['samples', 'coef', 'zi']);
const DetectAcousticRequestKeys = keysOf<DetectAcousticRequest>()([
  'nOctaveBands',
  'nThirdOctaveSubbands',
  'minDecayDb',
  'noiseFloorMarginDb',
  'validate',
  'samples',
  'sampleRate',
]);
const SamplesRequestKeys = keysOf<SamplesRequest>()(['samples', 'sampleRate', 'validate']);
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
  'peakDistance',
  'referenceWindow',
  'useMfcc',
  'useChroma',
  'validate',
]);
const DetectChordsRequestKeys = keysOf<DetectChordsRequest>()([
  'minDuration',
  'smoothingWindow',
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
  'tuning',
  'validate',
  'samples',
  'sampleRate',
]);
const DetectKeyRequestKeys = keysOf<DetectKeyRequest>()([
  'tuning',
  'nFft',
  'hopLength',
  'useHpss',
  'loudnessWeighted',
  'highPassHz',
  'modes',
  'profile',
  'genreHint',
  'validate',
  'samples',
  'sampleRate',
]);
const DetectKeyCandidatesRequestKeys = keysOf<DetectKeyCandidatesRequest>()([
  'nFft',
  'hopLength',
  'useHpss',
  'loudnessWeighted',
  'highPassHz',
  'modes',
  'profile',
  'genreHint',
  'validate',
  'samples',
  'sampleRate',
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
const Ebur128LoudnessRangeRequestKeys = keysOf<Ebur128LoudnessRangeRequest>()([
  'samples',
  'sampleRate',
  'validate',
]);
const EstimateMeterRequestKeys = keysOf<EstimateMeterRequest>()([
  'beatTimes',
  'beatStrengths',
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
  'norm',
  'validate',
]);
const FrameSignalRequestKeys = keysOf<FrameSignalRequest>()([
  'samples',
  'frameLength',
  'hopLength',
]);
const GriffinLimRequestKeys = keysOf<GriffinLimRequest>()([
  'magnitude',
  'nBins',
  'nFrames',
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
]);
const HpssWithResidualRequestKeys = keysOf<HpssWithResidualRequest>()([
  'samples',
  'sampleRate',
  'kernelHarmonic',
  'kernelPercussive',
  'nFft',
  'hopLength',
  'hardMask',
]);
const LufsRequestKeys = keysOf<LufsRequest>()(['samples', 'sampleRate', 'validate']);
const LufsInterleavedRequestKeys = keysOf<LufsInterleavedRequest>()([
  'samples',
  'channels',
  'sampleRate',
  'validate',
]);
const LufsSeriesInterleavedRequestKeys = keysOf<LufsSeriesInterleavedRequest>()([
  'samples',
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
]);
const MasteringChainConfigKeys = keysOf<MasteringChainConfig>()([
  'repair',
  'eq',
  'dynamics',
  'saturation',
  'spectral',
  'stereo',
  'maximizer',
  'loudness',
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
const MasteringRequestKeys = keysOf<MasteringRequest>()([
  'samples',
  'sampleRate',
  'targetLufs',
  'ceilingDb',
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
const MasteringAssistantParamsRequestKeys = keysOf<MasteringAssistantParamsRequest>()([
  'samples',
  'sampleRate',
  'params',
]);
const MasteringAssistantStereoParamsRequestKeys = keysOf<MasteringAssistantStereoParamsRequest>()([
  'left',
  'right',
  'sampleRate',
  'params',
]);
const MasteringSamplesParamsRequestKeys = keysOf<MasteringSamplesParamsRequest>()([
  'samples',
  'sampleRate',
  'params',
]);
const MasteringStereoParamsRequestKeys = keysOf<MasteringStereoParamsRequest>()([
  'left',
  'right',
  'sampleRate',
  'params',
]);
const MasteringChainRequestKeys = keysOf<MasteringChainRequest>()([
  'samples',
  'sampleRate',
  'config',
  'onProgress',
  'cancel',
]);
const MasteringChainStereoRequestKeys = keysOf<MasteringChainStereoRequest>()([
  'left',
  'right',
  'sampleRate',
  'config',
  'onProgress',
  'cancel',
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
  'sidechainHpfEnabled',
  'sidechainHpfHz',
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
  'keyHpfHz',
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
const Anon74Keys = keysOf<MasteringPairAnalyzeRequest<PairAnalysis>>()([
  'analysisName',
  'source',
  'reference',
  'sampleRate',
  'referenceSampleRate',
  'params',
]);
const MasteringPairProcessRequestKeys = keysOf<MasteringPairProcessRequest>()([
  'processorName',
  'source',
  'reference',
  'sampleRate',
  'referenceSampleRate',
  'params',
]);
const MasteringPairProcessStereoRequestKeys = keysOf<MasteringPairProcessStereoRequest>()([
  'processorName',
  'sourceLeft',
  'sourceRight',
  'referenceLeft',
  'referenceRight',
  'sampleRate',
  'referenceSampleRate',
  'params',
]);
const MasteringProcessRequestKeys = keysOf<MasteringProcessRequest>()([
  'processorName',
  'samples',
  'sampleRate',
  'params',
]);
const MasteringProcessStereoRequestKeys = keysOf<MasteringProcessStereoRequest>()([
  'processorName',
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
  'clipThreshold',
  'lpcOrder',
  'iterations',
  'lpcBlend',
]);
const MasteringRepairDeclipStereoRequestKeys = keysOf<MasteringRepairDeclipStereoRequest>()([
  'left',
  'right',
  'sampleRate',
  'clipThreshold',
  'lpcOrder',
  'iterations',
  'lpcBlend',
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
  'clipThreshold',
  'lpcOrder',
  'iterations',
  'lpcBlend',
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
  'threshold',
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
    'threshold',
    'paddingSamples',
    'paddingSec',
    'mode',
    'gateLufs',
    'windowMs',
  ]);
const MasteringRepairTrimSilenceRequestKeys = keysOf<MasteringRepairTrimSilenceRequest>()([
  'samples',
  'sampleRate',
  'threshold',
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
    'threshold',
    'paddingSamples',
    'paddingSec',
    'mode',
    'gateLufs',
    'windowMs',
  ]);
const Anon105Keys = keysOf<MasteringStereoAnalyzeRequest<StereoAnalysis>>()([
  'analysisName',
  'left',
  'right',
  'sampleRate',
  'params',
]);
const MasteringStreamingPreviewRequestKeys = keysOf<MasteringStreamingPreviewRequest>()([
  'samples',
  'sampleRate',
  'platforms',
]);
const MasteringStreamingPreviewStereoRequestKeys = keysOf<MasteringStreamingPreviewStereoRequest>()(
  ['left', 'right', 'sampleRate', 'platforms'],
);
const MelToStftRequestKeys = keysOf<MelToStftRequest & MelResultToStftRequest>()([
  'power',
  'result',
  'nMels',
  'nFrames',
  'sampleRate',
  'nFft',
  'fmin',
  'fmax',
  'htk',
  'validate',
]);
const MelToAudioRequestKeys = keysOf<MelToAudioRequest & MelResultToAudioRequest>()([
  'power',
  'result',
  'nMels',
  'nFrames',
  'sampleRate',
  'nFft',
  'hopLength',
  'fmin',
  'fmax',
  'htk',
  'nIter',
  'validate',
]);
const MfccToMelRequestKeys = keysOf<MfccToMelRequest & MfccResultToMelRequest>()([
  'coefficients',
  'result',
  'nMfcc',
  'nFrames',
  'nMels',
  'lifter',
  'validate',
]);
const MfccToAudioRequestKeys = keysOf<MfccToAudioRequest & MfccResultToAudioRequest>()([
  'coefficients',
  'result',
  'nMfcc',
  'nFrames',
  'nMels',
  'lifter',
  'sampleRate',
  'nFft',
  'hopLength',
  'fmin',
  'fmax',
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
const MelDeltaRequestKeys = keysOf<MelDeltaRequest>()([
  'features',
  'nFeatures',
  'nFrames',
  'width',
  'validate',
]);
const MelSpectrogramRequestKeys = keysOf<MelSpectrogramRequest>()([
  'nMels',
  'fmin',
  'fmax',
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
const MeteringSamplesRequestKeys = keysOf<MeteringSamplesRequest>()([
  'samples',
  'sampleRate',
  'validate',
]);
const MeteringStereoRequestKeys = keysOf<MeteringStereoRequest>()([
  'left',
  'right',
  'sampleRate',
  'validate',
]);
const MeteringDetectClippingRequestKeys = keysOf<MeteringDetectClippingRequest>()([
  'samples',
  'sampleRate',
  'threshold',
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
const MeteringStereoDecimatedRequestKeys = keysOf<MeteringStereoDecimatedRequest>()([
  'maxPoints',
  'left',
  'right',
  'sampleRate',
  'validate',
]);
const MeteringSilenceRatioRequestKeys = keysOf<MeteringSilenceRatioRequest>()([
  'thresholdDb',
  'frameLength',
  'hopLength',
  'samples',
  'sampleRate',
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
  'frameOffset',
  'samples',
  'sampleRate',
  'nFft',
  'applyOctaveSmoothing',
  'octaveFraction',
  'dbRef',
  'dbAmin',
  'validate',
]);
const MeteringTruePeakRequestKeys = keysOf<MeteringTruePeakRequest>()([
  'oversampleFactor',
  'samples',
  'sampleRate',
  'validate',
]);
const MfccRequestKeys = keysOf<MfccRequest>()([
  'nMfcc',
  'lifter',
  'nMels',
  'fmin',
  'fmax',
  'htk',
  'samples',
  'sampleRate',
  'nFft',
  'hopLength',
  'validate',
]);
const MixStereoRequestKeys = keysOf<MixStereoRequest>()([
  'leftChannels',
  'rightChannels',
  'sampleRate',
  'inputTrimDb',
  'faderDb',
  'pan',
  'panMode',
  'width',
  'muted',
]);
const NnFilterRequestKeys = keysOf<NnFilterRequest>()([
  's',
  'nFeatures',
  'nFrames',
  'aggregate',
  'k',
  'width',
]);
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
const OnsetEnvelopeRequestKeys = keysOf<OnsetEnvelopeRequest>()([
  'samples',
  'sampleRate',
  'nFft',
  'hopLength',
  'nMels',
  'validate',
]);
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
  'nBins',
  'nFrames',
  'sampleRate',
  'hopLength',
  'timeConstant',
  'gain',
  'bias',
  'power',
  'eps',
  'options',
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
  'rate',
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
  'fmin',
  'fmax',
  'threshold',
]);
const PitchCorrectTimevaryingRequestKeys = keysOf<PitchCorrectTimevaryingRequest>()([
  'samples',
  'f0Hz',
  'sampleRate',
  'hopLength',
  'mode',
  'targetMidi',
  'scaleRoot',
  'scaleModeMask',
  'referenceMidi',
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
const PitchCorrectToMidiTimevaryingRequestKeys = keysOf<PitchCorrectToMidiTimevaryingRequest>()([
  'samples',
  'f0Hz',
  'targetMidi',
  'sampleRate',
  'hopLength',
  'voiced',
  'voicedProb',
  'validate',
]);
const PitchPyinRequestKeys = keysOf<PitchPyinRequest>()([
  'samples',
  'sampleRate',
  'frameLength',
  'hopLength',
  'fmin',
  'fmax',
  'threshold',
  'fillNa',
]);
const PitchShiftRequestKeys = keysOf<PitchShiftRequest>()([
  'samples',
  'sampleRate',
  'semitones',
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
  'fmin',
  'fmax',
  'threshold',
  'fillNa',
]);
const PlpRequestKeys = keysOf<PlpRequest>()([
  'onsetEnvelope',
  'sampleRate',
  'hopLength',
  'tempoMin',
  'tempoMax',
  'winLength',
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
  'validate',
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
  'channels',
  'sampleRate',
  'config',
  'hrtf',
]);
const PlaybackRendererConfigKeys = keysOf<PlaybackRendererConfig>()([
  'input',
  'target',
  'lfe_mix_db',
  'upmix',
  'dialogue_level_db',
  'loudness',
  'night_mode',
  'room',
  'head_tracking',
  'output_limiter',
]);
const ResampleRequestKeys = keysOf<ResampleRequest>()(['samples', 'srcSr', 'targetSr']);
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
  'sourceX',
  'sourceY',
  'sourceZ',
  'listenerX',
  'listenerY',
  'listenerZ',
  'ismOrder',
  'seed',
  'maxSeconds',
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
  'sourceX',
  'sourceY',
  'sourceZ',
  'listenerX',
  'listenerY',
  'listenerZ',
  'ismOrder',
  'seed',
  'maxSeconds',
  'airAbsorptionEnabled',
  'airTemperatureC',
  'airHumidityPercent',
  'validate',
]);
const SegmentAgglomerativeRequestKeys = keysOf<SegmentAgglomerativeRequest>()([
  'data',
  'rows',
  'cols',
  'k',
  'linkage',
]);
const SegmentCrossSimilarityRequestKeys = keysOf<SegmentCrossSimilarityRequest>()([
  'x',
  'xRows',
  'xCols',
  'y',
  'yRows',
  'yCols',
  'k',
  'metric',
  'mode',
]);
const SegmentLagToRecurrenceRequestKeys = keysOf<SegmentLagToRecurrenceRequest>()([
  'lag',
  'rows',
  'lags',
]);
const SegmentPathEnhanceRequestKeys = keysOf<SegmentPathEnhanceRequest>()([
  'recurrence',
  'n',
  'win',
  'maxRatio',
  'minRatio',
  'nFilters',
]);
const SegmentRecurrenceMatrixRequestKeys = keysOf<SegmentRecurrenceMatrixRequest>()([
  'data',
  'rows',
  'cols',
  'k',
  'width',
  'sym',
  'metric',
  'mode',
]);
const SegmentRecurrenceToLagRequestKeys = keysOf<SegmentRecurrenceToLagRequest>()([
  'recurrence',
  'n',
  'pad',
]);
const SegmentSubsegmentRequestKeys = keysOf<SegmentSubsegmentRequest>()([
  'data',
  'rows',
  'cols',
  'boundaries',
  'nSegments',
]);
const Anon168Keys = keysOf<SpectralFrameRequest & { p?: number }>()([
  'samples',
  'sampleRate',
  'nFft',
  'hopLength',
  'p',
]);
const SpectralFrameRequestKeys = keysOf<SpectralFrameRequest>()([
  'samples',
  'sampleRate',
  'nFft',
  'hopLength',
]);
const SpectralContrastRequestKeys = keysOf<SpectralContrastRequest>()([
  'nBands',
  'fmin',
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
  'validate',
]);
const Anon172Keys = keysOf<SpectralFrameRequest & { lag?: number }>()([
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
const SilenceRequestKeys = keysOf<SilenceRequest>()([
  'samples',
  'topDb',
  'frameLength',
  'hopLength',
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
const SuggestMixSceneRequestKeys = keysOf<SuggestMixSceneRequest>()([
  'tracks',
  'sampleRate',
  'options',
]);
const MixAssistantOptionsKeys = keysOf<MixAssistantOptions>()([
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
const RirSynthOptionsKeys = keysOf<RirSynthOptions>()([
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
  'sourceX',
  'sourceY',
  'sourceZ',
  'listenerX',
  'listenerY',
  'listenerZ',
  'ismOrder',
  'seed',
  'maxSeconds',
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
  'tempogramData',
  'winLength',
  'sampleRate',
  'hopLength',
  'factors',
  'validate',
]);
const TimeStretchRequestKeys = keysOf<TimeStretchRequest>()([
  'samples',
  'sampleRate',
  'rate',
  'nFft',
  'hopLength',
  'validate',
]);
const ToneRequestKeys = keysOf<ToneRequest>()([
  'frequency',
  'sampleRate',
  'duration',
  'phase',
  'amplitude',
]);
const TonnetzRequestKeys = keysOf<TonnetzRequest>()(['chromagram', 'nChroma', 'nFrames']);
const TranscribeRequestKeys = keysOf<TranscribeRequest>()([
  'samples',
  'sampleRate',
  'tempoBpm',
  'polyphonic',
  'referenceHz',
  'fmin',
  'fmax',
  'minNoteMs',
  'minNoteDivision',
  'segmentationThresholdCents',
  'velocityFloorDb',
  'fixedVelocity',
  'maxPolyphony',
  'minFramePeakRatio',
  'minRidgePeakRatio',
  'reattackRatio',
  'group',
  'channel',
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
  'normType',
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
  'samples',
  'sampleRate',
  'preset',
  'channels',
  'blockSize',
  'validate',
]);
const VqtRequestKeys = keysOf<VqtRequest>()([
  'gamma',
  'samples',
  'sampleRate',
  'hopLength',
  'fmin',
  'nBins',
  'binsPerOctave',
  'validate',
]);
const VqtToAudioRequestKeys = keysOf<VqtToAudioRequest>()([
  'gamma',
  'magnitude',
  'nBins',
  'nFrames',
  'sampleRate',
  'hopLength',
  'fmin',
  'binsPerOctave',
  'nIter',
  'validate',
]);
const WaveformPeakPyramidRequestKeys = keysOf<WaveformPeakPyramidRequest>()([
  'samples',
  'channels',
  'samplesPerBucketLevels',
  'validate',
]);
const WaveformPeaksRequestKeys = keysOf<WaveformPeaksRequest>()([
  'samples',
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
]);

/** Request shape of every entry point that takes a request object, by entry-point name. */
export const REQUEST_SHAPES: Readonly<Record<string, RequestShape>> = {
  alignTakeToReference: { keys: AlignTakeToReferenceRequestKeys },
  amplitudeToDb: { keys: DbConversionRequestKeys },
  analyze: { keys: MusicAnalyzeRequestKeys },
  analyzeBpm: { keys: AnalyzeBpmRequestKeys },
  analyzeDynamics: { keys: AnalyzeDynamicsRequestKeys },
  analyzeImpulseResponse: { keys: AnalyzeImpulseResponseRequestKeys },
  analyzeMelody: { keys: AnalyzeMelodyRequestKeys },
  analyzePolyphonic: { keys: AnalyzePolyphonicRequestKeys },
  analyzeRhythm: { keys: AnalyzeRhythmRequestKeys },
  analyzeSections: { keys: AnalyzeSectionsRequestKeys },
  analyzeTimbre: { keys: AnalyzeTimbreRequestKeys },
  analyzeWithProgress: {
    keys: AnalyzeWithProgressRequestKeys,
    nested: { options: MusicAnalyzeOptionsKeys },
  },
  assignNoteTargets: { keys: AssignNoteTargetsRequestKeys },
  autoTune: { keys: AutoTuneRequestKeys, hints: { key: AutoTuneKeyKeys } },
  bassChroma: { keys: BassChromaSpectrogramRequestKeys },
  chirp: { keys: ChirpRequestKeys },
  chordFunctionalAnalysis: { keys: ChordFunctionalAnalysisRequestKeys },
  chordFunctions: { keys: Anon19Keys },
  chroma: { keys: SpectrogramRequestKeys },
  chromaCens: { keys: ChromaSpectrogramRequestKeys },
  chromaCqt: { keys: ChromaSpectrogramRequestKeys },
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
  decomposeWithInit: { keys: DecomposeWithInitRequestKeys },
  deemphasis: { keys: EmphasisRequestKeys },
  detectAcoustic: { keys: DetectAcousticRequestKeys },
  detectBeats: { keys: SamplesRequestKeys },
  detectBoundaries: { keys: DetectBoundariesRequestKeys },
  detectBpm: { keys: SamplesRequestKeys },
  detectChords: { keys: DetectChordsRequestKeys },
  detectDownbeats: { keys: SamplesRequestKeys },
  detectKey: { keys: DetectKeyRequestKeys },
  detectKeyCandidates: { keys: DetectKeyCandidatesRequestKeys },
  detectOnsets: { keys: DetectOnsetsRequestKeys },
  ebur128LoudnessRange: { keys: Ebur128LoudnessRangeRequestKeys },
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
  hybridCqt: { keys: CqtRequestKeys },
  lufs: { keys: LufsRequestKeys },
  lufsInterleaved: { keys: LufsInterleavedRequestKeys },
  lufsSeriesInterleaved: { keys: LufsSeriesInterleavedRequestKeys },
  masterAudio: { keys: MasterAudioRequestKeys, hints: { overrides: MasteringChainConfigKeys } },
  masterAudioStereo: {
    keys: MasterAudioStereoRequestKeys,
    hints: { overrides: MasteringChainConfigKeys },
  },
  masterAudioStereoWithProgress: {
    keys: MasterAudioStereoRequestKeys,
    hints: { overrides: MasteringChainConfigKeys },
  },
  masterAudioWithProgress: {
    keys: MasterAudioRequestKeys,
    hints: { overrides: MasteringChainConfigKeys },
  },
  mastering: { keys: MasteringRequestKeys },
  masteringAbMatchLoudness: { keys: MasteringAbMatchLoudnessRequestKeys },
  masteringAbMatchLoudnessStereo: { keys: MasteringAbMatchLoudnessStereoRequestKeys },
  masteringAssistantSuggest: { keys: MasteringAssistantParamsRequestKeys },
  masteringAssistantSuggestChain: { keys: MasteringAssistantParamsRequestKeys },
  masteringAssistantSuggestChainStereo: { keys: MasteringAssistantStereoParamsRequestKeys },
  masteringAssistantSuggestStereo: { keys: MasteringAssistantStereoParamsRequestKeys },
  masteringAudioProfile: { keys: MasteringSamplesParamsRequestKeys },
  masteringAudioProfileStereo: { keys: MasteringStereoParamsRequestKeys },
  masteringChain: { keys: MasteringChainRequestKeys, hints: { config: MasteringChainConfigKeys } },
  masteringChainStereo: {
    keys: MasteringChainStereoRequestKeys,
    hints: { config: MasteringChainConfigKeys },
  },
  masteringChainStereoWithProgress: {
    keys: MasteringChainStereoRequestKeys,
    hints: { config: MasteringChainConfigKeys },
  },
  masteringChainWithProgress: {
    keys: MasteringChainRequestKeys,
    hints: { config: MasteringChainConfigKeys },
  },
  masteringDynamicsCompressor: { keys: MasteringDynamicsCompressorRequestKeys },
  masteringDynamicsGate: { keys: MasteringDynamicsGateRequestKeys },
  masteringDynamicsTransientShaper: { keys: MasteringDynamicsTransientShaperRequestKeys },
  masteringPairAnalyze: { keys: Anon74Keys },
  masteringPairProcess: { keys: MasteringPairProcessRequestKeys },
  masteringPairProcessStereo: { keys: MasteringPairProcessStereoRequestKeys },
  masteringProcess: { keys: MasteringProcessRequestKeys },
  masteringProcessStereo: { keys: MasteringProcessStereoRequestKeys },
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
  masteringStereoAnalyze: { keys: Anon105Keys },
  masteringStreamingPreview: { keys: MasteringStreamingPreviewRequestKeys },
  masteringStreamingPreviewStereo: { keys: MasteringStreamingPreviewStereoRequestKeys },
  melDelta: { keys: MelDeltaRequestKeys },
  melSpectrogram: { keys: MelSpectrogramRequestKeys },
  melToAudio: { keys: MelToAudioRequestKeys },
  melToStft: { keys: MelToStftRequestKeys },
  mergeNotes: { keys: MergeNotesRequestKeys },
  meteringCrestFactorDb: { keys: MeteringSamplesRequestKeys },
  meteringCrestFactorDbStereo: { keys: MeteringStereoRequestKeys },
  meteringDcOffset: { keys: MeteringSamplesRequestKeys },
  meteringDetectClipping: { keys: MeteringDetectClippingRequestKeys },
  meteringDynamicRange: { keys: MeteringDynamicRangeRequestKeys },
  meteringPeakDb: { keys: MeteringSamplesRequestKeys },
  meteringPhaseScope: { keys: MeteringStereoDecimatedRequestKeys },
  meteringPhaseScopeDecimated: { keys: MeteringStereoDecimatedRequestKeys },
  meteringRmsDb: { keys: MeteringSamplesRequestKeys },
  meteringSilenceRatio: { keys: MeteringSilenceRatioRequestKeys },
  meteringSpectrum: { keys: MeteringSpectrumRequestKeys },
  meteringSpectrumFrame: { keys: MeteringSpectrumFrameRequestKeys },
  meteringStereoCorrelation: { keys: MeteringStereoRequestKeys },
  meteringStereoWidth: { keys: MeteringStereoRequestKeys },
  meteringTruePeakDb: { keys: MeteringTruePeakRequestKeys },
  meteringVectorscope: { keys: MeteringStereoDecimatedRequestKeys },
  meteringVectorscopeDecimated: { keys: MeteringStereoDecimatedRequestKeys },
  mfcc: { keys: MfccRequestKeys },
  mfccToAudio: { keys: MfccToAudioRequestKeys },
  mfccToMel: { keys: MfccToMelRequestKeys },
  mixStereo: { keys: MixStereoRequestKeys },
  momentaryLufs: { keys: LufsRequestKeys },
  nnFilter: { keys: NnFilterRequestKeys },
  nnlsChroma: { keys: NnlsChromaRequestKeys },
  normalize: { keys: NormalizeRequestKeys },
  normalizeStereo: { keys: NormalizeStereoRequestKeys },
  noteMove: { keys: NoteMoveRequestKeys },
  noteSegments: { keys: NoteSegmentsRequestKeys },
  noteStretch: { keys: NoteStretchRequestKeys },
  noteTargetsFromSmf: { keys: NoteTargetsFromSmfRequestKeys },
  onsetBacktrack: { keys: OnsetBacktrackRequestKeys },
  onsetEnvelope: { keys: OnsetEnvelopeRequestKeys },
  onsetStrengthMulti: { keys: OnsetStrengthMultiRequestKeys },
  padCenter: { keys: PadCenterRequestKeys },
  pcen: { keys: PcenRequestKeys },
  peakPick: { keys: PeakPickRequestKeys },
  percussive: { keys: PercussiveRequestKeys },
  phaseVocoder: { keys: PhaseVocoderRequestKeys },
  piptrack: { keys: PiptrackRequestKeys },
  pitchCorrectTimevarying: { keys: PitchCorrectTimevaryingRequestKeys },
  pitchCorrectToMidi: { keys: PitchCorrectToMidiRequestKeys },
  pitchCorrectToMidiTimevarying: { keys: PitchCorrectToMidiTimevaryingRequestKeys },
  pitchPyin: { keys: PitchPyinRequestKeys },
  pitchShift: { keys: PitchShiftRequestKeys },
  pitchTuning: { keys: PitchTuningRequestKeys },
  pitchYin: { keys: PitchYinRequestKeys },
  plp: { keys: PlpRequestKeys },
  polyFeatures: { keys: PolyFeaturesRequestKeys },
  powerToDb: { keys: DbConversionRequestKeys },
  preemphasis: { keys: EmphasisRequestKeys },
  pseudoCqt: { keys: CqtRequestKeys },
  reassignedSpectrogram: { keys: ReassignedSpectrogramRequestKeys },
  remix: { keys: RemixRequestKeys },
  remixAlignedIntervals: { keys: RemixRequestKeys },
  renderNotes: { keys: RenderNotesRequestKeys },
  renderPercussiveEvents: { keys: RenderPercussiveEventsRequestKeys },
  renderPlayback: {
    keys: RenderPlaybackRequestKeys,
    nested: { config: PlaybackRendererConfigKeys },
  },
  resample: { keys: ResampleRequestKeys },
  restoreVocalEditSession: {
    keys: VocalRestoreRequestKeys,
    nested: { limits: VocalSessionLimitsKeys },
  },
  rmsEnergy: { keys: ZeroCrossingRateRequestKeys },
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
  shortTermLufs: { keys: LufsRequestKeys },
  spectralBandwidth: { keys: Anon168Keys },
  spectralCentroid: { keys: SpectralFrameRequestKeys },
  spectralContrast: { keys: SpectralContrastRequestKeys },
  spectralEdit: { keys: SpectralEditRequestKeys },
  spectralFlatness: { keys: SpectralFrameRequestKeys },
  spectralFlux: { keys: Anon172Keys },
  spectralRolloff: { keys: SpectralRolloffRequestKeys },
  splitNote: { keys: SplitNoteRequestKeys },
  splitSilence: { keys: SilenceRequestKeys },
  splitSilenceCommon: { keys: SplitSilenceCommonRequestKeys },
  splitSilenceCommonWithReport: { keys: SplitSilenceCommonRequestKeys },
  stft: { keys: SpectrogramRequestKeys },
  stftDb: { keys: SpectrogramRequestKeys },
  streamingLoudnessGain: {
    keys: StreamingLoudnessGainRequestKeys,
    hints: { config: MasteringChainConfigKeys },
  },
  streamingLoudnessGainStereo: {
    keys: StreamingLoudnessGainStereoRequestKeys,
    hints: { config: MasteringChainConfigKeys },
  },
  suggestMixScene: {
    keys: SuggestMixSceneRequestKeys,
    hints: { options: MixAssistantOptionsKeys },
  },
  suggestMixSceneJson: {
    keys: SuggestMixSceneRequestKeys,
    hints: { options: MixAssistantOptionsKeys },
  },
  synthesizeRir: { keys: RirSynthOptionsKeys },
  tempogram: { keys: TempogramRequestKeys },
  tempogramRatio: { keys: TempogramRatioRequestKeys },
  timeStretch: { keys: TimeStretchRequestKeys },
  tone: { keys: ToneRequestKeys },
  tonnetz: { keys: TonnetzRequestKeys },
  transcribe: { keys: TranscribeRequestKeys },
  trim: { keys: TrimRequestKeys },
  trimSilence: { keys: SilenceRequestKeys },
  vectorNormalize: { keys: VectorNormalizeRequestKeys },
  voiceChange: { keys: VoiceChangeRequestKeys },
  voiceChangeRealtime: { keys: VoiceChangeRealtimeRequestKeys },
  vqt: { keys: VqtRequestKeys },
  vqtToAudio: { keys: VqtToAudioRequestKeys },
  waveformPeakPyramid: { keys: WaveformPeakPyramidRequestKeys },
  waveformPeaks: { keys: WaveformPeaksRequestKeys },
  zeroCrossingRate: { keys: ZeroCrossingRateRequestKeys },
  zeroCrossings: { keys: ZeroCrossingsRequestKeys },
};

/** Option fields {@link analyzeWithProgress} reads from the request itself as well as from `options`. */
export const MUSIC_ANALYZE_OPTION_KEYS: readonly string[] = MusicAnalyzeOptionsKeys;
