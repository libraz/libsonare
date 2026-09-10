/**
 * Note-object extraction and rendering on the WASM surface.
 *
 * The editing model rests on one property: a set of notes whose edits are all
 * identity renders back to the input bit for bit. Everything else here is an
 * edit applied to exactly one note's span, checked against the untouched
 * neighbour.
 */

import { beforeAll, describe, expect, it } from 'vitest';
import {
  ErrorCode,
  extractNotes,
  init,
  isSonareError,
  type NoteObject,
  type NoteObjectInput,
  renderNotes,
} from '../src/index';

const sampleRate = 22050;
const hopLength = 512;
const frameCount = 43;
const splitFrame = 22;
const frameRate = sampleRate / hopLength;
const totalSamples = frameCount * hopLength;

/** Two sustained tones a fifth apart, switching at `splitFrame`. */
function twoNoteSignal(): Float32Array {
  const samples = new Float32Array(totalSamples);
  const splitSample = splitFrame * hopLength;
  for (let i = 0; i < samples.length; i++) {
    const hz = i < splitSample ? 220 : 330;
    samples[i] = 0.5 * Math.sin((2 * Math.PI * hz * i) / sampleRate);
  }
  return samples;
}

function twoNoteF0(): Float32Array {
  const f0Hz = new Float32Array(frameCount);
  for (let frame = 0; frame < frameCount; frame++) {
    f0Hz[frame] = frame < splitFrame ? 220 : 330;
  }
  return f0Hz;
}

function voicedFlags(value: boolean): Int32Array {
  return new Int32Array(frameCount).fill(value ? 1 : 0);
}

/** Asserts that @p action throws a SonareError carrying InvalidParameter. */
function expectInvalidParameter(action: () => void): void {
  let caught: unknown;
  try {
    action();
  } catch (error) {
    caught = error;
  }
  expect(isSonareError(caught)).toBe(true);
  if (isSonareError(caught)) {
    expect(caught.code).toBe(ErrorCode.InvalidParameter);
  }
}

let samples: Float32Array;
let f0Hz: Float32Array;
let notes: NoteObject[];

beforeAll(async () => {
  await init();
  samples = twoNoteSignal();
  f0Hz = twoNoteF0();
  notes = extractNotes({ samples, sampleRate, f0Hz, voiced: voicedFlags(true), frameRate });
});

describe('extractNotes', () => {
  it('segments the track into one note per sustained pitch', () => {
    expect(notes).toHaveLength(2);
    expect(notes[0].frameStart).toBe(0);
    expect(notes[0].frameEnd).toBe(splitFrame);
    expect(notes[1].frameStart).toBe(splitFrame);
    expect(notes[1].frameEnd).toBe(frameCount);
    expect(notes[0].onsetSample).toBe(0);
    expect(notes[0].offsetSample).toBe(splitFrame * hopLength);
    expect(notes[1].offsetSample).toBe(totalSamples);
  });

  it('measures pitch and stability over each span', () => {
    expect(notes[0].medianHz).toBeCloseTo(220, 0);
    expect(notes[1].medianHz).toBeCloseTo(330, 0);
    // 220 Hz sits a whole tone below the 440 Hz reference: -1200 cents.
    expect(notes[0].medianCents).toBeCloseTo(-1200, 0);
    for (const note of notes) {
      // A perfectly steady contour has zero deviation from its median.
      expect(note.f0Stability).toBe(1);
    }
  });

  it('surfaces exactly the nine documented note fields', () => {
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

  it('gives each note its own amplitude curve, one RMS per F0 frame', () => {
    for (const note of notes) {
      expect(note.amplitude).toBeInstanceOf(Float32Array);
      expect(note.amplitude.length).toBe(note.frameEnd - note.frameStart);
      for (const rms of note.amplitude) {
        // A half-amplitude sine measures 0.5 / sqrt(2) per frame.
        expect(rms).toBeCloseTo(0.354, 1);
      }
    }
  });

  it('returns the identity edit on every extracted note', () => {
    for (const note of notes) {
      expect(note.edit).toEqual({
        timeOffsetSamples: 0,
        pitchShiftSemitones: 0,
        gainDb: 0,
        timeStretchRatio: 1,
        muted: false,
      });
    }
  });

  it('honours the voicedThreshold only when voiced is absent', () => {
    const voicedProb = new Float32Array(frameCount).fill(0.4);
    expect(
      extractNotes({ samples, sampleRate, f0Hz, voicedProb, frameRate, voicedThreshold: 0.3 }),
    ).toHaveLength(2);
    expect(
      extractNotes({ samples, sampleRate, f0Hz, voicedProb, frameRate, voicedThreshold: 0.9 }),
    ).toHaveLength(0);
  });

  it('returns an empty array when the track segments to nothing', () => {
    const empty = extractNotes({
      samples,
      sampleRate,
      f0Hz,
      voiced: voicedFlags(false),
      frameRate,
    });
    expect(empty).toEqual([]);
  });

  it('rejects a malformed request', () => {
    const voiced = voicedFlags(true);
    // Neither voicing input given.
    expectInvalidParameter(() => extractNotes({ samples, sampleRate, f0Hz, frameRate }));
    expectInvalidParameter(() => extractNotes({ samples, sampleRate, f0Hz, voiced, frameRate: 0 }));
    expectInvalidParameter(() =>
      extractNotes({ samples, sampleRate, f0Hz, voiced, frameRate, minNoteMs: -1 }),
    );
    expectInvalidParameter(() =>
      extractNotes({ samples, sampleRate, f0Hz, voiced, frameRate, voicedThreshold: 1.5 }),
    );
    expectInvalidParameter(() =>
      extractNotes({
        samples,
        sampleRate,
        f0Hz: new Float32Array([220, -1, 220]),
        voiced: new Int32Array([1, 1, 1]),
        frameRate,
      }),
    );
    expect(() =>
      extractNotes({ samples, sampleRate, f0Hz, voiced: new Int32Array(3), frameRate }),
    ).toThrow(RangeError);
    expect(() => extractNotes({ samples, sampleRate: 7999, f0Hz, voiced, frameRate })).toThrow(
      RangeError,
    );
    expect(() =>
      extractNotes({ samples: new Float32Array(0), sampleRate, f0Hz, voiced, frameRate }),
    ).toThrow(RangeError);
  });
});

describe('renderNotes', () => {
  it('reproduces the input exactly when every edit is the identity', () => {
    expect(renderNotes({ samples, sampleRate, notes })).toEqual(samples);
  });

  it('reproduces the input exactly for an empty note list', () => {
    expect(renderNotes({ samples, sampleRate, notes: [] })).toEqual(samples);
  });

  it('applies a gain edit to that note span only', () => {
    const edited = notes.map((note, index) =>
      index === 1 ? { ...note, edit: { gainDb: -20 } } : note,
    );
    const rendered = renderNotes({ samples, sampleRate, notes: edited });
    expect(rendered).toHaveLength(samples.length);

    // The untouched note passes through bit for bit.
    for (let i = 0; i < notes[0].offsetSample; i++) {
      expect(rendered[i]).toBe(samples[i]);
    }
    // Past the edge cross-fade the edited span is exactly the scaled source.
    for (let i = notes[1].onsetSample + 500; i < notes[1].offsetSample - 500; i += 97) {
      expect(rendered[i]).toBeCloseTo(samples[i] * 0.1, 5);
    }
  });

  it('silences a muted note span and leaves its neighbour alone', () => {
    const edited = notes.map((note, index) =>
      index === 0 ? { ...note, edit: { muted: true } } : note,
    );
    const rendered = renderNotes({ samples, sampleRate, notes: edited });

    // Math.abs, because erasing a negative sample leaves -0 and Object.is
    // separates that from 0.
    for (let i = 500; i < notes[0].offsetSample - 500; i++) {
      expect(Math.abs(rendered[i])).toBe(0);
    }
    for (let i = notes[1].onsetSample; i < notes[1].offsetSample; i++) {
      expect(rendered[i]).toBe(samples[i]);
    }
  });

  it('reads only the span and the edit of a hand-built note', () => {
    const hand: NoteObjectInput[] = [
      { onsetSample: 4096, offsetSample: 8192, edit: { gainDb: -20 } },
    ];
    const rendered = renderNotes({ samples, sampleRate, notes: hand });
    expect(rendered[0]).toBe(samples[0]);
    expect(rendered[6144]).toBeCloseTo(samples[6144] * 0.1, 5);
  });

  it('honours fadeMs at the edited note edges', () => {
    const edited = [{ onsetSample: 4096, offsetSample: 8192, edit: { muted: true } }];
    const shortFade = renderNotes({ samples, sampleRate, notes: edited, fadeMs: 1 });
    const longFade = renderNotes({ samples, sampleRate, notes: edited, fadeMs: 20 });
    // 1 ms is 22 samples, so the 200th sample of the span is already silent;
    // a 20 ms fade is still ramping there.
    expect(Math.abs(shortFade[4296])).toBe(0);
    expect(Math.abs(longFade[4296])).toBeGreaterThan(0);
  });

  it('rejects a malformed request', () => {
    expectInvalidParameter(() =>
      renderNotes({
        samples,
        sampleRate,
        notes: [{ offsetSample: 8192 } as unknown as NoteObjectInput],
      }),
    );
    expectInvalidParameter(() =>
      renderNotes({ samples, sampleRate, notes: [{ onsetSample: 8192, offsetSample: 4096 }] }),
    );
    expectInvalidParameter(() =>
      renderNotes({
        samples,
        sampleRate,
        notes: [
          { onsetSample: 0, offsetSample: 8192, edit: { gainDb: -3 } },
          { onsetSample: 4096, offsetSample: 12288 },
        ],
      }),
    );
    expectInvalidParameter(() => renderNotes({ samples, sampleRate, notes, fadeMs: -1 }));
    expectInvalidParameter(() =>
      renderNotes({
        samples,
        sampleRate,
        notes: [{ onsetSample: 0, offsetSample: 8192, edit: { timeStretchRatio: -1 } }],
      }),
    );
    expect(() => renderNotes({ samples, sampleRate: 7999, notes })).toThrow(RangeError);
  });
});
