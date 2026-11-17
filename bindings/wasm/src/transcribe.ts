import { projectModule } from './project_internal.js';
import type { TranscribeOptions, TranscribeResult } from './project_types.js';
import type { ValidateOptions } from './validation.js';
import {
  assertAudioInput,
  assertFloatValue,
  assertFrequencyBounds,
  assertPositiveFloatValue,
  sampleOneShot,
} from './validation.js';

/**
 * Canonical request form for {@link transcribe}.
 *
 * `validate: false` skips only the facade's NaN/Inf pre-scan; the transcription
 * C ABI (`sonare_transcribe`) re-validates the buffer either way, with its own
 * wording. Absent, the pre-scan runs.
 */
export interface TranscribeRequest extends TranscribeOptions, ValidateOptions {
  /** Mono source audio. Must be non-empty and all-finite. */
  samples: Float32Array;
  /** Sample rate of `samples` in Hz, `[8000, 384000]`. */
  sampleRate: number;
  /**
   * Tempo the PPQ grid is built on, in BPM. **Omit to have it detected** from
   * `samples`, which costs an onset/tempo pass; a detector that finds nothing
   * usable falls back to 120 rather than failing the transcription. A value
   * `<= 0` is a `RangeError`.
   *
   * Supplying a tempo is not a claim about the audio — it is the coordinate
   * system the events come back in. Twice the tempo is twice as many beats per
   * second, so the same audio lands on twice the ppq. The one setting that
   * reads it is `minNoteDivision`; with a grid tempo that is not the music's,
   * give `minNoteMs` instead.
   */
  tempoBpm?: number;
}

/**
 * Checks the ratio fields and the F0 bounds, and maps the bounds to the native
 * `fmin` / `fmax`; every other key passes through to the native option reader.
 */
export function nativeTranscribeOptions(
  fnName: string,
  options: TranscribeOptions & Record<string, unknown>,
): TranscribeOptions {
  const { f0MinHz, f0MaxHz, ...rest } = options;
  for (const key of ['minFramePeakRatio', 'minRidgePeakRatio'] as const) {
    const value = rest[key];
    if (value !== undefined && value !== null && typeof value === 'number' && value < 0) {
      throw new RangeError(`${fnName}: ${key} must not be negative`);
    }
  }
  assertFrequencyBounds(fnName, f0MinHz, f0MaxHz, 'f0MinHz', 'f0MaxHz');
  const native: Record<string, unknown> = { ...rest };
  if (f0MinHz !== undefined && f0MinHz !== null) {
    native.fmin = f0MinHz;
  }
  if (f0MaxHz !== undefined && f0MaxHz !== null) {
    native.fmax = f0MaxHz;
  }
  const reattack = rest.reattackRatio;
  if (reattack === undefined || reattack === null) {
    // Native reads an absent ratio as its default split; a written 0 is "off".
    if (rest.polyphonic === true) {
      native.reattackRatio = 0;
    } else {
      delete native.reattackRatio;
    }
  } else {
    assertFloatValue(fnName, reattack, 'reattackRatio');
    if ((reattack as number) <= 1) {
      throw new RangeError(
        `${fnName}: reattackRatio must be greater than 1 (omit it for no split)`,
      );
    }
  }
  return native as unknown as TranscribeOptions;
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
 * @throws {TypeError} when `sampleRate` is missing or not a number
 * @throws {RangeError} also on an option outside its domain that the reader
 *   refuses — a non-negative `velocityFloorDb`, a `fixedVelocity` outside
 *   `[1, 127]`, a written `0` on any field but `group`, `channel`, `minFramePeakRatio` and
 *   `minRidgePeakRatio`, or a negative ratio
 * @throws {RangeError} on a `group` or `channel` outside
 *   `[0, 15]` or an `f0MaxHz` at or below `f0MinHz`, which the C ABI refuses
 * @throws {SonareError} `NotSupported` when the library was built without the pitch editor
 *
 * @example
 * ```typescript
 * const { events, noteCount, tempoBpm } = transcribe({ samples, sampleRate, tempoBpm: 120 });
 * const project = new Project();
 * const { clipId } = project.addMidiClip(0, 16); // 4 bars at 4/4, in quarter notes (PPQ)
 * project.setMidiEvents(clipId, events);
 * ```
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
  const { samples, sampleRate, tempoBpm, validate, ...rest } = request;
  if (tempoBpm !== undefined && tempoBpm !== null) {
    assertPositiveFloatValue('transcribe', tempoBpm, 'tempoBpm');
  }
  const nativeOptions = nativeTranscribeOptions('transcribe', rest);
  return projectModule().transcribe(samples, sampleRate, tempoBpm, nativeOptions);
}
