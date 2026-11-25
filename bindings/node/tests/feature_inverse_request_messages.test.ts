import { describe, expect, it } from 'vitest';
import { melToAudio, melToStft, mfccToAudio, mfccToMel } from '../src/index.js';
import { unknownKeyText } from './_helpers.js';

describe('inverse request forms name an unknown key before a missing field', () => {
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

  // RED WHEN: a dimension of the wrong type is refused under a joint `rows and cols` subject
  // instead of the field the caller wrote.
  it.each([
    ['melToStft', melToStft],
    ['melToAudio', melToAudio],
  ] as const)('%s names a non-number dimension by its own path', (name, fn) => {
    const call = fn as unknown as (request: unknown) => unknown;
    const data = new Float32Array(4);
    expect(() => call({ power: { data, rows: 'nope', cols: 2 } })).toThrow(
      new TypeError(`${name}: power.rows must be an integer`),
    );
    expect(() => call({ power: { data, rows: 2, cols: 'nope' } })).toThrow(
      new TypeError(`${name}: power.cols must be an integer`),
    );
  });
});
