import { describe, expect, it } from 'vitest';
import { ErrorCode, PlaybackRenderer, Project } from '../src/index.js';
import { expectRefusal } from './_helpers.js';

// A MIDI-FX config's wrong-typed field is a plain TypeError from the core; the facade only
// serializes it, so a value of the wrong kind must arrive and be refused there.
// Other document content (the playback renderer config below) is still InvalidFormat.
const WRONG_KINDS: [string, unknown][] = [
  ['a string', '12'],
  ['a boolean', true],
  ['null', null],
  ['NaN (serialized as null)', Number.NaN],
];

describe('document values of the wrong kind', () => {
  it.each(WRONG_KINDS)('refuses %s on a MIDI-FX number field as a TypeError', (_kind, value) => {
    const project = new Project();
    try {
      const { clipId } = project.addMidiClip(0, 4);
      for (const key of ['transposeSemitones', 'velocityScale', 'seed', 'quantizePpq']) {
        expectRefusal(
          () => project.previewMidiFxCount({ clipId, config: { [key]: value } as never }),
          TypeError,
          new RegExp(`^Project\\.previewMidiFxCount: config\\.${key} must be a number$`),
        );
      }
    } finally {
      project.destroy();
    }
  });
});

describe('an unset playback renderer value is an absent key', () => {
  const NULLISH: [string, unknown][] = [
    ['null', null],
    ['NaN (serialized as null)', Number.NaN],
  ];
  const refusal = (message: string) =>
    expect.objectContaining({
      code: ErrorCode.InvalidFormat,
      message: expect.stringContaining(message),
    });
  const create = (config: object) => new PlaybackRenderer({ sampleRate: 48000, config });

  it.each(NULLISH)('refuses %s on loudness.programLufs', (_kind, value) => {
    expect(() => create({ loudness: { programLufs: value } })).toThrow(
      refusal('loudness.programLufs must be a number'),
    );
  });

  it.each(NULLISH)('refuses %s on a speaker distanceM', (_kind, value) => {
    expect(() =>
      create({
        target: { kind: 'speakers', layout: 'stereo', speakers: { left: { distanceM: value } } },
      }),
    ).toThrow(refusal('target.speakers.left.distanceM must be a number'));
  });

  it('refuses null on input.channelMap', () => {
    expect(() => create({ input: { layout: 'stereo', channelMap: null } })).toThrow(
      refusal('input.channelMap must be an array'),
    );
  });

  it('refuses the old outputLimiter.ceilingDbtp key', () => {
    expect(() => create({ outputLimiter: { ceilingDbtp: -1 } })).toThrow(
      refusal('unknown key outputLimiter.ceilingDbtp'),
    );
    const speakers = { kind: 'speakers', layout: 'stereo' };
    expect(() =>
      create({ target: speakers, outputLimiter: { ceilingDb: -1 } }).destroy(),
    ).not.toThrow();
  });
});
