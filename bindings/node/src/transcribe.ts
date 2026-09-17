import { addon } from './native.js';
import type { TranscribeRequest, TranscribeResult } from './types.js';
import { assertSampleRate, assertSamples } from './validation.js';

/**
 * Transcribe mono audio into MIDI note events on a constant-tempo grid.
 *
 * A bridge rather than a detector: the note spans and their measured pitch and
 * level come from the monophonic (pYIN) and polyphonic chains, the tempo is
 * either supplied or detected, and the output is the same flat
 * {@link ProjectMidiEvent} shape {@link Project.setMidiEvents} takes, so
 * nothing is left to convert.
 *
 * Finding no notes is not an error: silence, and material the chain cannot
 * resolve, come back with an empty `events` array and `noteCount` 0, and
 * `tempoBpm` still reports the tempo that was used or detected.
 *
 * See {@link TranscribeOptions} for what this deliberately does not do —
 * quantizing, tempo-map installation, key/chord annotation and tuning-reference
 * measurement each already live somewhere else.
 *
 * @example
 * ```typescript
 * const { events, noteCount, tempoBpm } = transcribe({
 *   samples: audio,
 *   sampleRate: 48000,
 *   tempoBpm: 120,
 *   fixedVelocity: 100,
 * });
 * project.setMidiEvents(clipId, events);
 * ```
 *
 * @throws `RangeError` when `samples` is empty or `sampleRate` is out of range,
 *         `TypeError` when an option carries the wrong type, and a
 *         `SonareError` when the library was built without the pitch editor.
 */
export function transcribe(request: TranscribeRequest): TranscribeResult {
  assertSamples('transcribe', request.samples, true);
  assertSampleRate('transcribe', request.sampleRate);
  return addon.transcribe(request);
}
