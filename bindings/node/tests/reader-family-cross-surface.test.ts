/**
 * One options bag, one answer per surface for a value of the wrong type.
 *
 * The addon and the WASM binding each have two reader families: one that
 * substitutes its default for a wrong-typed value and one that does not. Which
 * family a given public field was assigned to was decided per file, so the same
 * bag could be read under two different rules depending on which surface the
 * caller reached — and both calls SUCCEEDED, so nothing reported it.
 *
 * THE INPUT HAS TO CHANGE THE ANSWER. A boolean cannot see this: `delta: true`
 * coerces to 1, and 1 gives the same onset count as the default on an ordinary
 * click train, so a test written with a boolean reports agreement where there is
 * none. That case is driven below as a negative control precisely so the next
 * reader of this file does not reach for it. The discriminating inputs are a
 * numeric STRING and a single-element ARRAY — what a value read out of a form, a
 * query parameter or a JSON document arrives as.
 *
 * Every assertion is on the RESULT — the onset times, or the dereverb config —
 * never on the absence of an exception, and every surface carries a positive
 * control of two legitimate values producing two different results, so a surface
 * that stopped reading the field entirely cannot pass.
 *
 * What is settled here: neither surface answers a wrong-typed value with the
 * default any more. What is NOT settled, and is pinned below so a change to it
 * is visible: the WASM presence-checked readers COERCE (`val::as<double>` turns
 * `'30'` into 30) where the addon refuses, so the two still part company on a
 * numeric string — as an error on one side rather than as different audio.
 */

import { beforeAll, describe, expect, it } from 'vitest';
import {
  type RoomEstimateResult,
  masteringRepairDereverbConfigForRoom as wasmDereverbConfigForRoom,
  detectOnsets as wasmDetectOnsets,
  init as wasmInit,
} from '../../wasm/dist/index.js';
import { detectOnsets } from '../src/index.js';

const SAMPLE_RATE = 22050;

/** One second carrying ten clicks, the fixture the onset counts below are read off. */
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
const nodeOnsets = (options: Record<string, unknown> = {}): string =>
  Array.from(detectOnsets(clicks, SAMPLE_RATE, options)).join(',');

const wasmOnsets = (options: Record<string, unknown> = {}): string =>
  Array.from(wasmDetectOnsets(clicks, SAMPLE_RATE, options)).join(',');

const capture = (run: () => unknown): unknown => {
  try {
    run();
    return undefined;
  } catch (error) {
    return error;
  }
};

/**
 * Two legitimate values of `delta`, and two of `threshold`.
 *
 * Both fields are expressed in the units of the onset envelope, which is NOT
 * normalized, so the usable range is a property of the fixture rather than of
 * the signature. On this click train the peaks sit between roughly 5 and 40:
 * every value below that leaves the onset set identical to the default, so a
 * control written on the [0, 1] scale these names suggest discriminates
 * nothing while looking like it does.
 */
const LOW_DELTA = 8;
const HIGH_DELTA = 30;
const LOW_THRESHOLD = 8;
const HIGH_THRESHOLD = 20;

/** The values that separate a coercing reader from a substituting one. */
const DISCRIMINATING: readonly unknown[] = [`${HIGH_DELTA}`, [HIGH_DELTA]];

beforeAll(async () => {
  await wasmInit();
});

describe('detectOnsets reads delta on both surfaces', () => {
  it('answers two legitimate deltas with two different onset sets, on each surface', () => {
    // The positive control. Without it every refusal assertion below would pass
    // just as well against a field neither entry point reads.
    expect(nodeOnsets({ delta: LOW_DELTA })).not.toBe(nodeOnsets({ delta: HIGH_DELTA }));
    expect(wasmOnsets({ delta: LOW_DELTA })).not.toBe(wasmOnsets({ delta: HIGH_DELTA }));
    // Each also differs from the default, so neither value can be one that sits
    // below every peak and reads as "no setting given" — the way the threshold
    // control below was vacuous until its values were put on the right scale.
    expect(nodeOnsets({ delta: LOW_DELTA })).not.toBe(nodeOnsets());
    expect(nodeOnsets({ delta: HIGH_DELTA })).not.toBe(nodeOnsets());
    // And the two surfaces agree on each of them, which is what makes a
    // disagreement on a wrong-typed value attributable to the reader.
    expect(nodeOnsets({ delta: LOW_DELTA })).toBe(wasmOnsets({ delta: LOW_DELTA }));
    expect(nodeOnsets({ delta: HIGH_DELTA })).toBe(wasmOnsets({ delta: HIGH_DELTA }));
    expect(nodeOnsets()).toBe(wasmOnsets());
  });

  it('is not separated by a boolean, which is why one must not be used here', () => {
    // On WASM `true` coerces to 1, and 1 gives the default's onset set on this
    // fixture — so a boolean cannot tell a reader that APPLIES the value from
    // one that ignores it. That is what bars it as the discriminating input,
    // and it is a property of the value rather than of either reader. The addon
    // now refuses it by name instead of answering with the default, which is a
    // second reason and not the first one.
    expect(wasmOnsets({ delta: true })).toBe(wasmOnsets());
    const caught = capture(() => nodeOnsets({ delta: true }));
    expect(caught).toBeInstanceOf(TypeError);
    expect((caught as TypeError).message).toBe('delta must be a number');
  });

  it('refuses a numeric string and a single-element array on the addon', () => {
    for (const value of DISCRIMINATING) {
      const label = JSON.stringify(value);
      const caught = capture(() => nodeOnsets({ delta: value }));
      expect(caught, `delta ${label}`).toBeInstanceOf(TypeError);
      expect((caught as TypeError).message).toBe('delta must be a number');
    }
  });

  it('does not answer a numeric string with the default on the addon', () => {
    // The refusal above could be satisfied by a throw raised for any reason.
    // This is the half that says the OLD answer is gone: the default onset set
    // is no longer what a numeric string produces.
    const byDefault = nodeOnsets();
    for (const value of DISCRIMINATING) {
      expect(capture(() => nodeOnsets({ delta: value }))).not.toBe(byDefault);
    }
  });

  it('applies the coerced number on WASM, which is where the surfaces still part', () => {
    // Recorded, not endorsed. The WASM presence-checked readers have no type
    // test, so a numeric string reaches the core as the number. The addon
    // refuses it. Neither substitutes the default any more, which was the
    // silent half; this is the visible remainder, and a change to the WASM
    // reader shows up here rather than passing unnoticed.
    for (const value of DISCRIMINATING) {
      expect(wasmOnsets({ delta: value }), JSON.stringify(value)).toBe(
        wasmOnsets({ delta: HIGH_DELTA }),
      );
      expect(wasmOnsets({ delta: value })).not.toBe(wasmOnsets());
    }
  });

  it('reads threshold the same way, so the rule is the bag and not one key', () => {
    const byDefault = nodeOnsets();
    const low = nodeOnsets({ threshold: LOW_THRESHOLD });
    const high = nodeOnsets({ threshold: HIGH_THRESHOLD });
    // Three comparisons, not one. Two values differing from each other is not
    // enough: both could be below every peak in the envelope, in which case
    // they agree with the default and with each other and the control passes
    // while proving nothing about the key it names.
    expect(low).not.toBe(high);
    expect(low).not.toBe(byDefault);
    expect(high).not.toBe(byDefault);
    expect(wasmOnsets({ threshold: LOW_THRESHOLD })).toBe(low);
    expect(wasmOnsets({ threshold: HIGH_THRESHOLD })).toBe(high);
    const caught = capture(() => nodeOnsets({ threshold: `${HIGH_THRESHOLD}` }));
    expect(caught).toBeInstanceOf(TypeError);
    expect((caught as TypeError).message).toBe('threshold must be a number');
    expect(wasmOnsets({ threshold: `${HIGH_THRESHOLD}` })).toBe(high);
  });
});

describe('the room measurement volume is read one way on both surfaces', () => {
  /** A room estimate carrying only the two fields the entry point reads. */
  const estimate = (volume?: unknown): RoomEstimateResult => {
    const out: Record<string, unknown> = { rt60Bands: new Float32Array(0) };
    if (volume !== undefined) {
      out.volume = volume;
    }
    return out as unknown as RoomEstimateResult;
  };

  const wasmLateDelayMs = (volume?: unknown): number =>
    wasmDereverbConfigForRoom(estimate(volume)).lateDelayMs;

  it('answers two legitimate volumes with two different mixing times', () => {
    // Polack: lateDelayMs is sqrt(V) ms. The positive control for this field.
    expect(wasmLateDelayMs(100)).toBeCloseTo(10, 5);
    expect(wasmLateDelayMs(900)).toBeCloseTo(30, 5);
  });

  it('refuses a wrong-typed volume rather than reading it as no measurement', () => {
    // This field's substitution is reserved for a non-finite NUMBER, which the
    // core documents as "no measurement". A numeric string is not that, and
    // used to take the same silent path while the addon refused it.
    const byDefault = wasmLateDelayMs();
    for (const value of DISCRIMINATING) {
      const caught = capture(() => wasmLateDelayMs(value));
      expect(caught, JSON.stringify(value)).toBeInstanceOf(Error);
      expect((caught as Error).message).toBe('volume must be a number');
    }
    // Non-vacuity: the omitted call still succeeds and still differs from a
    // measured one, so the refusals above are about the value and not the call.
    expect(byDefault).not.toBeCloseTo(30, 5);
  });

  it('keeps the documented non-finite substitution', () => {
    const byDefault = wasmLateDelayMs();
    expect(wasmLateDelayMs(Number.NaN)).toBe(byDefault);
    expect(wasmLateDelayMs(Number.POSITIVE_INFINITY)).toBe(byDefault);
  });
});
