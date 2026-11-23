/**
 * A refusal raised below the facade names the field the caller wrote, never the core's own
 * parameter name.
 */

import { beforeAll, describe, expect, it } from 'vitest';
import { Audio, decodeChannels, init, Project, timeStretch } from '../dist/index.js';

const caught = (run: () => unknown): Error => {
  try {
    run();
  } catch (error) {
    return error as Error;
  }
  throw new Error('expected the call to throw');
};

beforeAll(async () => {
  await init();
});

const SR = 22050;
const samples = new Float32Array(SR).fill(0.1);

describe('timeStretch', () => {
  it('names speedRatio when the ratio is below the computed floor', () => {
    const error = caught(() => timeStretch({ samples, sampleRate: SR, speedRatio: 1e-9 }));
    expect(error).toBeInstanceOf(RangeError);
    expect(error.message).toMatch(/^timeStretch: speedRatio must be finite and in \[/);
  });
});

describe('the encoded-audio entries name their bytes data, as Node does', () => {
  it('decodeChannels refuses empty bytes as data', () => {
    const error = caught(() => decodeChannels(new Uint8Array(0)));
    expect(error).toBeInstanceOf(RangeError);
    expect(error.message).toBe('decodeChannels: data must not be empty');
  });

  it('Audio.fromMemory refuses empty bytes as empty, not as null', () => {
    const error = caught(() => Audio.fromMemory(new Uint8Array(0)));
    expect(error).toBeInstanceOf(RangeError);
    expect(error.message).toBe('Audio.fromMemory: data must not be empty');
  });

  it.each([
    ['decodeChannels', () => decodeChannels('wav' as never)],
    ['Audio.fromMemory', () => Audio.fromMemory('wav' as never)],
  ])('%s names a value of the wrong type data', (entry, call) => {
    const error = caught(call);
    expect(error).toBeInstanceOf(TypeError);
    expect(error.message).toMatch(new RegExp(`^${entry.replace('.', '\\.')}: data `));
  });
});

describe('a project descriptor refused by the native library', () => {
  it.each([
    ['startPpq', { startPpq: -1, lengthPpq: 960 }, 'startPpq must be finite and in [0, 1e12]'],
    ['lengthPpq', { startPpq: 0, lengthPpq: -5 }, 'lengthPpq must be finite and in (0, 1e12]'],
  ] as const)('names %s as the clip request spells it', (_field, clip, text) => {
    const project = new Project();
    const trackId = project.addTrack({ name: 'track' });
    const error = caught(() => project.addClip({ trackId, ...clip }));
    expect(error).toBeInstanceOf(RangeError);
    expect(error.message).toBe(`Project.addClip: ${text}`);
  });
});
