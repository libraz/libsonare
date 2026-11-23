import { beforeAll, describe, expect, it } from 'vitest';
import { init, istft, melToAudio, melToStft, mfccToAudio, mfccToMel } from '../src/index';
import { unknownKeyText } from './_helpers';

describe('inverse request forms name an unknown key before a missing field', () => {
  beforeAll(async () => {
    await init();
  });

  it('istft names a missing plane and refuses a surplus argument', () => {
    const call = istft as (...args: unknown[]) => unknown;
    const plane = { data: new Float32Array(6), rows: 3, cols: 2 };
    const cases: [unknown[], string][] = [
      [[{ phase: plane }], 'magnitude is required'],
      [[{ magnitude: plane, phase: null }], 'phase is required'],
      [[{ magnitude: plane, phase: plane }, 1], 'unexpected argument after the request object'],
    ];
    for (const [args, message] of cases) {
      expect(() => call(...args)).toThrow(TypeError);
      expect(() => call(...args)).toThrow(new TypeError(`istft: ${message}`));
    }
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
