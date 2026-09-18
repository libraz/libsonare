/**
 * Note-level editing: segmenting audio and a pitch track into editable notes,
 * rendering an edited set back, and the split/merge operations over it.
 */

import { toVoicedFloat32 } from './_effects_common';
import { getSonareModule } from './module_state';
import type {
  NoteExtractorOptions,
  NoteMoveOptions,
  NoteObject,
  NoteObjectInput,
  NoteSetEntry,
  NoteStretchOptions,
  NoteTarget,
  NoteTargetAssignResult,
  NoteTargetUnmatchedPolicy,
  PitchDecompositionResult,
  VoicedFlags,
} from './public_types';
import type { ValidateOptions } from './validation';
import { assertSampleRate, assertSamples } from './validation';

function requireModule() {
  return getSonareModule();
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

/** Canonical request form for {@link noteTargetsFromSmf}. */
export interface NoteTargetsFromSmfRequest {
  /** The Standard MIDI File, in memory. */
  data: Uint8Array;
  /**
   * Which MIDI-bearing track to read, NOT an index into the file's own tracks: a
   * track holding only meta events — a conductor track carrying the tempo map is
   * the usual one — is not counted. A file whose first track is a conductor track
   * therefore has its melody at 0, which is also the default.
   */
  trackIndex?: number;
}

/** Canonical request form for {@link assignNoteTargets}. */
export interface AssignNoteTargetsRequest {
  /**
   * The notes to assign targets to. Not modified; the result carries a new array.
   * Only `onsetSample`, `offsetSample` and `medianHz` are read.
   */
  notes: readonly NoteObject[];
  /** Converts each note's sample span to seconds, so the targets line up. Must be > 0. */
  sampleRate: number;
  /** The reference melody. An empty array assigns nothing and applies the policy. */
  targets: readonly NoteTarget[];
  /** What to do with a note that has a pitch and no target. Default `'leave'`. */
  unmatchedPolicy?: NoteTargetUnmatchedPolicy;
  /**
   * Fraction of the note that must overlap a target for it to count, in `[0, 1]`.
   * Default 0.5. `0` is its own meaning — any overlap at all counts — not a
   * request for the default.
   */
  minOverlapRatio?: number;
  /**
   * Where the assigned shift saturates, in semitones. Default 12. `0` is its own
   * meaning, as above: every correction saturates to nothing and the take is left
   * as recorded. Must not be negative.
   */
  maxCorrectionSemitones?: number;
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
 * Read one track of an in-memory Standard MIDI File as a reference melody.
 *
 * Each note-on is paired with the next note-off of the same note number on the
 * same channel, and the pair becomes one {@link NoteTarget} at the note's own
 * pitch. Times come from the file's tempo map, so a tempo change or a ramp inside
 * it is followed rather than the initial tempo being scaled.
 *
 * A note-on the track never closes is dropped: it has no end, and the track's end
 * is not a substitute for one — with events after it the note would span the whole
 * remainder and, being the longest overlap everywhere, take the assignment away
 * from every note that follows. Zero-length notes are skipped for the mirror
 * reason: they overlap nothing, so they could never be assigned.
 *
 * @param request - The file's bytes and which MIDI-bearing track to read
 * @returns One target per closed note, sorted by `startSec`; an empty array for a
 *   track with no closed note, which is a measurement that came up empty rather
 *   than an error
 * @throws SonareError (`InvalidFormat`) when the bytes are not a readable SMF
 * @throws SonareError (`InvalidParameter`) when `trackIndex` names no
 *   MIDI-bearing track
 *
 * @example
 * ```ts
 * // A project's own export writes the tempo map as a conductor track, which
 * // carries no MIDI, so the melody is at index 0 rather than 1.
 * const targets = noteTargetsFromSmf({ data: project.exportSmf() });
 * ```
 */
export function noteTargetsFromSmf(request: NoteTargetsFromSmfRequest): NoteTarget[] {
  const module = requireModule();
  if (typeof module.noteTargetsFromSmf !== 'function') {
    throw new Error('libsonare was built without arrangement support');
  }
  return module.noteTargetsFromSmf(request.data, request.trackIndex ?? 0);
}

/**
 * Write each note's `edit.pitchShiftSemitones` from the reference target it
 * overlaps, which is what makes a take follow a written melody instead of a
 * single stated interval.
 *
 * A note is matched to the target it overlaps longest, provided that overlap is
 * at least `minOverlapRatio` of the note's own span; an exact tie goes to the
 * target that starts first, so the answer does not depend on the order the
 * targets arrived in. The shift is `targetMidi` minus the note's own `medianHz`
 * as a MIDI number, saturated at `maxCorrectionSemitones` — a reference an octave
 * out is a wrong reference, and a rejected call would tell the caller less than a
 * bounded correction does.
 *
 * A note whose `medianHz` is not finite and positive is never assigned and never
 * edited, whatever `unmatchedPolicy` says. Such a note has no measured pitch to
 * correct from, so `'nearest'` would compute a shift from a pitch that does not
 * exist; the policy governs notes that have a pitch and no target, which is a
 * different thing from having no pitch.
 *
 * The notes handed in are not modified. Only `edit.pitchShiftSemitones` and
 * `edit.muted` are written on the returned copies; every other field of a note —
 * its spans, its metrics, its amplitude curve and its other edits — comes back
 * exactly as it went in.
 *
 * @param request - The notes, their sample rate, the reference melody, and the
 *   three tunable knobs
 * @returns The new note set and how many notes received a target
 * @throws SonareError (`InvalidParameter`) on a non-positive `sampleRate`, an
 *   unknown `unmatchedPolicy`, a `minOverlapRatio` outside `[0, 1]`, a negative
 *   `maxCorrectionSemitones`, a note missing `onsetSample` / `offsetSample`, or a
 *   target whose `startSec`, `endSec` or `targetMidi` is not finite
 *
 * @example
 * ```ts
 * const notes = extractNotes({ samples, sampleRate, f0Hz, voiced, frameRate });
 * const { notes: retuned, assignedCount } = assignNoteTargets({
 *   notes,
 *   sampleRate,
 *   targets: noteTargetsFromSmf({ data: smf }),
 *   unmatchedPolicy: 'mute',
 * });
 * const corrected = renderNotes({ samples, sampleRate, notes: retuned });
 * ```
 */
export function assignNoteTargets(request: AssignNoteTargetsRequest): NoteTargetAssignResult {
  return requireModule().assignNoteTargets(
    request.notes,
    request.sampleRate,
    request.targets,
    request,
  );
}
