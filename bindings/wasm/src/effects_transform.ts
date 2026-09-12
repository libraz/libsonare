import { resolveFftOptions } from './_fft_options';
import { getSonareModule } from './module_state';
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
} from './public_types';
import type { ValidateOptions } from './validation';
import {
  assertHpssKernels,
  assertPercussiveSeparation,
  assertSampleRate,
  assertSamples,
} from './validation';

function requireModule() {
  return getSonareModule();
}

// The embind layer reads the companion voicing array as Float32Array. A flag is
// a decision, not a magnitude: collapse to 1/0 on truthiness, which is the same
// reduction the Node facade applies, so both surfaces agree on every accepted
// input type.
function toVoicedFloat32(voiced: VoicedFlags): Float32Array {
  const out = new Float32Array(voiced.length);
  for (let index = 0; index < voiced.length; index += 1) {
    out[index] = voiced[index] ? 1 : 0;
  }
  return out;
}

// Shared input checks of the entry points that take audio plus a whole F0 track,
// returning the voicing flags in the Float32Array form the embind layer reads. A
// companion array of the wrong length is a RangeError here because the native
// side can only report it as a flat InvalidParameter.
function assertNoteTrack(
  fnName: string,
  request: ExtractNotesRequest | NoteSetRequest,
): Float32Array | undefined {
  assertSamples(fnName, request.samples, request.validate !== false);
  assertSampleRate(fnName, request.sampleRate);
  if (request.voiced && request.voiced.length !== request.f0Hz.length) {
    throw new RangeError(`${fnName}: voiced length must match f0Hz length`);
  }
  if (request.voicedProb && request.voicedProb.length !== request.f0Hz.length) {
    throw new RangeError(`${fnName}: voicedProb length must match f0Hz length`);
  }
  return request.voiced ? toVoicedFloat32(request.voiced) : undefined;
}

export type NormalizeMode = 'peak' | 'rms';

function resolveNormalizeMode(value: unknown): NormalizeMode {
  if (value === undefined) {
    return 'peak';
  }
  if (typeof value !== 'string') {
    throw new TypeError("normalize: mode must be the string 'peak' or 'rms'");
  }
  if (value !== 'peak' && value !== 'rms') {
    throw new RangeError("normalize: mode must be the string 'peak' or 'rms'");
  }
  return value;
}

function resolveHardMask(value: unknown, fnName: string): boolean {
  if (value === undefined) {
    return false;
  }
  if (typeof value !== 'boolean') {
    throw new TypeError(`${fnName}: hardMask must be a boolean`);
  }
  return value;
}

/** Canonical request form for HPSS. */
export interface HpssRequest {
  samples: Float32Array;
  sampleRate?: number;
  kernelHarmonic?: number;
  kernelPercussive?: number;
  nFft?: number;
  hopLength?: number;
  hardMask?: boolean;
}

export interface HarmonicRequest extends ValidateOptions {
  samples: Float32Array;
  sampleRate?: number;
}

export interface PercussiveRequest extends ValidateOptions {
  samples: Float32Array;
  sampleRate?: number;
}

export interface TimeStretchRequest extends ValidateOptions {
  samples: Float32Array;
  sampleRate?: number;
  rate: number;
  nFft?: number;
  hopLength?: number;
}

export interface PitchShiftRequest extends ValidateOptions {
  samples: Float32Array;
  sampleRate?: number;
  semitones: number;
  nFft?: number;
  hopLength?: number;
}

export interface PitchCorrectToMidiRequest extends ValidateOptions {
  samples: Float32Array;
  sampleRate?: number;
  currentMidi?: number;
  targetMidi?: number;
}

export interface PitchCorrectToMidiTimevaryingRequest extends ValidateOptions {
  samples: Float32Array;
  f0Hz: Float32Array;
  targetMidi: number;
  sampleRate?: number;
  hopLength?: number;
  voiced?: VoicedFlags;
  voicedProb?: Float32Array;
}

export interface PitchCorrectTimevaryingRequest extends PitchCorrectOptions {
  samples: Float32Array;
  f0Hz: Float32Array;
  sampleRate?: number;
  hopLength?: number;
}

export interface NoteStretchRequest extends NoteStretchOptions, ValidateOptions {
  samples: Float32Array;
  sampleRate?: number;
}
export interface NoteMoveRequest extends NoteMoveOptions, ValidateOptions {
  samples: Float32Array;
  sampleRate?: number;
}

/** Canonical request form for {@link extractNotes}. */
export interface ExtractNotesRequest extends NoteExtractorOptions, ValidateOptions {
  samples: Float32Array;
  /**
   * Sample rate in Hz. Required: `minNoteMs` and the per-frame RMS windows are
   * converted to samples with this rate, so a wrong/omitted value silently
   * segments differently.
   */
  sampleRate: number;
  /**
   * Per-frame F0 in Hz; finite and non-negative, zero meaning unvoiced. A
   * {@link pitchPyin} track only satisfies that with `fillNa: true` — its
   * default leaves unvoiced frames NaN, which this rejects.
   */
  f0Hz: Float32Array;
  /** F0 frames per second. */
  frameRate: number;
  /** Per-frame voiced flags (truthy = voiced). Takes precedence over `voicedProb`. */
  voiced?: VoicedFlags;
  /** Per-frame voicing probability in `[0, 1]`; read only when `voiced` is omitted. */
  voicedProb?: Float32Array;
}

/** Canonical request form for {@link renderNotes}. */
export interface RenderNotesRequest extends ValidateOptions {
  samples: Float32Array;
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
   * The F0 track the notes were extracted from. Required only by
   * `vibratoDepthChange` and `driftChange`, which act on the note's own pitch
   * curve; every other edit ignores it. The curve is not carried on a
   * {@link NoteObject} for the same reason it is not returned by
   * {@link extractNotes} — it is this array sliced by
   * `[frameStart, frameEnd)`, which the caller already holds.
   */
  f0Hz?: Float32Array;
  /** F0 frames per second. Required when `f0Hz` is given. */
  frameRate?: number;
  /**
   * Boundary between the drift and the vibrato that `vibratoDepthChange` and
   * `driftChange` act on, in Hz. Default 3 Hz.
   *
   * Pass whatever {@link decomposeNotePitch} was called with. A host that draws
   * the vibrato at one cutoff and edits it at another edits a curve it never
   * showed anyone.
   */
  vibratoCutoffHz?: number;
}

/** Canonical request form for {@link decomposeNotePitch}. */
export interface DecomposeNotePitchRequest {
  /**
   * The note's slice of the F0 track — `f0Hz.subarray(frameStart, frameEnd)`.
   * Every value must be finite and non-negative; zero denotes an unvoiced frame.
   */
  f0Hz: Float32Array;
  /** F0 frames per second. */
  frameRate: number;
  /**
   * The note's `medianHz`. A note with no pitch is spelled 0, so a negative
   * value is rejected rather than read as a second way of saying that.
   */
  medianHz: number;
  /** Where drift ends and vibrato begins, in Hz. Default 3 Hz. */
  vibratoCutoffHz?: number;
}

/** The audio, track and note set the two note-set reshaping calls share. */
export interface NoteSetRequest extends NoteExtractorOptions, ValidateOptions {
  /**
   * The audio the set was extracted from. Every note is re-measured against it,
   * so a different buffer re-derives the whole set against something else.
   */
  samples: Float32Array;
  sampleRate: number;
  /**
   * Per-frame F0 in Hz; finite and non-negative, zero meaning unvoiced. A
   * {@link pitchPyin} track only satisfies that with `fillNa: true` — its
   * default leaves unvoiced frames NaN, which this rejects.
   */
  f0Hz: Float32Array;
  /** F0 frames per second. */
  frameRate: number;
  /** Per-frame voiced flags (truthy = voiced). Takes precedence over `voicedProb`. */
  voiced?: VoicedFlags;
  /** Per-frame voicing probability in `[0, 1]`; read only when `voiced` is omitted. */
  voicedProb?: Float32Array;
  /** The current note set. Each note's `[frameStart, frameEnd)` must be non-empty and inside the track. */
  notes: readonly NoteSetEntry[];
}

/** Canonical request form for {@link splitNote}. */
export interface SplitNoteRequest extends NoteSetRequest {
  /** Index of the note to split. */
  index: number;
  /** Track frame to cut at, strictly inside that note's own span. */
  frame: number;
}

/** Canonical request form for {@link mergeNotes}. */
export interface MergeNotesRequest extends NoteSetRequest {
  /** Index of the first note to join; must be `< last`. */
  first: number;
  /** Index of the last note to join, inclusive. */
  last: number;
}

/**
 * The separation a percussive event's signal is lifted out with. Extraction
 * measures events against it and rendering has to repeat it, so both calls take
 * the same four fields and a render must be given what the extraction was.
 */
export interface PercussiveSeparationOptions {
  /**
   * FFT size and hop the separation and the onset detector share. They cannot be
   * set apart: an event measured on one framing and lifted out on another is not
   * the same signal. Default 2048 and 512. The pair must overlap-add — `nFft`
   * even and at least 2, `hopLength` no more than half of it — because the
   * separation inverts an STFT. A negative value is rejected on its own, before
   * the zero-is-default rule could swallow it.
   */
  nFft?: number;
  /** Hop length in samples. Default 512. */
  hopLength?: number;
  /**
   * Median filter lengths the separation runs, along time and along frequency. A
   * longer harmonic kernel calls more of a sustained sound harmonic. Default 31;
   * any other value must be odd and positive, so an even one is rejected rather
   * than rounded. 1 is legal and degenerate rather than an error: a length-1
   * median is the identity, so both components come back as the source.
   */
  hpssKernelHarmonic?: number;
  /** Vertical median filter length, under the same rule. Default 31. */
  hpssKernelPercussive?: number;
}

/** Canonical request form for {@link extractPercussiveEvents}. */
export interface ExtractPercussiveEventsRequest
  extends PercussiveSeparationOptions,
    ValidateOptions {
  samples: Float32Array | readonly number[];
  /**
   * Sample rate in Hz. Required: `maxEventMs` is converted to samples with this
   * rate, so a wrong/omitted value caps the spans differently.
   */
  sampleRate: number;
  /** Minimum frames between consecutive onsets. Default 1; negative is rejected. */
  onsetWait?: number;
  /**
   * Offset added to the detector's adaptive threshold; raising it finds fewer,
   * stronger hits and lowering it finds more. Default 0.06, so exactly zero is
   * the one value not selectable here — a negative one is accepted and puts the
   * threshold below the default, which is the direction a caller reaching for
   * zero wanted anyway.
   */
  onsetDelta?: number;
  /**
   * Caps a span that no onset follows. It binds at the end of a phrase and at the
   * end of the track; anywhere else the next onset closes the span first. Default
   * 500 ms.
   */
  maxEventMs?: number;
  /**
   * Drops an event whose `percussiveRatio` falls below this; must be in `[0, 1]`.
   * 0 is both the default and the meaningful "keep everything". Raising it is
   * useful on material that is mostly drums and wrong on a dense mix, where it
   * also drops real hits sitting over a loud sustain.
   */
  minPercussiveRatio?: number;
}

/** Canonical request form for {@link renderPercussiveEvents}. */
export interface RenderPercussiveEventsRequest
  extends PercussiveSeparationOptions,
    ValidateOptions {
  samples: Float32Array | readonly number[];
  /**
   * Sample rate in Hz. Required: `fadeMs` is converted to samples with this rate,
   * so a wrong/omitted value changes the fade length.
   */
  sampleRate: number;
  /** The events to render, with their edits. Source spans must not overlap. */
  events: readonly PercussiveEventInput[];
  /**
   * Fade-out at the tail of each lifted span. Default 5 ms, so a zero-length
   * fade is unreachable here rather than rejected — and a hard cut is not a thing
   * to want anyway, because what the fade shapes is the signal being subtracted,
   * so squaring it off leaves a step. There is deliberately
   * no matching fade-in: a span opens in front of its transient, where the
   * percussive component is near-silent, so cutting square there costs nothing
   * and keeps a muted hit's attack from surviving inside a fade.
   */
  fadeMs?: number;
}

export interface NormalizeRequest extends ValidateOptions {
  samples: Float32Array;
  sampleRate?: number;
  targetDb?: number;
  mode?: NormalizeMode;
}

export interface SpectralEditRequest extends SpectralEditOptions, ValidateOptions {
  samples: Float32Array;
  sampleRate: number;
  ops?: SpectralRegionOp[];
}

// ============================================================================
// Effects
// ============================================================================

/**
 * Perform Harmonic-Percussive Source Separation (HPSS).
 *
 * @param samples - Audio samples (mono, float32)
 * @param sampleRate - Sample rate in Hz (default: 22050)
 * @param kernelHarmonic - Horizontal median filter size for harmonic (default: 31)
 * @param kernelPercussive - Vertical median filter size for percussive (default: 31)
 * @returns Separated harmonic and percussive components
 * @throws SonareError (`InvalidParameter`) on a kernel that is not an integer
 *   within the signed 32-bit range, or one the core rejects as even,
 *   non-positive or above its ceiling
 */
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
  const resolvedHardMask = resolveHardMask(request.hardMask, 'hpss');
  const resolvedKernelHarmonic = request.kernelHarmonic ?? 31;
  const resolvedKernelPercussive = request.kernelPercussive ?? 31;
  assertHpssKernels('hpss', resolvedKernelHarmonic, resolvedKernelPercussive);
  return requireModule().hpssEx(
    request.samples,
    request.sampleRate ?? 22050,
    resolvedKernelHarmonic,
    resolvedKernelPercussive,
    fftOptions.nFft,
    fftOptions.hopLength,
    resolvedHardMask,
  );
}

/**
 * Extract harmonic component from audio.
 *
 * @param samples - Audio samples (mono, float32)
 * @param sampleRate - Sample rate in Hz
 * @returns Harmonic component
 */
export function harmonic(request: HarmonicRequest): Float32Array;
export function harmonic(
  samples: Float32Array,
  sampleRate?: number,
  options?: ValidateOptions,
): Float32Array;
export function harmonic(
  samples: Float32Array | HarmonicRequest,
  sampleRate = 22050,
  options: ValidateOptions = {},
): Float32Array {
  const request = samples instanceof Float32Array ? { samples, sampleRate, ...options } : samples;
  assertSamples('harmonic', request.samples, request.validate !== false);
  return requireModule().harmonic(request.samples, request.sampleRate ?? 22050);
}

/**
 * Extract percussive component from audio.
 *
 * @param samples - Audio samples (mono, float32)
 * @param sampleRate - Sample rate in Hz
 * @returns Percussive component
 */
export function percussive(request: PercussiveRequest): Float32Array;
export function percussive(
  samples: Float32Array,
  sampleRate?: number,
  options?: ValidateOptions,
): Float32Array;
export function percussive(
  samples: Float32Array | PercussiveRequest,
  sampleRate = 22050,
  options: ValidateOptions = {},
): Float32Array {
  const request = samples instanceof Float32Array ? { samples, sampleRate, ...options } : samples;
  assertSamples('percussive', request.samples, request.validate !== false);
  return requireModule().percussive(request.samples, request.sampleRate ?? 22050);
}

/**
 * Time-stretch audio without changing pitch.
 *
 * @param samples - Audio samples (mono, float32)
 * @param sampleRate - Sample rate in Hz (default: 22050)
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
  options?: ValidateOptions,
): Float32Array;
export function timeStretch(
  samples: Float32Array,
  sampleRate: number,
  rate: number,
  nFft?: number,
  hopLength?: number,
  options?: ValidateOptions,
): Float32Array;
export function timeStretch(
  samples: Float32Array | TimeStretchRequest,
  sampleRate?: number,
  rate?: number,
  nFftOrOptions?: number | ValidateOptions,
  hopLength?: number,
  options: ValidateOptions = {},
): Float32Array {
  if (
    nFftOrOptions !== undefined &&
    nFftOrOptions !== null &&
    typeof nFftOrOptions !== 'number' &&
    typeof nFftOrOptions !== 'object'
  ) {
    throw new TypeError('timeStretch: nFft must be an integer or options object');
  }
  if (nFftOrOptions === null) {
    throw new TypeError('timeStretch: nFft must be an integer or options object');
  }
  const positionalOptions =
    typeof nFftOrOptions === 'object' && nFftOrOptions !== null ? nFftOrOptions : options;
  const positionalNFft = typeof nFftOrOptions === 'number' ? nFftOrOptions : undefined;
  const request: TimeStretchRequest =
    samples instanceof Float32Array
      ? {
          samples,
          sampleRate,
          rate: rate as number,
          nFft: positionalNFft,
          hopLength,
          ...positionalOptions,
        }
      : samples;
  assertSamples('timeStretch', request.samples, request.validate !== false);
  const fftOptions = resolveFftOptions('timeStretch', request.nFft, request.hopLength);
  return requireModule().timeStretchEx(
    request.samples,
    request.sampleRate ?? 22050,
    request.rate,
    fftOptions.nFft,
    fftOptions.hopLength,
  );
}

/**
 * Pitch-shift audio without changing duration.
 *
 * @param samples - Audio samples (mono, float32)
 * @param sampleRate - Sample rate in Hz (default: 22050)
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
  options?: ValidateOptions,
): Float32Array;
export function pitchShift(
  samples: Float32Array,
  sampleRate: number,
  semitones: number,
  nFft?: number,
  hopLength?: number,
  options?: ValidateOptions,
): Float32Array;
export function pitchShift(
  samples: Float32Array | PitchShiftRequest,
  sampleRate?: number,
  semitones?: number,
  nFftOrOptions?: number | ValidateOptions,
  hopLength?: number,
  options: ValidateOptions = {},
): Float32Array {
  if (
    nFftOrOptions !== undefined &&
    nFftOrOptions !== null &&
    typeof nFftOrOptions !== 'number' &&
    typeof nFftOrOptions !== 'object'
  ) {
    throw new TypeError('pitchShift: nFft must be an integer or options object');
  }
  if (nFftOrOptions === null) {
    throw new TypeError('pitchShift: nFft must be an integer or options object');
  }
  const positionalOptions =
    typeof nFftOrOptions === 'object' && nFftOrOptions !== null ? nFftOrOptions : options;
  const positionalNFft = typeof nFftOrOptions === 'number' ? nFftOrOptions : undefined;
  const request: PitchShiftRequest =
    samples instanceof Float32Array
      ? {
          samples,
          sampleRate,
          semitones: semitones as number,
          nFft: positionalNFft,
          hopLength,
          ...positionalOptions,
        }
      : samples;
  assertSamples('pitchShift', request.samples, request.validate !== false);
  const fftOptions = resolveFftOptions('pitchShift', request.nFft, request.hopLength);
  return requireModule().pitchShiftEx(
    request.samples,
    request.sampleRate ?? 22050,
    request.semitones,
    fftOptions.nFft,
    fftOptions.hopLength,
  );
}

/**
 * Pitch-correct audio from a current MIDI note to a target MIDI note.
 *
 * Applies one constant, immediate transpose with no retune glide and preserves
 * the input buffer length. The whole interval is applied however large it is:
 * both endpoints are validated to [0, 127], so a two-octave move such as
 * C3 -> C5 transposes by the full 24 semitones. Use
 * {@link pitchCorrectToMidiTimevarying} for a caller-supplied pitch contour.
 *
 * @param samples - Audio samples (mono, float32)
 * @param sampleRate - Sample rate in Hz
 * @param currentMidi - Detected/current MIDI note number
 * @param targetMidi - Desired MIDI note number
 * @returns Pitch-corrected audio
 */
export function pitchCorrectToMidi(request: PitchCorrectToMidiRequest): Float32Array;
export function pitchCorrectToMidi(
  samples: Float32Array,
  sampleRate?: number,
  currentMidi?: number,
  targetMidi?: number,
  options?: ValidateOptions,
): Float32Array;
export function pitchCorrectToMidi(
  samples: Float32Array | PitchCorrectToMidiRequest,
  sampleRate = 22050,
  currentMidi = 69.0,
  targetMidi = 69.0,
  options: ValidateOptions = {},
): Float32Array {
  const request =
    samples instanceof Float32Array
      ? { samples, sampleRate, currentMidi, targetMidi, ...options }
      : samples;
  assertSamples('pitchCorrectToMidi', request.samples, request.validate !== false);
  return requireModule().pitchCorrectToMidi(
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
 *
 * @param samples - Audio samples (mono, float32)
 * @param f0Hz - Per-frame measured F0 in Hz (one entry per analysis frame)
 * @param targetMidi - Desired MIDI note number
 * @param sampleRate - Sample rate in Hz
 * @param hopLength - F0 hop in samples (frame i covers sample i*hopLength)
 * @param voiced - Optional per-frame voiced flags (truthy = voiced)
 * @param voicedProb - Optional per-frame voicing probability in [0, 1]
 * @returns Pitch-corrected audio
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
  options?: ValidateOptions,
): Float32Array;
export function pitchCorrectToMidiTimevarying(
  samples: Float32Array | PitchCorrectToMidiTimevaryingRequest,
  f0Hz?: Float32Array,
  targetMidi?: number,
  sampleRate = 22050,
  hopLength = 512,
  voiced?: VoicedFlags,
  voicedProb?: Float32Array,
  options: ValidateOptions = {},
): Float32Array {
  const request: PitchCorrectToMidiTimevaryingRequest =
    samples instanceof Float32Array
      ? {
          samples,
          f0Hz: f0Hz as Float32Array,
          targetMidi: targetMidi as number,
          sampleRate,
          hopLength,
          voiced,
          voicedProb,
          ...options,
        }
      : samples;
  assertSamples('pitchCorrectToMidiTimevarying', request.samples, request.validate !== false);
  if (request.voiced && request.voiced.length !== request.f0Hz.length) {
    throw new RangeError('pitchCorrectToMidiTimevarying: voiced length must match f0Hz length');
  }
  if (request.voicedProb && request.voicedProb.length !== request.f0Hz.length) {
    throw new RangeError('pitchCorrectToMidiTimevarying: voicedProb length must match f0Hz length');
  }
  const voicedF32 = request.voiced ? toVoicedFloat32(request.voiced) : undefined;
  return requireModule().pitchCorrectToMidiTimevarying(
    request.samples,
    request.sampleRate ?? 22050,
    request.f0Hz,
    request.targetMidi,
    request.hopLength ?? 512,
    voicedF32,
    request.voicedProb,
  );
}

/**
 * Contour-following pitch correction toward a fixed MIDI note OR a musical
 * scale, with tunable retune strength and vibrato preservation.
 *
 * Generalises {@link pitchCorrectToMidiTimevarying}: the same caller-supplied
 * per-frame `f0Hz` contour drives correction, but `options.mode` selects between
 * a fixed-MIDI target (`'midi'`, default) and scale quantisation (`'scale'`),
 * and the retune knobs shape natural-vs-robotic correction. An `f0Hz` NaN is
 * accepted only for a frame marked unvoiced.
 *
 * @param samples - Audio samples (mono, float32)
 * @param f0Hz - Per-frame measured F0 in Hz (one entry per analysis frame)
 * @param sampleRate - Sample rate in Hz
 * @param hopLength - F0 hop in samples (frame i covers sample i*hopLength)
 * @param options - Target mode + retune knobs + optional voiced/voicedProb arrays
 * @returns Pitch-corrected audio
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
  const request: PitchCorrectTimevaryingRequest =
    samples instanceof Float32Array
      ? { samples, f0Hz: f0Hz as Float32Array, sampleRate, hopLength, ...options }
      : samples;
  assertSamples('pitchCorrectTimevarying', request.samples, request.validate !== false);
  if (request.voiced && request.voiced.length !== request.f0Hz.length) {
    throw new RangeError('pitchCorrectTimevarying: voiced length must match f0Hz length');
  }
  if (request.voicedProb && request.voicedProb.length !== request.f0Hz.length) {
    throw new RangeError('pitchCorrectTimevarying: voicedProb length must match f0Hz length');
  }
  const nativeOptions = {
    ...request,
    voiced: request.voiced ? toVoicedFloat32(request.voiced) : undefined,
  };
  return requireModule().pitchCorrectTimevarying(
    request.samples,
    request.sampleRate ?? 22050,
    request.f0Hz,
    request.hopLength ?? 512,
    nativeOptions,
  );
}

/**
 * Time-stretch a note region between two sample offsets without changing pitch.
 *
 * @param samples - Audio samples (mono, float32)
 * @param sampleRate - Sample rate in Hz
 * @param onsetSample - Note onset position in samples
 * @param offsetSample - Note offset position in samples
 * @param stretchRatio - Stretch ratio (0.5 = half duration, 2.0 = double duration)
 * @returns Audio with the note region stretched
 */
export function noteStretch(request: NoteStretchRequest): Float32Array;
export function noteStretch(
  samples: Float32Array,
  sampleRate?: number,
  options?: NoteStretchOptions & ValidateOptions,
): Float32Array;
export function noteStretch(
  samples: Float32Array | NoteStretchRequest,
  sampleRate = 22050,
  options: NoteStretchOptions & ValidateOptions = {},
): Float32Array {
  const request = samples instanceof Float32Array ? { samples, sampleRate, ...options } : samples;
  assertSamples('noteStretch', request.samples, request.validate !== false);
  return requireModule().noteStretch(
    request.samples,
    request.sampleRate ?? 22050,
    request.onsetSample ?? 0,
    request.offsetSample ?? request.samples.length,
    request.stretchRatio ?? 1.0,
  );
}

/** Move a note region to a new onset without changing its duration. */
export function noteMove(request: NoteMoveRequest): Float32Array;
export function noteMove(
  samples: Float32Array,
  sampleRate?: number,
  options?: NoteMoveOptions & ValidateOptions,
): Float32Array;
export function noteMove(
  samples: Float32Array | NoteMoveRequest,
  sampleRate = 22050,
  options: NoteMoveOptions & ValidateOptions = {},
): Float32Array {
  const request = samples instanceof Float32Array ? { samples, sampleRate, ...options } : samples;
  assertSamples('noteMove', request.samples, request.validate !== false);
  return requireModule().noteMove(
    request.samples,
    request.sampleRate ?? 22050,
    request.onsetSample ?? 0,
    request.offsetSample ?? request.samples.length,
    request.targetOnsetSample ?? 0,
  );
}

/**
 * Extract editable note objects from audio and a caller-supplied F0 track.
 *
 * The track is segmented into monophonic notes; each note carries its span in
 * source samples, its span in the track's own frames, its median pitch, two
 * measured quality figures, its per-frame amplitude (RMS) curve, and an identity
 * {@link NoteEdit}. Edit the notes and hand them to {@link renderNotes} to apply
 * the result — the source audio is never mutated, and a set whose edits are all
 * identity renders back to the input bit for bit.
 *
 * The per-note F0 curve is deliberately not returned: it is the caller's own
 * `f0Hz` sliced by `[frameStart, frameEnd)`. The amplitude curve is measured
 * here, so it is, one entry per F0 frame over that note's span.
 *
 * Voicing comes from `voiced` (truthy = voiced). `voicedProb` is read only when
 * `voiced` is absent, and then a frame counts as voiced at or above
 * `voicedThreshold` (default 0.5). At least one of the two is required. Because
 * `voicedProb` from pYIN rises with F0 for a fixed frame length, prefer passing
 * a {@link PitchResult}'s `voicedFlag` through `voiced`.
 *
 * @param request - Audio, F0 track, frame cadence and segmenter options
 * @returns One {@link NoteObject} per segmented note, in time order; an empty
 *   array when the track segments to nothing
 * @throws RangeError when `voiced` / `voicedProb` do not match `f0Hz` in length,
 *   or the samples/sample rate fail the shared input checks
 * @throws SonareError (`InvalidParameter`) on an empty `f0Hz`, a non-positive
 *   `frameRate`, a negative or non-finite `f0Hz` value, a `voicedProb` outside
 *   `[0, 1]`, or a negative option value
 *
 * @example
 * ```ts
 * // fillNa is required: the default leaves unvoiced frames NaN.
 * const pitch = pitchPyin({ samples, sampleRate, fillNa: true });
 * const notes = extractNotes({
 *   samples,
 *   sampleRate,
 *   f0Hz: pitch.f0,
 *   voiced: pitch.voicedFlag,
 *   frameRate: sampleRate / 512,
 *   minNoteMs: 40,
 * });
 * // Lift the second note by a semitone and mute the third.
 * notes[1].edit.pitchShiftSemitones = 1;
 * notes[2].edit.muted = true;
 * const edited = renderNotes({ samples, sampleRate, notes });
 * ```
 */
export function extractNotes(request: ExtractNotesRequest): NoteObject[] {
  const voicedF32 = assertNoteTrack('extractNotes', request);
  return requireModule().extractNotes(
    request.samples,
    request.sampleRate,
    request.f0Hz,
    request.voicedProb,
    voicedF32,
    request.frameRate,
    request,
  );
}

/**
 * Render edited note objects back over their source audio.
 *
 * Only a note whose edit is non-identity is resynthesized; the source passes
 * through everywhere else, so a set of untouched {@link extractNotes} output
 * reproduces the input exactly. The output has the input's length: an edit that
 * pushes a note past either end is truncated there.
 *
 * Per note the order is: pitch curve, time stretch, pitch shift, formant warp,
 * amplitude envelope, then gain.
 *
 * Each note's `onsetSample`, `offsetSample` and `edit` are read, plus its
 * `frameStart`, `frameEnd` and `medianHz` when the request carries an `f0Hz`
 * track for a curve edit to act on; so an extracted note can be passed back
 * as-is, or a note can be built by hand from those fields alone. Overlap is
 * checked on the source spans only — where `timeOffsetSamples` lands a note is
 * not, and a note lengthened past its own span writes into its neighbours'
 * samples, so two moved or stretched notes may be written over each other.
 *
 * @param request - Source audio, the notes to render, the cross-fade length, and
 *   the F0 track a vibrato or drift edit reads
 * @returns The rendered audio, the same length and sample rate as the input
 * @throws RangeError when the samples or sample rate fail the shared input checks
 * @throws SonareError (`InvalidParameter`) on a note whose span is empty,
 *   reversed or missing, overlapping source spans, a non-finite or non-positive
 *   edit field, a negative or non-finite envelope value, a negative `fadeMs` or
 *   `vibratoCutoffHz`, a frame span outside `f0Hz`, or a `vibratoDepthChange` /
 *   `driftChange` on a note with no usable pitch curve to apply it to
 *
 * @example
 * ```ts
 * const notes = extractNotes({ samples, sampleRate, f0Hz, voiced, frameRate });
 *
 * // Silence the third note and leave the rest untouched.
 * const muted = notes.map((note, index) =>
 *   index === 2 ? { ...note, edit: { ...note.edit, muted: true } } : note,
 * );
 * const rendered = renderNotes({ samples, sampleRate, notes: muted, fadeMs: 10 });
 *
 * // Flatten the first note's vibrato, which needs the track it was measured on.
 * const flattened = notes.map((note, index) =>
 *   index === 0 ? { ...note, edit: { ...note.edit, vibratoDepthChange: -1 } } : note,
 * );
 * const steady = renderNotes({
 *   samples,
 *   sampleRate,
 *   notes: flattened,
 *   f0Hz,
 *   frameRate: sampleRate / 512,
 * });
 * ```
 */
export function renderNotes(request: RenderNotesRequest): Float32Array {
  assertSamples('renderNotes', request.samples, request.validate !== false);
  assertSampleRate('renderNotes', request.sampleRate);
  return requireModule().renderNotes(request.samples, request.sampleRate, request.notes, request);
}

/**
 * Split one note's pitch curve into a centre, a slow drift and a vibrato.
 *
 * A performed note's pitch is one curve carrying three things at once: the note
 * that was aimed at, a slow wander around it, and a periodic oscillation on top.
 * Editing any of them on its own needs them separated first, and the only thing
 * that decides where drift ends and vibrato begins is `vibratoCutoffHz`. Hand
 * the same cutoff to {@link renderNotes}, or it edits a curve nobody was shown.
 *
 * Frames whose F0 is unusable carry no measurement, so the curve is held at the
 * nearest usable neighbour across them. Both curves therefore have an entry
 * everywhere; a host marking the held ones reads them off `f0Hz`, which is exact.
 *
 * A note with no usable pitch is reported as a zero `centreHz` and two empty
 * curves rather than as an error — that is a measurement which came up empty,
 * not a bad argument.
 *
 * @param request - The note's slice of the F0 track, its cadence, its centre, and
 *   the cutoff
 * @returns The centre and the two curves, each one entry per frame of `f0Hz`
 * @throws SonareError (`InvalidParameter`) on an empty `f0Hz`, a negative or
 *   non-finite `f0Hz` value, a non-positive `frameRate`, a negative `medianHz`,
 *   or a negative `vibratoCutoffHz`
 *
 * @example
 * ```ts
 * const note = notes[0];
 * const { centreHz, driftCents, vibratoCents } = decomposeNotePitch({
 *   f0Hz: f0Hz.subarray(note.frameStart, note.frameEnd),
 *   frameRate: sampleRate / 512,
 *   medianHz: note.medianHz,
 * });
 * ```
 */
export function decomposeNotePitch(request: DecomposeNotePitchRequest): PitchDecompositionResult {
  return requireModule().decomposeNotePitch(
    request.f0Hz,
    request.frameRate,
    request.medianHz,
    request.vibratoCutoffHz ?? 0,
  );
}

/**
 * Split one note of a set in two at a track frame.
 *
 * Both halves are re-derived from the audio and the track the way
 * {@link extractNotes} derives its own, rather than by patching the fields of
 * the note they replace. Both inherit the source note's edit, and its amplitude
 * envelope is cut at the same proportion so each half keeps its own part of it —
 * a note whose edit is the identity therefore still renders bit for bit after
 * being split.
 *
 * Every note in the set, not just the two halves, has its spans, curves, medians
 * and stability re-derived from `samples` and the track, because a
 * {@link NoteSetEntry} carries no curves for this call to copy through. The
 * frame bounds are therefore what a note is identified by here, and the audio
 * and track must be the ones the set was extracted from or the whole set is
 * re-measured against something else.
 *
 * @param request - The source, the current note set, and where to cut
 * @returns The whole new note set, one note longer than the one handed in
 * @throws RangeError when `voiced` / `voicedProb` do not match `f0Hz` in length,
 *   or the samples/sample rate fail the shared input checks
 * @throws SonareError (`InvalidParameter`) on an out-of-range `index`, a `frame`
 *   that is not strictly inside that note's own span, a note whose frame span is
 *   empty or runs past the track, or the track arguments {@link extractNotes}
 *   itself rejects
 *
 * @example
 * ```ts
 * const notes = extractNotes({ samples, sampleRate, f0Hz, voiced, frameRate });
 * const halves = splitNote({
 *   samples,
 *   sampleRate,
 *   f0Hz,
 *   voiced,
 *   frameRate,
 *   notes,
 *   index: 1,
 *   frame: Math.floor((notes[1].frameStart + notes[1].frameEnd) / 2),
 * });
 * ```
 */
export function splitNote(request: SplitNoteRequest): NoteObject[] {
  const voicedF32 = assertNoteTrack('splitNote', request);
  return requireModule().splitNote(
    request.samples,
    request.sampleRate,
    request.f0Hz,
    request.voicedProb,
    voicedF32,
    request.frameRate,
    request.notes,
    request.index,
    request.frame,
    request,
  );
}

/**
 * Join a run of notes into one.
 *
 * The result spans from `notes[first]`'s onset to `notes[last]`'s offset,
 * including whatever the segmenter cut out between them, and its measured fields
 * are derived over that whole span — the pitch and amplitude of an unvoiced gap
 * live in the track and the audio, not in either neighbour.
 *
 * It takes `notes[first]`'s edit, envelope included. Notes carrying different
 * edits have no single correct answer here, so the rule is stated rather than
 * guessed at; a host that cares sets the edit afterwards. Every note in the set
 * is re-derived from `samples` and the track, exactly as {@link splitNote}
 * describes.
 *
 * @param request - The source, the current note set, and the run to join
 * @returns The whole new note set, `last - first` notes shorter than the one
 *   handed in
 * @throws RangeError when `voiced` / `voicedProb` do not match `f0Hz` in length,
 *   or the samples/sample rate fail the shared input checks
 * @throws SonareError (`InvalidParameter`) unless `first < last < notes.length`,
 *   on a note whose frame span is empty or runs past the track, or on the track
 *   arguments {@link extractNotes} itself rejects
 *
 * @example
 * ```ts
 * // Undo a split by rejoining the two halves it produced.
 * const rejoined = mergeNotes({
 *   samples,
 *   sampleRate,
 *   f0Hz,
 *   voiced,
 *   frameRate,
 *   notes: halves,
 *   first: 1,
 *   last: 2,
 * });
 * ```
 */
export function mergeNotes(request: MergeNotesRequest): NoteObject[] {
  const voicedF32 = assertNoteTrack('mergeNotes', request);
  return requireModule().mergeNotes(
    request.samples,
    request.sampleRate,
    request.f0Hz,
    request.voicedProb,
    voicedF32,
    request.frameRate,
    request.notes,
    request.first,
    request.last,
    request,
  );
}

/**
 * Extract editable percussive events from audio alone.
 *
 * Each event is a struck sound located in time: a span in source samples, its
 * detector strength, the percussive peak over the span, the share of the span's
 * energy the separation called percussive, and an identity
 * {@link PercussiveEventEdit}. Edit the events and hand them to
 * {@link renderPercussiveEvents} to apply the result — the source audio is never
 * mutated, and a set whose edits are all identity renders back to the input bit
 * for bit.
 *
 * Onsets are detected on the percussive component rather than on the source, so
 * a harmonic attack is attenuated before the detector sees it instead of being
 * filtered out afterwards. Each onset opens a span that the next one closes,
 * capped by `maxEventMs` and never running past the end of the audio.
 *
 * Each onset is backtracked to the transient's start, which is not optional and
 * is why there is no knob for it: peak-picking lands after the attack, and a span
 * that opened there would report the next hit's peak and leave its own attack
 * behind when muted.
 *
 * An event carries no pitch and is never associated with a {@link NoteObject} —
 * a struck sound has no steady F0 to edit, so the two models are extracted by
 * separate calls.
 *
 * @param request - Audio, its sample rate, and the separation, peak-picking and
 *   span options
 * @returns One {@link PercussiveEvent} per detected hit, in time order; an empty
 *   array when nothing was detected
 * @throws RangeError when the samples or sample rate fail the shared input checks
 * @throws SonareError (`InvalidParameter`) on a kernel size that is not an
 *   integer within the 32-bit range, a framing size that is negative or outside
 *   that range, a framing that breaks constant overlap-add, a negative or
 *   non-finite `onsetWait` / `onsetDelta` / `maxEventMs`, or a
 *   `minPercussiveRatio` outside `[0, 1]`
 *
 * @example
 * ```ts
 * const events = extractPercussiveEvents({ samples, sampleRate });
 * // Drop the second hit and push the third 10 ms late.
 * events[1].edit.muted = true;
 * events[2].edit.timeOffsetSamples = Math.round(0.01 * sampleRate);
 * const edited = renderPercussiveEvents({ samples, sampleRate, events });
 * ```
 */
export function extractPercussiveEvents(
  request: ExtractPercussiveEventsRequest,
): PercussiveEvent[] {
  assertSamples('extractPercussiveEvents', request.samples, request.validate !== false);
  assertSampleRate('extractPercussiveEvents', request.sampleRate);
  assertPercussiveSeparation('extractPercussiveEvents', request);
  return requireModule().extractPercussiveEvents(request.samples, request.sampleRate, request);
}

/**
 * Render edited percussive events back over their source audio.
 *
 * Per event the lifted signal is the percussive component over
 * `[onsetSample, offsetSample)` under the tail fade. It is subtracted where it
 * sits and, unless the event is muted, added back at the shifted position scaled
 * by the gain. Only that signal moves, so muting a hit leaves the harmonic
 * content under it sounding and moving one does not drag its neighbours' sustain
 * along.
 *
 * Each event's span and `edit` are read; `strength`, `peakAmplitude` and
 * `percussiveRatio` are ignored, so an extracted event can be passed back as-is,
 * or an event can be built by hand from the span alone. A set whose edits are all
 * identity reproduces the input bit for bit and runs no separation at all.
 *
 * Pass the separation the events were extracted with: a different one lifts a
 * different signal out of the span than the one the events describe. It is
 * validated even when every edit is the identity and no separation runs, so an
 * unusable framing is an error on every set rather than on some of them.
 *
 * Overlap is checked on the source spans only. Where `timeOffsetSamples` lands an
 * event is not, so two moved events may be written over each other, and a shift
 * that pushes the signal past either end is truncated there rather than wrapped.
 *
 * @param request - Source audio, the events to render, the separation and the
 *   tail fade
 * @returns The rendered audio, the same length and sample rate as the input
 * @throws RangeError when the samples or sample rate fail the shared input checks
 * @throws SonareError (`InvalidParameter`) on an event whose span is empty,
 *   reversed or outside the audio, overlapping source spans, a non-finite
 *   `gainDb`, a kernel size that is not an integer within the 32-bit range, a
 *   framing that breaks constant overlap-add, or a negative or non-finite
 *   `fadeMs`
 *
 * @example
 * ```ts
 * const events = extractPercussiveEvents({ samples, sampleRate });
 *
 * // Lift the loudest hit by 3 dB and leave the rest untouched.
 * const loudest = events.reduce((a, b) => (a.strength >= b.strength ? a : b));
 * const edited = events.map((event) =>
 *   event === loudest ? { ...event, edit: { ...event.edit, gainDb: 3 } } : event,
 * );
 * const rendered = renderPercussiveEvents({ samples, sampleRate, events: edited });
 * ```
 */
export function renderPercussiveEvents(request: RenderPercussiveEventsRequest): Float32Array {
  assertSamples('renderPercussiveEvents', request.samples, request.validate !== false);
  assertSampleRate('renderPercussiveEvents', request.sampleRate);
  assertPercussiveSeparation('renderPercussiveEvents', request);
  return requireModule().renderPercussiveEvents(
    request.samples,
    request.sampleRate,
    request.events,
    request,
  );
}

/**
 * Normalize audio to a target peak or RMS level.
 *
 * @param samples - Audio samples (mono, float32)
 * @param sampleRate - Sample rate in Hz (default: 22050)
 * @param targetDb - Finite target at or below 0 dBFS (default: 0 dB = full scale).
 *   For `mode: 'peak'`, this is the peak target; for `mode: 'rms'`, this is the RMS target.
 * @param mode - Normalization mode: `'peak'` (default) or `'rms'`.
 * @returns Normalized audio
 */
export function normalize(request: NormalizeRequest): Float32Array;
export function normalize(
  samples: Float32Array,
  sampleRate: number,
  targetDb?: number,
  options?: ValidateOptions,
): Float32Array;
export function normalize(
  samples: Float32Array,
  sampleRate: number,
  targetDb?: number,
  mode?: NormalizeMode,
  options?: ValidateOptions,
): Float32Array;
export function normalize(
  samples: Float32Array | NormalizeRequest,
  sampleRate?: number,
  targetDb = 0.0,
  modeOrOptions: NormalizeMode | ValidateOptions = 'peak',
  options: ValidateOptions = {},
): Float32Array {
  if (
    modeOrOptions !== undefined &&
    modeOrOptions !== null &&
    typeof modeOrOptions !== 'string' &&
    typeof modeOrOptions !== 'object'
  ) {
    throw new TypeError("normalize: mode must be the string 'peak' or 'rms'");
  }
  if (modeOrOptions === null) {
    throw new TypeError("normalize: mode must be the string 'peak' or 'rms'");
  }
  const positionalOptions =
    typeof modeOrOptions === 'object' && modeOrOptions !== null ? modeOrOptions : options;
  const positionalMode = typeof modeOrOptions === 'string' ? modeOrOptions : undefined;
  const request: NormalizeRequest =
    samples instanceof Float32Array
      ? { samples, sampleRate, targetDb, mode: positionalMode, ...positionalOptions }
      : samples;
  assertSamples('normalize', request.samples, request.validate !== false);
  const mode = resolveNormalizeMode(request.mode);
  return requireModule().normalizeEx(
    request.samples,
    request.sampleRate ?? 22050,
    request.targetDb ?? 0.0,
    mode,
  );
}

/**
 * Apply region-based spectral edits (gain/attenuate/mute/heal) to mono audio.
 *
 * Each op is a time x frequency rectangle applied in array order over a single
 * STFT buffer, so a later op observes the result of earlier ops. The output has
 * the same length and sample rate as the input; an empty `ops` list is an
 * identity transform (within the iSTFT's own tolerance).
 *
 * @param samples - Audio samples (mono, float32)
 * @param sampleRate - Sample rate in Hz
 * @param ops - Region edit ops applied in order ({@link SpectralRegionOp})
 * @param options - STFT + heal configuration ({@link SpectralEditOptions})
 * @returns Edited audio
 */
export function spectralEdit(request: SpectralEditRequest): Float32Array;
export function spectralEdit(
  samples: Float32Array,
  sampleRate: number,
  ops?: SpectralRegionOp[],
  options?: SpectralEditOptions & ValidateOptions,
): Float32Array;
export function spectralEdit(
  samples: Float32Array | SpectralEditRequest,
  sampleRate?: number,
  ops: SpectralRegionOp[] = [],
  options: SpectralEditOptions & ValidateOptions = {},
): Float32Array {
  const request: SpectralEditRequest =
    samples instanceof Float32Array
      ? { samples, sampleRate: sampleRate as number, ops, ...options }
      : samples;
  assertSamples('spectralEdit', request.samples, request.validate !== false);
  assertSampleRate('spectralEdit', request.sampleRate);
  return requireModule().spectralEdit(
    request.samples,
    request.sampleRate,
    request.ops ?? [],
    request as unknown as Record<string, unknown>,
  );
}
