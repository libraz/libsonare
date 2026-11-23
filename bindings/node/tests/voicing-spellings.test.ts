/**
 * `voiced` and `voicedProb` are two spellings of one input, so every entry reading a voicing track
 * refuses both given together, worded as the C ABI words the relation. The note entries also refuse
 * both omitted; pitch correction reads that as every frame voiced.
 */

import { describe, expect, it } from 'vitest';
import {
  extractNotes,
  mergeNotes,
  noteSegments,
  pitchCorrectTimevarying,
  splitNote,
} from '../src/index.js';
import { addon } from '../src/native.js';

/* biome-ignore lint/suspicious/noExplicitAny: the addon is untyped here on purpose. */
const native = addon as any;

const caught = (run: () => unknown): Error => {
  try {
    run();
  } catch (error) {
    return error as Error;
  }
  throw new Error('expected the call to throw');
};

const frames = 10;
const f0Hz = new Float32Array(frames).fill(220);
const voiced = new Int32Array(frames).fill(1);
const voicedProb = new Float32Array(frames).fill(1);
const track = { samples: new Float32Array(11025), sampleRate: 22050, f0Hz, frameRate: 40 };
const notes = [
  { onsetSample: 0, offsetSample: 2000, frameStart: 0, frameEnd: 4, medianHz: 220 },
  { onsetSample: 2000, offsetSample: 5000, frameStart: 4, frameEnd: 10, medianHz: 220 },
];

type Voicing = { voiced?: Int32Array; voicedProb?: Float32Array };

const entries: ReadonlyArray<readonly [string, (voicing: Voicing) => unknown]> = [
  ['extractNotes', (voicing) => extractNotes({ ...track, ...voicing } as never)],
  [
    'splitNote',
    (voicing) => splitNote({ ...track, ...voicing, notes, index: 1, frame: 7 } as never),
  ],
  [
    'mergeNotes',
    (voicing) => mergeNotes({ ...track, ...voicing, notes, first: 0, last: 1 } as never),
  ],
  ['noteSegments', (voicing) => noteSegments({ f0Hz, frameRate: 40, ...voicing } as never)],
];

describe('voiced and voicedProb are one input', () => {
  it.each(entries)('%s refuses both given together', (entry, call) => {
    const error = caught(() => call({ voiced, voicedProb }));
    expect(error).toBeInstanceOf(TypeError);
    expect(error.message).toBe(`${entry}: voiced must be omitted when voicedProb is given`);
  });

  it.each(entries)('%s refuses both omitted', (entry, call) => {
    const error = caught(() => call({}));
    expect(error).toBeInstanceOf(TypeError);
    expect(error.message).toBe(`${entry}: voicedProb and voiced must not both be omitted`);
  });

  it.each(entries)('%s reads either one alone', (_entry, call) => {
    expect(() => call({ voiced })).not.toThrow();
    expect(() => call({ voicedProb })).not.toThrow();
  });
});

describe('pitch correction takes at most one of voiced and voicedProb', () => {
  const correct = (voicing: Voicing) =>
    pitchCorrectTimevarying({
      samples: track.samples,
      sampleRate: track.sampleRate,
      f0Hz,
      hopLength: 512,
      ...voicing,
    } as never);

  it('refuses both given together', () => {
    const error = caught(() => correct({ voiced, voicedProb }));
    expect(error).toBeInstanceOf(TypeError);
    expect(error.message).toBe(
      'pitchCorrectTimevarying: voiced must be omitted when voicedProb is given',
    );
  });

  it('reads every frame as voiced when both are omitted, and either one alone', () => {
    expect(correct({})).toHaveLength(track.samples.length);
    expect(correct({ voiced })).toEqual(correct({}));
    expect(correct({ voicedProb })).toEqual(correct({}));
  });

  it('refuses both on the raw MIDI-target entry, which bypasses the C ABI', () => {
    const error = caught(() =>
      native.pitchCorrectToMidiTimevarying(
        track.samples,
        track.sampleRate,
        f0Hz,
        69,
        512,
        voiced,
        voicedProb,
      ),
    );
    expect(error).toBeInstanceOf(TypeError);
    expect(error.message).toMatch(/voiced must be omitted when voicedProb is given$/);
  });
});
