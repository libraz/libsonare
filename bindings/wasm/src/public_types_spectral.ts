import type { Matrix2D } from './public_types.js';
import type { Key, KeyMode, PitchClass } from './public_types_music.js';

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

/** What `pitchCorrectTimevarying` retunes toward: a fixed MIDI note or the scale. */
export type PitchTargetMode = 'midi' | 'scale';

/** Options for `pitchCorrectTimevarying`: the F0 track is required, everything else is optional. */
export interface PitchCorrectOptions {
  /** Per-frame measured F0 in Hz, one entry per analysis frame. */
  f0Hz: Float32Array;
  /**
   * F0 frame-center spacing in samples: frame `i` is centered at sample
   * `i * hopLength`, and nearest-frame voicing switches halfway between
   * centers. Default 512.
   */
  hopLength?: number;
  /** `'midi'` retunes toward `targetMidi`; `'scale'` snaps to the key. Default `'midi'`. */
  targetMode?: PitchTargetMode;
  /** Fixed target note when `targetMode` is `'midi'`, in `[0, 127]`. Default 69 (A4). */
  targetMidi?: number;
  /** Scale root pitch class (0=C .. 11=B) when `targetMode` is `'scale'`. Default 0. */
  scaleRoot?: number;
  /**
   * Scale degrees enabled: a 12-bit mask (bit `i` = semitone `i` above the root)
   * or a mode name, as `scaleMaskForMode` resolves it. Default C major.
   */
  scaleModeMask?: number | KeyMode;
  /** Reference MIDI anchoring the scale grid. Default 69 (A4). */
  scaleReferenceMidi?: number;
  /** Correction strength in `[0, 1]`; 1 = full snap, 0 = bypass. Default 1. */
  retuneAmount?: number;
  /** Hard clamp on per-frame correction magnitude (semitones). Default 12. */
  maxCorrectionSemitones?: number;
  /** Retune IIR time constant (ms); larger = slower glide. Default 50. */
  retuneSpeedMs?: number;
  /** Corrections below this are bypassed to preserve vibrato (cents). Default 20. */
  vibratoThresholdCents?: number;
  /** Per-frame voiced flags (truthy = voiced). Give at most one of `voiced` and `voicedProb`: both together is a TypeError, and with neither all frames are treated as voiced. */
  voiced?: VoicedFlags;
  /** Per-frame voicing probability in `[0, 1]`, given in place of `voiced`. */
  voicedProb?: Float32Array;
}

/** A key as `detectKey` reports it, which is what `autoTune` takes to name one. */
export interface AutoTuneKey {
  /** Pitch class of the key root, an integer 0-11 (C = 0). */
  root: PitchClass;
  /** The mode name `detectKey` reports. */
  mode: KeyMode;
}

/** Tuning knobs of `autoTune`. All fields are optional. */
export interface AutoTuneOptions {
  /**
   * `'detect'` (default) finds the key with `detectKey`'s defaults; a
   * `{ root, mode }` names it, as `detectKey`'s result does: `root` an integer
   * pitch class `0..11` (`RangeError` otherwise, `TypeError` for a non-number),
   * `mode` a {@link KeyMode} token (`RangeError` for an unknown one, `TypeError`
   * for a non-string).
   */
  key?: 'detect' | AutoTuneKey;
  /** Correction strength in `[0, 1]`; 1 = full snap to the scale, 0 = bypass. Default 1. */
  retuneAmount?: number;
  /** Retune IIR time constant (ms); larger = slower glide. Default 50. */
  retuneSpeedMs?: number;
  /** Corrections below this are bypassed to preserve vibrato (cents). Default 20. */
  vibratoThresholdCents?: number;
  /** Hard clamp on per-frame correction magnitude (semitones). Default 12. */
  maxCorrectionSemitones?: number;
  /** Reference MIDI anchoring the scale grid. Default 69 (A4). */
  scaleReferenceMidi?: number;
}

/** Result of `autoTune`. */
export interface AutoTuneResult {
  /** The corrected audio, the length of the input. */
  samples: Float32Array;
  /**
   * The key the audio was tuned to: the detected one (with the model's own
   * `confidence`, see `Key`) or the one the request named (`confidence` 1).
   */
  key: Key;
}

/** Options for `noteStretch`. `stretchRatio` is required. */
export interface NoteStretchOptions {
  /** Note onset position in samples (selects the region). Default 0. Not with `onsetSec`. */
  onsetSample?: number;
  /** Note onset in seconds, rounded to the nearest sample. Not with `onsetSample`. */
  onsetSec?: number;
  /** Note offset position in samples (selects the region). Defaults to the input length. Not with `offsetSec`. */
  offsetSample?: number;
  /** Note offset in seconds, rounded to the nearest sample. Not with `offsetSample`. */
  offsetSec?: number;
  /**
   * Duration ratio of the note (0.5 = half duration, 2.0 = double duration).
   * This is the opposite sense of `timeStretch`'s `speedRatio`, a speed
   * (`> 1` shortens).
   */
  stretchRatio: number;
}

/** Options for `noteMove`. One of `targetOnsetSample` / `targetOnsetSec` is required. */
export interface NoteMoveOptions {
  /** First sample of the note to move. Default 0. Not with `onsetSec`. */
  onsetSample?: number;
  /** First second of the note to move, rounded to the nearest sample. Not with `onsetSample`. */
  onsetSec?: number;
  /** Defaults to the input length. Not with `offsetSec`. */
  offsetSample?: number;
  /** Last second of the note to move, rounded to the nearest sample. Not with `offsetSample`. */
  offsetSec?: number;
  /** Sample the note's onset moves to. Default 0. Not with `targetOnsetSec`. */
  targetOnsetSample?: number;
  /** Second the note's onset moves to, rounded to the nearest sample. Not with `targetOnsetSample`. */
  targetOnsetSec?: number;
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
   * `[0, 1]`. Applies to `voicedProb` only. Default 0.5.
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
 *
 * Readonly: edit a note by passing a modified copy to `renderNotes`.
 *
 * @example
 * ```ts
 * declare const notes: NoteObject[]; // what extractNotes returned
 * renderNotes({ samples, sampleRate, notes: notes.map((n) => ({ ...n, edit: { ...n.edit, gainDb: -6 } })) });
 * ```
 */
export interface NoteEdit {
  /**
   * Moves the note along the timeline; negative moves it earlier. Where the note
   * lands is not bounds-checked, so a moved note may overwrite a neighbour.
   */
  readonly timeOffsetSamples: number;
  /** Transpose applied to the note's span. */
  readonly pitchSemitones: number;
  /** Level change applied to the note's span. */
  readonly gainDb: number;
  /**
   * Duration ratio: `>1` lengthens the note, `<1` shortens it; pitch is
   * preserved. 0 reads as 1. The opposite sense of `timeStretch`'s `speedRatio`, a speed.
   */
  readonly timeStretchRatio: number;
  /**
   * Moves the spectral envelope, in semitones, on top of whatever the pitch
   * shift already did to it.
   *
   * 0 runs no warp at all, so a pitch-only edit is not charged for an LPC
   * analysis-resynthesis round it did not ask for. A pitch shift drags the
   * formants with it, so holding them still is `-pitchSemitones` and the
   * chipmunk is the default. Saturates near -10.3 and +8.7 semitones rather
   * than being rejected.
   */
  readonly formantSemitones: number;
  /**
   * Scales the vibrato measured over the note, stated as a change from it: 0
   * keeps it, -1 flattens it, +1 doubles it.
   *
   * Applying it needs the note's own pitch curve, so `renderNotes` must be given
   * the `f0Hz` track and the note its `frameStart` / `frameEnd` / `medianHz`; a
   * note carrying no usable pitch is rejected rather than left alone. The curve
   * is split at the request's `vibratoCutoffHz`.
   */
  readonly vibratoDepthChange: number;
  /** The same, for the slow drift around the note's centre pitch. */
  readonly driftChange: number;
  /** Silences the note's span; the other fields then do not apply. */
  readonly muted: boolean;
  /**
   * Per-frame linear gain over the note's span, on top of `gainDb`.
   *
   * A set of gain points rather than a signal: it is stretched over whatever
   * length the note renders at, so it survives a time stretch and need not match
   * the note's frame count. One entry is a constant gain, and an empty array is
   * no envelope. Every value must be finite and non-negative.
   */
  readonly amplitudeEnvelope: Float32Array;
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
  /** Per-frame linear gain points over the note's span, as a `Float32Array` or a plain number array; omitted is no envelope. */
  amplitudeEnvelope?: Float32Array | readonly number[];
  /**
   * `NoteEdit.timeOffsetSamples` in seconds, rounded to the nearest sample at the
   * call's sample rate; negative moves the note earlier. Not with
   * `timeOffsetSamples`.
   */
  timeOffsetSec?: number;
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
  readonly onsetSample: number;
  /** One past the last sample of the span. */
  readonly offsetSample: number;
  /** First frame of the span, in the framing that found the note. */
  readonly frameStart: number;
  /** One past the last frame of the span. */
  readonly frameEnd: number;
  /** Median measured pitch over the span, in Hz. */
  readonly medianHz: number;
  /** Median pitch in cents above the request's `referenceHz`. */
  readonly medianCents: number;
  /** Pitch steadiness in `[0, 1]`; 1 is perfectly steady. */
  readonly f0Stability: number;
  /** One RMS value per frame of the span (`frameEnd - frameStart` entries). */
  readonly amplitude: Float32Array;
  /** This note's pending edit; the identity as returned. */
  readonly edit: NoteEdit;
}

/**
 * A note handed to `renderNotes`. Only the span, the edit and — for a
 * `vibratoDepthChange` or `driftChange` edit — the frame bounds and the centre
 * are read, so a {@link NoteObject} straight from `extractNotes` can be passed
 * back with its `edit` changed and nothing else.
 */
export interface NoteObjectInput {
  /** First sample of the note's span. Give this or `onsetSec`. */
  onsetSample?: number;
  /** First second of the span, rounded to the nearest sample. Not with `onsetSample`. */
  onsetSec?: number;
  /** One past the last sample of the span. Give this or `offsetSec`. */
  offsetSample?: number;
  /** End of the span in seconds, rounded to the nearest sample. Not with `offsetSample`. */
  offsetSec?: number;
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
 * One note of a reference melody, as `noteTargetsFromSmf` returns it and
 * `assignNoteTargets` reads it.
 *
 * Times are seconds from the start of the audio the notes being corrected were
 * extracted from — a reference is lined up against a take by its own clock, not
 * by a frame index into either one. Every field must be finite.
 */
export interface NoteTarget {
  /** First second of the target's span. */
  startSec: number;
  /** One past the last second of the span. */
  endSec: number;
  /** The pitch that stretch of the part is supposed to be, as a MIDI number. */
  targetMidi: number;
}

/**
 * What `assignNoteTargets` does with a note that has a measurable pitch and no
 * target.
 *
 * A note carrying no pitch at all is a different case and no policy reaches it:
 * there is nothing to correct from, so it is never assigned and never edited.
 */
export type NoteTargetUnmatchedPolicy =
  /** Leave the edit alone; the note renders as recorded. Default. */
  | 'leave'
  /** Mute the note's span. */
  | 'mute'
  /** Take the nearest target in time, however far away it is. */
  | 'nearest';

/** What `assignNoteTargets` returns. */
export interface AssignNoteTargetsResult {
  /**
   * The note set with each assigned note's `edit.pitchSemitones` — and,
   * under `'mute'`, its `edit.muted` — rewritten. A new array: the notes handed
   * in are not touched, and every other field of a note comes back as it went.
   */
  notes: NoteObject[];
  /**
   * How many notes received a target. Zero is a legitimate answer — a reference
   * that does not line up with the take — which is why it is reported rather
   * than left for the caller to infer from the edits.
   */
  assignedCount: number;
}

/**
 * One note's pitch curve split into a centre, a slow drift and a vibrato by
 * `decomposeNotePitch`.
 *
 * `driftCents[i] + vibratoCents[i]` is the note's own pitch at frame `i`, in
 * cents above `centerHz`, to within float rounding, so the three parts
 * reconstruct the curve. The drift filter is zero phase, so neither curve is
 * shifted in time against the audio.
 */
export interface PitchDecompositionResult {
  /**
   * The note's steady pitch in Hz. 0 when the note carries no usable pitch, and
   * then both curves are empty.
   */
  centerHz: number;
  /** Slow deviation from `centerHz` in cents, one entry per frame. */
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
  /**
   * STFT size the whole chain runs in. Default 4096, the size it is tuned at.
   * `nFft`, `hopLength` and `winLength` are samples at 44100 Hz, converted to the
   * input's sample rate; explicit values are converted too.
   */
  nFft?: number;
  /** STFT hop in samples at 44100 Hz. Default 512. */
  hopLength?: number;
  /** Window length in samples at 44100 Hz. Absent means `nFft`; an explicit 0 is a `RangeError`. */
  winLength?: number;
  /** Bottom of the cent axis the salience is folded onto, in Hz. Default 55. */
  centRefHz?: number;
  /** Cent-axis resolution. Default 100/3; finer than 1 cent is rejected. */
  centsPerBin?: number;
  /** Top of the cent axis, in Hz. Default 8000. */
  centMaxHz?: number;
  /** Weights bins by tonality. Default `true`. */
  tonality?: boolean;
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
   * Absent takes the library default (0.20); an explicit 0 means 0; a negative is
   * refused with a `RangeError`.
   */
  minFramePeakRatio?: number;
  /**
   * Closest two candidates of one frame may sit, in cents. Absent takes the library
   * default (50); an explicit 0 means 0; a negative is refused with a `RangeError`.
   */
  minSeparationCents?: number;
  /** Share of a found voice removed before the next iteration. Default 1. */
  subtractionFactor?: number;
  /** A larger move between two frames breaks the ridge. Default 50 cents. */
  maxJumpCents?: number;
  /**
   * A fade below this share of the ridge's own running peak breaks it. Absent
   * takes the library default (0.10); an explicit 0 means 0; a negative is
   * refused with a `RangeError`.
   */
  minRidgePeakRatio?: number;
  /**
   * Shorter ridges are dropped, in ms. Absent takes the library default (140); an
   * explicit 0 means 0; a negative is refused with a `RangeError`.
   */
  minRidgeDurationMs?: number;
  /**
   * Splits a ridge where its salience climbs past this multiple of the level
   * just before, the same pitch struck again while it sounds. Must be above `1`.
   * Absent leaves re-attack splitting off; an explicit 0 and a value in `(0, 1]`
   * or a negative are refused with a `RangeError`.
   */
  reattackRatio?: number;
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
  /** Highest partial usable to refine an F0, in Hz. Absent derives one; an explicit 0 is a `RangeError`. */
  maxRefineHz?: number;
  /** Worst F0 error tolerated by the fit, in cents. Default 50. */
  f0ToleranceCents?: number;
  /**
   * Scales each note's reported `f0Stability` only. Every tracked ridge is one
   * note, so a mid-ridge pitch jump is never split into two. Default 50.
   */
  segmentationThresholdCents?: number;
  /**
   * Not read: every tracked ridge is one note, so nothing filters a short note
   * here. Use `minRidgeDurationMs` to drop short ridges instead. Default 30.
   */
  minNoteMs?: number;
  /** Reference pitch each note's `medianCents` is measured against. Default 440. */
  referenceHz?: number;
}

/** Options for `PolyphonicAnalysis.render`. All fields are optional. */
export interface PolyphonicRenderOptions {
  /**
   * Cross-fade at each edited note's edges, weighted to hold the level whether
   * the note is coherent with the audio under it or not. Default 5 ms; a hard cut
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
export type PercussiveEventEditInput = Partial<PercussiveEventEdit> & {
  /**
   * `PercussiveEventEdit.timeOffsetSamples` in seconds, rounded to the nearest
   * sample at the call's sample rate; negative moves the hit earlier. Not with
   * `timeOffsetSamples`.
   */
  timeOffsetSec?: number;
};

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
  /** First sample of the span. Give this or `onsetSec`. */
  onsetSample?: number;
  /** First second of the span, rounded to the nearest sample. Not with `onsetSample`. */
  onsetSec?: number;
  /** One past the last sample of the span. Give this or `offsetSec`. */
  offsetSample?: number;
  /** End of the span in seconds, rounded to the nearest sample. Not with `offsetSample`. */
  offsetSec?: number;
  /** Omit for the identity edit. */
  edit?: PercussiveEventEditInput;
}

/** Mel band weighting: `'slaney'` scales each band to the same area, `'none'` leaves each at unit peak. */
export type MelNorm = 'none' | 'slaney';

/** How a `spectralEdit` region op modifies the masked bins. */
export type SpectralEditMode = 'gain' | 'attenuate' | 'mute' | 'heal';

/** Analysis/synthesis window, shared by `spectralEdit` and the streaming analyzer. */
export type WindowType = 'hann' | 'hamming' | 'blackman' | 'rectangular';

/** One time x frequency rectangle edit op for `spectralEdit`. */
export interface SpectralRegionOp {
  /** Region time start in input samples; past the end reads as the end, a negative value is refused. Default 0. Not with `startSec`. */
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
  /** Region frequency low edge in Hz; clamped to [0, nyquist]. Default 0. */
  lowHz?: number;
  /** Region frequency high edge in Hz, finite and greater than 0; at or above Nyquist reads as Nyquist. Omit for Nyquist. */
  highHz?: number;
  /** Linear gain in dB for 'gain'/'attenuate'; ignored by 'mute'/'heal'. Default 0. */
  gainDb?: number;
  /** Edit mode. Default 'gain'. */
  mode?: SpectralEditMode;
}

/** STFT + heal parameters for `spectralEdit`. `ops` is required; the rest is optional. */
export interface SpectralEditOptions {
  /** Region edit ops applied in array order; an empty array returns a copy of the input. */
  ops: readonly SpectralRegionOp[];
  /** FFT size; a power of two in `[4, 262144]`. Omit for 2048; 0 is refused. */
  nFft?: number;
  /** Hop length; `0 < hopLength <= nFft / 2`. Omit for a quarter of `nFft`; 0 is refused. */
  hopLength?: number;
  /** Analysis + synthesis window. Default 'hann'. */
  window?: WindowType;
  /** Neighbour frames each side used by `'heal'`, at least 1. Omit for 2; 0 is refused. */
  healRadiusFrames?: number;
}

/**
 * Constant-Q / Variable-Q transform magnitude result (mirrors the C
 * `SonareCqtResult`).
 */
export interface CqtResult {
  /** Hop length in samples. */
  hopLength: number;
  /** Sample rate in Hz. */
  sampleRate: number;
  /** Rows are CQT bins, cols are frames. */
  magnitude: Matrix2D;
  /** Center frequency (Hz) of each of the `nBins` bins. */
  frequencies: Float32Array;
  /**
   * Bandwidth offset in Hz the transform ran with: for `vqt`, the resolved value (the
   * ERB-derived one when `gamma` was omitted); 0 for the three constant-Q transforms.
   */
  gamma: number;
}

/**
 * STFT (Short-Time Fourier Transform) result
 */
export interface StftResult {
  /** FFT size in samples. */
  nFft: number;
  /** Hop length in samples of the analysis. */
  hopLength: number;
  /** Sample rate in Hz of the analysed audio. */
  sampleRate: number;
  /** Rows are `nFft / 2 + 1` bins, cols are frames. */
  magnitude: Matrix2D;
  /** Rows are `nFft / 2 + 1` bins, cols are frames. */
  power: Matrix2D;
  /** Magnitude in decibels, rows are bins and cols are frames; present only when `db` was requested. */
  db?: Matrix2D;
}

/**
 * Mel spectrogram, with the parameters of the forward transform, so the result
 * alone says how to invert it: pass `power` to `melToStft` or `melToAudio` with
 * the same `sampleRate`, `nFft`, `fminHz`, `fmaxHz` and `htk` (and, for
 * `melToAudio`, `hopLength`).
 */
export interface MelSpectrogramResult {
  /** Sample rate in Hz of the analysed audio. */
  sampleRate: number;
  /** Hop length in samples of the analysis. */
  hopLength: number;
  /** FFT size of the source STFT. */
  nFft: number;
  /** Lower Mel band edge in Hz as applied. */
  fminHz: number;
  /** Upper Mel band edge in Hz as applied: the Nyquist when the request left it at 0. */
  fmaxHz: number;
  /** `true` for the HTK Mel formula, `false` for Slaney. */
  htk: boolean;
  /** Linear power; rows are mel bands, cols are frames. */
  power: Matrix2D;
  /** The dB form of `power`; rows are mel bands, cols are frames. */
  db: Matrix2D;
}

/**
 * MFCCs, with the Mel parameters they were derived with and the lifter, so the
 * result alone says how to invert it: pass `coefficients` to `mfccToMel` or
 * `mfccToAudio` with the same `nMels`, `lifter` and Mel parameters (and, for
 * `mfccToAudio`, `hopLength`).
 */
export interface MfccResult {
  /** Sample rate in Hz of the analysed audio. */
  sampleRate: number;
  /** Hop length in samples of the analysis. */
  hopLength: number;
  /** FFT size in samples of the source STFT. */
  nFft: number;
  /** Mel bands the coefficients were taken from. */
  nMels: number;
  /** Lower Mel band edge in Hz as applied. */
  fminHz: number;
  /** Upper Mel band edge in Hz as applied: the Nyquist when the request left it at 0. */
  fmaxHz: number;
  /** `true` for the HTK Mel formula, `false` for Slaney. */
  htk: boolean;
  /** Cepstral lifter applied to the coefficients; 0 means none. */
  lifter: number;
  /** Rows are coefficients, cols are frames. */
  coefficients: Matrix2D;
}

/**
 * Chroma features result
 */
export interface ChromaResult {
  /** Sample rate in Hz of the analysed audio. */
  sampleRate: number;
  /** Hop length in samples of the analysis. */
  hopLength: number;
  /** Rows are chroma bins, cols are frames. */
  features: Matrix2D;
  /** Mean energy of each chroma bin over all frames. */
  meanEnergy: number[];
}

/**
 * Pitch detection result
 */
export interface PitchResult {
  /** Per-frame fundamental frequency in Hz; unvoiced frames are NaN. */
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
  /** Median F0 in Hz over the voiced frames. */
  medianF0: number;
  /** Mean F0 in Hz over the voiced frames. */
  meanF0: number;
}

/** Per-bin pitch candidates and peak magnitudes from spectral piptrack. */
export interface PiptrackResult {
  /** Rows are FFT bins, cols are frames. */
  pitches: Matrix2D;
  /** Rows are FFT bins, cols are frames. */
  magnitudes: Matrix2D;
}

/** Magnitude and reassigned time/frequency coordinates for every STFT bin. */
export interface ReassignedSpectrogramResult {
  /** Rows are FFT bins, cols are frames. */
  magnitude: Matrix2D;
  /** Rows are FFT bins, cols are frames. */
  times: Matrix2D;
  /** Rows are FFT bins, cols are frames. */
  frequencies: Matrix2D;
}

/** One stable monophonic note region segmented from an F0 track. */
export interface NoteSegment {
  /** First F0 frame of the note. */
  frameStart: number;
  /** End of the note in F0 frames (exclusive). */
  frameEnd: number;
  /** Note start in seconds. */
  startSec: number;
  /** Note end in seconds. */
  endSec: number;
  /** Median pitch of the note in cents. */
  medianCents: number;
}

/**
 * Loudness measurement result (EBU R128 / ITU-R BS.1770)
 */
export interface LufsResult {
  /**
   * Integrated loudness over the whole signal, in LUFS. `-Infinity` when nothing passes the gate.
   * Never absent: the integrated measurement clamps to the input.
   */
  integratedLufs: number;
  /** Final complete 400 ms window, not Max-M. `-Infinity` for zero energy; absent when the signal is shorter than one window. */
  momentaryLufs?: number;
  /** Final complete 3 s window, not Max-S. `-Infinity` for zero energy; absent when the signal is shorter than one window (a 1 s signal has no short-term reading). */
  shortTermLufs?: number;
  /** Maximum 400 ms window (EBU R128 Max-M). `-Infinity` for zero energy; absent when the signal is shorter than one window. */
  maxMomentaryLufs?: number;
  /** Maximum 3 s window (EBU R128 Max-S). `-Infinity` for zero energy; absent when the signal is shorter than one window. */
  maxShortTermLufs?: number;
  /**
   * Loudness range in LU. Absent for silence and when no 3 s block passes the gates:
   * a spread, not a level, so never `-Infinity`.
   */
  loudnessRangeLu?: number;
}

/**
 * The two per-block loudness series a multi-channel measurement builds, in
 * LUFS. A signal shorter than a window yields an empty series for it.
 */
export interface LufsSeriesResult {
  /** 400 ms momentary series, one value per 100 ms hop; empty when the input is shorter than 400 ms. */
  momentaryLufs: Float32Array;
  /** 3 s short-term series, one value per 100 ms hop; empty when the input is shorter than 3 s. */
  shortTermLufs: Float32Array;
}

/**
 * HPSS (Harmonic-Percussive Source Separation) result
 */
export interface HpssResult {
  /** Harmonic component, the same length as the input. */
  harmonic: Float32Array;
  /** Percussive component, the same length as the input. */
  percussive: Float32Array;
  /**
   * What neither component claimed (input - harmonic - percussive), the same
   * length as the input. Present only when the call set `withResidual: true`.
   */
  residual?: Float32Array;
  /** Sample rate in Hz of both components. */
  sampleRate: number;
}
