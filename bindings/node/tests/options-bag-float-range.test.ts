/**
 * `FloatProperty` refuses a finite number no 32-bit float can hold, and the
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
 * wrong-typed values, and both refuse them by name.
 *
 * Driven against the addon rather than the TypeScript facade for the reader
 * cases: `pcen`'s facade forwards the options bag verbatim, so both reach the
 * same reader, but the addon is the boundary a generated binding or a direct
 * consumer sees. One assertion pins that the facade adds nothing on top.
 *
 * Field: `gain` on `pcen`, read through the generic `FloatProperty` on this
 * surface and `floatProperty` on WASM, with no facade guard on either.
 */

import { beforeAll, describe, expect, it } from 'vitest';
import { init as wasmInit, pcen as wasmPcen } from '../../wasm/dist/index.js';
import { pcen } from '../src/index.js';
import { addon } from '../src/native.js';

/** The suffix the refusal message ends with, whatever the key is. */
const RANGE_MESSAGE = 'must be finite and in [-3.4028235e38, 3.4028235e38]';

/** Finite values wider than a 32-bit float. Every one of these must be refused. */
const PAST_FLOAT_MAX = [1e300, -1e300, 3.5e38] as const;

/** Just inside FLT_MAX: the control that keeps the refusal from being a blanket one. */
const INSIDE_FLOAT_MAX = 3.0e38;

/** Non-number values, driven against both surfaces below. */
const WRONG_TYPES: readonly unknown[] = ['0.8', true, [0.8], {}];

const BINS = 8;
const FRAMES = 16;

/** Every cell at or above 1, so `(M + eps)^-gain` stays finite for any gain a float can hold. */
const spectrogram = new Float32Array(BINS * FRAMES).map((_, i) => 1 + (i % 5) * 0.05);

/* biome-ignore lint/suspicious/noExplicitAny: the addon is untyped here on purpose. */
const native = addon as any;

/** The `pcen` output reduced to its cells, `gain` omitted when undefined. */
const pcenGain = (gain?: unknown): Float32Array =>
  native.pcen(spectrogram, BINS, FRAMES, gain === undefined ? {} : { gain });

const capture = (run: () => unknown): unknown => {
  try {
    run();
    return undefined;
  } catch (error) {
    return error;
  }
};

describe('the addon float option reader refuses a value no float can hold', () => {
  it('reads two legitimate gains as two different outputs', () => {
    // Without this a refusal assertion would pass just as well against a field
    // the entry point never reads.
    expect(Array.from(pcenGain(0.2))).not.toEqual(Array.from(pcenGain(0.9)));
  });

  it('refuses 1e300, -1e300 and 3.5e38 rather than passing +inf downstream', () => {
    for (const value of PAST_FLOAT_MAX) {
      const caught = capture(() => pcenGain(value));
      expect(caught, `gain ${value}`).toBeInstanceOf(RangeError);
      expect((caught as RangeError).message).toBe(`pcen: gain ${RANGE_MESSAGE}`);
    }
  });

  it('still accepts 3.0e38, which a float can hold', () => {
    expect(() => pcenGain(INSIDE_FLOAT_MAX)).not.toThrow();
    // Accepted AND consumed: the output is not the one the omitted field gives.
    expect(Array.from(pcenGain(INSIDE_FLOAT_MAX))).not.toEqual(Array.from(pcenGain()));
  });

  it('refuses a non-finite number by name rather than reading it as a default', () => {
    // "Not given" is absence, so a NaN or an infinity the caller wrote is a wrong
    // value, refused in the same wording as an overflow.
    for (const value of [Number.NaN, Number.POSITIVE_INFINITY, Number.NEGATIVE_INFINITY]) {
      const caught = capture(() => pcenGain(value));
      expect(caught, `gain ${value}`).toBeInstanceOf(RangeError);
      expect((caught as RangeError).message).toBe(`pcen: gain ${RANGE_MESSAGE}`);
    }
  });

  it('answers the same through the public facade as through the addon', () => {
    const values = { data: spectrogram, rows: BINS, cols: FRAMES };
    expect(Array.from(pcen({ values, gain: 0.5 }).data)).toEqual(Array.from(pcenGain(0.5)));
    expect(() => pcen({ values, gain: 1e300 })).toThrow(RangeError);
  });
});

describe('the addon and the WASM binding read one options bag one way', () => {
  const wasmGain = (gain?: unknown): Float32Array =>
    wasmPcen({
      values: { data: spectrogram, rows: BINS, cols: FRAMES },
      ...(gain === undefined ? {} : { gain: gain as number }),
    }).data;

  beforeAll(async () => {
    await wasmInit();
  });

  it('reads two legitimate values on each surface as two different results', () => {
    // One control per surface: a field either side ignores would satisfy every
    // agreement assertion below without reading anything.
    expect(Array.from(pcenGain(0.2))).not.toEqual(Array.from(pcenGain(0.9)));
    expect(Array.from(wasmGain(0.2))).not.toEqual(Array.from(wasmGain(0.9)));
  });

  it('refuses a finite out-of-float-range value on both, with the same message', () => {
    for (const value of PAST_FLOAT_MAX) {
      const fromNode = capture(() => pcenGain(value));
      const fromWasm = capture(() => wasmGain(value));
      expect(fromNode, `node ${value}`).toBeInstanceOf(Error);
      expect(fromWasm, `wasm ${value}`).toBeInstanceOf(Error);
      // The class differs by surface convention (RangeError vs SonareError); the
      // sentence after the entry name does not.
      expect((fromNode as Error).message).toBe(`pcen: gain ${RANGE_MESSAGE}`);
      expect((fromWasm as Error).message).toBe(`pcen: gain ${RANGE_MESSAGE}`);
    }
  });

  it('accepts the boundary value on both', () => {
    expect(() => pcenGain(INSIDE_FLOAT_MAX)).not.toThrow();
    expect(() => wasmGain(INSIDE_FLOAT_MAX)).not.toThrow();
  });

  it('refuses a non-number by name on both', () => {
    for (const value of WRONG_TYPES) {
      const label = JSON.stringify(value);
      const fromNode = capture(() => pcenGain(value));
      expect(fromNode, `node gain ${label}`).toBeInstanceOf(TypeError);
      expect((fromNode as Error).message).toBe('pcen: gain must be a number');
      const fromWasm = capture(() => wasmGain(value));
      expect(fromWasm, `wasm gain ${label}`).toBeInstanceOf(Error);
      expect((fromWasm as Error).message).toBe('pcen: gain must be a number');
    }
  });
});
