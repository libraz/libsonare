/**
 * Pitch class enum (C=0, C#=1, ..., B=11)
 */
export const PitchClass = {
  C: 0,
  Cs: 1,
  D: 2,
  Ds: 3,
  E: 4,
  F: 5,
  Fs: 6,
  G: 7,
  Gs: 8,
  A: 9,
  As: 10,
  B: 11,
} as const;

export type PitchClass = (typeof PitchClass)[keyof typeof PitchClass];

/** A key mode by name: what {@link Key.mode} reports, and what every mode input takes. */
export type KeyMode =
  | 'major'
  | 'minor'
  | 'dorian'
  | 'phrygian'
  | 'lydian'
  | 'mixolydian'
  | 'locrian';

/** Autocorrelation or cosine-similarity tempogram. */
export type TempogramMode = 'autocorrelation' | 'cosine';

/** Key-profile family; a closed set of lowerCamelCase names, numbers refused. */
export type KeyProfile =
  | 'krumhanslSchmuckler'
  | 'temperley'
  | 'shaath'
  | 'faraldoEdmt'
  | 'faraldoEdma'
  | 'faraldoEdmm'
  | 'bellmanBudge';

/** Genre hint that picks a key profile; an unknown value is refused with `RangeError`. */
export type GenreHint = 'auto' | 'edm' | 'electronic' | 'dance' | 'pop' | 'classical' | 'jazz';

/** Normalisation used by `vectorNormalize`. */
export type NormType = 'max' | 'l1' | 'l2' | 'power';

/** Chroma front end for chord detection; `'cqt'` is the default. */
export type ChordChromaMethod = 'stft' | 'nnls' | 'cqt';

/** Rhythmic feel reported by the rhythm analysis. */
export type GrooveType = 'straight' | 'shuffle' | 'swing';

/** How a tempo hypothesis relates to the chosen tempo. */
export type BpmCandidateRelation = 'primary' | 'half' | 'double' | 'other';

/**
 * Chord quality tokens, indexed by the native ordinal: `CHORD_QUALITIES[ordinal]`
 * names the quality a native result or a streaming frame reports.
 */
export const CHORD_QUALITIES = [
  'major',
  'minor',
  'diminished',
  'augmented',
  'dominant7',
  'major7',
  'minor7',
  'sus2',
  'sus4',
  'unknown',
  'add9',
  'minorAdd9',
  'dim7',
  'halfDim7',
  'major9',
  'dominant9',
  'sus2Add4',
  'major6',
  'minor6',
  'minorMajor7',
  'dominant7Sus4',
  'dominant11',
  'dominant13',
  'dominant7Flat9',
  'dominant7Sharp9',
] as const;

/** Chord quality as a token; `'unknown'` is a chord the analyzer did not name. */
export type ChordQuality = (typeof CHORD_QUALITIES)[number];

/**
 * Section type tokens, indexed by the native ordinal.
 *
 * `'unknown'` means the analyzer did not name the segment: no boundary was
 * detected, the segment matched none of the positive branches, or the evidence
 * for a musical function was too weak to assert one. Its `confidence` is 0.
 */
export const SECTION_TYPES = [
  'intro',
  'verse',
  'preChorus',
  'chorus',
  'bridge',
  'instrumental',
  'outro',
  'unknown',
] as const;

/** Section type as a token. */
export type SectionType = (typeof SECTION_TYPES)[number];

/**
 * Detected musical key
 */
export interface Key {
  /** Pitch class of the tonic (C = 0). */
  root: PitchClass;
  /** Mode name (`'major'`, `'minor'`, `'dorian'`, ...), the form `scaleMaskForMode` and `chordFunctions` take. */
  mode: KeyMode;
  /**
   * Share of the model's belief that this key is the answer, in `[0, 1)`.
   *
   * A softmax over the profile correlations of every candidate that was scored,
   * so it falls as the runner-up closes in and two keys that split the evidence
   * -- a relative major and minor, typically -- each report about half.
   *
   * This is the model's own belief, **not** a measured accuracy. It says how
   * decisively the chroma picked this key out of the candidate set; it does not
   * say how often that pick is right, and nothing here has been fitted against
   * annotated recordings. A confident wrong answer is entirely possible, so a
   * pipeline that branches on it must choose its own threshold against its own
   * material.
   */
  confidence: number;
  /** Full key name, e.g. `'C major'`. */
  name: string;
  /** Short key name: the tonic for major, with an `m` suffix for minor, e.g. `'C'`, `'Am'`. */
  shortName: string;
}

/** A detected key together with the tuning its chroma was built with. */
export interface KeyDetection extends Key {
  /** Tuning used, in cents: the given value, or the measured one under `'auto'`. */
  tuningCents: number;
}

/** Settings shared by `detectKey` and `detectKeyCandidates`. */
export interface KeyDetectionOptions {
  /** Analysis window in samples at 22050 Hz, rescaled to the input rate. Default `2048`. */
  nFft?: number;
  /** Hop in samples of the analysed signal: the input resampled to 22050 Hz when its rate is above 22050 Hz, the input itself otherwise. Default `512`. */
  hopLength?: number;
  /** Separate the harmonic part before the chroma. Default `false`. */
  useHpss?: boolean;
  /** Weight chroma frames by the RMS of the input. Default `false`. */
  loudnessWeighted?: boolean;
  /** Chroma high-pass cutoff in Hz; absent = 80, 0 = no filter. */
  chromaHighPassHz?: number;
  /** Modes to score: a list of names, or `'majorMinor'`, `'all'` (every mode) or `'modal'`. */
  modes?: KeyMode[] | 'majorMinor' | 'all' | 'modal';
  /**
   * Pins the key-profile family; a pinned profile outranks `genreHint`. Omitted, `genreHint`
   * chooses it, or the chroma does under the default `'auto'` hint.
   */
  profile?: KeyProfile;
  /** Genre hint; an unknown value is refused with `RangeError`. */
  genreHint?: GenreHint;
  /**
   * Tuning offset of the recording in cents, in `[-50, 50)`; 0 is concert A440.
   * Omitted or `'auto'` measures it from the audio. The value used is reported
   * as {@link KeyDetection.tuningCents}; {@link referenceHzToTuning} converts a
   * reference pitch to this unit. Pass 0 to read the chroma at A440.
   */
  tuningCents?: number | 'auto';
}

/** A candidate key with the correlation that ranked it. */
export interface KeyCandidate {
  /** The candidate key. */
  key: Key;
  /** Correlation of the chroma with this key profile. */
  correlation: number;
}

/** Settings of `detectChords` and `chordFunctionalAnalysis`. */
export interface ChordDetectionOptions {
  /** Shortest chord kept, in seconds. */
  minDurationSec?: number;
  /** Chord smoothing window, in seconds. */
  smoothingWindowSec?: number;
  /** Final-template correlation threshold in [0, 1]; below it emits Unknown / N.C. */
  threshold?: number;
  /** Use only triad templates, no seventh chords. Default `true`. */
  useTriadsOnly?: boolean;
  /** STFT chroma window in samples at 22050 Hz, rescaled to the input rate so its duration is the same at every rate. */
  nFft?: number;
  /** Hop in samples of the caller's buffer, at the sample rate. Default `512`. */
  hopLength?: number;
  /** Detect chords on beat boundaries. Default `true`. */
  useBeatSync?: boolean;
  /** Smooth chord candidates with a Viterbi HMM. Default `true`. */
  useHmm?: boolean;
  /** Candidate beam width of the HMM smoothing. Default `24`. */
  hmmBeamWidth?: number;
  /** Bias the HMM transitions by the key given in `keyRoot` and `keyMode`. Default `false`. */
  useKeyContext?: boolean;
  /** Pitch class 0-11, e.g. `detectKey`'s `root`. */
  keyRoot?: PitchClass;
  /** Mode name, e.g. `detectKey`'s `mode`. */
  keyMode?: KeyMode;
  /** Estimate the bass pitch class and emit slash chords. Default `false`. */
  detectInversions?: boolean;
  /** Chroma front end; default `'cqt'`. */
  chromaMethod?: ChordChromaMethod;
  /**
   * Tuning offset of the recording in cents; must be in `[-50, 50)`. Omitted or
   * `'auto'` measures it from the audio; the value used is reported as
   * {@link ChordAnalysisResult.tuningCents}. {@link referenceHzToTuning} converts
   * a reference pitch to this unit. Pass 0 for concert A440.
   */
  tuningCents?: number | 'auto';
}

/** Options for `analyzeBpm`. All fields are optional. */
export interface AnalyzeBpmOptions {
  /** Lowest BPM to consider. Default 60. */
  bpmMin?: number;
  /**
   * Highest BPM to consider. Default 200. A range other than the default is searched as given, and
   * the unconstrained estimate is preferred when it lies inside it.
   */
  bpmMax?: number;
  /** Tempo prior the tracker is biased toward. Default 120. */
  startBpm?: number;
  /** Analysis window length in samples at 22050 Hz, rescaled to the input sample rate. Default 2048. */
  nFft?: number;
  /** Hop length in samples of the input buffer. Default 512. */
  hopLength?: number;
  /** Number of tempo candidates to return. Default 5. */
  maxCandidates?: number;
}

/** Options for `analyzeRhythm`. All fields are optional. */
export interface AnalyzeRhythmOptions {
  /** Lowest BPM to consider. Default 60. */
  bpmMin?: number;
  /**
   * Highest BPM to consider. Default 200. A range other than the default is searched as given, and
   * the unconstrained estimate is preferred when it lies inside it.
   */
  bpmMax?: number;
  /** Tempo prior the tracker is biased toward. Default 120. */
  startBpm?: number;
  /** Analysis window length in samples at 22050 Hz, rescaled to the input sample rate. Default 2048. */
  nFft?: number;
  /** Hop length in samples of the input buffer. Default 512. */
  hopLength?: number;
}

/** Options for `analyzeDynamics`. All fields are optional. */
export interface AnalyzeDynamicsOptions {
  /** Loudness-curve window length in seconds. Default 0.4. */
  windowSec?: number;
  /** Hop in samples of the caller's buffer, at the sample rate. Default 512. */
  hopLength?: number;
  /** Crest-factor (dB) below which the signal is flagged as compressed. Default 6. */
  compressionThresholdDb?: number;
}

/** Options for `analyzeTimbre`. All fields are optional. */
export interface AnalyzeTimbreOptions {
  /** Analysis window length in samples at 22050 Hz, rescaled to the input sample rate. Default 2048. */
  nFft?: number;
  /** Hop length in samples of the input buffer. Default 512. */
  hopLength?: number;
  /** Number of Mel bands. Default 128. */
  nMels?: number;
  /** Number of MFCCs. Default 13. */
  nMfcc?: number;
  /** Per-window analysis length in seconds. Default 0.5. */
  windowSec?: number;
}

/** Options for `analyzeSections`. All fields are optional. */
export interface AnalyzeSectionsOptions {
  /** Analysis window in samples at 22050 Hz, rescaled to the analysis rate. Default 2048. */
  nFft?: number;
  /** Hop in samples of the analysed signal: the input resampled to 22050 Hz when its rate is above 22050 Hz, the input itself otherwise. Default 512. */
  hopLength?: number;
  /** Minimum section duration in seconds. Default 4. */
  minSectionSec?: number;
  /**
   * Section boundary threshold: the relative threshold a structural boundary must reach
   * to start a new section, as {@link BoundaryOptions.threshold}. A finite value of 0 or
   * more; a negative one is a RangeError. Omit it for the library default 0.22, which sits
   * below `detectBoundaries`' own default of 0.3 because sections also snap to
   * downbeats and keep a minimum length.
   */
  threshold?: number;
}

/**
 * Detected beat
 */
export interface Beat {
  /** Beat position in seconds. */
  timeSec: number;
  /**
   * Onset-envelope value sampled at the beat's frame.
   *
   * @remarks
   * A single raw frame of the onset envelope, not a normalized or relative
   * salience: it is unbounded, its scale depends on the material, and it moves
   * with any jitter in the beat position. For accent scoring use
   * {@link BeatObservations.onsetStrength}, which is the windowed aggregate the
   * library's own downbeat pass scores.
   */
  strength: number;
}

/**
 * Beat-level evidence behind the downbeat and meter decisions.
 *
 * @remarks
 * These are inputs to the library's decision, not outputs of it, exposed so a
 * caller running its own meter work scores the same evidence instead of
 * reconstructing a weaker approximation from the frame-level onset envelope.
 * Each non-empty stream holds one value per entry of
 * {@link AnalysisResult.beats} and indexes in parallel with it. An empty stream
 * means the analysis could not produce it, which is not the same as every beat
 * having scored zero.
 */
export interface BeatObservations {
  /**
   * Beat-local onset-strength window.
   *
   * @remarks
   * Unlike {@link Beat.strength} this is aggregated over a window around the
   * beat rather than sampled at a single frame, so it is the stream to score
   * accents with.
   */
  onsetStrength: number[];
  /**
   * Beat-local low-frequency energy — the accent evidence a log-spectral
   * difference discards. Empty when the analysis ran without audio.
   */
  lowFrequencyEnergy: number[];
  /** Per-beat chord-change evidence. Empty until chords are analyzed. */
  chordChange: number[];
}

/**
 * Detected chord
 */
export interface Chord {
  /** Root pitch class (C = 0). */
  root: PitchClass;
  /** Bass pitch class (C = 0). */
  bass: PitchClass;
  /** Canonical core spelling; stable across all language bindings. */
  rootName: string;
  /** Canonical core spelling; stable across all language bindings. */
  bassName: string;
  /** Chord quality token; `'unknown'` for a chord the analyzer did not name. */
  quality: ChordQuality;
  /** Chord start time in seconds. */
  startSec: number;
  /** Chord end time in seconds. */
  endSec: number;
  /** Derived from `endSec - startSec`; the core carries only the two endpoints. */
  durationSec: number;
  /** Correlation of the chroma with the chosen chord template, in `[0, 1]`. */
  confidence: number;
  /** Canonical core chord symbol (e.g. `'Cmaj7'`, `'Am/C'`, `'N.C.'`). */
  name: string;
}

/** A chord in {@link AnalysisResult.chords}, which carries its function in the key. */
export interface AnalysisChord extends Chord {
  /**
   * Roman numeral of the chord relative to {@link AnalysisResult.key}, e.g.
   * `'V7'`, `'vi'`, `'bVII'`. The same spelling `chordFunctionalAnalysis`
   * returns for that chord and key. Empty for a chord that is N.C.
   */
  romanNumeral: string;
}

/**
 * Harmonic function of a chord in a key: `'chromatic'` for a root outside the
 * key's scale and `'none'` for an unknown chord (`'N.C.'`).
 */
export type ChordFunction = 'tonic' | 'subdominant' | 'dominant' | 'chromatic' | 'none';

/** The fields `chordFunctions` reads from a chord; everything else is carried through. */
export interface ChordFunctionsInput {
  /** Root pitch class (C = 0). */
  root: PitchClass;
  /** Chord quality token. */
  quality: ChordQuality;
}

/** A key given as `detectKey` reports it. */
export interface ChordFunctionsKey {
  /** Pitch class 0-11. */
  root: PitchClass;
  /** Mode name as `detectKey` reports it. */
  mode: KeyMode;
}

/** A timed chord with its Roman numeral and harmonic function in a key. */
export type FunctionalChord<T extends ChordFunctionsInput = Chord> = T & {
  /** Roman numeral relative to the key (`'I'`, `'V7'`, `'vi'`; `'N.C.'` for an unknown chord). */
  romanNumeral: string;
  /** Harmonic function of the chord in the key. */
  function: ChordFunction;
};

/** Result of `detectChords`. */
export interface ChordAnalysisResult {
  /** Detected chords, in time order. */
  chords: Chord[];
  /** Tuning the chroma was built with, in cents: the given value, or the measured one under `tuningCents: 'auto'`. */
  tuningCents: number;
}

/**
 * Detected section
 */
export interface Section {
  /** Section type. */
  type: SectionType;
  /** Section start time in seconds. */
  startSec: number;
  /** Section end time in seconds. */
  endSec: number;
  /** Median frame RMS in the section over the loudest section's, in `[0, 1]`. */
  energyLevel: number;
  /** Confidence in `type`, in `[0, 1]`; always 0 for `Unknown`. */
  confidence: number;
  /**
   * Repetition group: sections sharing a value repeat the same material.
   * Numbered 0, 1, ... by first appearance.
   */
  group: number;
  /** Human-readable section name (e.g. `'Chorus'`). */
  name: string;
}

/** The sections `analyzeSections` detected, with the form they spell. */
export interface SectionAnalysis {
  /** Detected sections, in time order. */
  sections: Section[];
  /** One letter per section in time order (e.g. `'IABABCBO'`); `''` when there are no sections. */
  form: string;
}

/** Options for `detectBoundaries`. All fields are optional. */
export interface BoundaryOptions {
  /** Analysis window in samples at 22050 Hz, rescaled to the analysis rate. Default 2048. */
  nFft?: number;
  /** Hop in samples of the analysed signal: the input resampled to 22050 Hz when its rate is above 22050 Hz, the input itself otherwise. Default 512. */
  hopLength?: number;
  /** Checkerboard kernel size in frames. Default 64. */
  kernelSize?: number;
  /**
   * Relative novelty threshold: a peak must reach this fraction of the largest raw
   * novelty event within {@link referenceWindowSec} of it, a lone dominant event (one
   * no other event in the window comes within 30% of) set aside. Selects
   * how prominent a peak is *among its neighbours*; it says nothing about how much
   * the features actually changed. Default 0.3.
   */
  threshold?: number;
  /**
   * Novelty floor applied to the raw response before that scaling, asking whether
   * anything changed at all. Default 0.005. Set to 0 to gate on {@link threshold}
   * alone -- but self-scaling turns residual fluctuation into peaks of 1.0, so a
   * stationary input then segments anyway. Lowering the floor does not recover
   * level-only structure: a level change turns the feature vector about five times
   * less than a comparable pitch change, landing below what steady noise produces,
   * so the noise is admitted first.
   */
  absoluteThreshold?: number;
  /** Number of MFCC coefficients. Default 13. */
  nMfcc?: number;
  /** Number of chroma bins. Default 12. */
  nChroma?: number;
  /** Minimum spacing between peaks, in seconds. Default 2. */
  peakDistanceSec?: number;
  /**
   * One-sided span in seconds over which the local novelty maximum that
   * `threshold` is measured against is taken. Must be finite and non-negative;
   * 0 makes each frame its own reference, which disables the relative threshold.
   * A value at least the track length makes the reference whole-track, still with
   * a lone dominant event set aside. Default 60.
   */
  referenceWindowSec?: number;
  /** Use MFCC features. Default `true`. */
  useMfcc?: boolean;
  /** Use chroma features. Default `true`. */
  useChroma?: boolean;
}

/**
 * A single detected structural boundary (mirrors the C `SonareBoundary`).
 */
export interface Boundary {
  /** Boundary time in seconds (the authoritative output). */
  timeSec: number;
  /**
   * Index into the analysis grid, on {@link BoundaryResult.sampleRate}'s time
   * base rather than the source's. For a long-form input the feature grid is
   * additionally mean-pooled, so this indexes the pooled grid; use `timeSec` for
   * sample/second mapping either way.
   */
  frame: number;
  /** Boundary strength (novelty score). */
  strength: number;
}

/**
 * Result of `detectBoundaries` (mirrors the C `SonareBoundaryResult`).
 */
export interface BoundaryResult {
  /** Detected transitions, in time order. */
  boundaries: Boundary[];
  /**
   * Novelty curve scaled by its own maximum, one value per analysis frame. A
   * peak of 1 means "the most novel frame here", not "a large change"; multiply
   * by {@link noveltyPeak} to recover the raw response
   * {@link BoundaryOptions.absoluteThreshold} is compared against.
   */
  noveltyCurve: Float32Array;
  /**
   * The largest raw checkerboard response, before normalization. Zero when the
   * curve was left unnormalized because nothing rose above the numerical floor.
   */
  noveltyPeak: number;
  /**
   * The rate the analysis ran at, not the input's: a source above 22050 Hz is
   * resampled to 22050 Hz before any feature is computed, which is what makes
   * {@link Boundary.frame} interpretable.
   */
  sampleRate: number;
  /** Hop length the analysis grid was built on. */
  hopLength: number;
  /**
   * Feature pooling stride: 1 for every realistic input length, otherwise the
   * number of raw STFT frames averaged into each analysis frame.
   */
  frameStride: number;
}

/**
 * A single melody contour point (mirrors the C `SonareMelodyPoint`).
 */
export interface MelodyPoint {
  /** Frame time in seconds. */
  timeSec: number;
  /** Estimated fundamental frequency in Hz; absent when the frame is unvoiced. */
  frequencyHz?: number;
  /** Voicing confidence in `[0, 1]`. */
  confidence: number;
}

/**
 * Melody analysis result (mirrors the C `SonareMelodyResult`).
 */
export interface MelodyResult {
  /** Melody contour, one point per analysis frame. */
  points: MelodyPoint[];
  /** Span between the lowest and highest voiced pitch, in octaves. */
  pitchRangeOctaves: number;
  /** Pitch stability in `[0, 1]`; 1 is very stable. */
  pitchStability: number;
  /** Mean fundamental frequency of the voiced frames, in Hz. */
  meanFrequencyHz: number;
  /** Vibrato rate in Hz; 0 when there is no vibrato. */
  vibratoRateHz: number;
}

/**
 * Dynamics characteristics from the unified analysis.
 */
export interface AnalysisDynamics {
  /** Dynamic range in dB. `+Infinity` when the low percentile window is silent; absent when not measured. */
  dynamicRangeDb?: number;
  /** Peak level in dB. `-Infinity` for silence. Never absent: every input sample measures it. */
  peakDb: number;
  /** RMS level in dB. `-Infinity` for silence. Never absent: every input sample measures it. */
  rmsDb: number;
  /**
   * Loudness range in LU (EBU R128). Absent for silence and when no 3 s block passes the gates:
   * a spread, not a level, so never `-Infinity`.
   */
  loudnessRangeLu?: number;
  /** Peak-to-RMS ratio of the signal. Absent when not measured. */
  crestFactor?: number;
  /** True when the signal is flagged as compressed. */
  isCompressed: boolean;
}

/** A time signature with the support the analysis found for it. */
export interface TimeSignature {
  /** Beats per bar. */
  numerator: number;
  /** Note value of one beat: `4` is a quarter note. */
  denominator: number;
  /**
   * Support for this signature in `[0, 1]`.
   *
   * @remarks
   * What it measures depends on which field the signature arrived in. On
   * {@link MeterEstimate.timeSignature} it is derived from the margin over the
   * runner-up; on a {@link MeterEstimate.candidates} entry it is that
   * candidate's share of the summed support. The two are not comparable, so
   * read the value from the field you meant rather than from whichever one is
   * to hand.
   */
  confidence: number;
}

/**
 * Meter estimated over a caller-supplied beat series.
 */
export interface MeterEstimate {
  /**
   * Selected time signature.
   *
   * @remarks
   * Its `confidence` is the margin over the runner-up — how separated the
   * winner is — not the share-of-support a {@link candidates} entry carries
   * under the same field name.
   */
  timeSignature: TimeSignature;
  /** Beat index the first measure starts on, in `[0, timeSignature.numerator)`. */
  downbeatPhase: number;
  /**
   * Whether a search ran, as opposed to the fixed fallback being reported.
   *
   * @remarks
   * `false` means the beat series was too short to score any candidate, and
   * every other field then carries that fallback rather than a measurement —
   * `timeSignature.confidence` included, which is 0, so an unchecked read
   * degrades toward "no idea" rather than toward a middling detection. Read
   * this before treating a short span's answer as a detection.
   */
  searched: boolean;
  /**
   * How the bar divides, in beats per accent group.
   *
   * @remarks
   * `[3, 2, 2]` is the 7/8 an aksak meter notates as 3+2+2, and `[2, 2]` an
   * ordinary four. Always sums to `timeSignature.numerator`.
   *
   * A single entry means no internal division was resolved — the numerator has
   * none to find, it was too wide to search, or the span was too short to
   * search at all.
   *
   * This is also what tells a compound bar from a simple one: per-beat accents
   * cannot say how a beat subdivides, so a six accented 3+3 keeps the requested
   * denominator and reports `[3, 3]` rather than being promoted to 6/8.
   */
  grouping: number[];
  /**
   * Multi-comb score per requested candidate numerator.
   *
   * @remarks
   * Parallel to the `candidateNumerators` that were requested, in the order
   * they were given. This does NOT index alike with {@link candidates}, which
   * is ordered by descending support.
   *
   * Standardized and signed: zero is the level a numerator reaches on beats
   * carrying no meter, so a negative entry means less support than noise would
   * produce. Only the ordering and the gaps between entries carry meaning.
   *
   * Comparable only within one result. A score grows with the square root of
   * how many beats were scored, so the same meter over twice the beats scores
   * about 1.41 times as high; a segmentation search comparing spans of
   * different lengths has to normalize for length first.
   */
  candidateScores: number[];
  /**
   * Candidate signatures ordered by descending support.
   *
   * @remarks
   * A ranking, so entry `k` is the k-th best hypothesis — not the k-th
   * requested numerator. Use {@link candidateScores} to read the score of a
   * specific requested numerator.
   *
   * Each entry's `confidence` is that candidate's share of the summed support,
   * so the entries sum to one. It is a different quantity from
   * {@link timeSignature}'s margin-derived confidence, which shares the field
   * name and nothing else.
   */
  candidates: TimeSignature[];
}

/**
 * Rhythm features from the unified analysis.
 */
export interface AnalysisRhythm {
  /** Syncopation level in `[0, 1]`. */
  syncopation: number;
  /** Rhythmic feel of the track. */
  grooveType: GrooveType;
  /** Regularity of the beat intervals in `[0, 1]`; 0 with no beat interval. */
  patternRegularity: number;
  /** Tempo stability over time in `[0, 1]`; 0 below two beat intervals. */
  tempoStability: number;
  /** Estimated time signature. */
  timeSignature: TimeSignature;
}

/**
 * Melody contour from the unified analysis (pitch trajectory + summary stats).
 */
export interface AnalysisMelody {
  /** Span between the lowest and highest voiced pitch, in octaves. */
  pitchRangeOctaves: number;
  /** Pitch stability in `[0, 1]`; 1 is very stable. */
  pitchStability: number;
  /** Mean fundamental frequency of the voiced frames, in Hz. */
  meanFrequencyHz: number;
  /** Vibrato rate in Hz; 0 when there is no vibrato. */
  vibratoRateHz: number;
  /** Pitch contour, one point per analysis frame. */
  pitches: MelodyPoint[];
}

/**
 * Complete analysis result
 */
export interface AnalysisResult {
  /** Global tempo in beats per minute. */
  bpm: number;
  /**
   * Winning tempo cluster's vote share within the searched range: 60–200 BPM at 22050 Hz.
   * `analyzeBpm()` searches 60–200 BPM at the input rate, so the two can differ on the same
   * input; use a 22050 Hz input to compare them.
   */
  bpmConfidence: number;
  /** Tempo hypotheses, the chosen tempo first. */
  bpmCandidates: AnalysisBpmCandidate[];
  /** Detected musical key. */
  key: Key;
  /** Estimated time signature. */
  timeSignature: TimeSignature;
  /** Time-signature hypotheses ordered by descending support. */
  timeSignatureCandidates: TimeSignature[];
  /** Detected beats, in time order. */
  beats: Beat[];
  /**
   * The beats that fall on a measure start, in time order: one entry per detected
   * downbeat, each also present in {@link AnalysisResult.beats}.
   */
  downbeats: Beat[];
  /**
   * Beat index the first measure starts on, in `[0, timeSignature.numerator)`.
   *
   * @remarks
   * The meter estimator's phase, so the first downbeat normally sits at this
   * beat. It is not re-derived when downbeats are refined from chord and
   * low-frequency-energy evidence, so the two can disagree when the refinement
   * moves the first measure start.
   */
  downbeatPhase: number;
  /**
   * Beat-level evidence behind the downbeat and meter decisions, parallel to
   * {@link AnalysisResult.beats}.
   */
  beatObservations: BeatObservations;
  /**
   * Smoothed local tempo at each beat, in BPM, parallel to
   * {@link AnalysisResult.beats}.
   *
   * @remarks
   * Empty unless `computeTempoCurve` was set, and empty regardless when fewer
   * than two beats were detected, since a tempo is a property of the interval
   * between two beats. The last entry repeats the tempo of the interval leading
   * into the final beat, which opens no interval of its own.
   *
   * Values are continuous, not points on a tempo grid: each is a weighted
   * average, in log tempo, of the beat intervals around it. A steady tempo
   * reads within about 1% at every beat, and a tempo that moves is followed a
   * few beats late.
   *
   * This is the local tempo rather than {@link AnalysisResult.bpm} resampled:
   * on material whose tempo moves it departs from `bpm`, and reading a single
   * number out of it is not how to get the global tempo.
   */
  beatLocalBpm: number[];
  /** Detected chord progression. */
  chords: AnalysisChord[];
  /** Detected song-structure sections. */
  sections: Section[];
  /** Aggregate timbre summary. */
  timbre: TimbreFrame;
  /** Aggregate dynamics summary. */
  dynamics: AnalysisDynamics;
  /** Aggregate rhythm summary. */
  rhythm: AnalysisRhythm;
  /** Melody-contour summary. */
  melody: AnalysisMelody;
  /** Human-readable musical form label (e.g. `'AABA'`). */
  form: string;
  /** Tuning the chromagrams were built with, in cents: the given value, or the measured one under `tuningCents: 'auto'`. */
  tuningCents: number;
}

/** A tempo hypothesis with its confidence. */
export interface BpmCandidate {
  /** Tempo in beats per minute. */
  bpm: number;
  /** Winning tempo cluster's vote share, in `[0, 1]`. */
  confidence: number;
}

/** One tempo hypothesis in `AnalysisResult.bpmCandidates`: a `BpmCandidate` with its relation to the chosen tempo. */
export interface AnalysisBpmCandidate extends BpmCandidate {
  /** Relation to the chosen tempo. */
  relation: BpmCandidateRelation;
}

/** Result of `analyzeRhythm`. */
export interface RhythmResult {
  /** Estimated time signature. */
  timeSignature: {
    /** Beats per bar. */
    numerator: number;
    /** Note value of one beat: `4` is a quarter note. */
    denominator: number;
    /** Support for this signature in `[0, 1]`. */
    confidence: number;
  };
  /** Syncopation level in `[0, 1]`. */
  syncopation: number;
  /** Rhythmic feel of the track. */
  grooveType: GrooveType;
  /** Regularity of the beat intervals in `[0, 1]`; 0 with no beat interval. */
  patternRegularity: number;
  /** Tempo stability over time in `[0, 1]`; 0 below two beat intervals. */
  tempoStability: number;
  /**
   * The beat tracker's own tempo, refined from the local beat period. It is a
   * third figure rather than either tempo entry point's: measured against
   * synthesized click trains it differs from both at every sample rate, and
   * lands closer to the known tempo than either. Analysed at the sample rate
   * you pass: `nFft` is a window length in samples at 22050 Hz, rescaled to
   * that rate, and `hopLength` is in samples of your buffer.
   */
  bpm: number;
  /** Intervals between consecutive beats, in seconds. */
  beatIntervals: Float32Array;
}

/**
 * Dynamics metrics returned by {@link analyzeDynamics}.
 *
 * The Node package declares the same shape under the same name; the two are
 * pinned to one field list by `tests/conformance/shared_type_shapes.json`.
 */
export interface DynamicsResult {
  /** Dynamic range in dB. `+Infinity` when the low percentile window is silent; absent when not measured. */
  dynamicRangeDb?: number;
  /** Peak level in dB. `-Infinity` for silence. Never absent: every input sample measures it. */
  peakDb: number;
  /** RMS level in dB. `-Infinity` for silence. Never absent: every input sample measures it. */
  rmsDb: number;
  /** Peak-to-RMS ratio of the signal. Absent when not measured. */
  crestFactor?: number;
  /**
   * Loudness range in LU (EBU R128). Absent for silence and when no 3 s block passes the gates:
   * a spread, not a level, so never `-Infinity`.
   */
  loudnessRangeLu?: number;
  /** True when the signal is flagged as compressed. */
  isCompressed: boolean;
  /** Loudness curve timestamps in seconds, parallel to {@link loudnessRmsDb}. */
  loudnessTimesSec: Float32Array;
  /** Loudness curve RMS values (dB), parallel to {@link loudnessTimesSec}. `-Infinity` for an all-zero window. */
  loudnessRmsDb: Float32Array;
}

/** Timbre metrics for one analysis window. Entries are ordered by time in `timbreOverTime`. */
export interface TimbreFrame {
  /** Spectral brightness in `[0, 1]`; high is bright. */
  brightness: number;
  /** Spectral warmth in `[0, 1]`; high is warm. */
  warmth: number;
  /** Spectral density in `[0, 1]`; high is rich. */
  density: number;
  /** Spectral roughness in `[0, 1]`; high is rough. */
  roughness: number;
  /** Harmonic complexity in `[0, 1]`; high is complex. */
  complexity: number;
}

/** Result of `analyzeTimbre`: overall timbre metrics and per-frame spectral curves. */
export interface TimbreResult extends TimbreFrame {
  /** Spectral centroid per analysis frame, in Hz. */
  spectralCentroid: Float32Array;
  /** Spectral flatness per analysis frame, in `[0, 1]`. */
  spectralFlatness: Float32Array;
  /** Spectral roll-off frequency per analysis frame, in Hz. */
  spectralRolloff: Float32Array;
  /** Time-varying timbre metrics, one entry per analysis window. */
  timbreOverTime: TimbreFrame[];
}
