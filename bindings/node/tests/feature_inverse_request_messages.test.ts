import { describe, expect, it } from 'vitest';
import { melToAudio, melToStft, mfccToAudio, mfccToMel } from '../src/index.js';

describe('inverse request forms name a missing field', () => {
  it.each([
    ['melToStft', melToStft, 'power'],
    ['melToAudio', melToAudio, 'power'],
    ['mfccToMel', mfccToMel, 'coefficients'],
    ['mfccToAudio', mfccToAudio, 'coefficients'],
  ] as const)('%s', (name, fn, field) => {
    const call = fn as unknown as (request: unknown) => unknown;
    const message = `${name}: ${field} is required (or pass result)`;
    expect(() => call({ nMels: 4, nFrames: 2 })).toThrow(TypeError);
    expect(() => call({ nMels: 4, nFrames: 2 })).toThrow(message);
    expect(() => call({})).toThrow(message);
    expect(() => call(undefined)).toThrow(/Float32Array or a request object/);
    expect(() => call(null)).toThrow(TypeError);
  });
});
