import { resolvePositiveIntegerOption } from './_feature_options.js';
import { resolveFftOptions } from './_fft_options.js';
import { addon } from './native.js';
import type {
  HpssResult,
  NoteExtractorOptions,
  NoteMoveOptions,
  NoteObject,
  NoteObjectInput,
  NoteSetEntry,
  NoteStretchOptions,
  PercussiveEvent,
  PercussiveEventInput,
  PitchCorrectOptions,
  PitchDecompositionResult,
  SpectralEditOptions,
  SpectralRegionOp,
  VoicedFlags,
} from './types.js';
import {
  assertFiniteScalar,
  assertHpssKernels,
  assertInt32,
  assertInt64,
  assertIntegerType,
  assertSampleRate,
} from './validation.js';

// The addon reads the companion voicing array as an Int32Array and silently
// ignores any other type, so normalize here rather than at the N-API boundary.
// A flag is a decision, not a magnitude: collapse to 1/0 on truthiness, which
// is the same reduction the WASM facade applies, so both surfaces agree on
// every accepted input type.
function toVoicedInt32(voiced: VoicedFlags): Int32Array {
  const out = new Int32Array(voiced.length);
  for (let index = 0; index < voiced.length; index += 1) {
    out[index] = voiced[index] ? 1 : 0;
  }
  return out;
}

/**
 * Check the separation an extracted or rendered event set is measured against
 * before the addon narrows it. Every field defaults at 0 here, so a value that
 * wrapped to 0 would select the default and report success.
 */
function assertPercussiveSeparation(fnName: string, options: PercussiveSeparationOptions): void {
  const fields = ['nFft', 'hopLength', 'hpssKernelHarmonic', 'hpssKernelPercussive'] as const;
  for (const field of fields) {
    const value = options[field];
    if (value !== undefined) {
      assertInt32(fnName, value, field);
    }
  }
}

/**
 * Check the pending time offsets of a note or event set before the addon
 * narrows them. Zero is this field's identity rather than a default, so a
 * truncated sub-sample shift renders the set unmoved and reports success.
 */
function assertEditTimeOffsets(
  fnName: string,
  entries: ReadonlyArray<{ edit?: { timeOffsetSamples?: number } }>,
  arrayName: string,
): void {
  entries.forEach((entry, index) => {
    const offset = entry?.edit?.timeOffsetSamples;
    if (offset !== undefined) {
      assertInt64(fnName, offset, `${arrayName}[${index}].edit.timeOffsetSamples`);
    }
  });
}

function resolveHardMask(fnName: string, hardMask: unknown): boolean {
  if (hardMask === undefined) {
    return false;
  }
  if (typeof hardMask !== 'boolean') {
    throw new TypeError(`${fnName}: hardMask must be a boolean`);
  }
  return hardMask;
}

/** Common audio input fields for stateless effect requests. */
export interface EffectSamplesRequest {
  samples: Float32Array;
  sampleRate?: number;
}

export interface HpssRequest extends EffectSamplesRequest {
  /**
   * Horizontal median filter size, in STFT frames: a positive odd integer at
   * most 524287. Default 31. The ceiling is 524288 and an even kernel is
   * refused, so 524287 is the largest legal value.
   */
  kernelHarmonic?: number;
  /** Vertical median filter size, in STFT bins, under the same rule. Default 31. */
  kernelPercussive?: number;
  nFft?: number;
  hopLength?: number;
  hardMask?: boolean;
}

export interface TimeStretchRequest extends EffectSamplesRequest {
  rate: number;
  nFft?: number;
  hopLength?: number;
}

export interface SpectralEditRequest extends EffectSamplesRequest, SpectralEditOptions {
  /**
   * Sample rate in Hz. Required: region frequency boundaries are mapped to STFT
   * bins using this rate, so a wrong/omitted value silently corrupts the edit
   * (unlike the other effects, this has no safe default).
   */
  sampleRate: number;
  ops?: SpectralRegionOp[];
}

export interface PitchShiftRequest extends EffectSamplesRequest {
  semitones: number;
  nFft?: number;
  hopLength?: number;
}

export interface PitchCorrectToMidiRequest extends EffectSamplesRequest {
  currentMidi?: number;
  targetMidi?: number;
}

export interface PitchCorrectToMidiTimevaryingRequest extends EffectSamplesRequest {
  f0Hz: Float32Array;
  targetMidi: number;
  hopLength?: number;
  voiced?: VoicedFlags;
  voicedProb?: Float32Array;
}

export interface PitchCorrectTimevaryingRequest extends EffectSamplesRequest, PitchCorrectOptions {
  f0Hz: Float32Array;
  hopLength?: number;
}

export interface NoteStretchRequest extends EffectSamplesRequest, NoteStretchOptions {}
export interface NoteMoveRequest extends EffectSamplesRequest, NoteMoveOptions {}

/**
 * Audio and the caller's own F0 track, as every note-set entry point takes them.
 * The audio and the track must be the ones a set was extracted from, or the set
 * is measured against something else.
 */
export interface NoteTrackRequest extends EffectSamplesRequest, NoteExtractorOptions {
  /**
   * Sample rate in Hz. Required: `minNoteMs` and the per-frame RMS windows are
   * converted to samples with this rate, so a wrong/omitted value silently
   * segments differently.
   */
  sampleRate: number;
  /**
   * Per-frame F0 in Hz. A frame carrying no pitch is spelled as zero, a
   * negative value or a non-finite one, and all three read the same: that
   * frame contributes no measurement. A {@link pitchPyin} track can be passed
   * straight through — its `fillNa` is a choice about the contour you want,
   * not a requirement of this call.
   */
  f0Hz: Float32Array;
  /** F0 frames per second. */
  frameRate: number;
  /** Per-frame voiced flags (truthy = voiced). Takes precedence over `voicedProb`. */
  voiced?: VoicedFlags;
  /** Per-frame voicing probability in `[0, 1]`; read only when `voiced` is omitted. */
  voicedProb?: Float32Array;
}

export type ExtractNotesRequest = NoteTrackRequest;

export interface SplitNoteRequest extends NoteTrackRequest {
  /** The current note set, identified by its frame bounds. */
  notes: readonly NoteSetEntry[];
  /** Index of the note to split. */
  index: number;
  /** Track frame to cut at, strictly inside that note's own span. */
  frame: number;
}

export interface MergeNotesRequest extends NoteTrackRequest {
  /** The current note set, identified by its frame bounds. */
  notes: readonly NoteSetEntry[];
  /** Index of the first note to join; must be less than `last`. */
  first: number;
  /** Index of the last note to join, inclusive. */
  last: number;
}

export interface DecomposeNotePitchRequest {
  /**
   * The note's slice of the F0 track (`f0Hz.subarray(frameStart, frameEnd)`);
   * finite and non-negative, zero meaning unvoiced.
   */
  f0Hz: Float32Array;
  /** F0 frames per second. */
  frameRate: number;
  /** The note's `medianHz`; non-negative, 0 spelling a note with no pitch. */
  medianHz: number;
  /**
   * Boundary between the drift and the vibrato, in Hz. Default 3. Hand the same
   * value to {@link renderNotes}.
   */
  vibratoCutoffHz?: number;
}

export interface RenderNotesRequest extends EffectSamplesRequest {
  /**
   * Sample rate in Hz. Required: `fadeMs` is converted to samples with this
   * rate, so a wrong/omitted value changes the cross-fade length.
   */
  sampleRate: number;
  /** The notes to render, with their edits. Source spans must not overlap. */
  notes: readonly NoteObjectInput[];
  /**
   * Equal-power cross-fade at each edited note's edges. Default 5 ms; a hard cut
   * is deliberately not selectable, because the seam it leaves is a click.
   */
  fadeMs?: number;
  /**
   * The F0 track the notes were extracted from. Required by a
   * `vibratoDepthChange` / `driftChange` edit, which acts on the note's own
   * pitch curve; every other edit ignores it.
   */
  f0Hz?: Float32Array;
  /** F0 frames per second; required when `f0Hz` is given. */
  frameRate?: number;
  /**
   * Boundary between the drift and the vibrato those two edits act on, in Hz.
   * Default 3. Pass whatever {@link decomposeNotePitch} was called with: a host
   * that draws the vibrato at one cutoff and edits it at another edits a curve
   * it never showed anyone.
   */
  vibratoCutoffHz?: number;
}

/**
 * The separation an event's signal is lifted out with, mixed into both
 * percussive-event requests.
 *
 * Extraction measures events against it and rendering has to repeat it — a
 * different separation lifts a different signal out of the span than the one the
 * events describe — so it is one set of fields both sides take rather than a
 * framing each of them restates.
 *
 * The framing must overlap-add, because the separation inverts an STFT: `nFft`
 * even and at least 2, `hopLength` no more than half of it.
 */
export interface PercussiveSeparationOptions {
  /**
   * FFT size and hop the separation and the onset detector share. They cannot be
   * set apart: an event measured on one framing and lifted out on another is not
   * the same signal. Default 2048 and 512.
   */
  nFft?: number;
  hopLength?: number;
  /**
   * Median filter lengths the separation runs, along time and along frequency. A
   * longer harmonic kernel calls more of a sustained sound harmonic. Default 31.
   */
  hpssKernelHarmonic?: number;
  hpssKernelPercussive?: number;
}

export interface ExtractPercussiveEventsRequest extends PercussiveSeparationOptions {
  /**
   * Source audio. A plain number array is copied to a `Float32Array` first,
   * which is what the native side reads.
   */
  samples: Float32Array | readonly number[];
  /**
   * Sample rate in Hz. Required: `maxEventMs` is converted to samples with this
   * rate, so a wrong or omitted value caps the spans differently.
   */
  sampleRate: number;
  /**
   * Minimum frames between consecutive onsets. Default 1, and a whole number:
   * 0 is how the default is spelled, so a fractional wait is refused rather
   * than truncated onto it.
   */
  onsetWait?: number;
  /**
   * Offset added to the detector's adaptive threshold; raising it finds fewer,
   * stronger hits. Default 0.06, and 0 selects that default rather than a
   * detector with no offset at all.
   */
  onsetDelta?: number;
  /**
   * Caps a span that no onset follows. It binds at the end of a phrase and at
   * the end of the track; anywhere else the next onset closes the span first.
   * Default 500 ms.
   */
  maxEventMs?: number;
  /**
   * Drops an event whose `percussiveRatio` falls below this; must be in
   * `[0, 1]`. 0 is both the default and the meaningful "keep everything", so
   * nothing is lost to the rule that an omitted option takes the default.
   *
   * Raising it is useful on material that is mostly drums and wrong on a dense
   * mix, where it also drops real hits sitting over a loud sustain.
   */
  minPercussiveRatio?: number;
}

export interface RenderPercussiveEventsRequest extends PercussiveSeparationOptions {
  /**
   * Source audio. A plain number array is copied to a `Float32Array` first,
   * which is what the native side reads.
   */
  samples: Float32Array | readonly number[];
  /**
   * Sample rate in Hz. Required: `fadeMs` is converted to samples with this
   * rate, so a wrong or omitted value changes the fade length.
   */
  sampleRate: number;
  /**
   * The events to render, with their edits. Source spans must be non-empty,
   * inside `samples`, and must not overlap.
   */
  events: readonly PercussiveEventInput[];
  /**
   * Fade-out at the tail of each lifted span. Default 5 ms; 0 selects that
   * default rather than a hard cut, which would leave a step in the output.
   *
   * There is deliberately no matching fade-in: a span opens on an onset, where
   * the percussive component is near-silent the sample before, so cutting square
   * there costs nothing and keeps a muted hit's attack from surviving inside a
   * fade.
   */
  fadeMs?: number;
}

/**
 * The addon reads audio as a `Float32Array` and rejects anything else, so a
 * plain number array is converted here rather than at the N-API boundary.
 */
function toSamples(samples: Float32Array | readonly number[]): Float32Array {
  return samples instanceof Float32Array ? samples : Float32Array.from(samples);
}

function assertPitchTrackLengths(
  f0Hz: Float32Array,
  voiced?: VoicedFlags,
  voicedProb?: Float32Array,
): void {
  if (voiced !== undefined && voiced.length !== f0Hz.length) {
    throw new RangeError('voiced must have the same length as f0Hz');
  }
  if (voicedProb !== undefined && voicedProb.length !== f0Hz.length) {
    throw new RangeError('voicedProb must have the same length as f0Hz');
  }
}

/**
 * A note set is identified by its frame bounds alone, and a missing bound reads
 * as an empty span the C ABI rejects without naming what was wrong. Name it.
 *
 * The bounds are whole frames. The addon reads them through a narrowing that
 * truncates, so a fractional bound arrives as the frame below it -- one the
 * caller could have named, which leaves nothing downstream able to tell the
 * two apart.
 */
function assertNoteSetEntries(fnName: string, notes: readonly NoteSetEntry[]): void {
  for (let index = 0; index < notes.length; index += 1) {
    const note = notes[index];
    if (!Number.isFinite(note?.frameStart) || !Number.isFinite(note?.frameEnd)) {
      throw new TypeError(`${fnName}: notes[${index}] must carry a finite frameStart and frameEnd`);
    }
    assertIntegerType(fnName, note.frameStart, `notes[${index}].frameStart`);
    assertIntegerType(fnName, note.frameEnd, `notes[${index}].frameEnd`);
  }
}

/** Shared entry check for the three entry points that re-measure a whole track. */
function assertNoteTrackRequest(fnName: string, request: NoteTrackRequest): void {
  assertSampleRate(fnName, request.sampleRate);
  assertFiniteScalar(fnName, request.frameRate, 'frameRate');
  if (request.voiced === undefined && request.voicedProb === undefined) {
    throw new TypeError(`${fnName}: one of voiced or voicedProb is required`);
  }
  assertPitchTrackLengths(request.f0Hz, request.voiced, request.voicedProb);
}

// -- Effects --

export function hpss(request: HpssRequest): HpssResult;
export function hpss(
  samples: Float32Array,
  sampleRate?: number,
  kernelHarmonic?: number,
  kernelPercussive?: number,
  nFft?: number,
  hopLength?: number,
  hardMask?: boolean,
): HpssResult;
export function hpss(
  samples: Float32Array | HpssRequest,
  sampleRate = 22050,
  kernelHarmonic = 31,
  kernelPercussive = 31,
  nFft?: number,
  hopLength?: number,
  hardMask?: boolean,
): HpssResult {
  const request =
    samples instanceof Float32Array
      ? { samples, sampleRate, kernelHarmonic, kernelPercussive, nFft, hopLength, hardMask }
      : samples;
  const fftOptions = resolveFftOptions('hpss', request.nFft, request.hopLength);
  const resolvedHardMask = resolveHardMask('hpss', request.hardMask);
  const resolvedKernelHarmonic = request.kernelHarmonic ?? 31;
  const resolvedKernelPercussive = request.kernelPercussive ?? 31;
  assertHpssKernels('hpss', resolvedKernelHarmonic, resolvedKernelPercussive);
  return addon.hpss(
    request.samples,
    request.sampleRate ?? 22050,
    resolvedKernelHarmonic,
    resolvedKernelPercussive,
    fftOptions.nFft,
    fftOptions.hopLength,
    resolvedHardMask,
  );
}

export function harmonic(request: EffectSamplesRequest): Float32Array;
export function harmonic(samples: Float32Array, sampleRate?: number): Float32Array;
export function harmonic(
  samples: Float32Array | EffectSamplesRequest,
  sampleRate = 22050,
): Float32Array {
  const request = samples instanceof Float32Array ? { samples, sampleRate } : samples;
  return addon.harmonic(request.samples, request.sampleRate ?? 22050);
}

export function percussive(request: EffectSamplesRequest): Float32Array;
export function percussive(samples: Float32Array, sampleRate?: number): Float32Array;
export function percussive(
  samples: Float32Array | EffectSamplesRequest,
  sampleRate = 22050,
): Float32Array {
  const request = samples instanceof Float32Array ? { samples, sampleRate } : samples;
  return addon.percussive(request.samples, request.sampleRate ?? 22050);
}

/**
 * Time-stretch audio without changing pitch.
 *
 * @param samples - Audio samples (mono, float32)
 * @param sampleRate - Sample rate in Hz
 * @param rate - Time stretch rate (0.5 = double duration, 2.0 = half duration)
 * @param nFft - FFT size: an even integer >= 2 (default 2048)
 * @param hopLength - Hop in samples, in `(0, nFft / 2]` (default 512), so
 *   frames overlap by at least half a window
 * @returns Time-stretched audio
 */
export function timeStretch(request: TimeStretchRequest): Float32Array;
export function timeStretch(
  samples: Float32Array,
  sampleRate: number,
  rate: number,
  nFft?: number,
  hopLength?: number,
): Float32Array;
export function timeStretch(
  samples: Float32Array | TimeStretchRequest,
  sampleRate?: number,
  rate?: number,
  nFft?: number,
  hopLength?: number,
): Float32Array {
  const request =
    samples instanceof Float32Array ? { samples, sampleRate, rate, nFft, hopLength } : samples;
  assertFiniteScalar('timeStretch', request.rate as number, 'rate');
  const fftOptions = resolveFftOptions('timeStretch', request.nFft, request.hopLength);
  return addon.timeStretch(
    request.samples,
    request.sampleRate ?? 22050,
    request.rate,
    fftOptions.nFft,
    fftOptions.hopLength,
  );
}

/**
 * Region-based spectral editing: STFT -> per-op time x frequency bin/frame
 * masking -> iSTFT. A stateless mono transform whose output has the same length
 * and sample rate as the input.
 *
 * Each {@link SpectralRegionOp} in `ops` is a time x frequency rectangle applied
 * in array order (gain / attenuate / mute / heal). Passing an empty `ops` array
 * is the identity transform (the input is returned). Wraps the C
 * `sonare_spectral_edit`.
 *
 * @param samples - Mono input audio.
 * @param sampleRate - Sample rate in Hz (required; region frequency boundaries
 *   are mapped to STFT bins using this rate, so a wrong/omitted value silently
 *   corrupts the edit).
 * @param ops - Region ops applied in order.
 * @param options - Optional STFT + heal configuration.
 * @returns The edited audio (same length as `samples`).
 */
export function spectralEdit(request: SpectralEditRequest): Float32Array;
export function spectralEdit(
  samples: Float32Array,
  sampleRate: number,
  ops?: SpectralRegionOp[],
  options?: SpectralEditOptions,
): Float32Array;
export function spectralEdit(
  samples: Float32Array | SpectralEditRequest,
  sampleRate?: number,
  ops: SpectralRegionOp[] = [],
  options: SpectralEditOptions = {},
): Float32Array {
  const request =
    samples instanceof Float32Array ? { samples, sampleRate, ops, ...options } : samples;
  const {
    samples: input,
    sampleRate: requestSampleRate,
    ops: requestOps = [],
    ...requestOptions
  } = request;
  assertSampleRate('spectralEdit', requestSampleRate as number);
  // Each of the three is its own "0 => the documented default" on the C side,
  // and the addon's narrowing truncates onto that 0.
  for (const field of ['nFft', 'hopLength', 'healRadiusFrames'] as const) {
    const value = requestOptions[field];
    if (value !== undefined) {
      assertInt32('spectralEdit', value, field);
    }
  }
  return addon.spectralEdit(input, requestSampleRate as number, requestOps, requestOptions);
}

/**
 * Pitch-shift audio without changing duration.
 *
 * @param samples - Audio samples (mono, float32)
 * @param sampleRate - Sample rate in Hz
 * @param semitones - Pitch shift in semitones (+12 = one octave up, -12 = one octave down)
 * @param nFft - FFT size: an even integer >= 2 (default 2048)
 * @param hopLength - Hop in samples, in `(0, nFft / 2]` (default 512), so
 *   frames overlap by at least half a window
 * @returns Pitch-shifted audio
 */
export function pitchShift(request: PitchShiftRequest): Float32Array;
export function pitchShift(
  samples: Float32Array,
  sampleRate: number,
  semitones: number,
  nFft?: number,
  hopLength?: number,
): Float32Array;
export function pitchShift(
  samples: Float32Array | PitchShiftRequest,
  sampleRate?: number,
  semitones?: number,
  nFft?: number,
  hopLength?: number,
): Float32Array {
  const request =
    samples instanceof Float32Array ? { samples, sampleRate, semitones, nFft, hopLength } : samples;
  assertFiniteScalar('pitchShift', request.semitones as number, 'semitones');
  const fftOptions = resolveFftOptions('pitchShift', request.nFft, request.hopLength);
  return addon.pitchShift(
    request.samples,
    request.sampleRate ?? 22050,
    request.semitones,
    fftOptions.nFft,
    fftOptions.hopLength,
  );
}

/**
 * Apply one constant, immediate transpose from `currentMidi` to `targetMidi`.
 *
 * The result has exactly the input length. Both endpoints must be finite, and
 * the whole interval is applied however large it is: they are validated to
 * [0, 127], so a two-octave move such as C3 -> C5 transposes by the full 24
 * semitones. Use
 * {@link pitchCorrectToMidiTimevarying} for a caller-supplied pitch contour and
 * retune glide.
 */
export function pitchCorrectToMidi(request: PitchCorrectToMidiRequest): Float32Array;
export function pitchCorrectToMidi(
  samples: Float32Array,
  sampleRate?: number,
  currentMidi?: number,
  targetMidi?: number,
): Float32Array;
export function pitchCorrectToMidi(
  samples: Float32Array | PitchCorrectToMidiRequest,
  sampleRate = 22050,
  currentMidi = 69.0,
  targetMidi = 69.0,
): Float32Array {
  const request =
    samples instanceof Float32Array ? { samples, sampleRate, currentMidi, targetMidi } : samples;
  return addon.pitchCorrectToMidi(
    request.samples,
    request.sampleRate ?? 22050,
    request.currentMidi ?? 69.0,
    request.targetMidi ?? 69.0,
  );
}

/**
 * Contour-following ("time-varying") pitch correction toward a MIDI target.
 *
 * Unlike {@link pitchCorrectToMidi} (a single constant transpose), this follows
 * the caller-supplied per-frame `f0Hz` contour and retunes every voiced frame
 * toward `targetMidi`, so vibrato/drift in the source is tracked rather than
 * flattened. `voiced` (truthy = voiced) and `voicedProb` ([0,1]) are optional;
 * omitting them treats every frame as voiced. An `f0Hz` NaN is accepted only
 * when the corresponding `voiced` entry is falsy, matching pYIN output. The
 * `voicedFlag` / `voicedProb` arrays of a {@link PitchResult} can be passed
 * through directly.
 */
export function pitchCorrectToMidiTimevarying(
  request: PitchCorrectToMidiTimevaryingRequest,
): Float32Array;
export function pitchCorrectToMidiTimevarying(
  samples: Float32Array,
  f0Hz: Float32Array,
  targetMidi: number,
  sampleRate?: number,
  hopLength?: number,
  voiced?: VoicedFlags,
  voicedProb?: Float32Array,
): Float32Array;
export function pitchCorrectToMidiTimevarying(
  samples: Float32Array | PitchCorrectToMidiTimevaryingRequest,
  f0Hz?: Float32Array,
  targetMidi?: number,
  sampleRate = 22050,
  hopLength = 512,
  voiced?: VoicedFlags,
  voicedProb?: Float32Array,
): Float32Array {
  const request =
    samples instanceof Float32Array
      ? {
          samples,
          f0Hz: f0Hz as Float32Array,
          targetMidi: targetMidi as number,
          sampleRate,
          hopLength,
          voiced,
          voicedProb,
        }
      : samples;
  assertPitchTrackLengths(request.f0Hz, request.voiced, request.voicedProb);
  // Positivity only: the corrector requires a positive hop to place the contour
  // and carries no further domain.
  const resolvedHopLength = resolvePositiveIntegerOption(
    'pitchCorrectToMidiTimevarying',
    'hopLength',
    request.hopLength,
    512,
  );
  return addon.pitchCorrectToMidiTimevarying(
    request.samples,
    request.sampleRate ?? 22050,
    request.f0Hz,
    request.targetMidi,
    resolvedHopLength,
    request.voiced ? toVoicedInt32(request.voiced) : undefined,
    request.voicedProb,
  );
}

/**
 * Contour-following pitch correction toward a fixed MIDI note OR a musical
 * scale, with tunable retune strength and vibrato preservation.
 *
 * Generalises {@link pitchCorrectToMidiTimevarying}: the same caller-supplied
 * per-frame `f0Hz` contour drives correction, but {@link options.mode} selects
 * between a fixed-MIDI target (`'midi'`, default) and scale quantisation
 * (`'scale'`), and the retune knobs (`retuneAmount`, `maxCorrectionSemitones`,
 * `retuneSpeedMs`, `vibratoThresholdCents`) shape natural-vs-robotic correction.
 * An `f0Hz` NaN is accepted only for a frame marked unvoiced.
 */
export function pitchCorrectTimevarying(request: PitchCorrectTimevaryingRequest): Float32Array;
export function pitchCorrectTimevarying(
  samples: Float32Array,
  f0Hz: Float32Array,
  sampleRate?: number,
  hopLength?: number,
  options?: PitchCorrectOptions,
): Float32Array;
export function pitchCorrectTimevarying(
  samples: Float32Array | PitchCorrectTimevaryingRequest,
  f0Hz?: Float32Array,
  sampleRate = 22050,
  hopLength = 512,
  options: PitchCorrectOptions = {},
): Float32Array {
  const request =
    samples instanceof Float32Array
      ? { samples, f0Hz: f0Hz as Float32Array, sampleRate, hopLength, ...options }
      : samples;
  const {
    samples: input,
    sampleRate: requestSampleRate,
    f0Hz: requestF0Hz,
    hopLength: requestHopLength,
    ...requestOptions
  } = request;
  assertPitchTrackLengths(requestF0Hz, requestOptions.voiced, requestOptions.voicedProb);
  // Positivity only, as pitchCorrectToMidiTimevarying: the same corrector.
  const resolvedHopLength = resolvePositiveIntegerOption(
    'pitchCorrectTimevarying',
    'hopLength',
    requestHopLength,
    512,
  );
  return addon.pitchCorrectTimevarying(
    input,
    requestSampleRate ?? 22050,
    requestF0Hz,
    resolvedHopLength,
    {
      ...requestOptions,
      voiced: requestOptions.voiced ? toVoicedInt32(requestOptions.voiced) : undefined,
    },
  );
}

export function noteStretch(request: NoteStretchRequest): Float32Array;
export function noteStretch(
  samples: Float32Array,
  sampleRate?: number,
  options?: NoteStretchOptions,
): Float32Array;
export function noteStretch(
  samples: Float32Array | NoteStretchRequest,
  sampleRate = 22050,
  options: NoteStretchOptions = {},
): Float32Array {
  const request = samples instanceof Float32Array ? { samples, sampleRate, ...options } : samples;
  return addon.noteStretch(
    request.samples,
    request.sampleRate ?? 22050,
    request.onsetSample ?? 0,
    request.offsetSample ?? request.samples.length,
    request.stretchRatio ?? 1.0,
  );
}

/** Move a note region to a new onset sample without changing its duration. */
export function noteMove(request: NoteMoveRequest): Float32Array;
export function noteMove(
  samples: Float32Array,
  sampleRate?: number,
  options?: NoteMoveOptions,
): Float32Array;
export function noteMove(
  samples: Float32Array | NoteMoveRequest,
  sampleRate = 22050,
  options: NoteMoveOptions = {},
): Float32Array {
  const request = samples instanceof Float32Array ? { samples, sampleRate, ...options } : samples;
  return addon.noteMove(
    request.samples,
    request.sampleRate ?? 22050,
    request.onsetSample ?? 0,
    request.offsetSample ?? request.samples.length,
    request.targetOnsetSample ?? 0,
  );
}

/**
 * Segment audio and a caller-supplied F0 track into editable note objects.
 *
 * Each note carries its sample span, its frame span into the caller's own
 * `f0Hz`, its median pitch, two measured quality figures, its own amplitude
 * (RMS-per-frame) curve, and the identity {@link NoteEdit}. Change the edits and
 * hand the notes to {@link renderNotes} to hear them; nothing is applied here.
 *
 * The per-note F0 curve is deliberately not returned — it is already the
 * caller's, as `f0Hz.subarray(note.frameStart, note.frameEnd)`.
 *
 * Voicing comes from `voiced` when given, and otherwise from `voicedProb`
 * thresholded at `voicedThreshold`; at least one of the two is required. Prefer
 * `voiced`: `voicedProb` rises with F0 for a fixed frame length, so a fixed
 * threshold silently drops low-register notes.
 *
 * @param request - Audio, its sample rate, the F0 track and its frame rate,
 *   plus the optional segmentation tuning.
 * @returns One {@link NoteObject} per segmented note, in time order. A pitch
 *   track that segments into nothing returns an empty array.
 * @throws {TypeError} Neither `voiced` nor `voicedProb` was given.
 * @throws {RangeError} `frameRate` is not a finite number, `sampleRate` is out
 *   of the supported range, or `voiced` / `voicedProb` do not have the same
 *   length as `f0Hz`.
 *
 * @example
 * ```ts
 * // pitchPyin's default leaves unvoiced frames NaN, which reads here as a
 * // frame carrying no pitch, so fillNa is a choice rather than a requirement.
 * const pitch = pitchPyin({ samples, sampleRate });
 * const notes = extractNotes({
 *   samples,
 *   sampleRate,
 *   f0Hz: pitch.f0,
 *   voiced: pitch.voicedFlag,
 *   frameRate: sampleRate / 512,
 *   minNoteMs: 40,
 * });
 *
 * // Lift the second note by a semitone and drop it 3 dB.
 * notes[1].edit.pitchShiftSemitones = 1;
 * notes[1].edit.gainDb = -3;
 * const edited = renderNotes({ samples, sampleRate, notes });
 * ```
 */
export function extractNotes(request: ExtractNotesRequest): NoteObject[] {
  const { samples, sampleRate, f0Hz, frameRate, voiced, ...options } = request;
  assertNoteTrackRequest('extractNotes', request);
  return addon.extractNotes(samples, sampleRate, f0Hz, frameRate, {
    ...options,
    voiced: voiced ? toVoicedInt32(voiced) : undefined,
  });
}

/**
 * Render edited note objects over their source audio.
 *
 * Each note's `onsetSample`, `offsetSample` and `edit` are read, plus its frame
 * bounds and `medianHz` when a curve edit needs them, so the notes
 * {@link extractNotes} returned can be handed straight back. A note whose edit
 * is the identity is not resynthesized, so a set whose edits are all identity
 * reproduces the input bit for bit. The result has the input's length.
 *
 * Per note the order is: pitch curve, time stretch, pitch shift, formant warp,
 * amplitude envelope, then gain.
 *
 * Overlap is checked on the source spans only. Where `timeOffsetSamples` lands a
 * note is not, and a note lengthened past its own span writes into its
 * neighbours' samples, so two moved or stretched notes may overwrite each other.
 *
 * @param request - Audio, its sample rate, the notes to render, the optional
 *   edge cross-fade, and the F0 track a curve edit needs.
 * @returns The rendered audio, the same length as `samples`.
 * @throws {TypeError} `notes` is not an array, or `f0Hz` is not a `Float32Array`.
 * @throws {RangeError} `sampleRate` is out of the supported range, or `f0Hz` was
 *   given without a finite `frameRate`.
 * @throws {SonareError} A note is missing `onsetSample` or `offsetSample`, or
 *   carries `vibratoDepthChange` or `driftChange` without an `f0Hz` covering its
 *   frame span, or an `amplitudeEnvelope` value is not a finite non-negative
 *   gain.
 *
 * @example
 * ```ts
 * const notes = extractNotes({ samples, sampleRate, f0Hz, voiced, frameRate });
 *
 * // Silence the third note and leave the rest untouched.
 * const muted = notes.map((note, index) =>
 *   index === 2 ? { ...note, edit: { ...note.edit, muted: true } } : note,
 * );
 * const output = renderNotes({ samples, sampleRate, notes: muted, fadeMs: 10 });
 *
 * // Flatten the first note's vibrato and fade it out, which needs the track.
 * notes[0].edit.vibratoDepthChange = -1;
 * notes[0].edit.amplitudeEnvelope = Float32Array.from([1, 0]);
 * const tamed = renderNotes({ samples, sampleRate, notes, f0Hz, frameRate });
 * ```
 */
export function renderNotes(request: RenderNotesRequest): Float32Array {
  const { samples, sampleRate, notes, ...options } = request;
  assertSampleRate('renderNotes', sampleRate);
  if (!Array.isArray(notes)) {
    throw new TypeError('renderNotes: notes must be an array');
  }
  // The addon reads the track as a Float32Array and ignores any other type, so
  // a plain array would silently render as though no track had been given.
  if (options.f0Hz !== undefined && !(options.f0Hz instanceof Float32Array)) {
    throw new TypeError('renderNotes: f0Hz must be a Float32Array');
  }
  // Only a track edit reads the frame rate, so it is required exactly when f0Hz is.
  if (options.f0Hz !== undefined) {
    assertFiniteScalar('renderNotes', options.frameRate as number, 'frameRate');
  }
  assertEditTimeOffsets('renderNotes', notes, 'notes');
  return addon.renderNotes(samples, sampleRate, notes, options);
}

/**
 * Split one note's pitch curve into a centre, a slow drift and a vibrato.
 *
 * `driftCents[i] + vibratoCents[i]` is the note's own pitch at frame `i`, in
 * cents above `centreHz`, to within float rounding, so the three parts
 * reconstruct the curve. The drift filter is zero phase, so neither curve is
 * shifted in time against the audio.
 *
 * Frames whose F0 is unusable carry no measurement, so the curve is held at the
 * nearest usable neighbour across them. Both curves therefore have an entry
 * everywhere; a host marking the held ones reads them off `f0Hz`, which is
 * exact.
 *
 * A note with no usable pitch comes back as a zero centre and two empty curves
 * rather than as an error.
 *
 * @param request - The note's slice of the F0 track, its frame rate, its median
 *   pitch, and the optional cutoff between the two curves.
 * @returns The centre and the two curves, one entry per frame of `f0Hz`.
 * @throws {RangeError} `frameRate` is not a finite number.
 * @throws {SonareError} `f0Hz` is empty or carries a negative or non-finite
 *   value, or `medianHz` / `vibratoCutoffHz` is negative or non-finite.
 *
 * @example
 * ```ts
 * const note = extractNotes({ samples, sampleRate, f0Hz, voiced, frameRate })[0];
 * const curve = decomposeNotePitch({
 *   f0Hz: f0Hz.subarray(note.frameStart, note.frameEnd),
 *   frameRate,
 *   medianHz: note.medianHz,
 * });
 * // Draw curve.vibratoCents, then edit it at the same cutoff.
 * ```
 */
export function decomposeNotePitch(request: DecomposeNotePitchRequest): PitchDecompositionResult {
  const { f0Hz, frameRate, medianHz, vibratoCutoffHz = 0 } = request;
  assertFiniteScalar('decomposeNotePitch', frameRate, 'frameRate');
  return addon.decomposeNotePitch(f0Hz, frameRate, medianHz, vibratoCutoffHz);
}

/**
 * Split one note of a set in two at a track frame.
 *
 * Both halves are re-derived from the audio and the track the way
 * {@link extractNotes} derives its own, rather than by patching the fields of
 * the note they replace. Both inherit the source note's edit, and its amplitude
 * envelope is cut at the same proportion so each half keeps its own part of it;
 * a one-entry envelope is a constant over the span, so both halves get that same
 * entry.
 *
 * Every note in the set — not just the two halves — has its spans, curves,
 * medians and stability re-derived, because a note carries no curves for this
 * call to copy through. A note is therefore identified by its frame bounds, and
 * the audio and track must be the ones the set was extracted from.
 *
 * @param request - Audio, its sample rate, the F0 track, the current note set,
 *   the note to split and the frame to cut at.
 * @returns The whole new note set, in time order.
 * @throws {TypeError} Neither `voiced` nor `voicedProb` was given, `notes` is
 *   not an array, or one of its entries is missing a finite `frameStart` and
 *   `frameEnd`.
 * @throws {RangeError} `frameRate` is not a finite number, `sampleRate` is out
 *   of the supported range, or `voiced` / `voicedProb` do not have the same
 *   length as `f0Hz`.
 * @throws {SonareError} `index` is out of range, or `frame` is not strictly
 *   inside that note's own span.
 *
 * @example
 * ```ts
 * const notes = extractNotes({ samples, sampleRate, f0Hz, voiced, frameRate });
 * const split = splitNote({
 *   samples,
 *   sampleRate,
 *   f0Hz,
 *   voiced,
 *   frameRate,
 *   notes,
 *   index: 0,
 *   frame: notes[0].frameStart + 5,
 * });
 * ```
 */
export function splitNote(request: SplitNoteRequest): NoteObject[] {
  const { samples, sampleRate, f0Hz, frameRate, voiced, notes, index, frame, ...options } = request;
  assertNoteTrackRequest('splitNote', request);
  if (!Array.isArray(notes)) {
    throw new TypeError('splitNote: notes must be an array');
  }
  assertNoteSetEntries('splitNote', notes);
  return addon.splitNote(samples, sampleRate, f0Hz, frameRate, notes, index, frame, {
    ...options,
    voiced: voiced ? toVoicedInt32(voiced) : undefined,
  });
}

/**
 * Join a run of notes into one.
 *
 * The result spans from the first note's onset to the last note's offset,
 * including whatever the segmenter cut out between them, and its measured fields
 * are derived over that whole span — the pitch and amplitude of an unvoiced gap
 * live in the track and the audio, not in either neighbour.
 *
 * It takes `notes[first]`'s edit, envelope included. Notes carrying different
 * edits have no single correct answer here, so the rule is stated rather than
 * guessed at; a host that cares sets the edit afterwards. Every note in the set
 * is re-derived exactly as {@link splitNote} describes.
 *
 * @param request - Audio, its sample rate, the F0 track, the current note set,
 *   and the inclusive run to join.
 * @returns The whole new note set, in time order, one note shorter per note
 *   joined away.
 * @throws {TypeError} Neither `voiced` nor `voicedProb` was given, `notes` is
 *   not an array, or one of its entries is missing a finite `frameStart` and
 *   `frameEnd`.
 * @throws {RangeError} `frameRate` is not a finite number, `sampleRate` is out
 *   of the supported range, or `voiced` / `voicedProb` do not have the same
 *   length as `f0Hz`.
 * @throws {SonareError} `first` is not less than `last`, or `last` is out of
 *   range.
 *
 * @example
 * ```ts
 * const notes = extractNotes({ samples, sampleRate, f0Hz, voiced, frameRate });
 * const joined = mergeNotes({
 *   samples,
 *   sampleRate,
 *   f0Hz,
 *   voiced,
 *   frameRate,
 *   notes,
 *   first: 0,
 *   last: 1,
 * });
 * ```
 */
export function mergeNotes(request: MergeNotesRequest): NoteObject[] {
  const { samples, sampleRate, f0Hz, frameRate, voiced, notes, first, last, ...options } = request;
  assertNoteTrackRequest('mergeNotes', request);
  if (!Array.isArray(notes)) {
    throw new TypeError('mergeNotes: notes must be an array');
  }
  assertNoteSetEntries('mergeNotes', notes);
  return addon.mergeNotes(samples, sampleRate, f0Hz, frameRate, notes, first, last, {
    ...options,
    voiced: voiced ? toVoicedInt32(voiced) : undefined,
  });
}

/**
 * Extract editable percussive events — struck sounds located in time — from
 * audio alone.
 *
 * Onsets are detected on the percussive component rather than on the source, so
 * a harmonic attack is attenuated before the detector sees it instead of being
 * filtered out afterwards. Each onset opens a span that the next one closes, and
 * every returned event carries its strength, the percussive peak over the span,
 * the share of the span's energy the separation called percussive, and the
 * identity {@link PercussiveEventEdit}. Change the edits and hand the events to
 * {@link renderPercussiveEvents} to hear them; nothing is applied here.
 *
 * Each onset is backtracked to the transient's start, which is not optional and
 * is why there is no knob for it: peak-picking lands after the attack, and a
 * span that opened there would report the next hit's peak and leave its own
 * attack behind when muted.
 *
 * An event carries no pitch and is never associated with a {@link NoteObject};
 * {@link extractNotes} is the separate call that produces those.
 *
 * @param request - Audio, its sample rate, the separation framing, and the
 *   optional detection tuning.
 * @returns One {@link PercussiveEvent} per detected hit, in time order. Audio in
 *   which nothing was detected returns an empty array rather than throwing.
 * @throws {RangeError} `sampleRate` is out of the supported range.
 * @throws {SonareError} `samples` is empty, a separation field is not an
 *   integer within the signed 32-bit range, the framing breaks constant
 *   overlap-add, `maxEventMs` is not positive and finite, or
 *   `minPercussiveRatio` is outside `[0, 1]`.
 *
 * @example
 * ```ts
 * const events = extractPercussiveEvents({ samples, sampleRate });
 *
 * // Silence the second hit and drag the third 10 ms later.
 * events[1].edit.muted = true;
 * events[2].edit.timeOffsetSamples = Math.round(0.01 * sampleRate);
 * const edited = renderPercussiveEvents({ samples, sampleRate, events });
 * ```
 */
export function extractPercussiveEvents(
  request: ExtractPercussiveEventsRequest,
): PercussiveEvent[] {
  const { samples, sampleRate, ...options } = request;
  assertSampleRate('extractPercussiveEvents', sampleRate);
  assertPercussiveSeparation('extractPercussiveEvents', options);
  if (options.onsetWait !== undefined) {
    // Checked here with its four bag-siblings, and for the same reason: 0 is
    // this field's default, and the addon's narrowing truncates onto it.
    assertInt32('extractPercussiveEvents', options.onsetWait, 'onsetWait');
  }
  return addon.extractPercussiveEvents(toSamples(samples), sampleRate, options);
}

/**
 * Render edited percussive events over their source audio.
 *
 * Per event the lifted signal is the percussive component over
 * `[onsetSample, offsetSample)` under the tail fade. It is subtracted where it
 * sits and, unless the event is muted, added back at the shifted position scaled
 * by the gain. Only that signal moves, so muting a hit leaves the harmonic
 * content under it sounding and moving one does not drag its neighbours' sustain
 * along.
 *
 * Each event's span and `edit` are read; `strength`, `peakAmplitude` and
 * `percussiveRatio` are ignored, so what {@link extractPercussiveEvents}
 * returned can be handed straight back. A set whose edits are all identity
 * reproduces the input bit for bit and runs no separation at all. The result has
 * the input's length.
 *
 * Pass back the separation the extraction used: a different one lifts a
 * different signal out of the span than the one the events describe. It is
 * validated even when every edit is the identity, so an unusable framing is an
 * error on every set.
 *
 * Overlap is checked on the source spans only. Where `timeOffsetSamples` lands
 * an event is not, so two moved events may be written over each other, and a
 * shift that pushes the signal past either end is truncated there rather than
 * wrapped.
 *
 * @param request - Audio, its sample rate, the events to render, the separation
 *   framing, and the optional tail fade.
 * @returns The rendered audio, the same length as `samples`.
 * @throws {TypeError} `events` is not an array, or one of its entries is not a
 *   plain object.
 * @throws {RangeError} `sampleRate` is out of the supported range.
 * @throws {SonareError} `samples` is empty, a separation field is not an integer
 *   within the signed 32-bit range, an event's span is empty, reversed or
 *   outside the audio, two source spans overlap, a `gainDb` is not finite, the
 *   framing breaks constant overlap-add, or `fadeMs` is not positive and finite.
 *
 * @example
 * ```ts
 * const events = extractPercussiveEvents({ samples, sampleRate });
 *
 * // Drop every hit the separation was least sure about by 6 dB.
 * const tamed = events.map((event) =>
 *   event.percussiveRatio < 0.3 ? { ...event, edit: { ...event.edit, gainDb: -6 } } : event,
 * );
 * const output = renderPercussiveEvents({ samples, sampleRate, events: tamed, fadeMs: 10 });
 * ```
 */
export function renderPercussiveEvents(request: RenderPercussiveEventsRequest): Float32Array {
  const { samples, sampleRate, events, ...options } = request;
  assertSampleRate('renderPercussiveEvents', sampleRate);
  if (!Array.isArray(events)) {
    throw new TypeError('renderPercussiveEvents: events must be an array');
  }
  assertPercussiveSeparation('renderPercussiveEvents', options);
  assertEditTimeOffsets('renderPercussiveEvents', events, 'events');
  return addon.renderPercussiveEvents(toSamples(samples), sampleRate, events, options);
}
