/**
 * A refusal raised below the facade names the field the caller wrote, never the core's own
 * parameter name.
 */

import { beforeAll, describe, expect, it } from 'vitest';
import {
  Audio,
  chirp,
  clicks,
  cqt,
  cqtToAudio,
  decodeChannels,
  framesToTime,
  init,
  melToAudio,
  Project,
  timeStretch,
  timeToFrames,
  tone,
  vqtToAudio,
} from '../dist/index.js';

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

describe('a refusal naming the rate says sampleRate, as the request spells it', () => {
  const magnitude = { data: new Float32Array(84 * 4).fill(0.1), rows: 84, cols: 4 };
  it.each([
    [
      'cqt',
      () => cqt({ samples, sampleRate: SR, fminHz: 8000, nBins: 84 }),
      'cqt: fminHz * 2^((nBins - 1) / binsPerOctave) must be less than sampleRate / 2',
    ],
    [
      'cqtToAudio',
      () => cqtToAudio({ magnitude, sampleRate: SR, fminHz: 0.05 }),
      'cqtToAudio: fminHz and binsPerOctave must keep every filter at most 131072 samples long at sampleRate',
    ],
    [
      'vqtToAudio',
      () => vqtToAudio({ magnitude, sampleRate: SR, fminHz: 0.05, gamma: 0 }),
      'vqtToAudio: fminHz, binsPerOctave and gamma must keep every filter at most 131072 samples long at sampleRate',
    ],
    [
      'clicks',
      () => clicks({ times: new Float32Array([0.1]), sampleRate: SR, clickDurationSec: 1e-6 }),
      'clicks: clickDurationSec * sampleRate must be at least 1',
    ],
    [
      'tone',
      () => tone({ frequencyHz: 440, durationSec: 0.1, sampleRate: 1 }),
      'tone: sampleRate must be an integer in [8000, 384000]',
    ],
    [
      'chirp',
      () => chirp({ fminHz: 100, fmaxHz: 200, durationSec: 0.1, sampleRate: 1 }),
      'chirp: sampleRate must be an integer in [8000, 384000]',
    ],
    [
      'melToAudio',
      () =>
        melToAudio({
          power: { data: new Float32Array(128 * 4).fill(0.1), rows: 128, cols: 4 },
          sampleRate: 1,
        }),
      'melToAudio: sampleRate must be an integer in [8000, 384000]',
    ],
    [
      'framesToTime',
      () => framesToTime(1, 0),
      'framesToTime: sampleRate must be an integer in [1, 2147483647]',
    ],
    [
      'timeToFrames',
      () => timeToFrames(1, 0),
      'timeToFrames: sampleRate must be an integer in [1, 2147483647]',
    ],
  ] as const)('%s', (_entry, call, text) => {
    const error = caught(call);
    expect(error).toBeInstanceOf(RangeError);
    expect(error.message).toBe(text);
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
