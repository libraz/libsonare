import { beforeAll, describe, expect, it } from 'vitest';
import {
  init,
  Mixer,
  mergeNotes,
  renderNotes,
  renderPercussiveEvents,
  splitNote,
} from '../dist/index.js';

beforeAll(async () => {
  await init();
});

// A refusal about one element of a list names it by its index and the caller's field names, the
// same text on every surface, and a strip reference is refused by the name the caller passed.
const SR = 22050;
const FR = SR / 256;
const samples = Float32Array.from(
  { length: SR },
  (_, i) => 0.5 * Math.sin((2 * Math.PI * 220 * i) / SR),
);
const frameCount = Math.floor(SR / 256) + 1;
const f0Hz = new Float32Array(frameCount).fill(220);
const track = {
  samples,
  sampleRate: SR,
  f0Hz,
  frameRate: FR,
  voiced: new Uint8Array(frameCount).fill(1),
};
const noteSet = [
  { frameStart: 0, frameEnd: 20, edit: {} },
  { frameStart: 20, frameEnd: 40, edit: {} },
  { frameStart: 40, frameEnd: 60, edit: {} },
];
const spans = [
  { onsetSample: 0, offsetSample: 5000, edit: {} },
  { onsetSample: 5000, offsetSample: 10000, edit: {} },
];
// biome-ignore lint/suspicious/noExplicitAny: each case writes a field of the wrong type on purpose.
type Row = Record<string, any>;
const patched = (rows: Row[], patch: Row): Row[] =>
  rows.map((row, i) => (i === 1 ? { ...row, ...patch } : row));
const edited = (rows: Row[], edit: Row): Row[] => patched(rows, { edit });

// biome-ignore lint/suspicious/noExplicitAny: the cases pass malformed rows through the typed facade.
const loose = (value: unknown): any => value;

function refusal(call: () => unknown): Error {
  try {
    call();
  } catch (error) {
    return error as Error;
  }
  throw new Error('expected a refusal');
}

describe('a refusal about a list element names it by its index', () => {
  const cases: ReadonlyArray<readonly [string, () => unknown, ErrorConstructor, string]> = [
    [
      'a note edit field out of its domain',
      () =>
        mergeNotes({
          ...track,
          notes: loose(edited(noteSet, { timeStretchRatio: 0 })),
          first: 0,
          last: 1,
        }),
      RangeError,
      'mergeNotes: notes[1].edit.timeStretchRatio must be finite and greater than 0',
    ],
    [
      'a note edit field of the wrong type',
      () =>
        mergeNotes({
          ...track,
          notes: loose(edited(noteSet, { pitchSemitones: 'x' })),
          first: 0,
          last: 1,
        }),
      TypeError,
      'mergeNotes: notes[1].edit.pitchSemitones must be a number',
    ],
    [
      'a note edit the renderer could not apply',
      () =>
        mergeNotes({
          ...track,
          notes: loose(edited(noteSet, { amplitudeEnvelope: Float32Array.from([1, -1]) })),
          first: 0,
          last: 1,
        }),
      RangeError,
      'mergeNotes: notes[1].edit must hold finite values, a positive time-stretch ratio and a non-negative envelope whose product with the gain stays finite',
    ],
    [
      'an absent note frame bound',
      () =>
        mergeNotes({
          ...track,
          notes: loose(patched(noteSet, { frameStart: undefined })),
          first: 0,
          last: 1,
        }),
      TypeError,
      'mergeNotes: notes[1].frameStart must be an integer',
    ],
    [
      'a fractional note frame bound',
      () =>
        mergeNotes({
          ...track,
          notes: loose(patched(noteSet, { frameStart: 1.5 })),
          first: 0,
          last: 1,
        }),
      RangeError,
      'mergeNotes: notes[1].frameStart must be an integer in [-2147483648, 2147483647]',
    ],
    [
      'a split note edit field out of its domain',
      () =>
        splitNote({
          ...track,
          notes: loose(edited(noteSet, { timeStretchRatio: 0 })),
          index: 1,
          frame: 30,
        }),
      RangeError,
      'splitNote: notes[1].edit.timeStretchRatio must be finite and greater than 0',
    ],
    [
      'an absent rendered note sample bound',
      () =>
        renderNotes({
          samples,
          sampleRate: SR,
          notes: loose(patched(spans, { onsetSample: undefined })),
        }),
      TypeError,
      'renderNotes: notes[1].onsetSample must be an integer',
    ],
    [
      'a fractional rendered note sample bound',
      () =>
        renderNotes({
          samples,
          sampleRate: SR,
          notes: loose(patched(spans, { onsetSample: 1.5 })),
        }),
      RangeError,
      'renderNotes: notes[1].onsetSample must be an integer in [-9007199254740991, 9007199254740991]',
    ],
    [
      'a rendered note edit field of the wrong type',
      () => renderNotes({ samples, sampleRate: SR, notes: loose(edited(spans, { muted: 1 })) }),
      TypeError,
      'renderNotes: notes[1].edit.muted must be a boolean',
    ],
    [
      'a rendered note frame span past the track',
      () =>
        renderNotes({
          samples,
          sampleRate: SR,
          f0Hz,
          frameRate: FR,
          notes: loose(patched(spans, { frameStart: 0, frameEnd: 9999 })),
        }),
      RangeError,
      'renderNotes: notes[1].frameEnd must not exceed f0Hz.length',
    ],
    [
      'a rendered note frame span ending before it starts',
      () =>
        renderNotes({
          samples,
          sampleRate: SR,
          f0Hz,
          frameRate: FR,
          notes: loose(patched(spans, { frameStart: 30, frameEnd: 10 })),
        }),
      RangeError,
      'renderNotes: notes[1].frameEnd must not be less than notes[1].frameStart',
    ],
    [
      'an absent percussive event sample bound',
      () =>
        renderPercussiveEvents({
          samples,
          sampleRate: SR,
          events: loose([{ onsetSample: 0, offsetSample: 4410 }, { offsetSample: 9000 }]),
        }),
      TypeError,
      'renderPercussiveEvents: events[1].onsetSample must be an integer',
    ],
    [
      'a percussive event edit field of the wrong type',
      () =>
        renderPercussiveEvents({
          samples,
          sampleRate: SR,
          events: loose([{ onsetSample: 0, offsetSample: 4410, edit: { gainDb: 'x' } }]),
        }),
      TypeError,
      'renderPercussiveEvents: events[0].edit.gainDb must be a number',
    ],
  ];

  it.each(cases)('%s', (_name, call, kind, text) => {
    const error = refusal(call);
    expect(error).toBeInstanceOf(kind);
    expect(error.message).toBe(text);
  });

  it('accepts the same rows once the refused field is corrected', () => {
    // The control: the base rows are accepted, so each refusal above is about the one field.
    expect(mergeNotes({ ...track, notes: noteSet, first: 0, last: 1 }).length).toBe(2);
    expect(renderNotes({ samples, sampleRate: SR, notes: spans }).length).toBe(samples.length);
  });
});

describe('a strip reference is refused by the name the caller passed', () => {
  const mixer = (): Mixer => {
    const built = new Mixer({ sampleRate: 48000, maxBlockSize: 128 });
    built.addStrip('a');
    return built;
  };

  it('states the index domain of the strips the mixer holds', () => {
    const m = mixer();
    expect(() => m.setFaderDb(99, 0)).toThrow(
      new RangeError('Mixer.setFaderDb: strip must be an integer in [0, 0]'),
    );
    expect(() => m.setSendDb(99, 0, 0)).toThrow(
      new RangeError('Mixer.setSendDb: strip must be an integer in [0, 0]'),
    );
    expect(() => m.settle(99)).toThrow(
      new RangeError('Mixer.settle: strip must be an integer in [0, 0]'),
    );
  });

  it('refuses a reference outside the index type and an unknown id', () => {
    const m = mixer();
    expect(() => m.setFaderDb(1.5, 0)).toThrow(
      new RangeError('Mixer.setFaderDb: strip must be an integer in [0, 4294967295]'),
    );
    expect(() => m.setFaderDb('zz', 0)).toThrow(
      new RangeError("Mixer.setFaderDb: no strip with id 'zz'"),
    );
    expect(() => m.setFaderDb(loose(true), 0)).toThrow(TypeError);
  });

  it('accepts the strip by index and by id', () => {
    const m = mixer();
    expect(() => m.setFaderDb(0, -3)).not.toThrow();
    expect(() => m.setFaderDb('a', -3)).not.toThrow();
  });
});
