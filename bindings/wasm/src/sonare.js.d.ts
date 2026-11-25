/**
 * Hand-written type declarations describing the Emscripten/embind WASM module.
 *
 * This file is NOT generated — nothing in the build writes it. A new embind
 * registration reaches TypeScript only when someone declares it here, in the
 * same change as the binding, so a missing declaration is a lockstep omission
 * rather than a stale artifact waiting on a rebuild.
 */

import type { EncodeAudioRequest } from './audio_codec.js';
import type { ErrorReason } from './errors.js';
import type { SplitSilenceCommonWithReportResult } from './feature_core.js';
import type { StripMeteringOptions } from './mixer.js';
import type {
  AssignNoteTargetsResult,
  Capabilities,
  NoteEditInput,
  NoteObject,
  NoteObjectInput,
  NoteSetEntry,
  NoteTarget,
  NoteTargetUnmatchedPolicy,
  PercussiveEvent,
  PercussiveEventInput,
  PitchDecompositionResult,
  ProgressCallback,
  SpectralRegionOp,
  SurroundPan,
} from './public_types.js';
import type { VocalWasmExports } from './vocal_edit.js';

export interface SonareModuleOptions {
  locateFile?: (path: string, prefix: string) => string;
  wasmBinary?: ArrayBuffer | Uint8Array;
  onRuntimeInitialized?: () => void;
  print?: (text: string) => void;
  printErr?: (text: string) => void;
}

// Result types
export interface WasmKeyResult {
  root: number;
  mode: number;
  confidence: number;
  name: string;
  shortName: string;
}

/** A detected key with the tuning its chroma was built with. */
export interface WasmKeyDetectionResult extends WasmKeyResult {
  /** Cents from A440. */
  tuningCents: number;
}

export interface WasmKeyCandidateResult {
  key: WasmKeyResult;
  correlation: number;
}

// Flat POD mirror of SonareRealtimeVoiceChangerConfig. Keys are camelCase
// (the underlying C ABI / Python POD is snake_case).
export interface WasmRealtimeVoiceChangerPodConfig {
  inputGainDb: number;
  outputGainDb: number;
  wetMix: number;
  retunePitchSemitones: number;
  retuneMix: number;
  retuneGrainSizeSamples?: number;
  formantFactor: number;
  formantAmount: number;
  formantBody: number;
  formantBrightness: number;
  formantNasal: number;
  formantMode: 'relative' | 'absolute';
  eqHighPassHz: number;
  eqBodyDb: number;
  eqPresenceDb: number;
  eqAirDb: number;
  gateThresholdDb: number;
  gateAttackMs: number;
  gateReleaseMs: number;
  gateRangeDb: number;
  compressorThresholdDb: number;
  compressorRatio: number;
  compressorAttackMs: number;
  compressorReleaseMs: number;
  compressorMakeupGainDb: number;
  deesserFrequencyHz: number;
  deesserThresholdDb: number;
  deesserRatio: number;
  deesserRangeDb: number;
  reverbMix: number;
  reverbTimeMs: number;
  reverbDamping: number;
  reverbSeed: number;
  limiterCeilingDb: number;
  limiterReleaseMs: number;
  limiterEnableIspLimiter: boolean;
  limiterIspCeilingDbtp: number;
  voiceEngine: 'grain' | 'psola';
  voiceEngineF0MinHz: number;
  pitchCorrectionMode: 'off' | 'auto' | 'robot';
  pitchCorrectionAmount: number;
  pitchCorrectionSpeedMs: number;
  pitchCorrectionKeyRoot: number;
  pitchCorrectionScale: 'major' | 'naturalMinor' | 'chromatic';
  pitchCorrectionRobotOctave: number;
  harmonyVoices: number;
  harmonySteps: [number, number, number];
  harmonyLevel: number;
  harmonyLeadLevel: number;
  harmonyDetuneCents: number;
  harmonyDelayMs: number;
  vocoderEnabled: boolean;
  vocoderBands: number;
  vocoderAttackMs: number;
  vocoderReleaseMs: number;
  vocoderSibilance: number;
  vocoderCarrierSource: 'internal' | 'external';
  vocoderCarrierWave: 'saw' | 'pulse' | 'ensemble';
  vocoderEnsembleDetuneCents: number;
  vocoderLevel: number;
  characterType: 'none' | 'megaphone' | 'radio' | 'bbdChorus' | 'strobo';
  characterAmount: number;
  characterTone: number;
  reverbVariation: 'reverb' | 'echo' | 'tempoDelay' | 'dubEcho';
  reverbVariationDelayMs: number;
  reverbVariationFeedback: number;
  reverbVariationTempoBpm: number;
  reverbVariationDivision: 'quarter' | 'eighth' | 'dottedEighth' | 'sixteenth';
  midiRelease: 'hold' | 'input';
}

export interface WasmBeatResult {
  timeSec: number;
  /**
   * Onset-envelope value sampled at the beat's frame — a single raw frame, not
   * a normalized or relative salience. Meter scoring uses the measured beat
   * accents in `WasmBeatObservationsResult`.
   */
  strength: number;
}

/** Beat-level evidence behind the downbeat and meter decisions. */
export interface WasmBeatObservationsResult {
  accent: number[];
  hasAttack: boolean[];
  lowFrequencyEnergy: number[];
  subdivisionAccent: number[];
  chordChange: number[];
}

export interface WasmMeterEstimateResult {
  timeSignature: WasmTimeSignatureResult;
  downbeatPhase: number;
  searched: boolean;
  grouping: number[];
  candidateScores: number[];
  candidates: WasmTimeSignatureResult[];
  unobservedBeats: number[];
}

export interface WasmChordResult {
  root: number;
  bass: number;
  quality: number;
  startSec: number;
  endSec: number;
  durationSec: number;
  confidence: number;
  name: string;
}

export interface WasmAnalysisChordResult extends WasmChordResult {
  romanNumeral: string;
}

export interface WasmSectionResult {
  type: number;
  startSec: number;
  endSec: number;
  /** Median frame RMS in the section over the loudest section's, in `[0, 1]`. */
  energyLevel: number;
  confidence: number;
  group: number;
  name: string;
}

export interface WasmTimbreResult {
  brightness?: number;
  warmth?: number;
  density?: number;
  roughness?: number;
  complexity?: number;
}

/** Each scalar level is absent when not measured. */
export interface WasmDynamicsResult {
  dynamicRangeDb?: number;
  peakDb: number;
  rmsDb: number;
  loudnessRangeLu?: number;
  crestFactor?: number;
  isCompressed: boolean;
}

export interface WasmRhythmResult {
  syncopation?: number;
  grooveType: string;
  patternRegularity?: number;
  tempoStability?: number;
  timeSignature: WasmTimeSignatureResult;
}

export interface WasmTimeSignatureResult {
  numerator: number;
  denominator: number;
  confidence: number;
}

export interface WasmMelodyContourResult {
  pitchRangeOctaves?: number;
  pitchStability?: number;
  meanFrequencyHz?: number;
  vibratoRateHz?: number;
  pitches: WasmAnalysisMelodyPoint[];
}

/** A contour point; `frequencyHz` is absent for an unvoiced frame. */
export interface WasmAnalysisMelodyPoint {
  timeSec: number;
  frequencyHz?: number;
  confidence: number;
}

export interface WasmAnalysisResult {
  bpm?: number;
  bpmConfidence: number;
  bpmCandidates: WasmBpmHypothesis[];
  key: WasmKeyResult;
  timeSignature: WasmTimeSignatureResult;
  timeSignatureCandidates: WasmTimeSignatureResult[];
  beats: WasmBeatResult[];
  downbeats: WasmBeatResult[];
  downbeatPhase: number;
  beatObservations: WasmBeatObservationsResult;
  beatLocalBpm: number[];
  chords: WasmAnalysisChordResult[];
  sections: WasmSectionResult[];
  timbre: WasmTimbreResult;
  dynamics: WasmDynamicsResult;
  rhythm: WasmRhythmResult;
  melody: WasmMelodyContourResult;
  form: string;
  tuningCents: number;
}

export interface WasmBpmHypothesis {
  bpm: number;
  confidence: number;
  relation: 'primary' | 'half' | 'double' | 'other';
}

export interface WasmBpmCandidate {
  bpm: number;
  confidence: number;
}

export interface WasmBpmAnalysisResult {
  bpm?: number;
  confidence: number;
  candidates: WasmBpmCandidate[];
  autocorrelation: Float32Array;
  tempogram: Float32Array;
}

export interface WasmRhythmAnalysisResult {
  timeSignature: WasmTimeSignatureResult;
  syncopation?: number;
  grooveType: string;
  patternRegularity?: number;
  tempoStability?: number;
  bpm?: number;
  beatIntervals: Float32Array;
}

/** Each scalar level is absent when not measured. */
export interface WasmDynamicsAnalysisResult {
  dynamicRangeDb?: number;
  peakDb: number;
  rmsDb: number;
  crestFactor?: number;
  loudnessRangeLu?: number;
  isCompressed: boolean;
  loudnessTimesSec: Float32Array;
  loudnessRmsDb: Float32Array;
}

export interface WasmTimbreFrameResult {
  brightness?: number;
  warmth?: number;
  density?: number;
  roughness?: number;
  complexity?: number;
}

export interface WasmTimbreAnalysisResult {
  brightness?: number;
  warmth?: number;
  density?: number;
  roughness?: number;
  complexity?: number;
  spectralCentroid: Float32Array;
  spectralFlatness: Float32Array;
  spectralRolloff: Float32Array;
  timbreOverTime: WasmTimbreFrameResult[];
}

export interface WasmChordAnalysisResult {
  chords: WasmChordResult[];
  tuningCents: number;
}

export interface WasmAcousticResult {
  rt60?: number;
  edt?: number;
  c50?: number;
  c80?: number;
  d50?: number;
  rt60Bands?: Float32Array;
  edtBands?: Float32Array;
  c50Bands?: Float32Array;
  c80Bands?: Float32Array;
  confidence: number;
  isBlind: boolean;
}

export interface WasmRoomGeometryOptions {
  lengthM?: number;
  widthM?: number;
  heightM?: number;
  absorption?: number;
  materialPreset?: number;
  bandAbsorption?: Float32Array | number[];
  bandScattering?: Float32Array | number[];
  source?: { x: number; y: number; z: number };
  listener?: { x: number; y: number; z: number };
  ismOrder?: number;
  seed?: number;
  maxSec?: number;
  airAbsorptionEnabled?: boolean;
  airTemperatureC?: number;
  airHumidityPercent?: number;
}

export interface WasmRirSynthOptions extends WasmRoomGeometryOptions {
  sampleRate?: number;
  preferEyring?: boolean;
  mixingTimeMs?: number;
  crossfadeMs?: number;
}

export interface WasmRirDiagnostic {
  code: string;
  message: string;
  severity: 'info' | 'warning' | 'error';
}

export interface WasmRoomMorphResult {
  audio: Float32Array;
  sampleRate: number;
  diagnostics: WasmRirDiagnostic[];
}

export interface WasmRoomMorphStereoResult {
  left: Float32Array;
  right: Float32Array;
  sampleRate: number;
  diagnostics: WasmRirDiagnostic[];
}

export interface WasmRirResult {
  rir: Float32Array;
  sampleRate: number;
  diagnostics: WasmRirDiagnostic[];
}

export interface WasmRoomEstimateOptions {
  aspectHintLw?: number;
  aspectHintLh?: number;
  referenceAbsorption?: number;
  preferEyring?: boolean;
  nOctaveBands?: number;
  mode?: number;
  minDecayDb?: number;
  noiseFloorMarginDb?: number;
}

export interface WasmRoomEstimateResult {
  volume?: number;
  lengthM?: number;
  widthM?: number;
  heightM?: number;
  drrDb?: number;
  confidence: number;
  bandAbsorption?: Float32Array;
  rt60Bands?: Float32Array;
}

/** Every field of a dereverb config, as `masteringRepairDereverbConfigForRoom` returns it. */
export interface WasmDereverbClassicalConfig {
  threshold: number;
  attenuation: number;
  nFft: number;
  hopLength: number;
  t60Sec: number;
  lateDelayMs: number;
  overSubtraction: number;
  spectralFloor: number;
  wpeEnabled: boolean;
  wpeIterations: number;
  wpeTaps: number;
  wpeStrength: number;
}

/** What a declick analysis found in one channel, as `masteringRepairDeclickStereo` returns it. Runs, not samples. */
export interface WasmClickDetection {
  count: number;
  rejected: number;
  longestRunSamples: number;
  perSecond: number;
}

/** What a declick pass found in one channel of a stereo declick and what it did to it. */
export interface WasmDeclickReport {
  detected: WasmClickDetection;
  repairedRuns: number;
  repairedSamples: number;
  linkedRuns: number;
  lpcModelUsed: boolean;
}

export interface WasmDeclickStereoResult {
  left: Float32Array;
  right: Float32Array;
  leftReport: WasmDeclickReport;
  rightReport: WasmDeclickReport;
}

/** What a declip analysis found in one channel, as `masteringRepairDeclipStereo` returns it. */
export interface WasmClipDetection {
  sampleCount: number;
  sampleFraction: number;
  runCount: number;
  longestRunSamples: number;
  flatRunCount: number;
  longestFlatRunSamples: number;
  flatSampleCount: number;
  /** Largest run level once the two highest runs are set aside; 0 when there are none. */
  flatLevel: number;
}

/** What a declip pass found in one channel of a stereo declip and what it did to it. */
export interface WasmDeclipReport {
  detected: WasmClipDetection;
  lpcReconstructedRuns: number;
  unreconstructedRuns: number;
  repairedSamples: number;
  linkedRuns: number;
}

export interface WasmDeclipStereoResult {
  left: Float32Array;
  right: Float32Array;
  leftReport: WasmDeclipReport;
  rightReport: WasmDeclipReport;
}

/** What a decrackle analysis found in one channel, as `masteringRepairDecrackleStereo` returns it. */
export interface WasmCrackleDetection {
  sampleCount: number;
  sampleFraction: number;
  perSecond: number;
}

/** What a decrackle pass found in one channel of a stereo decrackle and what it did to it. */
export interface WasmDecrackleReport {
  detected: WasmCrackleDetection;
  replacedSamples: number;
  detailCoefficients: number;
  shrunkCoefficients: number;
  noiseSigma: number;
}

export interface WasmDecrackleStereoResult {
  left: Float32Array;
  right: Float32Array;
  leftReport: WasmDecrackleReport;
  rightReport: WasmDecrackleReport;
}

/** What a dehum analysis found in one channel, as `masteringRepairDehumStereo` returns it. */
export interface WasmHumDetection {
  fundamentalHz: number;
  fundamentalProminence: number;
  harmonics: number;
  harmonicDbfs: Float32Array;
}

/** What a dehum pass found in one channel of a stereo dehum and what it did to it. */
export interface WasmDehumReport {
  detected: WasmHumDetection;
  notchedHarmonics: number;
  appliedFundamentalHz: number;
  fundamentalDriftHz: number;
}

export interface WasmDehumStereoResult {
  left: Float32Array;
  right: Float32Array;
  leftReport: WasmDehumReport;
  rightReport: WasmDehumReport;
}

/** What a denoise analysis found, as `masteringRepairDenoiseClassicalStereo` returns it. */
export interface WasmNoiseDetection {
  floorDbfs?: number;
  bandFloorDbfs: Float32Array;
  bandFloorBand: Int32Array;
}

/** What a stereo denoise pass found and what it removed. */
export interface WasmDenoiseReport {
  detected: WasmNoiseDetection;
  meanReductionDb: number;
  maxReductionDb: number;
  floorLimitedFraction: number;
}

/** One `report`, not a per-channel pair: the gain mask is one array applied to both channels. */
export interface WasmDenoiseStereoResult {
  left: Float32Array;
  right: Float32Array;
  report: WasmDenoiseReport;
}

/** The N-channel form: one mask over the set, and one report whose `detected` is the set's. */
export interface WasmDenoiseLinkedResult {
  channels: Float32Array[];
  report: WasmDenoiseReport;
}

/** What a dereverb analysis found, as `masteringRepairDereverbClassicalStereo` returns it. */
export interface WasmReverbDetection {
  lateDecayRatioDb: number;
  latePredictability: number;
}

/** What a stereo dereverb pass found and what it removed. */
export interface WasmDereverbReport {
  detected: WasmReverbDetection;
  meanReductionDb: number;
  suppressedFraction: number;
  wpePredictorNorm: number;
}

/** One `report`, not a per-channel pair: the mask and the WPE predictor set are each one
 * object applied to both channels. */
export interface WasmDereverbStereoResult {
  left: Float32Array;
  right: Float32Array;
  report: WasmDereverbReport;
}

/** The N-channel form: one mask and one WPE predictor set over the whole channel set. */
export interface WasmDereverbLinkedResult {
  channels: Float32Array[];
  report: WasmDereverbReport;
}

/** One half-open input-buffer range, as a trim pass reports it. */
export interface WasmTrimRange {
  first: number;
  lastExclusive: number;
}

/** What a trim pass kept and what it dropped. */
export interface WasmTrimReport {
  range: WasmTrimRange;
  removedHeadSamples: number;
  removedTailSamples: number;
}

/** Arrays SHORTER than the input, and empty when neither channel carries signal. `report.range`
 * is the union applied to both channels; the two per-channel ranges are what it was formed
 * from. */
export interface WasmTrimSilenceStereoResult {
  left: Float32Array;
  right: Float32Array;
  report: WasmTrimReport;
  leftRange: WasmTrimRange;
  rightRange: WasmTrimRange;
}

export interface WasmRoomMorphOptions extends WasmRoomGeometryOptions {
  wet?: number;
  sourceTailSuppression?: number;
}

export interface WasmRoomMorphStereoOptions extends WasmRoomMorphOptions {
  /** Receiver spacing in metres, in (0, 4]; omitted = 0.5. */
  receiverSpacingM?: number;
}

export interface WasmHpssResult {
  harmonic: Float32Array;
  percussive: Float32Array;
  residual?: Float32Array;
  sampleRate: number;
}

export interface WasmAudioFromMemoryResult {
  samples: Float32Array;
  sampleRate: number;
}

/** Row-major 2-D matrix as a flat buffer plus its dimensions. */
export interface WasmMatrix2dResult {
  data: Float32Array;
  rows: number;
  cols: number;
}

export interface WasmSegmentMatrixResult {
  rows: number;
  cols: number;
  values: Float32Array;
}

export interface WasmDecomposeResult {
  w: Float32Array;
  h: Float32Array;
}

export interface WasmStftResult {
  nBins: number;
  nFrames: number;
  nFft: number;
  hopLength: number;
  sampleRate: number;
  magnitude: Float32Array;
  power: Float32Array;
  /** Present only when the call asked for it. */
  db?: Float32Array;
  /** Present only when the call asked for it. */
  phase?: Float32Array;
}

export interface WasmMelResult {
  nMels: number;
  nFrames: number;
  sampleRate: number;
  hopLength: number;
  nFft: number;
  fmin: number;
  fmax: number;
  htk: boolean;
  isDb: boolean;
  power: Float32Array;
  db: Float32Array;
}

export interface WasmMfccResult {
  nMfcc: number;
  nFrames: number;
  sampleRate: number;
  hopLength: number;
  nFft: number;
  nMels: number;
  fmin: number;
  fmax: number;
  htk: boolean;
  isDb: boolean;
  lifter: number;
  coefficients: Float32Array;
}

export interface WasmStftMagnitudeResult {
  nBins: number;
  nFrames: number;
  magnitude: Float32Array;
}

export interface WasmMelPowerResult {
  nMels: number;
  nFrames: number;
  power: Float32Array;
}

export interface WasmChromaResult {
  nChroma: number;
  nFrames: number;
  sampleRate: number;
  hopLength: number;
  features: Float32Array;
  meanEnergy: number[];
}

export interface WasmPitchResult {
  f0: Float32Array;
  voicedProb: Float32Array;
  voicedFlag: boolean[];
  nFrames: number;
  medianF0?: number;
  meanF0?: number;
}

export interface WasmNoteSegment {
  frameStart: number;
  frameEnd: number;
  startSec: number;
  endSec: number;
  medianCents: number;
}

export interface WasmTrimResult {
  audio: Float32Array;
  startSample: number;
  endSample: number;
}

export interface WasmFrameResult {
  nFrames: number;
  frames: Float32Array;
}

export interface WasmTempogramResult {
  nFrames: number;
  winLength: number;
  data: Float32Array;
}

export interface WasmCyclicTempogramResult {
  nFrames: number;
  nBins: number;
  data: Float32Array;
}

export interface WasmFourierTempogramResult {
  nBins: number;
  nFrames: number;
  data: Float32Array;
}

export interface WasmOnsetStrengthMultiResult {
  nBands: number;
  nFrames: number;
  data: Float32Array;
}

export interface WasmNnlsChromaResult {
  nChroma: number;
  nFrames: number;
  data: Float32Array;
}

export interface WasmMelodyPoint {
  time: number;
  frequency: number;
  confidence: number;
}

/**
 * Boundary options as the embind wrapper reads them. Every field is optional
 * because the wrapper seeds the bag from `BoundaryConfig`, so an omitted one
 * takes the core's own default.
 *
 * Declaring them required is what obliged the facade to spell all ten defaults
 * before the call, which made those literals the effective defaults on npm and
 * put the core's out of reach. That shape did buy one thing — a field name
 * drifting from the embind reader failed to compile instead of falling back in
 * silence — and the typed-options test carries it instead: it sends a
 * wrong-typed value for each of the ten keys and requires the module to refuse,
 * which stops happening the moment the reader stops reading one of them.
 */
export interface WasmBoundaryOptions {
  nFft?: number;
  hopLength?: number;
  kernelSize?: number;
  threshold?: number;
  absoluteThreshold?: number;
  nMfcc?: number;
  nChroma?: number;
  peakDistanceSec?: number;
  referenceWindowSec?: number;
  useMfcc?: boolean;
  useChroma?: boolean;
}

export interface WasmBoundary {
  time: number;
  frame: number;
  strength: number;
}

export interface WasmBoundaryResult {
  boundaries: WasmBoundary[];
  noveltyCurve: Float32Array;
  noveltyPeak: number;
  sampleRate: number;
  hopLength: number;
  nFrames: number;
  frameStride: number;
}

export interface WasmMelodyResult {
  points: WasmMelodyPoint[];
  pitchRangeOctaves?: number;
  pitchStability?: number;
  meanFrequency?: number;
  vibratoRate?: number;
}

export interface WasmCqtResult {
  nBins: number;
  nFrames: number;
  hopLength: number;
  sampleRate: number;
  magnitude: Float32Array;
  frequencies: Float32Array;
  gamma: number;
}

/** Each field but `integratedLufs` is absent when not measured. */
export interface WasmLufsResult {
  integratedLufs: number;
  momentaryLufs?: number;
  shortTermLufs?: number;
  maxMomentaryLufs?: number;
  maxShortTermLufs?: number;
  loudnessRangeLu?: number;
}

/**
 * The two per-block loudness series a multi-channel measurement builds, in
 * LUFS. A signal shorter than a window yields an empty series for it.
 */
export interface WasmLufsSeriesResult {
  /** 400 ms momentary series. */
  momentary: Float32Array;
  /** 3 s short-term series. */
  shortTerm: Float32Array;
}

export interface WasmMasteringResult {
  samples: Float32Array;
  sampleRate: number;
  inputLufs: number;
  outputLufs: number;
  appliedGainDb: number;
  loudnessTargetLimited?: boolean;
  latencySamples?: number;
  /**
   * Samples the named processor replaced with a finite in-domain one,
   * keeping the output finite and in range. A non-finite sample supplied by
   * the caller is rejected before the processor runs, so a replacement is
   * always of a value the processor itself produced.
   *
   * Whether this can be non-zero depends on which processor was named: one
   * that does not substitute reports zero because it has nothing to replace
   * with, not because nothing needed replacing.
   */
  nonFiniteSubstitutionCount: number;
  /** Present only when a declip stage ran. */
  declipUnreconstructedRuns?: number;
  /** Present only when a quantizing stage ran. */
  quantizeClippedSamples?: number;
}

export interface WasmStreamingLoudnessGainResult {
  loudnessStaticGainDb?: number;
  truePeakDb: number;
  integratedLufs: number;
}

export interface WasmLoudnessMatchResult {
  samples: Float32Array;
  sampleRate: number;
  referenceLufs: number;
  sourceLufs: number;
  appliedGainDb: number;
  matchedTruePeakDbtp: number;
}

export interface WasmLoudnessMatchStereoResult extends Omit<WasmLoudnessMatchResult, 'samples'> {
  left: Float32Array;
  right: Float32Array;
}

export interface WasmStageGainReduction {
  stage: string;
  gainReductionDb: number;
}

/** Each scalar is absent when not measured. */
export interface WasmMasteringLoudnessSummary {
  integratedLufs: number;
  maxMomentaryLufs?: number;
  maxShortTermLufs?: number;
  truePeakDbtp: number;
  loudnessRangeLu?: number;
}

export interface WasmMasteringReport {
  before: WasmMasteringLoudnessSummary;
  after: WasmMasteringLoudnessSummary;
  appliedGainDb: number;
  maxGainReductionDb: number;
  loudnessTargetLimited: boolean;
  bandEnergyDeltaDb: Float32Array;
  loudnessGainDb?: number;
}

export interface WasmMasteringChainResult extends WasmMasteringResult {
  stages: string[];
  outputTruePeakDbtp: number;
  outputLoudnessRangeLu?: number;
  loudnessTargetLimited: boolean;
  loudnessGainDb?: number;
  /**
   * Samples a stage replaced with a finite in-domain one, keeping the
   * output finite and in range. A non-finite sample supplied by the caller
   * is rejected before any stage runs, so a replacement is always of a
   * value a stage itself produced.
   *
   * Only the true-peak limiters replace anything, so with the maximizer's
   * limiter and the loudness stage both disabled a zero here means no
   * stage was able to replace anything rather than that nothing needed
   * replacing. Overrides the inherited {@link WasmMasteringResult} doc,
   * which describes a single named processor rather than a chain.
   */
  nonFiniteSubstitutionCount: number;
  /** Present only when a declip stage ran. */
  declipUnreconstructedRuns?: number;
  stageGainReductions: WasmStageGainReduction[];
  report: WasmMasteringReport;
}

export interface WasmMasteringStereoChainResult {
  left: Float32Array;
  right: Float32Array;
  sampleRate: number;
  inputLufs: number;
  outputLufs: number;
  appliedGainDb: number;
  stages: string[];
  outputTruePeakDbtp: number;
  outputLoudnessRangeLu?: number;
  loudnessTargetLimited: boolean;
  loudnessGainDb?: number;
  /** See {@link WasmMasteringChainResult} for field semantics. Aggregated over both channels. */
  nonFiniteSubstitutionCount: number;
  /** Present only when a declip stage ran. */
  declipUnreconstructedRuns?: number;
  stageGainReductions: WasmStageGainReduction[];
  report: WasmMasteringReport;
}

export interface WasmMasteringStereoResult {
  left: Float32Array;
  right: Float32Array;
  sampleRate: number;
  inputLufs: number;
  outputLufs: number;
  appliedGainDb: number;
  latencySamples: number;
  loudnessTargetLimited: boolean;
  /** Aggregated over both channels. */
  nonFiniteSubstitutionCount: number;
  /** Present only when a declip stage ran. */
  declipUnreconstructedRuns?: number;
  /** Present only when a quantizing stage ran. */
  quantizeClippedSamples?: number;
}

export interface WasmMixMeterSnapshot {
  peakDbL?: number;
  peakDbR?: number;
  rmsDbL?: number;
  rmsDbR?: number;
  correlation?: number;
  monoCompatWidth?: number;
  monoCompatPeak: number;
  monoCompatSideRms: number;
  likelyMonoCompatible: boolean;
  momentaryLufs?: number;
  shortTermLufs?: number;
  integratedLufs?: number;
  gainReductionDb: number;
  /**
   * Left-channel inter-sample (true) peak in dBTP, from the ITU-R BS.1770-4 polyphase reconstruction.
   * A streaming measurement that runs one reconstruction-filter group delay (a few samples) behind
   * the block, so every value has its whole stencil and the reading does not depend on the block size.
   */
  truePeakDbtpL?: number;
  /** Right-channel inter-sample (true) peak in dBTP. See {@link truePeakDbtpL}. */
  truePeakDbtpR?: number;
  /** Maximum inter-sample peak across channels in dBTP. See {@link truePeakDbtpL}. */
  maxTruePeakDbtp?: number;
  seq: number;
  numChannels: number;
  peakDb: number[];
  rmsDb: number[];
  /** Per-plane inter-sample peak in dBTP; [0]/[1] mirror {@link truePeakDbtpL}/R. */
  truePeakDbtp: number[];
}

export interface WasmMixResult {
  left: Float32Array;
  right: Float32Array;
  sampleRate: number;
  meters: WasmMixMeterSnapshot[];
}

export interface WasmEngineClip {
  id?: number;
  trackId?: number;
  channels?: Float32Array[];
  startPpq: number;
  lengthSamples?: number;
  clipOffsetSamples?: number;
  loop?: boolean;
  gain?: number;
  fadeInSamples?: number;
  fadeOutSamples?: number;
  warpMode?: 0 | 1 | 2 | 3;
  warpAnchors?: Array<{ warpSample: number; sourceSample: number }>;
  /** The provider instance that serves this clip's pages. */
  pageProvider?: WasmClipPageProvider;
}

export interface WasmEngineTrackSend {
  busId: number;
  levelDb?: number;
  enabled?: boolean;
  /** Native send tap: 0 post-fader (default), 1 pre-fader. */
  sendTiming?: number;
}

export interface WasmEngineTrackLane {
  trackId: number;
  sends?: WasmEngineTrackSend[];
  /**
   * Bus the lane's post-fader output sums into instead of the master mix
   * (group/folder routing); 0 or absent keeps the lane on the master mix.
   */
  outputBusId?: number;
  /**
   * Input channel layout of the source feeding this lane (`SonareChannelLayout`:
   * 0 mono, 1 stereo, 2 5.1, 3 7.1). Absent defaults to stereo.
   */
  sourceChannelLayout?: number;
}

export interface WasmEngineBus {
  busId: number;
  gainDb?: number;
  channelLayout?: number;
  /**
   * Bus this bus's output sums into instead of the master mix; 0 or absent
   * keeps it on the master mix.
   */
  outputBusId?: number;
  /** Sends to other buses, in the same shape as a track lane's sends. */
  sends?: WasmEngineTrackSend[];
}

/** Embind clip page store, shared by every clip whose `pageProvider` names it. */
export interface WasmClipPageProvider {
  supply: (pageIndex: number, channels: Float32Array[]) => void;
  clear: (pageIndex: number) => void;
  delete: () => void;
  isDeleted: () => boolean;
}

export interface WasmClipPageRequest {
  clipId: number;
  channel: number;
  sample: number;
}

export interface WasmEngineParameterInfo {
  id: number;
  name: string;
  /** Display unit; omit for an empty string. */
  unit?: string;
  /** Lower end of the parameter range. Default `0`. */
  minValue?: number;
  /** Upper end of the parameter range. Default `1`. */
  maxValue?: number;
  /** Value the parameter starts at. Default `0`. */
  defaultValue?: number;
  /** Whether the parameter is safe to change from the realtime thread. Default `true`. */
  rtSafe?: boolean;
  /** Curve used by automation points that omit `curveToNext`. Default `1` (exponential). */
  defaultCurve?: number;
}

export interface WasmEngineAutomationPoint {
  ppq: number;
  value: number;
  curveToNext?: number;
}

export interface WasmEngineMarker {
  id: number;
  ppq: number;
  name?: string;
  kind?: number;
  keyFifths?: number;
  keyMinor?: boolean;
}

export interface WasmProjectMarker {
  id: number;
  ppq: number;
  name?: string;
  kind?: number;
  keyFifths?: number;
  keyMinor?: boolean;
}
export interface WasmProjectTrack {
  id: number;
  kind: number;
  midiDestinationId: number;
  gain: number;
  pan: number;
  mute: boolean;
  solo: boolean;
  name: string;
  channelStripRef?: string;
  outputTarget?: string;
}
export interface WasmProjectClip {
  id: number;
  trackId: number;
  sourceId: number;
  sourceKind: number;
  startPpq: number;
  lengthPpq: number;
  sourceOffsetPpq: number;
  gain: number;
  loopMode: number;
  loopLengthPpq: number;
}
export interface WasmProjectSource {
  id: number;
  kind: number;
  numChannels: number;
  storageHandleId: number;
  sampleRateHint: number;
  nameOrUri: string;
  /** Owning content hash for audio sources; empty for MIDI sources. */
  contentHash: string;
  /** External source-separation role for audio sources; empty for MIDI sources. */
  externalStemRole: string;
}

export interface WasmEngineMetronomeConfig {
  enabled: boolean;
  beatGain?: number;
  accentGain?: number;
  /** Explicit click length in samples (0..384000). The engine caps it to one second at its sample rate. */
  clickSamples?: number;
  /** Optional click length in seconds (0..1); > 0 overrides the engine 2 ms default. */
  clickSeconds?: number;
}

export interface WasmEngineGraphNode {
  id: string;
  type?: number;
  gainDb?: number;
  numPorts?: number;
}

export interface WasmEngineGraphConnection {
  sourceNode: string;
  sourcePort: number;
  destNode: string;
  destPort: number;
  /**
   * Mixing intent: `0` replaces what the destination port already holds, `1` adds
   * to it. Omitted means add; any other value is refused with a `RangeError`.
   */
  mix?: number;
}

export interface WasmEngineGraphParameterBinding {
  paramId: number;
  nodeId: string;
}

export interface WasmEngineGraphSpec {
  nodes: WasmEngineGraphNode[];
  connections: WasmEngineGraphConnection[];
  inputNode: string;
  outputNode: string;
  numChannels: number;
  parameterBindings?: WasmEngineGraphParameterBinding[];
}

export interface WasmEngineTelemetry {
  type: number;
  error: number;
  renderFrame: number;
  timelineSample: number;
  audibleTimelineSample: number;
  graphLatencySamplesQ8: number;
  value: number;
}

export interface WasmEngineMeterTelemetry {
  /**
   * Telemetry tap target id: `0` master mix, `laneIndex + 1` (1..32) for track
   * lanes, `33 + busIndex` (33..40) for buses, and `0xFFFF` for the
   * input-monitor capture tap. Handle `0xFFFF` explicitly -- a naive
   * `targetId - 1` index into a 32-entry lane array runs past its end.
   */
  targetId: number;
  renderFrame: number;
  seq: number;
  channelCount: number;
  peakDb: number[];
  rmsDb: number[];
  /**
   * Per-plane inter-sample (true) peak in dB (length `channelCount`), from the ITU-R BS.1770-4
   * polyphase reconstruction at 4x. A streaming measurement that runs one reconstruction-filter
   * group delay (a few samples) behind the block, so every value has its whole stencil and the
   * reading does not depend on the block size. Use `meteringTruePeakDb` over the whole signal for
   * an exact dBTP number.
   */
  truePeakDb: number[];
  /** Maximum inter-sample peak across channels in dB. See {@link truePeakDb}. Absent when not measured; `-Infinity` for silence. */
  maxTruePeakDb?: number;
  /** Absent when not measured. */
  correlation?: number;
  /** Absent when not measured; `+Infinity` when the mid signal is silent but the side is not. */
  monoCompatWidth?: number;
  /** Absent until the window has filled; `-Infinity` for silence. */
  momentaryLufs?: number;
  /** Absent until the window has filled; `-Infinity` for silence. */
  shortTermLufs?: number;
  /** Absent before the first gating block; `-Infinity` while nothing passes the gate. */
  integratedLufs?: number;
  gainReductionDb: number;
  /** Pre-trim input peak for each valid plane, in dBFS. */
  inputPeakDb: number[];
  droppedRecords: number;
}

export interface WasmEngineScopeTelemetry {
  /**
   * Telemetry tap target id: `0` master mix, `laneIndex + 1` (1..32) for track
   * lanes, `33 + busIndex` (33..40) for buses, and `0xFFFF` for the
   * input-monitor capture tap. Handle `0xFFFF` explicitly -- a naive
   * `targetId - 1` index into a 32-entry lane array runs past its end.
   */
  targetId: number;
  renderFrame: number;
  seq: number;
  droppedRecords: number;
  bands: number[];
  /**
   * Goniometer/vectorscope sample points as `{ left, right }` objects. The
   * Python surface exposes the same data as `(left, right)` tuples; this
   * representation difference is intentional, the values match.
   */
  points: { left: number; right: number }[];
}

export interface WasmEngineCaptureStatus {
  capturedFrames: number;
  droppedFrames: number;
  armed: boolean;
  punchEnabled: boolean;
  source: 'output' | 'input';
  recordOffsetSamples: number;
}

export interface WasmEngineTransportState {
  playing: boolean;
  looping: boolean;
  renderFrame: number;
  samplePosition: number;
  ppq: number;
  bpm: number;
  loopStartPpq: number;
  loopEndPpq: number;
  sampleRate: number;
  /** PPQ of the current bar's downbeat (derived from the tempo map). */
  barStartPpq: number;
  /** Zero-based index of the current bar. */
  barIndex: number;
  /** Time signature in effect at the current PPQ. */
  timeSignature: { numerator: number; denominator: number; confidence: number };
  /** One-based beat within the current bar (`barIndex` is zero-based). */
  beat: number;
  /** Fractional position within the current beat, in [0, 1). */
  beatFraction: number;
}

export interface WasmEngineBounceOptions {
  /**
   * Length of the bounce in frames. The whole result is held in memory, so the
   * span is capped by a 1 GiB peak budget over the full-size float buffers a
   * bounce holds at once, and exceeding it throws a `RangeError`.
   *
   * The cap is a **frame** count: inversely proportional to `numChannels`, and
   * independent of the sample rate. Stereo refuses past 44,739,242 frames, or
   * past 33,554,432 frames when `dither` is set, which adds one more copy; 5.1
   * refuses past a third of each. Read as a **duration** those two stereo counts
   * are 15 min 32 s and 11 min 39 s at 48 kHz, and half that at 96 kHz. When
   * `sourceSampleRate` and `targetSampleRate` differ both the source and the
   * resampled length are checked, so the longer of the two is what binds.
   *
   * Render longer material in chunks via `renderOffline({ finalize: false })`.
   * The cap is a fixed budget, not a probe of the module's heap, which on wasm32
   * can run out below it.
   */
  totalFrames: number;
  blockSize?: number;
  numChannels?: number;
  /**
   * Output sample rate. Omitting it (or `0`) keeps the source rate, so nothing
   * is resampled; a different rate resamples. The result's `sampleRate`
   * reports the rate of the returned audio.
   */
  targetSampleRate?: number;
  /**
   * Rate `totalFrames` is counted in. Omitting it (or `0`) means the engine's
   * prepared rate; a different value throws a `RangeError`,
   * because the render always runs at the prepared rate.
   */
  sourceSampleRate?: number;
  normalizeLufs?: boolean;
  /**
   * Target integrated loudness when `normalizeLufs` is set. Omitting it, passing
   * `0`, and passing a non-finite value all mean the library default of
   * -14 LUFS: the non-finite case resolves rather than being reported, matching
   * the C ABI, because the loudness stage derives a per-sample gain from this
   * and checks nothing itself.
   */
  targetLufs?: number;
  dither?: 0 | 1 | 2 | 3;
  ditherBits?: number;
  ditherSeed?: number;
}

export interface WasmEngineBounceResult {
  interleaved: Float32Array;
  frames: number;
  numChannels: number;
  sampleRate: number;
  integratedLufs: number;
  /** Present only when the bounce dithered. */
  quantizeClippedSamples?: number;
}

export interface WasmEngineFreezeOptions {
  totalFrames: number;
  blockSize?: number;
  numChannels?: number;
  clipId?: number;
  startPpq?: number;
  gain?: number;
}

export interface WasmEngineFreezeResult {
  clipId: number;
  frames: number;
  numChannels: number;
}

export interface WasmEngineProcessWithMonitorResult {
  output: Float32Array[];
  monitor: Float32Array[];
}

export interface WasmEngineTempoSegment {
  startPpq: number;
  bpm: number;
  endBpm?: number;
}

export interface WasmEngineTimeSignatureSegment {
  startPpq: number;
  numerator: number;
  denominator: number;
}

/**
 * One external-MIDI event drained from the engine, already lowered to MIDI 1.0
 * bytes ready to write to a Web MIDI output port.
 */
export interface WasmExternalMidiEvent {
  /**
   * Originating track lane (Track.midi_destination_id), or the transport
   * sentinel 0xFFFFFFFF for clock / start / continue / stop bytes meant for
   * every external port.
   */
  destinationId: number;
  /** Absolute engine device frame at which the event fires, never an offset within a block. */
  renderFrame: number;
  /** MIDI 1.0 status + data bytes (1..3 entries). */
  bytes: number[];
}

export interface WasmRealtimeEngine {
  prepare: (
    sampleRate: number,
    maxBlockSize: number | undefined,
    commandCapacity: number,
    telemetryCapacity: number,
    maxChannels: number | undefined,
  ) => void;
  /** `smoothed` true queues a ramp; undefined queues a step. */
  setParameter: (
    paramId: number,
    value: number,
    renderFrame: number | undefined,
    smoothed: boolean | undefined,
  ) => void;
  setParamSmoothingMs: (smoothingMs: number) => void;
  setSoloMute: (laneIndex: number, solo: boolean, mute: boolean, renderFrame?: number) => void;
  setTrackMonitorMode: (laneIndex: number, mode: number, renderFrame?: number) => void;
  getTransportState: () => WasmEngineTransportState;
  renderFrame: () => number;
  play: (renderFrame?: number) => void;
  stop: (renderFrame?: number) => void;
  resetMasterLoudnessMeter: (renderFrame?: number) => void;
  insertParameterConstructedValue: (paramId: number) => number;
  seekSample: (timelineSample: number, renderFrame?: number) => void;
  settleParameters: () => void;
  primeOfflineParameters: (numChannels: number, blockSize: number | undefined) => void;
  resetProcessorState: (renderFrame?: number) => void;
  tailSamples: () => number;
  maxBlockSize: () => number;
  graphLatencySamplesQ8: () => number;
  settleInsertParameters: () => void;
  flushControlCommands: () => void;
  applyCommandsDueNowPreservingFuture: () => void;
  seekPpq: (ppq: number, renderFrame?: number) => void;
  setTempo: (bpm: number) => void;
  setTempoSegments: (segments: WasmEngineTempoSegment[]) => void;
  setTimeSignature: (numerator: number, denominator: number) => void;
  setTimeSignatureSegments: (segments: WasmEngineTimeSignatureSegment[]) => void;
  sampleAtPpq: (ppq: number) => number;
  setLoop: (startPpq: number, endPpq: number, enabled: boolean) => void;
  addParameter: (info: WasmEngineParameterInfo) => void;
  parameterCount: () => number;
  parameterInfoByIndex: (index: number) => Required<WasmEngineParameterInfo>;
  parameterInfo: (id: number) => Required<WasmEngineParameterInfo>;
  setAutomationLane: (paramId: number, points: WasmEngineAutomationPoint[]) => void;
  automationLaneCount: () => number;
  setMarkers: (markers: WasmEngineMarker[]) => void;
  markerCount: () => number;
  markerByIndex: (index: number) => WasmEngineMarker;
  marker: (id: number) => WasmEngineMarker;
  seekMarker: (id: number, renderFrame?: number) => void;
  setLoopFromMarkers: (startMarkerId: number, endMarkerId: number) => void;
  setMetronome: (config: WasmEngineMetronomeConfig) => void;
  metronome: () => Required<WasmEngineMetronomeConfig>;
  countInEndSample: (startSample: number, bars: number) => number;
  setGraph: (spec: WasmEngineGraphSpec) => void;
  graphNodeCount: () => number;
  graphConnectionCount: () => number;
  setClips: (clips: WasmEngineClip[]) => void;
  upsertClip: (clip: WasmEngineClip) => void;
  removeClip: (clipId: number) => void;
  prebakedClipChannels: (clipId: number) => Float32Array[] | null;
  clipCount: () => number;
  setTrackLanes: (lanes: Array<number | WasmEngineTrackLane>) => void;
  setTrackSends: (trackId: number, sends: WasmEngineTrackSend[]) => void;
  setTrackOutputBus: (trackId: number, busId: number) => void;
  setInputTrack: (trackId: number) => void;
  clearInputTrack: () => void;
  inputTrack: () => number | undefined;
  setLaneSidechain: (trackId: number, insertIndex: number, sourceTrackId: number) => void;
  canSetLaneSidechain: (trackId: number, insertIndex: number, sourceTrackId: number) => number;
  canSetBusSidechain: (
    busId: number,
    insertIndex: number,
    sourceKind: number,
    sourceId: number,
  ) => number;
  canSetMasterSidechain: (insertIndex: number, sourceKind: number, sourceId: number) => number;
  applyProjectTimeline: (timelineId: number) => void;
  setTrackBuses: (buses: WasmEngineBus[]) => void;
  setBusSidechain: (
    busId: number,
    insertIndex: number,
    sourceKind: number,
    sourceId: number,
  ) => void;
  setMasterSidechain: (insertIndex: number, sourceKind: number, sourceId: number) => void;
  setBusStripJson: (busId: number, settingsJson: string) => void;
  setTrackStripJson: (trackId: number, settingsJson: string) => void;
  setTrackStripEqBandJson: (trackId: number, bandIndex: number, bandJson: string) => void;
  setBusStripEqBandJson: (busId: number, bandIndex: number, bandJson: string) => void;
  setTrackStripInsertBypassed: (
    trackId: number,
    insertIndex: number,
    bypassed: boolean,
    resetOnBypass: boolean,
  ) => void;
  setMasterStripJson: (settingsJson: string) => void;
  setMasterStripEqBandJson: (bandIndex: number, bandJson: string) => void;
  setMasterStripInsertBypassed: (
    insertIndex: number,
    bypassed: boolean,
    resetOnBypass: boolean,
  ) => void;
  setTrackStripInsertParamByName: (
    trackId: number,
    insertIndex: number,
    paramName: string,
    value: number,
  ) => void;
  applyTrackStripInsertParamByNameNow: (
    trackId: number,
    insertIndex: number,
    paramName: string,
    value: number,
  ) => boolean;
  restoreTrackStripInsertParamByName: (
    trackId: number,
    insertIndex: number,
    paramName: string,
    value: number,
  ) => void;
  setMasterStripInsertParamByName: (insertIndex: number, paramName: string, value: number) => void;
  applyMasterStripInsertParamByNameNow: (
    insertIndex: number,
    paramName: string,
    value: number,
  ) => boolean;
  restoreMasterStripInsertParamByName: (
    insertIndex: number,
    paramName: string,
    value: number,
  ) => void;
  setBusStripInsertParamByName: (
    busId: number,
    insertIndex: number,
    paramName: string,
    value: number,
  ) => void;
  applyBusStripInsertParamByNameNow: (
    busId: number,
    insertIndex: number,
    paramName: string,
    value: number,
  ) => boolean;
  restoreBusStripInsertParamByName: (
    busId: number,
    insertIndex: number,
    paramName: string,
    value: number,
  ) => void;
  clearTrackInsertParameterBases: (trackId: number) => void;
  clearBusInsertParameterBases: (busId: number) => void;
  clearMasterInsertParameterBases: () => void;
  setBusStripInsertBypassed: (
    busId: number,
    insertIndex: number,
    bypassed: boolean,
    resetOnBypass: boolean,
  ) => void;
  resolveTrackInsertAutomationId: (
    trackId: number,
    insertIndex: number,
    paramName: string,
  ) => number;
  resolveTrackLaneAutomationId: (trackId: number, paramName: string) => number;
  resolveBusAutomationId: (busId: number, paramName: string) => number;
  resolveMasterAutomationId: (paramName: string) => number;
  resolveMasterInsertAutomationId: (insertIndex: number, paramName: string) => number;
  resolveBusInsertAutomationId: (busId: number, insertIndex: number, paramName: string) => number;
  resolveInstrumentAutomationId: (destinationId: number, paramName: string) => number;
  setTrackStripPan: (trackId: number, pan: number) => void;
  setTrackStripPanLaw: (trackId: number, panLaw: number) => void;
  setTrackStripPanMode: (trackId: number, panMode: number) => void;
  setTrackStripDualPan: (trackId: number, leftPan: number, rightPan: number) => void;
  setTrackStripSurroundPan: (trackId: number, pan: SurroundPan) => void;
  setBusStripPan: (busId: number, pan: number) => void;
  setBusStripPanLaw: (busId: number, panLaw: number) => void;
  setBusStripPanMode: (busId: number, panMode: number) => void;
  setBusStripDualPan: (busId: number, leftPan: number, rightPan: number) => void;
  setTrackStripChannelDelaySamples: (trackId: number, delaySamples: number) => void;
  popClipPageRequest: () => WasmClipPageRequest | null;
  popClipPageRequestToScratch: () => boolean;
  clipPageRequestScratchClipId: () => number;
  clipPageRequestScratchSample: () => number;
  clipPageRequestOverflowCount: () => number;
  warpStretchOverflowCount: () => number;
  setWarpVoiceCapacity: (voices: number) => void;
  warpVoiceCapacity: () => number;
  setClipPagePrefetchFrames: (frames: number) => void;
  clipPagePrefetchFrames: () => number;
  setCaptureBuffer: (numChannels: number, capacityFrames: number) => void;
  armCapture: (armed: boolean) => void;
  setCapturePunch: (startSample: number, endSample: number, enabled: boolean) => void;
  setCaptureSource: (source: WasmEngineCaptureStatus['source'] | number) => void;
  setRecordOffsetSamples: (offsetSamples: number) => void;
  setCaptureTiming: (
    recordOffsetSamples: number,
    punchStartSample: number,
    punchEndSample: number,
    punchEnabled: boolean,
  ) => void;
  setInputMonitor: (enabled: boolean, gain: number) => void;
  inputMonitor: () => { enabled: boolean; gain: number };
  resetCapture: () => void;
  captureStatus: () => WasmEngineCaptureStatus;
  capturedAudio: () => Float32Array[];
  setMidiClips: (clips: readonly object[]) => void;
  setBuiltinInstrument: (destinationId: number, config: object) => void;
  setSynthInstrument: (destinationId: number, patch: object | string) => void;
  loadSoundFont: (data: Uint8Array) => void;
  setSf2Instrument: (destinationId: number, config: object) => void;
  clearMidiInstrument: (destinationId: number) => void;
  midiInstrumentCount: () => number;
  bindMidiCc: (binding: import('./project_types.js').ProjectMidiCcBinding) => void;
  clearMidiCcBindings: () => void;
  midiCcBindings: () => Array<
    import('./project_types.js').ProjectMidiCcBinding & { consume: boolean }
  >;
  setControllerProfile: (destinationId: number, presetName: string) => void;
  bindController: (
    destinationId: number,
    binding: import('./instrument_types.js').ControllerBinding,
  ) => void;
  clearControllerBindings: (destinationId: number) => void;
  controllerBindingCount: (destinationId: number) => number;
  setControllerVelocityMeaningful: (destinationId: number, meaningful: boolean) => void;
  controllerVelocityMeaningful: (destinationId: number) => boolean;
  setControllerNoteTracking: (
    destinationId: number,
    dimension: import('./instrument_types.js').MpeDimension | number,
    tracking: import('./instrument_types.js').NoteTracking | number,
  ) => void;
  controllerNoteTracking: (
    destinationId: number,
    dimension: import('./instrument_types.js').MpeDimension | number,
  ) => import('./instrument_types.js').NoteTracking;
  setArticulation: (
    destinationId: number,
    channel: number,
    articulation: import('./instrument_types.js').Articulation | number,
  ) => void;
  articulation: (
    destinationId: number,
    channel: number,
  ) => import('./instrument_types.js').Articulation;
  legatoFallbackCount: (destinationId: number) => number;
  setPartRig: (
    destinationId: number,
    part: number,
    mode: import('./instrument_types.js').PartRigMode | number,
    insertsJson: string | undefined,
  ) => void;
  setMidiFx: (destinationId: number, configJson: string) => void;
  clearMidiFx: (destinationId: number) => void;
  setMidiInputSource: (destinationId: number) => void;
  clearMidiInputSource: () => void;
  midiInputPendingCount: () => number;
  pushMidiInputNoteOn: (
    group: number,
    channel: number,
    note: number,
    velocity: number,
    offsetInBlockSamples: number,
  ) => void;
  pushMidiInputNoteOff: (
    group: number,
    channel: number,
    note: number,
    velocity: number,
    offsetInBlockSamples: number,
  ) => void;
  pushMidiInputCc: (
    group: number,
    channel: number,
    controller: number,
    value: number,
    offsetInBlockSamples: number,
  ) => void;
  pushMidiInputPitchBend: (
    group: number,
    channel: number,
    bend14: number,
    offsetInBlockSamples: number,
  ) => void;
  pushMidiInputChannelPressure: (
    group: number,
    channel: number,
    pressure: number,
    offsetInBlockSamples: number,
  ) => void;
  pushMidiInputPolyPressure: (
    group: number,
    channel: number,
    note: number,
    pressure: number,
    offsetInBlockSamples: number,
  ) => void;
  pushMidiNoteOn: (
    destinationId: number,
    group: number,
    channel: number,
    note: number,
    velocity: number,
    renderFrame?: number,
  ) => void;
  pushMidiNoteOff: (
    destinationId: number,
    group: number,
    channel: number,
    note: number,
    velocity: number,
    renderFrame?: number,
  ) => void;
  pushMidiCc: (
    destinationId: number,
    group: number,
    channel: number,
    controller: number,
    value: number,
    renderFrame?: number,
  ) => void;
  pushMidiPitchBend: (
    destinationId: number,
    group: number,
    channel: number,
    bend14: number,
    renderFrame?: number,
  ) => void;
  pushMidiChannelPressure: (
    destinationId: number,
    group: number,
    channel: number,
    pressure: number,
    renderFrame?: number,
  ) => void;
  pushMidiPolyPressure: (
    destinationId: number,
    group: number,
    channel: number,
    note: number,
    pressure: number,
    renderFrame?: number,
  ) => void;
  pushMidiUmp: (
    destinationId: number,
    words: Uint32Array | readonly number[],
    renderFrame?: number,
  ) => void;
  pushMidiInputUmp: (words: Uint32Array | readonly number[], offsetInBlockSamples: number) => void;
  pushMidiInputUmpAtRenderFrame: (
    words: Uint32Array | readonly number[],
    renderFrame: number,
  ) => void;
  pushMidiProgram: (
    destinationId: number,
    group: number,
    channel: number,
    program: number,
    bankValid: boolean,
    bankMsb: number,
    bankLsb: number,
    renderFrame?: number,
  ) => void;
  pushMidiInputProgram: (
    group: number,
    channel: number,
    program: number,
    bankValid: boolean,
    bankMsb: number,
    bankLsb: number,
    offsetInBlockSamples: number,
  ) => void;
  pushMidiSysex: (destinationId: number, data: Uint8Array, renderFrame?: number) => void;
  pushMidiPanic: (renderFrame?: number) => void;
  setMidiDestinationExternal: (destinationId: number, external: boolean) => void;
  setExternalMidiClockEnabled: (enabled: boolean) => void;
  externalMidiDroppedCount: () => number;
  externalMidiPendingCount: () => number;
  drainExternalMidi: (maxRecords: number | undefined) => WasmExternalMidiEvent[];
  popExternalMidiToScratch: () => boolean;
  externalMidiScratchDestinationId: () => number;
  externalMidiScratchRenderFrame: () => number;
  externalMidiScratchByteWord: () => number;
  externalMidiScratchByteCount: () => number;
  consumeExternalMidiScratch: () => void;
  clearParameters: () => void;
  /** `withMonitor` true returns the program and cue buses apart; undefined folds the cue in. */
  process: (
    channels: Float32Array[],
    withMonitor: boolean | undefined,
  ) => Float32Array[] | WasmEngineProcessWithMonitorResult;
  prepareChannels: (numChannels: number, maxFrames: number) => void;
  getChannelBuffer: (channel: number, numFrames: number) => Float32Array;
  processPrepared: (numFrames: number, withMonitor: boolean | undefined) => void;
  prepareMonitorChannels: (numChannels: number, maxFrames: number) => void;
  getMonitorChannelBuffer: (channel: number, numFrames: number) => Float32Array;
  renderOffline: (
    channels: Float32Array[],
    blockSize: number | undefined,
    finalize: boolean,
  ) => Float32Array[];
  finishOfflineRender: () => void;
  bounceOffline: (options: WasmEngineBounceOptions) => WasmEngineBounceResult;
  freezeOffline: (options: WasmEngineFreezeOptions) => WasmEngineFreezeResult;
  drainTelemetry: (maxRecords: number | undefined) => WasmEngineTelemetry[];
  popTelemetryToScratch: () => boolean;
  telemetryScratchType: () => number;
  telemetryScratchError: () => number;
  telemetryScratchRenderFrame: () => number;
  telemetryScratchTimelineSample: () => number;
  telemetryScratchAudibleTimelineSample: () => number;
  telemetryScratchGraphLatencySamplesQ8: () => number;
  telemetryScratchValue: () => number;
  popMeterTelemetryToScratch: () => boolean;
  meterScratchTargetId: () => number;
  meterScratchRenderFrame: () => number;
  meterScratchValue: (field: number) => number;
  meterScratchInputPeakDbL: () => number;
  meterScratchInputPeakDbR: () => number;
  drainMeterTelemetry: (maxRecords: number | undefined) => WasmEngineMeterTelemetry[];
  meterTargetInsertGainReduction: (targetId: number) => number[];
  configureScopeTelemetry: (intervalFrames: number, bandCount: number) => number;
  drainScopeTelemetry: (maxRecords: number | undefined) => WasmEngineScopeTelemetry[];
  popScopeTelemetryToScratch: () => boolean;
  scopeScratchTargetId: () => number;
  scopeScratchRenderFrame: () => number;
  scopeScratchBandCount: () => number;
  scopeScratchBand: (index: number) => number;
  scopeScratchPointCount: () => number;
  scopeScratchPointLeft: (index: number) => number;
  scopeScratchPointRight: (index: number) => number;
  delete: () => void;
}

export type { ProgressCallback } from './public_types.js';
export type TempogramMode = 'autocorrelation' | 'auto' | 'ac' | 'cosine' | 0 | 1;

export interface WasmSynthEnumTables {
  engineModes: string[];
  waveforms: string[];
  builtinWaveforms: string[];
  filterModels: string[];
  filterOutputs: string[];
  bodyTypes: string[];
  modSources: string[];
  modDestinations: string[];
  controllerInputs: string[];
  controllerAxes: string[];
  articulations: string[];
  mpeDimensions: string[];
  noteTrackings: string[];
  sampleLoopModes: string[];
  sampleKeyTracks: string[];
  retriggers: string[];
  lfoShapes: string[];
  partRigModes: string[];
  sampleDescLoopModes: string[];
}

export interface SonareModule {
  vocalEditAbiVersion: () => number;
  vocalEditSessionCreate: VocalWasmExports['vocalEditSessionCreate'];
  vocalEditSessionRestore: VocalWasmExports['vocalEditSessionRestore'];
  vocalEditSessionDestroy: VocalWasmExports['vocalEditSessionDestroy'];
  vocalEditSessionNotes: VocalWasmExports['vocalEditSessionNotes'];
  vocalEditDraftNotes: VocalWasmExports['vocalEditDraftNotes'];
  vocalEditSessionToken: VocalWasmExports['vocalEditSessionToken'];
  vocalEditDraftToken: VocalWasmExports['vocalEditDraftToken'];
  vocalEditSessionAnalysis: VocalWasmExports['vocalEditSessionAnalysis'];
  vocalEditSessionCapabilities: VocalWasmExports['vocalEditSessionCapabilities'];
  vocalEditSessionOutputLength: VocalWasmExports['vocalEditSessionOutputLength'];
  vocalEditSessionHistory: VocalWasmExports['vocalEditSessionHistory'];
  vocalEditSessionBeginEdit: VocalWasmExports['vocalEditSessionBeginEdit'];
  vocalEditDraftApply: VocalWasmExports['vocalEditDraftApply'];
  vocalEditDraftCommit: VocalWasmExports['vocalEditDraftCommit'];
  vocalEditDraftCancel: VocalWasmExports['vocalEditDraftCancel'];
  vocalEditDraftDestroy: VocalWasmExports['vocalEditDraftDestroy'];
  vocalEditSessionApplyHistory: VocalWasmExports['vocalEditSessionApplyHistory'];
  vocalEditEvaluatePitch: VocalWasmExports['vocalEditEvaluatePitch'];
  vocalEditMapCoordinate: VocalWasmExports['vocalEditMapCoordinate'];
  vocalEditCaptureSnapshot: VocalWasmExports['vocalEditCaptureSnapshot'];
  vocalEditSnapshotDestroy: VocalWasmExports['vocalEditSnapshotDestroy'];
  vocalEditSnapshotOutputLength: VocalWasmExports['vocalEditSnapshotOutputLength'];
  vocalEditSnapshotRender: VocalWasmExports['vocalEditSnapshotRender'];
  vocalEditRenderJobBegin: VocalWasmExports['vocalEditRenderJobBegin'];
  vocalEditRenderJobNext: VocalWasmExports['vocalEditRenderJobNext'];
  vocalEditRenderJobFinalize: VocalWasmExports['vocalEditRenderJobFinalize'];
  vocalEditRenderJobAbort: VocalWasmExports['vocalEditRenderJobAbort'];
  vocalEditRenderJobDestroy: VocalWasmExports['vocalEditRenderJobDestroy'];
  vocalEditSessionExportState: VocalWasmExports['vocalEditSessionExportState'];
  vocalEditLastErrorDetail: VocalWasmExports['vocalEditLastErrorDetail'];

  audioFromMemory: (data: Uint8Array) => WasmAudioFromMemoryResult;
  decodeChannels: (data: Uint8Array) => { sampleRate: number; channels: Float32Array[] };
  readAudioTags: (data: Uint8Array) => Record<string, string>;
  istft: (
    magnitude: Float32Array,
    nBins: number,
    nFrames: number,
    phase: Float32Array,
    phaseBins: number,
    phaseFrames: number,
    nFft: number,
    hopLength: number,
    lengthSamples: number | undefined,
  ) => Float32Array;
  encodeAudio: (
    channels: Float32Array[],
    sampleRate: number,
    options: EncodeAudioRequest,
  ) => Uint8Array;
  downmix: (
    channels: Float32Array[],
    targetLayout: number,
    options: { normalize?: boolean; includeLfe?: boolean },
  ) => Float32Array[];

  // Quick API (high-level)
  detectBpm: (
    samples: Float32Array,
    sampleRate: number,
    bpmMin?: number,
    bpmMax?: number,
  ) => number | undefined;
  detectKey: (samples: Float32Array, sampleRate: number) => WasmKeyResult;
  _detectKeyWithOptions: (
    samples: Float32Array,
    sampleRate: number,
    nFft: number,
    hopLength: number,
    useHpss: boolean,
    loudnessWeighted: boolean,
    chromaHighPassHz: number,
    modes: number[],
    profileType: number,
    genreHint: string,
    tuningCents: number | 'auto',
  ) => WasmKeyDetectionResult;
  _detectKeyCandidates: (
    samples: Float32Array,
    sampleRate: number,
    nFft: number,
    hopLength: number,
    useHpss: boolean,
    loudnessWeighted: boolean,
    chromaHighPassHz: number,
    modes: number[],
    profileType: number,
    genreHint: string,
    tuningCents: number | 'auto',
  ) => WasmKeyCandidateResult[];
  detectOnsets: (samples: Float32Array, sampleRate: number, options: object) => Float32Array;
  detectBeats: (
    samples: Float32Array,
    sampleRate: number,
    bpmMin?: number,
    bpmMax?: number,
  ) => Float32Array;
  detectDownbeats: (samples: Float32Array, sampleRate: number) => Float32Array;
  detectChords: (
    samples: Float32Array,
    sampleRate: number,
    minDuration: number,
    smoothingWindow: number,
    threshold: number,
    useTriadsOnly: boolean,
    nFft: number,
    hopLength: number,
    useBeatSync: boolean,
    useHmm: boolean,
    hmmBeamWidth: number,
    useKeyContext: boolean,
    keyRoot: number,
    keyMode: number,
    detectInversions: boolean,
    chromaMethod: number,
    tuningCents: number | 'auto',
  ) => WasmChordAnalysisResult;
  chordFunctionalAnalysis: (
    samples: Float32Array,
    keyRoot: number,
    keyMode: number,
    sampleRate: number,
    minDuration: number,
    smoothingWindow: number,
    threshold: number,
    useTriadsOnly: boolean,
    nFft: number,
    hopLength: number,
    useBeatSync: boolean,
    useHmm: boolean,
    hmmBeamWidth: number,
    useKeyContext: boolean,
    detectInversions: boolean,
    chromaMethod: number,
    tuningCents: number | 'auto',
  ) => WasmChordAnalysisResult & { roman: string[]; functions: string[] };
  chordFunctions: (
    chords: readonly { root: number; quality: number }[],
    keyRoot: number,
    keyMode: number,
  ) => { roman: string[]; functions: string[] };
  acousticSelectorNames: () => { materialPreset: string[]; mode: string[] };
  analyze: (
    samples: Float32Array,
    sampleRate: number,
    options: object,
    progressCallback?: ProgressCallback | null,
    cancelCallback?: (() => boolean) | null,
  ) => WasmAnalysisResult;
  estimateMeter: (
    accent: readonly number[],
    hasAttack: readonly boolean[] | undefined,
    lowFrequencyEnergy: readonly number[] | undefined,
    subdivisionAccent: readonly number[] | undefined,
    options: object,
  ) => WasmMeterEstimateResult;
  _synthEnumTables: () => WasmSynthEnumTables;
  _synthPatchRoundTrip: (patch: unknown) => unknown;
  // Arrangement surface. Present only in a build with arrangement support, like
  // the mixing and voice-changer entries elsewhere in this interface; callers
  // probe for it at runtime (see projectModule() in project_internal.ts) rather
  // than through the type. `unknown` marks the shapes the facade owns — this
  // file describes the raw module and does not depend on the facade's types.
  projectAbiVersion: () => number;
  automationTargetKindNames: () => string[];
  synthPresetNames: () => string[];
  synthPresetPatch: (name: string) => unknown;
  synthEngineParamInfo: (engineMode: number) => string;
  synthPatchParamInfo: () => string;
  synthGsDrumKitName: (program: number) => string | null;
  synthGsDrumKitIsVoicedApart: (program: number) => number;
  synthGsVariationIsVoicedApart: (bank: number, program: number) => number;
  controllerProfileNames: () => string[];
  midiVelocityToMidi2: (velocity: number) => number;
  midiNoteOnVelocityToMidi2: (velocity: number) => number;
  midi2VelocityToMidi1: (velocity16: number) => number;
  midi2NoteOnVelocityToMidi1: (velocity16: number) => number;
  midiControlToMidi2: (value: number) => number;
  midiControl14ToMidi2: (value14: number) => number;
  midi2ControlToMidi1: (value32: number) => number;
  midi2ControlToMidi1Control14: (value32: number) => number;
  midiPitchBendToMidi2: (bend14: number) => number;
  midi2PitchBendToMidi1: (bend32: number) => number;
  midiGmInstrumentName: (program: number) => string | null;
  midiGmProgramForName: (name: string) => number;
  midiGmFamilyName: (family: number) => string | null;
  midiGmFamilyFirstProgram: (family: number) => number;
  midiGm2InstrumentName: (bankLsb: number, program: number) => string | null;
  midiGmDrumName: (note: number) => string | null;
  midiGmDrumNoteForName: (name: string) => number;
  midiGm2DrumSetName: (bankLsb: number) => string | null;
  midiGm2DrumName: (bankLsb: number, note: number) => string | null;
  midiCcName: (controller: number) => string | null;
  midiCcIndexForName: (name: string) => number;
  midiPerNoteControllerName: (index: number) => string | null;
  midiBankProgram: (
    ppq: number,
    group: number,
    channel: number,
    bankMsb: number,
    bankLsb: number,
    program: number,
  ) => unknown;
  midiRouteEvents: (events: readonly unknown[], config: unknown) => unknown;
  midiCcLearn: (
    events: ReadonlyArray<import('./project_types.js').ProjectMidiEvent>,
    paramId: number,
    minValue: number,
    maxValue: number,
    minMovement: number,
  ) => (import('./project_types.js').ProjectMidiCcBinding & { consume: boolean }) | null;
  midiCcToBreakpoint: (
    bindings: readonly unknown[],
    event: import('./project_types.js').ProjectMidiEvent,
  ) => unknown;
  midiParamToCc: (
    bindings: readonly unknown[],
    paramId: number,
    unitValue: number,
    group: number,
    ppq: number,
  ) => unknown;
  transcribe: (
    samples: Float32Array,
    sampleRate: number,
    tempoBpm: number | undefined,
    config: unknown,
  ) => unknown;
  alignTakeToReference: (
    reference: Float32Array,
    take: Float32Array,
    sampleRate: number,
    takeSampleRate: number,
    config: unknown,
  ) => unknown;
  chordSymbolFromAnalysis: (root: number, quality: number, bass: number) => unknown;
  keyModeFromAnalysis: (mode: number) => number;
  _analysisResultSchemaPaths: () => string[];
  _analysisResultSchemaFixture: () => WasmAnalysisResult;
  analyzeImpulseResponse: (
    samples: Float32Array,
    sampleRate: number,
    nOctaveBands: number,
    minDecayDb?: number | null,
  ) => WasmAcousticResult;
  detectAcoustic: (
    samples: Float32Array,
    sampleRate: number,
    nOctaveBands: number,
    nThirdOctaveSubbands: number,
    minDecayDb: number,
    noiseFloorMarginDb: number,
  ) => WasmAcousticResult;
  // Acoustic-simulation entry points are present only in builds compiled with
  // SONARE_WITH_ACOUSTIC_SIM; absent otherwise (the wrappers throw a clear error).
  synthesizeRir?: (options: WasmRirSynthOptions) => WasmRirResult;
  estimateRoom?: (
    samples: Float32Array,
    sampleRate: number,
    options: WasmRoomEstimateOptions,
  ) => WasmRoomEstimateResult;
  roomMorph?: (
    samples: Float32Array,
    sampleRate: number,
    options: WasmRoomMorphOptions,
  ) => WasmRoomMorphResult;
  roomMorphStereo?: (
    left: Float32Array,
    right: Float32Array,
    sampleRate: number,
    options: WasmRoomMorphStereoOptions,
  ) => WasmRoomMorphStereoResult;
  roomGeometryFromEstimate?: (estimate: {
    lengthM?: number;
    widthM?: number;
    heightM?: number;
    bandAbsorption?: Float32Array | number[];
  }) => Omit<WasmRoomGeometryOptions, 'materialPreset'>;
  analyzeBpm: (
    samples: Float32Array,
    sampleRate: number,
    bpmMin: number | undefined,
    bpmMax: number | undefined,
    nFft: number,
    hopLength: number,
    maxCandidates: number,
  ) => WasmBpmAnalysisResult;
  /**
   * @param startBpm Finite, positive BPM prior that seeds beat tracking if tempo search finds none.
   *   The result reports measured BPM or omits `bpm` when no tempo is measured. Default 120.
   */
  analyzeRhythm: (
    samples: Float32Array,
    sampleRate: number,
    bpmMin: number | undefined,
    bpmMax: number | undefined,
    startBpm: number | undefined,
    nFft: number,
    hopLength: number,
  ) => WasmRhythmAnalysisResult;
  analyzeDynamics: (
    samples: Float32Array,
    sampleRate: number,
    windowSec: number,
    hopLength: number,
    compressionThreshold: number,
  ) => WasmDynamicsAnalysisResult;
  analyzeTimbre: (
    samples: Float32Array,
    sampleRate: number,
    nFft: number,
    hopLength: number,
    nMels: number,
    nMfcc: number,
    windowSec: number,
  ) => WasmTimbreAnalysisResult;
  detectKeyCandidates: (samples: Float32Array, sampleRate: number) => WasmKeyCandidateResult[];
  version: () => string;
  capabilities: () => Capabilities;
  systemInfo: () => string;
  capabilityCatalog: () => string;
  abiVersion: () => number;
  engineAbiVersion: () => number;
  voiceChangerAbiVersion: () => number;
  voiceCharacterPresetId: (preset: number) => string;
  realtimeVoiceChangerPresetConfig: (preset: number) => WasmRealtimeVoiceChangerPodConfig;

  meteringPeakDb: (samples: Float32Array, sampleRate: number) => number;
  meteringRmsDb: (samples: Float32Array, sampleRate: number) => number;
  meteringSilenceRatio: (
    samples: Float32Array,
    sampleRate: number,
    thresholdDb: number,
    frameLength: number,
    hopLength: number,
  ) => number;
  meteringCrestFactorDb: (samples: Float32Array, sampleRate: number) => number;
  meteringCrestFactorDbStereo: (
    left: Float32Array,
    right: Float32Array,
    sampleRate: number,
  ) => number;
  meteringDcOffset: (samples: Float32Array, sampleRate: number) => number;
  meteringTruePeakDb: (
    samples: Float32Array,
    sampleRate: number,
    oversampleFactor: number,
  ) => number;
  meteringDetectClipping: (
    samples: Float32Array,
    sampleRate: number,
    thresholdDb: number | undefined,
    minRegionSamples: number,
  ) => {
    clippedSamples: number;
    clippingRatio: number;
    maxClippedPeak: number;
    regions: Array<{ startSample: number; endSample: number; length: number; peak: number }>;
  };
  meteringDynamicRange: (
    samples: Float32Array,
    sampleRate: number,
    windowSec: number,
    hopSec: number,
    lowPercentile: number,
    highPercentile: number,
  ) => {
    dynamicRangeDb?: number;
    lowPercentileDb?: number;
    highPercentileDb?: number;
    windowRmsDb: Float32Array;
  };

  meteringStereoCorrelation: (
    left: Float32Array,
    right: Float32Array,
    sampleRate: number,
  ) => number;
  meteringStereoWidth: (left: Float32Array, right: Float32Array, sampleRate: number) => number;
  meteringVectorscope: (
    left: Float32Array,
    right: Float32Array,
    sampleRate: number,
    maxPoints: number,
  ) => { mid: Float32Array; side: Float32Array };
  meteringPhaseScope: (
    left: Float32Array,
    right: Float32Array,
    sampleRate: number,
    maxPoints: number,
  ) => {
    mid: Float32Array;
    side: Float32Array;
    radius: Float32Array;
    angleRad: Float32Array;
    correlation?: number;
    averageAbsAngleRad?: number;
    maxRadius: number;
  };
  meteringSpectrum: (
    samples: Float32Array,
    sampleRate: number,
    options: {
      nFft?: number;
      applyOctaveSmoothing?: boolean;
      octaveFraction?: number;
      dbRef?: number;
      dbAmin?: number;
    },
  ) => {
    frequencies: Float32Array;
    magnitude: Float32Array;
    power: Float32Array;
    db: Float32Array;
    nFft: number;
    sampleRate: number;
  };
  meteringSpectrumFrame: (
    samples: Float32Array,
    sampleRate: number,
    frameOffset: number,
    options: {
      nFft?: number;
      applyOctaveSmoothing?: boolean;
      octaveFraction?: number;
      dbRef?: number;
      dbAmin?: number;
    },
  ) => {
    frequencies: Float32Array;
    magnitude: Float32Array;
    power: Float32Array;
    db: Float32Array;
    nFft: number;
    sampleRate: number;
  };
  waveformPeaks: (
    samples: Float32Array,
    channels: number,
    samplesPerBucket: number,
  ) => {
    min: Float32Array;
    max: Float32Array;
    channels: number;
    bucketCount: number;
    samplesPerBucket: number;
  };
  waveformPeakPyramid: (
    samples: Float32Array,
    channels: number,
    samplesPerBucketLevels: number[],
  ) => Array<{
    min: Float32Array;
    max: Float32Array;
    channels: number;
    bucketCount: number;
    samplesPerBucket: number;
  }>;

  scaleMaskForMode: (root: number, mode: number | string) => number;
  scaleQuantizeMidi: (
    root: number,
    modeMask: number,
    midi: number,
    scaleReferenceMidi: number | undefined,
  ) => number;
  scaleCorrectionSemitones: (
    root: number,
    modeMask: number,
    midi: number,
    scaleReferenceMidi: number | undefined,
  ) => number;
  scalePitchClassEnabled: (root: number, modeMask: number, pitchClass: number) => boolean;

  // Effects
  hpss: (
    samples: Float32Array,
    sampleRate: number,
    kernelHarmonic: number,
    kernelPercussive: number,
    nFft: number,
    hopLength: number,
    hardMask: boolean,
    withResidual: boolean,
  ) => WasmHpssResult;
  decompose: (
    s: Float32Array,
    nFeatures: number,
    nFrames: number,
    nComponents: number,
    nIter: number | undefined,
    beta: number,
    init: string,
  ) => WasmDecomposeResult;
  nnFilter: (
    s: Float32Array,
    nFeatures: number,
    nFrames: number,
    aggregate: string,
    k: number | undefined,
    width: number | undefined,
  ) => WasmMatrix2dResult;
  remix: (
    samples: Float32Array,
    intervals: Int32Array,
    sampleRate: number,
    alignZeros?: boolean | null,
  ) => Float32Array;
  decomposeStems: (
    samples: Float32Array,
    sampleRate: number,
    options: {
      nComponents?: number;
      nFft?: number;
      hopLength?: number;
      nIter?: number;
      beta?: number;
      init?: string;
      maskPower?: number;
    },
  ) => {
    components: Float32Array[];
    w: Float32Array;
    h: Float32Array;
    sampleRate: number;
  };
  decomposeStemsLinked: (
    channels: Float32Array[],
    sampleRate: number,
    options: {
      nComponents?: number;
      nFft?: number;
      hopLength?: number;
      nIter?: number;
      beta?: number;
      init?: string;
      maskPower?: number;
    },
  ) => {
    components: Float32Array[][];
    w: Float32Array;
    h: Float32Array;
    sampleRate: number;
  };
  remixAlignedIntervals: (
    samples: Float32Array,
    intervals: Int32Array,
    sampleRate: number,
    alignZeros?: boolean | null,
  ) => Int32Array;
  phaseVocoder: (
    samples: Float32Array,
    sampleRate: number,
    rate: number,
    nFft: number,
    hopLength: number,
  ) => Float32Array;
  harmonic: (samples: Float32Array, sampleRate: number) => Float32Array;
  percussive: (samples: Float32Array, sampleRate: number) => Float32Array;
  timeStretch: (
    samples: Float32Array,
    sampleRate: number,
    rate: number,
    nFft: number,
    hopLength: number,
  ) => Float32Array;
  pitchShift: (
    samples: Float32Array,
    sampleRate: number,
    pitchSemitones: number,
    nFft: number,
    hopLength: number,
  ) => Float32Array;
  pitchCorrectToMidi: (
    samples: Float32Array,
    sampleRate: number,
    currentMidi: number,
    targetMidi: number,
  ) => Float32Array;
  pitchCorrectToMidiTimevarying: (
    samples: Float32Array,
    sampleRate: number,
    f0Hz: Float32Array,
    targetMidi: number,
    hopLength: number,
    voiced?: Float32Array,
    voicedProb?: Float32Array,
  ) => Float32Array;
  pitchCorrectTimevarying: (
    samples: Float32Array,
    sampleRate: number,
    f0Hz: Float32Array,
    hopLength: number,
    options: {
      targetMode?: string;
      targetMidi?: number;
      scaleRoot?: number;
      scaleModeMask?: number | string;
      scaleReferenceMidi?: number;
      retuneAmount?: number;
      maxCorrectionSemitones?: number;
      retuneSpeedMs?: number;
      vibratoThresholdCents?: number;
      voiced?: Float32Array;
      voicedProb?: Float32Array;
    },
  ) => Float32Array;
  autoTune: (
    samples: Float32Array,
    sampleRate: number,
    key: { root: number; mode: number | string } | null,
    options: {
      strength?: number;
      retuneSpeedMs?: number;
      vibratoThresholdCents?: number;
      maxCorrectionSemitones?: number;
      scaleReferenceMidi?: number;
    },
  ) => {
    samples: Float32Array;
    key: {
      root: number;
      mode: number;
      confidence: number;
      name: string;
      shortName: string;
    };
  };
  noteStretch: (
    samples: Float32Array,
    sampleRate: number,
    onsetSample: number,
    offsetSample: number,
    stretchRatio: number,
  ) => Float32Array;
  noteMove: (
    samples: Float32Array,
    sampleRate: number,
    onsetSample: number,
    offsetSample: number,
    targetOnsetSample: number,
  ) => Float32Array;
  extractNotes: (
    samples: Float32Array,
    sampleRate: number,
    f0Hz: Float32Array,
    voicedProb: Float32Array | undefined,
    voiced: Float32Array | undefined,
    frameRate: number,
    options: {
      segmentationThresholdCents?: number;
      minNoteMs?: number;
      referenceHz?: number;
      voicedThreshold?: number;
    },
  ) => NoteObject[];
  renderNotes: (
    samples: Float32Array,
    sampleRate: number,
    notes: readonly NoteObjectInput[],
    options: {
      fadeMs?: number;
      f0Hz?: Float32Array;
      voiced?: Float32Array;
      frameRate?: number;
      vibratoCutoffHz?: number;
    },
  ) => Float32Array;
  decomposeNotePitch: (
    f0Hz: Float32Array,
    voiced: Float32Array | undefined,
    frameRate: number,
    medianHz: number,
    vibratoCutoffHz: number,
  ) => PitchDecompositionResult;
  splitNote: (
    samples: Float32Array,
    sampleRate: number,
    f0Hz: Float32Array,
    voicedProb: Float32Array | undefined,
    voiced: Float32Array | undefined,
    frameRate: number,
    notes: readonly NoteSetEntry[],
    index: number,
    frame: number,
    options: {
      segmentationThresholdCents?: number;
      minNoteMs?: number;
      referenceHz?: number;
      voicedThreshold?: number;
    },
  ) => NoteObject[];
  mergeNotes: (
    samples: Float32Array,
    sampleRate: number,
    f0Hz: Float32Array,
    voicedProb: Float32Array | undefined,
    voiced: Float32Array | undefined,
    frameRate: number,
    notes: readonly NoteSetEntry[],
    first: number,
    last: number,
    options: {
      segmentationThresholdCents?: number;
      minNoteMs?: number;
      referenceHz?: number;
      voicedThreshold?: number;
    },
  ) => NoteObject[];
  assignNoteTargets: (
    notes: readonly NoteObject[],
    sampleRate: number,
    targets: readonly NoteTarget[],
    options: {
      unmatchedPolicy?: NoteTargetUnmatchedPolicy;
      minOverlapRatio?: number;
      maxCorrectionSemitones?: number;
    },
  ) => AssignNoteTargetsResult;
  /** Absent from a build without arrangement support, which is what links the MIDI reader. */
  noteTargetsFromSmf?: (data: Uint8Array, trackIndex: number) => NoteTarget[];
  createPolyphonicAnalysis: (
    samples: Float32Array,
    sampleRate: number,
    config: Record<string, unknown>,
  ) => WasmPolyphonicAnalysis;
  extractPercussiveEvents: (
    samples: Float32Array | readonly number[],
    sampleRate: number,
    options: {
      nFft?: number;
      hopLength?: number;
      hpssKernelHarmonic?: number;
      hpssKernelPercussive?: number;
      onsetWait?: number;
      onsetDelta?: number;
      maxEventMs?: number;
      minPercussiveRatio?: number;
    },
  ) => PercussiveEvent[];
  renderPercussiveEvents: (
    samples: Float32Array | readonly number[],
    sampleRate: number,
    events: readonly PercussiveEventInput[],
    options: {
      nFft?: number;
      hopLength?: number;
      hpssKernelHarmonic?: number;
      hpssKernelPercussive?: number;
      fadeMs?: number;
    },
  ) => Float32Array;
  voiceChange: (
    samples: Float32Array,
    sampleRate: number,
    pitchSemitones: number,
    formantFactor: number,
    formantMode: string,
  ) => Float32Array;
  realtimeVoiceChangerRender: (
    samples: Float32Array,
    sampleRate: number,
    preset: string,
    channels: number,
  ) => Float32Array;
  normalize: (
    samples: Float32Array,
    sampleRate: number,
    targetDb: number,
    mode: string,
  ) => Float32Array;
  normalizeStereo: (
    left: Float32Array,
    right: Float32Array,
    sampleRate: number,
    targetDb: number,
    mode: string,
  ) => { left: Float32Array; right: Float32Array; appliedGainDb: number };
  mastering: (
    samples: Float32Array,
    sampleRate: number,
    targetLufs: number,
    ceilingDbtp: number,
    truePeakOversample: number,
    releaseMs: number,
    applyGainAtInputRate: boolean,
  ) => WasmMasteringResult;
  masteringProcessorNames: () => string[];
  masteringPairProcessorNames: () => string[];
  masteringRequireStereoPairProcessor: (processorName: string) => void;
  masteringPairAnalysisNames: () => string[];
  masteringStereoAnalysisNames: () => string[];
  masteringInsertNames: () => string[];
  masteringInsertParamNames: (name: string) => string[];
  // These return JSON the facade parses; the raw module has no object shape here.
  masteringInsertParamInfo: (name: string, sampleRate: number | undefined) => string;
  masteringProcessorCatalog: () => string;
  masteringAmpPresetCatalog: () => string;
  masteringEnumValue: (processor: string, key: string, choice: string) => number | null;
  masteringInsertTiming: (
    name: string,
    paramsJson: string,
    sampleRate: number,
  ) => { latencySamples: number; tailSamples: number };
  masteringProcess: (
    processorName: string,
    samples: Float32Array,
    sampleRate: number,
    params: Record<string, number | boolean>,
  ) => WasmMasteringResult;
  masteringProcessStereo: (
    processorName: string,
    left: Float32Array,
    right: Float32Array,
    sampleRate: number,
    params: Record<string, number | boolean>,
  ) => WasmMasteringStereoResult;
  masteringPairProcess: (
    processorName: string,
    source: Float32Array,
    reference: Float32Array,
    sampleRate: number,
    referenceSampleRate: number,
    params: Record<string, number | boolean>,
  ) => WasmMasteringResult;
  masteringPairAnalyze: (
    analysisName: string,
    source: Float32Array,
    reference: Float32Array,
    sampleRate: number,
    referenceSampleRate: number,
    params: Record<string, number | boolean>,
  ) => string;
  masteringPairAnalyzeStereo: (
    analysisName: string,
    sourceLeft: Float32Array,
    sourceRight: Float32Array,
    referenceLeft: Float32Array,
    referenceRight: Float32Array,
    sampleRate: number,
    referenceSampleRate: number,
    params: Record<string, number | boolean>,
  ) => string;
  masteringPairProcessStereo: (
    processorName: string,
    sourceLeft: Float32Array,
    sourceRight: Float32Array,
    referenceLeft: Float32Array,
    referenceRight: Float32Array,
    sampleRate: number,
    referenceSampleRate: number,
    params: Record<string, number | boolean>,
  ) => WasmMasteringStereoResult;
  masteringAbMatchLoudness: (
    source: Float32Array,
    reference: Float32Array,
    sampleRate: number,
    referenceSampleRate: number,
  ) => WasmLoudnessMatchResult;
  masteringAbMatchLoudnessStereo: (
    sourceLeft: Float32Array,
    sourceRight: Float32Array,
    referenceLeft: Float32Array,
    referenceRight: Float32Array,
    sampleRate: number,
    referenceSampleRate: number,
  ) => WasmLoudnessMatchStereoResult;
  masteringStereoAnalyze: (
    analysisName: string,
    left: Float32Array,
    right: Float32Array,
    sampleRate: number,
    params: Record<string, number | boolean>,
  ) => string;
  masteringAssistantSuggest: (
    samples: Float32Array,
    sampleRate: number,
    params: Record<string, number | boolean | string>,
  ) => string;
  masteringAudioProfile: (
    samples: Float32Array,
    sampleRate: number,
    params: Record<string, number | boolean>,
  ) => string;
  masteringStreamingPreview: (
    samples: Float32Array,
    sampleRate: number,
    platforms: Array<{ name: string; targetLufs: number; ceilingDbtp: number }>,
  ) => string;
  masteringAssistantSuggestStereo: (
    left: Float32Array,
    right: Float32Array,
    sampleRate: number,
    params: Record<string, number | boolean | string>,
  ) => string;
  masteringAudioProfileStereo: (
    left: Float32Array,
    right: Float32Array,
    sampleRate: number,
    params: Record<string, number | boolean>,
  ) => string;
  masteringStreamingPreviewStereo: (
    left: Float32Array,
    right: Float32Array,
    sampleRate: number,
    platforms: Array<{ name: string; targetLufs: number; ceilingDbtp: number }>,
  ) => string;
  masteringRepairDeclick: (
    samples: Float32Array,
    sampleRate: number,
    options: object,
  ) => { samples: Float32Array; report: WasmDeclickReport };
  masteringRepairDeclickStereo: (
    left: Float32Array,
    right: Float32Array,
    sampleRate: number,
    options: object,
  ) => WasmDeclickStereoResult;
  masteringRepairDenoiseClassical: (
    samples: Float32Array,
    sampleRate: number,
    options: object,
  ) => { samples: Float32Array; report: WasmDenoiseReport };
  masteringRepairDenoiseClassicalStereo: (
    left: Float32Array,
    right: Float32Array,
    sampleRate: number,
    options: object,
  ) => WasmDenoiseStereoResult;
  masteringRepairDenoiseClassicalLinked: (
    channels: Float32Array[],
    sampleRate: number,
    options: object,
  ) => WasmDenoiseLinkedResult;
  masteringRepairAnalyze: (
    channels: Float32Array[],
    sampleRate: number,
    requestJson: string,
  ) => string;
  masteringRepairApply: (
    channels: Float32Array[],
    sampleRate: number,
    stagesJson: string,
    progressCallback: ProgressCallback | null,
    cancelCallback: (() => boolean) | null,
  ) => { channels: Float32Array[]; reportsJson: string };
  masteringRepairDeclip: (
    samples: Float32Array,
    sampleRate: number,
    options: object,
  ) => { samples: Float32Array; report: WasmDeclipReport };
  masteringRepairDeclipStereo: (
    left: Float32Array,
    right: Float32Array,
    sampleRate: number,
    options: object,
  ) => WasmDeclipStereoResult;
  masteringRepairDecrackle: (
    samples: Float32Array,
    sampleRate: number,
    options: object,
  ) => { samples: Float32Array; report: WasmDecrackleReport };
  masteringRepairDecrackleStereo: (
    left: Float32Array,
    right: Float32Array,
    sampleRate: number,
    options: object,
  ) => WasmDecrackleStereoResult;
  masteringRepairDehum: (
    samples: Float32Array,
    sampleRate: number,
    options: object,
  ) => { samples: Float32Array; report: WasmDehumReport };
  masteringRepairDehumStereo: (
    left: Float32Array,
    right: Float32Array,
    sampleRate: number,
    options: object,
  ) => WasmDehumStereoResult;
  masteringRepairDereverbClassical: (
    samples: Float32Array,
    sampleRate: number,
    options: object,
  ) => { samples: Float32Array; report: WasmDereverbReport };
  masteringRepairDereverbClassicalStereo: (
    left: Float32Array,
    right: Float32Array,
    sampleRate: number,
    options: object,
  ) => WasmDereverbStereoResult;
  masteringRepairDereverbClassicalLinked: (
    channels: Float32Array[],
    sampleRate: number,
    options: object,
  ) => WasmDereverbLinkedResult;
  masteringRepairDereverbConfigForRoom: (
    estimate: WasmRoomEstimateResult,
    options: object,
  ) => WasmDereverbClassicalConfig;
  masteringRepairTrimSilence: (
    samples: Float32Array,
    sampleRate: number,
    options: object,
  ) => { samples: Float32Array; report: WasmTrimReport };
  masteringRepairTrimSilenceStereo: (
    left: Float32Array,
    right: Float32Array,
    sampleRate: number,
    options: object,
  ) => WasmTrimSilenceStereoResult;
  masteringRepairDetectClicks: (
    samples: Float32Array,
    sampleRate: number,
    options: object,
  ) => WasmClickDetection;
  masteringRepairDetectNoiseFloor: (
    samples: Float32Array,
    sampleRate: number,
    options: object,
  ) => WasmNoiseDetection;
  masteringRepairNoiseBandBins: (nFft: number, sampleRate: number) => Int32Array;
  masteringRepairDetectClipping: (
    samples: Float32Array,
    sampleRate: number,
    options: object,
  ) => WasmClipDetection;
  masteringRepairDetectCrackle: (
    samples: Float32Array,
    sampleRate: number,
    options: object,
  ) => WasmCrackleDetection;
  masteringRepairDetectHum: (
    samples: Float32Array,
    sampleRate: number,
    options: object,
  ) => WasmHumDetection;
  masteringRepairDetectReverb: (
    samples: Float32Array,
    sampleRate: number,
    options: object,
  ) => WasmReverbDetection;
  masteringRepairDetectTrimRange: (
    samples: Float32Array,
    sampleRate: number,
    options: object,
  ) => WasmTrimRange;
  masteringRepairDetectTrimRangeStereo: (
    left: Float32Array,
    right: Float32Array,
    sampleRate: number,
    options: object,
  ) => WasmTrimRange;
  masteringDynamicsCompressor: (
    samples: Float32Array,
    sampleRate: number,
    options: object,
  ) => { samples: Float32Array; latencySamples: number };
  masteringDynamicsGate: (
    samples: Float32Array,
    sampleRate: number,
    options: object,
  ) => { samples: Float32Array; latencySamples: number };
  masteringDynamicsTransientShaper: (
    samples: Float32Array,
    sampleRate: number,
    options: object,
  ) => { samples: Float32Array; latencySamples: number };
  masteringChain: (
    samples: Float32Array,
    sampleRate: number,
    configJson: string | null,
    progressCallback?: ProgressCallback | null,
    cancelCallback?: (() => boolean) | null,
  ) => WasmMasteringChainResult;
  masteringChainStereo: (
    left: Float32Array,
    right: Float32Array,
    sampleRate: number,
    configJson: string | null,
    progressCallback?: ProgressCallback | null,
    cancelCallback?: (() => boolean) | null,
  ) => WasmMasteringStereoChainResult;
  masteringStreamingLoudnessGain: (
    samples: Float32Array,
    sampleRate: number,
    configJson: string | null,
  ) => WasmStreamingLoudnessGainResult;
  masteringStreamingLoudnessGainStereo: (
    left: Float32Array,
    right: Float32Array,
    sampleRate: number,
    configJson: string | null,
  ) => WasmStreamingLoudnessGainResult;
  masteringPresetNames: () => string[];
  masteringPresetParams: (preset: string) => string;
  masteringChainConfigParams: (document: string) => string;
  masteringPlatformNames: () => string[];
  masterAudio: (
    preset: string | null,
    samples: Float32Array,
    sampleRate: number,
    overridesJson: string | null,
    progressCallback?: ProgressCallback | null,
    cancelCallback?: (() => boolean) | null,
  ) => WasmMasteringChainResult;
  masterAudioStereo: (
    preset: string | null,
    left: Float32Array,
    right: Float32Array,
    sampleRate: number,
    overridesJson: string | null,
    progressCallback?: ProgressCallback | null,
    cancelCallback?: (() => boolean) | null,
  ) => WasmMasteringStereoChainResult;
  mixingScenePresetNames: () => string[];
  mixingScenePresetJson: (presetName: string) => string;
  mixStereo: (
    leftChannels: Float32Array[],
    rightChannels: Float32Array[],
    sampleRate: number,
    options: Record<string, unknown>,
  ) => WasmMixResult;
  mixingAssistantSuggest: (
    leftChannels: Float32Array[],
    rightChannels: (Float32Array | null)[] | null,
    trackIds: string[],
    trackNames: (string | null)[] | null,
    trackSampleRates: (number | null)[] | null,
    sampleRate: number,
    params: Record<string, number | boolean>,
  ) => string;
  mixingAssistantSourceClassNames: () => string[];
  mixingAssistantSourceClassFromName: (name: string) => number;
  fade: (
    samples: Float32Array,
    sampleRate: number,
    fadeInSec: number,
    fadeOutSec: number,
  ) => Float32Array;
  biquadFilter: (
    samples: Float32Array,
    sampleRate: number,
    type: number,
    order: number,
    zeroPhase: boolean,
    cutoffHz: number | undefined,
    centerHz: number | undefined,
    bandwidthHz: number | undefined,
  ) => Float32Array;
  trim: (
    samples: Float32Array,
    sampleRate: number,
    thresholdDb: number,
    frameLength: number,
    hopLength: number,
  ) => Float32Array;
  spectralEdit: (
    samples: Float32Array,
    sampleRate: number,
    ops: SpectralRegionOp[],
    options: Record<string, unknown>,
  ) => Float32Array;

  // Features - Spectrogram
  /** `db` and `phase` select optional result planes. */
  stft: (
    samples: Float32Array,
    sampleRate: number,
    nFft: number,
    hopLength: number,
    db: boolean | undefined,
    phase: boolean | undefined,
  ) => WasmStftResult;

  // Features - Mel Spectrogram
  melSpectrogram: (
    samples: Float32Array,
    sampleRate: number,
    nFft: number,
    hopLength: number,
    nMels: number,
    fmin: number,
    fmax: number,
    htk: boolean,
  ) => WasmMelResult;
  mfcc: (
    samples: Float32Array,
    sampleRate: number,
    nFft: number,
    hopLength: number,
    nMels: number,
    nMfcc: number,
    fmin: number,
    fmax: number,
    htk: boolean,
    lifter: number,
  ) => WasmMfccResult;
  melDelta: (
    features: Float32Array,
    nFeatures: number,
    nFrames: number,
    width: number,
  ) => Float32Array;
  melFilterbank: (
    sampleRate: number,
    nFft: number,
    nMels: number,
    fminHz: number,
    fmaxHz: number,
    htk: boolean,
    norm: number,
  ) => { data: Float32Array; nMels: number; nBins: number };
  melFrequencies: (nMels: number, fminHz: number, fmaxHz: number, htk: boolean) => Float32Array;
  reassignedSpectrogram: (
    samples: Float32Array,
    sampleRate: number,
    nFft: number,
    hopLength: number,
    refPower: number,
    fillNan: boolean,
  ) => {
    nBins: number;
    nFrames: number;
    magnitude: Float32Array;
    times: Float32Array;
    frequencies: Float32Array;
  };
  segmentCrossSimilarity: (
    x: Float32Array,
    xRows: number,
    xCols: number,
    y: Float32Array,
    yRows: number,
    yCols: number,
    k: number | undefined,
    metric: string,
    mode: string,
  ) => WasmSegmentMatrixResult;
  segmentRecurrenceMatrix: (
    data: Float32Array,
    rows: number,
    cols: number,
    k: number | undefined,
    width: number,
    sym: boolean,
    metric: string,
    mode: string,
  ) => WasmSegmentMatrixResult;
  segmentRecurrenceToLag: (
    recurrence: Float32Array,
    n: number,
    pad: boolean,
  ) => WasmSegmentMatrixResult;
  segmentLagToRecurrence: (
    lag: Float32Array,
    rows: number,
    lags: number,
  ) => WasmSegmentMatrixResult;
  segmentSubsegment: (
    data: Float32Array,
    rows: number,
    cols: number,
    boundaries: Int32Array,
    nSegments: number,
  ) => Int32Array;
  segmentAgglomerative: (
    data: Float32Array,
    rows: number,
    cols: number,
    k: number,
    linkage: string,
  ) => Int32Array;
  segmentPathEnhance: (
    recurrence: Float32Array,
    n: number,
    win: number,
    maxRatio: number,
    minRatio: number,
    nFilters: number,
  ) => WasmSegmentMatrixResult;

  // Features - Inverse reconstruction
  melToStft: (
    power: Float32Array,
    nMels: number,
    nFrames: number,
    sampleRate: number,
    nFft: number,
    fmin: number,
    fmax: number,
    htk: boolean,
  ) => WasmStftMagnitudeResult;
  melToAudio: (
    power: Float32Array,
    nMels: number,
    nFrames: number,
    sampleRate: number,
    nFft: number,
    hopLength: number,
    fmin: number,
    fmax: number,
    nIter: number,
    htk: boolean,
  ) => Float32Array;
  griffinLim: (
    magnitude: Float32Array,
    nBins: number,
    nFrames: number,
    sampleRate: number,
    nFft: number,
    hopLength: number,
    nIter: number,
    momentum: number,
  ) => Float32Array;
  mfccToMel: (
    mfcc: Float32Array,
    nMfcc: number,
    nFrames: number,
    nMels: number,
    lifter: number,
  ) => WasmMelPowerResult;
  mfccToAudio: (
    mfcc: Float32Array,
    nMfcc: number,
    nFrames: number,
    nMels: number,
    sampleRate: number,
    nFft: number,
    hopLength: number,
    fmin: number,
    fmax: number,
    nIter: number,
    htk: boolean,
    lifter: number,
  ) => Float32Array;

  // Features - Chroma
  chroma: (
    samples: Float32Array,
    sampleRate: number,
    nFft: number,
    hopLength: number,
  ) => WasmChromaResult;

  // Features - Spectral
  spectralCentroid: (
    samples: Float32Array,
    sampleRate: number,
    nFft: number,
    hopLength: number,
  ) => Float32Array;
  spectralBandwidth: (
    samples: Float32Array,
    sampleRate: number,
    nFft: number,
    hopLength: number,
    p: number,
  ) => Float32Array;
  spectralRolloff: (
    samples: Float32Array,
    sampleRate: number,
    nFft: number,
    hopLength: number,
    rollPercent: number,
  ) => Float32Array;
  spectralFlatness: (
    samples: Float32Array,
    sampleRate: number,
    nFft: number,
    hopLength: number,
  ) => Float32Array;
  spectralFlux: (
    samples: Float32Array,
    sampleRate: number,
    nFft: number,
    hopLength: number,
    lag: number,
  ) => Float32Array;
  zeroCrossingRate: (
    samples: Float32Array,
    sampleRate: number,
    frameLength: number,
    hopLength: number,
  ) => Float32Array;
  rmsEnergy: (
    samples: Float32Array,
    sampleRate: number,
    frameLength: number,
    hopLength: number,
  ) => Float32Array;
  spectralContrast: (
    samples: Float32Array,
    sampleRate: number,
    nFft: number,
    hopLength: number,
    nBands: number,
    fmin: number,
    quantile: number,
  ) => WasmMatrix2dResult;
  polyFeatures: (
    samples: Float32Array,
    sampleRate: number,
    nFft: number,
    hopLength: number,
    order: number,
  ) => WasmMatrix2dResult;
  zeroCrossings: (
    samples: Float32Array,
    threshold: number,
    refMagnitude: boolean,
    pad: boolean,
    zeroPos: boolean,
  ) => Int32Array;
  pitchTuning: (frequencies: Float32Array, resolution: number, binsPerOctave: number) => number;
  tuningToReferenceHz: (tuningCents: number, a4Hz: number) => number;
  referenceHzToTuning: (referenceHz: number, a4Hz: number) => number;
  estimateTuning: (
    samples: Float32Array,
    sampleRate: number,
    nFft: number,
    hopLength: number,
    resolution: number,
    binsPerOctave: number,
  ) => number;

  // Features - Pitch
  pitchYin: (
    samples: Float32Array,
    sampleRate: number,
    frameLength: number,
    hopLength: number,
    fmin: number,
    fmax: number,
    threshold: number,
  ) => WasmPitchResult;
  pitchPyin: (
    samples: Float32Array,
    sampleRate: number,
    frameLength: number,
    hopLength: number,
    fmin: number,
    fmax: number,
    threshold: number,
    fillNa: boolean,
  ) => WasmPitchResult;
  piptrack: (
    samples: Float32Array,
    sampleRate: number,
    nFft: number,
    hopLength: number,
    fmin: number,
    fmax: number,
    threshold: number,
  ) => {
    nBins: number;
    nFrames: number;
    pitches: Float32Array;
    magnitudes: Float32Array;
  };
  noteSegments: (
    f0Hz: Float32Array,
    voicedProb: Float32Array | null,
    voicedFlags: Uint8Array | null,
    frameRate: number,
    options: {
      segmentationThresholdCents?: number;
      minNoteMs?: number;
      referenceHz?: number;
      voicedThreshold?: number;
    },
  ) => WasmNoteSegment[];

  // Core - Conversion
  hzToMel: (hz: number) => number;
  melToHz: (mel: number) => number;
  hzToMidi: (hz: number) => number;
  midiToHz: (midi: number) => number;
  /** Throws a `RangeError` for a frequency that is not finite and positive. */
  hzToNote: (hz: number) => string;
  /** Throws a `RangeError` for a name that does not parse or an octave out of range. */
  noteToHz: (note: string) => number;
  framesToTime: (frames: number, sr: number, hopLength: number) => number;
  timeToFrames: (time: number, sr: number, hopLength: number) => number;
  framesToSamples: (frames: number, hopLength: number, nFft: number) => number;
  samplesToFrames: (samples: number, hopLength: number, nFft: number) => number;
  powerToDb: (values: Float32Array, ref: number, amin: number, topDb: number) => Float32Array;
  amplitudeToDb: (values: Float32Array, ref: number, amin: number, topDb: number) => Float32Array;
  dbToPower: (values: Float32Array, ref: number) => Float32Array;
  dbToAmplitude: (values: Float32Array, ref: number) => Float32Array;
  preemphasis: (samples: Float32Array, coef: number, zi?: number | null) => Float32Array;
  deemphasis: (samples: Float32Array, coef: number, zi?: number | null) => Float32Array;
  trimSilence: (
    samples: Float32Array,
    topDb: number,
    frameLength: number,
    hopLength: number,
  ) => WasmTrimResult;
  splitSilence: (
    samples: Float32Array,
    topDb: number,
    frameLength: number,
    hopLength: number,
  ) => Int32Array;
  splitSilenceCommon: {
    (
      signals: Float32Array[],
      topDb: number,
      frameLength: number,
      hopLength: number,
      withReport?: false | null,
    ): Int32Array;
    (
      signals: Float32Array[],
      topDb: number,
      frameLength: number,
      hopLength: number,
      withReport: true,
    ): SplitSilenceCommonWithReportResult;
  };
  frameSignal: (samples: Float32Array, frameLength: number, hopLength: number) => WasmFrameResult;
  tone: (
    frequency: number,
    sampleRate: number,
    duration: number,
    phase: number,
    amplitude: number,
  ) => Float32Array;
  chirp: (
    fmin: number,
    fmax: number,
    sampleRate: number,
    duration: number,
    linear: boolean,
  ) => Float32Array;
  clicks: (
    times: Float32Array,
    sampleRate: number,
    length: number | undefined,
    frequency: number,
    clickDuration: number,
  ) => Float32Array;
  padCenter: (values: Float32Array, size: number, padValue: number) => Float32Array;
  fixLength: (values: Float32Array, size: number, padValue: number) => Float32Array;
  fixFrames: (frames: Int32Array, xMin: number, xMax: number, pad: boolean) => Int32Array;
  onsetBacktrack: (events: Int32Array, energy: Float32Array) => Int32Array;
  peakPick: (
    values: Float32Array,
    preMax: number,
    postMax: number,
    preAvg: number,
    postAvg: number,
    delta: number,
    wait: number,
  ) => Int32Array;
  vectorNormalize: (values: Float32Array, normType: number, threshold: number) => Float32Array;
  pcen: (
    values: Float32Array,
    nBins: number,
    nFrames: number,
    options?: Record<string, number> | null,
  ) => Float32Array;
  tonnetz: (chromagram: Float32Array, nChroma: number, nFrames: number) => Float32Array;
  tempogram: (
    onsetEnvelope: Float32Array,
    sampleRate: number,
    hopLength: number,
    winLength: number,
    mode: TempogramMode,
    center: boolean,
    norm: boolean,
  ) => WasmTempogramResult;
  cyclicTempogram: (
    onsetEnvelope: Float32Array,
    sampleRate: number,
    hopLength: number,
    winLength: number,
    bpmMin: number | undefined,
    nBins: number | undefined,
  ) => WasmCyclicTempogramResult;
  plp: (
    onsetEnvelope: Float32Array,
    sampleRate: number,
    hopLength: number,
    bpmMin: number | undefined,
    bpmMax: number | undefined,
    winLength: number,
  ) => Float32Array;
  chromaCens: (
    samples: Float32Array,
    sampleRate: number,
    hopLength: number,
    nChroma: number,
    binsPerOctave: number,
  ) => WasmChromaResult;
  chromaCqt: (
    samples: Float32Array,
    sampleRate: number,
    hopLength: number,
    nChroma: number,
    binsPerOctave: number,
  ) => WasmChromaResult;
  bassChroma: (
    samples: Float32Array,
    sampleRate: number,
    hopLength: number,
    nChroma: number,
  ) => WasmChromaResult;
  nnlsChroma: (
    samples: Float32Array,
    sampleRate: number,
    enableStftBlend: boolean,
    stftBlendWeight: number,
    stftBlendNFft: number,
    hopLength?: number | null,
  ) => WasmNnlsChromaResult;
  cqt: (
    samples: Float32Array,
    sampleRate: number,
    hopLength: number,
    fmin: number,
    nBins: number,
    binsPerOctave: number,
    filterScale: number | undefined,
  ) => WasmCqtResult;
  pseudoCqt: (
    samples: Float32Array,
    sampleRate: number,
    hopLength: number,
    fmin: number,
    nBins: number,
    binsPerOctave: number,
    filterScale: number | undefined,
  ) => WasmCqtResult;
  hybridCqt: (
    samples: Float32Array,
    sampleRate: number,
    hopLength: number,
    fmin: number,
    nBins: number,
    binsPerOctave: number,
    filterScale: number | undefined,
  ) => WasmCqtResult;
  vqt: (
    samples: Float32Array,
    sampleRate: number,
    hopLength: number,
    fmin: number,
    nBins: number,
    binsPerOctave: number,
    filterScale: number | undefined,
    gamma: number | undefined,
  ) => WasmCqtResult;
  cqtToAudio: (
    magnitude: Float32Array,
    nBins: number,
    nFrames: number,
    sampleRate: number,
    hopLength: number,
    fmin: number,
    binsPerOctave: number,
    nIter: number,
  ) => Float32Array;
  vqtToAudio: (
    magnitude: Float32Array,
    nBins: number,
    nFrames: number,
    sampleRate: number,
    hopLength: number,
    fmin: number,
    binsPerOctave: number,
    gamma: number | undefined,
    nIter: number,
  ) => Float32Array;
  analyzeSections: (
    samples: Float32Array,
    sampleRate: number,
    nFft: number,
    hopLength: number,
    minSectionSec: number,
    threshold: number | undefined,
  ) => { sections: WasmSectionResult[]; form: string };
  detectBoundaries: (
    samples: Float32Array,
    sampleRate: number,
    options: WasmBoundaryOptions,
  ) => WasmBoundaryResult;
  analyzeMelody: (
    samples: Float32Array,
    sampleRate: number,
    fmin: number,
    fmax: number,
    frameLength: number,
    hopLength: number,
    threshold: number,
    usePyin: boolean,
    center: boolean,
  ) => WasmMelodyResult;
  onsetEnvelope: (
    samples: Float32Array,
    sampleRate: number,
    nFft: number,
    hopLength: number,
    nMels: number,
  ) => Float32Array;
  onsetStrengthMulti: (
    samples: Float32Array,
    sampleRate: number,
    nFft: number,
    hopLength: number,
    nMels: number,
    nBands: number,
  ) => WasmOnsetStrengthMultiResult;
  fourierTempogram: (
    onsetEnvelope: Float32Array,
    sampleRate: number,
    hopLength: number,
    winLength: number,
    center: boolean,
    norm: boolean,
  ) => WasmFourierTempogramResult;
  tempogramRatio: (
    tempogramData: Float32Array,
    winLength: number,
    sampleRate: number,
    hopLength: number,
    factors?: Float32Array | number[],
  ) => Float32Array;
  lufs: (samples: Float32Array, sampleRate: number) => WasmLufsResult;
  momentaryLufs: (samples: Float32Array, sampleRate: number) => Float32Array;
  shortTermLufs: (samples: Float32Array, sampleRate: number) => Float32Array;
  lufsInterleaved: (samples: Float32Array, channels: number, sampleRate: number) => WasmLufsResult;
  lufsSeriesInterleaved: (
    samples: Float32Array,
    channels: number,
    sampleRate: number,
  ) => WasmLufsSeriesResult;
  ebur128LoudnessRangeLu: (samples: Float32Array, sampleRate: number) => number;

  // Core - Resample
  resample: (samples: Float32Array, srcSr: number, targetSr: number) => Float32Array;

  // Embind-exposed enums
  PitchClass: {
    C: { value: 0 };
    Cs: { value: 1 };
    D: { value: 2 };
    Ds: { value: 3 };
    E: { value: 4 };
    F: { value: 5 };
    Fs: { value: 6 };
    G: { value: 7 };
    Gs: { value: 8 };
    A: { value: 9 };
    As: { value: 10 };
    B: { value: 11 };
  };
  Mode: {
    Major: { value: 0 };
    Minor: { value: 1 };
  };
  ChordQuality: {
    Major: { value: 0 };
    Minor: { value: 1 };
    Diminished: { value: 2 };
    Augmented: { value: 3 };
    Dominant7: { value: 4 };
    Major7: { value: 5 };
    Minor7: { value: 6 };
    Sus2: { value: 7 };
    Sus4: { value: 8 };
    Unknown: { value: 9 };
    Add9: { value: 10 };
    MinorAdd9: { value: 11 };
    Dim7: { value: 12 };
    HalfDim7: { value: 13 };
    Major9: { value: 14 };
    Dominant9: { value: 15 };
    Sus2Add4: { value: 16 };
    Major6: { value: 17 };
    Minor6: { value: 18 };
    MinorMajor7: { value: 19 };
    Dominant7Sus4: { value: 20 };
    Dominant11: { value: 21 };
    Dominant13: { value: 22 };
    Dominant7Flat9: { value: 23 };
    Dominant7Sharp9: { value: 24 };
  };
  SectionType: {
    Intro: { value: 0 };
    Verse: { value: 1 };
    PreChorus: { value: 2 };
    Chorus: { value: 3 };
    Bridge: { value: 4 };
    Instrumental: { value: 5 };
    Outro: { value: 6 };
    Unknown: { value: 7 };
  };

  // Streaming - StreamAnalyzer
  /** Every field but the rate may be undefined, which keeps the library default. */
  StreamAnalyzer: new (
    sampleRate: number,
    nFft: number | undefined,
    hopLength: number | undefined,
    nMels: number | undefined,
    fmin: number | undefined,
    fmax: number | undefined,
    referenceHz: number | undefined,
    computeMel: boolean | undefined,
    computeChroma: boolean | undefined,
    computeOnset: boolean | undefined,
    computeSpectral: boolean | undefined,
    emitEveryNFrames: number | undefined,
    maxPendingFrames: number | undefined,
    maxProgressionEntries: number | undefined,
    keyUpdateIntervalSec: number | undefined,
    bpmUpdateIntervalSec: number | undefined,
    window: number | undefined,
  ) => WasmStreamAnalyzer;

  ClipPageProvider: new (
    numChannels: number,
    numSamples: number,
    pageFrames: number,
  ) => WasmClipPageProvider;

  RealtimeEngine: new (
    sampleRate: number,
    maxBlockSize: number | undefined,
    commandCapacity: number,
    telemetryCapacity: number,
    maxChannels?: number,
  ) => WasmRealtimeEngine;

  // Streaming - StreamingMasteringChain
  createStreamingMasteringChain: (
    configJson: string | null,
    options?: { loudnessStaticGainDb?: number; loudnessStaticGainPeakDbtp?: number },
  ) => WasmStreamingMasteringChain;

  // Streaming - StreamingEqualizer
  createEqualizer: (config: Record<string, unknown>) => WasmStreamingEqualizer;

  // Streaming - StreamingRetune
  createStreamingRetune: (config: Record<string, unknown>) => WasmStreamingRetune;
  createRealtimeVoiceChanger: (
    config: Record<string, unknown> | string,
  ) => WasmRealtimeVoiceChanger;
  realtimeVoiceChangerPresetNames: () => string[];
  realtimeVoiceChangerPresetJson: (id: string) => string;
  validateRealtimeVoiceChangerPresetJson: (document: string) => {
    ok: boolean;
    normalizedJson?: string;
    error?: string;
  };

  // Playback - renderer, HRTF set and program loudness meter
  createHrtfSet: (bytes: Uint8Array) => WasmHrtfSet;
  createPlaybackRenderer: (
    configJson: string,
    hrtf: WasmHrtfSet | null,
    sampleRate: number,
    maxBlockSize: number | undefined,
  ) => WasmPlaybackRenderer;
  createPlaybackLoudnessMeter: (channels: number, sampleRate: number) => WasmPlaybackLoudnessMeter;
  renderPlayback: (
    samples: Float32Array,
    channels: number,
    sampleRate: number,
    configJson: string,
    hrtf: WasmHrtfSet | null,
  ) => { samples: Float32Array; channels: number };
  /** Output channel count a renderer built from `configJson` would have, without building one. */
  playbackConfigOutputChannels: (configJson: string) => number;

  // Mixing - scene-based Mixer
  createMixerFromSceneJson: (
    json: string,
    sampleRate: number,
    blockSize: number | undefined,
  ) => WasmMixer;

  // Decodes a thrown native exception-object pointer into a structured error.
  // Consumed by the module-error wrapper in module_state.ts.
  sonareExceptionInfo: (ptr: number) => {
    code: number;
    codeName: string;
    message: string;
    reason?: ErrorReason;
    kind?: 'TypeError' | 'RangeError';
  };

  // Drops the reference __cxa_throw took on the exception object. No C++ frame
  // catches a throw that reaches JS, so the module-error wrapper releases every
  // pointer it surfaces.
  sonareReleaseException: (ptr: number) => void;

  // Bytes the heap can still hand out at most: the linear-memory cap less what is in use.
  // Exact once `needBytes` exceeds the never-grown tail; below that it reports the tail alone.
  sonareHeapHeadroom: (needBytes: number) => number;

  // Native Wasm exception helpers: the C++ tag, and the thrown object's address
  // inside a WebAssembly.Exception carrying it.
  getCppExceptionTag: () => WebAssembly.Tag;
  getCppExceptionThrownObjectFromWebAssemblyException: (error: WebAssembly.Exception) => number;
  renderRefusal: (
    refusal: unknown,
    subjects: string[],
    scales: number[],
  ) => { message: string; refusal: unknown };
}

// The polyphonic editing analysis. `noteCount` and `frameCount` are embind
// properties rather than methods; everything else is a call.
export interface WasmPolyphonicAnalysis {
  readonly noteCount: number;
  readonly frameCount: number;
  notes: () => NoteObject[];
  setNoteEdit: (note: number, edit: NoteEditInput) => void;
  polyphony: () => Int32Array;
  noteF0: (note: number) => Float32Array;
  noteAmplitude: (note: number) => Float32Array;
  noteSalience: (note: number) => Float32Array;
  noteInharmonicity: () => Float32Array;
  noteEnvelope: (note: number) => Float32Array;
  render: (options: Record<string, unknown>) => Float32Array;
  delete: () => void;
}

export interface WasmStreamingMasteringChain {
  /** An undefined `maxBlockSize` selects the library default. */
  prepare: (sampleRate: number, maxBlockSize: number | undefined, numChannels: number) => void;
  setParameter: (key: string, value: number) => void;
  processMono: (samples: Float32Array) => Float32Array;
  processStereo: (
    left: Float32Array,
    right: Float32Array,
  ) => { left: Float32Array; right: Float32Array };
  flushMono: () => Float32Array;
  flushStereo: () => { left: Float32Array; right: Float32Array };
  reset: () => void;
  latencySamples: () => number;
  maxBlockSize: () => number;
  stageNames: () => string[];
  /** Cumulative since prepare(), which rebuilds the stages and so clears it. */
  nonFiniteSubstitutionCount: () => number;
  /** Not the same measurement as nonFiniteSubstitutionCount; see the facade. */
  nonFiniteDiscardCount: () => number;
  delete: () => void;
}

export interface WasmEqSpectrumSnapshot {
  preLeft: Float32Array;
  preRight: Float32Array;
  postLeft: Float32Array;
  postRight: Float32Array;
  bandGainDb: Float32Array;
  profileDb: Float32Array;
  lastAutoGainDb: number;
  seq: number;
}

export interface WasmStreamingEqualizer {
  setBand: (index: number, band: Record<string, unknown>) => void;
  clear: () => void;
  setPhaseMode: (mode: number) => void;
  setAutoGain: (enabled: boolean) => void;
  setGainScale: (scale: number) => void;
  setOutputGainDb: (gainDb: number) => void;
  setOutputPan: (pan: number) => void;
  setSidechainMono: (samples: Float32Array) => void;
  setSidechainStereo: (left: Float32Array, right: Float32Array) => void;
  clearSidechain: () => void;
  lastAutoGainDb: () => number;
  latencySamples: () => number;
  maxBlockSize: () => number;
  nonFiniteDiscardCount: () => number;
  processMono: (samples: Float32Array) => Float32Array;
  processStereo: (
    left: Float32Array,
    right: Float32Array,
  ) => { left: Float32Array; right: Float32Array };
  magnitudeResponse: (placement: number, frequenciesHz: Float32Array) => Float32Array;
  spectrum: () => WasmEqSpectrumSnapshot;
  match: (source: Float32Array, reference: Float32Array, options: Record<string, unknown>) => void;
  delete: () => void;
}

export interface WasmStreamingRetune {
  prepare: (sampleRate: number, maxBlockSize: number) => void;
  reset: () => void;
  setConfig: (config: Record<string, unknown>) => void;
  config: () => { pitchSemitones: number; mix: number; grainSizeSamples: number };
  grainSizeSamples: () => number;
  latencySamples: () => number;
  processMono: (samples: Float32Array) => Float32Array;
  delete: () => void;
}

export interface WasmRealtimeVoiceChanger {
  /** An undefined `maxBlockSize` selects the library default. */
  prepare: (sampleRate: number, maxBlockSize: number | undefined, channels: number) => void;
  reset: () => void;
  setConfig: (config: Record<string, unknown> | string) => void;
  setPodConfig: (config: WasmRealtimeVoiceChangerPodConfig) => void;
  config: () => WasmRealtimeVoiceChangerPodConfig;
  configJson: () => string;
  latencySamples: () => number;
  maxBlockSize: () => number;
  nonFiniteDiscardCount: () => number;
  bufferGeneration: () => number;
  // `carrier` is a mono external vocoder carrier as long as the input (frames), or undefined.
  processMono: (samples: Float32Array, carrier: Float32Array | undefined) => Float32Array;
  processMonoInto: (
    samples: Float32Array,
    carrier: Float32Array | undefined,
    output: Float32Array,
  ) => void;
  processInterleaved: (
    samples: Float32Array,
    carrier: Float32Array | undefined,
    channels: number,
  ) => Float32Array;
  processInterleavedInto: (
    samples: Float32Array,
    carrier: Float32Array | undefined,
    channels: number,
    output: Float32Array,
  ) => void;
  noteOn: (note: number, velocity: number) => void;
  noteOff: (note: number) => void;
  allNotesOff: () => void;
  getCarrierBuffer: (numFrames: number) => Float32Array;
  // Zero-copy "prepared" path. The returned Float32Array is a typed_memory_view
  // backed by the WASM heap; it is only valid until the next call to a method
  // that may grow the underlying scratch buffer (i.e. another get*Buffer with a
  // larger length) or until delete().
  getMonoInputBuffer: (numSamples: number) => Float32Array;
  getMonoOutputBuffer: (numSamples: number) => Float32Array;
  /** `withCarrier` true reads the carrier view from getCarrierBuffer; undefined is none. */
  processPreparedMono: (numSamples: number, withCarrier: boolean | undefined) => void;
  getInterleavedInputBuffer: (numFrames: number, numChannels: number) => Float32Array;
  getInterleavedOutputBuffer: (numFrames: number, numChannels: number) => Float32Array;
  processPreparedInterleaved: (
    numFrames: number,
    numChannels: number,
    withCarrier: boolean | undefined,
  ) => void;
  getPlanarChannelBuffer: (channel: number, numFrames: number) => Float32Array;
  processPreparedPlanar: (numFrames: number, withCarrier: boolean | undefined) => void;
  delete: () => void;
}

export interface WasmHrtfSet {
  delete: () => void;
}

export interface WasmPlaybackRenderer {
  processPlanar: (planes: Float32Array[]) => Float32Array[];
  processInterleaved: (samples: Float32Array, inChannels: number) => Float32Array;
  processSilence: (frames: number) => Float32Array[];
  renderOffline: (samples: Float32Array, inChannels: number) => Float32Array;
  setConfig: (configJson: string) => void;
  configJson: () => string;
  setHeadOrientation: (yawDeg: number, pitchDeg: number, rollDeg: number) => void;
  reset: () => void;
  latencySamples: () => number;
  maxBlockSize: () => number;
  inputChannels: () => number;
  outputChannels: () => number;
  /** Widest input block the active input layout accepts (8 under "auto"). */
  maxInputChannels: () => number;
  diagnosticsJson: () => string;
  nonFiniteDiscardCount: () => number;
  // AudioWorklet path: heap views allocated once at construction (they detach
  // on memory growth and are then re-acquired), and a SonareError code instead
  // of a throw for a rejected block.
  inputPlane: (channel: number) => Float32Array;
  outputPlane: (channel: number) => Float32Array;
  processPrepared: (inChannels: number, frames: number) => number;
  processPreparedSilence: (frames: number) => number;
  delete: () => void;
}

export interface WasmPlaybackLoudnessMeter {
  pushInterleaved: (samples: Float32Array) => void;
  integratedLufs: () => number;
  delete: () => void;
}

export interface WasmMixer {
  compile: () => void;
  processStereo: (
    leftChannels: Float32Array[],
    rightChannels: Float32Array[],
  ) => { left: Float32Array; right: Float32Array; sampleRate: number };
  processStereoInto: (
    leftChannels: Float32Array[],
    rightChannels: Float32Array[],
    outLeft: Float32Array,
    outRight: Float32Array,
  ) => void;
  inputLeftView: (index: number) => Float32Array;
  inputRightView: (index: number) => Float32Array;
  outputLeftView: () => Float32Array;
  outputRightView: () => Float32Array;
  processPreparedStereo: (numSamples: number) => void;
  configureMeter: (
    enabled: boolean,
    truePeakOversample?: number,
    measureLufs?: boolean,
    measureTruePeak?: boolean,
  ) => void;
  meterSnapshot: () => WasmMixMeterSnapshot;
  latchMeterSnapshot: () => boolean;
  meterScratchValue: (field: number) => number;
  stripCount: () => number;
  sceneWarnings: () => string[];
  scheduleInsertAutomation: (
    stripIndex: number,
    insertIndex: number,
    paramId: number,
    samplePos: number,
    value: number,
    curve: number,
  ) => void;
  stripById: (id: string) => number;
  setInputTrimDb: (stripIndex: number, db: number) => void;
  setFaderDb: (stripIndex: number, db: number) => void;
  setPan: (stripIndex: number, pan: number, panMode: number) => void;
  setWidth: (stripIndex: number, width: number) => void;
  settle: (stripIndex: number) => void;
  setMuted: (stripIndex: number, muted: boolean) => void;
  setSoloed: (stripIndex: number, soloed: boolean) => void;
  setSoloSafe: (stripIndex: number, soloSafe: boolean) => void;
  setPolarityInvert: (stripIndex: number, invertLeft: boolean, invertRight: boolean) => void;
  setPanLaw: (stripIndex: number, panLaw: number) => void;
  setChannelDelaySamples: (stripIndex: number, delaySamples: number) => void;
  setVcaOffsetDb: (stripIndex: number, offsetDb: number) => void;
  setDualPan: (stripIndex: number, leftPan: number, rightPan: number) => void;
  setSurroundPan: (stripIndex: number, pan: SurroundPan) => void;
  addSend: (
    stripIndex: number,
    id: string,
    destinationBusId: string,
    sendDb: number,
    timing: number,
  ) => number;
  setSendDb: (stripIndex: number, sendIndex: number, sendDb: number) => void;
  removeSend: (stripIndex: number, sendIndex: number) => void;
  stripMeter: (stripIndex: number, tap: number) => WasmMixMeterSnapshot;
  busMeter: (busId: string) => WasmMixMeterSnapshot;
  stripNonFiniteDiscardCount: (stripIndex: number) => number;
  busNonFiniteDiscardCount: (busId: string) => number;
  scheduleFaderAutomation: (
    stripIndex: number,
    samplePos: number,
    faderDb: number,
    curve: number,
  ) => void;
  schedulePanAutomation: (
    stripIndex: number,
    samplePos: number,
    pan: number,
    curve: number,
  ) => void;
  scheduleWidthAutomation: (
    stripIndex: number,
    samplePos: number,
    width: number,
    curve: number,
  ) => void;
  scheduleSendAutomation: (
    stripIndex: number,
    sendIndex: number,
    samplePos: number,
    db: number,
    curve: number,
  ) => void;
  readGoniometerLatest: (stripIndex: number, maxPoints: number) => WasmGoniometerPoint[];
  addStrip: (id: string, metering: StripMeteringOptions) => void;
  addBus: (id: string, role: string) => void;
  removeBus: (id: string) => void;
  setOutputBus: (sourceId: string, busId: string) => void;
  busCount: () => number;
  addVcaGroup: (id: string, gainDb: number, members: string[]) => void;
  setVcaGroupGainDb: (id: string, gainDb: number) => void;
  setVcaGroupMembers: (id: string, members: string[]) => void;
  removeVcaGroup: (id: string) => void;
  vcaGroupCount: () => number;
  toSceneJson: () => string;
  tailSamples: () => number;
  latencySamples: () => number;
  maxBlockSize: () => number;
  drainTailStereo: (numSamples: number) => {
    left: Float32Array;
    right: Float32Array;
    sampleRate: number;
  };
  flushMeters: () => void;
  delete: () => void;
}

export interface WasmGoniometerPoint {
  left: number;
  right: number;
}

// Streaming types for StreamAnalyzer
export interface WasmChordChange {
  root: number;
  quality: number;
  name: string;
  startSec: number;
  confidence: number;
}

export interface WasmBarChord {
  barIndex: number;
  root: number;
  quality: number;
  name: string;
  startSec: number;
  confidence: number;
}

export interface WasmPatternScore {
  name: string;
  score: number;
}

export interface WasmProgressiveEstimate {
  bpm: number;
  bpmConfidence: number;
  bpmCandidateCount: number;
  key: number;
  keyMinor: boolean;
  keyConfidence: number;
  keyName: string | null;
  keyShortName: string | null;
  chordRoot: number;
  chordQuality: number;
  chordName: string;
  chordConfidence: number;
  chordStartSec: number;
  chordProgression: WasmChordChange[];
  barChordProgression: WasmBarChord[];
  currentBar: number;
  barDurationSec: number;
  votedPattern: WasmBarChord[];
  patternLength: number;
  detectedPatternName: string;
  detectedPatternScore: number;
  allPatternScores: WasmPatternScore[];
  accumulatedSec: number;
  usedFrames: number;
  updated: boolean;
}

export interface WasmAnalyzerStats {
  totalFrames: number;
  totalSamples: number;
  durationSec: number;
  pendingFrames: number;
  droppedOutputFrames: number;
  droppedChordProgressionEntries: number;
  droppedBarProgressionEntries: number;
  nonFiniteDiscardBlocks: number;
  estimate: WasmProgressiveEstimate;
}

export interface WasmFrameBuffer {
  nFrames: number;
  /** Number of mel bands; flat `mel` is `[nFrames * nMels]` row-major. */
  nMels: number;
  nChroma: number;
  featureFlags: number;
  timestampsSec: Float32Array;
  mel: Float32Array;
  chroma: Float32Array;
  onsetStrength: Float32Array;
  rmsEnergy: Float32Array;
  spectralCentroid: Float32Array;
  spectralFlatness: Float32Array;
  chordRoot: Int32Array;
  chordQuality: Int32Array;
  chordConfidence: Float32Array;
}

export interface WasmStreamFramesU8 {
  nFrames: number;
  nMels: number;
  nChroma: number;
  featureFlags: number;
  timestampsSec: Float32Array;
  mel: Uint8Array;
  chroma: Uint8Array;
  onsetStrength: Uint8Array;
  rmsEnergy: Uint8Array;
  spectralCentroid: Uint8Array;
  spectralFlatness: Uint8Array;
  chordRoot: Int8Array;
  chordQuality: Int8Array;
  chordConfidence: Uint8Array;
}

export interface WasmStreamFramesI16 {
  nFrames: number;
  nMels: number;
  nChroma: number;
  featureFlags: number;
  timestampsSec: Float32Array;
  mel: Int16Array;
  chroma: Int16Array;
  onsetStrength: Int16Array;
  rmsEnergy: Int16Array;
  spectralCentroid: Int16Array;
  spectralFlatness: Int16Array;
  chordRoot: Int8Array;
  chordQuality: Int8Array;
  chordConfidence: Int16Array;
}

export interface WasmStreamAnalyzer {
  /** Throws (invalid state) when the stream was already finalized; reset first. */
  process: (samples: Float32Array, sampleOffset: number | undefined) => void;
  /** Idempotent on success; a failed call leaves the stream un-finalized. */
  finalize: () => void;
  availableFrames: () => number;
  readFramesSoa: (maxFrames: number) => WasmFrameBuffer;
  readFramesU8: (maxFrames: number, quantizeConfig?: unknown) => WasmStreamFramesU8;
  readFramesI16: (maxFrames: number, quantizeConfig?: unknown) => WasmStreamFramesI16;
  reset: (baseSampleOffset?: number) => void;
  stats: () => WasmAnalyzerStats;
  frameCount: () => number;
  currentTimeSec: () => number;
  sampleRate: () => number;
  setExpectedDuration: (durationSec: number) => void;
  /** Throws outside 0.01..100; the request is refused, never clamped. */
  setNormalizationGain: (gain: number) => void;
  /** Throws outside 220..880 Hz; the request is refused, never clamped. */
  setReferenceHz: (referenceHz: number) => void;
  delete: () => void;
}

declare function createModule(options?: SonareModuleOptions): Promise<SonareModule>;

export default createModule;
