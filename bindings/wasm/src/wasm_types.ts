export type ProgressCallback = (progress: number, stage: string) => void;

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

export interface WasmBeatResult {
  time: number;
  strength: number;
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
  loudnessRangeDb: number;
  crestFactor: number;
  isCompressed: boolean;
}

export interface WasmRhythmResult {
  syncopation: number;
  grooveType: string;
  patternRegularity: number;
}

export interface WasmTimeSignatureResult {
  numerator: number;
  denominator: number;
  confidence: number;
}

export interface WasmAnalysisResult {
  bpm: number;
  bpmConfidence: number;
  key: WasmKeyResult;
  timeSignature: WasmTimeSignatureResult;
  beats: WasmBeatResult[];
  chords: WasmChordResult[];
  sections: WasmSectionResult[];
  timbre: WasmTimbreResult;
  dynamics: WasmDynamicsResult;
  rhythm: WasmRhythmResult;
  form: string;
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

export interface WasmHpssResult {
  harmonic: Float32Array;
  percussive: Float32Array;
  sampleRate: number;
}

export interface WasmMasteringResult {
  samples: Float32Array;
  sampleRate: number;
  inputLufs: number;
  outputLufs: number;
  appliedGainDb: number;
  latencySamples?: number;
}

export interface WasmMasteringChainResult extends WasmMasteringResult {
  stages: string[];
}

export interface WasmMasteringStereoResult {
  left: Float32Array;
  right: Float32Array;
  sampleRate: number;
  inputLufs: number;
  outputLufs: number;
  appliedGainDb: number;
  latencySamples: number;
}

export interface WasmMasteringStereoChainResult {
  left: Float32Array;
  right: Float32Array;
  sampleRate: number;
  inputLufs: number;
  outputLufs: number;
  appliedGainDb: number;
  stages: string[];
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
  truePeakDbL: number;
  truePeakDbR: number;
  maxTruePeakDb: number;
  seq: number;
}

export interface WasmMixResult {
  left: Float32Array;
  right: Float32Array;
  sampleRate: number;
  meters: WasmMixMeterSnapshot[];
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
  loudnessRange: number;
}

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
  estimate: WasmProgressiveEstimate;
}

export interface WasmFrameBuffer {
  nFrames: number;
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
  timestamps: Float32Array;
  mel: Int16Array;
  chroma: Int16Array;
  onsetStrength: Int16Array;
  rmsEnergy: Int16Array;
  spectralCentroid: Int16Array;
  spectralFlatness: Int16Array;
}

export interface WasmStreamingMasteringChain {
  prepare: (sampleRate: number, maxBlockSize: number, numChannels: number) => void;
  processMono: (samples: Float32Array) => Float32Array;
  processStereo: (
    left: Float32Array,
    right: Float32Array,
  ) => { left: Float32Array; right: Float32Array };
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

export interface WasmEngineClip {
  id?: number;
  channels: Float32Array[];
  startPpq: number;
  lengthSamples?: number;
  clipOffsetSamples?: number;
  loop?: boolean;
  gain?: number;
  fadeInSamples?: number;
  fadeOutSamples?: number;
}

export interface WasmEngineParameterInfo {
  id: number;
  name: string;
  unit: string;
  minValue: number;
  maxValue: number;
  defaultValue: number;
  rtSafe: boolean;
  defaultCurve: number;
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
}

export interface WasmEngineMetronomeConfig {
  enabled: boolean;
  beatGain?: number;
  accentGain?: number;
  clickSamples?: number;
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
  targetId: number;
  renderFrame: number;
  seq: number;
  peakDbL: number;
  peakDbR: number;
  rmsDbL: number;
  rmsDbR: number;
  truePeakDbL: number;
  truePeakDbR: number;
  maxTruePeakDb: number;
  correlation: number;
  monoCompatWidth: number;
  momentaryLufs: number;
  shortTermLufs: number;
  integratedLufs: number;
  gainReductionDb: number;
  droppedRecords: number;
}

export interface WasmEngineCaptureStatus {
  capturedFrames: number;
  overflowCount: number;
  armed: boolean;
  punchEnabled: boolean;
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

export interface WasmRealtimeEngine {
  prepare: (
    sampleRate: number,
    maxBlockSize: number,
    commandCapacity: number,
    telemetryCapacity: number,
  ) => void;
  setParameter: (paramId: number, value: number, renderFrame: number) => void;
  setParameterSmoothed: (paramId: number, value: number, renderFrame: number) => void;
  getTransportState: () => WasmEngineTransportState;
  play: (renderFrame: number) => void;
  stop: (renderFrame: number) => void;
  seekSample: (timelineSample: number, renderFrame: number) => void;
  seekPpq: (ppq: number, renderFrame: number) => void;
  setTempo: (bpm: number) => void;
  setTimeSignature: (numerator: number, denominator: number) => void;
  setLoop: (startPpq: number, endPpq: number, enabled: boolean) => void;
  addParameter: (info: WasmEngineParameterInfo) => void;
  parameterCount: () => number;
  parameterInfoByIndex: (index: number) => WasmEngineParameterInfo;
  parameterInfo: (id: number) => WasmEngineParameterInfo;
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
  clipCount: () => number;
  setCaptureBuffer: (numChannels: number, capacityFrames: number) => void;
  armCapture: (armed: boolean) => void;
  setCapturePunch: (startSample: number, endSample: number, enabled: boolean) => void;
  resetCapture: () => void;
  captureStatus: () => WasmEngineCaptureStatus;
  capturedAudio: () => Float32Array[];
  process: (channels: Float32Array[]) => Float32Array[];
  renderOffline: (channels: Float32Array[], blockSize: number) => Float32Array[];
  bounceOffline: (options: WasmEngineBounceOptions) => WasmEngineBounceResult;
  freezeOffline: (options: WasmEngineFreezeOptions) => WasmEngineFreezeResult;
  drainTelemetry: (maxRecords: number) => WasmEngineTelemetry[];
  drainMeterTelemetry: (maxRecords: number) => WasmEngineMeterTelemetry[];
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
  stripCount: () => number;
  scheduleInsertAutomation: (
    stripIndex: number,
    insertIndex: number,
    paramId: number,
    samplePos: number,
    value: number,
    curve: number,
  ) => void;
  stripById: (id: string) => number;
  setSoloed: (stripIndex: number, soloed: boolean) => void;
  setSoloSafe: (stripIndex: number, soloSafe: boolean) => void;
  setPolarityInvert: (stripIndex: number, invertLeft: boolean, invertRight: boolean) => void;
  setPanLaw: (stripIndex: number, panLaw: number) => void;
  setChannelDelaySamples: (stripIndex: number, delaySamples: number) => void;
  setVcaOffsetDb: (stripIndex: number, offsetDb: number) => void;
  setDualPan: (stripIndex: number, leftPan: number, rightPan: number) => void;
  addSend: (
    stripIndex: number,
    id: string,
    destinationBusId: string,
    sendDb: number,
    timing: number,
  ) => number;
  setSendDb: (stripIndex: number, sendIndex: number, sendDb: number) => void;
  meterTap: (stripIndex: number, tap: number) => WasmMixMeterSnapshot;
  stripMeter: (stripIndex: number, tap: number) => WasmMixMeterSnapshot;
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
  removeVcaGroup: (id: string) => void;
  vcaGroupCount: () => number;
  toSceneJson: () => string;
  delete: () => void;
}

export interface WasmGoniometerPoint {
  left: number;
  right: number;
}

export interface WasmStreamAnalyzer {
  process: (samples: Float32Array) => void;
  processWithOffset: (samples: Float32Array, sampleOffset: number) => void;
  availableFrames: () => number;
  readFramesSoa: (maxFrames: number) => WasmFrameBuffer;
  readFramesU8: (maxFrames: number) => WasmStreamFramesU8;
  readFramesI16: (maxFrames: number) => WasmStreamFramesI16;
  reset: (baseSampleOffset?: number) => void;
  stats: () => WasmAnalyzerStats;
  frameCount: () => number;
  currentTime: () => number;
  sampleRate: () => number;
  setExpectedDuration: (durationSeconds: number) => void;
  setNormalizationGain: (gain: number) => void;
  setTuningRefHz: (refHz: number) => void;
  delete: () => void;
}

export interface SonareModule {
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
  detectOnsets: (samples: Float32Array, sampleRate: number) => Float32Array;
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
  analyze: (samples: Float32Array, sampleRate: number) => WasmAnalysisResult;
  analyzeImpulseResponse: (
    samples: Float32Array,
    sampleRate: number,
    nOctaveBands: number,
  ) => WasmAcousticResult;
  detectAcoustic: (
    samples: Float32Array,
    sampleRate: number,
    nOctaveBands: number,
    nThirdOctaveSubbands: number,
    minDecayDb: number,
    noiseFloorMarginDb: number,
  ) => WasmAcousticResult;
  analyzeWithProgress: (
    samples: Float32Array,
    sampleRate: number,
    progressCallback: ProgressCallback | null,
  ) => WasmAnalysisResult;
  version: () => string;
  engineAbiVersion: () => number;

  hpss: (
    samples: Float32Array,
    sampleRate: number,
    kernelHarmonic: number,
    kernelPercussive: number,
  ) => WasmHpssResult;
  harmonic: (samples: Float32Array, sampleRate: number) => Float32Array;
  percussive: (samples: Float32Array, sampleRate: number) => Float32Array;
  timeStretch: (samples: Float32Array, sampleRate: number, rate: number) => Float32Array;
  pitchShift: (samples: Float32Array, sampleRate: number, semitones: number) => Float32Array;
  pitchCorrectToMidi: (
    samples: Float32Array,
    sampleRate: number,
    currentMidi: number,
    targetMidi: number,
  ) => Float32Array;
  noteStretch: (
    samples: Float32Array,
    sampleRate: number,
    onsetSample: number,
    offsetSample: number,
    stretchRatio: number,
  ) => Float32Array;
  voiceChange: (
    samples: Float32Array,
    sampleRate: number,
    pitchSemitones: number,
    formantFactor: number,
  ) => Float32Array;
  normalize: (samples: Float32Array, sampleRate: number, targetDb: number) => Float32Array;
  mastering: (
    samples: Float32Array,
    sampleRate: number,
    targetLufs: number,
    ceilingDb: number,
    truePeakOversample: number,
  ) => WasmMasteringResult;
  masteringProcessorNames: () => string[];
  masteringPairProcessorNames: () => string[];
  masteringPairAnalysisNames: () => string[];
  masteringStereoAnalysisNames: () => string[];
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
  ) => WasmMasteringChainResult;
  masteringChainStereoWithProgress: (
    left: Float32Array,
    right: Float32Array,
    sampleRate: number,
    config: Record<string, unknown>,
    progressCallback: ProgressCallback | null,
  ) => WasmMasteringStereoChainResult;
  masteringPresetNames: () => string[];
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
  mixingScenePresetNames: () => string[];
  mixingScenePresetJson: (presetName: string) => string;
  mixStereo: (
    leftChannels: Float32Array[],
    rightChannels: Float32Array[],
    sampleRate: number,
    options: Record<string, unknown>,
  ) => WasmMixResult;
  trim: (samples: Float32Array, sampleRate: number, thresholdDb: number) => Float32Array;

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

  melSpectrogram: (
    samples: Float32Array,
    sampleRate: number,
    nFft: number,
    hopLength: number,
    nMels: number,
  ) => WasmMelResult;
  mfcc: (
    samples: Float32Array,
    sampleRate: number,
    nFft: number,
    hopLength: number,
    nMels: number,
    nMfcc: number,
  ) => WasmMfccResult;

  melToStft: (
    melPower: Float32Array,
    nMels: number,
    nFrames: number,
    sampleRate: number,
    nFft: number,
    hopLength: number,
    fmin: number,
    fmax: number,
  ) => WasmStftPowerResult;
  melToAudio: (
    melPower: Float32Array,
    nMels: number,
    nFrames: number,
    sampleRate: number,
    nFft: number,
    hopLength: number,
    nIter: number,
    fmin: number,
    fmax: number,
  ) => Float32Array;
  mfccToMel: (
    mfcc: Float32Array,
    nMfcc: number,
    nFrames: number,
    nMels: number,
  ) => WasmMelPowerResult;
  mfccToAudio: (
    mfcc: Float32Array,
    nMfcc: number,
    nFrames: number,
    nMels: number,
    sampleRate: number,
    nFft: number,
    hopLength: number,
    nIter: number,
    fmin: number,
    fmax: number,
  ) => Float32Array;

  chroma: (
    samples: Float32Array,
    sampleRate: number,
    nFft: number,
    hopLength: number,
  ) => WasmChromaResult;

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
  ) => WasmPitchResult;

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
  padCenter: (values: Float32Array, size: number, padValue: number) => Float32Array;
  fixLength: (values: Float32Array, size: number, padValue: number) => Float32Array;
  fixFrames: (frames: Int32Array, xMin: number, xMax: number, pad: boolean) => Int32Array;
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
  nnlsChroma: (samples: Float32Array, sampleRate: number) => WasmNnlsChromaResult;
  cqt: (
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
  ) => WasmMelodyResult;
  onsetEnvelope: (
    samples: Float32Array,
    sampleRate: number,
    nFft: number,
    hopLength: number,
    nMels: number,
  ) => Float32Array;
  fourierTempogram: (
    onsetEnvelope: Float32Array,
    sampleRate: number,
    hopLength: number,
    winLength: number,
  ) => WasmFourierTempogramResult;
  tempogramRatio: (
    tempogramData: Float32Array,
    winLength: number,
    sampleRate: number,
    hopLength: number,
  ) => Float32Array;
  lufs: (samples: Float32Array, sampleRate: number) => WasmLufsResult;
  momentaryLufs: (samples: Float32Array, sampleRate: number) => Float32Array;
  shortTermLufs: (samples: Float32Array, sampleRate: number) => Float32Array;
  resample: (samples: Float32Array, srcSr: number, targetSr: number) => Float32Array;

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
  ) => WasmRealtimeEngine;

  createStreamingMasteringChain: (config: Record<string, unknown>) => WasmStreamingMasteringChain;
  createEqualizer: (config: Record<string, unknown>) => WasmStreamingEqualizer;
  createMixerFromSceneJson: (json: string, sampleRate: number, blockSize: number) => WasmMixer;
  mixerPresetJson: (presetName: string) => string;
}
