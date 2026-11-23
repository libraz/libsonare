/**
 * A refusal raised below the facade names the field the caller wrote, never the core's own
 * parameter name.
 */

import { describe, expect, it } from 'vitest';
import { timeStretch } from '../dist/index.js';

const caught = (run: () => unknown): Error => {
  try {
    run();
  } catch (error) {
    return error as Error;
  }
  throw new Error('expected the call to throw');
};

const SR = 22050;
const samples = new Float32Array(SR).fill(0.1);

describe('timeStretch', () => {
  it('names speedRatio when the ratio is below the computed floor', () => {
    const error = caught(() => timeStretch({ samples, sampleRate: SR, speedRatio: 1e-9 }));
    expect(error).toBeInstanceOf(RangeError);
    expect(error.message).toMatch(/^timeStretch: speedRatio must be finite and in \[/);
  });
});
