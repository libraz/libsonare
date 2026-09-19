/**
 * Take alignment on the WASM surface: the standalone `alignTakeToReference` over
 * the sonare_c_project_edit.h C ABI.
 *
 * The fixture is a pair of continuous pitch glides at 22.05 kHz — the same 220 Hz
 * to 440 Hz sweep, five partials at `1/h`, traversed over 2.0 s by the reference
 * and 2.6 s by the take. Continuous and not a held tone on purpose: a held tone
 * leaves every path through the cost matrix equally good, so the anchors it comes
 * back with say nothing about whether an alignment happened. The 30 % length
 * difference is what gives the two axes different ends, which is what makes the
 * orientation checks below able to fail.
 */

import { beforeAll, describe, expect, it } from 'vitest';
import type { AlignTakeToReferenceRequest, AlignTakeToReferenceResult } from '../src/index';
import { alignTakeToReference, ErrorCode, init, isSonareError } from '../src/index';

const sampleRate = 22050;
/** 2.0 s of reference against 2.6 s of take. Neither is a multiple of a hop below. */
const referenceLength = 44100;
const takeLength = 57330;

/**
 * An exponential 220 Hz to 440 Hz glide over the whole buffer — exponential in Hz
 * is linear in pitch, so both buffers walk the same chroma path at different
 * rates. Five partials at `1/h`, with a 10 ms fade at each end.
 */
function glide(length: number): Float32Array {
  const out = new Float32Array(length);
  const fade = Math.round(sampleRate * 0.01);
  let phase = 0;
  for (let i = 0; i < length; i++) {
    const f0 = 220 * 2 ** (i / (length - 1));
    phase += (2 * Math.PI * f0) / sampleRate;
    let sum = 0;
    for (let h = 1; h <= 5; h++) {
      if (h * f0 >= sampleRate / 2) {
        break;
      }
      sum += (0.3 / h) * Math.sin(h * phase);
    }
    const gain = i < fade ? i / fade : i > length - fade ? (length - i) / fade : 1;
    out[i] = sum * gain;
  }
  return out;
}

/** The anchor count and both axes, as a comparable key. */
function shape(result: AlignTakeToReferenceResult): string {
  return JSON.stringify(result.anchors.map((a) => [a.warpSample, a.sourceSample]));
}

let reference: Float32Array;
let take: Float32Array;
let baseline: AlignTakeToReferenceResult;

describe('WASM alignTakeToReference', () => {
  beforeAll(async () => {
    await init();
    reference = glide(referenceLength);
    take = glide(takeLength);
    baseline = alignTakeToReference({ reference, take, sampleRate });
  });

  it('returns a usable warp map for the take', () => {
    expect(baseline.anchors.length).toBeGreaterThanOrEqual(2);

    for (let i = 1; i < baseline.anchors.length; i++) {
      expect(baseline.anchors[i].warpSample).toBeGreaterThan(baseline.anchors[i - 1].warpSample);
      expect(baseline.anchors[i].sourceSample).toBeGreaterThan(
        baseline.anchors[i - 1].sourceSample,
      );
    }

    const first = baseline.anchors[0];
    expect(first.warpSample).toBeGreaterThanOrEqual(0);
    expect(first.sourceSample).toBeGreaterThanOrEqual(0);

    // Each axis has to end inside the buffer it indexes, and has to have covered
    // most of it: a containment check on its own is satisfied by a pair of
    // anchors near the start.
    const last = baseline.anchors[baseline.anchors.length - 1];
    expect(last.warpSample).toBeLessThan(referenceLength);
    expect(last.warpSample).toBeGreaterThan(referenceLength / 2);
    expect(last.sourceSample).toBeLessThan(takeLength);
    expect(last.sourceSample).toBeGreaterThan(takeLength / 2);
  });

  it('orients the anchors for the take, not for the reference', () => {
    // warpSample is a position on the REFERENCE timeline and sourceSample the
    // matching one in the TAKE, which is the direction a clip whose source is the
    // take needs. The take is the longer of the two, so a swapped pair of axes
    // reverses this comparison -- that is the whole reason the fixture's two
    // lengths differ.
    const last = baseline.anchors[baseline.anchors.length - 1];
    expect(last.sourceSample).toBeGreaterThan(last.warpSample);

    // Same swap, seen through the summary: the frame counts are named for the
    // arguments this entry point took, not the ones it passed on.
    expect(baseline.alignment.takeFrames).toBeGreaterThan(baseline.alignment.referenceFrames);
    expect(baseline.alignment.takeFrames / baseline.alignment.referenceFrames).toBeCloseTo(
      takeLength / referenceLength,
      1,
    );
    expect(Number.isFinite(baseline.alignment.meanResidualFrames)).toBe(true);
    expect(baseline.alignment.meanResidualFrames).toBeGreaterThanOrEqual(0);
  });

  it('measures more frames at a finer hop, and fewer at a coarser one', () => {
    // hopLength sets the chroma frame rate, so it decides how many anchors there
    // are to reduce. A reader that accepted the field and dropped it would answer
    // both calls with the default resolution and the same count.
    const fine = alignTakeToReference({ reference, take, sampleRate, hopLength: 256 });
    const coarse = alignTakeToReference({ reference, take, sampleRate, hopLength: 1024 });

    expect(fine.anchors.length).toBeGreaterThanOrEqual(2);
    expect(coarse.anchors.length).toBeGreaterThanOrEqual(2);
    expect(fine.anchors.length).not.toBe(coarse.anchors.length);
    expect(fine.alignment.referenceFrames).toBeGreaterThan(coarse.alignment.referenceFrames);
  });

  it('forwards binsPerOctave to the resolution the core folds from', () => {
    // Twice the CQT resolution behind the same twelve pitch classes: the frame
    // rate is hopLength's business, so the frame counts must NOT move.
    const finer = alignTakeToReference({ reference, take, sampleRate, binsPerOctave: 24 });
    expect(finer.anchors.length).toBeGreaterThanOrEqual(2);
    expect(finer.alignment.referenceFrames).toBe(baseline.alignment.referenceFrames);
    expect(finer.alignment.takeFrames).toBe(baseline.alignment.takeFrames);

    // And this is what proves the value is forwarded rather than accepted and
    // dropped: each pitch class takes the mean of a whole number of CQT bins, so a
    // resolution that is not a multiple of twelve is refused by the CORE. The
    // surface reader only checks the sign, so it cannot be the one refusing this.
    let thrown: unknown;
    try {
      alignTakeToReference({ reference, take, sampleRate, binsPerOctave: 13 });
    } catch (error) {
      thrown = error;
    }
    expect(thrown).toBeDefined();
    expect(isSonareError(thrown) && thrown.code).toBe(ErrorCode.InvalidParameter);
  });

  it('reads an absent resolution field as the library value however absence is spelled', () => {
    // The control for the refusals below. This reader zero-initializes the config
    // rather than seeding it from the C defaults, so an omitted field IS the 0 the
    // guard refuses, and presence is the only thing separating them. Nothing but a
    // call with the field left out can show that separation works -- a value check
    // would refuse omission, and no rejection test would notice.
    const expected = shape(baseline);
    for (const key of ['hopLength', 'binsPerOctave'] as const) {
      for (const absent of [undefined, null]) {
        const result = alignTakeToReference({
          reference,
          take,
          sampleRate,
          [key]: absent,
        } as AlignTakeToReferenceRequest);
        expect(shape(result), `${key}: ${String(absent)}`).toBe(expected);
      }
    }
  });

  it('refuses a buffer or a rate it cannot use', () => {
    const empty = new Float32Array(0);
    expect(() => alignTakeToReference({ reference: empty, take, sampleRate })).toThrow(
      /reference must not be empty/,
    );
    expect(() => alignTakeToReference({ reference, take: empty, sampleRate })).toThrow(
      /take must not be empty/,
    );

    // Named per buffer, so a scan that only ever reads the first argument shows up
    // as the wrong name rather than as a pass.
    const nanReference = reference.slice();
    nanReference[1000] = Number.NaN;
    expect(() => alignTakeToReference({ reference: nanReference, take, sampleRate })).toThrow(
      /reference contains NaN or Inf/,
    );
    const nanTake = take.slice();
    nanTake[1000] = Number.POSITIVE_INFINITY;
    expect(() => alignTakeToReference({ reference, take: nanTake, sampleRate })).toThrow(
      /take contains NaN or Inf/,
    );

    expect(() => alignTakeToReference({ reference, take, sampleRate: 0 })).toThrow(RangeError);
    expect(() => alignTakeToReference({ reference, take, sampleRate: 22050.5 })).toThrow(
      RangeError,
    );
    expect(() => alignTakeToReference({ reference, take, sampleRate: 400000 })).toThrow(RangeError);
  });

  it('reads a written zero as the library value, the way the C ABI defines it', () => {
    // 0 is the C ABI's "use the library value" on both fields, so a written 0 and an
    // omitted key are one request -- the same answer the other two surfaces give,
    // which is what this case holds this surface to.
    for (const options of [
      { hopLength: 0 },
      { binsPerOctave: 0 },
      {
        hopLength: 0,
        binsPerOctave: 0,
      },
    ]) {
      const written = alignTakeToReference({ reference, take, sampleRate, ...options });
      expect(written.anchors.length).toBe(baseline.anchors.length);
      expect(written.alignment.referenceFrames).toBe(baseline.alignment.referenceFrames);
    }
  });

  it('names the field for a value it cannot read, and does not for one it cannot use', () => {
    // Two layers refuse, and which one answers decides what the caller is told.
    // The reader knows the field's name and nothing about its domain, so a value
    // of the wrong shape comes back named. A well-formed value the measurement
    // cannot use travels past the reader, and the core reports the condition
    // without knowing which argument carried it. Asserting the difference is what
    // keeps a domain check from drifting back into this layer, where it would
    // duplicate the core's own and could disagree with it.
    const unreadable: Array<[string, Partial<AlignTakeToReferenceRequest>, RegExp]> = [
      ['a string where a number belongs', { hopLength: '512' as unknown as number }, /hopLength/],
      ['a fraction where a whole number belongs', { hopLength: 512.5 }, /hopLength/],
    ];
    for (const [name, options, field] of unreadable) {
      let thrown: unknown;
      try {
        alignTakeToReference({ reference, take, sampleRate, ...options });
      } catch (error) {
        thrown = error;
      }
      expect(thrown, name).toBeDefined();
      expect((thrown as Error).message, name).toMatch(field);
    }

    const unusable: Array<[string, Partial<AlignTakeToReferenceRequest>]> = [
      ['a negative hop', { hopLength: -256 }],
      ['a negative resolution', { binsPerOctave: -12 }],
      // Positive and a whole number, and still refused: the chroma folds onto
      // twelve pitch classes and 18 does not divide by them. Nothing short of the
      // core's own grid knows that, which is why this case is here rather than
      // among the named ones above.
      ['a resolution off the twelve-bin grid', { binsPerOctave: 18 }],
    ];
    for (const [name, options] of unusable) {
      let thrown: unknown;
      try {
        alignTakeToReference({ reference, take, sampleRate, ...options });
      } catch (error) {
        thrown = error;
      }
      expect(thrown, name).toBeDefined();
      expect(isSonareError(thrown) && thrown.code, name).toBe(ErrorCode.InvalidParameter);
    }
  });

  it('refuses a pair it cannot produce two distinct anchors from', () => {
    // Shorter than one chroma hop, so neither signal yields the two frames an
    // alignment needs. Reported rather than answered with a map the caller cannot
    // hand to setWarpMap.
    const tiny = glide(256);
    let thrown: unknown;
    try {
      alignTakeToReference({ reference: tiny, take: tiny.slice(0, 200), sampleRate });
    } catch (error) {
      thrown = error;
    }
    expect(thrown).toBeDefined();
    expect(isSonareError(thrown) && thrown.code).toBe(ErrorCode.InvalidParameter);
  });
});
