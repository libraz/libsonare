/**
 * Window-scoped validation and dB-parameter domain for the spectrum meters.
 *
 * `meteringSpectrumFrame` reads exactly `[frameOffset, frameOffset + nFft)` and
 * that frame is the only span it validates: a non-finite sample inside the frame
 * is refused, one outside it neither reaches the FFT nor refuses the call. The
 * emptiness and `sampleRate` checks still cover the whole buffer, and the index
 * a refusal quotes is absolute -- an index into the caller's buffer, not into
 * the frame.
 *
 * `dbRef` and `dbAmin` take exactly `0` as "use the library default"; every
 * other value outside `[0, inf)` is refused rather than promoted to the default
 * or applied as a real level.
 */

import { describe, expect, it } from 'vitest';
import { meteringSpectrum, meteringSpectrumFrame } from '../src/index.js';

const SR = 22050;
const LENGTH = 8192;
// The frame sits away from both ends of the buffer, so a scan anchored at 0 and
// a scan anchored at the frame cover visibly different spans.
const FRAME_OFFSET = 1024;
const N_FFT = 512;
const FRAME_END = FRAME_OFFSET + N_FFT;
// The library's own fallback FFT size, which the facade mirrors to size the scan
// for `nFft: 0`. Pinned below against the `nFft` the library reports back.
const DEFAULT_N_FFT = 2048;

function tone(length = LENGTH): Float32Array {
  return Float32Array.from({ length }, (_, i) => 0.5 * Math.sin((2 * Math.PI * 440 * i) / SR));
}

function toneWith(index: number, value: number): Float32Array {
  const buf = tone();
  buf[index] = value;
  return buf;
}

function expectSameSpectrum(
  actual: ReturnType<typeof meteringSpectrumFrame>,
  expected: ReturnType<typeof meteringSpectrumFrame>,
): void {
  expect(actual.nFft).toBe(expected.nFft);
  expect(actual.sampleRate).toBe(expected.sampleRate);
  expect(Array.from(actual.magnitude)).toEqual(Array.from(expected.magnitude));
  expect(Array.from(actual.power)).toEqual(Array.from(expected.power));
  expect(Array.from(actual.db)).toEqual(Array.from(expected.db));
}

describe('meteringSpectrumFrame validates only the analysis frame', () => {
  it.each([Number.NaN, Number.POSITIVE_INFINITY, Number.NEGATIVE_INFINITY])(
    'accepts %p outside the frame and returns the clean spectrum',
    (bad) => {
      const expected = meteringSpectrumFrame(tone(), SR, FRAME_OFFSET, { nFft: N_FFT });
      const actual = meteringSpectrumFrame(toneWith(4000, bad), SR, FRAME_OFFSET, { nFft: N_FFT });
      expectSameSpectrum(actual, expected);
      expect(Math.max(...actual.magnitude)).toBeGreaterThan(0);
    },
  );

  it.each([Number.NaN, Number.POSITIVE_INFINITY])(
    'refuses %p inside the frame by absolute buffer index',
    (bad) => {
      // The offending sample sits 176 samples into a frame that starts at 1024,
      // so the exact index separates an absolute report from a frame-relative
      // one -- and this is the assertion a deleted scan cannot survive.
      const badIndex = FRAME_OFFSET + 176;
      expect(() =>
        meteringSpectrumFrame(toneWith(badIndex, bad), SR, FRAME_OFFSET, { nFft: N_FFT }),
      ).toThrow(`meteringSpectrumFrame: samples contains NaN or Inf at index ${badIndex}`);
    },
  );

  it('treats the frame as half-open at both edges', () => {
    // The inside cases fail if the scan is deleted; the outside cases fail if it
    // widens back to the whole buffer, so neither edge can drift silently.
    for (const inside of [FRAME_OFFSET, FRAME_END - 1]) {
      expect(() =>
        meteringSpectrumFrame(toneWith(inside, Number.NaN), SR, FRAME_OFFSET, { nFft: N_FFT }),
      ).toThrow(`at index ${inside}`);
    }
    for (const outside of [FRAME_OFFSET - 1, FRAME_END]) {
      const report = meteringSpectrumFrame(toneWith(outside, Number.NaN), SR, FRAME_OFFSET, {
        nFft: N_FFT,
      });
      expect(Array.from(report.magnitude).every(Number.isFinite)).toBe(true);
    }
  });

  it('never reports a sample that is not there for a fractional frameOffset', () => {
    // A fractional index reads `undefined` out of the buffer, which a scan that
    // used the offset unfloored reported as a non-finite sample. The addon takes
    // a fractional offset rather than refusing it -- that divergence from the
    // WASM surface is its own question -- so what is pinned here is only that
    // the facade does not answer it by naming a sample the caller never passed.
    let caught: unknown;
    try {
      meteringSpectrumFrame(tone(), SR, 100.5, { nFft: N_FFT });
    } catch (error) {
      caught = error;
    }
    if (caught !== undefined) {
      expect(String((caught as Error).message)).not.toContain('NaN or Inf');
    }
  });

  it('still refuses an empty buffer whatever the frame asks for', () => {
    expect(() => meteringSpectrumFrame(new Float32Array(0), SR, 99999, { nFft: N_FFT })).toThrow(
      /must not be empty/,
    );
  });

  it('scans the span the library actually reads when nFft is omitted', () => {
    // Two halves. The reported nFft pins the mirrored default against the
    // library; the index probes pin the scan against the same value. A drifted
    // mirror would scan a span the call does not read, which neither half alone
    // can see.
    expect(meteringSpectrumFrame(tone(), SR, FRAME_OFFSET, {}).nFft).toBe(DEFAULT_N_FFT);

    const lastInside = FRAME_OFFSET + DEFAULT_N_FFT - 1;
    const firstOutside = FRAME_OFFSET + DEFAULT_N_FFT;
    expect(firstOutside).toBeLessThan(LENGTH);
    expect(() =>
      meteringSpectrumFrame(toneWith(lastInside, Number.NaN), SR, FRAME_OFFSET, {}),
    ).toThrow(`at index ${lastInside}`);
    const report = meteringSpectrumFrame(toneWith(firstOutside, Number.NaN), SR, FRAME_OFFSET, {});
    expect(report.nFft).toBe(DEFAULT_N_FFT);
    expect(Array.from(report.magnitude).every(Number.isFinite)).toBe(true);
  });

  it('drops the facade scan under validate:false but not the core guard', () => {
    const inside = FRAME_OFFSET + 200;
    // The two refusals are distinguishable: the facade names the index, the core
    // does not.
    expect(() =>
      meteringSpectrumFrame(toneWith(inside, Number.NaN), SR, FRAME_OFFSET, {
        nFft: N_FFT,
        validate: false,
      }),
    ).toThrow();
    expect(() =>
      meteringSpectrumFrame(toneWith(inside, Number.NaN), SR, FRAME_OFFSET, {
        nFft: N_FFT,
        validate: false,
      }),
    ).not.toThrow(`at index ${inside}`);

    const unchecked = meteringSpectrumFrame(toneWith(4000, Number.NaN), SR, FRAME_OFFSET, {
      nFft: N_FFT,
      validate: false,
    });
    expectSameSpectrum(unchecked, meteringSpectrumFrame(tone(), SR, FRAME_OFFSET, { nFft: N_FFT }));
  });
});

describe('the spectrum dB parameters refuse everything outside their domain', () => {
  const entries = [
    ['meteringSpectrum', (options: object) => meteringSpectrum(tone(), SR, options)],
    [
      'meteringSpectrumFrame',
      (options: object) => meteringSpectrumFrame(tone(), SR, FRAME_OFFSET, options),
    ],
  ] as const;

  for (const [name, run] of entries) {
    it.each([Number.NaN, Number.POSITIVE_INFINITY, -1])(`${name} refuses dbRef %p`, (bad) => {
      expect(() => run({ nFft: N_FFT, dbRef: bad })).toThrow();
    });

    it.each([Number.NaN, Number.POSITIVE_INFINITY, -1])(`${name} refuses dbAmin %p`, (bad) => {
      expect(() => run({ nFft: N_FFT, dbAmin: bad })).toThrow();
    });

    it(`${name} still applies an in-domain dbRef`, () => {
      // The control. Without it the refusals above are satisfied by an entry
      // point that rejects every dbRef. A reference of 2 divides every linear
      // magnitude by two, which is 20*log10(2) dB down; bins that saturate at
      // the dB floor on either side are excluded.
      const base = run({ nFft: N_FFT }).db;
      const scaled = run({ nFft: N_FFT, dbRef: 2 }).db;
      const deltas: number[] = [];
      for (let i = 0; i < base.length; i++) {
        if ((base[i] as number) > -119 && (scaled[i] as number) > -119) {
          deltas.push((base[i] as number) - (scaled[i] as number));
        }
      }
      expect(deltas.length).toBeGreaterThan(0);
      for (const delta of deltas) {
        expect(delta).toBeCloseTo(20 * Math.log10(2), 3);
      }
    });
  }
});
