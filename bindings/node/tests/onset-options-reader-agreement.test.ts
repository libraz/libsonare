/**
 * `detectOnsets` answers a wrong-typed `threshold` or `delta` the same way on
 * both surfaces.
 *
 * Both fields are peak-picking QUANTITIES: nothing documents a value of either
 * as meaning "unspecified", so omitting the key is the only way to ask for the
 * default and a wrong-typed value is a caller error. Both surfaces therefore
 * read them through their presence-checked family and refuse. The addon already
 * did; the embind binding read them through `floatProperty`, which goes through
 * `val::as<double>()` and COERCES, so a numeric string reached the peak picker
 * as the number it spells and produced a different onset set from the one the
 * addon refused to compute at all.
 *
 * THE INPUT HAS TO CHANGE THE ANSWER, AND SO DOES ITS MAGNITUDE. A boolean
 * cannot see this -- it coerces to 1, and 1 leaves the onset set identical to
 * the default on any ordinary fixture, so a test written with one reports
 * agreement where there is none. Neither can a value that sits below every peak:
 * `'0.5'` parses and coerces, and the coerced answer is the default's, so a
 * coercing reader and a refusing one are indistinguishable through it. The
 * discriminating inputs are a numeric STRING and a single-element ARRAY carrying
 * a magnitude that moves the result -- what a value read out of a form, a query
 * parameter or a JSON document arrives as.
 *
 * NO ASSERTION HERE IS SATISFIED BY A THROW ALONE. A refusal is checked against
 * BOTH answers it replaced: the coerced one (what the surface used to compute)
 * and the default one (what a substituting reader would have computed). A throw
 * raised for an unrelated reason clears neither.
 */

import { beforeAll, describe, expect, it } from 'vitest';
import { detectOnsets as wasmDetectOnsets, init as wasmInit } from '../../wasm/dist/index.js';
import { detectOnsets } from '../src/index.js';

const SAMPLE_RATE = 22050;

/** One second carrying ten clicks, the fixture every count below is read off. */
const clicks = (() => {
  const out = new Float32Array(SAMPLE_RATE);
  for (let click = 0; click < 10; click++) {
    const at = Math.round((click * SAMPLE_RATE) / 10);
    for (let i = 0; i < 64; i++) {
      out[at + i] = (1 - i / 64) * (i % 2 === 0 ? 1 : -1);
    }
  }
  return out;
})();

/** Onset times for an options bag, as a comparable string. */
const onsets = {
  node: (options: Record<string, unknown> = {}): string =>
    Array.from(detectOnsets(clicks, SAMPLE_RATE, options)).join(','),
  wasm: (options: Record<string, unknown> = {}): string =>
    Array.from(wasmDetectOnsets(clicks, SAMPLE_RATE, options)).join(','),
};

const SURFACES = ['node', 'wasm'] as const;
type Surface = (typeof SURFACES)[number];

/** The result, or the error, whichever the call produced. */
const capture = (run: () => string): string | Error => {
  try {
    return run();
  } catch (error) {
    return error as Error;
  }
};

/**
 * Two legitimate values per field, chosen against the fixture rather than
 * against the names.
 *
 * Both fields are in the units of the onset-strength envelope, which is NOT
 * normalized, so the usable range is a property of the signal. On this click
 * train the peaks sit well above 1: every value on the [0, 1] scale these names
 * suggest leaves the onset set identical to the default, which is how a control
 * comes to discriminate nothing while looking like it does. `high` is also the
 * magnitude the wrong-typed values below carry, so a reader that coerced one
 * would land on a result this file can name.
 */
const FIELDS = [
  { key: 'delta', low: 8, high: 30 },
  { key: 'threshold', low: 8, high: 20 },
] as const;

/** The values that separate a refusing reader from a coercing one. */
const wrongTyped = (magnitude: number): readonly unknown[] => [`${magnitude}`, [magnitude]];

beforeAll(async () => {
  await wasmInit();
});

describe.each(FIELDS)('detectOnsets reads $key the same way on both surfaces', (field) => {
  const { key, low, high } = field;
  const withValue = (surface: Surface, value: unknown) => onsets[surface]({ [key]: value });

  it.each(SURFACES)('answers two legitimate values with two different onset sets on %s', (s) => {
    // The positive control. Without it every refusal below would pass just as
    // well against an entry point that stopped reading the field entirely.
    const byDefault = onsets[s]();
    const atLow = withValue(s, low);
    const atHigh = withValue(s, high);
    // Three comparisons, not one. Two values differing from each other is not
    // enough: both could sit below every peak, in which case they agree with
    // each other AND with the default, and the control proves nothing.
    expect(atLow).not.toBe(atHigh);
    expect(atLow).not.toBe(byDefault);
    expect(atHigh).not.toBe(byDefault);
  });

  it('agrees across the surfaces on every legitimate value', () => {
    // What makes a disagreement on a wrong-typed value attributable to the
    // reader rather than to the two builds computing different audio.
    expect(onsets.node()).toBe(onsets.wasm());
    expect(withValue('node', low)).toBe(withValue('wasm', low));
    expect(withValue('node', high)).toBe(withValue('wasm', high));
  });

  it.each(SURFACES)('refuses a numeric string and a one-element array on %s', (s) => {
    for (const value of wrongTyped(high)) {
      const caught = capture(() => withValue(s, value));
      expect(caught, `${key} ${JSON.stringify(value)} on ${s}`).toBeInstanceOf(Error);
      expect((caught as Error).message).toBe(`${key} must be a number`);
    }
  });

  it.each(SURFACES)('produces neither the coerced nor the default answer on %s', (s) => {
    // The half that says the refusals above are the RIGHT refusals. A throw
    // raised for any other reason would satisfy the message check; what it
    // cannot do is fail to equal both of the two results this field used to
    // produce for these inputs -- the coerced one on WASM, the default one on
    // any surface whose reader substitutes.
    const coerced = withValue(s, high);
    const byDefault = onsets[s]();
    expect(coerced).not.toBe(byDefault);
    for (const value of wrongTyped(high)) {
      const outcome = capture(() => withValue(s, value));
      expect(outcome, `${key} ${JSON.stringify(value)} on ${s}`).not.toBe(coerced);
      expect(outcome).not.toBe(byDefault);
    }
  });

  it('refuses a boolean too, and records why one cannot be the discriminating input', () => {
    // `true` coerces to 1, and 1 gives the default's onset set on this fixture,
    // so a boolean cannot tell a reader that APPLIES the value from one that
    // ignores it. That is a property of the value, not of either reader, and it
    // is driven here so the next reader of this file does not reach for it.
    expect(onsets.node({ [key]: 1 })).toBe(onsets.node());
    for (const s of SURFACES) {
      const caught = capture(() => withValue(s, true));
      expect(caught, `${key} true on ${s}`).toBeInstanceOf(Error);
      expect((caught as Error).message).toBe(`${key} must be a number`);
    }
  });

  it.each(SURFACES)('still takes the default for an omitted, null or undefined %s field', (s) => {
    // The refusal is about a PRESENT wrong-typed value. Absence is still the way
    // to ask for the default, on both surfaces, and a reader that started
    // throwing for an omitted field would be a different defect.
    const byDefault = onsets[s]();
    expect(withValue(s, undefined)).toBe(byDefault);
    expect(withValue(s, null)).toBe(byDefault);
  });
});
