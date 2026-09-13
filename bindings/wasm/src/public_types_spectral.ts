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
  /**
   * Moves the spectral envelope, in semitones, on top of whatever the pitch
   * shift already did to it.
   *
   * 0 runs no warp at all, so a pitch-only edit is not charged for an LPC
   * analysis-resynthesis round it did not ask for. A pitch shift drags the
   * formants with it, so holding them still is `-pitchShiftSemitones` and the
   * chipmunk is the default. Saturates near -10.3 and +8.7 semitones rather
   * than being rejected.
   */
  formantShiftSemitones: number;
  /**
   * Scales the vibrato measured over the note, stated as a change from it: 0
   * keeps it, -1 flattens it, +1 doubles it.
   *
   * Applying it needs the note's own pitch curve, so `renderNotes` must be given
   * the `f0Hz` track and the note its `frameStart` / `frameEnd` / `medianHz`; a
   * note carrying no usable pitch is rejected rather than left alone. The curve
   * is split at the request's `vibratoCutoffHz`.
   */
  vibratoDepthChange: number;
  /** The same, for the slow drift around the note's centre pitch. */
  driftChange: number;
  /** Silences the note's span; the other fields then do not apply. */
  muted: boolean;
  /**
   * Per-frame linear gain over the note's span, on top of `gainDb`.
   *
   * A set of gain points rather than a signal: it is stretched over whatever
   * length the note renders at, so it survives a time stretch and need not match
   * the note's frame count. One entry is a constant gain, and an empty array is
   * no envelope. Every value must be finite and non-negative.
   */
  amplitudeEnvelope: Float32Array;
}

/**
 * A {@link NoteEdit} as supplied to `renderNotes`, `splitNote` or `mergeNotes`.
 * Every field is optional and an omitted one is the identity, so `{}` leaves the
 * note untouched.
 *
 * `amplitudeEnvelope` is the one field that widens on the way in: a host
 * building an envelope in JS naturally ends up with a plain array, and the
 * points are copied into WASM memory either way. What comes back on a
 * {@link NoteEdit} is always a `Float32Array`.
 */
export type NoteEditInput = Omit<Partial<NoteEdit>, 'amplitudeEnvelope'> & {
  amplitudeEnvelope?: Float32Array | readonly number[];
};

/**
 * One editable note, returned by `extractNotes` and by
 * `PolyphonicAnalysis.notes()`.
 *
 * Sample bounds are half-open into the source audio. Frame bounds are half-open
 * into whichever framing found the note: the caller's own `f0Hz` track through
 * `extractNotes`, and the analysis's own STFT framing through a
 * `PolyphonicAnalysis`, whose `f0Hz` curve therefore comes from `noteF0(note)`
 * rather than from an array the caller holds.
 *
 * The per-note F0 curve is deliberately not repeated here — through the by-value
 * door it is `f0Hz.subarray(frameStart, frameEnd)`, and through a handle it has its
 * own accessor.
 *
 * `onsetSample` and `offsetSample` are 64-bit on the core side and arrive as JS
 * numbers, which are exact up to `Number.MAX_SAFE_INTEGER`.
 */
export interface NoteObject {
  /** First sample of the note's span. */
  onsetSample: number;
  /** One past the last sample of the span. */
  offsetSample: number;
  /** First frame of the span, in the framing that found the note. */
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
 * A note handed to `renderNotes`. Only the span, the edit and — for a
 * `vibratoDepthChange` or `driftChange` edit — the frame bounds and the centre
 * are read, so a {@link NoteObject} straight from `extractNotes` can be passed
 * back with its `edit` changed and nothing else.
 */
export interface NoteObjectInput {
  /** First sample of the note's span. */
  onsetSample: number;
  /** One past the last sample of the span. */
  offsetSample: number;
  /**
   * First frame of the span in the request's `f0Hz` track. Read only when a
   * track is given; a curve edit acts on `f0Hz.subarray(frameStart, frameEnd)`.
   */
  frameStart?: number;
  /** One past the last frame of the span, under the same rule. */
  frameEnd?: number;
  /** The note's centre pitch in Hz, which a curve edit measures its cents against. */
  medianHz?: number;
  /** Omit for the identity edit. */
  edit?: NoteEditInput;
}

/**
 * A note handed to `splitNote` or `mergeNotes`. Both re-derive every note in the
 * set from the audio and the track, so only the frame bounds and the edit are
 * read — and the frame bounds are therefore what a note is identified by.
 */
export interface NoteSetEntry {
  /** First frame of the span in the request's `f0Hz` track. */
  frameStart: number;
  /** One past the last frame of the span. */
  frameEnd: number;
  /** Omit for the identity edit. */
  edit?: NoteEditInput;
}

/**
 * One note's pitch curve split into a centre, a slow drift and a vibrato by
 * `decomposeNotePitch`.
 *
 * `driftCents[i] + vibratoCents[i]` is the note's own pitch at frame `i`, in
 * cents above `centreHz`, to within float rounding, so the three parts
 * reconstruct the curve. The drift filter is zero phase, so neither curve is
 * shifted in time against the audio.
 */
export interface PitchDecompositionResult {
  /**
   * The note's steady pitch in Hz. 0 when the note carries no usable pitch, and
   * then both curves are empty.
   */
  centreHz: number;
  /** Slow deviation from `centreHz` in cents, one entry per frame. */
  driftCents: Float32Array;
  /** Fast deviation in cents, over the same frames. */
  vibratoCents: Float32Array;
}

/**
 * Tuning for `analyzePolyphonic`. Every field is optional and 0 or absent takes
 * the documented default.
 *
 * Four fields accept 0 as a value as well as reading it as their default, and are
 * marked below: **pass a negative number to select 0 on those**. Each rejects a
 * negative otherwise, so the two meanings cannot collide.
 *
 * The window function and the centred framing are not settable. Every span, claim
 * and mask offset in this chain is derived against one framing, so a second way to
 * state it would be a second thing to keep in agreement.
 */
export interface PolyphonicAnalysisOptions {
  /** STFT size the whole chain runs in. Default 4096, the size it is tuned at. */
  nFft?: number;
  /** STFT hop in samples. Default 512. */
  hopLength?: number;
  /** Window length in samples. Defaults to `nFft`. */
  winLength?: number;
  /** Bottom of the cent axis the salience is folded onto, in Hz. Default 55. */
  centRefHz?: number;
  /** Cent-axis resolution. Default 100/3; finer than 1 cent is rejected. */
  centsPerBin?: number;
  /** Top of the cent axis, in Hz. Default 8000. */
  centMaxHz?: number;
  /** Stops weighting bins by tonality, which is on. Default `false`. */
  tonalityOff?: boolean;
  /** Partials summed per F0 candidate. Default 20, at most 128. */
  salienceHarmonics?: number;
  /** Lowest F0 a candidate may take, in Hz. Default 55. */
  f0MinHz?: number;
  /** Highest F0 a candidate may take, in Hz. Default 1760. */
  f0MaxHz?: number;
  /** Harmonic weighting offset, in Hz. Default 27. */
  salienceAlphaHz?: number;
  /** Harmonic weighting scale, in Hz. Default 320. */
  salienceBetaHz?: number;
  /**
   * Partial-series stretch assumed while scoring a candidate, `B` in
   * `f_h = h*f0*sqrt(1 + B*h^2)`. 0 is the default and also a stretch of zero, so
   * it needs no sentinel.
   */
  salienceInharmonicity?: number;
  /** Voices one frame may hold. Default 4, at most 64. */
  maxPolyphony?: number;
  /**
   * Stops the per-frame iteration below this share of the frame's first peak.
   * Default 0.20; **negative selects 0**.
   */
  minFramePeakRatio?: number;
  /** Closest two candidates of one frame may sit. Default 50; **negative selects 0**. */
  minSeparationCents?: number;
  /** Share of a found voice removed before the next iteration. Default 1. */
  subtractionFactor?: number;
  /** A larger move between two frames breaks the ridge. Default 50 cents. */
  maxJumpCents?: number;
  /**
   * A fade below this share of the ridge's own running peak breaks it. Default
   * 0.10; **negative selects 0**.
   */
  minRidgePeakRatio?: number;
  /** Shorter ridges are dropped. Default 140 ms; **negative selects 0**. */
  minRidgeDurationMs?: number;
  /** Partials claimed per note. Default 20, at most 128. */
  maskHarmonics?: number;
  /** Claim half-width in Hann main lobes. Default 1. */
  claimLobes?: number;
  /**
   * Stretch of the claimed partial series, `B` in `f_h = h*f0*sqrt(1 + B*h^2)`.
   * Default 0, which is also a value.
   *
   * Leaving it at 0 for stretched material costs more than a widened claim would:
   * at a piano's `1e-4` the highest partial of a twenty-harmonic claim sits
   * outside the claim entirely, and a partial outside every claim is residual —
   * carried unedited, so it keeps sounding at the old pitch after its note moves.
   */
  inharmonicity?: number;
  /**
   * Fits a stretch per note from the spectrum instead of spending
   * {@link PolyphonicAnalysisOptions.inharmonicity} on every one of them. Default
   * `false`.
   *
   * Off by default because of what it reaches rather than what it costs: at the
   * default framing the fit takes an isolated note in the middle register and
   * refuses a chord. A refused note keeps the declared stretch, so the fit only
   * ever replaces a guess with a measurement —
   * `PolyphonicAnalysis.noteInharmonicity()` reports which notes it reached.
   */
  estimateInharmonicity?: boolean;
  /** Usable partials one fit needs before its result is believed. Default 3. */
  inharmonicityMinPartials?: number;
  /** Largest per-partial misfit a fit may leave, in STFT bins. Default 0.5. */
  inharmonicityMaxResidualBins?: number;
  /** A fitted stretch above this is refused. Default 0.03125. */
  inharmonicityMaxStretch?: number;
  /** Frames per apportionment fit. Default 8, between 4 and 64. */
  windowFrames?: number;
  /** Radians per frame two claimed partials must differ by to be fitted. Default 0.01. */
  minPartialSeparation?: number;
  /** Relative misfit ceiling above which the fit refuses the bin. Default 0.02. */
  maxFitResidual?: number;
  /**
   * Ceiling on one weight's modulus. A weight is a fitted component over the
   * observed bin, so where two partials nearly cancel it exceeds one and the
   * residual carries several times the input there. While every edit is identity
   * that is inaudible — the notes and the residual still sum to the input.
   * Lowering it trades separation for a quieter residual. Default 8.
   */
  maxWeightModulus?: number;
  /** Highest partial usable to refine an F0, in Hz. Default 0, which derives one. */
  maxRefineHz?: number;
  /** Worst F0 error tolerated by the fit, in cents. Default 50. */
  f0ToleranceCents?: number;
  /** Cents of pitch change that cut one ridge into two notes. Default 50. */
  segmentationThresholdCents?: number;
  /** Shortest span kept as a note, in ms. Default 30. */
  minNoteMs?: number;
  /** Reference pitch each note's `medianCents` is measured against. Default 440. */
  referenceHz?: number;
}

/** Options for `PolyphonicAnalysis.render`. All fields are optional. */
export interface PolyphonicRenderOptions {
  /**
   * Equal-power cross-fade at each edited note's edges. Default 5 ms; a hard cut
   * is deliberately not selectable, because the seam it leaves is a click.
   */
  fadeMs?: number;
  /**
   * Boundary between the drift and the vibrato that `vibratoDepthChange` and
   * `driftChange` act on, in Hz. Default 3 Hz, and it has to be whatever a curve
   * edit was drawn at.
   */
  vibratoCutoffHz?: number;
}

/**
 * A pending, non-destructive change to one percussive event.
 * `extractPercussiveEvents` attaches the identity edit (no move, unity gain,
 * unmuted) to every event it returns; `renderPercussiveEvents` applies whatever
 * the caller has changed.
 *
 * A struck sound has no steady pitch to edit, so the axes are time and amplitude
 * and there is deliberately nothing else here.
 */
export interface PercussiveEventEdit {
  /**
   * Moves the hit along the timeline; negative moves it earlier. Where the hit
   * lands is not bounds-checked, so a moved event may be written over a
   * neighbour, and a shift past either end is truncated there.
   */
  timeOffsetSamples: number;
  /** Level change applied to the hit, which is the span's percussive component. */
  gainDb: number;
  /** Silences the hit; the other fields then do not apply. */
  muted: boolean;
}

/**
 * A {@link PercussiveEventEdit} as supplied to `renderPercussiveEvents`. Every
 * field is optional and an omitted one is the identity, so `{}` leaves the event
 * untouched.
 */
export type PercussiveEventEditInput = Partial<PercussiveEventEdit>;

/**
 * One editable percussive event returned by `extractPercussiveEvents`.
 *
 * A struck sound located in time: a span in source samples, three measured
 * figures, and a pending edit. It carries no pitch and is never associated with
 * a {@link NoteObject} — the two models are produced by separate calls and do
 * not refer to each other.
 *
 * `onsetSample` and `offsetSample` are 64-bit on the core side and arrive as JS
 * numbers, which are exact up to `Number.MAX_SAFE_INTEGER`.
 */
export interface PercussiveEvent {
  /** First sample of the span, backtracked to in front of the transient. */
  onsetSample: number;
  /** One past the last sample of the span; the next onset, or the cap. */
  offsetSample: number;
  /**
   * Detector strength at the onset, on the onset envelope's own scale. It orders
   * events against each other and carries no absolute meaning.
   */
  strength: number;
  /**
   * Peak absolute sample of the percussive component over the span, linear.
   * Measured on the signal `gainDb` scales rather than on the source.
   */
  peakAmplitude: number;
  /**
   * Share of the span's energy the separation assigned to percussion, in
   * `[0, 1]`; 0 when the span is silent.
   *
   * It describes the span rather than the onset that opened it. An isolated hit
   * sits near 1, but a real hit over a loud sustain sits near 0, because the
   * sustain owns the span's energy. So it separates a hit from a note attack
   * only where nothing is sustaining through both, and it is not a test for
   * whether a hit is there.
   */
  percussiveRatio: number;
  /** This event's pending edit; the identity as returned. */
  edit: PercussiveEventEdit;
}

/**
 * An event handed to `renderPercussiveEvents`. Only the span and the edit are
 * read — the three measured figures are ignored — so a {@link PercussiveEvent}
 * straight from `extractPercussiveEvents` can be passed back with its `edit`
 * changed and nothing else.
 */
export interface PercussiveEventInput {
  /** First sample of the span. */
  onsetSample: number;
  /** One past the last sample of the span. */
  offsetSample: number;
  /** Omit for the identity edit. */
  edit?: PercussiveEventEditInput;
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
  /** FFT size; a power of two in `[2, 262144]`. Default 2048. */
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
