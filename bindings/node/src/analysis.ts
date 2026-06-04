import { addon } from './native.js';
import type {
  AcousticOptions,
  AcousticResult,
  AnalysisProgressCallback,
  AnalysisResult,
  AnalyzeBpmOptions,
  AnalyzeDynamicsOptions,
  AnalyzeRhythmOptions,
  AnalyzeSectionsOptions,
  AnalyzeTimbreOptions,
  BpmAnalysisResult,
  ChordAnalysisResult,
  ChordChromaMethod,
  ChordDetectionOptions,
  DynamicsResult,
  Key,
  KeyCandidate,
  KeyDetectionOptions,
  MelodyOptions,
  MelodyResult,
  RhythmResult,
  RirResult,
  RirSynthOptions,
  RoomEstimateOptions,
  RoomEstimateResult,
  RoomMorphOptions,
  Section,
  TimbreResult,
} from './types.js';

export function detectBpm(samples: Float32Array, sampleRate = 22050): number {
  return addon.detectBpm(samples, sampleRate);
}

export function detectKey(
  samples: Float32Array,
  sampleRate = 22050,
  options: KeyDetectionOptions = {},
): Key {
  return addon.detectKey(samples, sampleRate, options);
}

export function detectKeyCandidates(
  samples: Float32Array,
  sampleRate = 22050,
  options: KeyDetectionOptions = {},
): KeyCandidate[] {
  return addon.detectKeyCandidates(samples, sampleRate, options);
}

export function detectBeats(samples: Float32Array, sampleRate = 22050): Float32Array {
  return addon.detectBeats(samples, sampleRate);
}

export function detectDownbeats(samples: Float32Array, sampleRate = 22050): Float32Array {
  return addon.detectDownbeats(samples, sampleRate);
}

export function detectOnsets(samples: Float32Array, sampleRate = 22050): Float32Array {
  return addon.detectOnsets(samples, sampleRate);
}

export function analyze(samples: Float32Array, sampleRate = 22050): AnalysisResult {
  return addon.analyze(samples, sampleRate);
}

/**
 * Synthesize a room impulse response from shoebox geometry. `hasError` is true
 * when the source/listener falls outside the room (the RIR is then empty).
 */
export function synthesizeRir(options: RirSynthOptions = {}): RirResult {
  return addon.synthesizeRir(options);
}

/**
 * Estimate an equivalent room (volume/dimensions/absorption/DRR) from a
 * recording or impulse response. The volume scale is anchored by
 * `referenceAbsorption`; `confidence` reports how well the data support it.
 */
export function estimateRoom(
  samples: Float32Array,
  sampleRate = 48000,
  options: RoomEstimateOptions = {},
): RoomEstimateResult {
  return addon.estimateRoom(samples, sampleRate, options);
}

/**
 * Morph a recording's reverberation toward a target room (creative FX, not
 * dereverberation). Returns the morphed samples (input length plus the target
 * room's reverb tail).
 */
export function roomMorph(
  samples: Float32Array,
  sampleRate: number,
  options: RoomMorphOptions = {},
): Float32Array {
  return addon.roomMorph(samples, sampleRate, options);
}

/**
 * Asynchronous variant of {@link analyze}. Runs the DSP pipeline on a libuv
 * worker thread so the JS event loop is never blocked. The returned promise
 * resolves with the same shape as the synchronous version.
 */
export function analyzeAsync(samples: Float32Array, sampleRate = 22050): Promise<AnalysisResult> {
  return addon.analyzeAsync(samples, sampleRate);
}

/**
 * Run the full music analysis, reporting per-stage progress.
 *
 * The progress callback is invoked synchronously during analysis with a
 * normalized progress value in `[0, 1]` and the current stage name. The result
 * shape matches {@link analyze}.
 */
export function analyzeWithProgress(
  samples: Float32Array,
  sampleRate = 22050,
  onProgress: AnalysisProgressCallback,
): AnalysisResult {
  return addon.analyzeWithProgress(samples, sampleRate, onProgress);
}

/** Detect song-structure sections (intro/verse/chorus/...). */
export function analyzeSections(
  samples: Float32Array,
  sampleRate = 22050,
  options: AnalyzeSectionsOptions = {},
): Section[] {
  return addon.analyzeSections(
    samples,
    sampleRate,
    options.nFft ?? 2048,
    options.hopLength ?? 512,
    options.minSectionSec ?? 4.0,
  );
}

/**
 * Extract the melody contour from monophonic audio.
 *
 * By default this uses plain per-frame YIN. Pass `{ usePyin: true }` for the
 * Viterbi-smoothed pYIN tracker (less prone to octave jumps), or supply
 * `usePyin` / `center` positionally. When pYIN is active, `center` (default
 * `true`) reflect-pads by `frameLength / 2` so frame `i` is centered at
 * `i * hopLength` (matching `librosa.pyin(center=True)`); `center` is ignored
 * for plain YIN.
 */
export function analyzeMelody(
  samples: Float32Array,
  sampleRate = 22050,
  options: MelodyOptions = {},
): MelodyResult {
  return addon.analyzeMelody(
    samples,
    sampleRate,
    options.fmin ?? 65.0,
    options.fmax ?? 2093.0,
    options.frameLength ?? 2048,
    options.hopLength ?? 256,
    options.threshold ?? 0.1,
    options.usePyin ?? false,
    options.center ?? true,
  );
}

export function analyzeBpm(
  samples: Float32Array,
  sampleRate = 22050,
  options: AnalyzeBpmOptions = {},
): BpmAnalysisResult {
  return addon.analyzeBpm(
    samples,
    sampleRate,
    options.bpmMin ?? 30.0,
    options.bpmMax ?? 300.0,
    options.startBpm ?? 120.0,
    options.nFft ?? 2048,
    options.hopLength ?? 512,
    options.maxCandidates ?? 5,
  );
}

export function analyzeRhythm(
  samples: Float32Array,
  sampleRate = 22050,
  options: AnalyzeRhythmOptions = {},
): RhythmResult {
  return addon.analyzeRhythm(
    samples,
    sampleRate,
    options.bpmMin ?? 60.0,
    options.bpmMax ?? 200.0,
    options.startBpm ?? 120.0,
    options.nFft ?? 2048,
    options.hopLength ?? 512,
  );
}

export function analyzeDynamics(
  samples: Float32Array,
  sampleRate = 22050,
  options: AnalyzeDynamicsOptions = {},
): DynamicsResult {
  return addon.analyzeDynamics(
    samples,
    sampleRate,
    options.windowSec ?? 0.4,
    options.hopLength ?? 512,
    options.compressionThreshold ?? 6.0,
  );
}

export function analyzeImpulseResponse(
  samples: Float32Array,
  sampleRate = 48000,
  nOctaveBands = 6,
): AcousticResult {
  return addon.analyzeImpulseResponse(samples, sampleRate, nOctaveBands);
}

export function detectAcoustic(
  samples: Float32Array,
  sampleRate = 48000,
  options: AcousticOptions = {},
): AcousticResult {
  return addon.detectAcoustic(
    samples,
    sampleRate,
    options.nOctaveBands ?? 6,
    options.nThirdOctaveSubbands ?? 24,
    options.minDecayDb ?? 30.0,
    options.noiseFloorMarginDb ?? 10.0,
  );
}

export function analyzeTimbre(
  samples: Float32Array,
  sampleRate = 22050,
  options: AnalyzeTimbreOptions = {},
): TimbreResult {
  return addon.analyzeTimbre(
    samples,
    sampleRate,
    options.nFft ?? 2048,
    options.hopLength ?? 512,
    options.nMels ?? 128,
    options.nMfcc ?? 13,
    options.windowSec ?? 0.5,
  );
}

/**
 * Resolved chord-detection parameters with all defaults applied. Used to feed
 * the positional native call from either the positional or options-object
 * public forms.
 */
interface ResolvedChordParams {
  minDuration: number;
  smoothingWindow: number;
  threshold: number;
  useTriadsOnly: boolean;
  nFft: number;
  hopLength: number;
  useBeatSync: boolean;
  useHmm: boolean;
  hmmBeamWidth: number;
  useKeyContext: boolean;
  keyRoot: number;
  keyMode: number;
  detectInversions: boolean;
  chromaMethod: ChordChromaMethod;
}

function resolveChordOptions(options: ChordDetectionOptions): ResolvedChordParams {
  return {
    minDuration: options.minDuration ?? 0.3,
    smoothingWindow: options.smoothingWindow ?? 2.0,
    threshold: options.threshold ?? 0.5,
    useTriadsOnly: options.useTriadsOnly ?? false,
    nFft: options.nFft ?? 2048,
    hopLength: options.hopLength ?? 512,
    useBeatSync: options.useBeatSync ?? true,
    useHmm: options.useHmm ?? false,
    hmmBeamWidth: options.hmmBeamWidth ?? 24,
    useKeyContext: options.useKeyContext ?? false,
    keyRoot: options.keyRoot ?? 0,
    keyMode: options.keyMode ?? 0,
    detectInversions: options.detectInversions ?? false,
    chromaMethod: options.chromaMethod ?? 'stft',
  };
}

/**
 * Detect chords from mono samples.
 *
 * Accepts either an options object (`detectChords(samples, sampleRate, options)`,
 * matching the WASM binding) or the legacy positional argument list. The form
 * is selected by the type of the third argument: an object selects the
 * options form, otherwise the positional form is used.
 */
export function detectChords(
  samples: Float32Array,
  sampleRate?: number,
  options?: ChordDetectionOptions,
): ChordAnalysisResult;
export function detectChords(
  samples: Float32Array,
  sampleRate?: number,
  minDuration?: number,
  smoothingWindow?: number,
  threshold?: number,
  useTriadsOnly?: boolean,
  nFft?: number,
  hopLength?: number,
  useBeatSync?: boolean,
  useHmm?: boolean,
  hmmBeamWidth?: number,
  useKeyContext?: boolean,
  keyRoot?: number,
  keyMode?: number,
  detectInversions?: boolean,
  chromaMethod?: ChordChromaMethod,
): ChordAnalysisResult;
export function detectChords(
  samples: Float32Array,
  sampleRate = 22050,
  minDurationOrOptions: number | ChordDetectionOptions = 0.3,
  smoothingWindow = 2.0,
  threshold = 0.5,
  useTriadsOnly = false,
  nFft = 2048,
  hopLength = 512,
  useBeatSync = true,
  useHmm = false,
  hmmBeamWidth = 24,
  useKeyContext = false,
  keyRoot = 0,
  keyMode = 0,
  detectInversions = false,
  chromaMethod: ChordChromaMethod = 'stft',
): ChordAnalysisResult {
  const p: ResolvedChordParams =
    typeof minDurationOrOptions === 'object'
      ? resolveChordOptions(minDurationOrOptions)
      : {
          minDuration: minDurationOrOptions,
          smoothingWindow,
          threshold,
          useTriadsOnly,
          nFft,
          hopLength,
          useBeatSync,
          useHmm,
          hmmBeamWidth,
          useKeyContext,
          keyRoot,
          keyMode,
          detectInversions,
          chromaMethod,
        };
  return addon.detectChords(
    samples,
    sampleRate,
    p.minDuration,
    p.smoothingWindow,
    p.threshold,
    p.useTriadsOnly,
    p.nFft,
    p.hopLength,
    p.useBeatSync,
    p.useHmm,
    p.hmmBeamWidth,
    p.useKeyContext,
    p.keyRoot,
    p.keyMode,
    p.detectInversions,
    chordChromaMethodValue(p.chromaMethod),
  );
}

/**
 * Functional (Roman-numeral) chord analysis from mono samples.
 *
 * Accepts either an options object
 * (`chordFunctionalAnalysis(samples, keyRoot, keyMode, sampleRate, options)`,
 * matching the WASM binding) or the legacy positional argument list. The form
 * is selected by the type of the fifth argument.
 */
export function chordFunctionalAnalysis(
  samples: Float32Array,
  keyRoot: number,
  keyMode?: number,
  sampleRate?: number,
  options?: ChordDetectionOptions,
): string[];
export function chordFunctionalAnalysis(
  samples: Float32Array,
  keyRoot: number,
  keyMode?: number,
  sampleRate?: number,
  minDuration?: number,
  smoothingWindow?: number,
  threshold?: number,
  useTriadsOnly?: boolean,
  nFft?: number,
  hopLength?: number,
  useBeatSync?: boolean,
  useHmm?: boolean,
  hmmBeamWidth?: number,
  useKeyContext?: boolean,
  detectInversions?: boolean,
  chromaMethod?: ChordChromaMethod,
): string[];
export function chordFunctionalAnalysis(
  samples: Float32Array,
  keyRoot: number,
  keyMode = 0,
  sampleRate = 22050,
  minDurationOrOptions: number | ChordDetectionOptions = 0.3,
  smoothingWindow = 2.0,
  threshold = 0.5,
  useTriadsOnly = false,
  nFft = 2048,
  hopLength = 512,
  useBeatSync = true,
  useHmm = false,
  hmmBeamWidth = 24,
  useKeyContext = false,
  detectInversions = false,
  chromaMethod: ChordChromaMethod = 'stft',
): string[] {
  const p: ResolvedChordParams =
    typeof minDurationOrOptions === 'object'
      ? resolveChordOptions(minDurationOrOptions)
      : {
          minDuration: minDurationOrOptions,
          smoothingWindow,
          threshold,
          useTriadsOnly,
          nFft,
          hopLength,
          useBeatSync,
          useHmm,
          hmmBeamWidth,
          useKeyContext,
          keyRoot,
          keyMode,
          detectInversions,
          chromaMethod,
        };
  return addon.chordFunctionalAnalysis(
    samples,
    keyRoot,
    keyMode,
    sampleRate,
    p.minDuration,
    p.smoothingWindow,
    p.threshold,
    p.useTriadsOnly,
    p.nFft,
    p.hopLength,
    p.useBeatSync,
    p.useHmm,
    p.hmmBeamWidth,
    p.useKeyContext,
    p.detectInversions,
    chordChromaMethodValue(p.chromaMethod),
  );
}

function chordChromaMethodValue(method: ChordChromaMethod): number {
  if (method === 'stft') {
    return 0;
  }
  if (method === 'nnls') {
    return 1;
  }
  throw new Error(`Invalid chord chroma method: ${method}`);
}
