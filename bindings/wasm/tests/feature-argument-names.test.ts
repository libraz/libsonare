/**
 * The feature and synthesis entries call the core directly, so a refusal of a frequency, a
 * duration or the mel range names the request key the caller wrote, as the C ABI does.
 */

import { beforeAll, describe, expect, it } from 'vitest';
import {
  chirp,
  clicks,
  init,
  melSpectrogram,
  melToAudio,
  melToStft,
  mfcc,
  spectralContrast,
  tone,
} from '../dist/index.js';

const REFUSAL = Symbol.for('libsonare.refusal');
const SR = 22050;
const samples = new Float32Array(4096).map((_, n) => Math.sin(n / 10));
const power = { data: new Float32Array(16 * 4).fill(1), rows: 16, cols: 4 };

const refusalOf = (run: () => unknown) => {
  try {
    run();
  } catch (error) {
    return {
      error: error as Error,
      subjects: (error as Record<symbol, { subjects: string[] }>)[REFUSAL]?.subjects,
    };
  }
  throw new Error('expected the call to throw');
};

describe('feature refusals name the request key', () => {
  beforeAll(async () => {
    await init();
  });

  it.each([
    ['melSpectrogram', () => melSpectrogram({ samples, sampleRate: SR, fminHz: -1 }), ['fminHz']],
    [
      'melSpectrogram',
      () => melSpectrogram({ samples, sampleRate: SR, fminHz: 8000, fmaxHz: 4000 }),
      ['fmaxHz', 'fminHz'],
    ],
    ['mfcc', () => mfcc({ samples, sampleRate: SR, fminHz: -1 }), ['fminHz']],
    ['melToStft', () => melToStft({ power, sampleRate: SR, fminHz: -1 }), ['fminHz']],
    [
      'melToStft',
      () => melToStft({ power, sampleRate: SR, fminHz: 8000, fmaxHz: 4000 }),
      ['fmaxHz', 'fminHz'],
    ],
    [
      'melToAudio',
      () => melToAudio({ power, sampleRate: SR, fminHz: 8000, fmaxHz: 4000 }),
      ['fmaxHz', 'fminHz'],
    ],
    [
      'spectralContrast',
      () => spectralContrast({ samples, sampleRate: SR, fminHz: 4000 }),
      ['fminHz', 'nBands', 'sampleRate'],
    ],
    ['chirp', () => chirp({ fminHz: 0 }), ['fminHz']],
    ['chirp', () => chirp({ durationSec: -1 }), ['durationSec']],
    ['tone', () => tone({ durationSec: -1 }), ['durationSec']],
    ['clicks', () => clicks({ times: new Float32Array([0]), frequencyHz: 0 }), ['frequencyHz']],
    [
      'clicks',
      () => clicks({ times: new Float32Array([0]), clickDurationSec: -1 }),
      ['clickDurationSec'],
    ],
  ] as const)('%s case %#', (entry, call, keys) => {
    const { error, subjects } = refusalOf(call);
    expect(error).toBeInstanceOf(RangeError);
    expect(subjects).toEqual(keys);
    expect(error.message.startsWith(`${entry}: ${keys[0]} `)).toBe(true);
  });
});
