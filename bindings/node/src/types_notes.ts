/**
 * Note- and event-level types: the editable objects a polyphonic or
 * percussive decomposition produces, and the options that drive it.
 */

import type { Key, KeyMode, PitchClass } from './types_analysis.js';

/**
 * Per-frame voicing decision, one entry per `f0Hz` frame. A truthy or non-zero
 * entry marks the frame voiced. The union covers what the analysis side hands
 * back — {@link PitchResult.voicedFlag} is a `boolean[]` — as well as the typed
 * and plain numeric arrays a caller may build directly, so a pitch track can be
 * fed straight into pitch correction without a conversion step.
 */
export type VoicedFlags =
  | Int32Array
  | Uint8Array
  | Float32Array
  | readonly number[]
  | readonly boolean[];

/** What {@link pitchCorrectTimevarying} retunes toward: a fixed MIDI note or the scale. */
export type PitchTargetMode = 'midi' | 'scale';

/** Options for {@link pitchCorrectTimevarying}: the F0 track is required, everything else is optional. */
export interface PitchCorrectOptions {
  /** Per-frame measured F0 in Hz, one entry per analysis frame. */
  f0Hz: Float32Array;
  /**
   * F0 frame-center spacing in samples: frame `i` is centered at sample
   * `i * hopLength`, and nearest-frame voicing switches halfway between
   * centers. Default 512.
   */
  hopLength?: number;
  /** `'midi'` retunes toward {@link targetMidi}; `'scale'` snaps to the key. Default `'midi'`. */
  targetMode?: PitchTargetMode;
  /** Fixed target note when `targetMode` is `'midi'`, in `[0, 127]`. Default 69 (A4). */
  targetMidi?: number;
  /** Scale root pitch class (0=C .. 11=B) when `targetMode` is `'scale'`. Default 0. */
  scaleRoot?: number;
  /**
   * Scale degrees enabled: a 12-bit mask (bit `i` = semitone `i` above the root)
   * or a mode name, as {@link scaleMaskForMode} resolves it. Default C major.
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
  /**
   * Per-frame voiced flags (truthy = voiced). Give at most one of `voiced` and
   * `voicedProb`: both together is a TypeError, and with both omitted every
   * frame is treated as voiced.
   */
  voiced?: VoicedFlags;
  /**
   * Per-frame voicing probability in `[0, 1]`, given in place of `voiced`.
   */
  voicedProb?: Float32Array;
}

/** A key as {@link detectKey} reports it, which is what {@link autoTune} takes to name one. */
export interface AutoTuneKey {
  /** Pitch class of the tonic (C = 0). */
  root: PitchClass;
  /** Mode of the key. */
  mode: KeyMode;
}

/** Tuning knobs of {@link autoTune}. All fields are optional. */
export interface AutoTuneOptions {
  /**
   * `'detect'` (default) finds the key with {@link detectKey}'s defaults; a
   * `{ root, mode }` names it, as {@link detectKey}'s result does: `root` an
   * integer pitch class `0..11` (`RangeError` otherwise, `TypeError` for a
   * non-number), `mode` a {@link KeyMode} token (`RangeError` for an unknown one,
   * `TypeError` for a non-string).
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

/** Result of {@link autoTune}. */
export interface AutoTuneResult {
  /** The corrected audio, the length of the input. */
  samples: Float32Array;
  /**
   * The key the audio was tuned to: the detected one (with the model's own
   * `confidence`, see {@link Key}) or the one the request named (`confidence` 1).
   */
  key: Key;
}

/** Options for {@link noteStretch}. `stretchRatio` is required. */
export interface NoteStretchOptions {
  /** First sample of the note to stretch. Default 0. Not with `onsetSec`. */
  onsetSample?: number;
  /** First second of the note to stretch, rounded to the nearest sample. Not with `onsetSample`. */
  onsetSec?: number;
  /** Last sample of the note to stretch. Defaults to the input length. Not with `offsetSec`. */
  offsetSample?: number;
  /** Last second of the note to stretch, rounded to the nearest sample. Not with `offsetSample`. */
  offsetSec?: number;
  /**
   * Duration ratio of the note: `> 1` lengthens it, `< 1` shortens it, 1 is
   * unchanged (a finite number). This is the opposite sense of
   * {@link timeStretch}'s `speedRatio`, which is a speed (`> 1` shortens).
   */
  stretchRatio: number;
}

/** Segmentation tuning for {@link extractNotes}. All fields are optional. */
export interface NoteExtractorOptions {
  /** Cents of pitch change that start a new note. Default 50. */
  segmentationThresholdCents?: number;
  /** Shortest span kept as a note. Default 30 ms. */
  minNoteMs?: number;
  /** Reference pitch the `medianCents` of each note is measured against. Default 440. */
  referenceHz?: number;
  /**
   * Value of `voicedProb` at or above which a frame counts as voiced. Applies
   * to `voicedProb` only. Default 0.5.
   *
   * pYIN's `voicedProb` is a frame's voiced observation mass and rises with F0
   * for a fixed frame length, so this default silently drops low registers —
   * pass `pitchPyin`'s `voicedFlag` through `voiced` instead.
   */
  voicedThreshold?: number;
}

/**
 * A pending, non-destructive change to one note. {@link extractNotes} attaches
 * the identity edit (no move, no transpose, unity gain and stretch, unmuted) to
 * every note it returns; {@link renderNotes} applies whatever the caller has
 * changed.
 *
 * Readonly: edit a note by passing a modified copy to {@link renderNotes}.
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
  /**
   * Per-frame linear gain over the note's span, on top of `gainDb`. Empty for no
   * envelope.
   *
   * A set of gain points rather than a signal: it is stretched over whatever
   * length the note renders at, so it survives a time stretch and need not match
   * the note's frame count. One entry is a constant gain. Every value must be
   * finite and non-negative.
   *
   * The gain this edit applies, as against `NoteObject.amplitude`, which is the
   * level the note was measured at. Both are `Float32Array`, which is what the
   * core holds them as.
   */
  readonly amplitudeEnvelope: Float32Array;
  /** Transpose applied to the note's span. */
  readonly pitchSemitones: number;
  /** Level change applied to the note's span. */
  readonly gainDb: number;
  /**
   * Duration ratio: `>1` lengthens the note, `<1` shortens it; pitch is
   * preserved. The opposite sense of {@link timeStretch}'s `rate`, a speed.
   */
  readonly timeStretchRatio: number;
  /**
   * Moves the spectral envelope, in semitones, on top of whatever the pitch
   * shift already did to it.
   *
   * 0 runs no warp at all, so a pitch-only edit is not charged for an LPC
   * analysis-resynthesis round it did not ask for. A pitch shift drags the
   * formants with it, so holding them still is `-pitchSemitones` and the
   * chipmunk is the default. Saturates near -10.3 and +8.7 semitones rather than
   * being rejected.
   */
  readonly formantSemitones: number;
  /**
   * Scales the vibrato measured over the note, stated as a change from it: 0
   * keeps it, -1 flattens it, +1 doubles it.
   *
   * Applying it needs the note's pitch curve, so {@link renderNotes} requires
   * `f0Hz` and the note's `frameStart` / `frameEnd` / `medianHz`; a note carrying
   * none is rejected rather than left alone. The curve is split at the request's
   * `vibratoCutoffHz`.
   */
  readonly vibratoDepthChange: number;
  /** The same, for the slow drift around the note's centre pitch. */
  readonly driftChange: number;
  /** Silences the note's span; the other fields then do not apply. */
  readonly muted: boolean;
}

/**
 * A {@link NoteEdit} as supplied to {@link renderNotes}, {@link splitNote} and
 * {@link mergeNotes}. Every field is optional and an omitted one is the
 * identity, so `{}` leaves the note untouched.
 *
 * `amplitudeEnvelope` additionally accepts a plain number array, so a curve
 * written out by hand needs no conversion; it comes back as a `Float32Array`.
 */
export type NoteEditInput = Partial<Omit<NoteEdit, 'amplitudeEnvelope'>> & {
  /** Per-sample gain curve; a plain number array or a `Float32Array`. */
  amplitudeEnvelope?: readonly number[] | Float32Array;
  /**
   * {@link NoteEdit.timeOffsetSamples} in seconds, rounded to the nearest sample
   * at the call's sample rate; negative moves the note earlier. Not with
   * `timeOffsetSamples`.
   */
  timeOffsetSec?: number;
};

/**
 * One editable note returned by {@link extractNotes}.
 *
 * Sample bounds are half-open into the source audio; frame bounds are half-open
 * into the caller's own `f0Hz` track. The per-note F0 curve is deliberately not
 * repeated here — it is `f0Hz.subarray(frameStart, frameEnd)`.
 *
 * `onsetSample` and `offsetSample` are 64-bit on the C side and arrive as JS
 * numbers, which are exact up to `Number.MAX_SAFE_INTEGER`.
 */
export interface NoteObject {
  /** First sample of the note's span. */
  readonly onsetSample: number;
  /** One past the last sample of the span. */
  readonly offsetSample: number;
  /** First frame of the span in the caller's `f0Hz` track. */
  readonly frameStart: number;
  /** One past the last frame of the span. */
  readonly frameEnd: number;
  /** Median measured pitch over the span, in Hz. */
  readonly medianHz: number;
  /** Median pitch in cents above the request's `referenceHz`. */
  readonly medianCents: number;
  /**
   * Pitch steadiness in `[0, 1]`; 1 is perfectly steady.
   *
   * The only quality figure a note carries. A voiced fraction would be one too,
   * but the segmenter emits maximal voiced runs, so it is 1 for every note it
   * can produce and measures nothing.
   */
  readonly f0Stability: number;
  /** One RMS value per frame of the span (`frameEnd - frameStart` entries). */
  readonly amplitude: Float32Array;
  /** This note's pending edit; the identity as returned. */
  readonly edit: NoteEdit;
}

/**
 * A note handed to {@link renderNotes}. The span, the edit, and — for a curve
 * edit — the frame bounds and median pitch are read, so a {@link NoteObject}
 * straight from {@link extractNotes} can be passed back with its `edit` changed
 * and nothing else.
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
   * First frame of the span in the request's `f0Hz`. Read only by a
   * `vibratoDepthChange` / `driftChange` edit, which slices the track with it.
   */
  frameStart?: number;
  /** One past the last frame of the span; read under the same condition. */
  frameEnd?: number;
  /** The note's centre pitch, which a curve edit measures its cents against. */
  medianHz?: number;
  /** Omit for the identity edit. */
  edit?: NoteEditInput;
}

/**
 * A note handed to {@link splitNote} or {@link mergeNotes}. Both re-derive every
 * note in the set from the audio and the track, so a note is identified by its
 * frame bounds alone and only its edit travels through unchanged.
 */
export interface NoteSetEntry {
  /** First frame of the span in the request's `f0Hz`. */
  frameStart: number;
  /** One past the last frame of the span; must be greater than `frameStart`. */
  frameEnd: number;
  /** Omit for the identity edit. */
  edit?: NoteEditInput;
}

/**
 * One note of a reference melody, as {@link noteTargetsFromSmf} returns it and
 * {@link assignNoteTargets} reads it.
 *
 * Times are seconds from the start of the audio the notes were extracted from,
 * so a reference that starts elsewhere has to be offset by its caller.
 */
export interface NoteTarget {
  /** First second of the target's span. */
  startSec: number;
  /** One past the last second of the span. */
  endSec: number;
  /** The pitch the overlapping note is corrected to, as a MIDI number. */
  targetMidi: number;
}

/**
 * What {@link assignNoteTargets} does with a note that has a measurable pitch
 * and no target.
 *
 * `'leave'` keeps the note's own edit, so it renders as recorded; `'mute'`
 * silences its span; `'nearest'` takes the nearest target in time, however far
 * away. A note with no measured pitch is never edited whatever this says — that
 * is a different thing from having a pitch and no target.
 */
export type NoteTargetUnmatchedPolicy = 'leave' | 'mute' | 'nearest';

/** The notes {@link assignNoteTargets} produced, and how many got a target. */
export interface AssignNoteTargetsResult {
  /**
   * A new array, one note per input note in the same order, carrying the
   * assigned `edit.pitchSemitones` and `edit.muted`. Every other field is
   * the caller's own, unchanged.
   */
  notes: NoteObject[];
  /**
   * How many notes got a target. Zero is a legitimate answer — a reference that
   * does not line up with the take — so it is reported rather than left implicit
   * in the notes.
   */
  assignedCount: number;
}

/**
 * Tuning for {@link analyzePolyphonic}. Every field is optional and an omitted
 * one takes the default stated below.
 *
 * Four fields accept 0 as a value as well as reading it as their default, and are
 * marked: **pass a negative number to select 0 on those**. Each of them rejects a
 * negative otherwise, so the meaning cannot collide with a setting a caller meant
 * literally.
 *
 * The window function and the centred framing are deliberately not settable:
 * every span, claim and mask offset in this chain is derived against one framing,
 * so a second way to state it would be a second thing to keep in agreement.
 */
export interface PolyphonicAnalysisOptions {
  /**
   * FFT size, hop and window length of the one STFT that serves the extraction,
   * the claims, the apportionment and the render. Default 4096, 512 and `nFft`.
   * All three are samples at 44100 Hz, converted to the input's sample rate;
   * explicit values are converted too.
   */
  nFft?: number;
  /** STFT hop in samples at 44100 Hz, converted to the input rate. Default `512`. */
  hopLength?: number;
  /**
   * STFT window length in samples at 44100 Hz, converted to the input rate. Absent
   * means `nFft`; an explicit 0 is a `RangeError`.
   */
  winLength?: number;

  /** Bottom of the cent axis the salience is folded onto, in Hz. Default 55. */
  centRefHz?: number;
  /**
   * Resolution of that axis, in cents per bin. Default 100/3; finer than 1 cent
   * is rejected.
   */
  centsPerBin?: number;
  /** Top of that axis, in Hz. Default 8000. */
  centMaxHz?: number;
  /** Weight bins by tonality. Default true. */
  tonality?: boolean;

  /** Partials summed per F0 candidate; at most 128. Default 20. */
  salienceHarmonics?: number;
  /** Range a candidate may be found in, in Hz. Default 55 and 1760. */
  f0MinHz?: number;
  /** Top of the F0 candidate range, in Hz. Default `1760`. */
  f0MaxHz?: number;
  /** Offset and scale of the harmonic weighting, in Hz. Default 27 and 320. */
  salienceAlphaHz?: number;
  /** Scale of the harmonic weighting, in Hz. Default `320`. */
  salienceBetaHz?: number;
  /**
   * Partial-series stretch assumed while scoring a candidate. 0 is both the
   * default and a value, and needs no sentinel: a stretch of zero is what a
   * harmonic series is.
   */
  salienceInharmonicity?: number;

  /** Voices a frame is allowed; at most 64. Default 4. */
  maxPolyphony?: number;
  /**
   * Share of a frame's peak salience below which the estimation stops iterating.
   * Absent = 0.20; an explicit 0 is a real zero; a negative is a `RangeError`.
   */
  minFramePeakRatio?: number;
  /**
   * Closest two candidates in one frame may sit, in cents. Absent = 50; an
   * explicit 0 is a real zero; a negative is a `RangeError`.
   */
  minSeparationCents?: number;
  /** Share of a found voice removed before the next iteration. Default 1. */
  subtractionFactor?: number;

  /** A jump larger than this breaks a ridge, in cents. Default 50. */
  maxJumpCents?: number;
  /**
   * A fade below this share of a ridge's own peak breaks it. Absent = 0.10; an
   * explicit 0 is a real zero; a negative is a `RangeError`.
   */
  minRidgePeakRatio?: number;
  /**
   * Shorter ridges are dropped, in ms. Absent = 140; an explicit 0 is a real
   * zero; a negative is a `RangeError`.
   */
  minRidgeDurationMs?: number;

  /** Partials claimed per note; at most 128. Default 20. */
  maskHarmonics?: number;
  /** Claim half-width, in Hann main lobes. Default 1. */
  claimLobes?: number;
  /**
   * Stretch of the partial series, `B` in `f_h = h*f0*sqrt(1 + B*h^2)`. 0 is both
   * the default and a value.
   *
   * Leaving it at 0 for material that is stretched costs more than a widened
   * claim would: at a piano's `1e-4` the highest partial of a twenty-harmonic
   * claim sits outside the claim entirely, and a partial outside every claim is
   * residual — carried unedited, so it keeps sounding at the old pitch after its
   * note is moved.
   */
  inharmonicity?: number;
  /**
   * Fit a stretch per note from the spectrum instead of spending
   * {@link PolyphonicAnalysisOptions.inharmonicity} on every one of them.
   * Default false, and the reason is reach rather than cost: at this framing the
   * fit takes an isolated note in the middle register and refuses a chord.
   *
   * A refused note keeps the declared stretch, so the fit only ever replaces a
   * guess with a measurement. {@link PolyphonicAnalysis.noteInharmonicity}
   * reports which notes it reached.
   */
  estimateInharmonicity?: boolean;
  /** Usable partials a fit needs before it is attempted. Default 3. */
  inharmonicityMinPartials?: number;
  /** Largest per-partial misfit a fit may keep, in bins. Default 0.5. */
  inharmonicityMaxResidualBins?: number;
  /** A fit above this stretch is refused. Default 0.03125. */
  inharmonicityMaxStretch?: number;

  /** Frames per apportionment fit; between 4 and 64. Default 8. */
  windowFrames?: number;
  /** Radians per frame two partials must differ by. Default 0.01. */
  minPartialSeparation?: number;
  /** Relative misfit ceiling above which a shared bin keeps the equal share. Default 0.02. */
  maxFitResidual?: number;
  /**
   * Ceiling on one fitted weight's modulus. A weight is a fitted component over
   * the observed bin, so where two partials nearly cancel it exceeds one and the
   * residual carries several times the input there; while every edit is identity
   * that is inaudible, because the notes and the residual still sum to the input.
   * Lowering it trades separation for a quieter residual. Default 8.
   */
  maxWeightModulus?: number;
  /** Highest partial usable to refine an F0, in Hz. Absent derives one; an explicit 0 is a `RangeError`. */
  maxRefineHz?: number;
  /** Worst F0 error tolerated while refining, in cents. Default 50. */
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
  /**
   * A re-strike whose level rises to this multiple of the preceding trough
   * splits a ridge into two notes. Must be greater than 1; absent leaves
   * splitting off, and an explicit 0, a negative or a non-finite value is a
   * `RangeError`.
   */
  reattackRatio?: number;
  /** Reference pitch each note's `medianCents` is measured against. Default 440. */
  referenceHz?: number;
}

/** Options for {@link PolyphonicAnalysis.render}. All fields are optional. */
export interface PolyphonicRenderOptions {
  /**
   * Cross-fade at each edited note's edges, weighted to hold the level whether
   * the note is coherent with the audio under it or not. Default 5 ms; a hard cut
   * is deliberately not selectable, because the seam it leaves is a click.
   */
  fadeMs?: number;
  /**
   * Boundary between the drift and the vibrato that `vibratoDepthChange` and
   * `driftChange` act on, in Hz. Default 3. Pass whatever a curve edit was drawn
   * at: a host that draws the vibrato at one cutoff and edits it at another edits
   * a curve it never showed anyone.
   */
  vibratoCutoffHz?: number;
}

/**
 * A pending, non-destructive change to one percussive event.
 * {@link extractPercussiveEvents} attaches the identity edit (no move, unity
 * gain, unmuted) to every event it returns; {@link renderPercussiveEvents}
 * applies whatever the caller has changed.
 *
 * A struck sound has no steady pitch to edit, so the axes are time and amplitude
 * and there is deliberately nothing else here.
 */
export interface PercussiveEventEdit {
  /**
   * Moves the hit along the timeline; negative moves it earlier. Where it lands
   * is not bounds-checked against the other events, so two moved hits may be
   * written over each other; a shift past either end is truncated there rather
   * than wrapped.
   */
  timeOffsetSamples: number;
  /**
   * Level change applied to the hit. It scales the percussive component of the
   * span — the signal `PercussiveEvent.peakAmplitude` is measured on — and not
   * whatever else is sounding through it.
   */
  gainDb: number;
  /** Silences the hit; the other fields then do not apply. */
  muted: boolean;
}

/**
 * A {@link PercussiveEventEdit} as supplied to {@link renderPercussiveEvents}.
 * Every field is optional and an omitted one is the identity, so `{}` leaves the
 * hit untouched.
 */
export type PercussiveEventEditInput = Partial<PercussiveEventEdit> & {
  /**
   * {@link PercussiveEventEdit.timeOffsetSamples} in seconds, rounded to the
   * nearest sample at the call's sample rate; negative moves the hit earlier.
   * Not with `timeOffsetSamples`.
   */
  timeOffsetSec?: number;
};

/**
 * One editable percussive event returned by {@link extractPercussiveEvents}.
 *
 * Sample bounds are half-open into the source audio. `onsetSample` and
 * `offsetSample` are 64-bit on the C side and arrive as JS numbers, which are
 * exact up to `Number.MAX_SAFE_INTEGER`.
 *
 * It carries no pitch and is never associated with a {@link NoteObject}: the two
 * models come from separate calls and do not refer to each other. The three
 * measured figures are taken on the percussive component of the span, because
 * that is the signal an edit acts on.
 */
export interface PercussiveEvent {
  /** First sample of the hit's span, backtracked to in front of the transient. */
  onsetSample: number;
  /** One past the last sample of the span; the next onset, or the span cap. */
  offsetSample: number;
  /**
   * Detector strength at the onset, on the onset envelope's own scale. It orders
   * events against each other and carries no absolute meaning.
   */
  strength: number;
  /**
   * Peak absolute sample of the percussive component over the span, linear.
   * Measured on the signal `edit.gainDb` scales rather than on the source, so a
   * quiet hit under a loud sustain reads at its own level.
   */
  peakAmplitude: number;
  /**
   * Share of the span's energy the separation assigned to percussion, in
   * `[0, 1]`; absent when the span is silent.
   *
   * It describes the span rather than the onset that opened it. An isolated hit
   * sits near 1, but a real hit over a loud sustain sits near 0, because the
   * sustain owns the span's energy. So it separates a hit from a note attack
   * only where nothing is sustaining through both, and it is not a test for
   * whether a hit is there.
   */
  percussiveRatio?: number;
  /** This event's pending edit; the identity as returned. */
  edit: PercussiveEventEdit;
}

/**
 * An event handed to {@link renderPercussiveEvents}. Only the span and the edit
 * are read — the measured figures are ignored — so a {@link PercussiveEvent}
 * straight from {@link extractPercussiveEvents} can be passed back with its
 * `edit` changed and nothing else.
 */
export interface PercussiveEventInput {
  /** First sample of the hit's span. Give this or `onsetSec`. */
  onsetSample?: number;
  /** First second of the span, rounded to the nearest sample. Not with `onsetSample`. */
  onsetSec?: number;
  /** One past the last sample of the span; must be greater than the onset. Give this or `offsetSec`. */
  offsetSample?: number;
  /** End of the span in seconds, rounded to the nearest sample. Not with `offsetSample`. */
  offsetSec?: number;
  /** Omit for the identity edit. */
  edit?: PercussiveEventEditInput;
}

/**
 * One note's pitch curve split into a centre, a slow drift and a vibrato, as
 * returned by {@link decomposeNotePitch}.
 *
 * `driftCents[i] + vibratoCents[i]` is the note's own pitch at frame `i`, in
 * cents above `centerHz`, so the three parts reconstruct the curve.
 */
export interface PitchDecompositionResult {
  /** The note's steady pitch in Hz, or 0 when it carries no usable pitch. */
  centerHz: number;
  /** Slow deviation from `centerHz` in cents, one entry per frame. */
  driftCents: Float32Array;
  /** Fast deviation in cents, over the same frames. */
  vibratoCents: Float32Array;
}

/** Options for {@link noteMove}. One of `targetOnsetSample` / `targetOnsetSec` is required. */
export interface NoteMoveOptions {
  /** First sample of the note to move. Default 0. Not with `onsetSec`. */
  onsetSample?: number;
  /** First second of the note to move, rounded to the nearest sample. Not with `onsetSample`. */
  onsetSec?: number;
  /** Defaults to the input length. Not with `offsetSec`. */
  offsetSample?: number;
  /** Last second of the note to move, rounded to the nearest sample. Not with `offsetSample`. */
  offsetSec?: number;
  /** Sample the note's onset moves to. Required unless given as seconds. Not with `targetOnsetSec`. */
  targetOnsetSample?: number;
  /** Second the note's onset moves to, rounded to the nearest sample. Not with `targetOnsetSample`. */
  targetOnsetSec?: number;
}
