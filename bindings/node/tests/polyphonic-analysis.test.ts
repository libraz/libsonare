/**
 * The polyphonic editing surface: the analysis handle, what it reports, the one
 * field a host writes, and the render back to audio.
 *
 * The fixture is the C-ABI test's: a fifth whose partials collide — E4's 3rd and
 * B4's 2nd land in one bin at this framing — so the chain the handle wraps has a
 * shared bin to divide and is not exercising its trivial path. Keeping the same
 * signal means a note count that disagrees with `sonare_c_polyphony_test.cpp` is
 * this wrapper's doing rather than the chain's.
 */

import { describe, expect, it } from 'vitest';
import type { NoteObject, PolyphonicAnalysis, PolyphonicAnalysisOptions } from '../src/index.js';
import { analyzePolyphonic, ErrorCode, isSonareError } from '../src/index.js';

const SR = 44100;
const LENGTH = 22050;
const PARTIALS = 10;
const LOW_HZ = 329.6276;
const HIGH_HZ = 493.8833;
/** Frames this fixture analyses at the default hop, and at a doubled one. */
const DEFAULT_FRAMES = 44;
const WIDE_HOP_FRAMES = 22;

function addTone(into: Float32Array, f0Hz: number): void {
  const nyquist = SR / 2;
  for (let h = 1; h <= PARTIALS; h += 1) {
    const hz = h * f0Hz;
    if (hz >= nyquist) {
      break;
    }
    const phase = 0.37 * h * h;
    const level = 0.25 / h;
    for (let i = 0; i < into.length; i += 1) {
      into[i] += level * Math.sin((2 * Math.PI * hz * i) / SR + phase);
    }
  }
}

/** The chord every case here analyses; built once, never mutated. */
const CHORD = (() => {
  const samples = new Float32Array(LENGTH);
  addTone(samples, LOW_HZ);
  addTone(samples, HIGH_HZ);
  return samples;
})();

/** Runs `body` against a fresh analysis and releases it, however `body` ends. */
function withAnalysis<T>(
  body: (analysis: PolyphonicAnalysis) => T,
  options: PolyphonicAnalysisOptions = {},
): T {
  const analysis = analyzePolyphonic({ samples: CHORD, sampleRate: SR, ...options });
  try {
    return body(analysis);
  } finally {
    analysis.destroy();
  }
}

function cents(hz: number, against: number): number {
  return Math.abs(1200 * Math.log2(hz / against));
}

function peak(samples: Float32Array): number {
  let highest = 0;
  for (let i = 0; i < samples.length; i += 1) {
    highest = Math.max(highest, Math.abs(samples[i]));
  }
  return highest;
}

function maxDifference(a: Float32Array, b: Float32Array): number {
  let worst = 0;
  for (let i = 0; i < a.length; i += 1) {
    worst = Math.max(worst, Math.abs(a[i] - b[i]));
  }
  return worst;
}

/**
 * Asserts that an edit meant to change the sound did: the output moved away from
 * `source`, and it is still a signal. The amplitude band is what a bare
 * difference check misses — silence differs from the source by its own peak, so it
 * passes one, and a blow-up passes it too.
 */
function expectEdited(out: Float32Array, source: Float32Array): void {
  expect(out).toHaveLength(source.length);
  expect(maxDifference(source, out)).toBeGreaterThan(0.01);
  expect(peak(out)).toBeGreaterThan(0.5 * peak(source));
  expect(peak(out)).toBeLessThan(2 * peak(source));
}

function span(note: NoteObject): number {
  return note.frameEnd - note.frameStart;
}

function caught(body: () => unknown): unknown {
  try {
    body();
  } catch (error) {
    return error;
  }
  return undefined;
}

function expectSonareError(error: unknown, code: ErrorCode): void {
  expect(isSonareError(error)).toBe(true);
  if (isSonareError(error)) {
    expect(error.code).toBe(code);
  }
}

describe('analyzePolyphonic', () => {
  it('resolves a two-tone chord into its two notes', () => {
    withAnalysis((analysis) => {
      // Two tones, so every count below is comparing two notes rather than one
      // note against itself.
      expect(analysis.noteCount).toBe(2);
      expect(analysis.frameCount).toBe(DEFAULT_FRAMES);

      const pitches = analysis
        .notes()
        .map((note) => note.medianHz)
        .sort((a, b) => a - b);
      expect(cents(pitches[0], LOW_HZ)).toBeLessThan(50);
      expect(cents(pitches[1], HIGH_HZ)).toBeLessThan(50);
      expect(pitches[0]).toBeCloseTo(329.62, 1);
      expect(pitches[1]).toBeCloseTo(493.88, 1);
      expect(analysis.render()).toHaveLength(LENGTH);
    });
  });

  it('reports every note with the identity edit', () => {
    withAnalysis((analysis) => {
      const notes = analysis.notes();
      expect(notes).toHaveLength(analysis.noteCount);
      for (const note of notes) {
        expect(note.offsetSample).toBeGreaterThan(note.onsetSample);
        expect(note.frameEnd).toBeGreaterThan(note.frameStart);
        expect(note.medianHz).toBeGreaterThan(0);
        expect(note.edit.pitchShiftSemitones).toBe(0);
        expect(note.edit.gainDb).toBe(0);
        expect(note.edit.timeOffsetSamples).toBe(0);
        // 1, not 0: zero is how a caller *writes* the identity, and the way out
        // reports the ratio that will be applied. The by-value door already does
        // this, and its own tests pin the same 1.
        expect(note.edit.timeStretchRatio).toBe(1);
        expect(note.edit.formantShiftSemitones).toBe(0);
        expect(note.edit.vibratoDepthChange).toBe(0);
        expect(note.edit.driftChange).toBe(0);
        expect(note.edit.muted).toBe(false);
        // The handle holds no envelope until a host sets one.
        expect(note.edit.amplitudeEnvelope).toBeInstanceOf(Float32Array);
        expect(note.edit.amplitudeEnvelope).toHaveLength(0);
      }
    });
  });

  it('gives each note an amplitude curve and three per-frame readings over its span', () => {
    withAnalysis((analysis) => {
      const notes = analysis.notes();
      for (let i = 0; i < notes.length; i += 1) {
        const frames = span(notes[i]);
        expect(frames).toBeGreaterThan(0);
        expect(notes[i].amplitude).toHaveLength(frames);

        const f0 = analysis.noteF0(i);
        expect(f0).toHaveLength(frames);
        expect([...f0].every((hz) => hz > 0)).toBe(true);

        const amplitude = analysis.noteAmplitude(i);
        expect(amplitude).toHaveLength(frames);
        expect([...amplitude]).toEqual([...notes[i].amplitude]);

        // The salience curve is the ridge's, read through the ridge's own start.
        // A misalignment there would leave it all zeros.
        const salience = analysis.noteSalience(i);
        expect(salience).toHaveLength(frames);
        expect([...salience].every((value) => value >= 0)).toBe(true);
        expect([...salience].some((value) => value > 0)).toBe(true);
      }
    });
  });

  it('reports one voice count per frame', () => {
    withAnalysis((analysis) => {
      const voices = analysis.polyphony();
      expect(voices).toBeInstanceOf(Int32Array);
      expect(voices).toHaveLength(analysis.frameCount);
      expect([...voices].every((count) => count >= 0)).toBe(true);
      // What the estimation saw, so the chord's own frames carry at least one.
      expect([...voices].some((count) => count > 0)).toBe(true);
    });
  });

  it('passes an integer tuning field through to the framing', () => {
    // A longer hop is fewer frames. Without this the whole config object could be
    // dropped on the floor and every other case here would still pass.
    expect(withAnalysis((analysis) => analysis.frameCount, { hopLength: 1024 })).toBe(
      WIDE_HOP_FRAMES,
    );
    expect(withAnalysis((analysis) => analysis.frameCount)).toBe(DEFAULT_FRAMES);
  });

  it('passes a float tuning field through to the tracker, and reads a negative as zero', () => {
    // A ridge floor longer than the fixture drops every ridge, which is how the
    // float fields are shown to reach their stage rather than being ignored.
    expect(withAnalysis((analysis) => analysis.noteCount, { minRidgeDurationMs: 1e6 })).toBe(0);
    // The sentinel's claim: on this field a negative is the value 0, not a
    // refusal and not the default, so dropping the floor keeps at least what the
    // default kept.
    expect(
      withAnalysis((analysis) => analysis.noteCount, {
        minRidgeDurationMs: -1,
        minSeparationCents: -1,
      }),
    ).toBeGreaterThanOrEqual(2);
  });

  it('refuses an empty buffer and a sample rate out of range', () => {
    expectSonareError(
      caught(() => analyzePolyphonic({ samples: new Float32Array(0), sampleRate: SR })),
      ErrorCode.InvalidParameter,
    );
    expect(() => analyzePolyphonic({ samples: CHORD, sampleRate: 1 })).toThrow(RangeError);
  });

  it('refuses an integer tuning field the narrowing would wrap', () => {
    // 2 ** 32 narrows to 0, which selects the default, so the analysis would
    // otherwise succeed at a framing the caller never asked for. The facade
    // pre-empts that as the library's own InvalidParameter; a wrong TYPE on the
    // same field is the same check and reports the same way.
    expectSonareError(
      caught(() => analyzePolyphonic({ samples: CHORD, sampleRate: SR, hopLength: 2 ** 32 })),
      ErrorCode.InvalidParameter,
    );
    expectSonareError(
      caught(() =>
        analyzePolyphonic({
          samples: CHORD,
          sampleRate: SR,
          hopLength: '1024' as unknown as number,
        }),
      ),
      ErrorCode.InvalidParameter,
    );
  });

  it('refuses a wrong-typed tuning field instead of silently taking the default', () => {
    // The float fields have no narrowing to pre-empt, so their rejection is the
    // addon's: the presence-checked reader family raises rather than falling back
    // to the default, which would analyse at a claim width nobody asked for.
    expect(() =>
      analyzePolyphonic({
        samples: CHORD,
        sampleRate: SR,
        claimLobes: '2' as unknown as number,
      }),
    ).toThrow(TypeError);
    expect(() =>
      analyzePolyphonic({
        samples: CHORD,
        sampleRate: SR,
        tonalityOff: 1 as unknown as boolean,
      }),
    ).toThrow(TypeError);
  });
});

describe('a polyphonic note edit', () => {
  it('reaches the render', () => {
    withAnalysis((analysis) => {
      const unedited = analysis.render();
      analysis.setNoteEdit(0, { pitchShiftSemitones: 1 });

      // The edit came back, so the handle holds it rather than having validated
      // and dropped it.
      const notes = analysis.notes();
      expect(notes[0].edit.pitchShiftSemitones).toBe(1);
      expect(notes[1].edit.pitchShiftSemitones).toBe(0);

      // The assertion this whole surface exists for: a wrapper that accepts an
      // edit and never forwards it passes every readback above and fails here.
      const edited = analysis.render();
      expectEdited(edited, unedited);
      // A semitone on one note of this chord moves the render by 0.954 peak
      // absolute, so the threshold sits nowhere near the measurement.
      expect(maxDifference(unedited, edited)).toBeGreaterThan(0.5);
    });
  });

  it('carries an envelope the caller need not keep', () => {
    withAnalysis((analysis) => {
      const unedited = analysis.render();
      // A fade to half rather than to silence: the render must change, and the
      // band expectEdited asserts still has to recognise the output as a signal.
      analysis.setNoteEdit(1, { amplitudeEnvelope: [1, 0.5] });

      const envelope = analysis.notes()[1].edit.amplitudeEnvelope;
      expect(envelope).toBeInstanceOf(Float32Array);
      expect([...envelope]).toEqual([1, 0.5]);
      expectEdited(analysis.render(), unedited);
    });
  });

  it('hands its envelope points back per note', () => {
    withAnalysis((analysis) => {
      // Exactly representable in float32, so the readback can be compared for
      // equality rather than to a tolerance.
      analysis.setNoteEdit(1, { amplitudeEnvelope: [1, 0.25, 0] });

      const points = analysis.noteEnvelope(1);
      expect(points).toBeInstanceOf(Float32Array);
      expect([...points]).toEqual([1, 0.25, 0]);
      // Three points over a span of many frames: an envelope is a set of gain
      // points, not a per-frame signal, so its length is its own.
      expect(points.length).toBeLessThan(span(analysis.notes()[1]));
      // The note that was never given one reads empty, which is what proves the
      // read is per note rather than whatever was set last.
      expect(analysis.noteEnvelope(0)).toHaveLength(0);
      expect([...analysis.notes()[1].edit.amplitudeEnvelope]).toEqual([1, 0.25, 0]);
      expect(analysis.notes()[0].edit.amplitudeEnvelope).toHaveLength(0);
    });
  });

  it('is replaced rather than merged', () => {
    withAnalysis((analysis) => {
      const unedited = analysis.render();
      analysis.setNoteEdit(0, { gainDb: -6 });
      expect(analysis.notes()[0].edit.gainDb).toBe(-6);

      analysis.setNoteEdit(0);
      expect(analysis.notes()[0].edit.gainDb).toBe(0);
      expect(analysis.notes()[0].edit.amplitudeEnvelope).toHaveLength(0);
      // Back to the identity, so the render is the untouched round trip again.
      expect([...analysis.render()]).toEqual([...unedited]);
    });
  });

  it('reads a zero stretch ratio as one, the way the by-value door reads it', () => {
    withAnalysis((analysis) => {
      const unedited = analysis.render();
      analysis.setNoteEdit(0, { timeStretchRatio: 0 });
      expect(analysis.notes()[0].edit.timeStretchRatio).toBe(1);
      expect([...analysis.render()]).toEqual([...unedited]);
    });
  });

  it('treats an explicit undefined like an omitted field', () => {
    withAnalysis((analysis) => {
      analysis.setNoteEdit(0, {
        timeOffsetSamples: undefined,
        pitchShiftSemitones: undefined,
        gainDb: undefined,
        timeStretchRatio: undefined,
        formantShiftSemitones: undefined,
        vibratoDepthChange: undefined,
        driftChange: undefined,
        muted: undefined,
        amplitudeEnvelope: undefined,
      });
      const explicit = analysis.notes()[0].edit;
      const rendered = [...analysis.render({ fadeMs: undefined, vibratoCutoffHz: undefined })];

      analysis.setNoteEdit(0);
      expect(analysis.notes()[0].edit).toEqual(explicit);
      expect([...analysis.render()]).toEqual(rendered);
    });
  });

  it('reaches the render at a fade the caller chose', () => {
    withAnalysis((analysis) => {
      analysis.setNoteEdit(0, { gainDb: -12 });
      const short = analysis.render({ fadeMs: 1 });
      const long = analysis.render({ fadeMs: 200 });
      // One edit, two cross-fade lengths over the same edges: the render option
      // reaches the core rather than being dropped on the way.
      expect(maxDifference(short, long)).toBeGreaterThan(0);
    });
  });
});

describe('the polyphonic surface refuses what it cannot do', () => {
  it('rejects a note index the analysis does not have', () => {
    withAnalysis((analysis) => {
      const past = analysis.noteCount;
      expectSonareError(
        caught(() => analysis.setNoteEdit(past, { gainDb: -6 })),
        ErrorCode.InvalidParameter,
      );
      expectSonareError(
        caught(() => analysis.noteF0(past)),
        ErrorCode.InvalidParameter,
      );
      expectSonareError(
        caught(() => analysis.noteAmplitude(past)),
        ErrorCode.InvalidParameter,
      );
      expectSonareError(
        caught(() => analysis.noteSalience(past)),
        ErrorCode.InvalidParameter,
      );
      // The envelope sizes its buffer from the note rather than from the span, so
      // its out-of-range path is its own and is asserted rather than assumed.
      expectSonareError(
        caught(() => analysis.noteEnvelope(past)),
        ErrorCode.InvalidParameter,
      );
      // The refusals left the handle usable.
      expect(analysis.notes()).toHaveLength(past);
    });
  });

  it('rejects a note index that is not a non-negative integer', () => {
    withAnalysis((analysis) => {
      const readers = [
        'noteF0',
        'noteAmplitude',
        'noteSalience',
        'noteEnvelope',
        'setNoteEdit',
      ] as const;
      for (const reader of readers) {
        const call = (note: unknown) =>
          (analysis[reader] as (note: unknown) => unknown).call(analysis, note);
        expect(() => call('0')).toThrow(TypeError);
        // A negative would otherwise arrive as a size_t past every note and a
        // fractional or NaN index as note 0, which is an edit nobody asked for.
        expect(() => call(-1)).toThrow(RangeError);
        expect(() => call(0.5)).toThrow(RangeError);
        expect(() => call(Number.NaN)).toThrow(RangeError);
      }
      expect(analysis.noteCount).toBe(2);
    });
  });

  it('rejects a render option that is not a number', () => {
    withAnalysis((analysis) => {
      expect(() => analysis.render({ fadeMs: '5' as unknown as number })).toThrow(TypeError);
      expect(analysis.render()).toHaveLength(LENGTH);
    });
  });

  it('refuses every method once destroyed, and destroys idempotently', () => {
    const analysis = analyzePolyphonic({ samples: CHORD, sampleRate: SR });
    expect(analysis.noteCount).toBe(2);
    analysis.destroy();
    expect(() => analysis.destroy()).not.toThrow();

    expect(() => analysis.noteCount).toThrow('PolyphonicAnalysis has been destroyed');
    expect(() => analysis.frameCount).toThrow('PolyphonicAnalysis has been destroyed');
    expect(() => analysis.notes()).toThrow('PolyphonicAnalysis has been destroyed');
    expect(() => analysis.polyphony()).toThrow('PolyphonicAnalysis has been destroyed');
    expect(() => analysis.noteF0(0)).toThrow('PolyphonicAnalysis has been destroyed');
    expect(() => analysis.noteAmplitude(0)).toThrow('PolyphonicAnalysis has been destroyed');
    expect(() => analysis.noteSalience(0)).toThrow('PolyphonicAnalysis has been destroyed');
    expect(() => analysis.noteEnvelope(0)).toThrow('PolyphonicAnalysis has been destroyed');
    expect(() => analysis.setNoteEdit(0, { gainDb: -6 })).toThrow(
      'PolyphonicAnalysis has been destroyed',
    );
    expect(() => analysis.render()).toThrow('PolyphonicAnalysis has been destroyed');
  });

  it('frees the native analysis via `using`', () => {
    let captured: PolyphonicAnalysis | undefined;
    {
      using analysis = analyzePolyphonic({ samples: CHORD, sampleRate: SR });
      captured = analysis;
      expect(analysis.noteCount).toBe(2);
    }
    // After the block, dispose ran; destroy() is idempotent so a second call must
    // not throw, and the handle is gone.
    expect(() => captured?.destroy()).not.toThrow();
    expect(() => captured?.notes()).toThrow('PolyphonicAnalysis has been destroyed');
  });
});
