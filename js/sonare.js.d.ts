/**
 * Type declarations for the Emscripten-generated WASM module with embind
 */

interface SonareModuleOptions {
  locateFile?: (path: string, prefix: string) => string;
  onRuntimeInitialized?: () => void;
  print?: (text: string) => void;
  printErr?: (text: string) => void;
}

// Result types
interface WasmKeyResult {
  root: number;
  mode: number;
  confidence: number;
  name: string;
  shortName: string;
}

interface WasmBeatResult {
  time: number;
  strength: number;
}

interface WasmChordResult {
  root: number;
  quality: number;
  start: number;
  end: number;
  confidence: number;
  name: string;
}

interface WasmSectionResult {
  type: number;
  start: number;
  end: number;
  energyLevel: number;
  confidence: number;
  name: string;
}

interface WasmTimbreResult {
  brightness: number;
  warmth: number;
  density: number;
  roughness: number;
  complexity: number;
}

interface WasmDynamicsResult {
  dynamicRangeDb: number;
  loudnessRangeDb: number;
  crestFactor: number;
  isCompressed: boolean;
}

interface WasmRhythmResult {
  syncopation: number;
  grooveType: string;
  patternRegularity: number;
}

interface WasmTimeSignatureResult {
  numerator: number;
  denominator: number;
  confidence: number;
}

interface WasmAnalysisResult {
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

interface WasmHpssResult {
  harmonic: Float32Array;
  percussive: Float32Array;
  sampleRate: number;
}

interface WasmStftResult {
  nBins: number;
  nFrames: number;
  nFft: number;
  hopLength: number;
  sampleRate: number;
  magnitude: Float32Array;
  power: Float32Array;
}

interface WasmStftDbResult {
  nBins: number;
  nFrames: number;
  db: Float32Array;
}

interface WasmMelResult {
  nMels: number;
  nFrames: number;
  sampleRate: number;
  hopLength: number;
  power: Float32Array;
  db: Float32Array;
}

interface WasmMfccResult {
  nMfcc: number;
  nFrames: number;
  coefficients: Float32Array;
}

interface WasmChromaResult {
  nChroma: number;
  nFrames: number;
  sampleRate: number;
  hopLength: number;
  features: Float32Array;
  meanEnergy: number[];
}

interface WasmPitchResult {
  f0: Float32Array;
  voicedProb: Float32Array;
  voicedFlag: boolean[];
  nFrames: number;
  medianF0: number;
  meanF0: number;
}

type ProgressCallback = (progress: number, stage: string) => void;

interface SonareModule {
  // Quick API (high-level)
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

  // Effects
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
  trim: (samples: Float32Array, sampleRate: number, thresholdDb: number) => Float32Array;

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
  ) => WasmMelResult;
  mfcc: (
    samples: Float32Array,
    sampleRate: number,
    nFft: number,
    hopLength: number,
    nMels: number,
    nMfcc: number,
  ) => WasmMfccResult;

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
  ) => WasmPitchResult;

  // Core - Conversion
  hzToMel: (hz: number) => number;
  melToHz: (mel: number) => number;
  hzToMidi: (hz: number) => number;
  midiToHz: (midi: number) => number;
  hzToNote: (hz: number) => string;
  noteToHz: (note: string) => number;
  framesToTime: (frames: number, sr: number, hopLength: number) => number;
  timeToFrames: (time: number, sr: number, hopLength: number) => number;

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
  };
  SectionType: {
    Intro: { value: 0 };
    Verse: { value: 1 };
    PreChorus: { value: 2 };
    Chorus: { value: 3 };
    Bridge: { value: 4 };
    Instrumental: { value: 5 };
    Outro: { value: 6 };
  };
}

declare function createModule(options?: SonareModuleOptions): Promise<SonareModule>;

export default createModule;
