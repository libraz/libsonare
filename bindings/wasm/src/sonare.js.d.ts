/**
 * Hand-written type declarations describing the Emscripten/embind WASM module.
 *
 * This file is NOT generated — nothing in the build writes it. A new embind
 * registration reaches TypeScript only when someone declares it here, in the
 * same change as the binding, so a missing declaration is a lockstep omission
 * rather than a stale artifact waiting on a rebuild.
 */

import type {
  ProgressCallback,
  SonareCapabilities,
  SpectralRegionOp,
  SurroundPan,
} from './public_types';

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

export interface WasmKeyCandidateResult {
  key: WasmKeyResult;
  correlation: number;
}

// Flat POD mirror of SonareRealtimeVoiceChangerConfig. Keys are camelCase to
// match the Node addon getter (the underlying C ABI / Python POD is snake_case).
export interface WasmRealtimeVoiceChangerPodConfig {
  inputGainDb: number;
  outputGainDb: number;
  wetMix: number;
  retuneSemitones: number;
  retuneMix: number;
  retuneGrainSize: number;
  formantFactor: number;
  formantAmount: number;
  formantBody: number;
  formantBrightness: number;
  formantNasal: number;
  eqHighpassHz: number;
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
}

export interface WasmBeatResult {
  time: number;
  /**
   * Onset-envelope value sampled at the beat's frame — a single raw frame, not
   * a normalized or relative salience. Use
   * `WasmBeatObservationsResult.onsetStrength` for accent scoring.
   */
  strength: number;
}

/** Beat-level evidence behind the downbeat and meter decisions. */
export interface WasmBeatObservationsResult {
  onsetStrength: number[];
  lowFrequencyEnergy: number[];
  chordChange: number[];
}

export interface WasmMeterEstimateResult {
  timeSignature: WasmTimeSignatureResult;
  downbeatPhase: number;
  grouping: number[];
  candidateScores: number[];
  candidates: WasmTimeSignatureResult[];
}

export interface WasmChordResult {
  root: number;
  bass: number;
  quality: number;
  start: number;
  end: number;
  confidence: number;
  name: string;
}

export interface WasmSectionResult {
  type: number;
  start: number;
  end: number;
  energyLevel: number;
  confidence: number;
  name: string;
}

export interface WasmTimbreResult {
  brightness: number;
  warmth: number;
  density: number;
  roughness: number;
  complexity: number;
}

export interface WasmDynamicsResult {
  dynamicRangeDb: number;
  peakDb: number;
  rmsDb: number;
  loudnessRangeDb: number;
  crestFactor: number;
  isCompressed: boolean;
}

export interface WasmRhythmResult {
  syncopation: number;
  grooveType: string;
  patternRegularity: number;
  tempoStability: number;
  timeSignature: WasmTimeSignatureResult;
}

export interface WasmTimeSignatureResult {
  numerator: number;
  denominator: number;
  confidence: number;
}

export interface WasmMelodyContourResult {
  pitchRangeOctaves: number;
  pitchStability: number;
  meanFrequency: number;
  vibratoRate: number;
  pitches: WasmMelodyPoint[];
}

export interface WasmAnalysisResult {
  bpm: number;
  bpmConfidence: number;
  bpmCandidates: WasmBpmHypothesis[];
  key: WasmKeyResult;
  timeSignature: WasmTimeSignatureResult;
  timeSignatureCandidates: WasmTimeSignatureResult[];
  beats: WasmBeatResult[];
  downbeatIndices: number[];
  downbeatPhase: number;
  beatObservations: WasmBeatObservationsResult;
  beatLocalBpm: number[];
  chords: WasmChordResult[];
  sections: WasmSectionResult[];
  timbre: WasmTimbreResult;
  dynamics: WasmDynamicsResult;
  rhythm: WasmRhythmResult;
  melody: WasmMelodyContourResult;
  form: string;
}

export interface WasmBpmHypothesis {
  value: number;
  confidence: number;
  relation: 'primary' | 'half' | 'double' | 'other';
}

export interface WasmBpmCandidate {
  bpm: number;
  confidence: number;
}

export interface WasmBpmAnalysisResult {
  bpm: number;
  confidence: number;
  candidates: WasmBpmCandidate[];
  autocorrelation: Float32Array;
  tempogram: Float32Array;
}

export interface WasmRhythmAnalysisResult {
  timeSignature: WasmTimeSignatureResult;
  syncopation: number;
  grooveType: string;
  patternRegularity: number;
  tempoStability: number;
  bpm: number;
  beatIntervals: Float32Array;
}

export interface WasmDynamicsAnalysisResult {
  dynamicRangeDb: number;
  peakDb: number;
  rmsDb: number;
  crestFactor: number;
  loudnessRangeDb: number;
  isCompressed: boolean;
  loudnessTimes: Float32Array;
  loudnessRmsDb: Float32Array;
}

export interface WasmStreamConfigDefaults {
  sampleRate: number;
  nFft: number;
  hopLength: number;
  nMels: number;
  fmin: number;
  fmax: number;
  tuningRefHz: number;
  computeMagnitude: boolean;
  computeMel: boolean;
  computeChroma: boolean;
  computeOnset: boolean;
  computeSpectral: boolean;
  emitEveryNFrames: number;
  magnitudeDownsample: number;
  maxPendingFrames: number;
  maxProgressionEntries: number;
  keyUpdateIntervalSec: number;
  bpmUpdateIntervalSec: number;
  window: number;
  outputFormat: number;
}

export interface WasmTimbreFrameResult {
  brightness: number;
  warmth: number;
  density: number;
  roughness: number;
  complexity: number;
}

export interface WasmTimbreAnalysisResult {
  brightness: number;
  warmth: number;
  density: number;
  roughness: number;
  complexity: number;
  spectralCentroid: Float32Array;
  spectralFlatness: Float32Array;
  spectralRolloff: Float32Array;
  timbreOverTime: WasmTimbreFrameResult[];
}

export interface WasmChordAnalysisResult {
  chords: WasmChordResult[];
}

export interface WasmAcousticResult {
  rt60: number;
  edt: number;
  c50: number;
  c80: number;
  d50: number;
  rt60Bands: Float32Array;
  edtBands: Float32Array;
  c50Bands: Float32Array;
  c80Bands: Float32Array;
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
  sourceX?: number;
  sourceY?: number;
  sourceZ?: number;
  listenerX?: number;
  listenerY?: number;
  listenerZ?: number;
  ismOrder?: number;
  seed?: number;
  maxSeconds?: number;
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

export interface WasmRirResult {
  rir: Float32Array;
  sampleRate: number;
  hasError: boolean;
  errorMessage: string;
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
  volume: number;
  length: number;
  width: number;
  height: number;
  drrDb: number;
  confidence: number;
  absorptionBands: Float32Array;
  rt60Bands: Float32Array;
}

export interface WasmRoomMorphOptions extends WasmRoomGeometryOptions {
  wet?: number;
  sourceTailSuppression?: number;
}

export interface WasmHpssResult {
  harmonic: Float32Array;
  percussive: Float32Array;
  sampleRate: number;
}

export interface WasmAudioFromMemoryResult {
  samples: Float32Array;
  sampleRate: number;
}

export interface WasmHpssWithResidualResult {
  harmonic: Float32Array;
  percussive: Float32Array;
  residual: Float32Array;
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
}

export interface WasmStftDbResult {
  nBins: number;
  nFrames: number;
  db: Float32Array;
}

export interface WasmMelResult {
  nMels: number;
  nFrames: number;
  sampleRate: number;
  hopLength: number;
  power: Float32Array;
  db: Float32Array;
}

export interface WasmMfccResult {
  nMfcc: number;
  nFrames: number;
  coefficients: Float32Array;
}

export interface WasmStftPowerResult {
  nBins: number;
  nFrames: number;
  power: Float32Array;
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
  medianF0: number;
  meanF0: number;
}

export interface WasmNoteSegment {
  frameStart: number;
  frameEnd: number;
  startSeconds: number;
  endSeconds: number;
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

export interface WasmMelodyResult {
  points: WasmMelodyPoint[];
  pitchRangeOctaves: number;
  pitchStability: number;
  meanFrequency: number;
  vibratoRate: number;
}

export interface WasmCqtResult {
  nBins: number;
  nFrames: number;
  hopLength: number;
  sampleRate: number;
  magnitude: Float32Array;
  frequencies: Float32Array;
}

export interface WasmLufsResult {
  integratedLufs: number;
  momentaryLufs: number;
  shortTermLufs: number;
  maxMomentaryLufs: number;
  maxShortTermLufs: number;
  loudnessRange: number;
}

export interface WasmMasteringResult {
  samples: Float32Array;
  sampleRate: number;
  inputLufs: number;
  outputLufs: number;
  appliedGainDb: number;
  loudnessTargetLimited?: boolean;
  latencySamples?: number;
}

export interface WasmStageGainReduction {
  stage: string;
  gainReductionDb: number;
}

export interface WasmMasteringLoudnessSummary {
  integratedLufs: number;
  maxMomentaryLufs: number;
  maxShortTermLufs: number;
  truePeakDbtp: number;
  loudnessRange: number;
}

export interface WasmMasteringReport {
  before: WasmMasteringLoudnessSummary;
  after: WasmMasteringLoudnessSummary;
  appliedGainDb: number;
  maxGainReductionDb: number;
  loudnessTargetLimited: boolean;
  bandEnergyDeltaDb: Float32Array;
}

export interface WasmMasteringChainResult extends WasmMasteringResult {
  stages: string[];
  outputTruePeakDbtp: number;
  outputLra: number;
  loudnessTargetLimited: boolean;
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
  outputLra: number;
  loudnessTargetLimited: boolean;
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
}

export interface WasmMixMeterSnapshot {
  peakDbL: number;
  peakDbR: number;
  rmsDbL: number;
  rmsDbR: number;
  correlation: number;
  monoCompatWidth: number;
  monoCompatPeak: number;
  monoCompatSideRms: number;
  likelyMonoCompatible: boolean;
  momentaryLufs: number;
  shortTermLufs: number;
  integratedLufs: number;
  gainReductionDb: number;
  /**
   * Left-channel inter-sample (true) peak in dB, from the ITU-R BS.1770-4
   * polyphase reconstruction at 4x. A streaming measurement: the centered
   * reconstruction stencil needs a few future samples a realtime path does not
   * have, so each block's last samples read marginally low (about 0.1 dB across
   * 64..8192-sample blocks on a near-Nyquist tone, always under-reading). Use
   * `meteringTruePeakDb` over the whole signal for an exact dBTP number.
   */
  truePeakDbL: number;
  /** Right-channel inter-sample (true) peak in dB. See {@link truePeakDbL}. */
  truePeakDbR: number;
  /** Maximum inter-sample peak across channels in dB. See {@link truePeakDbL}. */
  maxTruePeakDb: number;
  seq: number;
  channelCount: number;
  peakDb: number[];
  rmsDb: number[];
  /** Per-plane inter-sample peak in dB; [0]/[1] mirror {@link truePeakDbL}/R. */
  truePeakDb: number[];
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
  warpMode?: 'off' | 'repitch' | 'tempo-sync' | 'time-stretch' | 0 | 1 | 2 | 3;
  warpAnchors?: Array<{ warpSample: number; sourceSample: number }>;
  pageProvider?: number | { readonly id: number };
}

export interface WasmEngineTrackSend {
  busId: number;
  levelDb?: number;
  enabled?: boolean;
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
  channelCount: number;
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
   * Mixing intent (`0` = replace, `1` = add).
   *
   * NOTE: not currently honored — the compiled graph always sums edges into a
   * shared destination port in an order-independent way (the first edge into a
   * port overwrites, every later edge adds), regardless of this value.
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
  peakDbL: number;
  peakDbR: number;
  rmsDbL: number;
  rmsDbR: number;
  /**
   * Left-channel inter-sample (true) peak in dB, from the ITU-R BS.1770-4
   * polyphase reconstruction at 4x. A streaming measurement: the centered
   * reconstruction stencil needs a few future samples a realtime path does not
   * have, so each block's last samples read marginally low (about 0.1 dB across
   * 64..8192-sample blocks on a near-Nyquist tone, always under-reading). Use
   * `meteringTruePeakDb` over the whole signal for an exact dBTP number.
   */
  truePeakDbL: number;
  /** Right-channel inter-sample (true) peak in dB. See {@link truePeakDbL}. */
  truePeakDbR: number;
  /** Maximum inter-sample peak across channels in dB. See {@link truePeakDbL}. */
  maxTruePeakDb: number;
  correlation: number;
  monoCompatWidth: number;
  momentaryLufs: number;
  shortTermLufs: number;
  integratedLufs: number;
  gainReductionDb: number;
  droppedRecords: number;
}

export interface WasmEngineMeterTelemetryWide {
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
   * Per-plane inter-sample (true) peak in dB (length `channelCount`), from the
   * ITU-R BS.1770-4 polyphase reconstruction at 4x. A streaming measurement: the
   * centered reconstruction stencil needs a few future samples a realtime path
   * does not have, so each block's last samples read marginally low (about
   * 0.1 dB across 64..8192-sample blocks on a near-Nyquist tone, always
   * under-reading). Use `meteringTruePeakDb` over the whole signal for an exact
   * dBTP number.
   */
  truePeakDb: number[];
  /** Maximum inter-sample peak across channels in dB. See {@link truePeakDb}. */
  maxTruePeakDb: number;
  correlation: number;
  monoCompatWidth: number;
  momentaryLufs: number;
  shortTermLufs: number;
  integratedLufs: number;
  gainReductionDb: number;
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
  overflowCount: number;
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
  barCount: number;
  /** Time signature in effect at the current PPQ. */
  timeSignature: { numerator: number; denominator: number; confidence: number };
  /** One-based beat within the current bar (`barCount` is zero-based). */
  beat: number;
  /** Fractional position within the current beat, in [0, 1). */
  beatFraction: number;
}

export interface WasmEngineBounceOptions {
  totalFrames: number;
  blockSize?: number;
  numChannels?: number;
  targetSampleRate?: number;
  sourceSampleRate?: number;
  normalizeLufs?: boolean;
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
  /** Sample position within the producing block at which the event fires. */
  renderFrame: number;
  /** MIDI 1.0 status + data bytes (1..3 entries). */
  bytes: number[];
}

export interface WasmRealtimeEngine {
  prepare: (
    sampleRate: number,
    maxBlockSize: number,
    commandCapacity: number,
    telemetryCapacity: number,
  ) => void;
  setParameter: (paramId: number, value: number, renderFrame: number) => void;
  setParameterSmoothed: (paramId: number, value: number, renderFrame: number) => void;
  setParamSmoothingMs: (smoothingMs: number) => void;
  setSoloMute: (laneIndex: number, solo: boolean, mute: boolean, renderFrame: number) => void;
  setTrackMonitorMode: (laneIndex: number, mode: number, renderFrame: number) => void;
  getTransportState: () => WasmEngineTransportState;
  play: (renderFrame: number) => void;
  stop: (renderFrame: number) => void;
  seekSample: (timelineSample: number, renderFrame: number) => void;
  settleParameters: () => void;
  flushControlCommands: () => void;
  seekPpq: (ppq: number, renderFrame: number) => void;
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
  seekMarker: (id: number, renderFrame: number) => void;
  setLoopFromMarkers: (startMarkerId: number, endMarkerId: number) => void;
  setMetronome: (config: WasmEngineMetronomeConfig) => void;
  metronome: () => Required<WasmEngineMetronomeConfig>;
  countInEndSample: (startSample: number, bars: number) => number;
  setGraph: (spec: WasmEngineGraphSpec) => void;
  graphNodeCount: () => number;
  graphConnectionCount: () => number;
  setClips: (clips: WasmEngineClip[]) => void;
  prebakedClipChannels: (clipId: number) => Float32Array[] | null;
  clipCount: () => number;
  setTrackLanes: (lanes: Array<number | WasmEngineTrackLane>) => void;
  setLaneSidechain: (trackId: number, insertIndex: number, sourceTrackId: number) => void;
  setTrackBuses: (buses: WasmEngineBus[]) => void;
  setBusStripJson: (busId: number, sceneJson: string) => void;
  setTrackStripJson: (trackId: number, sceneJson: string) => void;
  setTrackStripEqBandJson: (trackId: number, bandIndex: number, bandJson: string) => void;
  setTrackStripInsertBypassed: (
    trackId: number,
    insertIndex: number,
    bypassed: boolean,
    resetOnBypass: boolean,
  ) => void;
  setMasterStripJson: (sceneJson: string) => void;
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
  setMasterStripInsertParamByName: (insertIndex: number, paramName: string, value: number) => void;
  setBusStripInsertParamByName: (
    busId: number,
    insertIndex: number,
    paramName: string,
    value: number,
  ) => void;
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
  resolveMasterInsertAutomationId: (insertIndex: number, paramName: string) => number;
  resolveBusInsertAutomationId: (busId: number, insertIndex: number, paramName: string) => number;
  resolveInstrumentAutomationId: (destinationId: number, paramName: string) => number;
  setTrackStripPan: (trackId: number, pan: number) => void;
  setTrackStripPanLaw: (trackId: number, panLaw: number) => void;
  setTrackStripPanMode: (trackId: number, panMode: number) => void;
  setTrackStripDualPan: (trackId: number, leftPan: number, rightPan: number) => void;
  setTrackStripChannelDelaySamples: (trackId: number, delaySamples: number) => void;
  createClipPageProvider: (numChannels: number, numSamples: number, pageFrames: number) => number;
  supplyClipPage: (providerId: number, pageIndex: number, channels: Float32Array[]) => void;
  clearClipPage: (providerId: number, pageIndex: number) => void;
  destroyClipPageProvider: (providerId: number) => void;
  popClipPageRequest: () => WasmClipPageRequest | null;
  popClipPageRequestToScratch: () => boolean;
  clipPageRequestScratchClipId: () => number;
  clipPageRequestScratchSample: () => number;
  clipPageRequestOverflowCount: () => number;
  setClipPagePrefetchFrames: (frames: number) => void;
  clipPagePrefetchFrames: () => number;
  setCaptureBuffer: (numChannels: number, capacityFrames: number) => void;
  armCapture: (armed: boolean) => void;
  setCapturePunch: (startSample: number, endSample: number, enabled: boolean) => void;
  setCaptureSource: (source: WasmEngineCaptureStatus['source'] | number) => void;
  setRecordOffsetSamples: (offsetSamples: number) => void;
  setInputMonitor: (enabled: boolean, gain: number) => void;
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
  bindMidiCc: (
    channel: number,
    controller: number,
    paramId: number,
    minValue: number,
    maxValue: number,
  ) => void;
  bindMidiCcBinding: (binding: import('./project_types').ProjectMidiCcBinding) => void;
  clearMidiCcBindings: () => void;
  midiCcBindingCount: () => number;
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
    portTimeSamples: number,
  ) => void;
  pushMidiInputNoteOff: (
    group: number,
    channel: number,
    note: number,
    velocity: number,
    portTimeSamples: number,
  ) => void;
  pushMidiInputCc: (
    group: number,
    channel: number,
    controller: number,
    value: number,
    portTimeSamples: number,
  ) => void;
  pushMidiNoteOn: (
    destinationId: number,
    group: number,
    channel: number,
    note: number,
    velocity: number,
    renderFrame: number,
  ) => void;
  pushMidiNoteOff: (
    destinationId: number,
    group: number,
    channel: number,
    note: number,
    velocity: number,
    renderFrame: number,
  ) => void;
  pushMidiCc: (
    destinationId: number,
    group: number,
    channel: number,
    controller: number,
    value: number,
    renderFrame: number,
  ) => void;
  pushMidiUmp: (destinationId: number, word0: number, renderFrame: number) => void;
  pushMidiSysex: (destinationId: number, data: Uint8Array, renderFrame: number) => void;
  pushMidiPanic: (renderFrame: number) => void;
  setMidiDestinationExternal: (destinationId: number, external: boolean) => void;
  setExternalMidiClockEnabled: (enabled: boolean) => void;
  externalMidiDroppedCount: () => number;
  externalMidiPendingCount: () => number;
  drainExternalMidi: (maxRecords: number) => WasmExternalMidiEvent[];
  popExternalMidiToScratch: () => boolean;
  externalMidiScratchDestinationId: () => number;
  externalMidiScratchRenderFrame: () => number;
  externalMidiScratchByteWord: () => number;
  externalMidiScratchByteCount: () => number;
  consumeExternalMidiScratch: () => void;
  clearParameters: () => void;
  process: (channels: Float32Array[]) => Float32Array[];
  prepareWithChannels: (
    sampleRate: number,
    maxBlockSize: number,
    commandCapacity: number,
    telemetryCapacity: number,
    maxChannels: number,
  ) => void;
  prepareChannels: (numChannels: number, maxFrames: number) => void;
  getChannelBuffer: (channel: number, numFrames: number) => Float32Array;
  processPrepared: (numFrames: number) => void;
  prepareMonitorChannels: (numChannels: number, maxFrames: number) => void;
  getMonitorChannelBuffer: (channel: number, numFrames: number) => Float32Array;
  processPreparedWithMonitor: (numFrames: number) => void;
  processWithMonitor: (channels: Float32Array[]) => WasmEngineProcessWithMonitorResult;
  renderOffline: (channels: Float32Array[], blockSize: number, finalize: boolean) => Float32Array[];
  finishOfflineRender: () => void;
  bounceOffline: (options: WasmEngineBounceOptions) => WasmEngineBounceResult;
  freezeOffline: (options: WasmEngineFreezeOptions) => WasmEngineFreezeResult;
  drainTelemetry: (maxRecords: number) => WasmEngineTelemetry[];
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
  drainMeterTelemetry: (maxRecords: number) => WasmEngineMeterTelemetry[];
  drainMeterTelemetryWide: (maxRecords: number) => WasmEngineMeterTelemetryWide[];
  configureScopeTelemetry: (intervalFrames: number, bandCount: number) => number;
  drainScopeTelemetry: (maxRecords: number) => WasmEngineScopeTelemetry[];
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

export type { ProgressCallback } from './public_types';
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
}

export interface SonareModule {
  audioFromMemory: (bytes: Uint8Array) => WasmAudioFromMemoryResult;

  // Quick API (high-level)
  detectBpm: (samples: Float32Array, sampleRate: number) => number;
  detectKey: (samples: Float32Array, sampleRate: number) => WasmKeyResult;
  _detectKeyWithOptions: (
    samples: Float32Array,
    sampleRate: number,
    nFft: number,
    hopLength: number,
    useHpss: boolean,
    loudnessWeighted: boolean,
    highPassHz: number,
    modes: number[],
    profileType: number,
    genreHint: string,
  ) => WasmKeyResult;
  _detectKeyCandidates: (
    samples: Float32Array,
    sampleRate: number,
    nFft: number,
    hopLength: number,
    useHpss: boolean,
    loudnessWeighted: boolean,
    highPassHz: number,
    modes: number[],
    profileType: number,
    genreHint: string,
  ) => WasmKeyCandidateResult[];
  detectOnsets: (samples: Float32Array, sampleRate: number, options: object) => Float32Array;
  detectBeats: (samples: Float32Array, sampleRate: number) => Float32Array;
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
  ) => string[];
  analyze: (samples: Float32Array, sampleRate: number, options: object) => WasmAnalysisResult;
  estimateMeter: (
    beatTimes: ArrayLike<number>,
    beatStrengths: ArrayLike<number>,
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
  synthPresetNames: () => string[];
  synthPresetPatch: (name: string) => unknown;
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
    events: readonly unknown[],
    paramId: number,
    minValue: number,
    maxValue: number,
    minMovement: number,
  ) => unknown;
  midiCcToBreakpoint: (bindings: readonly unknown[], event: unknown) => unknown;
  midiParamToCc: (
    bindings: readonly unknown[],
    paramId: number,
    unitValue: number,
    group: number,
    ppq: number,
  ) => unknown;
  _analysisResultSchemaPaths: () => string[];
  _analysisResultSchemaFixture: () => WasmAnalysisResult;
  analyzeImpulseResponse: (
    samples: Float32Array,
    sampleRate: number,
    nOctaveBands: number,
  ) => WasmAcousticResult;
  analyzeImpulseResponseEx: (
    samples: Float32Array,
    sampleRate: number,
    nOctaveBands: number,
    minDecayDb: number,
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
  ) => Float32Array;
  analyzeWithProgress: (
    samples: Float32Array,
    sampleRate: number,
    progressCallback: ProgressCallback | null,
    cancelCallback: (() => boolean) | null,
  ) => WasmAnalysisResult;
  analyzeBpm: (
    samples: Float32Array,
    sampleRate: number,
    bpmMin: number,
    bpmMax: number,
    startBpm: number,
    nFft: number,
    hopLength: number,
    maxCandidates: number,
  ) => WasmBpmAnalysisResult;
  analyzeRhythm: (
    samples: Float32Array,
    sampleRate: number,
    bpmMin: number,
    bpmMax: number,
    startBpm: number,
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
  hasFfmpegSupport: () => boolean;
  version: () => string;
  capabilities: () => SonareCapabilities;
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
    threshold: number,
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
    dynamicRangeDb: number;
    lowPercentileDb: number;
    highPercentileDb: number;
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
  ) => { mid: Float32Array; side: Float32Array };
  meteringVectorscopeDecimated: (
    left: Float32Array,
    right: Float32Array,
    sampleRate: number,
    maxPoints: number,
  ) => { mid: Float32Array; side: Float32Array };
  meteringPhaseScope: (
    left: Float32Array,
    right: Float32Array,
    sampleRate: number,
  ) => {
    mid: Float32Array;
    side: Float32Array;
    radius: Float32Array;
    angleRad: Float32Array;
    correlation: number;
    averageAbsAngleRad: number;
    maxRadius: number;
  };
  meteringPhaseScopeDecimated: (
    left: Float32Array,
    right: Float32Array,
    sampleRate: number,
    maxPoints: number,
  ) => {
    mid: Float32Array;
    side: Float32Array;
    radius: Float32Array;
    angleRad: Float32Array;
    correlation: number;
    averageAbsAngleRad: number;
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

  scaleQuantizeMidi: (
    root: number,
    modeMask: number,
    midi: number,
    referenceMidi: number,
  ) => number;
  scaleCorrectionSemitones: (
    root: number,
    modeMask: number,
    midi: number,
    referenceMidi: number,
  ) => number;
  scalePitchClassEnabled: (root: number, modeMask: number, pitchClass: number) => boolean;

  // Effects
  hpss: (
    samples: Float32Array,
    sampleRate: number,
    kernelHarmonic: number,
    kernelPercussive: number,
  ) => WasmHpssResult;
  hpssEx: (
    samples: Float32Array,
    sampleRate: number,
    kernelHarmonic: number,
    kernelPercussive: number,
    nFft: number,
    hopLength: number,
    hardMask: boolean,
  ) => WasmHpssResult;
  hpssWithResidual: (
    samples: Float32Array,
    sampleRate: number,
    kernelHarmonic: number,
    kernelPercussive: number,
  ) => WasmHpssWithResidualResult;
  hpssWithResidualEx: (
    samples: Float32Array,
    sampleRate: number,
    kernelHarmonic: number,
    kernelPercussive: number,
    nFft: number,
    hopLength: number,
    hardMask: boolean,
  ) => WasmHpssWithResidualResult;
  decompose: (
    s: Float32Array,
    nFeatures: number,
    nFrames: number,
    nComponents: number,
    nIter: number,
    beta: number,
  ) => WasmDecomposeResult;
  decomposeWithInit: (
    s: Float32Array,
    nFeatures: number,
    nFrames: number,
    nComponents: number,
    nIter: number,
    beta: number,
    init: string,
  ) => WasmDecomposeResult;
  nnFilter: (
    s: Float32Array,
    nFeatures: number,
    nFrames: number,
    aggregate: string,
    k: number,
    width: number,
  ) => WasmMatrix2dResult;
  remix: (
    samples: Float32Array,
    intervals: Int32Array,
    sampleRate: number,
    alignZeros: boolean,
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
  remixAlignedIntervals: (
    samples: Float32Array,
    intervals: Int32Array,
    sampleRate: number,
    alignZeros: boolean,
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
  timeStretch: (samples: Float32Array, sampleRate: number, rate: number) => Float32Array;
  timeStretchEx: (
    samples: Float32Array,
    sampleRate: number,
    rate: number,
    nFft: number,
    hopLength: number,
  ) => Float32Array;
  pitchShift: (samples: Float32Array, sampleRate: number, semitones: number) => Float32Array;
  pitchShiftEx: (
    samples: Float32Array,
    sampleRate: number,
    semitones: number,
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
      mode?: string;
      targetMidi?: number;
      scaleRoot?: number;
      scaleModeMask?: number;
      referenceMidi?: number;
      retuneAmount?: number;
      maxCorrectionSemitones?: number;
      retuneSpeedMs?: number;
      vibratoThresholdCents?: number;
      voiced?: Float32Array;
      voicedProb?: Float32Array;
    },
  ) => Float32Array;
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
  voiceChange: (
    samples: Float32Array,
    sampleRate: number,
    pitchSemitones: number,
    formantFactor: number,
  ) => Float32Array;
  voiceChangeRealtime: (
    samples: Float32Array,
    sampleRate: number,
    preset: string,
    channels: number,
  ) => Float32Array;
  normalize: (samples: Float32Array, sampleRate: number, targetDb: number) => Float32Array;
  normalizeEx: (
    samples: Float32Array,
    sampleRate: number,
    targetDb: number,
    mode: string,
  ) => Float32Array;
  mastering: (
    samples: Float32Array,
    sampleRate: number,
    targetLufs: number,
    ceilingDb: number,
    truePeakOversample: number,
    releaseMs: number,
    applyGainAtInputRate: boolean,
  ) => WasmMasteringResult;
  masteringProcessorNames: () => string[];
  masteringPairProcessorNames: () => string[];
  masteringPairAnalysisNames: () => string[];
  masteringStereoAnalysisNames: () => string[];
  masteringInsertNames: () => string[];
  masteringInsertParamNames: (name: string) => string[];
  // Both return JSON the facade parses; the raw module has no object shape here.
  masteringInsertParamInfo: (name: string) => string;
  masteringProcessorCatalog: () => string;
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
    params: Record<string, number | boolean>,
  ) => WasmMasteringResult;
  masteringPairAnalyze: (
    analysisName: string,
    source: Float32Array,
    reference: Float32Array,
    sampleRate: number,
    params: Record<string, number | boolean>,
  ) => string;
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
    platforms: Array<{ name: string; targetLufs: number; ceilingDb: number }>,
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
    platforms: Array<{ name: string; targetLufs: number; ceilingDb: number }>,
  ) => string;
  masteringRepairDeclick: (
    samples: Float32Array,
    sampleRate: number,
    options: object,
  ) => Float32Array;
  masteringRepairDenoiseClassical: (
    samples: Float32Array,
    sampleRate: number,
    options: object,
  ) => Float32Array;
  masteringRepairDeclip: (
    samples: Float32Array,
    sampleRate: number,
    options: object,
  ) => Float32Array;
  masteringRepairDecrackle: (
    samples: Float32Array,
    sampleRate: number,
    options: object,
  ) => Float32Array;
  masteringRepairDehum: (
    samples: Float32Array,
    sampleRate: number,
    options: object,
  ) => Float32Array;
  masteringRepairDereverbClassical: (
    samples: Float32Array,
    sampleRate: number,
    options: object,
  ) => Float32Array;
  masteringRepairTrimSilence: (
    samples: Float32Array,
    sampleRate: number,
    options: object,
  ) => Float32Array;
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
    config: Record<string, unknown>,
  ) => WasmMasteringChainResult;
  masteringChainStereo: (
    left: Float32Array,
    right: Float32Array,
    sampleRate: number,
    config: Record<string, unknown>,
  ) => WasmMasteringStereoChainResult;
  masteringChainWithProgress: (
    samples: Float32Array,
    sampleRate: number,
    config: Record<string, unknown>,
    progressCallback: ProgressCallback | null,
    cancelCallback: (() => boolean) | null,
  ) => WasmMasteringChainResult;
  masteringChainStereoWithProgress: (
    left: Float32Array,
    right: Float32Array,
    sampleRate: number,
    config: Record<string, unknown>,
    progressCallback: ProgressCallback | null,
    cancelCallback: (() => boolean) | null,
  ) => WasmMasteringStereoChainResult;
  masteringPresetNames: () => string[];
  masteringPlatformNames: () => string[];
  masterAudio: (
    presetName: string,
    samples: Float32Array,
    sampleRate: number,
    overrides: Record<string, number | boolean> | null,
  ) => WasmMasteringChainResult;
  masterAudioStereo: (
    presetName: string,
    left: Float32Array,
    right: Float32Array,
    sampleRate: number,
    overrides: Record<string, number | boolean> | null,
  ) => WasmMasteringStereoChainResult;
  masterAudioWithProgress: (
    presetName: string,
    samples: Float32Array,
    sampleRate: number,
    overrides: Record<string, number | boolean> | null,
    progressCallback: ProgressCallback | null,
    cancelCallback: (() => boolean) | null,
  ) => WasmMasteringChainResult;
  masterAudioStereoWithProgress: (
    presetName: string,
    left: Float32Array,
    right: Float32Array,
    sampleRate: number,
    overrides: Record<string, number | boolean> | null,
    progressCallback: ProgressCallback | null,
    cancelCallback: (() => boolean) | null,
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
    sampleRate: number,
    params: Record<string, number | boolean>,
  ) => string;
  mixingAssistantSuggestSceneJson: (
    leftChannels: Float32Array[],
    rightChannels: (Float32Array | null)[] | null,
    trackIds: string[],
    trackNames: (string | null)[] | null,
    sampleRate: number,
    params: Record<string, number | boolean>,
  ) => string;
  mixingAssistantSourceClassNames: () => string[];
  mixingAssistantSourceClassFromName: (name: string) => number;
  trim: (samples: Float32Array, sampleRate: number, thresholdDb: number) => Float32Array;
  trimEx: (
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
  stft: (
    samples: Float32Array,
    sampleRate: number,
    nFft: number,
    hopLength: number,
  ) => WasmStftResult;
  stftDb: (
    samples: Float32Array,
    sampleRate: number,
    nFft: number,
    hopLength: number,
  ) => WasmStftDbResult;

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
    k: number,
    metric: string,
    mode: string,
  ) => WasmSegmentMatrixResult;
  segmentRecurrenceMatrix: (
    data: Float32Array,
    rows: number,
    cols: number,
    k: number,
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
    melPower: Float32Array,
    nMels: number,
    nFrames: number,
    sampleRate: number,
    nFft: number,
    fmin: number,
    fmax: number,
    htk: boolean,
  ) => WasmStftPowerResult;
  melToAudio: (
    melPower: Float32Array,
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
    fillNa: boolean,
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
    voicedProb: Float32Array,
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
  hzToNote: (hz: number) => string;
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
    length: number,
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
    bpmMin: number,
    nBins: number,
  ) => WasmCyclicTempogramResult;
  plp: (
    onsetEnvelope: Float32Array,
    sampleRate: number,
    hopLength: number,
    tempoMin: number,
    tempoMax: number,
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
  ) => WasmNnlsChromaResult;
  nnlsChromaEx: (
    samples: Float32Array,
    sampleRate: number,
    enableStftBlend: boolean,
    stftBlendWeight: number,
    stftBlendNFft: number,
    hopLength: number,
  ) => WasmNnlsChromaResult;
  cqt: (
    samples: Float32Array,
    sampleRate: number,
    hopLength: number,
    fmin: number,
    nBins: number,
    binsPerOctave: number,
  ) => WasmCqtResult;
  pseudoCqt: (
    samples: Float32Array,
    sampleRate: number,
    hopLength: number,
    fmin: number,
    nBins: number,
    binsPerOctave: number,
  ) => WasmCqtResult;
  hybridCqt: (
    samples: Float32Array,
    sampleRate: number,
    hopLength: number,
    fmin: number,
    nBins: number,
    binsPerOctave: number,
  ) => WasmCqtResult;
  vqt: (
    samples: Float32Array,
    sampleRate: number,
    hopLength: number,
    fmin: number,
    nBins: number,
    binsPerOctave: number,
    gamma: number,
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
    gamma: number,
    nIter: number,
  ) => Float32Array;
  analyzeSections: (
    samples: Float32Array,
    sampleRate: number,
    nFft: number,
    hopLength: number,
    minSectionSec: number,
  ) => WasmSectionResult[];
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
  ebur128LoudnessRange: (samples: Float32Array, sampleRate: number) => number;

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
  streamAnalyzerConfigDefault: () => WasmStreamConfigDefaults;
  StreamAnalyzer: new (
    sampleRate: number,
    nFft: number,
    hopLength: number,
    nMels: number,
    fmin: number,
    fmax: number,
    tuningRefHz: number,
    computeMagnitude: boolean,
    computeMel: boolean,
    computeChroma: boolean,
    computeOnset: boolean,
    computeSpectral: boolean,
    emitEveryNFrames: number,
    magnitudeDownsample: number,
    maxPendingFrames: number,
    maxProgressionEntries: number,
    keyUpdateIntervalSec: number,
    bpmUpdateIntervalSec: number,
    window: number,
    outputFormat: number,
  ) => WasmStreamAnalyzer;

  RealtimeEngine: new (
    sampleRate: number,
    maxBlockSize: number,
    commandCapacity: number,
    telemetryCapacity: number,
    maxChannels?: number,
  ) => WasmRealtimeEngine;

  // Streaming - StreamingMasteringChain
  createStreamingMasteringChain: (config: Record<string, unknown>) => WasmStreamingMasteringChain;

  // Streaming - StreamingEqualizer
  createEqualizer: (config: Record<string, unknown>) => WasmStreamingEqualizer;

  // Streaming - StreamingRetune
  createStreamingRetune: (config: Record<string, unknown>) => WasmStreamingRetune;
  createRealtimeVoiceChanger: (
    config: Record<string, unknown> | string,
  ) => WasmRealtimeVoiceChanger;
  realtimeVoiceChangerPresetNames: () => string[];
  realtimeVoiceChangerPresetJson: (id: string) => string;
  validateRealtimeVoiceChangerPresetJson: (json: string) => {
    ok: boolean;
    normalizedJson?: string;
    error?: string;
  };

  // Mixing - scene-based Mixer
  createMixerFromSceneJson: (json: string, sampleRate: number, blockSize: number) => WasmMixer;

  // Decodes a thrown native exception-object pointer (emscripten classic EH
  // surfaces a C++ throw as the raw pointer number) into a structured error.
  // Consumed by the module-error wrapper in module_state.ts.
  sonareExceptionInfo: (ptr: number) => { code: number; codeName: string; message: string };

  // Drops the reference emscripten's __cxa_throw took on the exception object
  // before rethrowing its pointer into JS. Nothing else ever drops it, so the
  // module-error wrapper releases every pointer it surfaces.
  sonareReleaseException: (ptr: number) => void;
}

export interface WasmStreamingMasteringChain {
  prepare: (sampleRate: number, maxBlockSize: number, numChannels: number) => void;
  processMono: (samples: Float32Array) => Float32Array;
  processStereo: (
    left: Float32Array,
    right: Float32Array,
  ) => { left: Float32Array; right: Float32Array };
  flushMono: () => Float32Array;
  flushStereo: () => { left: Float32Array; right: Float32Array };
  reset: () => void;
  latencySamples: () => number;
  stageNames: () => string[];
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
  processMono: (samples: Float32Array) => Float32Array;
  processStereo: (
    left: Float32Array,
    right: Float32Array,
  ) => { left: Float32Array; right: Float32Array };
  spectrum: () => WasmEqSpectrumSnapshot;
  match: (source: Float32Array, reference: Float32Array, options: Record<string, unknown>) => void;
  delete: () => void;
}

export interface WasmStreamingRetune {
  prepare: (sampleRate: number, maxBlockSize: number) => void;
  reset: () => void;
  setConfig: (config: Record<string, unknown>) => void;
  config: () => { semitones: number; mix: number; grainSize: number };
  grainSize: () => number;
  latencySamples: () => number;
  processMono: (samples: Float32Array) => Float32Array;
  delete: () => void;
}

export interface WasmRealtimeVoiceChanger {
  prepare: (sampleRate: number, maxBlockSize: number, channels: number) => void;
  reset: () => void;
  setConfig: (config: Record<string, unknown> | string) => void;
  setPodConfig: (config: WasmRealtimeVoiceChangerPodConfig) => void;
  configJson: () => string;
  latencySamples: () => number;
  bufferGeneration: () => number;
  processMono: (samples: Float32Array) => Float32Array;
  processMonoInto: (samples: Float32Array, output: Float32Array) => void;
  processInterleaved: (samples: Float32Array, channels: number) => Float32Array;
  processInterleavedInto: (samples: Float32Array, channels: number, output: Float32Array) => void;
  // Zero-copy "prepared" path. The returned Float32Array is a typed_memory_view
  // backed by the WASM heap; it is only valid until the next call to a method
  // that may grow the underlying scratch buffer (i.e. another get*Buffer with a
  // larger length) or until delete().
  getMonoInputBuffer: (numSamples: number) => Float32Array;
  getMonoOutputBuffer: (numSamples: number) => Float32Array;
  processPreparedMono: (numSamples: number) => void;
  getInterleavedInputBuffer: (numFrames: number, numChannels: number) => Float32Array;
  getInterleavedOutputBuffer: (numFrames: number, numChannels: number) => Float32Array;
  processPreparedInterleaved: (numFrames: number, numChannels: number) => void;
  getPlanarChannelBuffer: (channel: number, numFrames: number) => Float32Array;
  processPreparedPlanar: (numFrames: number) => void;
  delete: () => void;
}

/** One block's worth of meter readings, all dB fields floored at -120. */
export interface WasmMixerMeterSnapshot {
  peakDbL: number;
  peakDbR: number;
  rmsDbL: number;
  rmsDbR: number;
  correlation: number;
  /**
   * Left-channel inter-sample (true) peak in dB, from the ITU-R BS.1770-4
   * polyphase reconstruction at 4x. A streaming measurement: the centered
   * reconstruction stencil needs a few future samples a realtime path does not
   * have, so each block's last samples read marginally low (about 0.1 dB across
   * 64..8192-sample blocks on a near-Nyquist tone, always under-reading). Use
   * `meteringTruePeakDb` over the whole signal for an exact dBTP number.
   */
  truePeakDbL: number;
  /** Right-channel inter-sample (true) peak in dB. See {@link truePeakDbL}. */
  truePeakDbR: number;
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
  configureMeter: (enabled: boolean, truePeakOversample: number) => void;
  meterSnapshot: () => WasmMixerMeterSnapshot;
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
  meterTap: (stripIndex: number, tap: number) => WasmMixMeterSnapshot;
  stripMeter: (stripIndex: number) => WasmMixMeterSnapshot;
  busMeter: (busId: string) => WasmMixMeterSnapshot;
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
  addBus: (id: string, role: string) => void;
  removeBus: (id: string) => void;
  busCount: () => number;
  addVcaGroup: (id: string, gainDb: number, members: string[]) => void;
  setVcaGroupGainDb: (id: string, gainDb: number) => void;
  setVcaGroupMembers: (id: string, members: string[]) => void;
  removeVcaGroup: (id: string) => void;
  vcaGroupCount: () => number;
  toSceneJson: () => string;
  tailSamples: () => number;
  latencySamples: () => number;
  drainTailStereo: (numSamples: number) => {
    left: Float32Array;
    right: Float32Array;
    sampleRate: number;
  };
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
  startTime: number;
  confidence: number;
}

export interface WasmBarChord {
  barIndex: number;
  root: number;
  quality: number;
  startTime: number;
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
  chordRoot: number;
  chordQuality: number;
  chordConfidence: number;
  chordStartTime: number;
  chordProgression: WasmChordChange[];
  barChordProgression: WasmBarChord[];
  currentBar: number;
  barDuration: number;
  votedPattern: WasmBarChord[];
  patternLength: number;
  detectedPatternName: string;
  detectedPatternScore: number;
  allPatternScores: WasmPatternScore[];
  accumulatedSeconds: number;
  usedFrames: number;
  updated: boolean;
}

export interface WasmAnalyzerStats {
  totalFrames: number;
  totalSamples: number;
  durationSeconds: number;
  pendingFrames: number;
  droppedOutputFrames: number;
  droppedChordProgressionEntries: number;
  droppedBarProgressionEntries: number;
  estimate: WasmProgressiveEstimate;
}

export interface WasmFrameBuffer {
  nFrames: number;
  /** Number of mel bands; flat `mel` is `[nFrames * nMels]` row-major. */
  nMels: number;
  nChroma: number;
  featureFlags: number;
  timestamps: Float32Array;
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
  timestamps: Float32Array;
  mel: Uint8Array;
  chroma: Uint8Array;
  onsetStrength: Uint8Array;
  rmsEnergy: Uint8Array;
  spectralCentroid: Uint8Array;
  spectralFlatness: Uint8Array;
}

export interface WasmStreamFramesI16 {
  nFrames: number;
  nMels: number;
  nChroma: number;
  featureFlags: number;
  timestamps: Float32Array;
  mel: Int16Array;
  chroma: Int16Array;
  onsetStrength: Int16Array;
  rmsEnergy: Int16Array;
  spectralCentroid: Int16Array;
  spectralFlatness: Int16Array;
}

export interface WasmStreamAnalyzer {
  /** Throws (invalid state) when the stream was already finalized; reset first. */
  process: (samples: Float32Array) => void;
  /** Rejects a gap, a seek, a mode switch, and a chunk fed after finalize. */
  processWithOffset: (samples: Float32Array, sampleOffset: number) => void;
  /** Idempotent on success; a failed call leaves the stream un-finalized. */
  finalize: () => void;
  availableFrames: () => number;
  readFramesSoa: (maxFrames: number) => WasmFrameBuffer;
  readFramesU8: (maxFrames: number, quantizeConfig?: unknown) => WasmStreamFramesU8;
  readFramesI16: (maxFrames: number, quantizeConfig?: unknown) => WasmStreamFramesI16;
  reset: (baseSampleOffset?: number) => void;
  stats: () => WasmAnalyzerStats;
  frameCount: () => number;
  currentTime: () => number;
  sampleRate: () => number;
  setExpectedDuration: (durationSeconds: number) => void;
  /** Throws outside 0.01..100; the request is refused, never clamped. */
  setNormalizationGain: (gain: number) => void;
  /** Throws outside 220..880 Hz; the request is refused, never clamped. */
  setTuningRefHz: (refHz: number) => void;
  delete: () => void;
}

declare function createModule(options?: SonareModuleOptions): Promise<SonareModule>;

export default createModule;
