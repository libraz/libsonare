/**
 * Note-level editing: segmenting audio and a pitch track into editable notes,
 * rendering an edited set back, and the split/merge operations over it.
 */

import type { EffectSamplesRequest } from './_effects_common.js';
import {
  assertEditTimeOffsets,
  assertPitchTrackLengths,
  toVoicedInt32,
} from './_effects_common.js';
import { addon } from './native.js';
import type {
  AssignNoteTargetsResult,
  NoteExtractorOptions,
  NoteMoveOptions,
  NoteObject,
  NoteObjectInput,
  NoteSetEntry,
  NoteStretchOptions,
  NoteTarget,
  NoteTargetUnmatchedPolicy,
  PitchDecompositionResult,
  VoicedFlags,
} from './types.js';
import { assertFiniteScalar, assertIntegerValue, assertSampleRate } from './validation.js';

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

export interface NoteTargetsFromSmfRequest {
  /** The Standard MIDI File's bytes. */
  data: Uint8Array;
  /**
   * Index into the tracks that carried MIDI events, NOT the file's own track
   * numbering: a track holding only meta events — a conductor track carrying the
   * tempo map is the usual one — is not counted. A file whose first track is a
   * conductor track therefore has its melody at index 0. Default 0.
   */
  trackIndex?: number;
}

export interface AssignNoteTargetsRequest {
  /**
   * The notes to assign to. Their sample bounds, `medianHz` and `edit` are read;
   * the array itself is not modified.
   */
  notes: readonly NoteObject[];
  /** Sample rate in Hz, which converts each note's sample span to seconds. */
  sampleRate: number;
  /** The reference melody, as {@link noteTargetsFromSmf} returns it. */
  targets: readonly NoteTarget[];
  /** What to do with a note that has a pitch and no target. Default `'leave'`. */
  unmatchedPolicy?: NoteTargetUnmatchedPolicy;
  /**
   * Fraction of the note that must overlap a target for it to count. Default
   * 0.5; 0 is its own meaning — any overlap at all counts — not a request for
   * the default.
   */
  minOverlapRatio?: number;
  /**
   * The assigned shift saturates here rather than being refused: a reference an
   * octave out is a wrong reference, and a rejected call says less than a bounded
   * correction does. Default 12; 0 is its own meaning, as above.
   */
  maxCorrectionSemitones?: number;
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
    assertIntegerValue(fnName, note.frameStart, `notes[${index}].frameStart`);
    assertIntegerValue(fnName, note.frameEnd, `notes[${index}].frameEnd`);
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
 * Read one track of an in-memory Standard MIDI File as a reference melody.
 *
 * Each note-on is paired with the next note-off of the same note number on the
 * same channel, and the pair becomes one target at the note's own pitch. Times
 * follow the file's tempo map, so a tempo change or a ramp inside it is honoured
 * rather than the initial tempo being scaled over the whole file.
 *
 * A note-on the track never closes is dropped — it has no end, and the track's
 * end is not a substitute for one. Zero-length notes are skipped: they overlap
 * nothing, so they could never be assigned.
 *
 * @param request - The file's bytes and which of its MIDI-carrying tracks to read.
 * @returns One {@link NoteTarget} per closed note, sorted by `startSec`. A track
 *   with no closed note comes back as an empty array rather than as an error.
 * @throws {TypeError} `data` is neither a `Buffer` nor a `Uint8Array`.
 * @throws {RangeError} `trackIndex` is not a whole number.
 * @throws {SonareError} `trackIndex` is out of range, or the bytes are not a
 *   readable Standard MIDI File.
 *
 * @example
 * ```ts
 * const targets = noteTargetsFromSmf({ data: await readFile('melody.mid') });
 * const notes = extractNotes({ samples, sampleRate, f0Hz, voiced, frameRate });
 * const { notes: tuned } = assignNoteTargets({ notes, sampleRate, targets });
 * const output = renderNotes({ samples, sampleRate, notes: tuned });
 * ```
 */
export function noteTargetsFromSmf(request: NoteTargetsFromSmfRequest): NoteTarget[] {
  return addon.noteTargetsFromSmf(request.data, request.trackIndex ?? 0);
}

/**
 * Write each note's `edit.pitchShiftSemitones` from the target it overlaps.
 *
 * A note is matched to the target it overlaps longest, provided that overlap is
 * at least `minOverlapRatio` of the note's own span; ties go to the target that
 * starts first. The shift is `targetMidi` minus the note's own `medianHz` as a
 * MIDI number, saturated at `maxCorrectionSemitones`.
 *
 * A note whose `medianHz` is not finite and positive is never assigned and never
 * edited, whatever `unmatchedPolicy` says: such a note has no measured pitch to
 * correct from, which is a different thing from having a pitch and no target.
 *
 * The returned notes are a new array — the C ABI edits in place, and a caller
 * handing in its own notes does not expect them rewritten. Every field but the
 * two the assignment writes is the caller's own, so the result goes straight to
 * {@link renderNotes}.
 *
 * @param request - The notes, their sample rate, the reference melody, and the
 *   optional matching tuning.
 * @returns The new notes and how many of them got a target.
 * @throws {TypeError} `notes` or `targets` is not an array, an entry of either is
 *   not a plain object, a note is missing `onsetSample` / `offsetSample`, a
 *   target is missing one of its three fields, or `unmatchedPolicy` is not a
 *   string.
 * @throws {RangeError} `sampleRate` is out of the supported range, or
 *   `unmatchedPolicy` is not one of the three spellings.
 * @throws {SonareError} `minOverlapRatio` is outside `[0, 1]`,
 *   `maxCorrectionSemitones` is negative, or a target carries a non-finite value.
 *
 * @example
 * ```ts
 * // Mute whatever the reference does not cover, and cap corrections at a fifth.
 * const { notes: tuned, assignedCount } = assignNoteTargets({
 *   notes,
 *   sampleRate,
 *   targets,
 *   unmatchedPolicy: 'mute',
 *   maxCorrectionSemitones: 7,
 * });
 * if (assignedCount === 0) throw new Error('the reference does not line up with the take');
 * ```
 */
export function assignNoteTargets(request: AssignNoteTargetsRequest): AssignNoteTargetsResult {
  const { notes, sampleRate, targets, ...options } = request;
  assertSampleRate('assignNoteTargets', sampleRate);
  if (!Array.isArray(notes)) {
    throw new TypeError('assignNoteTargets: notes must be an array');
  }
  if (!Array.isArray(targets)) {
    throw new TypeError('assignNoteTargets: targets must be an array');
  }
  const assigned = addon.assignNoteTargets(notes, sampleRate, targets, options) as {
    edits: ReadonlyArray<{ pitchShiftSemitones: number; muted: boolean }>;
    assignedCount: number;
  };
  return {
    // The addon answers only the two fields the assignment writes, so the rest of
    // each note travels through here rather than through the C ABI.
    notes: notes.map((note, index) => ({
      ...note,
      edit: { ...note.edit, ...assigned.edits[index] },
    })),
    assignedCount: assigned.assignedCount,
  };
}
