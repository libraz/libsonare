import { nativeTranscribeRequest } from './internal/transcribe_request.js';
import { addon } from './native.js';
import type { TranscribeRequest, TranscribeResult } from './types.js';
import type { OptionsOf } from './validation.js';
import { assertAudioInput, assertPositiveFloatValue, sampleOneShot } from './validation.js';

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
 * quantizing, tempo-map installation and key/chord annotation each already live
 * somewhere else. The tuning reference is `referenceHz`, or measured with
 * `referenceHz: 'auto'`; the result reports it as `tuningCents`.
 *
 * @example
 * ```typescript
 * const { events, noteCount, tempoBpm } = transcribe({
 *   samples,
 *   sampleRate: 48000,
 *   tempoBpm: 120,
 *   fixedVelocity: 100,
 * });
 * const clipId = 0; // a MIDI clip of `project`
 * project.setMidiEvents(clipId, events);
 * ```
 *
 * @throws `RangeError` when `samples` is empty or `sampleRate` is out of range,
 *         `TypeError` when an option carries the wrong type, and a
 *         `SonareError` when the library was built without the pitch editor.
 */
export function transcribe(request: TranscribeRequest): TranscribeResult;
export function transcribe(
  samples: Float32Array,
  sampleRate: number,
  options?: OptionsOf<TranscribeRequest>,
): TranscribeResult;
export function transcribe(...args: unknown[]): TranscribeResult {
  const request = sampleOneShot<TranscribeRequest>('transcribe', args);
  assertAudioInput('transcribe', request.samples, request.sampleRate, request);
  const { ...rest } = request;
  if (rest.tempoBpm !== undefined && rest.tempoBpm !== null) {
    assertPositiveFloatValue('transcribe', rest.tempoBpm, 'tempoBpm');
  }
  return addon.transcribe(nativeTranscribeRequest('transcribe', rest));
}
