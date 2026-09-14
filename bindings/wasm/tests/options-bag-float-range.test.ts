/**
 * A finite number no 32-bit float can hold is refused by both options-bag float
 * readers, and the same value reaches both of them the same way.
 *
 * The input that matters is FINITE. `Infinity` cannot see this at all: both
 * readers already had an answer for it, and the two answers happen to look
 * alike from JS. `1e300` is the separator — it is a number the caller chose, it
 * survives every JS-side check, and it becomes `+inf` on the narrowing to
 * float. A reader that substitutes its default there reports success carrying a
 * value the caller never asked for; a reader that refuses it reports the
 * mistake. `3.5e38` is the first value past `FLT_MAX` (~3.4028235e38) and
 * `3.0e38` is the boundary control that must still be ACCEPTED, so the refusal
 * cannot be a blanket one.
 *
 * Both readers are driven here on purpose. `floatOption` (the fallback family,
 * for fields whose owner documents a non-finite value as "unspecified") and
 * `floatProperty` / `checkedFloatFromVal` (the presence-checked family) split on
 * NaN by design, and used to split on `1e300` by accident. Keeping them in one
 * file is what makes a change to either one visible.
 *
 * Where the contract keeps the substitution — a NaN or an infinity reaching
 * `floatOption` — the signature is a SUCCESSFUL call carrying a value the caller
 * did not choose, so those cases compare the whole result against the
 * default-parameter run. "It did not throw" would pass for a field the entry
 * point never reads.
 *
 * Fields:
 *  - `floatOption`: `estimate.volume` on `masteringRepairDereverbConfigForRoom`,
 *    the only one of the four WASM `floatOption` call sites whose facade passes
 *    the bag through untouched and whose result is a plain config object. The
 *    volume determines `lateDelayMs` as Polack's sqrt(V) ms, clamped to
 *    [1, 1000].
 *  - `floatProperty`: `gain` on `pcen`, whose facade forwards its options object
 *    verbatim.
 */

import { beforeAll, describe, expect, it } from 'vitest';
import {
  ErrorCode,
  init,
  isSonareError,
  masteringRepairDereverbConfigForRoom,
  pcen,
  type RoomEstimateResult,
  type SonareError,
} from '../dist/index.js';

/** The suffix both readers' refusal message ends with, whatever the key is. */
const RANGE_MESSAGE = 'must be a finite number within the 32-bit float range';

/** Finite values wider than a 32-bit float. Every one of these must be refused. */
const PAST_FLOAT_MAX = [1e300, -1e300, 3.5e38] as const;

/** Just inside FLT_MAX: the control that keeps the refusal from being a blanket one. */
const INSIDE_FLOAT_MAX = 3.0e38;

/** Non-number values, for the type rule the two `_option` families share. */
const WRONG_TYPES: readonly unknown[] = ['0.8', true, [0.8], {}];

const PCEN_BINS = 8;
const PCEN_FRAMES = 16;
const matrix = new Float32Array(PCEN_BINS * PCEN_FRAMES).map((_, i) => 0.1 + (i % 5) * 0.05);

const capture = (run: () => unknown): unknown => {
  try {
    run();
    return undefined;
  } catch (error) {
    return error;
  }
};

/** Asserts the caught value is the reader's out-of-float-range refusal for `key`. */
function expectRangeRefusal(caught: unknown, key: string): SonareError {
  expect(isSonareError(caught)).toBe(true);
  const error = caught as SonareError;
  expect(error.code).toBe(ErrorCode.InvalidParameter);
  expect(error.message).toBe(`${key} ${RANGE_MESSAGE}`);
  return error;
}

/** A room estimate carrying only the two fields the entry point reads. */
function estimate(volume?: unknown): RoomEstimateResult {
  const out: Record<string, unknown> = { rt60Bands: new Float32Array(0) };
  if (volume !== undefined) {
    out.volume = volume;
  }
  return out as unknown as RoomEstimateResult;
}

const configForRoom = (volume?: unknown) => masteringRepairDereverbConfigForRoom(estimate(volume));

const pcenWith = (gain?: unknown) =>
  pcen(
    matrix,
    PCEN_BINS,
    PCEN_FRAMES,
    (gain === undefined ? {} : { gain }) as Record<string, number>,
  );

beforeAll(async () => {
  await init();
});

describe('floatOption refuses a finite value wider than a float', () => {
  it('reads two legitimate volumes as two different mixing times', () => {
    // Polack: lateDelayMs is sqrt(V) ms. Without this the refusals below would
    // pass just as well against a field the entry point ignores.
    expect(configForRoom(100).lateDelayMs).toBeCloseTo(10, 5);
    expect(configForRoom(900).lateDelayMs).toBeCloseTo(30, 5);
    expect(configForRoom(100).lateDelayMs).not.toBe(configForRoom(900).lateDelayMs);
  });

  it('refuses 1e300, -1e300 and 3.5e38 instead of substituting the default', () => {
    for (const value of PAST_FLOAT_MAX) {
      expectRangeRefusal(
        capture(() => configForRoom(value)),
        'volume',
      );
    }
  });

  it('still accepts 3.0e38, which a float can hold', () => {
    const accepted = configForRoom(INSIDE_FLOAT_MAX);
    // sqrt(3e38) is far past the 1000 ms ceiling, so the value lands on the
    // clamp rather than on the default — accepted AND consumed.
    expect(accepted.lateDelayMs).toBeCloseTo(1000, 5);
    expect(accepted.lateDelayMs).not.toBe(configForRoom().lateDelayMs);
  });

  it('substitutes the default for NaN and for an infinity the caller wrote', () => {
    // These are the cases whose contract KEEPS the substitution, so the result
    // is the assertion: the call succeeds carrying the default-parameter answer.
    const omitted = configForRoom();
    expect(configForRoom(Number.NaN)).toEqual(omitted);
    expect(configForRoom(Number.POSITIVE_INFINITY)).toEqual(omitted);
    expect(configForRoom(Number.NEGATIVE_INFINITY)).toEqual(omitted);
  });

  it('substitutes the default for a value that is not a number', () => {
    const omitted = configForRoom();
    for (const value of WRONG_TYPES) {
      // `{}` reached the default through NaN coercion before the type check and
      // reaches it by type afterwards. The result is the same either way, which
      // is why the result is what is asserted.
      expect(configForRoom(value), `volume ${JSON.stringify(value)}`).toEqual(omitted);
    }
  });
});

describe('floatProperty refuses a finite value wider than a float', () => {
  it('reads two legitimate gains as two different outputs', () => {
    const low = pcenWith(0.2);
    const high = pcenWith(0.9);
    expect(Array.from(low)).not.toEqual(Array.from(high));
  });

  it('refuses 1e300, -1e300 and 3.5e38', () => {
    for (const value of PAST_FLOAT_MAX) {
      expectRangeRefusal(
        capture(() => pcenWith(value)),
        'gain',
      );
    }
  });

  it('still accepts 3.0e38, which a float can hold', () => {
    expect(() => pcenWith(INSIDE_FLOAT_MAX)).not.toThrow();
    expect(Array.from(pcenWith(INSIDE_FLOAT_MAX))).not.toEqual(Array.from(pcenWith()));
  });

  it('refuses NaN and an infinity rather than substituting, which is the family split', () => {
    // The documented difference from floatOption: this family treats a present
    // non-finite value as a caller error, because most of its fields land in a
    // config guard written as `x > lo`, where a NaN takes the permissive arm.
    for (const value of [Number.NaN, Number.POSITIVE_INFINITY, Number.NEGATIVE_INFINITY]) {
      expectRangeRefusal(
        capture(() => pcenWith(value)),
        'gain',
      );
    }
  });

  it('coerces a non-number rather than type-checking it, unlike floatOption', () => {
    // Recorded, not endorsed: floatProperty has no type check, so a string, an
    // array and a boolean are coerced by val::as<double>() while `{}` becomes
    // NaN and is refused. floatOption answers all four with its default. This
    // assertion exists so unifying the two readers shows up here rather than
    // passing unnoticed.
    const omitted = Array.from(pcenWith());
    expect(Array.from(pcenWith('0.8'))).toEqual(Array.from(pcenWith(0.8)));
    expect(Array.from(pcenWith([0.8]))).toEqual(Array.from(pcenWith(0.8)));
    expect(Array.from(pcenWith(true))).toEqual(Array.from(pcenWith(1)));
    expect(Array.from(pcenWith('0.8'))).not.toEqual(omitted);
    expectRangeRefusal(
      capture(() => pcenWith({})),
      'gain',
    );
  });
});

describe('both readers answer 1e300 the same way', () => {
  it('refuses it by the same code and the same message, naming each field', () => {
    const fromOption = expectRangeRefusal(
      capture(() => configForRoom(1e300)),
      'volume',
    );
    const fromProperty = expectRangeRefusal(
      capture(() => pcenWith(1e300)),
      'gain',
    );
    expect(fromOption.code).toBe(fromProperty.code);
    expect(fromOption.message.replace('volume', '<key>')).toBe(
      fromProperty.message.replace('gain', '<key>'),
    );
  });
});
