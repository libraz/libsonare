import { projectModule } from './project_internal.js';
import type { TranscribeOptions, TranscribeResult } from './project_types.js';
import { assertSampleRate, assertSamples } from './validation.js';

/**
 * Canonical request form for {@link transcribe}.
 *
 * Deliberately does NOT extend `ValidateOptions`. That option's contract is
 * that skipping the JS-side scan is safe because the native layer re-validates
 * with an equivalent result -- and here it would not be equivalent: the
 * transcription C ABI (`sonare_transcribe`) does re-validate the buffer,
 * including a non-finite scan, but a rejection there surfaces as a
 * `SonareError` `InvalidParameter`, not the `RangeError` this function
 * documents and every other empty/non-finite/rate check on this surface
 * raises. A `{ validate: false }` here would silently change the thrown error
 * class instead of skipping a redundant check. The scan is also nearly free
 * against this pipeline, which already reads every sample several times.
 */
export interface TranscribeRequest extends TranscribeOptions {
  /** Mono source audio. Must be non-empty and all-finite. */
  samples: Float32Array;
  /** Sample rate of `samples` in Hz, `[8000, 384000]`. */
  sampleRate: number;
  /**
   * Tempo the PPQ grid is built on, in BPM. **Omit to have it detected** from
   * `samples`, which costs an onset/tempo pass; a detector that finds nothing
   * usable falls back to 120 rather than failing the transcription.
   *
   * Supplying a tempo is not a claim about the audio — it is the coordinate
   * system the events come back in. Twice the tempo is twice as many beats per
   * second, so the same audio lands on twice the ppq.
   */
  tempoBpm?: number;
}

/**
 * Transcribes mono audio into MIDI events on a constant-tempo grid.
 *
 * This joins parts that already exist rather than adding a detector: the note
 * spans and their measured pitch and level come from the monophonic (pYIN) or
 * polyphonic (multi-F0) chain, and the output is the flat
 * {@link ProjectMidiEvent} shape {@link Project.setMidiEvents} takes, so
 * nothing is left to convert.
 *
 * Finding no notes is not an error. Silence, and material the chain cannot
 * resolve, come back with an empty `events` array and `noteCount` 0, and
 * `tempoBpm` still reports the tempo that was used or detected.
 *
 * Two things are deliberately **not** done here, because the library already
 * does each of them somewhere else and a second implementation would drift:
 *
 * - **Quantizing to a grid** — {@link Project.bakeMidiFx}'s `quantizePpq` /
 *   `quantizeStrength`.
 * - **Detecting and installing a tempo map** — {@link Project.autoTempo}. The
 *   `tempoBpm` fallback here builds one constant-tempo grid for this call and
 *   installs nothing; use {@link Project.transcribeToClip} to transcribe onto a
 *   project's real map.
 * - **Annotating key and chords** — {@link Project.annotateKeys} /
 *   {@link Project.annotateChords}.
 *
 * The tuning reference is `referenceHz`, or measured from the audio with
 * `referenceHz: 'auto'`; the result reports it as `tuning` — see
 * {@link TranscribeOptions.referenceHz}.
 *
 * @throws {RangeError} on empty `samples`, a non-finite sample, or a
 *   `sampleRate` outside `[8000, 384000]`
 * @throws {SonareError} `InvalidParameter` on an option outside its domain —
 *   a non-negative `velocityFloorDb`, a `fixedVelocity` outside `[1, 127]`, a
 *   `group` or `channel` outside `[0, 15]`, an `fmax` at or below `fmin`, or a
 *   written `0` on any field but `group` and `channel` — or `NotSupported`
 *   when the library was built without the pitch editor
 *
 * @example
 * ```typescript
 * const { events, noteCount, tempoBpm } = transcribe({ samples, sampleRate, tempoBpm: 120 });
 * const project = new Project();
 * const { clipId } = project.addMidiClip(0, 16); // 4 bars at 4/4, in quarter notes (PPQ)
 * project.setMidiEvents(clipId, events);
 * ```
 */
export function transcribe(request: TranscribeRequest): TranscribeResult {
  assertSamples('transcribe', request.samples, true);
  assertSampleRate('transcribe', request.sampleRate);
  return projectModule().transcribe(
    request.samples,
    request.sampleRate,
    request.tempoBpm,
    request as unknown as TranscribeOptions,
  );
}
