/**
 * What a POSITIONAL `float` parameter is allowed to become on its way into an
 * embind-registered function, as distinct from `float-narrowing-guards.test.ts`,
 * which covers the same defect for `val::as<float>()` reads.
 *
 * A plain `float` in a function's C++ signature is narrowed by embind's own
 * glue, and that glue SATURATES: a caller-chosen finite number wider than
 * `FLT_MAX` (~3.4028235e38) arrives as an infinity with no exception raised
 * anywhere. `3.5e38` is the case that matters most, because it is a number a
 * caller could plausibly choose rather than a deliberately extreme one.
 *
 * Each field below now narrows through `checkedFloatFromVal` instead. Acceptance
 * is asserted by reading a value back, never by "it did not throw" — a function
 * that ignored its argument entirely would satisfy every refusal case below in
 * exactly the same way.
 */

import { beforeAll, describe, expect, it } from 'vitest';
import {
  chirp,
  deemphasis,
  ErrorCode,
  init,
  isSonareError,
  peakPick,
  preemphasis,
  type SonareError,
  splitSilence,
  tone,
  trimSilence,
  vectorNormalize,
} from '../dist/index.js';

const SR = 44100;

/** The suffix every one of these refusals ends with, whatever the key is. */
const RANGE_MESSAGE = 'must be a finite number within the 32-bit float range';

/**
 * Values that saturate onto a legal float. These are the ones that matter: each
 * is a number the caller chose, survives every JS-side check, and used to
 * arrive as an infinity a config guard written `x > lo` or `x < hi` waves
 * through.
 */
const SATURATES_ONTO_A_FLOAT = [1e40, -1e40, 3.5e38, 1e300, -1e300];

/** Non-finite values written directly, which must reach the same refusal. */
const NON_FINITE = [Number.NaN, Number.POSITIVE_INFINITY, Number.NEGATIVE_INFINITY];

const REFUSED = [...SATURATES_ONTO_A_FLOAT, ...NON_FINITE];

function capture(run: () => unknown): unknown {
  try {
    run();
    return undefined;
  } catch (error) {
    return error;
  }
}

/** Asserts the caught value is the float-range refusal naming `key`. */
function expectRangeRefusal(caught: unknown, key: string, context: string): void {
  expect(isSonareError(caught), `${context}: expected a SonareError`).toBe(true);
  const error = caught as SonareError;
  expect(error.code, context).toBe(ErrorCode.InvalidParameter);
  expect(error.message, context).toBe(`${key} ${RANGE_MESSAGE}`);
}

/** Counts sign changes, a cheap proxy for how much high-frequency content a signal carries. */
function countZeroCrossings(values: Float32Array): number {
  let crossings = 0;
  for (let i = 1; i < values.length; i++) {
    if ((values[i - 1] as number) < 0 !== (values[i] as number) < 0) crossings++;
  }
  return crossings;
}

// A short single-sample impulse: preemphasis/deemphasis compute
// y[0] = x[0] + zi and y[1] = x[1] -+ coef * (x or y)[0], so with x = [1, 0, 0, 0]
// and zi pinned to 0, y[1] reads back coef directly.
const IMPULSE = new Float32Array([1, 0, 0, 0]);

// A single sharp local max with a flat surround, so peakPick's `delta` decides
// alone whether it clears the local average: avg over [1..4] is 2.5, so
// delta = 0 accepts the peak and delta = 10 (avg + delta = 12.5 > 10) rejects it.
// Two local maxima of different heights, separated by zeros. A single spike in a
// flat run would not do: the equal-valued neighbours at each end satisfy the
// local-max test too, so the result carries edge artefacts that move with the
// window rather than with `delta`, which is the parameter under test.
const PEAK_SIGNAL = new Float32Array([0, 1, 0, 5, 0, 1, 0]);

// Quiet header, loud tail: peak frame RMS is ~0.707 (from the loud half), so
// `top_db` decides whether the quiet header's ~0.00707 RMS clears
// `thr = peak * 10^(-topDb/20)`. topDb = 6 -> thr ~= 0.354 (quiet header stays
// silent); topDb = 80 -> thr ~= 7.07e-5 (quiet header counts as non-silent too).
const QUIET_SAMPLES = 8192;
const LOUD_SAMPLES = 8192;
const silenceSignal = new Float32Array(QUIET_SAMPLES + LOUD_SAMPLES).map((_, i) => {
  const amplitude = i < QUIET_SAMPLES ? 0.01 : 1.0;
  return amplitude * Math.sin((2 * Math.PI * 440 * i) / SR);
});

/** One float field of one entry point, and how to drive it with a value. */
interface FloatField {
  entry: string;
  key: string;
  run: (value: number) => unknown;
}

const FLOAT_FIELDS: FloatField[] = [
  {
    entry: 'vectorNormalize',
    key: 'threshold',
    run: (value) => vectorNormalize(new Float32Array([0.02, -0.01]), 0, value),
  },
  {
    entry: 'peakPick',
    key: 'delta',
    run: (value) => peakPick(PEAK_SIGNAL, 2, 2, 2, 2, value, 1),
  },
  {
    entry: 'trimSilence',
    key: 'topDb',
    run: (value) => trimSilence(silenceSignal, value, 2048, 512),
  },
  {
    entry: 'splitSilence',
    key: 'topDb',
    run: (value) => splitSilence(silenceSignal, value, 2048, 512),
  },
  {
    entry: 'chirp',
    key: 'fmin',
    run: (value) => chirp(value, 880, SR, 0.01, true),
  },
  {
    entry: 'chirp',
    key: 'fmax',
    run: (value) => chirp(440, value, SR, 0.01, true),
  },
  {
    entry: 'preemphasis',
    key: 'coef',
    run: (value) => preemphasis(IMPULSE, value, 0),
  },
  {
    entry: 'deemphasis',
    key: 'coef',
    run: (value) => deemphasis(IMPULSE, value, 0),
  },
  {
    entry: 'tone',
    key: 'frequency',
    run: (value) => tone(value, SR, 0.01, 0, 1),
  },
  {
    entry: 'tone',
    key: 'phase',
    run: (value) => tone(440, SR, 0.01, value, 1),
  },
  {
    entry: 'tone',
    key: 'amplitude',
    run: (value) => tone(440, SR, 0.01, 0, value),
  },
];

beforeAll(async () => {
  await init();
});

describe('every positional float parameter refuses a value the float type cannot hold', () => {
  it('covers one field per positional float read these facades make', () => {
    // A field dropped from the table stops being covered without anything going
    // red, which is the one way this file could quietly shrink.
    expect(FLOAT_FIELDS).toHaveLength(11);
    const ids = FLOAT_FIELDS.map((field) => `${field.entry}.${field.key}`);
    expect(new Set(ids).size).toBe(ids.length);
  });

  it.each(FLOAT_FIELDS)('$entry names $key rather than accepting an infinity', ({ key, run }) => {
    for (const value of REFUSED) {
      expectRangeRefusal(
        capture(() => run(value)),
        key,
        `${key} = ${value}`,
      );
    }
  });
});

describe('vectorNormalize consumes the threshold it accepts', () => {
  it('reads two legitimate thresholds as normalized vs. unchanged', () => {
    const values = new Float32Array([0.02, -0.01]);
    // The Inf-norm of `values` is 0.02: a threshold below it normalizes, a
    // threshold above it is librosa's fill=None no-op.
    const normalized = vectorNormalize(values, 0, 0);
    const untouched = vectorNormalize(values, 0, 0.05);
    expect(normalized[0]).toBeCloseTo(1, 5);
    expect(untouched[0]).toBeCloseTo(0.02, 5);
    expect(normalized[0]).not.toBe(untouched[0]);
  });
});

describe('peakPick consumes the delta it accepts', () => {
  it('reads two legitimate deltas as peak found vs. peak suppressed', () => {
    // `delta` is an absolute margin a candidate must clear above the local mean,
    // so raising it drops the weaker maximum before the stronger one. Three
    // levels rather than two: a binary pair cannot tell "delta is read" from
    // "delta is compared against zero".
    const both = peakPick(PEAK_SIGNAL, 2, 2, 2, 2, 0, 1);
    const strongerOnly = peakPick(PEAK_SIGNAL, 2, 2, 2, 2, 2, 1);
    const suppressed = peakPick(PEAK_SIGNAL, 2, 2, 2, 2, 10, 1);
    expect(Array.from(both)).toEqual([1, 3]);
    expect(Array.from(strongerOnly)).toEqual([3]);
    expect(Array.from(suppressed)).toEqual([]);
  });
});

describe('trimSilence and splitSilence consume the topDb they accept', () => {
  it('trimSilence reads two legitimate topDb values as two different trims', () => {
    const strict = trimSilence(silenceSignal, 6, 2048, 512);
    const lenient = trimSilence(silenceSignal, 80, 2048, 512);
    expect(strict.startSample).toBeGreaterThan(lenient.startSample);
    expect(strict.audio.length).toBeLessThan(lenient.audio.length);
  });

  it('splitSilence reads two legitimate topDb values as two different ranges', () => {
    const totalCovered = (flat: Int32Array): number => {
      let total = 0;
      for (let i = 0; i + 1 < flat.length; i += 2) {
        total += (flat[i + 1] as number) - (flat[i] as number);
      }
      return total;
    };
    const strict = splitSilence(silenceSignal, 6, 2048, 512);
    const lenient = splitSilence(silenceSignal, 80, 2048, 512);
    expect(totalCovered(strict)).toBeLessThan(totalCovered(lenient));
  });
});

describe('chirp consumes the fmin/fmax it accepts', () => {
  it('reads two legitimate sweep ranges as two different crossing counts', () => {
    const low = chirp(100, 200, SR, 0.05, true);
    const high = chirp(4000, 8000, SR, 0.05, true);
    expect(countZeroCrossings(high)).toBeGreaterThan(countZeroCrossings(low));
  });
});

describe('preemphasis and deemphasis consume the coef they accept', () => {
  // With x = [1, 0, 0, 0] and zi pinned to 0, y[1] reads back coef exactly:
  // preemphasis computes y[1] = x[1] - coef * x[0] = -coef,
  // deemphasis computes y[1] = x[1] + coef * y[0] = coef.
  it('preemphasis reads coef back at sample one, negated', () => {
    expect(preemphasis(IMPULSE, 0.5, 0)[1]).toBe(Math.fround(-0.5));
    expect(preemphasis(IMPULSE, 0.25, 0)[1]).toBe(Math.fround(-0.25));
  });

  it('deemphasis reads coef back at sample one, unchanged', () => {
    expect(deemphasis(IMPULSE, 0.5, 0)[1]).toBe(Math.fround(0.5));
    expect(deemphasis(IMPULSE, 0.25, 0)[1]).toBe(Math.fround(0.25));
  });
});

describe('tone consumes the frequency, phase and amplitude it accepts', () => {
  it('reads two legitimate frequencies as two different crossing counts', () => {
    const low = tone(110, SR, 0.05, 0, 1);
    const high = tone(8000, SR, 0.05, 0, 1);
    expect(countZeroCrossings(high)).toBeGreaterThan(countZeroCrossings(low));
  });

  it('reads phase back at sample zero: y[0] = amplitude * sin(phase)', () => {
    // t = 0 on the first sample, so y[0] collapses to amplitude * sin(phase)
    // regardless of frequency -- an exact readback rather than a comparison.
    expect(tone(440, SR, 0.05, 0, 1)[0]).toBeCloseTo(0, 5);
    expect(tone(440, SR, 0.05, Math.PI / 2, 1)[0]).toBeCloseTo(1, 5);
  });

  it('reads amplitude back at sample zero, scaling the same phase', () => {
    expect(tone(440, SR, 0.05, Math.PI / 2, 1)[0]).toBeCloseTo(1, 5);
    expect(tone(440, SR, 0.05, Math.PI / 2, 2)[0]).toBeCloseTo(2, 5);
  });
});

describe('the refusal is the same one the shared readers already raised', () => {
  it('answers a positional zi field and a positional coef field by the same code and wording', () => {
    const fromZi = capture(() => preemphasis(IMPULSE, 0.97, 1e300));
    const fromCoef = capture(() => preemphasis(IMPULSE, 1e300, 0));
    expectRangeRefusal(fromZi, 'zi', 'preemphasis zi');
    expectRangeRefusal(fromCoef, 'coef', 'preemphasis coef');
    expect((fromZi as SonareError).code).toBe((fromCoef as SonareError).code);
    expect((fromZi as SonareError).message.replace('zi', '<key>')).toBe(
      (fromCoef as SonareError).message.replace('coef', '<key>'),
    );
  });
});
