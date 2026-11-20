import { beforeAll, describe, expect, it } from 'vitest';
import * as sonare from '../../dist/index.js';
import {
  Audio,
  autoTune,
  extractNotes,
  extractPercussiveEvents,
  init,
  mergeNotes,
  type NoteObject,
  noteMove,
  noteStretch,
  pitchCorrectTimevarying,
  pitchCorrectToMidi,
  pitchShift,
  renderNotes,
  renderPercussiveEvents,
  scaleCorrectionSemitones,
  scaleMaskForMode,
  scaleQuantizeMidi,
  spectralEdit,
  splitNote,
  timeStretch,
} from '../../dist/index.js';
import { unknownKeyText } from '../_helpers';

const sampleRate = 22050;
const hop = 512;
const frames = 20;

/** A 440 Hz tone, then 660 Hz, so the pitch track segments into two notes. */
function twoNoteAudio(): Float32Array {
  const out = new Float32Array(frames * hop);
  for (let i = 0; i < out.length; i++) {
    const f = i < out.length / 2 ? 440 : 660;
    out[i] = 0.3 * Math.sin((2 * Math.PI * f * i) / sampleRate);
  }
  return out;
}

function track(): {
  f0Hz: Float32Array;
  voiced: Uint8Array;
  frameRate: number;
} {
  const f0Hz = new Float32Array(frames).map((_, i) => (i < frames / 2 ? 440 : 660));
  return { f0Hz, voiced: new Uint8Array(frames).fill(1), frameRate: sampleRate / hop };
}

const samples = twoNoteAudio();

/** Runs `fn` and returns what it threw, or undefined. */
function thrown(fn: () => unknown): unknown {
  try {
    fn();
  } catch (error) {
    return error;
  }
  return undefined;
}

function maxDiff(a: Float32Array, b: Float32Array): number {
  expect(a.length).toBe(b.length);
  let worst = 0;
  for (let i = 0; i < a.length; i++) {
    worst = Math.max(worst, Math.abs(a[i] - b[i]));
  }
  return worst;
}

type Loose = (...args: unknown[]) => unknown;
const loose = (fn: unknown): Loose => fn as Loose;

/** A removed option, typed so it passes the excess-property check. */
const NOT_AN_OPTION: object = { validate: false };

describe('effects notes surface', () => {
  beforeAll(async () => {
    await init();
  });

  const { f0Hz, voiced, frameRate } = track();
  const spectralOps = [{ startSec: 0.05, endSec: 0.2, lowHz: 100, highHz: 2000, gainDb: -6 }];

  // Request-form calls with every required field except the one under test.
  const requestCalls: Array<[string, () => unknown]> = [
    ['timeStretch', () => timeStretch({ samples, speedRatio: 1.2 } as never)],
    ['pitchShift', () => pitchShift({ samples, pitchSemitones: 2 } as never)],
    [
      'pitchCorrectToMidi',
      () => pitchCorrectToMidi({ samples, currentMidi: 60, targetMidi: 62 } as never),
    ],
    ['pitchCorrectTimevarying', () => pitchCorrectTimevarying({ samples, f0Hz } as never)],
    ['autoTune', () => autoTune({ samples } as never)],
    ['spectralEdit', () => spectralEdit({ samples, ops: spectralOps } as never)],
    ['noteStretch', () => noteStretch({ samples, stretchRatio: 1.2 } as never)],
    ['noteMove', () => noteMove({ samples, targetOnsetSample: 100 } as never)],
    ['extractNotes', () => extractNotes({ samples, f0Hz, voiced, frameRate } as never)],
    ['renderNotes', () => renderNotes({ samples, notes: [] } as never)],
    [
      'splitNote',
      () => splitNote({ samples, f0Hz, voiced, frameRate, notes: [], index: 0, frame: 3 } as never),
    ],
    [
      'mergeNotes',
      () => mergeNotes({ samples, f0Hz, voiced, frameRate, notes: [], first: 0, last: 1 } as never),
    ],
    ['extractPercussiveEvents', () => extractPercussiveEvents({ samples } as never)],
    ['renderPercussiveEvents', () => renderPercussiveEvents({ samples, events: [] } as never)],
  ];

  it.each(requestCalls)('requires sampleRate on %s', (_name, call) => {
    expect(thrown(call)).toBeInstanceOf(TypeError);
  });

  it.each([
    ['timeStretch', () => timeStretch({ samples, sampleRate } as never)],
    ['pitchShift', () => pitchShift({ samples, sampleRate } as never)],
    [
      'pitchCorrectToMidi without currentMidi',
      () => pitchCorrectToMidi({ samples, sampleRate, targetMidi: 62 } as never),
    ],
    [
      'pitchCorrectToMidi without targetMidi',
      () => pitchCorrectToMidi({ samples, sampleRate, currentMidi: 60 } as never),
    ],
    ['noteStretch', () => noteStretch({ samples, sampleRate } as never)],
    ['noteMove', () => noteMove({ samples, sampleRate } as never)],
    ['spectralEdit', () => spectralEdit({ samples, sampleRate } as never)],
    ['positional timeStretch', () => loose(timeStretch)(samples, sampleRate)],
    ['positional pitchShift', () => loose(pitchShift)(samples, sampleRate)],
    ['positional pitchCorrectToMidi', () => loose(pitchCorrectToMidi)(samples, sampleRate)],
    ['positional noteStretch', () => loose(noteStretch)(samples, sampleRate)],
    ['positional noteMove', () => loose(noteMove)(samples, sampleRate)],
    ['positional spectralEdit', () => loose(spectralEdit)(samples, sampleRate)],
  ])('requires the defining parameter of %s', (_name, call) => {
    expect(thrown(call)).toBeInstanceOf(TypeError);
  });

  it.each([
    ['timeStretch rate', () => loose(timeStretch)({ samples, sampleRate, rate: 1.2 })],
    ['pitchShift semitones', () => loose(pitchShift)({ samples, sampleRate, semitones: 2 })],
    ['timeStretch positional number', () => loose(timeStretch)(samples, sampleRate, 1.2)],
    ['pitchShift positional number', () => loose(pitchShift)(samples, sampleRate, 2)],
    [
      'pitchCorrectToMidi positional numbers',
      () => loose(pitchCorrectToMidi)(samples, sampleRate, 60, 62),
    ],
    [
      'pitchCorrectTimevarying mode',
      () => loose(pitchCorrectTimevarying)({ samples, sampleRate, f0Hz, mode: 'scale' }),
    ],
    [
      'pitchCorrectTimevarying referenceMidi',
      () => loose(pitchCorrectTimevarying)({ samples, sampleRate, f0Hz, referenceMidi: 69 }),
    ],
    [
      'pitchCorrectTimevarying positional f0Hz',
      () => loose(pitchCorrectTimevarying)(samples, f0Hz, sampleRate, 512),
    ],
    ['autoTune referenceMidi', () => loose(autoTune)({ samples, sampleRate, referenceMidi: 69 })],
    ['autoTune strength', () => loose(autoTune)({ samples, sampleRate, strength: 0.5 })],
    [
      'spectralEdit positional ops list',
      () => loose(spectralEdit)(samples, sampleRate, spectralOps),
    ],
    [
      'noteStretch positional options without rate',
      () => loose(noteStretch)(samples, 'x', { stretchRatio: 1 }),
    ],
    ['splitNote old frame-only positional', () => loose(splitNote)(samples, sampleRate, f0Hz)],
  ])('refuses the removed shape of %s', (_name, call) => {
    expect(thrown(call)).toBeInstanceOf(TypeError);
  });

  it('removes pitchCorrectToMidiTimevarying', () => {
    expect((sonare as Record<string, unknown>).pitchCorrectToMidiTimevarying).toBeUndefined();
  });

  it('refuses a removed note-request name at the type level', () => {
    const typesOnly = () => {
      // @ts-expect-error NoteTrackRequest is now ExtractNotesRequest
      type Removed = sonare.NoteTrackRequest;
      return undefined as unknown as Removed;
    };
    expect(typeof typesOnly).toBe('function');
  });

  it.each([
    [
      'timeStretch',
      () => timeStretch({ samples, sampleRate, speedRatio: 1.25 }),
      () => timeStretch(samples, sampleRate, { speedRatio: 1.25 }),
    ],
    [
      'pitchShift',
      () => pitchShift({ samples, sampleRate, pitchSemitones: 3 }),
      () => pitchShift(samples, sampleRate, { pitchSemitones: 3 }),
    ],
    [
      'pitchCorrectToMidi',
      () => pitchCorrectToMidi({ samples, sampleRate, currentMidi: 60, targetMidi: 62 }),
      () => pitchCorrectToMidi(samples, sampleRate, { currentMidi: 60, targetMidi: 62 }),
    ],
    [
      'pitchCorrectTimevarying',
      () =>
        pitchCorrectTimevarying({
          samples,
          sampleRate,
          f0Hz,
          targetMode: 'scale',
          scaleReferenceMidi: 69,
          voiced,
        }),
      () =>
        pitchCorrectTimevarying(samples, sampleRate, {
          f0Hz,
          targetMode: 'scale',
          scaleReferenceMidi: 69,
          voiced,
        }),
    ],
    [
      'noteStretch',
      () => noteStretch({ samples, sampleRate, onsetSec: 0.05, offsetSec: 0.3, stretchRatio: 1.5 }),
      () => noteStretch(samples, sampleRate, { onsetSec: 0.05, offsetSec: 0.3, stretchRatio: 1.5 }),
    ],
    [
      'noteMove',
      () => noteMove({ samples, sampleRate, onsetSec: 0.05, offsetSec: 0.2, targetOnsetSec: 0.1 }),
      () => noteMove(samples, sampleRate, { onsetSec: 0.05, offsetSec: 0.2, targetOnsetSec: 0.1 }),
    ],
    [
      'spectralEdit',
      () => spectralEdit({ samples, sampleRate, ops: spectralOps }),
      () => spectralEdit(samples, sampleRate, { ops: spectralOps }),
    ],
  ])('positional form equals request form for %s', (_name, requestForm, positionalForm) => {
    const a = requestForm() as Float32Array;
    const b = positionalForm() as Float32Array;
    expect(maxDiff(a, b)).toBeLessThan(1e-6);
  });

  it('positional form equals request form for autoTune', () => {
    const key = { root: 9, mode: 'minor' } as const;
    const a = autoTune({ samples, sampleRate, key, scaleReferenceMidi: 69, retuneAmount: 0.5 });
    const b = autoTune(samples, sampleRate, { key, scaleReferenceMidi: 69, retuneAmount: 0.5 });
    expect(maxDiff(a.samples, b.samples)).toBeLessThan(1e-6);
    expect(a.key).toEqual(b.key);
  });

  it('positional form equals request form for the note operations', () => {
    const options = { f0Hz, voiced, frameRate, minNoteMs: 10 };
    const a = extractNotes({ samples, sampleRate, ...options });
    const b = extractNotes(samples, sampleRate, options);
    expect(a.length).toBeGreaterThan(1);
    expect(b).toEqual(a);

    const renderedA = renderNotes({ samples, sampleRate, notes: a });
    const renderedB = renderNotes(samples, sampleRate, { notes: a });
    expect(maxDiff(renderedA, renderedB)).toBeLessThan(1e-6);

    const frame = a[0].frameStart + 5;
    const setOptions = { ...options, notes: a, index: 0 };
    const splitA = splitNote({ samples, sampleRate, ...setOptions, frame });
    const splitB = splitNote(samples, sampleRate, { ...setOptions, frame });
    expect(splitB).toEqual(splitA);

    const mergeA = mergeNotes({
      samples,
      sampleRate,
      ...options,
      notes: splitA,
      first: 0,
      last: 1,
    });
    const mergeB = mergeNotes(samples, sampleRate, {
      ...options,
      notes: splitA,
      first: 0,
      last: 1,
    });
    expect(mergeB).toEqual(mergeA);
  });

  it('positional form equals request form for the percussive events', () => {
    const a = extractPercussiveEvents({ samples, sampleRate });
    const b = extractPercussiveEvents(samples, sampleRate);
    const c = extractPercussiveEvents(Array.from(samples), sampleRate);
    expect(b).toEqual(a);
    expect(c).toEqual(a);
    const renderedA = renderPercussiveEvents({ samples, sampleRate, events: a });
    const renderedB = renderPercussiveEvents(samples, sampleRate, { events: a });
    expect(maxDiff(renderedA, renderedB)).toBeLessThan(1e-6);
  });

  it('splitNote takes exactly one of frame and frameSec', () => {
    const options = { f0Hz, voiced, frameRate, minNoteMs: 10 };
    const notes = extractNotes({ samples, sampleRate, ...options });
    const frame = notes[0].frameStart + 5;
    const base = { samples, sampleRate, ...options, notes, index: 0 };
    expect(thrown(() => splitNote({ ...base, frame, frameSec: frame / frameRate }))).toBeInstanceOf(
      RangeError,
    );
    expect(() => splitNote({ ...base })).toThrow(
      new TypeError('splitNote: give frame or frameSec'),
    );
    const byFrame = splitNote({ ...base, frame });
    const bySec = splitNote({ ...base, frameSec: frame / frameRate });
    expect(bySec).toEqual(byFrame);
  });

  it('splitNote rounds frameSec to the nearest track frame', () => {
    const options = { f0Hz, voiced, frameRate, minNoteMs: 10 };
    const notes = extractNotes({ samples, sampleRate, ...options });
    const frame = notes[0].frameStart + 5;
    const base = { samples, sampleRate, ...options, notes, index: 0 };
    const nearby = splitNote({ ...base, frameSec: (frame + 0.3) / frameRate });
    expect(nearby).toEqual(splitNote({ ...base, frame }));
  });

  it.each([
    ['targetMode number', 1, TypeError],
    ['targetMode synonym', 'MIDI', RangeError],
    ['targetMode unknown', 'chromatic', RangeError],
  ])('refuses a closed-set value: pitchCorrectTimevarying %s', (_name, targetMode, errorClass) => {
    const error = thrown(() =>
      loose(pitchCorrectTimevarying)({ samples, sampleRate, f0Hz, targetMode }),
    );
    expect(error).toBeInstanceOf(errorClass);
  });

  it('pitchCorrectTimevarying covers the midi target without a separate entry', () => {
    const out = pitchCorrectTimevarying({
      samples,
      sampleRate,
      f0Hz,
      targetMode: 'midi',
      targetMidi: 72,
    });
    expect(out.length).toBe(samples.length);
  });

  it('exports no numeric Mode: a key mode is a KeyMode name', async () => {
    expect('Mode' in sonare).toBe(false);
    expect('Mode' in (await import('../../dist/analysis.js'))).toBe(false);
  });

  it.each([
    ['scaleMaskForMode number mode', () => loose(scaleMaskForMode)(0, 0), TypeError],
    ['scaleMaskForMode Mode ordinal', () => loose(scaleMaskForMode)(0, 1), TypeError],
    ['scaleMaskForMode unknown token', () => loose(scaleMaskForMode)(0, 'ionian'), RangeError],
    ['scaleMaskForMode synonym', () => loose(scaleMaskForMode)(0, 'Major'), RangeError],
  ])('refuses a closed-set value: %s', (_name, call, errorClass) => {
    expect(thrown(call)).toBeInstanceOf(errorClass);
  });

  it('scale helpers keep their positional scalar form', () => {
    const major = scaleMaskForMode(0, 'major');
    expect(major).toBe(0b101010110101);
    expect(scaleQuantizeMidi(0, major, 61, 69)).toBe(60);
    expect(scaleQuantizeMidi(0, major, 61)).toBe(scaleQuantizeMidi(0, major, 61, 69));
    expect(Number.isFinite(scaleCorrectionSemitones(0, major, 61, 69))).toBe(true);
    expect(thrown(() => scaleQuantizeMidi(0, major, Number.NaN, 69))).toBeInstanceOf(RangeError);
    expect(thrown(() => scaleQuantizeMidi(0, major, 61, Number.NaN))).toBeInstanceOf(RangeError);
    expect(thrown(() => scaleCorrectionSemitones(0, major, 61, Number.NaN))).toBeInstanceOf(
      RangeError,
    );
  });

  it('scale helpers: an absent scaleReferenceMidi is A4 and an explicit 0 is MIDI note 0', () => {
    const major = scaleMaskForMode(0, 'major');
    for (const fn of [scaleQuantizeMidi, scaleCorrectionSemitones]) {
      const absent = fn(0, major, 60.4);
      expect(fn(0, major, 60.4, 69)).toBe(absent);
      expect(fn(0, major, 60.4, 69.5)).not.toBe(absent);
      // A whole-note anchor lands on the same semitone grid as A4, so a 0 read as a
      // note answers as A4 does, and its half-note neighbour moves the grid.
      expect(fn(0, major, 60.4, 0)).toBe(absent);
      expect(fn(0, major, 60.4, 0.5)).toBe(fn(0, major, 60.4, 69.5));
      expect(thrown(() => fn(0, major, 60.4, -1))).toBeInstanceOf(RangeError);
      expect(thrown(() => loose(fn)(0, major, 60.4, '69'))).toBeInstanceOf(TypeError);
    }
  });

  it('autoTune names its key and scale reference in the options', () => {
    const named = autoTune({
      samples,
      sampleRate,
      key: { root: 0, mode: 'major' },
      scaleReferenceMidi: 69,
    });
    expect(named.samples.length).toBe(samples.length);
    expect(
      thrown(() => loose(autoTune)({ samples, sampleRate, key: { root: 12, mode: 'major' } })),
    ).toBeInstanceOf(RangeError);
    expect(
      thrown(() => loose(autoTune)({ samples, sampleRate, key: { root: 0, mode: 1 } })),
    ).toBeInstanceOf(TypeError);
    expect(thrown(() => loose(autoTune)({ samples, sampleRate, key: 'C' }))).toBeInstanceOf(
      TypeError,
    );
  });

  it('spectralEdit with an empty ops list returns a copy', () => {
    const out = spectralEdit({ samples, sampleRate, ops: [] });
    expect(out).not.toBe(samples);
    expect(maxDiff(out, samples)).toBeLessThan(1e-3);
  });

  it('spectralEdit takes a window by name only', () => {
    expect(spectralEdit({ samples, sampleRate, ops: [], window: 'blackman' }).length).toBe(
      samples.length,
    );
    for (const ordinal of [0, 1, 2, 3]) {
      expect(
        thrown(() => loose(spectralEdit)({ samples, sampleRate, ops: [], window: ordinal })),
      ).toBeInstanceOf(TypeError);
    }
    expect(
      thrown(() => loose(spectralEdit)({ samples, sampleRate, ops: [], window: 'rect' })),
    ).toBeInstanceOf(RangeError);
  });

  it.each<[string, (s: Float32Array, x: object) => unknown]>([
    [
      'timeStretch',
      (s: Float32Array, x: object) =>
        timeStretch({ samples: s, sampleRate, speedRatio: 1.1, ...x }),
    ],
    [
      'pitchShift',
      (s: Float32Array, x: object) =>
        pitchShift({ samples: s, sampleRate, pitchSemitones: 1, ...x }),
    ],
    [
      'pitchCorrectToMidi',
      (s: Float32Array, x: object) =>
        pitchCorrectToMidi({
          samples: s,
          sampleRate,
          currentMidi: 60,
          targetMidi: 61,
          ...x,
        }),
    ],
    [
      'noteStretch',
      (s: Float32Array, x: object) =>
        noteStretch({ samples: s, sampleRate, stretchRatio: 1.1, ...x }),
    ],
    [
      'noteMove',
      (s: Float32Array, x: object) =>
        noteMove({ samples: s, sampleRate, targetOnsetSample: 10, ...x }),
    ],
    [
      'spectralEdit',
      (s: Float32Array, x: object) => spectralEdit({ samples: s, sampleRate, ops: [], ...x }),
    ],
    [
      'pitchCorrectTimevarying',
      (s: Float32Array, x: object) =>
        pitchCorrectTimevarying({ samples: s, sampleRate, f0Hz, ...x }),
    ],
    ['autoTune', (s: Float32Array, x: object) => autoTune({ samples: s, sampleRate, ...x })],
    [
      'renderNotes',
      (s: Float32Array, x: object) => renderNotes({ samples: s, sampleRate, notes: [], ...x }),
    ],
    [
      'renderPercussiveEvents',
      (s: Float32Array, x: object) =>
        renderPercussiveEvents({ samples: s, sampleRate, events: [], ...x }),
    ],
    [
      'extractPercussiveEvents',
      (s: Float32Array, x: object) => extractPercussiveEvents({ samples: s, sampleRate, ...x }),
    ],
  ])('%s refuses a validate key and non-finite samples', (name, call) => {
    expect(() => call(samples, NOT_AN_OPTION)).toThrow(
      unknownKeyText(`${name}: unknown key validate`),
    );
    const bad = Float32Array.from(samples);
    bad[10] = Number.NaN;
    expect(() => call(bad, {})).toThrow(RangeError);
  });

  it.each([
    [
      'timeStretch',
      () =>
        timeStretch({
          samples: new Float32Array([Number.NaN, 0, 0, 0]),
          sampleRate,
          speedRatio: 1,
        }),
    ],
    [
      'pitchShift',
      () =>
        pitchShift({
          samples: new Float32Array([Number.NaN, 0, 0, 0]),
          sampleRate,
          pitchSemitones: 1,
        }),
    ],
    [
      'noteMove',
      () =>
        noteMove({
          samples: new Float32Array([Number.NaN, 0, 0, 0]),
          sampleRate,
          targetOnsetSample: 1,
        }),
    ],
  ])('scans for non-finite samples by default: %s', (_name, call) => {
    expect(thrown(call)).toBeInstanceOf(RangeError);
  });

  it('exposes the note types readonly', () => {
    const typesOnly = (note: NoteObject) => {
      // @ts-expect-error NoteObject fields are readonly
      note.medianHz = 1;
      // @ts-expect-error NoteObject.edit is readonly
      note.edit = { ...note.edit };
      // @ts-expect-error NoteEdit fields are readonly
      note.edit.gainDb = -6;
    };
    expect(typeof typesOnly).toBe('function');
  });

  it('edits a note by rendering a copy', () => {
    const notes = extractNotes({ samples, sampleRate, f0Hz, voiced, frameRate, minNoteMs: 10 });
    const muted = notes.map((note, i) =>
      i === 0 ? { ...note, edit: { ...note.edit, muted: true } } : note,
    );
    const out = renderNotes({ samples, sampleRate, notes: muted });
    expect(out.length).toBe(samples.length);
    expect(notes[0].edit.muted).toBe(false);
  });

  describe('Audio', () => {
    const audio = Audio.fromBuffer(samples, sampleRate);

    it('timeStretch takes its request fields', () => {
      const viaMethod = audio.timeStretch({ speedRatio: 1.25 });
      expect(
        maxDiff(viaMethod, timeStretch({ samples, sampleRate, speedRatio: 1.25 })),
      ).toBeLessThan(1e-6);
    });

    it('pitchShift takes its request fields', () => {
      const viaMethod = audio.pitchShift({ pitchSemitones: 3 });
      expect(
        maxDiff(viaMethod, pitchShift({ samples, sampleRate, pitchSemitones: 3 })),
      ).toBeLessThan(1e-6);
    });

    it('noteStretch and noteMove take their option objects', () => {
      expect(
        maxDiff(
          audio.noteStretch({ stretchRatio: 1.5, offsetSec: 0.2 }),
          noteStretch({ samples, sampleRate, stretchRatio: 1.5, offsetSec: 0.2 }),
        ),
      ).toBeLessThan(1e-6);
      expect(
        maxDiff(
          audio.noteMove({ onsetSec: 0.05, offsetSec: 0.2, targetOnsetSec: 0.1 }),
          noteMove({ samples, sampleRate, onsetSec: 0.05, offsetSec: 0.2, targetOnsetSec: 0.1 }),
        ),
      ).toBeLessThan(1e-6);
    });

    it('takes option objects for the separation, normalize, trim and voice methods', () => {
      expect(audio.hpss({}).harmonic.length).toBe(samples.length);
      expect(audio.harmonic({}).length).toBe(samples.length);
      expect(audio.percussive({}).length).toBe(samples.length);
      expect(audio.normalize({}).length).toBe(samples.length);
      expect(audio.trim({}).length).toBeLessThanOrEqual(samples.length);
      expect(audio.voiceChange({}).length).toBeGreaterThan(0);
    });

    it.each([
      ['hpss', () => loose(audio.hpss.bind(audio))(31, 31)],
      ['timeStretch', () => loose(audio.timeStretch.bind(audio))(1.2)],
      ['pitchShift', () => loose(audio.pitchShift.bind(audio))(2)],
      ['pitchCorrectToMidi', () => loose(audio.pitchCorrectToMidi.bind(audio))(60, 62)],
      ['normalize', () => loose(audio.normalize.bind(audio))(-3)],
      ['trim', () => loose(audio.trim.bind(audio))(-60)],
      ['noteMove without a target', () => loose(audio.noteMove.bind(audio))({})],
      ['noteStretch without a ratio', () => loose(audio.noteStretch.bind(audio))({})],
    ])('refuses the removed positional form of Audio.%s', (_name, call) => {
      expect(thrown(call)).toBeInstanceOf(TypeError);
    });
  });
});
