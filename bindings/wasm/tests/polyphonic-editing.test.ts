/**
 * The polyphonic editing handle on the WASM surface: one analysis of a chord, the
 * notes it found, an edit on one of them, and a render back to audio.
 *
 * The handle exists so the measurement stays inside it — the source's complex
 * spectrogram and, per note, the complex weight of every claimed bin. Two cases
 * below are about that rather than about a result: the note object carries exactly
 * eight fields, and no spectrum, mask or per-bin accessor is reachable from JS.
 *
 * The fixture is half a second of E4 and B4 held together at 44.1 kHz, each as a
 * harmonically rich tone: the salience model scores a candidate by summing its
 * partials, so two bare sines would let the sub-octave that explains both of them
 * compete with either, which is the composite-pitch case the estimator documents
 * rather than anything about this binding.
 *
 * The note count, the frame counts and the median pitches below are the library's
 * own measured figures for that chord, not values read off a run of this file.
 * Note indices are still resolved by pitch rather than by position, so a failure
 * names the pitch that moved instead of an index that shifted under it.
 */

import { afterAll, beforeAll, describe, expect, it } from 'vitest';
import {
  analyzePolyphonic,
  ErrorCode,
  init,
  isSonareError,
  type NoteObject,
  type PolyphonicAnalysis,
} from '../src/index';

const sampleRate = 44100;
/** The analysis default, and the hop the frame counts below belong to. */
const hopLength = 512;
const totalSamples = sampleRate / 2;
/**
 * Measured: 44 frames at the default hop, 22 at twice it. Both are
 * `1 + floor(length / hop)`, which is what a centred framing emits
 * (core/spectrum.cpp's frames_for_padded_length).
 */
const defaultFrameCount = 44;
const coarseFrameCount = 22;

/** E4 and B4, a fifth apart. */
const lowerHz = 329.63;
const upperHz = 493.88;
/** Notes come back by ascending median pitch where their spans start together. */
const expectedNoteCount = 2;

/** `harmonics` partials of `f0Hz` at `1/h` amplitude, which a salience sum scores. */
function richTone(f0Hz: number, amplitude: number, harmonics: number, n: number): Float32Array {
  const out = new Float32Array(n);
  for (let h = 1; h <= harmonics; h++) {
    const partialHz = h * f0Hz;
    if (partialHz >= sampleRate / 2) {
      break;
    }
    for (let i = 0; i < n; i++) {
      out[i] += (amplitude / h) * Math.sin((2 * Math.PI * partialHz * i) / sampleRate);
    }
  }
  return out;
}

/**
 * The two tones held together for the whole fixture, peaking under 0.7 together.
 *
 * Deliberately not full scale, which is why every claim below about a render being
 * changed or still being a signal is stated against this fixture's own peak rather
 * than as an absolute level that holds at one input gain only.
 */
function chord(): Float32Array {
  const lower = richTone(lowerHz, 0.15, 5, totalSamples);
  const upper = richTone(upperHz, 0.15, 5, totalSamples);
  const out = new Float32Array(totalSamples);
  for (let i = 0; i < totalSamples; i++) {
    out[i] = lower[i] + upper[i];
  }
  return out;
}

function maxDifference(a: Float32Array, b: Float32Array, lo = 0, hi = a.length): number {
  let worst = 0;
  for (let i = lo; i < hi; i++) {
    worst = Math.max(worst, Math.abs(a[i] - b[i]));
  }
  return worst;
}

function peak(data: Float32Array): number {
  let highest = 0;
  for (const value of data) {
    highest = Math.max(highest, Math.abs(value));
  }
  return highest;
}

/**
 * Asserts an edit meant to change the sound did: the output moved away from
 * `reference`, and it is still a signal. The amplitude band is what a bare
 * difference check misses — silence differs from the reference by its own peak, so
 * it passes one, and a blow-up passes it too.
 */
function expectEdited(out: Float32Array, reference: Float32Array): void {
  expect(out).toHaveLength(reference.length);
  expect(maxDifference(reference, out)).toBeGreaterThan(0.02);
  const referencePeak = peak(reference);
  expect(peak(out)).toBeGreaterThan(0.5 * referencePeak);
  expect(peak(out)).toBeLessThan(2 * referencePeak);
}

/** Index of the note whose median pitch is nearest `hz`, which must be within 2%. */
function indexOfPitch(notes: readonly NoteObject[], hz: number): number {
  let best = -1;
  for (let i = 0; i < notes.length; i++) {
    if (best < 0 || Math.abs(notes[i].medianHz - hz) < Math.abs(notes[best].medianHz - hz)) {
      best = i;
    }
  }
  expect(best).toBeGreaterThanOrEqual(0);
  expect(Math.abs(notes[best].medianHz - hz)).toBeLessThan(0.02 * hz);
  return best;
}

/** Asserts `action` throws a SonareError carrying `code`. */
function expectSonareError(action: () => unknown, code: ErrorCode): void {
  let caught: unknown;
  try {
    action();
  } catch (error) {
    caught = error;
  }
  expect(isSonareError(caught)).toBe(true);
  if (isSonareError(caught)) {
    expect(caught.code).toBe(code);
  }
}

let samples: Float32Array;
/** Shared read-only analysis; the cases that edit or release build their own. */
let analysis: PolyphonicAnalysis;
let notes: NoteObject[];

beforeAll(async () => {
  await init();
  samples = chord();
  analysis = analyzePolyphonic({ samples, sampleRate });
  notes = analysis.notes();
});

afterAll(() => {
  analysis.destroy();
});

describe('analyzePolyphonic', () => {
  it('resolves the chord into one note per tone', () => {
    expect(analysis.noteCount).toBe(expectedNoteCount);
    expect(notes).toHaveLength(expectedNoteCount);
    // Both spans start together, so the pair comes back by ascending median pitch.
    expect(indexOfPitch(notes, lowerHz)).toBe(0);
    expect(indexOfPitch(notes, upperHz)).toBe(1);
    // The library measures 329.62 and 493.88 Hz for this chord, so half a percent
    // (about 9 cents) is tight enough to catch a swapped or composite pitch.
    expect(Math.abs(notes[0].medianHz - lowerHz)).toBeLessThan(0.005 * lowerHz);
    expect(Math.abs(notes[1].medianHz - upperHz)).toBeLessThan(0.005 * upperHz);
  });

  it('frames the source at the analysis framing', () => {
    expect(analysis.frameCount).toBe(defaultFrameCount);
    expect(analysis.polyphony()).toHaveLength(defaultFrameCount);
  });

  it('reports more than one voice where both tones are held', () => {
    const voices = [...analysis.polyphony()];
    expect(Math.max(...voices)).toBeGreaterThanOrEqual(2);
  });

  it('spans each note over frames of the analysis framing', () => {
    for (const note of notes) {
      expect(note.frameStart).toBeGreaterThanOrEqual(0);
      expect(note.frameEnd).toBeGreaterThan(note.frameStart);
      expect(note.frameEnd).toBeLessThanOrEqual(defaultFrameCount);
      expect(note.offsetSample).toBeGreaterThan(note.onsetSample);
      expect(note.f0Stability).toBeGreaterThanOrEqual(0);
      expect(note.f0Stability).toBeLessThanOrEqual(1);
    }
    // Both tones are held steady for the whole fixture, so each reads near the
    // top of that band.
    for (const hz of [lowerHz, upperHz]) {
      expect(notes[indexOfPitch(notes, hz)].f0Stability).toBeGreaterThan(0.5);
    }
  });

  it('surfaces exactly the documented NoteObject fields, and no more', () => {
    // The guard on what the handle keeps: a spectrogram, a mask or a per-bin weight
    // appearing on a note would show up here as a tenth key. The set is
    // extractNotes' own, which is the point -- one note shape across both doors.
    expect(Object.keys(notes[0]).sort()).toEqual([
      'amplitude',
      'edit',
      'f0Stability',
      'frameEnd',
      'frameStart',
      'medianCents',
      'medianHz',
      'offsetSample',
      'onsetSample',
    ]);
  });

  it('gives every note the identity edit', () => {
    for (const note of notes) {
      expect(note.edit).toEqual({
        timeOffsetSamples: 0,
        pitchShiftSemitones: 0,
        gainDb: 0,
        timeStretchRatio: 1,
        formantShiftSemitones: 0,
        vibratoDepthChange: 0,
        driftChange: 0,
        muted: false,
        amplitudeEnvelope: new Float32Array(0),
      });
    }
  });

  it('exposes no spectrum, mask or per-bin accessor', () => {
    // The measurement is the handle's whole reason to exist, so its absence is
    // asserted rather than left to a reviewer's reading of the class.
    const reachable = analysis as unknown as Record<string, unknown>;
    for (const name of ['magnitude', 'power', 'spectrum', 'masks', 'weights', 'sharedBins']) {
      expect(reachable[name]).toBeUndefined();
    }
  });

  it('rejects an unusable request before it reaches the core', () => {
    expect(() => analyzePolyphonic({ samples: new Float32Array(0), sampleRate })).toThrow(
      RangeError,
    );
    expect(() => analyzePolyphonic({ samples, sampleRate: 7999 })).toThrow(RangeError);
  });
});

describe('PolyphonicAnalysis curves', () => {
  it('gives each note one value per frame of its own span', () => {
    for (let i = 0; i < notes.length; i++) {
      const span = notes[i].frameEnd - notes[i].frameStart;
      expect(analysis.noteF0(i)).toHaveLength(span);
      expect(analysis.noteAmplitude(i)).toHaveLength(span);
      expect(analysis.noteSalience(i)).toHaveLength(span);
      // The note's own inline curve is the accessor's, not a second measurement.
      expect(notes[i].amplitude).toEqual(analysis.noteAmplitude(i));
    }
  });

  it("measures each note's own pitch and level over its span", () => {
    const lower = indexOfPitch(notes, lowerHz);
    for (const hz of analysis.noteF0(lower)) {
      expect(Math.abs(hz - lowerHz)).toBeLessThan(0.05 * lowerHz);
    }
    for (const rms of analysis.noteAmplitude(lower)) {
      expect(rms).toBeGreaterThan(0);
    }
    // Salience is the ridge's score, not a level, so the only claim is that the
    // material supported the note at all.
    expect(Math.max(...analysis.noteSalience(lower))).toBeGreaterThan(0);
  });

  it('reports no envelope on a note nobody has given one', () => {
    for (let i = 0; i < notes.length; i++) {
      expect(analysis.noteEnvelope(i)).toHaveLength(0);
      expect(notes[i].edit.amplitudeEnvelope).toHaveLength(0);
    }
  });

  it('rejects a note index outside the set', () => {
    for (const index of [analysis.noteCount, analysis.noteCount + 4, -1, 1.5]) {
      expectSonareError(() => analysis.noteF0(index), ErrorCode.InvalidParameter);
      expectSonareError(() => analysis.noteAmplitude(index), ErrorCode.InvalidParameter);
      expectSonareError(() => analysis.noteSalience(index), ErrorCode.InvalidParameter);
      expectSonareError(() => analysis.noteEnvelope(index), ErrorCode.InvalidParameter);
      expectSonareError(() => analysis.setNoteEdit(index, {}), ErrorCode.InvalidParameter);
    }
  });
});

describe('PolyphonicAnalysis.render', () => {
  let edited: PolyphonicAnalysis;
  let identity: Float32Array;
  let lower: number;

  beforeAll(() => {
    edited = analyzePolyphonic({ samples, sampleRate });
    lower = indexOfPitch(edited.notes(), lowerHz);
    identity = edited.render();
  });

  afterAll(() => {
    edited.destroy();
  });

  it('returns the source length and reproduces the round trip unedited', () => {
    expect(identity).toHaveLength(totalSamples);
    // Not the source bit for bit — the STFT round trip's own error is neither
    // added to nor removed here — but close enough over the interior that the
    // edits below are the edits rather than the reconstruction. The comparison
    // stops one window short of each end, where a centred framing's overlap is
    // incomplete and the round trip legitimately tapers.
    const nFft = 4096;
    expect(maxDifference(samples, identity, nFft, totalSamples - nFft)).toBeLessThan(
      0.05 * peak(samples),
    );
    // And it is still the chord, not a silent or blown-up reconstruction.
    expect(peak(identity)).toBeGreaterThan(0.5 * peak(samples));
  });

  it('applies a pitch-shift edit to the note it was set on', () => {
    edited.setNoteEdit(lower, { pitchShiftSemitones: 1 });
    const shifted = edited.render();
    expectEdited(shifted, identity);
    // Transposing one voice of a fifth moves the waveform by about its own peak —
    // the library measures 0.954 absolute on a full-scale chord — so this sits
    // nowhere near a tolerance. Stated against the fixture's peak rather than as
    // that absolute, which only holds at that input gain.
    expect(maxDifference(identity, shifted)).toBeGreaterThan(0.5 * peak(identity));
  });

  it('reads the edit back off the note', () => {
    // Every other assertion here passes on a wrapper that silently drops the
    // edit, so the readback is what says it was stored.
    const stored = edited.notes()[lower].edit;
    expect(stored.pitchShiftSemitones).toBe(1);
    // 0 is the identity spelling of a stretch ratio and reads back as 1.
    expect(stored.timeStretchRatio).toBe(1);
    expect(stored.muted).toBe(false);
  });

  it('restores the identity when the edit is cleared', () => {
    edited.setNoteEdit(lower, {});
    expect(edited.notes()[lower].edit.pitchShiftSemitones).toBe(0);
    expect(edited.render()).toEqual(identity);
  });

  it('leaves the other notes alone', () => {
    const upper = indexOfPitch(edited.notes(), upperHz);
    edited.setNoteEdit(upper, { gainDb: -20 });
    const quietUpper = edited.render();
    expectEdited(quietUpper, identity);
    edited.setNoteEdit(upper, {});

    // The same edit on the other note is a different render, so the edit reached
    // the note it named rather than the whole analysis.
    edited.setNoteEdit(lower, { gainDb: -20 });
    expect(maxDifference(edited.render(), quietUpper)).toBeGreaterThan(0.02);
    edited.setNoteEdit(lower, {});
  });

  it('reads an envelope back through its own accessor and off the note', () => {
    const points = new Float32Array([0.25, 1]);
    edited.setNoteEdit(lower, { amplitudeEnvelope: points });
    // Indexed from 0 rather than over the note's span: these are the points as
    // given, not a per-frame curve.
    expect(edited.noteEnvelope(lower)).toEqual(points);
    expect(edited.notes()[lower].edit.amplitudeEnvelope).toEqual(points);
    // A plain number array is the same envelope, copied into WASM memory either way.
    edited.setNoteEdit(lower, { amplitudeEnvelope: [0.25, 1] });
    expect(edited.noteEnvelope(lower)).toEqual(points);
    // And it reached the render rather than only the readback.
    expectEdited(edited.render(), identity);
    edited.setNoteEdit(lower, {});
    expect(edited.noteEnvelope(lower)).toHaveLength(0);
  });

  it('refuses an envelope point that is not a usable linear gain, on the way out', () => {
    // The C ABI checks an edit where the render checks it, so setting it is not
    // the refusal.
    edited.setNoteEdit(lower, { amplitudeEnvelope: new Float32Array([1, -1]) });
    expectSonareError(() => edited.render(), ErrorCode.InvalidParameter);
    edited.setNoteEdit(lower, { amplitudeEnvelope: new Float32Array([1, 0.25]) });
    expectEdited(edited.render(), identity);
    edited.setNoteEdit(lower, {});
  });

  it('rejects a render option that cannot be one', () => {
    for (const bad of [-1, Number.NaN, Number.POSITIVE_INFINITY]) {
      expectSonareError(() => edited.render({ fadeMs: bad }), ErrorCode.InvalidParameter);
      expectSonareError(() => edited.render({ vibratoCutoffHz: bad }), ErrorCode.InvalidParameter);
    }
    // Positive control: 0 is the default on both, so the rejections above are
    // about the values rather than about the fields being read at all.
    expect(edited.render({ fadeMs: 0, vibratoCutoffHz: 0 })).toEqual(identity);
  });
});

describe('PolyphonicAnalysis config', () => {
  it('passes the framing through to the core', () => {
    // A longer hop frames the same audio fewer times. Nothing else here can move
    // this number, so it is the config field arriving rather than a default.
    const coarse = analyzePolyphonic({ samples, sampleRate, hopLength: 2 * hopLength });
    try {
      expect(coarse.frameCount).toBe(coarseFrameCount);
      expect(coarse.frameCount).toBeLessThan(defaultFrameCount);
      expect(coarse.polyphony()).toHaveLength(coarseFrameCount);
    } finally {
      coarse.destroy();
    }
  });

  it('rejects a config value the chain refuses', () => {
    // 64 is the cap on voices per frame, and the estimator owns the bound.
    expectSonareError(
      () => analyzePolyphonic({ samples, sampleRate, maxPolyphony: 65 }),
      ErrorCode.InvalidParameter,
    );
  });
});

describe('PolyphonicAnalysis disposal', () => {
  it('accepts both delete() and destroy(), and refuses use afterwards', () => {
    const handle = analyzePolyphonic({ samples, sampleRate });
    expect(typeof handle.delete).toBe('function');
    expect(typeof handle.destroy).toBe('function');
    expect(handle.noteCount).toBeGreaterThanOrEqual(0);

    handle.destroy();

    // Every door is shut, including the two property reads — a released handle
    // must not reach a freed native object by any of them.
    expectSonareError(() => handle.noteCount, ErrorCode.InvalidState);
    expectSonareError(() => handle.frameCount, ErrorCode.InvalidState);
    expectSonareError(() => handle.notes(), ErrorCode.InvalidState);
    expectSonareError(() => handle.polyphony(), ErrorCode.InvalidState);
    expectSonareError(() => handle.noteF0(0), ErrorCode.InvalidState);
    expectSonareError(() => handle.noteAmplitude(0), ErrorCode.InvalidState);
    expectSonareError(() => handle.noteSalience(0), ErrorCode.InvalidState);
    expectSonareError(() => handle.noteEnvelope(0), ErrorCode.InvalidState);
    expectSonareError(() => handle.setNoteEdit(0, {}), ErrorCode.InvalidState);
    expectSonareError(() => handle.render(), ErrorCode.InvalidState);
    // A second release would be a double free, so it is refused rather than run.
    expectSonareError(() => handle.destroy(), ErrorCode.InvalidState);
  });
});
