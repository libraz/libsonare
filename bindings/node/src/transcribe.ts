import { addon } from './native.js';
import type { TranscribeRequest, TranscribeResult } from './types.js';
import {
  assertAudioInput,
  assertFloatValue,
  assertFrequencyBounds,
  assertPositiveFloatValue,
  sampleOneShot,
} from './validation.js';

/** The request fields the facade checks and renames before the addon reads them. */
interface TranscribeNativeFields {
  f0MinHz?: number;
  f0MaxHz?: number;
  polyphonic?: boolean;
  reattackRatio?: number;
}

/**
 * Checks the F0 bounds and `reattackRatio`, and spells them as the addon reads
 * them: `fmin` / `fmax`, and an absent polyphonic ratio as 0 (no split).
 */
export function nativeTranscribeRequest(
  fnName: string,
  request: TranscribeNativeFields,
): Record<string, unknown> {
  const { f0MinHz, f0MaxHz, ...rest } = request;
  assertFrequencyBounds(fnName, f0MinHz, f0MaxHz, 'f0MinHz', 'f0MaxHz');
  const native: Record<string, unknown> = { ...rest, fmin: f0MinHz, fmax: f0MaxHz };
  const { reattackRatio } = request;
  if (reattackRatio === undefined || reattackRatio === null) {
    // The addon reads an absent ratio as its default split and a written 0 as "off".
    if (request.polyphonic === true) {
      native.reattackRatio = 0;
    } else {
      delete native.reattackRatio;
    }
  } else {
    assertFloatValue(fnName, reattackRatio, 'reattackRatio');
    if (reattackRatio <= 1) {
      throw new RangeError(
        `${fnName}: reattackRatio must be greater than 1 (omit it for no split)`,
      );
    }
  }
  return native;
}

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
 * `referenceHz: 'auto'`; the result reports it as `tuning`.
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
export function transcribe(request: TranscribeRequest): TranscribeResult;
export function transcribe(
  samples: Float32Array,
  sampleRate: number,
  options?: Omit<TranscribeRequest, 'samples' | 'sampleRate'>,
): TranscribeResult;
export function transcribe(...args: unknown[]): TranscribeResult {
  const request = sampleOneShot<TranscribeRequest>('transcribe', args);
  assertAudioInput('transcribe', request.samples, request.sampleRate, request);
  const { validate: _validate, ...rest } = request;
  if (rest.tempoBpm !== undefined && rest.tempoBpm !== null) {
    assertPositiveFloatValue('transcribe', rest.tempoBpm, 'tempoBpm');
  }
  return addon.transcribe(nativeTranscribeRequest('transcribe', rest));
}
