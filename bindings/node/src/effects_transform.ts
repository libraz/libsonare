import { resolveFftOptions } from './_fft_options.js';
import { addon } from './native.js';
import type {
  HpssResult,
  NoteExtractorOptions,
  NoteMoveOptions,
  NoteObject,
  NoteObjectInput,
  NoteStretchOptions,
  PitchCorrectOptions,
  SpectralEditOptions,
  SpectralRegionOp,
  VoicedFlags,
} from './types.js';
import { assertSampleRate } from './validation.js';

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
  kernelHarmonic?: number;
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

export interface ExtractNotesRequest extends EffectSamplesRequest, NoteExtractorOptions {
  /**
   * Sample rate in Hz. Required: `minNoteMs` and the per-frame RMS windows are
   * converted to samples with this rate, so a wrong/omitted value silently
   * segments differently.
   */
  sampleRate: number;
  /** Per-frame F0 in Hz; finite and non-negative, zero meaning unvoiced. */
  f0Hz: Float32Array;
  /** F0 frames per second. */
  frameRate: number;
  /** Per-frame voiced flags (truthy = voiced). Takes precedence over `voicedProb`. */
  voiced?: VoicedFlags;
  /** Per-frame voicing probability in `[0, 1]`; read only when `voiced` is omitted. */
  voicedProb?: Float32Array;
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
  return addon.hpss(
    request.samples,
    request.sampleRate ?? 22050,
    request.kernelHarmonic ?? 31,
    request.kernelPercussive ?? 31,
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
  if (typeof request.rate !== 'number' || !Number.isFinite(request.rate)) {
    throw new TypeError('timeStretch: rate must be a finite number');
  }
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
  if (typeof request.semitones !== 'number' || !Number.isFinite(request.semitones)) {
    throw new TypeError('pitchShift: semitones must be a finite number');
  }
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
 * The result has exactly the input length. The whole interval is applied
 * however large it is: both endpoints are validated to [0, 127], so a two-octave
 * move such as C3 -> C5 transposes by the full 24 semitones. Use
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
  return addon.pitchCorrectToMidiTimevarying(
    request.samples,
    request.sampleRate ?? 22050,
    request.f0Hz,
    request.targetMidi,
    request.hopLength ?? 512,
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
  return addon.pitchCorrectTimevarying(
    input,
    requestSampleRate ?? 22050,
    requestF0Hz,
    requestHopLength ?? 512,
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
 * @throws {TypeError} `frameRate` is not a finite number, or neither `voiced`
 *   nor `voicedProb` was given.
 * @throws {RangeError} `sampleRate` is out of the supported range, or `voiced` /
 *   `voicedProb` do not have the same length as `f0Hz`.
 *
 * @example
 * ```ts
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
  assertSampleRate('extractNotes', sampleRate);
  if (typeof frameRate !== 'number' || !Number.isFinite(frameRate)) {
    throw new TypeError('extractNotes: frameRate must be a finite number');
  }
  if (voiced === undefined && options.voicedProb === undefined) {
    throw new TypeError('extractNotes: one of voiced or voicedProb is required');
  }
  assertPitchTrackLengths(f0Hz, voiced, options.voicedProb);
  return addon.extractNotes(samples, sampleRate, f0Hz, frameRate, {
    ...options,
    voiced: voiced ? toVoicedInt32(voiced) : undefined,
  });
}

/**
 * Render edited note objects over their source audio.
 *
 * Only each note's `onsetSample`, `offsetSample` and `edit` are read, so the
 * notes {@link extractNotes} returned can be handed straight back. A note whose
 * edit is the identity is not resynthesized, so a set whose edits are all
 * identity reproduces the input bit for bit. The result has the input's length.
 *
 * Overlap is checked on the source spans only. Where `timeOffsetSamples` lands a
 * note is not, and a note lengthened past its own span writes into its
 * neighbours' samples, so two moved or stretched notes may overwrite each other.
 *
 * @param request - Audio, its sample rate, the notes to render, and the
 *   optional edge cross-fade.
 * @returns The rendered audio, the same length as `samples`.
 * @throws {TypeError} `notes` is not an array.
 * @throws {RangeError} `sampleRate` is out of the supported range.
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
 * ```
 */
export function renderNotes(request: RenderNotesRequest): Float32Array {
  const { samples, sampleRate, notes, ...options } = request;
  assertSampleRate('renderNotes', sampleRate);
  if (!Array.isArray(notes)) {
    throw new TypeError('renderNotes: notes must be an array');
  }
  return addon.renderNotes(samples, sampleRate, notes, options);
}
