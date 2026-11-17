import type { ChordQuality, Key, Matrix2D, PitchClass } from './types_analysis.js';
import type { ValidateOptions } from './validation.js';

/** Constant-Q / Variable-Q transform magnitude result. */
export interface CqtResult {
  /** Hop length in samples. */
  hopLength: number;
  /** Sample rate in Hz. */
  sampleRate: number;
  /** Magnitude matrix: rows = CQT bins, cols = frames. */
  magnitude: Matrix2D;
  /** Center frequency (Hz) of each CQT bin (`magnitude.rows` entries). */
  frequencies: Float32Array;
}

/**
 * Construction options for {@link StreamAnalyzer}. Mirrors `sonare::StreamConfig`.
 *
 * Every count-valued field (`sampleRate`, `nFft`, `hopLength`, `nMels`,
 * `emitEveryNFrames`, `magnitudeDownsample`, `maxPendingFrames`,
 * `maxProgressionEntries`) must be an integer the native type can hold; a
 * fractional or out-of-range value is rejected (`RangeError`) rather than
 * truncated.
 */
export interface StreamConfig {
  /** Input rate in Hz. Required. */
  sampleRate: number;
  /**
   * Analysis window as a duration, in samples at 44100 Hz. Below that rate the
   * window is rescaled to the analysis rate, never shorter than `hopLength`. The
   * emitted bin count follows the FFT length the window is zero-padded to, not
   * `nFft / 2 + 1`.
   */
  nFft?: number;
  hopLength?: number;
  nMels?: number;
  /** Lower mel band edge in Hz. */
  fminHz?: number;
  /** Upper mel band edge in Hz. Absent means the Nyquist; an explicit 0 is refused (`RangeError`). */
  fmaxHz?: number;
  /** A4 reference in Hz. Defaults to 440; must be within 220..880, the same range `setReferenceHz` accepts live. */
  referenceHz?: number;
  computeMel?: boolean;
  computeChroma?: boolean;
  computeOnset?: boolean;
  computeSpectral?: boolean;
  emitEveryNFrames?: number;
  magnitudeDownsample?: number;
  /** Maximum unread frames; overflow drops the newly produced frame. */
  maxPendingFrames?: number;
  /** Maximum retained chord and bar progression entries; overflow drops oldest. */
  maxProgressionEntries?: number;
  keyUpdateIntervalSec?: number;
  bpmUpdateIntervalSec?: number;
  /** Analysis window. Default `'hann'`. */
  window?: WindowType;
}

/**
 * Frames read from a {@link StreamAnalyzer} (`readFrames`). A feature array is
 * absent when its feature was not computed.
 */
export interface StreamFrames {
  nFrames: number;
  /** Per-frame time in seconds. */
  timestampsSec: Float32Array;
  /**
   * Mel spectrogram in LINEAR power (not dB), the raw per-frame mel energies:
   * rows = mel bands, cols = frames. The quantized read paths (`readFramesU8` /
   * `readFramesI16`) convert to dB before packing, so their `mel` is dB-scaled.
   */
  mel?: Matrix2D;
  /** Chroma: rows = chroma bins, cols = frames. */
  chroma?: Matrix2D;
  onsetStrength?: Float32Array;
  rmsEnergy?: Float32Array;
  spectralCentroid?: Float32Array;
  spectralFlatness?: Float32Array;
  chordRoot?: Int32Array;
  /** Chord quality ordinals; decode each through `CHORD_QUALITIES`. */
  chordQuality?: Int32Array;
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

/** Quantized (uint8) frame buffer (`StreamAnalyzer.readFramesU8`); a feature array is absent when not computed. */
export interface StreamFramesU8 {
  nFrames: number;
  nMels: number;
  nChroma: number;
  /** Per-frame time in seconds. */
  timestampsSec: Float32Array;
  /** Band-major `[nMels x nFrames]` mel in dB, quantized over `[melDbMin, melDbMax]`. */
  mel?: Uint8Array;
  /** Band-major `[nChroma x nFrames]` chroma. */
  chroma?: Uint8Array;
  onsetStrength?: Uint8Array;
  rmsEnergy?: Uint8Array;
  spectralCentroid?: Uint8Array;
  spectralFlatness?: Uint8Array;
}

/** Quantized (int16) frame buffer (`StreamAnalyzer.readFramesI16`); a feature array is absent when not computed. */
export interface StreamFramesI16 {
  nFrames: number;
  nMels: number;
  nChroma: number;
  /** Per-frame time in seconds. */
  timestampsSec: Float32Array;
  /** Band-major `[nMels x nFrames]` mel in dB, quantized over `[melDbMin, melDbMax]`. */
  mel?: Int16Array;
  /** Band-major `[nChroma x nFrames]` chroma. */
  chroma?: Int16Array;
  onsetStrength?: Int16Array;
  rmsEnergy?: Int16Array;
  spectralCentroid?: Int16Array;
  spectralFlatness?: Int16Array;
}

/** A chord change in a progressive estimate. */
export interface StreamChordChange {
  root: PitchClass;
  quality: ChordQuality;
  /** Chord symbol (e.g. `C`, `C#m`, `N.C.`), spelled as the batch analysis spells it. */
  name: string;
  /** Start of the chord in seconds. */
  startSec: number;
  confidence: number;
}

/** A per-bar chord in a progressive estimate. */
export interface StreamBarChord {
  /**
   * Bar number, not the index of this entry in the array: bars with no
   * confident chord are not recorded and the oldest entries are dropped at the
   * history cap. Group bars by pattern position with this, never with the array
   * index. In `votedPattern` it is the pattern position instead.
   */
  barIndex: number;
  root: PitchClass;
  quality: ChordQuality;
  /** Chord symbol, spelled as the batch analysis spells it. */
  name: string;
  /**
   * Start of the bar in seconds, on the same timeline as `timestampsSec`
   * (including a `processWithOffset` anchor). Consecutive bars are
   * `barDurationSec` apart rather than snapped to the analysis frame grid.
   * Unused in `votedPattern`.
   */
  startSec: number;
  confidence: number;
}

/** A named chord-pattern match score. */
export interface StreamPatternScore {
  name: string;
  score: number;
}

/**
 * Progressive (incremental) musical estimate from {@link StreamAnalyzer.stats}.
 * A field is absent until its quantity has been estimated.
 */
export interface StreamProgressiveEstimate {
  bpm?: number;
  bpmConfidence?: number;
  /**
   * Tempo candidates the most recent BPM estimate chose from; 0 until an
   * estimate has run. Same quantity as the batch analysis result's field of the
   * same name.
   */
  bpmCandidateCount: number;
  key?: Key;
  /** The current chord; absent while no chord is detected. */
  chord?: StreamChordChange;
  chordProgression: StreamChordChange[];
  barChordProgression: StreamBarChord[];
  /** Current bar number; absent until a bar is detected. */
  currentBar?: number;
  /** Bar length in seconds; absent until a bar is detected. */
  barDurationSec?: number;
  votedPattern: StreamBarChord[];
  patternLength: number;
  /** Absent when no pattern is detected. */
  detectedPatternName?: string;
  /** Absent when no pattern is detected. */
  detectedPatternScore?: number;
  allPatternScores: StreamPatternScore[];
  /** Audio accumulated so far, in seconds. */
  accumulatedSec: number;
  usedFrames: number;
  /**
   * True when the key or BPM was re-estimated since the previous stats
   * snapshot. One change sets it on exactly one snapshot however the caller
   * chunks its input, and a call that produced no frame does not repeat it.
   */
  updated: boolean;
}

/** Snapshot returned by {@link StreamAnalyzer.stats}. */
export interface StreamStats {
  totalFrames: number;
  totalSamples: number;
  durationSec: number;
  pendingFrames: number;
  droppedOutputFrames: number;
  droppedChordProgressionEntries: number;
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
  estimate: StreamProgressiveEstimate;
}

/** Window shape for the streaming analyzer and spectral edits. */
export type WindowType = 'hann' | 'hamming' | 'blackman' | 'rectangular';

export interface StftResult {
  nFft: number;
  hopLength: number;
  sampleRate: number;
  /** Magnitude matrix: rows = `nFft / 2 + 1` bins, cols = frames. */
  magnitude: Matrix2D;
  /** Power matrix: rows = `nFft / 2 + 1` bins, cols = frames. */
  power: Matrix2D;
}

/**
 * Mel spectrogram, with the parameters of the forward transform, so the result
 * alone says how to invert it: pass `power` to `melToStft` or `melToAudio` with
 * the same `sampleRate`, `nFft`, `fminHz`, `fmaxHz` and `htk`.
 */
export interface MelSpectrogramResult {
  sampleRate: number;
  hopLength: number;
  /** FFT size of the source STFT. */
  nFft: number;
  /** Lower Mel band edge in Hz as applied. */
  fminHz: number;
  /** Upper Mel band edge in Hz as applied: the Nyquist when the request left it out. */
  fmaxHz: number;
  /** `true` for the HTK Mel formula, `false` for Slaney. */
  htk: boolean;
  /** Linear mel power: rows = mel bands, cols = frames. */
  power: Matrix2D;
  /** The dB form of `power`: rows = mel bands, cols = frames. */
  db: Matrix2D;
}

/**
 * MFCCs, with the Mel parameters they were derived with and the lifter, so the
 * result alone says how to invert it: pass `coefficients` to `mfccToMel` or
 * `mfccToAudio` with the same `nMels`, `lifter` and Mel parameters.
 */
export interface MfccResult {
  sampleRate: number;
  hopLength: number;
  nFft: number;
  /** Mel bands the coefficients were taken from. */
  nMels: number;
  fminHz: number;
  /** Upper Mel band edge in Hz as applied: the Nyquist when the request left it out. */
  fmaxHz: number;
  htk: boolean;
  /** Cepstral lifter applied to the coefficients; 0 means none. */
  lifter: number;
  /** Coefficients: rows = coefficients, cols = frames. */
  coefficients: Matrix2D;
}

export interface ChromaResult {
  sampleRate: number;
  hopLength: number;
  /** Chromagram: rows = chroma bins, cols = frames. */
  features: Matrix2D;
  meanEnergy: number[];
}

export interface PitchResult {
  f0: Float32Array;
  /**
   * pYIN's per-frame voiced **observation mass**, exactly as librosa returns
   * it: the summed probability of the frame's voiced pitch hypotheses.
   *
   * This is NOT a signal-quality confidence and NOT a correction weight. The
   * mass depends on how many periods of the pitch fit inside `frameLength`,
   * because the CMNDF troughs of a long period measured over a short frame are
   * shallower. For a fixed `frameLength` it therefore rises with F0 even when
   * the signal is unchanged: a steady three-harmonic tone at 2048 samples /
   * 48 kHz averages well under 0.1 at C2 and about 0.5 at C5, with every frame
   * flagged voiced throughout.
   *
   * Use {@link voicedFlag} for any voicing decision. In particular, thresholding
   * this value at a fixed 0.5 (`noteSegments`' default) drops entire low
   * registers.
   */
  voicedProb: Float32Array;
  /** Per-frame voicing decision from the Viterbi path — the voicing oracle. */
  voicedFlag: boolean[];
  nFrames: number;
  medianF0: number;
  meanF0: number;
}

/** Per-bin pitch candidates and peak magnitudes from spectral piptrack. */
export interface PiptrackResult {
  /** Pitch candidate (Hz) per bin: rows = FFT bins, cols = frames. */
  pitches: Matrix2D;
  /** Peak magnitude per bin: rows = FFT bins, cols = frames. */
  magnitudes: Matrix2D;
}

/** Magnitude and reassigned time/frequency coordinates for each STFT bin. */
export interface ReassignedSpectrogramResult {
  /** Magnitude: rows = FFT bins, cols = frames. */
  magnitude: Matrix2D;
  /** Reassigned times (s): rows = FFT bins, cols = frames. */
  times: Matrix2D;
  /** Reassigned frequencies (Hz): rows = FFT bins, cols = frames. */
  frequencies: Matrix2D;
}

/** One stable monophonic note region segmented from an F0 track. */
export interface NoteSegment {
  frameStart: number;
  frameEnd: number;
  startSec: number;
  endSec: number;
  medianCents: number;
}

/**
 * Why {@link splitSilenceCommonWithReport} found the gaps it did.
 *
 * One interval covering everything is the answer to three different situations
 * and the interval list cannot separate them: no take has a quiet moment at
 * all, the takes each have one but not in the same place, or `topDb` was set
 * too loose to see the ones they have.
 *
 * Read `silenceCeilingDb` against the `topDb` that was passed, which is the
 * whole decision:
 *
 * - ceiling near `0` — a take is sounding continuously. No threshold helps, and
 *   a cut point has to come from somewhere other than silence.
 * - ceiling below `topDb` — the threshold was too loose to see the quiet these
 *   takes do have. A `topDb` under the reported ceiling finds it.
 * - ceiling at or above `topDb`, and still one interval — every take shows
 *   silence at this setting and they do not share any of it. That is the
 *   alignment case, and what {@link alignTakeToReference} is for.
 *
 * The figures come from the same RMS pass the intervals do, so they can never
 * describe a different measurement.
 */
export interface SilenceCommonReport {
  /**
   * The largest `topDb` at which EVERY signal still shows silence.
   *
   * A frame counts as silent when it sits at least `topDb` under its own
   * signal's peak RMS, so each signal's deepest dip decides whether any
   * threshold can find silence in it, and the union needs all of them quiet at
   * once — hence the minimum across the signals. `0` for an all-silent signal,
   * where the peak is 0 and the ratio has no value; `120` is the floor the dB
   * conversion clamps at, reported for a signal holding a zero-valued frame.
   */
  silenceCeilingDb: number;
  /**
   * How many intervals the most fragmented signal produced alone.
   *
   * Counted before the union merges anything, and a measure of shape rather
   * than of cause: `1` is a take that sounds once and stops, so a take that is
   * loud then silent counts `1` exactly as a take with no silence does. Use
   * `silenceCeilingDb` to tell those apart; use these two to see whether any
   * take has an interior gap at all (`>= 2`) and whether the takes differ in
   * how broken up they are (`maxSignalIntervals !== minSignalIntervals`).
   */
  maxSignalIntervals: number;
  /** How many intervals the least fragmented signal produced alone. */
  minSignalIntervals: number;
}

/** Result of {@link splitSilenceCommonWithReport}: the shared intervals and why they are the gaps. */
export interface SplitSilenceCommonWithReportResult {
  /** Flat `[start, end, ...]` sample pairs. */
  intervals: Int32Array;
  report: SilenceCommonReport;
}

/** Equalizer band type. */
export type EqBandType =
  | 'peak'
  | 'lowShelf'
  | 'highShelf'
  | 'lowPass'
  | 'highPass'
  | 'bandPass'
  | 'notch'
  | 'tiltShelf'
  | 'flatTilt'
  | 'allPass';

/** Biquad coefficient design mode. */
export type EqCoeffMode = 'rbj' | 'vicanek';

/** Per-band phase behaviour. */
export type EqBandPhase = 'inherit' | 'zeroLatency' | 'naturalPhase' | 'linearPhase';

/** Trimmed signal and its bounds, in samples of the input. */
export interface TrimResult {
  audio: Float32Array;
  startSample: number;
  endSample: number;
}

/** Phase processing mode for the streaming equalizer. */
export type EqPhaseMode = 'zero' | 'natural' | 'linear';

/**
 * Single equalizer band passed to {@link StreamingEqualizer.setBand}.
 *
 * Only `type`/`frequencyHz`/`gainDb`/`enabled` are commonly needed; the
 * remaining fields fall back to processor defaults.
 */
/** Which signal path an EQ band sits on. */
export type EqStereoPlacement = 'Stereo' | 'Left' | 'Right' | 'Mid' | 'Side';

export interface EqBandInput {
  type?: EqBandType;
  frequencyHz?: number;
  gainDb?: number;
  /**
   * Resonance/slope. Ignored for a LowShelf/HighShelf band once `phase:
   * 'naturalPhase'` forces Vicanek coefficients (the Vicanek matched-Z shelf
   * design has no Q/S parameter) -- the value is still stored and read back
   * verbatim, so a reflected `q` on such a band does not describe the applied
   * response. Set `coeffMode: 'rbj'` for a Q-controllable shelf.
   */
  q?: number;
  enabled?: boolean;
  coeffMode?: EqCoeffMode;
  /**
   * Slope in dB/octave. 6 selects a first-order section on a pass or shelf
   * band; a first-order shelf's frequency is its half-gain point and its `q`
   * is unused.
   */
  slopeDbOct?: number;
  placement?: EqStereoPlacement;
  /**
   * `'naturalPhase'` forces `coeffMode: 'vicanek'` for this band, which
   * ignores `q` for LowShelf/HighShelf (see `q`'s doc).
   */
  phase?: EqBandPhase;
  soloed?: boolean;
  bypassed?: boolean;
  proportionalQ?: boolean;
  proportionalQStrength?: number;
  dynamic?: boolean;
  thresholdDb?: number;
  autoThreshold?: boolean;
  ratio?: number;
  rangeDb?: number;
  /** Attack in ms: one time constant, about 63% of the way at the set time. */
  attackMs?: number;
  /** Release in ms: one time constant, about 63% of the way at the set time. */
  releaseMs?: number;
  /**
   * Delays the detector's view of the signal by this many ms; a larger value
   * makes the band react LATER, not earlier -- this is a detector delay, not
   * true look-ahead, and adds no latency to the audio path.
   */
  detectorDelayMs?: number;
  externalSidechain?: boolean;
  sidechainFreqHz?: number;
  sidechainQ?: number;
}

/** Options for {@link StreamingEqualizer.match}. */
export interface EqMatchOptions {
  /** Rate of `source`, in Hz. Omitted: the equalizer's own rate. */
  sampleRate?: number;
  /**
   * Rate of `reference`, in Hz. Omitted: `sampleRate`. A reference at another
   * rate is resampled onto the source's before its spectrum is taken.
   */
  referenceSampleRate?: number;
  /** Most bands the match may set; at least 1. Default 8. */
  maxBands?: number;
}

/**
 * Realtime-safe snapshot from {@link StreamingEqualizer.spectrum}.
 *
 * `preLeft`/`preRight` and `postLeft`/`postRight` are the pre- and post-EQ
 * waveform streams (uniformly decimated time-domain samples), so they are a
 * scope feed rather than a spectral estimate. `profileDb` is the frequency-domain
 * view: the post-EQ signal is Hann-windowed, transformed and its bin powers
 * summed into 16 geometrically spaced bands covering 20 Hz to 20 kHz, in
 * amplitude decibels relative to full scale, rising immediately and falling
 * smoothly.
 */
export interface EqSpectrumSnapshot {
  preLeft: Float32Array;
  preRight: Float32Array;
  postLeft: Float32Array;
  postRight: Float32Array;
  bandGainDb: Float32Array;
  profileDb: Float32Array;
  lastAutoGainDb: number;
  seq: number;
}

export interface LufsResult {
  integratedLufs: number;
  /** Final complete 400 ms window, not Max-M. */
  momentaryLufs: number;
  /** Final complete 3 s window, not Max-S. */
  shortTermLufs: number;
  /** Maximum 400 ms window (EBU R128 Max-M). */
  maxMomentaryLufs: number;
  /** Maximum 3 s window (EBU R128 Max-S). */
  maxShortTermLufs: number;
  /** Loudness range in LU. */
  loudnessRangeLu: number;
}

/**
 * The two per-block loudness series a multi-channel measurement builds, in
 * LUFS. A signal shorter than a window yields an empty series for it.
 */
export interface LufsSeriesResult {
  /** 400 ms momentary series. */
  momentary: Float32Array;
  /** 3 s short-term series. */
  shortTerm: Float32Array;
}

/**
 * How a {@link SpectralRegionOp} modifies the masked STFT bins. Mirrors the C
 * `SonareSpectralEditMode`:
 * - `'gain'`: multiply magnitude by `10^(gainDb/20)` (phase kept).
 * - `'attenuate'`: gain with a (typically negative) `gainDb`.
 * - `'mute'`: hard-zero the masked bins (`gainDb` ignored).
 * - `'heal'`: tonal continuation from neighbouring time frames.
 */
export type SpectralEditMode = 'gain' | 'attenuate' | 'mute' | 'heal';

/**
 * One time x frequency rectangle edit op for {@link spectralEdit}. Ops apply in
 * array order. Mirrors the C `SonareSpectralRegionOp`.
 */
export interface SpectralRegionOp {
  /** Region time start in input samples (clamped to `[0, length]`). Default 0. Not with `startSec`. */
  startSample?: number;
  /** Region time start in seconds, rounded to the nearest sample. Not with `startSample`. */
  startSec?: number;
  /**
   * Region time end (exclusive) in input samples. Omit for the end of the signal;
   * a negative value is refused. Not with `endSec`.
   */
  endSample?: number;
  /** Region time end in seconds, rounded to the nearest sample. Not with `endSample`. */
  endSec?: number;
  /** Region frequency low edge in Hz (clamped to `[0, nyquist]`). Default 0. */
  lowHz?: number;
  /** Region frequency high edge in Hz; `<= 0` or `>= nyquist` means nyquist. Default 0. */
  highHz?: number;
  /** Gain in dB for `'gain'` / `'attenuate'`; ignored by `'mute'` / `'heal'`. Default 0. */
  gainDb?: number;
  /** How the op modifies the masked bins. Default `'gain'`. */
  mode?: SpectralEditMode;
}

/**
 * STFT + heal parameters for {@link spectralEdit}. All fields are optional;
 * omitted fields take the documented native defaults. Mirrors the C
 * `SonareSpectralEditConfig`.
 */
export interface SpectralEditOptions extends ValidateOptions {
  /** Region edit ops applied in array order; an empty array returns a copy of the input. */
  ops: readonly SpectralRegionOp[];
  /** FFT size; a power of two in `[4, 262144]`. Default 2048. */
  nFft?: number;
  /** Hop length; must satisfy `0 < hop <= nFft / 2`. Default 512. */
  hopLength?: number;
  /** Analysis/synthesis window. Default `'hann'`. */
  window?: WindowType;
  /** Neighbour frames each side used by `'heal'`. Default 2. */
  healRadiusFrames?: number;
}
