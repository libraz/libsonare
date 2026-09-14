import { addon } from './native.js';
import type {
  NoteEditInput,
  NoteObject,
  PolyphonicAnalysisOptions,
  PolyphonicRenderOptions,
} from './types.js';
import { assertInt32, assertInt64, assertSampleRate } from './validation.js';

/** Audio, its sample rate, and the analysis tuning, flat in one request. */
export interface AnalyzePolyphonicRequest extends PolyphonicAnalysisOptions {
  /** Source audio; must be non-empty. */
  samples: Float32Array;
  /**
   * Sample rate in Hz. Required: every frequency bound in the tuning is mapped to
   * STFT bins with this rate, so a wrong or omitted value analyses a different
   * register than the one asked for.
   */
  sampleRate: number;
}

/**
 * Integer tuning fields, checked before the addon narrows them.
 *
 * Each defaults at 0, and the narrowing to a C `int` wraps, so `2 ** 32` would
 * arrive as 0 and select the default — an analysis at a framing the caller never
 * asked for, reported as a success. Every other constraint (parity, the harmonic
 * ceilings, the window-frame range) stays the core's to enforce, which names the
 * stage that rejected the value.
 */
const INT_FIELDS = [
  'nFft',
  'hopLength',
  'winLength',
  'salienceHarmonics',
  'maxPolyphony',
  'maskHarmonics',
  'inharmonicityMinPartials',
  'windowFrames',
] as const;

function assertPolyphonicOptions(fnName: string, options: PolyphonicAnalysisOptions): void {
  for (const field of INT_FIELDS) {
    const value = options[field];
    if (value !== undefined) {
      assertInt32(fnName, value, field);
    }
  }
}

/**
 * A polyphonic analysis held open for editing: the notes it found, the figures
 * that say what was found, and a render back to audio.
 *
 * Build one with {@link analyzePolyphonic}. The analysis holds a complex
 * spectrogram and, per note, the complex weight of every bin that note claimed —
 * the measurement, which no host acts on — so it stays native and only what a
 * host edits crosses: the notes, each note's pending edit, the per-frame voice
 * count, and per note a pitch, a level and a salience curve. Re-rendering an edit
 * therefore costs no second analysis, which is the only reason to hold the
 * measurement at all.
 *
 * Native memory is held until {@link destroy} (or a `using` block) releases it.
 *
 * @example
 * ```ts
 * using analysis = analyzePolyphonic({ samples, sampleRate, maxPolyphony: 3 });
 *
 * // Lift the lowest note of the chord by a semitone and leave the rest alone.
 * const notes = analysis.notes();
 * const lowest = notes.reduce((a, b) => (a.medianHz <= b.medianHz ? a : b));
 * analysis.setNoteEdit(notes.indexOf(lowest), { pitchShiftSemitones: 1 });
 * const edited = analysis.render();
 * ```
 */
export class PolyphonicAnalysis {
  private native: InstanceType<typeof addon.PolyphonicAnalysis>;

  /**
   * Analyses `samples`. Equivalent to {@link analyzePolyphonic}, which is the
   * canonical spelling.
   */
  constructor(request: AnalyzePolyphonicRequest) {
    const { samples, sampleRate, ...options } = request;
    assertSampleRate('analyzePolyphonic', sampleRate);
    assertPolyphonicOptions('analyzePolyphonic', options);
    this.native = new addon.PolyphonicAnalysis(samples, sampleRate, options);
  }

  /**
   * Number of notes the analysis resolved, which is also the number of claim
   * sets. Silence, or material the framing cannot resolve, tracks no ridge and is
   * 0 notes rather than an error.
   */
  get noteCount(): number {
    return this.native.noteCount();
  }

  /** Number of STFT frames the analysis ran over. */
  get frameCount(): number {
    return this.native.frameCount();
  }

  /**
   * The notes, in the order their claim sets are held in.
   *
   * Each carries its sample span, its frame span, its median pitch, its
   * steadiness, its amplitude curve and its pending edit — envelope points
   * included; every note comes back with the identity edit, so rendering an
   * untouched analysis reproduces its own round trip. The F0 and salience curves
   * are {@link noteF0} and {@link noteSalience} rather than fields, because a host
   * that only edits a gain never pays for them.
   */
  notes(): NoteObject[] {
    return this.native.notes();
  }

  /**
   * Replaces one note's pending edit; omitting `edit` restores the identity.
   *
   * The only thing a host writes. Everything else on a note is a measurement, and
   * the order is the pairing with the claim sets, so neither is settable. An edit
   * replaces rather than merges: a field left out of `edit` is the identity, not
   * whatever was set before.
   *
   * `edit.amplitudeEnvelope` is copied, so the caller's array need not be kept.
   * It is stretched over whatever length the note renders at, so it survives a
   * time stretch and need not match the note's frame count; one entry is a
   * constant gain, and every value must be finite and non-negative.
   *
   * @throws {TypeError} `note` is not a number, or `edit` is not a plain object.
   * @throws {RangeError} `note` is not a non-negative integer.
   * @throws {SonareError} `note` is past the last note.
   */
  setNoteEdit(note: number, edit?: NoteEditInput): void {
    if (edit?.timeOffsetSamples !== undefined) {
      // 0 is this field's identity, and the addon truncates onto it, so a
      // sub-sample shift would render unmoved.
      assertInt64('setNoteEdit', edit.timeOffsetSamples, 'edit.timeOffsetSamples');
    }
    this.native.setNoteEdit(note, edit);
  }

  /**
   * Per-frame voice count over the whole analysis, before tracking dropped
   * anything.
   *
   * What the estimation saw rather than what survived: a frame reported as three
   * voices with two notes spanning it is the difference between the two stages,
   * which is the figure a host deciding what to edit wants.
   */
  polyphony(): Int32Array {
    return this.native.polyphony();
  }

  /**
   * One note's F0 in Hz, per frame over its own span — `frameEnd - frameStart`
   * entries, so index `i` belongs to frame `frameStart + i`.
   *
   * @throws {SonareError} `note` is past the last note.
   */
  noteF0(note: number): Float32Array {
    return this.native.noteF0(note);
  }

  /** One note's linear RMS, indexed exactly as {@link noteF0}. */
  noteAmplitude(note: number): Float32Array {
    return this.native.noteAmplitude(note);
  }

  /**
   * One note's salience, indexed exactly as {@link noteF0}, and the one measured
   * curve here that is not the note's own: it is the tracked ridge's, so a frame
   * of the note the ridge does not reach reads 0.
   *
   * Salience is what the estimation scored the candidate at, so it says how well
   * the material supported this note rather than how loud the note is —
   * {@link noteAmplitude} is the loud.
   */
  noteSalience(note: number): Float32Array {
    return this.native.noteSalience(note);
  }

  /**
   * One note's amplitude envelope points, as last set through
   * {@link setNoteEdit}; empty for a note carrying none.
   *
   * The one reading here that is not a measurement, and the one that is **not**
   * indexed over the note's frame span: an envelope is a set of gain points
   * stretched over whatever length the note renders at, so its length is however
   * many points were handed in and has nothing to do with
   * `frameEnd - frameStart`.
   *
   * The same points reach JS as `notes()[note].edit.amplitudeEnvelope`; this is
   * the cheap way to read one note's without marshalling every note.
   */
  noteEnvelope(note: number): Float32Array {
    return this.native.noteEnvelope(note);
  }

  /**
   * The partial-series stretch fitted for each note, one entry per note in
   * {@link notes} order.
   *
   * Empty unless the analysis was built with `estimateInharmonicity`, so an
   * empty array means the fit was never asked for rather than that it found
   * nothing. An analysis that did ask reports one entry per note whatever
   * happened to each:
   *
   * - A non-negative entry is the fitted stretch, `B` in
   *   `f_h = h*f0*sqrt(1 + B*h^2)`. **0 is a fitted result and means the
   *   harmonic series** — it is not the refusal.
   * - Exactly `-1` is a refusal: that note's claims were placed at the
   *   `inharmonicity` the options declared instead.
   *
   * The refusal is reported rather than folded away because the declared
   * `inharmonicity` also defaults to 0, which is what a genuine fit returns for
   * an unstretched note. Handed only the effective stretch, a host could not
   * tell a fit that reached its material from one that did not — and at the
   * default framing the fit takes an isolated note in the middle register and
   * refuses a chord.
   *
   * @example
   * ```ts
   * using analysis = analyzePolyphonic({ samples, sampleRate, estimateInharmonicity: true });
   *
   * const stretch = analysis.noteInharmonicity();
   * const fitted = [...stretch].filter((b) => b !== -1);
   * ```
   */
  noteInharmonicity(): Float32Array {
    return this.native.noteInharmonicity();
  }

  /**
   * Renders the analysis back to audio with whatever edits its notes carry.
   *
   * Each note's claimed share is inverted, edited, and added to the residual —
   * the part of the input no note claimed. With every edit identity the result is
   * the analysis's own round trip, not the source bit for bit: the STFT round
   * trip's error is neither added to nor removed here.
   *
   * The render is additive per note with no cross-note term, so an unedited
   * note's contribution is identical between two renders. That is worth stating
   * because it is also the limit: a host cannot tell from two renders whether a
   * claim set divided the energy correctly.
   *
   * @returns The rendered audio, the same length as the analysed source.
   * @throws {SonareError} A note carries an edit the render refuses — an
   *   envelope value that is not a finite non-negative gain, for instance.
   */
  render(options: PolyphonicRenderOptions = {}): Float32Array {
    return this.native.render(options);
  }

  /**
   * Release the native analysis now instead of waiting for garbage collection.
   * Idempotent; any other method called afterwards throws. A long-lived process
   * that analyses per request must call this, or the spectrogram and the claim
   * sets accumulate for as long as the wrapper stays unreachable-but-uncollected.
   */
  destroy(): void {
    this.native.destroy();
  }

  /** Releases native resources; lets `using` (Node 22+) free them automatically. */
  [Symbol.dispose](): void {
    this.destroy();
  }
}

/**
 * Analyse audio into the individually editable notes of a chord.
 *
 * One pass: one STFT, a multi-F0 extraction over it, a claim set per tracked
 * ridge, the apportionment of the bins two notes stand on, and the measured
 * fields of each note. The result is a handle — see {@link PolyphonicAnalysis} —
 * so an edit can be re-rendered without analysing again.
 *
 * This is the polyphonic counterpart of `extractNotes`: that one needs a
 * monophonic F0 track supplied by the caller and edits one line, this one finds
 * its own voices and edits one note of a chord. Finding no notes is not an error
 * — silence tracks no ridge, and rendering that is the residual alone, which is
 * the whole round trip.
 *
 * @param request - Audio, its sample rate, and the optional analysis tuning.
 * @returns An open analysis the caller owns and must {@link PolyphonicAnalysis.destroy}.
 * @throws {TypeError} `samples` is not a `Float32Array`.
 * @throws {RangeError} `sampleRate` is out of the supported range.
 * @throws {SonareError} `samples` is empty, a tuning field is out of its stage's
 *   range, or the library was built without the pitch editor.
 *
 * @example
 * ```ts
 * using analysis = analyzePolyphonic({ samples, sampleRate });
 *
 * // What the estimation saw, frame by frame, before tracking dropped anything.
 * const voices = analysis.polyphony();
 *
 * // Fade the second note out and hear it.
 * analysis.setNoteEdit(1, { amplitudeEnvelope: Float32Array.from([1, 0]) });
 * const output = analysis.render({ fadeMs: 10 });
 * ```
 */
export function analyzePolyphonic(request: AnalyzePolyphonicRequest): PolyphonicAnalysis {
  return new PolyphonicAnalysis(request);
}
