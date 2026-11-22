/**
 * Each refusal carries its entry's name once. The entry prefix is recomposed from the bare text by
 * the outermost entry, so a second prefix can only come from a bare text that already names a
 * function. The scan drives every exported function with a few argument shapes and fails on any
 * message whose leading name repeats or is followed by a second name.
 * A floor, not the guard: facade preflights hide most native texts, which a static check covers.
 * The refusal count floor keeps a scan that stopped refusing from passing as clean.
 */

import { describe, expect, it } from 'vitest';
import * as api from '../dist/index.js';

const DOUBLED = /^(\w[\w.]*): \1: /;
const TWO_NAMES = /^\w[\w.]*: [a-z_][\w.]*: /;

function signal(): Float32Array {
  return new Float32Array(2048).map((_, i) => Math.sin(i / 10));
}

const SHAPES: ReadonlyArray<() => unknown[]> = [
  () => [],
  () => [{}],
  () => [[]],
  () => [signal()],
  () => [signal(), 22050],
  () => [new Float32Array(0), 22050],
  () => [{ samples: signal(), sampleRate: 22050 }],
  () => [{ samples: new Float32Array(0), sampleRate: 22050 }],
  () => [{ beats: [] }],
];

describe('refusal entry prefix', () => {
  it('never names an entry twice on an exported function', async () => {
    const offenders: string[] = [];
    let refusals = 0;
    for (const [name, value] of Object.entries(api)) {
      if (typeof value !== 'function' || /^[A-Z]/.test(name) || name === 'init') {
        continue;
      }
      for (const shape of SHAPES) {
        let error: unknown;
        try {
          const result = (value as (...args: unknown[]) => unknown)(...shape());
          if (result instanceof Promise) {
            await result.catch((rejected: unknown) => {
              error = rejected;
            });
          }
        } catch (thrown) {
          error = thrown;
        }
        if (error instanceof Error) {
          refusals += 1;
          if (DOUBLED.test(error.message) || TWO_NAMES.test(error.message)) {
            offenders.push(`${name}: ${error.message}`);
          }
        }
      }
    }
    expect(refusals).toBeGreaterThan(2000);
    expect([...new Set(offenders)]).toEqual([]);
  });
});
