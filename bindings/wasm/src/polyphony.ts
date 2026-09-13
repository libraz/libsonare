import { ErrorCode, SonareError } from './errors';
import { getSonareModule } from './module_state';
import type {
  NoteEditInput,
  NoteObject,
  PolyphonicAnalysisOptions,
  PolyphonicRenderOptions,
} from './public_types';
import type { WasmPolyphonicAnalysis } from './sonare.js';
import type { ValidateOptions } from './validation';
import { assertSampleRate, assertSamples } from './validation';

/** Canonical request form for {@link analyzePolyphonic}. */
export interface AnalyzePolyphonicRequest extends PolyphonicAnalysisOptions, ValidateOptions {
  samples: Float32Array;
  /**
   * Sample rate in Hz. Required: every duration in the chain — the ridge
   * minimum, the note minimum, the per-frame windows — is converted to samples
   * with this rate, so a wrong value silently analyses differently.
   */
  sampleRate: number;
}

/**
 * A polyphonic analysis, held as a handle so one note of a chord can be edited
 * and the result re-rendered without analysing the audio again.
 *
 * Created by {@link analyzePolyphonic}. **Release it with {@link destroy} as soon
 * as you are done with it**: the handle owns the source's complex spectrogram plus
 * the claimed bins of every note, which is the input over again plus the claims.
 * That is the price of re-rendering an edit for free, and it is why this is a
 * handle and not a result object — the measurement never crosses into JS.
 *
 * What crosses is what a host acts on: the notes ({@link notes}), each note's
 * pending edit ({@link setNoteEdit}), the per-frame voice count
 * ({@link polyphony}), and per note a pitch, a level and a salience curve. The
 * spectrogram, the masks and the per-bin weights do not, and no method reports a
 * per-bin figure.
 *
 * Using the analysis after it has been released throws `InvalidState` rather than
 * reaching a freed native object.
 */
export class PolyphonicAnalysis {
  private native: WasmPolyphonicAnalysis | null;

  /** Analyses the request's audio. {@link analyzePolyphonic} is the same call. */
  constructor(request: AnalyzePolyphonicRequest) {
    assertSamples('analyzePolyphonic', request.samples, request.validate !== false);
    assertSampleRate('analyzePolyphonic', request.sampleRate);
    const module = getSonareModule();
    this.native = module.createPolyphonicAnalysis(
      request.samples,
      request.sampleRate,
      request as unknown as Record<string, unknown>,
    );
  }

  private handle(): WasmPolyphonicAnalysis {
    if (this.native === null) {
      throw new SonareError(
        ErrorCode.InvalidState,
        'InvalidState',
        'PolyphonicAnalysis has been released',
      );
    }
    return this.native;
  }

  /** Number of notes, which is also the number of claim sets. */
  get noteCount(): number {
    return this.handle().noteCount;
  }

  /** Number of STFT frames the analysis ran over. */
  get frameCount(): number {
    return this.handle().frameCount;
  }

  /**
   * Every note, in the order their claim sets are held in — the same
   * {@link NoteObject} shape `extractNotes` returns, so a host that edits through
   * both doors sees one note.
   *
   * Each note carries its sample span, its frame span (in the analysis's own
   * framing), its median pitch, its steadiness, its per-frame `amplitude` and its
   * pending edit. The curves a note does not carry inline have their own accessors:
   * {@link noteF0}, {@link noteSalience}, and {@link noteEnvelope} for the points
   * last set through {@link setNoteEdit}. `amplitude` is {@link noteAmplitude}'s
   * curve, read once per note.
   */
  notes(): NoteObject[] {
    return this.handle().notes();
  }

  /**
   * Replaces one note's pending edit.
   *
   * The only thing a host writes. Everything else on a note is a measurement, and
   * the order is the pairing with the claim sets, so neither is settable.
   *
   * An omitted field is the identity, so `{}` restores the identity edit. The
   * envelope is `edit.amplitudeEnvelope`, which the handle copies, and its points
   * are per-frame linear gains over the note's span on top of `gainDb` — stretched
   * over whatever length the note renders at, so one entry is a constant gain and
   * the count need not match the note's frame count. Every value must be finite
   * and non-negative, which {@link render} is where it is checked, so one refusal
   * names one place.
   *
   * @param note - Index below {@link noteCount}
   * @throws {SonareError} `InvalidParameter` when `note` is out of range
   */
  setNoteEdit(note: number, edit: NoteEditInput): void {
    this.handle().setNoteEdit(note, edit);
  }

  /**
   * Voices estimated per frame, before tracking dropped anything — one entry per
   * frame from frame 0.
   *
   * What the estimation saw rather than what survived: a frame reported as three
   * voices with two notes spanning it is the difference between the two stages,
   * which is the figure a host deciding what to edit wants.
   */
  polyphony(): Int32Array {
    return this.handle().polyphony();
  }

  /**
   * One note's F0 in Hz, per frame over its own span.
   *
   * `frameEnd - frameStart` entries, so the value at index `i` belongs to frame
   * `frameStart + i`. This is the curve the monophonic door makes a caller pass
   * back in; here the handle already holds it, so a curve edit needs nothing from
   * the caller.
   *
   * @throws {SonareError} `InvalidParameter` when `note` is out of range
   */
  noteF0(note: number): Float32Array {
    return this.handle().noteF0(note);
  }

  /** One note's linear RMS, per frame over its own span. Indexed as {@link noteF0}. */
  noteAmplitude(note: number): Float32Array {
    return this.handle().noteAmplitude(note);
  }

  /**
   * One note's salience, per frame over its own span. Indexed as {@link noteF0},
   * and the one curve here that is not the note's own: it is the tracked ridge's,
   * so a frame of the note the ridge does not reach reads 0.
   *
   * Salience is what the estimation scored the candidate at, so it says how well
   * the material supported this note rather than how loud the note is —
   * {@link noteAmplitude} is the loud.
   */
  noteSalience(note: number): Float32Array {
    return this.handle().noteSalience(note);
  }

  /**
   * One note's amplitude envelope points, as last set — the same array
   * `notes()[note].edit.amplitudeEnvelope` carries.
   *
   * Indexed from 0 rather than over the note's span: an envelope is a set of gain
   * points stretched over whatever length the note renders at, not a per-frame
   * signal. The only one of the four curves that is not a measurement, and empty on
   * a note carrying no envelope.
   */
  noteEnvelope(note: number): Float32Array {
    return this.handle().noteEnvelope(note);
  }

  /**
   * Renders the analysis back to audio with whatever edits its notes carry, at the
   * source's length.
   *
   * Each note's claimed share is inverted, edited, and added to the residual — the
   * part of the input no note claimed. With every edit identity the result is the
   * analysis's own round trip, not the source bit for bit, the STFT round trip's
   * error being neither added to nor removed here.
   *
   * The render is additive per note with no cross-note term, so an unedited note's
   * contribution is identical between two renders. That is also the limit: a host
   * cannot tell from two renders whether a claim set divided the energy correctly.
   *
   * @throws {SonareError} `InvalidParameter` on an option or an edit field the
   *   renderer rejects — a non-positive stretch ratio, a negative or non-finite
   *   envelope point, or a vibrato or drift edit on a note carrying no usable
   *   pitch curve
   */
  render(options: PolyphonicRenderOptions = {}): Float32Array {
    return this.handle().render(options as Record<string, unknown>);
  }

  /**
   * Releases the underlying WASM object and everything it holds. A second call
   * throws `InvalidState` rather than freeing twice.
   */
  delete(): void {
    const native = this.handle();
    this.native = null;
    native.delete();
  }

  /** Alias for {@link delete}, provided for cross-binding (Node) compatibility. */
  destroy(): void {
    this.delete();
  }
}

/**
 * Analyses audio into editable notes and returns a handle to the analysis.
 *
 * One pass: one STFT, the multi-F0 extraction over it, a claim set per tracked
 * ridge, the apportionment of the bins two notes stand on, and the measured fields
 * of each note. Every note comes back with the identity edit, so rendering the
 * result unchanged reproduces the analysis's own round trip.
 *
 * An analysis finding no notes is not an error. Silence, or material the register
 * of the framing cannot resolve, tracks no ridge; rendering that is the residual
 * alone, which is the whole round trip.
 *
 * **This is for spans, not for whole songs.** The handle holds the source's
 * complex spectrogram plus every note's claimed bins — roughly 1,440 MiB for five
 * minutes of audio against a 2 GiB linear-memory cap — so analyse the passage you
 * are editing and {@link PolyphonicAnalysis.destroy} it when done.
 *
 * @throws {RangeError} on empty samples, a non-finite sample, or a `sampleRate`
 *   outside `[8000, 384000]`
 * @throws {SonareError} `InvalidParameter` on a config value the chain rejects
 *
 * @example
 * ```typescript
 * const analysis = analyzePolyphonic({ samples, sampleRate, maxPolyphony: 3 });
 * try {
 *   // Transpose the lowest note of the chord up a semitone.
 *   const notes = analysis.notes();
 *   const lowest = notes.reduce((a, b) => (a.medianHz <= b.medianHz ? a : b));
 *   analysis.setNoteEdit(notes.indexOf(lowest), { pitchShiftSemitones: 1 });
 *   const edited = analysis.render();
 * } finally {
 *   analysis.destroy();
 * }
 * ```
 */
export function analyzePolyphonic(request: AnalyzePolyphonicRequest): PolyphonicAnalysis {
  return new PolyphonicAnalysis(request);
}
