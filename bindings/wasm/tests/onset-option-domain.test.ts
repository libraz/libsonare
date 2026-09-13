/**
 * What the onset detector's options bag accepts, once the shared readers are
 * correct.
 *
 * The readers guarantee a value survived its conversion unchanged. That leaves
 * two classes they cannot decide, and both of them used to return a normal onset
 * list rather than an error:
 *
 * A negative frame count. `-1` is finite, integral and inside `int`, so the
 * integer reader passes it through exactly as asked; nothing downstream asks
 * whether a negative window length means anything. `nFft` and `hopLength` in the
 * same bag DO refuse it, but only because `StftConfig` objects on its own terms,
 * so which fields were defended was decided by an unrelated consumer. Whether a
 * length is a length is the field's own question, which is where it is now asked.
 *
 * A non-finite threshold. Those two fields were read by a lambda this file's
 * subject defined for itself — the shared float reader's body written out by
 * hand, minus the check — so correcting the reader did not reach them.
 *
 * Every field carries the positive control the refusal cannot supply: two legal
 * values that produce DIFFERENT onsets, compared by content. Without it a field
 * that is never read reads exactly like a field that accepted the value.
 */

import { beforeAll, describe, expect, it } from 'vitest';
import {
  detectOnsets,
  ErrorCode,
  extractPercussiveEvents,
  init,
  isSonareError,
  type SonareError,
} from '../dist/index.js';

const sampleRate = 22050;

/**
 * Two seconds of decaying clicks whose spacing, pitch and amplitude all vary, so
 * a window length and a threshold each have something to change their mind about.
 * A steady train separates nothing: every peak looks the same to every window.
 */
const clicks = ((): Float32Array => {
  const out = new Float32Array(sampleRate * 2);
  let at = 1000;
  let index = 0;
  while (at < out.length - 2000) {
    const amplitude = 0.2 + 0.8 * ((index % 4) / 3);
    const hz = 500 + 200 * (index % 3);
    for (let i = 0; i < 300; i += 1) {
      out[at + i] += amplitude * Math.exp(-i / 60) * Math.sin((2 * Math.PI * hz * i) / sampleRate);
    }
    at += 1500 + 900 * (index % 5);
    index += 1;
  }
  return out;
})();

beforeAll(async () => {
  await init();
});

const onsets = (options: Record<string, number | boolean>): number[] =>
  Array.from(detectOnsets(clicks, sampleRate, options));

function expectRefusalNaming(field: string, options: Record<string, number>): SonareError {
  let caught: unknown;
  try {
    detectOnsets(clicks, sampleRate, options);
  } catch (error) {
    caught = error;
  }
  expect(caught, `expected ${field} = ${options[field]} to be refused`).toBeDefined();
  expect(isSonareError(caught)).toBe(true);
  const error = caught as SonareError;
  expect(error.code).toBe(ErrorCode.InvalidParameter);
  expect(error.message).toContain(field);
  return error;
}

/** A field, and two legal values of it that pick different onsets. */
interface WindowCase {
  readonly field: string;
  readonly low: number;
  readonly high: number;
  /** Extra options the field needs to reach the picker on a live path. */
  readonly context?: Record<string, boolean>;
}

const WINDOWS: readonly WindowCase[] = [
  { field: 'preMax', low: 1, high: 8 },
  { field: 'postMax', low: 1, high: 12 },
  { field: 'preAvg', low: 3, high: 20 },
  { field: 'postAvg', low: 4, high: 20 },
  { field: 'wait', low: 1, high: 12 },
  // Backtracking is what reads this one; without the flag it is carried and
  // never consulted, and the control below would be measuring nothing.
  { field: 'backtrackRange', low: 0, high: 20, context: { backtrack: true } },
];

describe('an onset window length refuses a negative frame count', () => {
  for (const { field, low, high, context = {} } of WINDOWS) {
    it(`${field}: two legal lengths pick different onsets`, () => {
      // The control, and it is per field on purpose: a bag-wide control cannot
      // tell a field that is read from one that is silently ignored.
      const lower = onsets({ ...context, [field]: low });
      const higher = onsets({ ...context, [field]: high });
      expect(lower.length).toBeGreaterThan(0);
      expect(higher.length).toBeGreaterThan(0);
      expect(higher).not.toEqual(lower);
    });

    it(`${field}: refuses -1 instead of returning a normal onset list`, () => {
      // -1 used to be accepted here and the call returned onsets, so this is the
      // assertion that goes green again if the check is removed.
      const error = expectRefusalNaming(field, { ...context, [field]: -1 } as Record<
        string,
        number
      >);
      expect(error.message).toContain('non-negative');
    });

    it(`${field}: still accepts 0, which is a length`, () => {
      // The boundary the guard must not eat. Zero is a degenerate window rather
      // than an impossible one, and several of these fields read it as their
      // own default.
      expect(() => onsets({ ...context, [field]: 0 })).not.toThrow();
    });
  }
});

describe('an onset strength refuses a non-finite number', () => {
  const FLOATS: ReadonlyArray<{ field: string; low: number; high: number }> = [
    { field: 'threshold', low: 0, high: 20 },
    { field: 'delta', low: 0, high: 20 },
  ];

  for (const { field, low, high } of FLOATS) {
    it(`${field}: two legal strengths pick different onsets`, () => {
      const quiet = onsets({ [field]: low });
      const strict = onsets({ [field]: high });
      expect(quiet.length).toBeGreaterThan(0);
      expect(strict.length).toBeGreaterThan(0);
      expect(strict).not.toEqual(quiet);
    });

    it(`${field}: refuses NaN and both infinities`, () => {
      // NaN is the discriminating input, not a large number: every guard below
      // this field is a comparison, and a comparison against NaN answers false,
      // so NaN took the permissive arm of each and the call succeeded.
      for (const value of [Number.NaN, Number.POSITIVE_INFINITY, Number.NEGATIVE_INFINITY]) {
        expectRefusalNaming(field, { [field]: value });
      }
    });
  }
});

describe('the percussive extractor refuses a wait that is not a whole frame count', () => {
  /** Three hits 400 ms apart: far enough that a wait long enough to merge them is visible. */
  const hits = ((): Float32Array => {
    const out = new Float32Array(Math.round(sampleRate * 1.6));
    for (const at of [0.2, 0.6, 1.0].map((s) => Math.round(s * sampleRate))) {
      for (let i = 0; i < 2000; i += 1) {
        out[at + i] += 0.8 * Math.exp(-i / 250) * Math.sin((2 * Math.PI * 1800 * i) / sampleRate);
      }
    }
    return out;
  })();

  const extract = (onsetWait?: number): number[] =>
    extractPercussiveEvents({ samples: hits, sampleRate, onsetWait }).map((e) => e.onsetSample);

  it('consumes a legal wait: a longer one merges hits the default keeps apart', () => {
    // The control, and it has to move the SET rather than merely succeed. A wait
    // that was silently ignored would return the default set from every call.
    const base = extract();
    expect(base).toHaveLength(3);
    expect(extract(1)).toEqual(base);
    expect(extract(20)).toHaveLength(2);
    expect(extract(60)).toHaveLength(1);
  });

  it('refuses a fractional wait rather than running the default framing', () => {
    // The discriminating case is -0.5, not a large number. It truncates onto 0,
    // and 0 is how this field spells "keep the default", so before the check it
    // did not fail — it returned exactly the default-parameter event set while
    // the caller believed a wait had been set.
    const base = extract();
    for (const value of [0.5, -0.5]) {
      let caught: unknown;
      let result: number[] | undefined;
      try {
        result = extract(value);
      } catch (error) {
        caught = error;
      }
      expect(result, `onsetWait = ${value} must not return the default-wait set`).toBeUndefined();
      expect(isSonareError(caught)).toBe(true);
      expect((caught as SonareError).code).toBe(ErrorCode.InvalidParameter);
      expect((caught as SonareError).message).toContain('onsetWait must be an integer');
    }
    // The set the refusals must not have silently produced.
    expect(base).toHaveLength(3);
  });

  it('refuses a non-finite or negative wait', () => {
    for (const value of [Number.NaN, Number.POSITIVE_INFINITY, -1, 2 ** 31]) {
      let caught: unknown;
      try {
        extract(value);
      } catch (error) {
        caught = error;
      }
      expect(isSonareError(caught), `onsetWait = ${value} must be refused`).toBe(true);
      expect((caught as SonareError).code).toBe(ErrorCode.InvalidParameter);
    }
  });
});
