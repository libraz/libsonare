/**
 * The two positional float readers refuse a finite number no 32-bit float can
 * hold, and still pass a non-finite one through.
 *
 * The input that matters is FINITE: `1e300` survives every JS-side check, and
 * narrowing it to float yields `+inf` — which is the spelling several of these
 * fields document as "unspecified", so an overflowed request was
 * indistinguishable from a caller who wrote `Infinity` on purpose. Each entry
 * point therefore pins both halves, and pins that the two reach DIFFERENT
 * observable answers; without that difference the refusal would be unverifiable.
 *
 * The bound is asserted one double-ULP wide on both signs rather than one
 * decade, so a limit off by any amount fails here.
 *
 * `analyzeBpm` covers `node_arg_float` (argument 2, `bpmMin`), `peakPick`
 * covers `RequiredFloatValue` (`delta`), `renderNotes` covers
 * `FloatArrayProperty` (each `amplitudeEnvelope` element). All three run
 * through the public facade, which forwards each value verbatim.
 */

import { describe, expect, it } from 'vitest';
import {
  analyzeBpm,
  ErrorCode,
  isSonareError,
  type NoteEditInput,
  onsetEnvelope,
  peakPick,
  renderNotes,
} from '../src/index.js';

/** The suffix every refusal from the float narrowing ends with. */
const RANGE_MESSAGE = 'must be a finite number within the 32-bit float range';

/** Largest magnitude a 32-bit float holds: the last value that must be accepted. */
const FLOAT_MAX = 3.4028234663852886e38;

/** The next double above `x`, so the boundary is pinned to one ULP. */
function nextUp(x: number): number {
  const view = new DataView(new ArrayBuffer(8));
  view.setFloat64(0, x);
  view.setBigUint64(0, view.getBigUint64(0) + 1n);
  return view.getFloat64(0);
}

/** Finite magnitudes no 32-bit float can hold. Every one must be refused. */
const PAST_FLOAT_MAX = [1e300, -1e300, nextUp(FLOAT_MAX), -nextUp(FLOAT_MAX)] as const;

const capture = (run: () => unknown): unknown => {
  try {
    run();
    return undefined;
  } catch (error) {
    return error;
  }
};

const SAMPLE_RATE = 22050;

/** Two seconds of clicks at 120 BPM: a signal whose tempo the analyzer resolves. */
const clicks = new Float32Array(SAMPLE_RATE * 2).map((_, i) => {
  const phase = i % (SAMPLE_RATE / 2);
  return phase < 200 ? 0.8 * Math.exp(-phase / 40) : 0;
});

describe('analyzeBpm narrows the bpmMin positional argument', () => {
  const run = (bpmMin?: number) =>
    analyzeBpm(
      bpmMin === undefined
        ? { samples: clicks, sampleRate: SAMPLE_RATE }
        : { samples: clicks, sampleRate: SAMPLE_RATE, bpmMin },
    );

  it('reads two legitimate bpmMin values as two different analyses', () => {
    // Without this every refusal below would pass equally well against an
    // argument the entry point never reads.
    const base = run();
    const low = run(100);
    const high = run(150);
    expect(low.candidates.length).not.toBe(base.candidates.length);
    expect(high.candidates.length).not.toBe(base.candidates.length);
    expect(low.candidates.length).not.toBe(high.candidates.length);
    expect(low.bpm).not.toBe(high.bpm);
  });

  it('refuses a finite bpmMin no float can hold, naming the argument', () => {
    for (const value of PAST_FLOAT_MAX) {
      const caught = capture(() => run(value));
      expect(caught, `bpmMin ${value}`).toBeInstanceOf(RangeError);
      expect((caught as RangeError).message).toBe(`argument 2 ${RANGE_MESSAGE}`);
    }
  });

  it('passes a non-finite bpmMin through to the core', () => {
    // Deliberate: the reader settles overflow, not what a field makes of
    // Infinity, so each non-finite input is answered where it is interpreted.
    const infinite = capture(() => run(Number.POSITIVE_INFINITY));
    expect(isSonareError(infinite)).toBe(true);
    expect((infinite as { code: number }).code).toBe(ErrorCode.InvalidParameter);
    expect(() => run(Number.NaN)).not.toThrow();
    expect(typeof run(Number.NaN).bpm).toBe('number');
  });

  it('answers 1e300 differently from the Infinity it would have become', () => {
    const overflow = capture(() => run(1e300));
    const infinite = capture(() => run(Number.POSITIVE_INFINITY));
    expect(overflow).toBeInstanceOf(RangeError);
    expect(isSonareError(overflow)).toBe(false);
    expect(isSonareError(infinite)).toBe(true);
    expect((overflow as Error).message).not.toBe((infinite as Error).message);
  });

  it('accepts the largest bpmMin a float holds and refuses the next double up', () => {
    // Accepted by the reader: the answer is the core's own refusal of an absurd
    // tempo floor, not the narrowing's RangeError.
    const held = capture(() => run(FLOAT_MAX));
    expect(held).not.toBeInstanceOf(RangeError);
    expect(isSonareError(held)).toBe(true);
    expect(capture(() => run(nextUp(FLOAT_MAX)))).toBeInstanceOf(RangeError);
  });
});

describe('peakPick narrows the required delta value', () => {
  const envelope = onsetEnvelope(clicks, SAMPLE_RATE);
  const pick = (delta: number) =>
    peakPick({ values: envelope, preMax: 3, postMax: 3, preAvg: 3, postAvg: 5, delta, wait: 5 });
  const peaks = (delta: number) => pick(delta).length;

  it('reads two legitimate delta values as two different peak sets', () => {
    // 0 is the default the positional overload supplies; delta raises the
    // threshold above the running mean, so more of it picks fewer peaks.
    expect(peaks(20)).not.toBe(peaks(0));
    expect(peaks(50)).not.toBe(peaks(0));
    expect(peaks(20)).not.toBe(peaks(50));
    expect(peaks(50)).toBeLessThan(peaks(20));
  });

  it('refuses a finite delta no float can hold, naming the argument', () => {
    for (const value of PAST_FLOAT_MAX) {
      const caught = capture(() => peaks(value));
      expect(caught, `delta ${value}`).toBeInstanceOf(RangeError);
      expect((caught as RangeError).message).toBe(`delta ${RANGE_MESSAGE}`);
    }
  });

  it('passes a non-finite delta through to the core', () => {
    // Infinity is a threshold nothing clears; NaN loses every comparison, so
    // the run reads as an unset threshold. Both are answers, not refusals.
    expect(peaks(Number.POSITIVE_INFINITY)).toBe(0);
    expect(() => peaks(Number.NaN)).not.toThrow();
    expect(pick(Number.NaN)).toBeInstanceOf(Int32Array);
  });

  it('answers 1e300 by refusing, where Infinity still returns a result', () => {
    expect(pick(Number.POSITIVE_INFINITY)).toBeInstanceOf(Int32Array);
    expect(capture(() => peaks(1e300))).toBeInstanceOf(RangeError);
  });

  it('accepts the largest delta a float holds, on both signs', () => {
    // Accepted AND consumed: the two saturating signs give the two extreme
    // answers, so the boundary value reached the threshold rather than a default.
    expect(peaks(FLOAT_MAX)).toBe(0);
    expect(peaks(-FLOAT_MAX)).toBe(peaks(0));
    expect(peaks(-FLOAT_MAX)).toBeGreaterThan(0);
  });
});

describe('renderNotes narrows every amplitudeEnvelope element', () => {
  const tone = new Float32Array(SAMPLE_RATE / 2).map(
    (_, i) => 0.5 * Math.sin((2 * Math.PI * 220 * i) / SAMPLE_RATE),
  );

  /** One note rendered over the tone, reduced to its total absolute output. */
  const render = (amplitudeEnvelope?: unknown): number => {
    const edit = (amplitudeEnvelope === undefined ? {} : { amplitudeEnvelope }) as NoteEditInput;
    const out = renderNotes({
      samples: tone,
      sampleRate: SAMPLE_RATE,
      notes: [{ onsetSample: 1000, offsetSample: 9000, edit }],
    });
    let total = 0;
    for (let i = 0; i < out.length; i++) {
      total += Math.abs(out[i]);
    }
    return total;
  };

  it('reads two legitimate envelopes as two different renders', () => {
    // An identity edit is not resynthesized at all, so the omitted key is a
    // third answer rather than a synonym for a flat envelope.
    const flat = render([1, 1]);
    const fade = render([1, 0]);
    expect(flat).not.toBe(render());
    expect(fade).not.toBe(render());
    expect(fade).not.toBe(flat);
    expect(fade).toBeLessThan(flat);
  });

  it('refuses an element no float can hold, naming the key and the index', () => {
    for (const value of PAST_FLOAT_MAX) {
      const caught = capture(() => render([1, 1, value]));
      expect(caught, `element ${value}`).toBeInstanceOf(RangeError);
      expect((caught as RangeError).message).toBe(`amplitudeEnvelope[2] ${RANGE_MESSAGE}`);
    }
  });

  it('labels the element that was refused, not the first one', () => {
    // The bad element sits away from position 0 above and here, because an
    // off-by-one in the index is invisible to a position-0 case.
    const at = (index: number) => {
      const envelope = [1, 1, 1, 1, 1, 1];
      envelope[index] = 1e300;
      return (capture(() => render(envelope)) as RangeError).message;
    };
    expect(at(1)).toBe(`amplitudeEnvelope[1] ${RANGE_MESSAGE}`);
    expect(at(5)).toBe(`amplitudeEnvelope[5] ${RANGE_MESSAGE}`);
  });

  it('refuses 1e300 in a plain array and not in a Float32Array', () => {
    // These are two different VALUES, not two spellings of one. JS folds 1e300
    // to Infinity when it stores it in a Float32Array, so the typed path never
    // receives the number the caller wrote and has nothing left to refuse,
    // while the plain array still carries it. Making the two agree would mean
    // destroying the surviving copy to match the case that lost it.
    const plain = capture(() => render([1, 1, 1e300]));
    const typed = capture(() => render(Float32Array.from([1, 1, 1e300])));
    expect(plain).toBeInstanceOf(RangeError);
    expect((plain as RangeError).message).toBe(`amplitudeEnvelope[2] ${RANGE_MESSAGE}`);
    // Not refused by the reader: the typed element reaches the core as the
    // infinity it became, and is answered there exactly as a written one is.
    expect(typed).not.toBeInstanceOf(RangeError);
    expect(isSonareError(typed)).toBe(true);
    expect((typed as { code: number }).code).toBe(ErrorCode.InvalidParameter);
    const written = capture(() => render(Float32Array.from([1, 1, Number.POSITIVE_INFINITY])));
    expect((typed as Error).message).toBe((written as Error).message);
  });

  it('still reads a non-numeric element as NaN', () => {
    // Unchanged by the narrowing, which only ever sees a number: a string
    // element is answered wherever a written NaN is answered.
    const text = capture(() => render([1, 1, 'x']));
    const nan = capture(() => render([1, 1, Number.NaN]));
    expect(text).not.toBeInstanceOf(RangeError);
    expect(isSonareError(text)).toBe(true);
    expect((text as Error).message).toBe((nan as Error).message);
  });

  it('accepts the largest element a float holds and refuses the next double up', () => {
    // Accepted AND consumed: FLOAT_MAX renders, and renders nothing a legitimate
    // envelope could produce. The negative sign is the core's to refuse, a gain
    // being non-negative, so there the reader shows by not being the one to answer.
    expect(render([1, 1, FLOAT_MAX])).toBeGreaterThan(render([1, 1, 1]));
    expect(capture(() => render([1, 1, nextUp(FLOAT_MAX)]))).toBeInstanceOf(RangeError);
    expect(capture(() => render([1, 1, -FLOAT_MAX]))).not.toBeInstanceOf(RangeError);
    expect(capture(() => render([1, 1, -nextUp(FLOAT_MAX)]))).toBeInstanceOf(RangeError);
  });
});
