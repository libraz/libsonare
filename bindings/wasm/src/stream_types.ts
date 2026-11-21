import type { ChordQuality, Key, Matrix2D, PitchClass, WindowType } from './public_types.js';

/** A detected chord change in the progression. */
export interface StreamChordChange {
  /** Chord root pitch class. */
  root: PitchClass;
  /** Chord quality token. */
  quality: ChordQuality;
  /** Chord symbol (e.g. `C`, `C#m`), spelled as the batch analysis spells it. */
  name: string;
  /** Start of the chord in seconds. */
  startSec: number;
  /** Chord detection confidence in `[0, 1]`. */
  confidence: number;
}

/** A chord detected at a bar boundary (beat-synchronized). */
export interface StreamBarChord {
  /**
   * Bar number, not the index of this entry in the array: bars with no
   * confident chord are not recorded and the oldest entries are dropped at the
   * history cap. Group bars by pattern position with this, never with the array
   * index. In `votedPattern` it is the pattern position instead.
   */
  barIndex: number;
  /** Chord root pitch class. */
  root: PitchClass;
  /** Chord quality token. */
  quality: ChordQuality;
  /** Chord symbol, spelled as the batch analysis spells it. */
  name: string;
  /**
   * Start of the bar in seconds, on the same timeline as
   * `StreamFrames.timestampsSec` (including a `startSample` anchor).
   * Consecutive bars are `barDurationSec` apart rather than snapped to the
   * analysis frame grid. Unused in `votedPattern`.
   */
  startSec: number;
  /** Chord detection confidence in `[0, 1]`. */
  confidence: number;
}

/** Pattern score for known chord progressions. */
export interface StreamPatternScore {
  /** Name of the known chord progression pattern. */
  name: string;
  /** Match score in `[0, 1]`. */
  score: number;
}

/**
 * Progressive estimation results for BPM, key and chord. A field is absent
 * until the analyzer has an estimate for it.
 */
export interface StreamProgressiveEstimate {
  /** Tempo in BPM. */
  bpm?: number;
  /** BPM confidence in `[0, 1]`; grows as more audio accumulates. */
  bpmConfidence?: number;
  /**
   * Tempo candidates the most recent BPM estimate chose from; 0 until an
   * estimate has run. Same quantity as the batch analysis result's field of the
   * same name.
   */
  bpmCandidateCount: number;
  /** Estimated key. */
  key?: Key;
  /** Current chord. */
  chord?: StreamChordChange;
  /** Chord changes detected so far, including the current chord. */
  chordProgression: StreamChordChange[];
  /** Chord per bar, beat-synchronized; empty until a bar is detected. */
  barChordProgression: StreamBarChord[];
  /** Index of the bar being played; absent until a bar is detected. */
  currentBar?: number;
  /** Bar length in seconds; absent until a bar is detected. */
  barDurationSec?: number;
  /** Voted chord for each position of the repeating pattern. */
  votedPattern: StreamBarChord[];
  /** Length of the repeating chord pattern in bars. */
  patternLength: number;
  /** Name of the best-matching known progression pattern. */
  detectedPatternName?: string;
  /** Match score of the best pattern in `[0, 1]`. */
  detectedPatternScore?: number;
  /** Score of every known progression pattern. */
  allPatternScores: StreamPatternScore[];
  /** Seconds of audio the estimates have accumulated. */
  accumulatedSec: number;
  /** Number of analysis frames the estimates were computed from. */
  usedFrames: number;
  /**
   * True when the key or BPM was re-estimated since the previous stats
   * snapshot. One change sets it on exactly one snapshot however the caller
   * chunks its input, and a call that produced no frame does not repeat it.
   */
  updated: boolean;
}

/** Statistics and current state of the analyzer. */
export interface StreamStats {
  /** Frames produced so far. */
  totalFrames: number;
  /** Input samples processed so far. */
  totalSamples: number;
  /** Duration of the audio processed so far, in seconds. */
  durationSec: number;
  /** Frames produced but not yet read. */
  pendingFrames: number;
  /** New frames dropped because `maxPendingFrames` unread frames were already pending. */
  droppedOutputFrames: number;
  /** Chord changes dropped at the `maxProgressionEntries` cap. */
  droppedChordProgressionEntries: number;
  /** Bar chords dropped at the `maxProgressionEntries` cap. */
  droppedBarProgressionEntries: number;
  /**
   * Blocks in which a non-finite input sample was replaced before it could
   * reach the analyzer's recursive state.
   *
   * Unlike the drop counts above, nothing is missing from the output: every
   * estimate is produced as usual and simply stops describing the input, so
   * this is the only report that the stream was degraded. The unit is one
   * {@link StreamAnalyzer.process} call, never a sample, so a block carrying a
   * thousand NaNs adds one. Cleared by {@link StreamAnalyzer.reset} alongside
   * the drop counts, because that call rebuilds the timeline and the count
   * describes a segment rather than the analyzer.
   */
  nonFiniteDiscardBlocks: number;
  /** Current progressive BPM, key and chord estimates. */
  estimate: StreamProgressiveEstimate;
}

/**
 * Frames read from the analyzer. A feature array is absent when its feature is
 * not computed; the chord arrays follow `computeChroma`.
 */
export interface StreamFrames {
  /** Number of frames in this read. */
  nFrames: number;
  /** Per-frame time in seconds. */
  timestampsSec: Float32Array;
  /**
   * Mel spectrogram in LINEAR power (not dB), the raw per-frame mel energies:
   * rows are mel bands, cols are frames. The quantized read paths
   * (`readFramesU8` / `readFramesI16`) convert to dB before packing.
   */
  mel?: Matrix2D;
  /** Chromagram: rows are chroma bins, cols are frames. */
  chroma?: Matrix2D;
  /** Per-frame onset strength. */
  onsetStrength?: Float32Array;
  /** Per-frame RMS energy. */
  rmsEnergy?: Float32Array;
  /** Per-frame spectral centroid in Hz. */
  spectralCentroid?: Float32Array;
  /** Per-frame spectral flatness. */
  spectralFlatness?: Float32Array;
  /** Pitch class of each frame's chord, or -1 where none was detected. */
  chordRoot?: Int32Array;
  /** Chord quality ordinal per frame; `CHORD_QUALITIES[ordinal]` is its token. */
  chordQuality?: Int32Array;
  /** Per-frame chord confidence in `[0, 1]`. */
  chordConfidence?: Float32Array;
}

/**
 * Quantization ranges for the uint8/int16 bandwidth-reduction read paths
 * (`StreamAnalyzer.readFramesU8` / `readFramesI16`). Omitted fields fall back to
 * the library defaults shown below; widen any range whose source values exceed
 * the defaults, otherwise a louder/quieter stream saturates to the endpoints.
 */
export interface StreamQuantizeConfig {
  /** dB floor for mel quantization (default -80). */
  melDbMin?: number;
  /** dB ceiling for mel quantization (default 0). */
  melDbMax?: number;
  /** Max expected onset strength (default 50). */
  onsetMax?: number;
  /** Max expected RMS energy (default 1). */
  rmsMax?: number;
  /** Max expected spectral centroid in Hz (default 11025). */
  centroidMax?: number;
}

/** Frames quantized to uint8; a feature array is absent when not computed. */
export interface StreamFramesU8 {
  /** Number of frames in this read. */
  nFrames: number;
  /** Per-frame time in seconds. */
  timestampsSec: Float32Array;
  /** Band-major `[nMels x nFrames]` mel in dB, quantized over `[melDbMin, melDbMax]`. */
  mel?: Uint8Array;
  /** Band-major `[nChroma x nFrames]` chroma. */
  chroma?: Uint8Array;
  /** Per-frame onset strength, quantized to uint8. */
  onsetStrength?: Uint8Array;
  /** Per-frame RMS energy, quantized to uint8. */
  rmsEnergy?: Uint8Array;
  /** Per-frame spectral centroid, quantized to uint8 over `[0, centroidMax]`. */
  spectralCentroid?: Uint8Array;
  /** Per-frame spectral flatness, quantized to uint8. */
  spectralFlatness?: Uint8Array;
  /** Pitch class of each frame's chord, or -1 where none was detected. Present with `chroma`. */
  chordRoot?: Int8Array;
  /** Chord quality ordinal per frame; `CHORD_QUALITIES[ordinal]` is its token. */
  chordQuality?: Int8Array;
  /** Per-frame chord confidence, quantized to uint8 over `[0, 1]`. */
  chordConfidence?: Uint8Array;
}

/** Frames quantized to int16; a feature array is absent when not computed. */
export interface StreamFramesI16 {
  /** Number of frames in this read. */
  nFrames: number;
  /** Per-frame time in seconds. */
  timestampsSec: Float32Array;
  /** Band-major `[nMels x nFrames]` mel in dB, quantized over `[melDbMin, melDbMax]`. */
  mel?: Int16Array;
  /** Band-major `[nChroma x nFrames]` chroma. */
  chroma?: Int16Array;
  /** Per-frame onset strength, quantized to int16. */
  onsetStrength?: Int16Array;
  /** Per-frame RMS energy, quantized to int16. */
  rmsEnergy?: Int16Array;
  /** Per-frame spectral centroid, quantized to int16 over `[0, centroidMax]`. */
  spectralCentroid?: Int16Array;
  /** Per-frame spectral flatness, quantized to int16. */
  spectralFlatness?: Int16Array;
  /** Pitch class of each frame's chord, or -1 where none was detected. Present with `chroma`. */
  chordRoot?: Int8Array;
  /** Chord quality ordinal per frame; `CHORD_QUALITIES[ordinal]` is its token. */
  chordQuality?: Int8Array;
  /** Per-frame chord confidence, quantized to int16 over `[0, 1]`. */
  chordConfidence?: Int16Array;
}

/**
 * Configuration for StreamAnalyzer.
 *
 * An omitted field takes the library default. Integer fields must be integers;
 * an out-of-domain value is a RangeError.
 */
export interface StreamConfig {
  /** Sample rate in Hz. Required. */
  sampleRate: number;
  /**
   * Analysis window as a duration, in samples at 44100 Hz. Below that rate the
   * window is rescaled to the analysis rate, never shorter than `hopLength`. The
   * emitted bin count follows the FFT length the window is zero-padded to, not
   * `nFft / 2 + 1`.
   */
  nFft?: number;
  /** Hop between frames in samples at the analysis rate. Default `512`. */
  hopLength?: number;
  /** Number of mel bands. Default `128`. */
  nMels?: number;
  /** Lower edge of the mel band in Hz. */
  fminHz?: number;
  /** Upper edge of the mel band in Hz. Absent means Nyquist; an explicit 0 is refused. */
  fmaxHz?: number;
  /** A4 reference in Hz. Defaults to 440; must be within 220..880, the same range `setReferenceHz` accepts. */
  referenceHz?: number;
  /** Compute the mel spectrogram. Default `true`. */
  computeMel?: boolean;
  /** Compute the chromagram and chords. Default `true`. */
  computeChroma?: boolean;
  /** Compute onset strength, which also feeds the BPM estimate. Default `true`. */
  computeOnset?: boolean;
  /** Compute spectral features (centroid, flatness). Default `true`. */
  computeSpectral?: boolean;
  /** Emit every Nth frame, for throttling. Default `1`. */
  emitEveryNFrames?: number;
  /** Maximum unread frames; overflow drops the newly produced frame. */
  maxPendingFrames?: number;
  /** Maximum retained chord and bar progression entries; overflow drops oldest. */
  maxProgressionEntries?: number;
  /** Seconds between key re-estimates. Default `5`. */
  keyUpdateIntervalSec?: number;
  /** Seconds between BPM re-estimates. Default `10`. */
  bpmUpdateIntervalSec?: number;
  /** Analysis window function. Default `'hann'`. */
  window?: WindowType;
}
