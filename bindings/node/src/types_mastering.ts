export type VoicePresetId =
  | 'neutral-monitor'
  | 'bright-idol'
  | 'soft-whisper'
  | 'deep-narrator'
  | 'robot-mascot'
  | 'dark-villain';

export type VoicePresetCategory =
  | 'monitor'
  | 'bright'
  | 'soft'
  | 'deep'
  | 'robot'
  | 'dark'
  | 'custom';

export interface RealtimeVoiceChangerPresetMetadata {
  schemaVersion: 1;
  id: string;
  name: string;
  description?: string;
  category: VoicePresetCategory;
}

export type RealtimeVoiceChangerPreset =
  | (RealtimeVoiceChangerPresetMetadata & {
      dsp: Record<string, unknown>;
      macros?: never;
    })
  | (RealtimeVoiceChangerPresetMetadata & {
      macros: Record<string, number>;
      dsp?: never;
    });

export type RealtimeVoiceChangerConfigInput =
  | VoicePresetId
  | RealtimeVoiceChangerPreset
  | RealtimeVoiceChangerConfig;

export interface RealtimeVoiceChangerOptions {
  sampleRate: number;
  maxBlockSize?: number;
  channels?: 1 | 2;
  preset?: RealtimeVoiceChangerConfigInput;
}

/**
 * Flat (normalized) realtime-voice-changer configuration, mirroring the
 * `SonareRealtimeVoiceChangerConfig` POD returned by
 * `realtimeVoiceChangerPresetConfig`.
 */
export interface RealtimeVoiceChangerConfig {
  inputGainDb: number;
  outputGainDb: number;
  wetMix: number;
  retuneSemitones: number;
  retuneMix: number;
  retuneGrainSize: number;
  formantFactor: number;
  formantAmount: number;
  formantBody: number;
  formantBrightness: number;
  formantNasal: number;
  eqHighpassHz: number;
  eqBodyDb: number;
  eqPresenceDb: number;
  eqAirDb: number;
  gateThresholdDb: number;
  gateAttackMs: number;
  gateReleaseMs: number;
  gateRangeDb: number;
  compressorThresholdDb: number;
  compressorRatio: number;
  compressorAttackMs: number;
  compressorReleaseMs: number;
  compressorMakeupGainDb: number;
  deesserFrequencyHz: number;
  deesserThresholdDb: number;
  deesserRatio: number;
  deesserRangeDb: number;
  reverbMix: number;
  reverbTimeMs: number;
  reverbDamping: number;
  reverbSeed: number;
  limiterCeilingDb: number;
  limiterReleaseMs: number;
  /** Whether the inter-sample-peak (true-peak) limiter is enabled (default true). */
  limiterEnableIspLimiter: boolean;
  /** Inter-sample-peak limiter ceiling in dBTP (default -1.0). */
  limiterIspCeilingDbtp: number;
}

/** Options for the high-level {@link mastering} one-shot. All fields are optional. */
export interface MasteringOptions {
  /**
   * Integrated-loudness target in LUFS. Default -14. Must be finite; a
   * non-finite value is rejected.
   *
   * Deliberately NOT the contract of {@link EngineBounceOptions.targetLufs},
   * which shares this name, unit and default but resolves a non-finite value to
   * the default. There the loudness stage derives a per-sample gain and checks
   * nothing itself; here the target reaches a validator that owns the domain.
   */
  targetLufs?: number;
  /** True-peak ceiling in dBTP. Default -1. Must be finite. */
  ceilingDb?: number;
  /** True-peak oversampling factor. Default 4. */
  truePeakOversample?: number;
  /**
   * Post true-peak limiter release in ms. Default 0 => library default (50 ms).
   * Any other value is used as given; a negative or non-finite one is rejected.
   */
  releaseMs?: number;
  /** Apply the static loudness gain at the input (pre-oversample) rate. Default false. */
  applyGainAtInputRate?: boolean;
}

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
 * Built-in mastering presets. Each carries an integrated-loudness target and a
 * true-peak ceiling, which are not equally binding: the ceiling always holds,
 * while the loudness target is what one normalization pass aims at.
 *
 * Reaching a target above the input's loudness costs gain the input's peak
 * headroom may not have, so the stage drives its true-peak limiter up to
 * `loudness.maxLimiterGainReductionDb` (12 dB by default) to close the distance
 * and stops there. Material needing more limiting than that — and material where
 * the limiter's own gain reduction takes back part of the applied gain, which a
 * single pass does not re-measure — finishes below target with
 * `loudnessTargetLimited` set on the result. Peak-normalized input, whose
 * headroom is ~0 dB, is the common case for both. Read `outputLufs` for what was
 * achieved rather than assuming the preset's target.
 */
export type MasteringPreset =
  | 'pop'
  | 'edm'
  | 'acoustic'
  | 'hipHop'
  | 'aiMusic'
  | 'speech'
  | 'streaming'
  | 'youtube'
  | 'broadcast'
  | 'podcast'
  | 'audiobook'
  | 'cinema'
  | 'jpop'
  | 'ambient'
  | 'lofi'
  | 'classical'
  | 'drumAndBass'
  | 'techno'
  | 'metal'
  | 'trap'
  | 'rnb'
  | 'jazz'
  | 'kpop'
  | 'trance'
  | 'gameOst';

/**
 * One delivery target `masteringStreamingPreview` reports a normalization gain
 * for.
 *
 * Both numbers are required and must be finite. Unlike
 * {@link EngineBounceOptions.targetLufs}, which shares this name, unit and
 * default, a non-finite value here selects nothing — this interface has no
 * "use the library default" spelling — so it is refused by name.
 */
export interface StreamingPlatform {
  /** Platform name, echoed into the reported result. */
  name: string;
  /** Integrated-loudness target in LUFS, e.g. -14. */
  targetLufs: number;
  /** True-peak ceiling in dBTP, e.g. -1. */
  ceilingDb: number;
}

// BEGIN GENERATED SoloProcessor (make processor-types)
/**
 * Every processor name `masteringProcessorNames()` can return, and therefore
 * every name `masteringProcess` / `masteringProcessStereo` accept.
 */
export const SOLO_PROCESSORS = [
  'dynamics.brickwallLimiter',
  'dynamics.compressor',
  'dynamics.deesser',
  'dynamics.duckingProcessor',
  'dynamics.expander',
  'dynamics.gate',
  'dynamics.limiter',
  'dynamics.parallelComp',
  'dynamics.sidechainRouter',
  'dynamics.transientShaper',
  'dynamics.upwardCompressor',
  'dynamics.upwardExpander',
  'dynamics.vocalRider',
  'effects.acoustic.roomMorph',
  'effects.delay.stereo',
  'effects.modulation.autoWah',
  'effects.modulation.chorus',
  'effects.modulation.ensemble',
  'effects.modulation.flanger',
  'effects.modulation.phaser',
  'effects.modulation.pitchShifter',
  'effects.modulation.ringModulator',
  'effects.modulation.rotary',
  'effects.modulation.wah',
  'effects.reverb.convolution',
  'effects.reverb.dattorro',
  'effects.reverb.fdn',
  'effects.reverb.plate',
  'effects.reverb.room',
  'effects.reverb.velvet',
  'eq.apiStyle',
  'eq.bandPass',
  'eq.cutFilter',
  'eq.dynamic',
  'eq.equalizer',
  'eq.graphic',
  'eq.linearPhase',
  'eq.midSide',
  'eq.minimumPhase',
  'eq.parametric',
  'eq.pultec',
  'eq.shelving',
  'eq.tilt',
  'final.bitDepth',
  'final.dither',
  'final.outputChain',
  'maximizer.adaptiveRelease',
  'maximizer.loudnessOptimize',
  'maximizer.maximizer',
  'maximizer.softKneeMax',
  'maximizer.truePeakLimiter',
  'multiband.compressor',
  'multiband.dynamicEq',
  'multiband.expander',
  'multiband.imager',
  'multiband.limiter',
  'multiband.saturation',
  'repair.declick',
  'repair.declip',
  'repair.decrackle',
  'repair.dehum',
  'repair.denoiseClassical',
  'repair.dereverbClassical',
  'repair.trimSilence',
  'saturation.ampSim',
  'saturation.bitcrusher',
  'saturation.exciter',
  'saturation.hardClipper',
  'saturation.multibandExciter',
  'saturation.softClipper',
  'saturation.tape',
  'saturation.transformer',
  'saturation.tube',
  'saturation.waveshaper',
  'spectral.airBand',
  'spectral.lowEndFocus',
  'spectral.presenceEnhancer',
  'spectral.spectralShaper',
  'stereo.autoPan',
  'stereo.haasEnhancer',
  'stereo.imager',
  'stereo.monoMaker',
  'stereo.phaseAlign',
  'stereo.stereoBalance',
] as const;

export type SoloProcessor = (typeof SOLO_PROCESSORS)[number];
// END GENERATED SoloProcessor

export type PairProcessor =
  | 'match.applyMatchEq'
  | 'match.alignReferenceToSource'
  | 'match.abSwitch'
  | 'match.abCrossfade';

export type PairAnalysis =
  | 'match.referenceLoudness'
  | 'match.tonalBalance'
  | 'match.tonalBalanceLogBands'
  | 'match.matchEqCurve'
  | 'match.estimateReferenceDelaySamples';

export type StereoAnalysis = 'stereo.monoCompatCheck' | 'stereo.monoCompatCheckLogBands';

export interface MasteringResult {
  samples: Float32Array;
  sampleRate: number;
  inputLufs: number;
  outputLufs: number;
  appliedGainDb: number;
  /** True when peak headroom prevented the requested LUFS target. */
  loudnessTargetLimited?: boolean;
  latencySamples?: number;
  /**
   * Samples the named processor replaced with a finite in-domain one,
   * keeping the output finite and in range.
   *
   * A non-finite sample supplied by the caller is rejected before the
   * processor runs, so a replacement is always of a value the processor
   * itself produced.
   *
   * Whether this can be non-zero depends on which processor was named: one
   * that does not substitute reports zero because it has nothing to replace
   * with, not because nothing needed replacing.
   *
   * @example
   * ```ts
   * const result = masteringProcess('maximizer.truePeakLimiter', samples, 44100);
   * if (result.nonFiniteSubstitutionCount > 0) {
   *   // part of `result.samples` is unrelated to `samples`
   * }
   * ```
   */
  nonFiniteSubstitutionCount: number;
}

export interface MasteringStereoResult {
  left: Float32Array;
  right: Float32Array;
  sampleRate: number;
  inputLufs: number;
  outputLufs: number;
  appliedGainDb: number;
  latencySamples: number;
  /** True when peak headroom prevented the requested LUFS target. */
  loudnessTargetLimited: boolean;
  /**
   * See {@link MasteringResult.nonFiniteSubstitutionCount}. Summed over both
   * channels.
   */
  nonFiniteSubstitutionCount: number;
}

/** One channel's click detection, from {@link masteringRepairDeclickStereo}. Counts runs, not samples. */
export interface ClickDetection {
  /** Runs meeting the repair criteria. */
  count: number;
  /** Runs the criteria excluded as outliers. */
  rejected: number;
  /** Longest counted run, in samples. */
  longestRunSamples: number;
  /** `count` divided by the input duration. */
  perSecond: number;
}

/**
 * What one channel's declick pass found and what it did to it, from
 * {@link masteringRepairDeclickStereo}.
 *
 * A large `detected.rejected` says the configured run length or neighbour
 * ratio is too tight for this material, not that the material is clean.
 */
export interface DeclickReport {
  /** This channel's own analysis of the input. */
  detected: ClickDetection;
  /** Runs interpolated. */
  repairedRuns: number;
  /** Samples overwritten by interpolation. */
  repairedSamples: number;
  /**
   * Of `repairedRuns`, those this channel's own detection did not produce --
   * they were selected because the other channel's detector found them.
   */
  linkedRuns: number;
  /** False when the input was too short for `lpcOrder`, which reduces every fill to linear interpolation. */
  lpcModelUsed: boolean;
}

/**
 * A declicked stereo pair and what each channel's pass did, from
 * {@link masteringRepairDeclickStereo}.
 *
 * A run either channel's detector selects is repaired in both, so a
 * common-mode click never moves the stereo image; only the selection is
 * shared, and each channel's fill comes from its own samples and its own
 * model, which is why `leftReport` and `rightReport` can differ.
 */
export interface DeclickStereoResult {
  left: Float32Array;
  right: Float32Array;
  leftReport: DeclickReport;
  rightReport: DeclickReport;
}

/** One channel's clip detection, from {@link masteringRepairDeclipStereo}. Counts runs, not samples. */
export interface ClipDetection {
  /** Samples at or past the clip threshold. */
  sampleCount: number;
  /** `sampleCount` divided by the input length. */
  sampleFraction: number;
  /** Runs of consecutive clipped samples. */
  runCount: number;
  /** A run past the 512-sample cap takes the interpolation fallback instead of the solver. */
  longestRunSamples: number;
}

/**
 * What one channel's declip pass found and what it did to it, from
 * {@link masteringRepairDeclipStereo}.
 */
export interface DeclipReport {
  /** This channel's own analysis of the input. */
  detected: ClipDetection;
  /** Runs the LPC solver filled. */
  lpcReconstructedRuns: number;
  /**
   * Runs past the 512-sample cap, filled by interpolation instead: for these,
   * `lpcOrder`, `iterations` and `lpcBlend` had no effect.
   */
  interpolatedRuns: number;
  /** Samples overwritten by either fill. */
  repairedSamples: number;
  /**
   * Of the repaired runs, those reaching past this channel's own clipped
   * samples because the other channel's run was wider.
   */
  linkedRuns: number;
}

/**
 * A declipped stereo pair and what each channel's pass did, from
 * {@link masteringRepairDeclipStereo}.
 *
 * Each channel reconstructs the whole of every union run it has at least one
 * clipped sample in; a channel with no clipped sample in a run is left
 * untouched there, so `leftReport` and `rightReport` can genuinely differ.
 */
export interface DeclipStereoResult {
  left: Float32Array;
  right: Float32Array;
  leftReport: DeclipReport;
  rightReport: DeclipReport;
}

/**
 * One channel's crackle detection, from {@link masteringRepairDecrackleStereo}.
 *
 * Measured by the median criterion whatever `mode` is configured: wavelet
 * shrinkage removes crackle without ever deciding a sample is crackle, so
 * these counts do not describe what wavelet mode repaired.
 */
export interface CrackleDetection {
  /** Samples deviating from the local median by more than `threshold`. */
  sampleCount: number;
  /** `sampleCount` divided by the input length. */
  sampleFraction: number;
  perSecond: number;
}

/**
 * What one channel's decrackle pass found and what it did to it, from
 * {@link masteringRepairDecrackleStereo}.
 *
 * The two modes report through different fields: median mode fills
 * `replacedSamples` only, wavelet mode fills `detailCoefficients`,
 * `shrunkCoefficients` and `noiseSigma` only. The field belonging to the
 * other mode reads zero because that mode did not run, which the caller
 * knows from the config it passed rather than from the value.
 */
export interface DecrackleReport {
  /** This channel's own analysis of the input. */
  detected: CrackleDetection;
  /** Median mode: samples the filter overwrote, equal to `detected.sampleCount`. */
  replacedSamples: number;
  /** Wavelet mode: detail coefficients examined by the unshifted pass, not by every pass the mode averages. */
  detailCoefficients: number;
  /** Wavelet mode: of those, driven to zero. */
  shrunkCoefficients: number;
  /** Wavelet mode: the MAD noise estimate that set every level's threshold, which `threshold` only caps. */
  noiseSigma: number;
}

/**
 * A decrackled stereo pair and what each channel's pass did, from
 * {@link masteringRepairDecrackleStereo}.
 *
 * Crackle is surface damage: the two channels carry different scratches at
 * different instants, so each channel is decrackled independently. Unlike
 * declick and declip, no run is ever widened to match the other channel and
 * no report field counts such a widening.
 */
export interface DecrackleStereoResult {
  left: Float32Array;
  right: Float32Array;
  leftReport: DecrackleReport;
  rightReport: DecrackleReport;
}

/**
 * What a denoise analysis found in the input, from
 * {@link masteringRepairDenoiseClassicalStereo}.
 *
 * These are absolute levels, which makes them the one part of a stereo
 * denoise report that depends on how many channels were passed: the estimator
 * runs on the channel-summed power, so two identical channels read about 3 dB
 * above the same material through the mono entry point. Compare a stereo floor
 * against another stereo floor, never against a mono one.
 */
export interface NoiseDetection {
  /** Broadband estimated noise floor, in dBFS. */
  floorDbfs: number;
  /**
   * The noise floor's shape, low band to high, length 32. A geometric grid
   * from 20 Hz to Nyquist -- the same axis the mastering report's
   * `bandEnergyDeltaDb` uses, so a noise floor and a tonal-balance change can
   * be read together.
   */
  bandFloorDbfs: number[];
}

/**
 * What a denoise pass found and what it removed, from
 * {@link masteringRepairDenoiseClassicalStereo}.
 */
export interface DenoiseReport {
  /** Analysis of the input, before the mask. */
  detected: NoiseDetection;
  /**
   * Mean attenuation the gain mask applied. Zero reads the same whether the
   * mask was transparent or no mask ran at all.
   */
  meanReductionDb: number;
  /**
   * Deepest attenuation any cell applied. At `reductionDb` the floor set the
   * depth rather than the estimate.
   */
  maxReductionDb: number;
  /**
   * Fraction of cells sitting on that floor. Always 0 in
   * `spectralSubtraction` mode, which floors on `spectralFloor` instead, so 0
   * from that mode is the mode and not a measurement.
   */
  floorLimitedFraction: number;
}

/**
 * A denoised stereo pair and the one mask that produced it, from
 * {@link masteringRepairDenoiseClassicalStereo}.
 *
 * One `report` rather than a per-channel pair: the gain mask is built from the
 * channel-summed power and applied unchanged to both channels, so a pair would
 * be two copies of one measurement and would read as though the two could
 * differ.
 */
export interface DenoiseStereoResult {
  left: Float32Array;
  right: Float32Array;
  report: DenoiseReport;
}

/**
 * A denoised channel set and the one mask that produced it, from
 * {@link masteringRepairDenoiseClassicalLinked}.
 *
 * The N-channel form of {@link DenoiseStereoResult}, carrying one `report` for
 * the same reason: the gain mask is built from the channel-summed power and
 * applied unchanged to every channel, so a per-channel pair would be N copies of
 * one measurement.
 *
 * `report.detected` is a measurement of the SET. Its levels are absolute dBFS
 * taken on the summed power, so N identical channels read `10*log10(N)` dB above
 * one of them alone. The attenuation figures on {@link DenoiseReport} are
 * fractions and do not move with the channel count.
 */
export interface DenoiseLinkedResult {
  /** One output per input channel, in input order. */
  channels: Float32Array[];
  report: DenoiseReport;
}

/**
 * What a dehum analysis found, from {@link masteringRepairDehumStereo}.
 *
 * Always measured through the estimation path, whatever `adaptive` is
 * configured to: the fixed path notches the configured frequency without
 * ever looking for hum, so a detector following the flag would hand back its
 * own input.
 */
export interface HumDetection {
  /** Tracked fundamental; the configured value when adaptive tracking is off. */
  fundamentalHz: number;
  /**
   * Winning candidate's projected energy over the median candidate. 1.0 means
   * no peak was found at all. Not a lock flag.
   */
  fundamentalProminence: number;
  /** Harmonics above the floor, not necessarily a contiguous run from the first. */
  harmonics: number;
  /**
   * Input level at each k*f0, k ascending, length 16. Measured for every k the
   * sample rate carries, not only the notched ones; a k*f0 at or past Nyquist
   * reads the dB floor because nothing is there to measure.
   */
  harmonicDbfs: number[];
}

/**
 * What one channel's dehum pass found and what it did to it, from
 * {@link masteringRepairDehumStereo}.
 */
export interface DehumReport {
  /** This channel's own analysis, before filtering. */
  detected: HumDetection;
  /** Harmonics the cascade reached; fewer than the configured count once k*f0 hits Nyquist. */
  notchedHarmonics: number;
  /** Frequency the last notch refresh used. */
  appliedFundamentalHz: number;
  /**
   * Largest excursion of the tracked frequency from the configured one. Zero
   * without adaptive tracking, which is the measurement rather than an unset
   * field.
   */
  fundamentalDriftHz: number;
}

/**
 * A dehummed stereo pair and what each channel's pass did, from
 * {@link masteringRepairDehumStereo}.
 *
 * With `adaptive` set, the tracker reads the channel mean and both cascades
 * follow the one frequency it finds, so `leftReport` and `rightReport` share
 * the same `appliedFundamentalHz` and `fundamentalDriftHz` by construction;
 * only the frequency is shared, so each report's `detected` still measures
 * that channel's own input. Without `adaptive`, which is the default,
 * nothing is shared and the two channels are filtered independently.
 */
export interface DehumStereoResult {
  left: Float32Array;
  right: Float32Array;
  leftReport: DehumReport;
  rightReport: DehumReport;
}

/**
 * What a dereverb analysis found in the input, from
 * {@link masteringRepairDereverbClassicalStereo}.
 *
 * NOT an ISO 3382 reverberation time: no Schroeder integration, no
 * noise-floor truncation, STFT bins rather than octave bands, and music is not
 * a free decay. Use {@link estimateRoom} for a graded RT60; this reports what
 * the module itself measured while deciding how much to subtract.
 */
export interface ReverbDetection {
  /**
   * Decay across the module's own late lag, in dB. Less negative means the
   * material sustains across that lag, which a late tail does and a dry offset
   * does not -- so a reverberant input reads *higher* here than the same
   * material dry, which is the opposite of what the name suggests.
   */
  lateDecayRatioDb: number;
  /**
   * Mean WPE predictor norm before the clamp. Zero whenever the WPE stage did
   * not run, which is the case unless `wpeEnabled` is set -- and it is clear
   * by default, so a default-config pass reports 0 here as its measurement.
   */
  latePredictability: number;
}

/**
 * What a dereverb pass found and what it removed, from
 * {@link masteringRepairDereverbClassicalStereo}.
 */
export interface DereverbReport {
  /** Analysis of the input. */
  detected: ReverbDetection;
  /** Mean attenuation the subtraction applied. */
  meanReductionDb: number;
  /**
   * Fraction of cells the `threshold` gate admitted as late reverberation. The
   * only observation of that knob: 0 alongside a nonzero `meanReductionDb`
   * says the gate admitted nothing.
   */
  suppressedFraction: number;
  /**
   * Mean predictor norm actually applied, after the clamp. Below
   * `detected.latePredictability` says the clamp acted, an otherwise silent
   * branch. Zero when the WPE stage did not run, so 0 by default.
   */
  wpePredictorNorm: number;
}

/**
 * A dereverberated stereo pair and the one mask that produced it, from
 * {@link masteringRepairDereverbClassicalStereo}.
 *
 * One `report` rather than a per-channel pair: the mask is built from the
 * channel-summed power and the WPE stage accumulates over both channels, so a
 * pair would be two copies of one measurement. Every field of that report is a
 * ratio or a fraction, so unlike {@link NoiseDetection} nothing here shifts
 * with the channel count and a stereo figure is comparable against a mono one.
 */
export interface DereverbStereoResult {
  left: Float32Array;
  right: Float32Array;
  report: DereverbReport;
}

/**
 * A dereverberated channel set and the one mask that produced it, from
 * {@link masteringRepairDereverbClassicalLinked}.
 *
 * The N-channel form of {@link DereverbStereoResult}: one mask over the
 * channel-summed power and one WPE predictor set fitted over every channel's
 * statistics, so a per-channel report pair would be N copies of one measurement.
 *
 * Every field of that report is a ratio or a fraction, so unlike
 * {@link DenoiseLinkedResult} nothing here shifts with the channel count and a
 * figure measured over a set is comparable against a mono one.
 */
export interface DereverbLinkedResult {
  /** One output per input channel, in input order. */
  channels: Float32Array[];
  report: DereverbReport;
}

/**
 * One half-open sample range, in INPUT-buffer coordinates, from
 * {@link masteringRepairTrimSilenceStereo}.
 *
 * The coordinates are the input's, so `lastExclusive - first` is the number of
 * samples the range covers and the returned channels are that long -- not the
 * length they are indexed by.
 */
export interface TrimRange {
  /** First kept sample. */
  first: number;
  /** One past the last kept sample. */
  lastExclusive: number;
}

/**
 * What a trim pass kept and what it dropped, from
 * {@link masteringRepairTrimSilenceStereo}.
 *
 * A pass that kept nothing reports the range `(length, length)`, which counts
 * the whole buffer as removed head and leaves removed tail at 0. The two still
 * sum to the input length, so a caller reporting how much went reads the right
 * total; only the split between the ends is arbitrary there.
 */
export interface TrimReport {
  /** The kept range, padding included. */
  range: TrimRange;
  /** Samples dropped before `range.first`. */
  removedHeadSamples: number;
  /** Samples dropped after `range.lastExclusive`. */
  removedTailSamples: number;
}

/**
 * A trimmed stereo pair, the range both channels were cut to, and the two
 * per-channel ranges that range is the union of, from
 * {@link masteringRepairTrimSilenceStereo}.
 *
 * Unlike every other repair stereo result, `left` and `right` are SHORTER than
 * the input -- trimming is the point -- and they are empty when neither channel
 * carried signal, which is a success rather than an error. There is no separate
 * length field: `left.length` is the output length.
 *
 * `report.range` is the union that was applied to both channels. `leftRange`
 * and `rightRange` are the per-channel scans it was formed from, so a caller
 * can see which channel decided each edge; a channel carrying nothing reports
 * an empty range and contributes nothing to the union.
 */
export interface TrimSilenceStereoResult {
  left: Float32Array;
  right: Float32Array;
  report: TrimReport;
  leftRange: TrimRange;
  rightRange: TrimRange;
}

/** What gain-matching one take to another's loudness took, and produced. */
export interface LoudnessMatchResult {
  /** The source, gain-matched to the reference's integrated loudness. */
  samples: Float32Array;
  sampleRate: number;
  /**
   * The reference's BS.1770 integrated loudness. Non-finite for a silent or
   * below-gate take, which is also when `appliedGainDb` is 0.
   */
  referenceLufs: number;
  /** The source's, before the gain. Same non-finite case. */
  sourceLufs: number;
  /** Gain applied to the source, in dB. */
  appliedGainDb: number;
  /**
   * The matched take's true peak after the gain, in dBTP. It can sit above
   * 0 dBTP: the gain is applied with no upper bound, because clamping for
   * headroom would return the source at its own loudness whenever it started
   * near full scale, which is the one thing a loudness match must not do.
   * Limiting is the caller's decision, so a value above 0 is a report rather
   * than a defect.
   */
  matchedTruePeakDbtp: number;
}

/** Generic traversal view used by chain-config tooling; public configs are fully typed below. */
export type MasteringChainSection = Record<string, unknown>;

/**
 * Nested mastering-chain configuration. Top-level keys are the processing
 * modules; nest processor and parameter names beneath them, e.g.
 *
 * ```ts
 * masteringChain(samples, sr, {
 *   dynamics: { compressor: { thresholdDb: -24 } },
 *   loudness: { targetLufs: -14 },
 * });
 * ```
 *
 * A boolean toggles a module/processor's `enabled` flag; setting any field
 * implicitly enables its module unless `enabled: false` is also given. Unknown
 * keys throw at apply time. (`stereo.*` modules apply on the stereo path only.)
 *
 * Exception — color stages as `masterAudio` overrides: the `saturation.tape`
 * and `saturation.exciter` stages are engaged from an override only when you
 * pass `enabled: true` explicitly. On a preset where they are off, adjusting a
 * parameter alone (e.g. `saturation: { tape: { driveDb: 6 } }`) has no audible
 * effect; use `saturation: { tape: { enabled: true, driveDb: 6 } }`.
 */
export interface MasteringChainConfig {
  repair?: {
    /** `boolean` is retained as a deprecated shorthand for `{ enabled }`. */
    denoise?:
      | boolean
      | {
          enabled?: boolean;
          nFft?: number;
          hopLength?: number;
          ddAlpha?: number;
          reductionDb?: number;
          /** @deprecated Use `reductionDb`; converted to it (dB = -20*log10(gainFloor)). */
          gainFloor?: number;
          overSubtraction?: number;
          spectralFloor?: number;
          noiseEstimationQuantile?: number;
          speechPresenceGain?: boolean;
          gainSmoothing?: boolean;
        };
    nFft?: number;
    hopLength?: number;
    ddAlpha?: number;
    reductionDb?: number;
    /** @deprecated Use `denoise.reductionDb`; converted to it (dB = -20*log10(gainFloor)). */
    gainFloor?: number;
    declip?: {
      enabled?: boolean;
      clipThreshold?: number;
      lpcOrder?: number;
      iterations?: number;
      lpcBlend?: number;
    };
    decrackle?: {
      enabled?: boolean;
      threshold?: number;
      /** 0 = median, 1 = wavelet shrinkage. */
      mode?: number;
      levels?: number;
    };
    dehum?: {
      enabled?: boolean;
      fundamentalHz?: number;
      harmonics?: number;
      q?: number;
      adaptive?: boolean;
      searchRangeHz?: number;
      adaptation?: number;
      frameSize?: number;
      pllBandwidth?: number;
      /** 0 = subtract the tracked harmonics, 1 = cascaded notches. */
      mode?: number;
    };
    declick?: {
      enabled?: boolean;
      threshold?: number;
      neighborRatio?: number;
      maxClickSamples?: number;
      lpcOrder?: number;
      residualRatio?: number;
    };
    dereverb?: {
      enabled?: boolean;
      threshold?: number;
      attenuation?: number;
      nFft?: number;
      hopLength?: number;
      t60Sec?: number;
      lateDelayMs?: number;
      overSubtraction?: number;
      spectralFloor?: number;
      wpeEnabled?: boolean;
      wpeIterations?: number;
      wpeTaps?: number;
      wpeStrength?: number;
    };
  };
  eq?: {
    tilt?: {
      enabled?: boolean;
      tiltDb?: number;
      pivotHz?: number;
    };
    /** @deprecated Use `eq.tilt.tiltDb`. */
    tiltDb?: number;
    /** @deprecated Use `eq.tilt.pivotHz`. */
    pivotHz?: number;
  };
  dynamics?: {
    compressor?: {
      enabled?: boolean;
      thresholdDb?: number;
      ratio?: number;
      attackMs?: number;
      releaseMs?: number;
      kneeDb?: number;
      makeupGainDb?: number;
      autoMakeup?: boolean;
    };
    deesser?: {
      enabled?: boolean;
      frequencyHz?: number;
      thresholdDb?: number;
      ratio?: number;
      attackMs?: number;
      releaseMs?: number;
      rangeDb?: number;
      bandpassQ?: number;
    };
    transientShaper?: {
      enabled?: boolean;
      attackGainDb?: number;
      sustainGainDb?: number;
      fastAttackMs?: number;
      fastReleaseMs?: number;
      slowAttackMs?: number;
      slowReleaseMs?: number;
      sensitivity?: number;
      maxGainDb?: number;
      gainSmoothingMs?: number;
      lookaheadMs?: number;
    };
    multibandComp?: {
      enabled?: boolean;
      lowCutoffHz?: number;
      highCutoffHz?: number;
      lowThresholdDb?: number;
      lowRatio?: number;
      lowAttackMs?: number;
      lowReleaseMs?: number;
      midThresholdDb?: number;
      midRatio?: number;
      midAttackMs?: number;
      midReleaseMs?: number;
      highThresholdDb?: number;
      highRatio?: number;
      highAttackMs?: number;
      highReleaseMs?: number;
    };
  };
  saturation?: {
    tape?: {
      enabled?: boolean;
      driveDb?: number;
      saturation?: number;
      hysteresis?: number;
      outputGainDb?: number;
      speedIps?: number;
      headBumpDb?: number;
      bias?: number;
      gapLoss?: number;
      /** Jiles-Atherton core oversampling: 1 (default), 2, or 4. */
      oversampleFactor?: number;
    };
    exciter?: {
      enabled?: boolean;
      frequencyHz?: number;
      driveDb?: number;
      amount?: number;
      q?: number;
      evenOddMix?: number;
      /**
       * Antialiasing mode ordinal: 0 = none, 3 = 4x oversampling. The ADAA
       * modes (1, 2) name a member the exciter does not implement and are
       * rejected; an ordinal outside 0-3 is rejected as out of range.
       */
      aliasing?: number;
    };
  };
  spectral?: {
    airBand?: {
      enabled?: boolean;
      amount?: number;
      shelfFrequencyHz?: number;
      dynamicThresholdDb?: number;
      dynamicRangeDb?: number;
    };
  };
  stereo?: {
    imager?: {
      enabled?: boolean;
      width?: number;
      outputGainDb?: number;
      decorrelationAmount?: number;
      preserveEnergy?: boolean;
    };
    monoMaker?: {
      enabled?: boolean;
      amount?: number;
      frequencyHz?: number;
    };
  };
  maximizer?: {
    truePeakLimiter?: {
      enabled?: boolean;
      ceilingDb?: number;
      lookaheadMs?: number;
      releaseMs?: number;
      oversampleFactor?: number;
      applyGainAtInputRate?: boolean;
    };
  };
  loudness?: {
    enabled?: boolean;
    targetLufs?: number;
    ceilingDb?: number;
    truePeakOversample?: number;
    releaseMs?: number;
    applyGainAtInputRate?: boolean;
    /**
     * How deep, in dB, the stage may drive its post-gain true-peak limiter to
     * reach {@link targetLufs}. Default 12. The static normalization gain may
     * exceed the peak headroom toward `ceilingDb` by this much; `0` restores a
     * strict headroom clamp, under which peak-normalized input keeps its input
     * loudness whatever target is asked for. `ceilingDb` holds at every setting.
     */
    maxLimiterGainReductionDb?: number;
  };
  /**
   * Dot-notation spelling of any leaf above, e.g. `'loudness.targetLufs': -20`
   * beside or instead of `loudness: { targetLufs: -20 }`. It is the form the C
   * ABI carries parameters in, so a caller assembling overrides dynamically can
   * emit it directly; the core validates the key and rejects an unknown one.
   * The nested spelling is canonical — prefer it in hand-written code, where it
   * is checked field by field while a dotted key is only checked at run time.
   */
  [flatKey: `${string}.${string}`]: number | boolean | undefined;
}

/** Gain reduction reported by a single dynamics/maximizer chain stage. */
export interface StageGainReduction {
  /** Stage identifier, e.g. `"dynamics.compressor"`. */
  stage: string;
  /**
   * Most recent (typically last-block) gain reduction in dB (negative or
   * zero); for multiband stages it is the most-reduced band.
   */
  gainReductionDb: number;
}

/** Existing EBU R128 measurements captured before or after mastering. */
export interface MasteringLoudnessSummary {
  integratedLufs: number;
  maxMomentaryLufs: number;
  maxShortTermLufs: number;
  truePeakDbtp: number;
  loudnessRange: number;
}

/** Compact explanation of how an offline mastering chain changed a program. */
export interface MasteringReport {
  before: MasteringLoudnessSummary;
  after: MasteringLoudnessSummary;
  appliedGainDb: number;
  /** Most-negative final dynamics/limiter gain reduction, or zero when none ran. */
  maxGainReductionDb: number;
  loudnessTargetLimited: boolean;
  /** 32 logarithmically-spaced after-minus-before spectral energy deltas (dB). */
  bandEnergyDeltaDb: Float32Array;
}

export interface MasteringChainResult {
  /** Latency-compensated offline output; no separate latency field is reported. */
  samples: Float32Array;
  sampleRate: number;
  inputLufs: number;
  outputLufs: number;
  appliedGainDb: number;
  stages: string[];
  /**
   * ITU-R BS.1770-4 true peak of the output (dBTP). Lets callers verify a
   * preset ceiling was met without a second oversampled scan.
   *
   * The oversample factor follows the peak-limiting stage the chain actually
   * applied, so it is not fixed: the loudness stage's true-peak oversample
   * (default 4x) when loudness is enabled, and the maximizer true-peak
   * limiter's own oversample factor when loudness is disabled but that stage
   * ran. The two disagree by roughly 0.02 dB between 4x and 8x, so comparing
   * this against an independently measured peak needs the same factor.
   */
  outputTruePeakDbtp: number;
  /** EBU Tech 3342 Loudness Range of the output (LU). */
  outputLra: number;
  /** True when peak headroom prevented the requested LUFS target. */
  loudnessTargetLimited: boolean;
  /**
   * Samples a stage replaced with a finite in-domain one, keeping the
   * output finite and in range.
   *
   * A non-finite sample supplied by the caller is rejected before any stage
   * runs, so a replacement is always of a value a stage itself produced.
   *
   * Only the true-peak limiters replace anything, so with the maximizer's
   * limiter and the loudness stage both disabled a zero here means no stage
   * was able to replace anything rather than that nothing needed replacing.
   *
   * @example
   * ```ts
   * const result = masterAudio(samples, 44100, 'pop');
   * if (result.nonFiniteSubstitutionCount > 0) {
   *   // part of `result.samples` is unrelated to `samples`
   * }
   * ```
   */
  nonFiniteSubstitutionCount: number;
  /** Per-stage gain reductions for the dynamics/maximizer stages (a subset of `stages`). */
  stageGainReductions: StageGainReduction[];
  report: MasteringReport;
}

export interface MasteringChainStereoResult {
  left: Float32Array;
  right: Float32Array;
  sampleRate: number;
  inputLufs: number;
  outputLufs: number;
  appliedGainDb: number;
  stages: string[];
  /** See {@link MasteringChainResult} for field semantics. */
  outputTruePeakDbtp: number;
  outputLra: number;
  loudnessTargetLimited: boolean;
  /** See {@link MasteringChainResult} for field semantics. Aggregated over both channels. */
  nonFiniteSubstitutionCount: number;
  stageGainReductions: StageGainReduction[];
  report: MasteringReport;
}

export type PanMode =
  | 'balance'
  | 'pan'
  | 'stereoPan'
  | 'stereo-pan'
  | 'dualPan'
  | 'dual-pan'
  | number;

/**
 * Surround pan position for a strip feeding a >2-channel bus. Phase 1 honors
 * `azimuth`/`divergence`/`lfe`; `elevation`/`distance` are reserved. All fields
 * are optional and default to a centered point source.
 */
export interface SurroundPan {
  /** -180..180 deg, 0 = front-center, positive = right. */
  azimuth?: number;
  /** Reserved (no height beds in phase 1). */
  elevation?: number;
  /** 0 = point source, 1 = spread across the front. */
  divergence?: number;
  /** 0..1 scalar send into the LFE plane. */
  lfe?: number;
  /** Reserved (focus/spread), defaults to 1. */
  distance?: number;
}

/**
 * Per-strip options for {@link mixStereo}. Each field is either one value for
 * every strip or an array with one entry per strip; an array shorter than the
 * strip list leaves the remaining strips at their defaults.
 *
 * Every field applies independently of the others. In particular `pan` and
 * `panMode` are separate requests: `panMode` alone selects the mode at the
 * strip's centre position, and `pan` alone moves the position while keeping the
 * strip's current mode, the same way `Mixer.setPan(strip, pan, panMode?)` does.
 */
export interface MixOptions {
  inputTrimDb?: number | number[];
  faderDb?: number | number[];
  pan?: number | number[];
  panMode?: PanMode | PanMode[];
  width?: number | number[];
  muted?: boolean | boolean[];
}

export interface MixMeterSnapshot {
  peakDbL: number;
  peakDbR: number;
  rmsDbL: number;
  rmsDbR: number;
  correlation: number;
  monoCompatWidth: number;
  monoCompatPeak: number;
  monoCompatSideRms: number;
  likelyMonoCompatible: boolean;
  momentaryLufs: number;
  shortTermLufs: number;
  integratedLufs: number;
  gainReductionDb: number;
  /**
   * Left-channel inter-sample (true) peak in dB, from the ITU-R BS.1770-4
   * polyphase reconstruction at 4x. A streaming measurement: the centered
   * reconstruction stencil needs a few future samples a realtime path does not
   * have, so each block's last samples read marginally low (about 0.1 dB across
   * 64..8192-sample blocks on a near-Nyquist tone, always under-reading). Use
   * `meteringTruePeakDb` over the whole signal for an exact dBTP number.
   */
  truePeakDbL: number;
  /** Right-channel inter-sample (true) peak in dB. See {@link truePeakDbL}. */
  truePeakDbR: number;
  /** Maximum inter-sample peak across channels in dB. See {@link truePeakDbL}. */
  maxTruePeakDb: number;
  seq: number;
  /** Number of valid surround planes (5.1/7.1); 0 before the meter sees audio. */
  channelCount: number;
  /** Per-plane peak dB, length channelCount; [0]/[1] mirror peakDbL/peakDbR. */
  peakDb: number[];
  /** Per-plane RMS dB, length channelCount; [0]/[1] mirror rmsDbL/rmsDbR. */
  rmsDb: number[];
  /**
   * Per-plane true-peak dB, length channelCount; [0]/[1] mirror
   * {@link truePeakDbL}/R and carry the same streaming caveat.
   */
  truePeakDb: number[];
}

export interface MixResult {
  left: Float32Array;
  right: Float32Array;
  sampleRate: number;
  meters: MixMeterSnapshot[];
}

/** Mixed stereo master returned by {@link Mixer.processStereo}. */
export interface MixerProcessResult {
  left: Float32Array;
  right: Float32Array;
  sampleRate: number;
}

/**
 * Interpolation curve for scheduled automation events
 * (see {@link Mixer.scheduleInsertAutomation}).
 */
export type AutomationCurve = 'linear' | 'exponential' | 'hold' | 's-curve';

/**
 * Pan law applied by a strip's panner. On mono strips it changes the centre
 * gain; on stereo strips in Balance mode the centre remains unity and it
 * changes only the far-channel taper. Mapped to C enum ints
 * `0=const3dB`, `1=const4.5dB`, `2=const6dB`, `3=linear0dB`.
 */
export type PanLaw = 'const3dB' | 'const4.5dB' | 'const6dB' | 'linear0dB';

/**
 * Accepted pan-law name aliases. Names are normalized case-insensitively with
 * underscores treated as hyphens at runtime; the canonical four spellings in
 * {@link PanLaw} remain the preferred TypeScript values.
 */
export type PanLawName =
  | PanLaw
  | 'const-3db'
  | '-3db'
  | 'const-4.5db'
  | '-4.5db'
  | 'const-6db'
  | '-6db'
  | 'linear-0db'
  | 'linear'
  | '0db';

/** Pan-law name or raw C ABI ordinal. */
export type PanLawInput = PanLawName | number;

/**
 * Meter tap point on a strip. Mapped to the C enum ints
 * `0=preFader`, `1=postFader`.
 */
export type MeterTap = 'preFader' | 'postFader';

/** Pre/post-fader send timing (see {@link Mixer.addSend}). */
export type SendTiming = 'preFader' | 'postFader';

/**
 * A reference to a strip in the {@link Mixer}: either a 0-based strip index or
 * the strip's string id.
 */
export type StripRef = number | string;

/** Single goniometer sample from {@link Mixer.readGoniometerLatest}. */
export interface GoniometerPoint {
  left: number;
  right: number;
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
  /** Region time start in input samples (clamped to `[0, length]`). Default 0. */
  startSample?: number;
  /** Region time end (exclusive) in input samples. Default = signal length. */
  endSample?: number;
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
export interface SpectralEditOptions {
  /** FFT size; a power of two in `[2, 262144]`. Default 2048. */
  nFft?: number;
  /** Hop length; must satisfy `0 < hop <= nFft / 2`. Default 512. */
  hopLength?: number;
  /** Analysis/synthesis window. Default `'hann'`. */
  window?: 'hann' | 'hamming' | 'blackman' | 'rectangular';
  /** Neighbour frames each side used by `'heal'`. Default 2. */
  healRadiusFrames?: number;
}
