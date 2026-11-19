import { beforeAll, describe, expect, it } from 'vitest';
import { init, melToAudio, melToStft, mfccToAudio, mfccToMel } from '../src/index';
import { unknownKeyText } from './_helpers';

describe('inverse request forms name an unknown key before a missing field', () => {
  beforeAll(async () => {
    await init();
  });

  it.each([
    ['melToStft', melToStft, 'power'],
    ['melToAudio', melToAudio, 'power'],
    ['mfccToMel', mfccToMel, 'coefficients'],
    ['mfccToAudio', mfccToAudio, 'coefficients'],
  ] as const)('%s', (name, fn, field) => {
    const call = fn as unknown as (request: unknown) => unknown;
    // Keys are checked before any field is read, so a misspelt field is named as written.
    expect(() => call({ nFrames: 2 })).toThrow(TypeError);
    expect(() => call({ nFrames: 2 })).toThrow(unknownKeyText(`${name}: unknown key nFrames`));
    expect(() => call({})).toThrow(TypeError);
    expect(() => call({})).toThrow(`${name}: ${field} must be a Matrix2D`);
    expect(() => call(undefined)).toThrow(`${name}: request must be an object`);
    expect(() => call(null)).toThrow(TypeError);
  });
});
