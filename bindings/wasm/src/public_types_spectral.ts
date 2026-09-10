import type { ValidateOptions } from './validation';

/**
 * Per-frame voicing decision, one entry per `f0Hz` frame. A truthy or non-zero
 * entry marks the frame voiced. The union covers what the analysis side hands
 * back — `PitchResult.voicedFlag` is a `boolean[]` — as well as the typed and
 * plain numeric arrays a caller may build directly, so a pitch track can be fed
 * straight into pitch correction without a conversion step.
 */
export type VoicedFlags =
  | Int32Array
  | Uint8Array
  | Float32Array
  | readonly number[]
  | readonly boolean[];

/** Options for `pitchCorrectTimevarying`. All fields are optional. */
export interface PitchCorrectOptions extends ValidateOptions {
  /** `'midi'` retunes toward `targetMidi`; `'scale'` snaps to the key. Default `'midi'`. */
  mode?: 'midi' | 'scale';
  /** Fixed target note when `mode` is `'midi'`, in `[0, 127]`. Default 69 (A4). */
  targetMidi?: number;
  /** Scale root pitch class (0=C .. 11=B) when `mode` is `'scale'`. Default 0. */
  scaleRoot?: number;
  /** 12-bit degree mask, bit `i` = semitone `i` above the root enabled. Default C major. */
  scaleModeMask?: number;
  /** Reference MIDI anchoring the scale grid. Default 69 (A4). */
  referenceMidi?: number;
  /** Correction strength in `[0, 1]`; 1 = full snap, 0 = bypass. Default 1. */
  retuneAmount?: number;
  /** Hard clamp on per-frame correction magnitude (semitones). Default 12. */
  maxCorrectionSemitones?: number;
  /** Retune IIR time constant (ms); larger = slower glide. Default 50. */
  retuneSpeedMs?: number;
  /** Corrections below this are bypassed to preserve vibrato (cents). Default 20. */
  vibratoThresholdCents?: number;
  /** Per-frame voiced flags (truthy = voiced); omit to treat all frames as voiced. */
  voiced?: VoicedFlags;
  /** Per-frame voicing probability in `[0, 1]`; omit to derive from `voiced`. */
  voicedProb?: Float32Array;
}

/** Options for `noteStretch`. All fields are optional. */
export interface NoteStretchOptions {
  /** Note onset position in samples (selects the region). Default 0. */
  onsetSample?: number;
  /** Note offset position in samples (selects the region). Defaults to the input length. */
  offsetSample?: number;
  /** Stretch ratio (0.5 = half duration, 2.0 = double duration). Default 1. */
  stretchRatio?: number;
}

/** Options for `noteMove`. */
export interface NoteMoveOptions {
  onsetSample?: number;
  /** Defaults to the input length. */
  offsetSample?: number;
  targetOnsetSample?: number;
}

/** Segmentation tuning for `extractNotes`. All fields are optional; 0 or absent takes the default. */
export interface NoteExtractorOptions {
  /** Cents of pitch change that start a new note. Default 50. */
  segmentationThresholdCents?: number;
  /** Shortest span kept as a note, in ms. Default 30. */
  minNoteMs?: number;
  /** Reference pitch the `medianCents` of each note is measured against. Default 440. */
  referenceHz?: number;
  /**
   * Value of `voicedProb` at or above which a frame counts as voiced, in
   * `[0, 1]`. Read only when `voiced` is omitted. Default 0.5.
   *
   * pYIN's `voicedProb` is a frame's voiced observation mass and rises with F0
   * for a fixed frame length, so this default silently drops low registers —
   * pass `pitchPyin`'s `voicedFlag` through `voiced` instead.
   */
  voicedThreshold?: number;
}

/**
 * A pending, non-destructive change to one note. `extractNotes` attaches the
 * identity edit (no move, no transpose, unity gain and stretch, unmuted) to
 * every note it returns; `renderNotes` applies whatever the caller has changed.
 */
export interface NoteEdit {
  /**
   * Moves the note along the timeline; negative moves it earlier. Where the note
   * lands is not bounds-checked, so a moved note may overwrite a neighbour.
   */
  timeOffsetSamples: number;
  /** Transpose applied to the note's span. */
  pitchShiftSemitones: number;
  /** Level change applied to the note's span. */
  gainDb: number;
  /** `>1` lengthens the note, `<1` shortens it; pitch is preserved. 0 reads as 1. */
  timeStretchRatio: number;
  /** Silences the note's span; the other fields then do not apply. */
  muted: boolean;
}

/**
 * A {@link NoteEdit} as supplied to `renderNotes`. Every field is optional and
 * an omitted one is the identity, so `{}` leaves the note untouched.
 */
export type NoteEditInput = Partial<NoteEdit>;

/**
 * One editable note returned by `extractNotes`.
 *
 * Sample bounds are half-open into the source audio; frame bounds are half-open
 * into the caller's own `f0Hz` track. The per-note F0 curve is deliberately not
 * repeated here — it is `f0Hz.subarray(frameStart, frameEnd)`.
 *
 * `onsetSample` and `offsetSample` are 64-bit on the core side and arrive as JS
 * numbers, which are exact up to `Number.MAX_SAFE_INTEGER`.
 */
export interface NoteObject {
  /** First sample of the note's span. */
  onsetSample: number;
  /** One past the last sample of the span. */
  offsetSample: number;
  /** First frame of the span in the caller's `f0Hz` track. */
  frameStart: number;
  /** One past the last frame of the span. */
  frameEnd: number;
  /** Median measured pitch over the span, in Hz. */
  medianHz: number;
  /** Median pitch in cents above the request's `referenceHz`. */
  medianCents: number;
  /** Pitch steadiness in `[0, 1]`; 1 is perfectly steady. */
  f0Stability: number;
  /** One RMS value per frame of the span (`frameEnd - frameStart` entries). */
  amplitude: Float32Array;
  /** This note's pending edit; the identity as returned. */
  edit: NoteEdit;
}

/**
 * A note handed to `renderNotes`. Only the span and the edit are read, so a
 * {@link NoteObject} straight from `extractNotes` can be passed back with its
 * `edit` changed and nothing else.
 */
export interface NoteObjectInput {
  /** First sample of the note's span. */
  onsetSample: number;
  /** One past the last sample of the span. */
  offsetSample: number;
  /** Omit for the identity edit. */
  edit?: NoteEditInput;
}

/** How a `spectralEdit` region op modifies the masked bins. */
export type SpectralEditMode = 'gain' | 'attenuate' | 'mute' | 'heal';

/** Analysis/synthesis window used by `spectralEdit`. */
export type SpectralEditWindow = 'hann' | 'hamming' | 'blackman' | 'rectangular' | 'rect';

/** One time x frequency rectangle edit op for `spectralEdit`. */
export interface SpectralRegionOp {
  /** Region time start (input samples); clamped to [0, length]. Default 0. */
  startSample?: number;
  /** Region time end, exclusive (input samples); clamped to [0, length]. Default = signal length. */
  endSample?: number;
  /** Region frequency low edge in Hz; clamped to [0, nyquist]. Default 0. */
  lowHz?: number;
  /** Region frequency high edge in Hz; <=0 or >= nyquist means nyquist. Default 0. */
  highHz?: number;
  /** Linear gain in dB for 'gain'/'attenuate'; ignored by 'mute'/'heal'. Default 0. */
  gainDb?: number;
  /** Edit mode. Default 'gain'. */
  mode?: SpectralEditMode;
}

/** STFT + heal parameters for `spectralEdit`. All fields are optional. */
export interface SpectralEditOptions {
  /** FFT size; must be a power of two (>= 2). Default 2048. */
  nFft?: number;
  /** Hop length; must satisfy 0 < hop <= nFft/2. Default 512. */
  hopLength?: number;
  /** Analysis + synthesis window. Default 'hann'. */
  window?: SpectralEditWindow;
  /** Neighbour frames each side used by 'heal' (>= 1). Default 2. */
  healRadiusFrames?: number;
}

/**
 * Constant-Q / Variable-Q transform magnitude result (mirrors the C
 * `SonareCqtResult`).
 */
export interface CqtResult {
  /** Number of frequency bins. */
  nBins: number;
  /** Number of time frames. */
  nFrames: number;
  /** Hop length in samples. */
  hopLength: number;
  /** Sample rate in Hz. */
  sampleRate: number;
  /** Row-major `[nBins x nFrames]` magnitude matrix. */
  magnitude: Float32Array;
  /** Center frequency (Hz) of each of the `nBins` bins. */
  frequencies: Float32Array;
}

/**
 * Multi-band onset strength matrix result.
 */
export interface OnsetStrengthMultiResult {
  nBands: number;
  nFrames: number;
  data: Float32Array;
}

/**
 * STFT (Short-Time Fourier Transform) result
 */
export interface StftResult {
  nBins: number;
  nFrames: number;
  nFft: number;
  hopLength: number;
  sampleRate: number;
  magnitude: Float32Array;
  power: Float32Array;
}

/**
 * Mel spectrogram result
 */
export interface MelSpectrogramResult {
  nMels: number;
  nFrames: number;
  sampleRate: number;
  hopLength: number;
  power: Float32Array;
  db: Float32Array;
}

/**
 * MFCC result
 */
export interface MfccResult {
  nMfcc: number;
  nFrames: number;
  coefficients: Float32Array;
}

/**
 * STFT power spectrogram result (from inverse Mel reconstruction)
 */
export interface StftPowerResult {
  nBins: number;
  nFrames: number;
  power: Float32Array;
}

/**
 * Mel power spectrogram result (from inverse MFCC reconstruction)
 */
export interface MelPowerResult {
  nMels: number;
  nFrames: number;
  power: Float32Array;
}

/**
 * Chroma features result
 */
export interface ChromaResult {
  nChroma: number;
  nFrames: number;
  sampleRate: number;
  hopLength: number;
  features: Float32Array;
  meanEnergy: number[];
}

/**
 * Pitch detection result
 */
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
  nBins: number;
  nFrames: number;
  pitches: Float32Array;
  magnitudes: Float32Array;
}

/** Magnitude and reassigned time/frequency coordinates for every STFT bin. */
export interface ReassignedSpectrogramResult {
  nBins: number;
  nFrames: number;
  magnitude: Float32Array;
  times: Float32Array;
  frequencies: Float32Array;
}

/** Row-major matrix returned by librosa.segment-compatible APIs. */
export interface SegmentMatrix {
  rows: number;
  cols: number;
  values: Float32Array;
}

/** One stable monophonic note region segmented from an F0 track. */
export interface NoteSegment {
  frameStart: number;
  frameEnd: number;
  startSeconds: number;
  endSeconds: number;
  medianCents: number;
}

/**
 * Loudness measurement result (EBU R128 / ITU-R BS.1770)
 */
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
  loudnessRange: number;
}

/**
 * HPSS (Harmonic-Percussive Source Separation) result
 */
export interface HpssResult {
  harmonic: Float32Array;
  percussive: Float32Array;
  sampleRate: number;
}
