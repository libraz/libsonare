import { beforeAll, describe, expect, it } from 'vitest';
import { decomposeStems, fixLength, init, meteringPeakDb } from '../dist/index.js';

function refusal(call: () => unknown): RangeError {
  let caught: unknown;
  try {
    call();
  } catch (error) {
    caught = error;
  }
  expect(caught).toBeInstanceOf(RangeError);
  return caught as RangeError;
}

const largestOf = (message: string): number =>
  Number(message.match(/; the largest accepted is (\d+)$/)?.[1]);

// A call whose input copy and output alone exceed what the heap can still hand out is refused
// before it allocates, naming the argument that scales it and the largest value that fits.
describe('a one-shot call whose certain need exceeds the heap', () => {
  beforeAll(async () => {
    await init();
  });

  it('is refused by the argument that scales it, with the largest value accepted', () => {
    const values = new Float32Array(4);
    const error = refusal(() => fixLength({ values, targetSize: 700_000_000 }));
    expect(error.message).toMatch(
      /^fixLength: targetSize is 700000000 and needs 2670\.3 MiB, more than the \d+\.\d MiB available; the largest accepted is \d+$/,
    );

    // The stated value is the boundary: one past it is refused with the same answer.
    const largest = largestOf(error.message);
    expect(largest).toBeGreaterThan(0);
    expect(largest).toBeLessThan(700_000_000);
    const next = refusal(() => fixLength({ values, targetSize: largest + 1 }));
    expect(largestOf(next.message)).toBe(largest);
  });

  it('states a measured floor as the least the call needs', () => {
    const samples = new Float32Array(20_000_000);
    const error = refusal(() => decomposeStems({ samples, sampleRate: 48000, nComponents: 8 }));
    expect(error.message).toMatch(
      /^decomposeStems: samples is 20000000 samples \(416\.67 s at 48000 Hz\) and needs at least \d+\.\d MiB, more than the \d+\.\d MiB available; the longest accepted is \d+ samples \(\d+\.\d\d s\)$/,
    );
  });

  it('bounds a typed array by the heap, not by the plain-array element budget', () => {
    const samples = new Float32Array(64 * 1024 * 1024 + 1).fill(0.5);
    expect(meteringPeakDb({ samples, sampleRate: 48000 })).toBeCloseTo(-6.0206, 3);
  });

  it('leaves an out-of-domain value to the argument check', () => {
    const error = refusal(() => fixLength({ values: new Float32Array(4), targetSize: 2 ** 32 }));
    expect(error.message).not.toMatch(/MiB/);
  });
});
