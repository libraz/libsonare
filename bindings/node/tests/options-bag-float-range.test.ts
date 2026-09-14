/**
 * `node_float_option` refuses a finite number no 32-bit float can hold, and the
 * WASM surface answers the same input the same way.
 *
 * The input that matters is FINITE. `Infinity` was already handled and cannot
 * separate the two surfaces; `1e300` is the value a caller can write, that
 * survives every JS-side check, and that the narrowing to float turns into
 * `+inf` — a number nobody asked for, presented as a successful call. `3.5e38`
 * is the first value past `FLT_MAX` (~3.4028235e38); `3.0e38` is the boundary
 * control that must still be accepted and consumed.
 *
 * The type question is the other half of the finding, and it is an asymmetry
 * question: a test against one surface establishes nothing about it. The last
 * describe drives both the addon and the WASM binding with the same four
 * wrong-typed values and compares each against its own default-parameter run,
 * which is the comparison that holds whichever answer the surfaces settle on.
 *
 * Driven against the addon rather than the TypeScript facade for the reader
 * cases: `meteringSpectrum`'s facade validates only `samples` and forwards the
 * bag verbatim, so both reach the same reader, but the addon is the boundary a
 * generated binding or a direct consumer sees. One assertion pins that the
 * facade adds nothing on top.
 *
 * Field: `dbRef` on `meteringSpectrum`. It is one of the two `node_float_option`
 * fields on that bag, it has no facade guard, and the returned `db` curve moves
 * with it.
 */

import { beforeAll, describe, expect, it } from 'vitest';
import {
  type RoomEstimateResult,
  masteringRepairDereverbConfigForRoom as wasmDereverbConfigForRoom,
  init as wasmInit,
} from '../../wasm/dist/index.js';
import { ErrorCode, isSonareError, meteringSpectrum } from '../src/index.js';
import { addon } from '../src/native.js';

/** The suffix the refusal message ends with, whatever the key is. */
const RANGE_MESSAGE = 'must be a finite number within the 32-bit float range';

/** Finite values wider than a 32-bit float. Every one of these must be refused. */
const PAST_FLOAT_MAX = [1e300, -1e300, 3.5e38] as const;

/** Just inside FLT_MAX: the control that keeps the refusal from being a blanket one. */
const INSIDE_FLOAT_MAX = 3.0e38;

/** Non-number values, driven against both surfaces below. */
const WRONG_TYPES: readonly unknown[] = ['0.8', true, [0.8], {}];

const SAMPLE_RATE = 22050;

const tone = new Float32Array(SAMPLE_RATE / 2).map(
  (_, i) => 0.3 * Math.sin((2 * Math.PI * 220 * i) / SAMPLE_RATE),
);

/* biome-ignore lint/suspicious/noExplicitAny: the addon is untyped here on purpose. */
const native = addon as any;

const sum = (values: ArrayLike<number>): number => {
  let total = 0;
  for (let i = 0; i < values.length; i++) {
    total += values[i];
  }
  return total;
};

/** The dB curve `meteringSpectrum` returns, reduced to one comparable number. */
const spectrumDb = (dbRef?: unknown): number =>
  sum(native.meteringSpectrum(tone, SAMPLE_RATE, dbRef === undefined ? {} : { dbRef }).db);

const capture = (run: () => unknown): unknown => {
  try {
    run();
    return undefined;
  } catch (error) {
    return error;
  }
};

describe('the addon float option reader refuses a value no float can hold', () => {
  it('reads two legitimate dbRef values as two different dB curves', () => {
    // Without this a refusal assertion would pass just as well against a field
    // the entry point never reads.
    // dbRef is the reference the dB curve is taken against, so raising it moves
    // the whole curve down by a fixed amount. The direction is asserted rather
    // than the two totals: an absolute pin on a summed 1024-bin curve would
    // break on any float-build difference without saying anything more.
    const low = spectrumDb(0.5);
    const high = spectrumDb(2.0);
    expect(low).not.toBe(high);
    expect(high).toBeLessThan(low);
  });

  it('refuses 1e300, -1e300 and 3.5e38 rather than passing +inf downstream', () => {
    for (const value of PAST_FLOAT_MAX) {
      const caught = capture(() => spectrumDb(value));
      expect(caught, `dbRef ${value}`).toBeInstanceOf(RangeError);
      expect((caught as RangeError).message).toBe(`dbRef ${RANGE_MESSAGE}`);
    }
  });

  it('still accepts 3.0e38, which a float can hold', () => {
    expect(() => spectrumDb(INSIDE_FLOAT_MAX)).not.toThrow();
    // Accepted AND consumed: the curve is not the one the omitted field gives.
    expect(spectrumDb(INSIDE_FLOAT_MAX)).not.toBe(spectrumDb());
  });

  it('forwards a non-finite number to the C ABI, which refuses it', () => {
    // Not the default substitution: `node_float_option` falls back only for a
    // value that is not a JS number, so a NaN the caller wrote reaches the core
    // and is answered there. Asserted as the outcome rather than as a throw
    // shape, because the two are different reports of the same input.
    for (const value of [Number.NaN, Number.POSITIVE_INFINITY, Number.NEGATIVE_INFINITY]) {
      const caught = capture(() => spectrumDb(value));
      expect(isSonareError(caught), `dbRef ${value}`).toBe(true);
      expect((caught as { code: number }).code).toBe(ErrorCode.InvalidParameter);
    }
  });

  it('answers the same through the public facade as through the addon', () => {
    expect(sum(meteringSpectrum(tone, SAMPLE_RATE, { dbRef: 0.5 }).db)).toBe(spectrumDb(0.5));
    expect(() => meteringSpectrum(tone, SAMPLE_RATE, { dbRef: 1e300 })).toThrow(RangeError);
  });
});

describe('the addon and the WASM binding read one options bag one way', () => {
  /** A room estimate carrying only the two fields the WASM entry point reads. */
  const estimate = (volume?: unknown): RoomEstimateResult => {
    const out: Record<string, unknown> = { rt60Bands: new Float32Array(0) };
    if (volume !== undefined) {
      out.volume = volume;
    }
    return out as unknown as RoomEstimateResult;
  };

  /** The WASM `floatOption` field: `volume` sets `lateDelayMs` to sqrt(V) ms. */
  const wasmLateDelayMs = (volume?: unknown): number =>
    wasmDereverbConfigForRoom(estimate(volume)).lateDelayMs;

  beforeAll(async () => {
    await wasmInit();
  });

  it('reads two legitimate values on each surface as two different results', () => {
    // One control per field, on both surfaces: a field either side ignores
    // would satisfy every agreement assertion below without reading anything.
    expect(spectrumDb(0.5)).not.toBe(spectrumDb(2.0));
    expect(wasmLateDelayMs(100)).toBeCloseTo(10, 5);
    expect(wasmLateDelayMs(900)).toBeCloseTo(30, 5);
  });

  it('refuses a finite out-of-float-range value on both, with the same message', () => {
    for (const value of PAST_FLOAT_MAX) {
      const fromNode = capture(() => spectrumDb(value));
      const fromWasm = capture(() => wasmLateDelayMs(value));
      expect(fromNode, `node ${value}`).toBeInstanceOf(Error);
      expect(fromWasm, `wasm ${value}`).toBeInstanceOf(Error);
      // The error CLASS differs by surface convention (RangeError vs
      // SonareError); the sentence the caller reads does not.
      expect((fromNode as Error).message).toBe(`dbRef ${RANGE_MESSAGE}`);
      expect((fromWasm as Error).message).toBe(`volume ${RANGE_MESSAGE}`);
    }
  });

  it('accepts the boundary value on both', () => {
    expect(() => spectrumDb(INSIDE_FLOAT_MAX)).not.toThrow();
    expect(() => wasmLateDelayMs(INSIDE_FLOAT_MAX)).not.toThrow();
  });

  it("answers a non-number by each field's own contract", () => {
    // The two fields are not in the same class and must not be asserted as if
    // they were. `dbRef` is read by the substituting family, whose contract IS
    // the fallback, so its assertion is the RESULT against the
    // default-parameter run. `volume` is read by a reader that reserves its
    // substitution for a non-finite NUMBER, so a wrong type is refused by name.
    const nodeDefault = spectrumDb();
    expect(nodeDefault).not.toBe(spectrumDb(2.0));
    for (const value of WRONG_TYPES) {
      const label = JSON.stringify(value);
      expect(spectrumDb(value), `node dbRef ${label}`).toBe(nodeDefault);
      const caught = capture(() => wasmLateDelayMs(value));
      expect(caught, `wasm volume ${label}`).toBeInstanceOf(Error);
      expect((caught as Error).message).toBe('volume must be a number');
    }
  });
});
