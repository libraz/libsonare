export type ProgressCallback = (progress: number, stage: string) => void;

export interface WasmKeyResult {
  root: number;
  mode: number;
  confidence: number;
  name: string;
  shortName: string;
}

export interface WasmBeatResult {
  time: number;
  strength: number;
}

export interface WasmChordResult {
  root: number;
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

export interface WasmStreamAnalyzer {
  process: (samples: Float32Array) => void;
  processWithOffset: (samples: Float32Array, sampleOffset: number) => void;
  availableFrames: () => number;
  readFramesSoa: (maxFrames: number) => WasmFrameBuffer;
  readFramesU8: (maxFrames: number) => unknown;
  readFramesI16: (maxFrames: number) => unknown;
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
  detectOnsets: (samples: Float32Array, sampleRate: number) => Float32Array;
  detectBeats: (samples: Float32Array, sampleRate: number) => Float32Array;
  analyze: (samples: Float32Array, sampleRate: number) => WasmAnalysisResult;
  analyzeWithProgress: (
    samples: Float32Array,
    sampleRate: number,
    progressCallback: ProgressCallback | null,
  ) => WasmAnalysisResult;
  version: () => string;

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
  resample: (samples: Float32Array, srcSr: number, targetSr: number) => Float32Array;

  StreamAnalyzer: new (
    sampleRate: number,
    nFft: number,
    hopLength: number,
    nMels: number,
    computeMel: boolean,
    computeChroma: boolean,
    computeOnset: boolean,
    emitEveryNFrames: number,
  ) => WasmStreamAnalyzer;
}
