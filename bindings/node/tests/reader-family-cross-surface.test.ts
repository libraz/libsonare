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
 * What is settled here: for these two fields both surfaces now refuse a
 * wrong-typed value by name, and neither answers one with the default. What is
 * NOT settled is the CLASS — most fields read on both surfaces still disagree,
 * the embind reader coercing where the addon refuses. This file drives the one
 * bag it can measure end to end; `reader-family-scope.test.ts` holds the
 * population and is where a field leaves the divergent list.
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
    // A boolean coerces to 1, and 1 answers with the default's onset set on
    // this fixture — so even a reader that APPLIED it would reply exactly as
    // one that ignored it. That is what bars a boolean as the discriminating
    // input, and it is a property of the VALUE. It is asserted with the number
    // rather than through either reader precisely because both now refuse the
    // boolean: a reader can no longer be asked to demonstrate it, and a claim
    // resting on a reader would have died with the refusal it was written
    // against.
    expect(nodeOnsets({ delta: 1 })).toBe(nodeOnsets());
    expect(wasmOnsets({ delta: 1 })).toBe(wasmOnsets());
    for (const onsets of [nodeOnsets, wasmOnsets]) {
      const caught = capture(() => onsets({ delta: true }));
      expect(caught).toBeInstanceOf(Error);
      expect((caught as Error).message).toContain('delta must be a number');
    }
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

  it('refuses them on WASM too, so this field no longer parts the surfaces', () => {
    // This assertion used to record the remainder the other way round: the
    // embind reader had no type test, so a numeric string reached the core as
    // the number while the addon refused it. It is asserted here rather than
    // left to the scanner because only a driven call can say the refusal
    // reaches a caller rather than merely being written.
    for (const value of DISCRIMINATING) {
      const label = JSON.stringify(value);
      const caught = capture(() => wasmOnsets({ delta: value }));
      expect(caught, `delta ${label}`).toBeInstanceOf(Error);
      expect((caught as Error).message, `delta ${label}`).toContain('delta must be a number');
    }
    // Not a blanket rejection of the key: the same reader still applies a
    // legitimate value, to the same answer the addon gives. Without this a
    // reader that threw for everything would pass the loop above.
    expect(wasmOnsets({ delta: HIGH_DELTA })).toBe(nodeOnsets({ delta: HIGH_DELTA }));
    expect(wasmOnsets({ delta: HIGH_DELTA })).not.toBe(wasmOnsets());
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
    for (const onsets of [nodeOnsets, wasmOnsets]) {
      const caught = capture(() => onsets({ threshold: `${HIGH_THRESHOLD}` }));
      expect(caught).toBeInstanceOf(Error);
      expect((caught as Error).message).toContain('threshold must be a number');
    }
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
