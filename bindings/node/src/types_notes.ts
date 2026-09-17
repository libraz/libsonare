/**
 * Note- and event-level types: the editable objects a polyphonic or
 * percussive decomposition produces, and the options that drive it.
 */

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

/** Options for {@link pitchCorrectTimevarying}. All fields are optional. */

export interface PitchCorrectOptions {
  /** `'midi'` retunes toward {@link targetMidi}; `'scale'` snaps to the key. Default `'midi'`. */
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

/** Options for {@link noteStretch}. All fields are optional. */

export interface NoteStretchOptions {
  /** First sample of the note to stretch. Default 0. */
  onsetSample?: number;
  /** Last sample of the note to stretch. Defaults to the input length. */
  offsetSample?: number;
  /** Stretch ratio (1 = unchanged), a finite number. Default 1. */
  stretchRatio?: number;
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
   * Value of `voicedProb` at or above which a frame counts as voiced. Read only
   * when `voiced` is omitted. Default 0.5.
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
 */
export interface NoteEdit {
  /**
   * Moves the note along the timeline; negative moves it earlier. Where the note
   * lands is not bounds-checked, so a moved note may overwrite a neighbour.
   */
  timeOffsetSamples: number;
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
  amplitudeEnvelope: Float32Array;
  /** Transpose applied to the note's span. */
  pitchShiftSemitones: number;
  /** Level change applied to the note's span. */
  gainDb: number;
  /** `>1` lengthens the note, `<1` shortens it; pitch is preserved. */
  timeStretchRatio: number;
  /**
   * Moves the spectral envelope, in semitones, on top of whatever the pitch
   * shift already did to it.
   *
   * 0 runs no warp at all, so a pitch-only edit is not charged for an LPC
   * analysis-resynthesis round it did not ask for. A pitch shift drags the
   * formants with it, so holding them still is `-pitchShiftSemitones` and the
   * chipmunk is the default. Saturates near -10.3 and +8.7 semitones rather than
   * being rejected.
   */
  formantShiftSemitones: number;
  /**
   * Scales the vibrato measured over the note, stated as a change from it: 0
   * keeps it, -1 flattens it, +1 doubles it.
   *
   * Applying it needs the note's pitch curve, so {@link renderNotes} requires
   * `f0Hz` and the note's `frameStart` / `frameEnd` / `medianHz`; a note carrying
   * none is rejected rather than left alone. The curve is split at the request's
   * `vibratoCutoffHz`.
   */
  vibratoDepthChange: number;
  /** The same, for the slow drift around the note's centre pitch. */
  driftChange: number;
  /** Silences the note's span; the other fields then do not apply. */
  muted: boolean;
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
  amplitudeEnvelope?: readonly number[] | Float32Array;
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
  /**
   * Pitch steadiness in `[0, 1]`; 1 is perfectly steady.
   *
   * The only quality figure a note carries. A voiced fraction would be one too,
   * but the segmenter emits maximal voiced runs, so it is 1 for every note it
   * can produce and measures nothing.
   */
  f0Stability: number;
  /** One RMS value per frame of the span (`frameEnd - frameStart` entries). */
  amplitude: Float32Array;
  /** This note's pending edit; the identity as returned. */
  edit: NoteEdit;
}

/**
 * A note handed to {@link renderNotes}. The span, the edit, and — for a curve
 * edit — the frame bounds and median pitch are read, so a {@link NoteObject}
 * straight from {@link extractNotes} can be passed back with its `edit` changed
 * and nothing else.
 */
export interface NoteObjectInput {
  /** First sample of the note's span. */
  onsetSample: number;
  /** One past the last sample of the span. */
  offsetSample: number;
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
   */
  nFft?: number;
  hopLength?: number;
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
  /** Stops weighting bins by tonality, which is on. Default false. */
  tonalityOff?: boolean;

  /** Partials summed per F0 candidate; at most 128. Default 20. */
  salienceHarmonics?: number;
  /** Range a candidate may be found in, in Hz. Default 55 and 1760. */
  f0MinHz?: number;
  f0MaxHz?: number;
  /** Offset and scale of the harmonic weighting, in Hz. Default 27 and 320. */
  salienceAlphaHz?: number;
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
   * Default 0.20; **negative selects 0**.
   */
  minFramePeakRatio?: number;
  /**
   * Closest two candidates in one frame may sit, in cents. Default 50;
   * **negative selects 0**.
   */
  minSeparationCents?: number;
  /** Share of a found voice removed before the next iteration. Default 1. */
  subtractionFactor?: number;

  /** A jump larger than this breaks a ridge, in cents. Default 50. */
  maxJumpCents?: number;
  /**
   * A fade below this share of a ridge's own peak breaks it. Default 0.10;
   * **negative selects 0**.
   */
  minRidgePeakRatio?: number;
  /**
   * Shorter ridges are dropped, in ms. Default 140; **negative selects 0**.
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
  /** Highest partial usable to refine an F0, in Hz. 0 derives one. */
  maxRefineHz?: number;
  /** Worst F0 error tolerated while refining, in cents. Default 50. */
  f0ToleranceCents?: number;

  /** Cents of pitch change that cut one ridge into two notes. Default 50. */
  segmentationThresholdCents?: number;
  /** Shortest span kept as a note, in ms. Default 30. */
  minNoteMs?: number;
  /** Reference pitch each note's `medianCents` is measured against. Default 440. */
  referenceHz?: number;
}

/** Options for {@link PolyphonicAnalysis.render}. All fields are optional. */

export interface PolyphonicRenderOptions {
  /**
   * Equal-power cross-fade at each edited note's edges. Default 5 ms; a hard cut
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
export type PercussiveEventEditInput = Partial<PercussiveEventEdit>;

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
 * An event handed to {@link renderPercussiveEvents}. Only the span and the edit
 * are read — the measured figures are ignored — so a {@link PercussiveEvent}
 * straight from {@link extractPercussiveEvents} can be passed back with its
 * `edit` changed and nothing else.
 */
export interface PercussiveEventInput {
  /** First sample of the hit's span. */
  onsetSample: number;
  /** One past the last sample of the span; must be greater than `onsetSample`. */
  offsetSample: number;
  /** Omit for the identity edit. */
  edit?: PercussiveEventEditInput;
}

/**
 * One note's pitch curve split into a centre, a slow drift and a vibrato, as
 * returned by {@link decomposeNotePitch}.
 *
 * `driftCents[i] + vibratoCents[i]` is the note's own pitch at frame `i`, in
 * cents above `centreHz`, so the three parts reconstruct the curve.
 */
export interface PitchDecompositionResult {
  /** The note's steady pitch in Hz, or 0 when it carries no usable pitch. */
  centreHz: number;
  /** Slow deviation from `centreHz` in cents, one entry per frame. */
  driftCents: Float32Array;
  /** Fast deviation in cents, over the same frames. */
  vibratoCents: Float32Array;
}

/** Options for {@link noteMove}. */

export interface NoteMoveOptions {
  onsetSample?: number;
  /** Defaults to the input length. */
  offsetSample?: number;
  targetOnsetSample?: number;
}
