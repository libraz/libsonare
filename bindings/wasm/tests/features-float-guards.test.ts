/**
 * What a POSITIONAL `float` parameter is allowed to become on its way into an
 * embind-registered *feature* function, covering the 18 parameters converted
 * across `src/wasm/bindings/features/{spectrogram,pitch,music}.cpp`.
 *
 * Same defect as `positional-float-guards.test.ts`: embind's float glue
 * SATURATES, so a caller-chosen finite number wider than `FLT_MAX`
 * (~3.4028235e38) used to arrive as an infinity with no exception raised
 * anywhere. `3.5e38` is the case that matters most, because it is a number a
 * caller could plausibly choose rather than a deliberately extreme one.
 *
 * These entry points are driven off the module rather than through the TS
 * facades: several facades (`melSpectrogram`, `mfcc`, `pseudoCqt`,
 * `hybridCqt`, `analyzeMelody`, `analyzeSections`) already run their own
 * `assertFiniteScalar` on the JS side, which would intercept `NaN`/`Infinity`
 * before they ever reach the WASM boundary under test here and throw a plain
 * `RangeError` instead of the branded `SonareError` this file asserts. Driving
 * the module directly measures the embind parameter itself, independent of
 * that JS-side duplicate. `nnlsChroma`'s non-`Ex` embind function has no TS
 * facade at all reaching it (the facade always calls `nnlsChromaEx`), so it is
 * only reachable this way regardless.
 *
 * Acceptance is asserted by reading a value back, never by "it did not
 * throw" — a function that ignored its argument entirely would satisfy every
 * refusal case below in exactly the same way.
 */

import { beforeAll, describe, expect, it } from 'vitest';
import { ErrorCode, init, isSonareError, type SonareError } from '../src/index';
import { getSonareModule } from '../src/module_state';

const SR = 8000;
const N = 4096; // ~0.512s at SR -- short enough for the expensive (CQT/pYIN) entries too.

/** The suffix every one of these refusals ends with, whatever the key is. */
const RANGE_MESSAGE = 'must be a finite number within the 32-bit float range';

/**
 * Values that saturate onto a legal float. These are the ones that matter:
 * each is a number the caller chose, survives every JS-side check, and used
 * to arrive as an infinity a config guard written `x > lo` or `x < hi` waves
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

/** A pure tone or sum of tones, generated without touching the WASM module. */
function mixTones(
  partials: Array<{ freqHz: number; amp: number }>,
  sr: number,
  n: number,
): Float32Array {
  const out = new Float32Array(n);
  for (let i = 0; i < n; i++) {
    let s = 0;
    for (const p of partials) {
      s += p.amp * Math.sin((2 * Math.PI * p.freqHz * i) / sr);
    }
    out[i] = s;
  }
  return out;
}

function sumAbs(values: Float32Array): number {
  let total = 0;
  for (const v of values) {
    total += Math.abs(v);
  }
  return total;
}

function l1Distance(a: Float32Array, b: Float32Array): number {
  let total = 0;
  for (let i = 0; i < a.length; i++) {
    total += Math.abs((a[i] as number) - (b[i] as number));
  }
  return total;
}

const pureTone300 = mixTones([{ freqHz: 300, amp: 0.8 }], SR, N);
const threePartials = mixTones(
  [
    { freqHz: 300, amp: 1 },
    { freqHz: 1200, amp: 1 },
    { freqHz: 3200, amp: 1 },
  ],
  SR,
  N,
);

function m() {
  return getSonareModule();
}

// mfccToMel/mfccToAudio need a real, non-trivial MFCC matrix as input. It is
// only obtainable through the module, so it is computed once the first time a
// field needs it and cached rather than at module-eval time (before init()).
const N_MFCC = 8;
let cachedMfcc: { coefficients: Float32Array; nFrames: number } | undefined;
function mfccResult() {
  if (!cachedMfcc) {
    const result = m().mfcc(threePartials, SR, 512, 256, 16, N_MFCC, 0, 0, false, 0);
    cachedMfcc = { coefficients: result.coefficients, nFrames: result.nFrames };
  }
  return cachedMfcc;
}
function mfccCoeffs() {
  return mfccResult().coefficients;
}
function N_FRAMES() {
  return mfccResult().nFrames;
}

/** One float field of one entry point, and how to drive it with a value. */
interface FloatField {
  entry: string;
  key: string;
  run: (value: number) => unknown;
}

const FLOAT_FIELDS: FloatField[] = [
  {
    entry: 'melSpectrogram',
    key: 'fmin',
    run: (v) => m().melSpectrogram(threePartials, SR, 512, 256, 16, v, 3600, false),
  },
  {
    entry: 'melSpectrogram',
    key: 'fmax',
    run: (v) => m().melSpectrogram(threePartials, SR, 512, 256, 16, 0, v, false),
  },
  {
    entry: 'mfcc',
    key: 'fmin',
    run: (v) => m().mfcc(threePartials, SR, 512, 256, 16, 8, v, 3600, false, 0),
  },
  {
    entry: 'mfcc',
    key: 'fmax',
    run: (v) => m().mfcc(threePartials, SR, 512, 256, 16, 8, 0, v, false, 0),
  },
  {
    entry: 'mfcc',
    key: 'lifter',
    run: (v) => m().mfcc(threePartials, SR, 512, 256, 16, 8, 0, 0, false, v),
  },
  {
    entry: 'mfccToMel',
    key: 'lifter',
    run: (v) => m().mfccToMel(mfccCoeffs(), N_MFCC, N_FRAMES(), 16, v),
  },
  {
    entry: 'mfccToAudio',
    key: 'lifter',
    run: (v) =>
      m().mfccToAudio(mfccCoeffs(), N_MFCC, N_FRAMES(), 16, SR, 512, 256, 0, 3600, 4, false, v),
  },
  {
    entry: 'pitchYin',
    key: 'fmax',
    run: (v) => m().pitchYin(pureTone300, SR, 1024, 256, 65, v, 0.1, false),
  },
  {
    entry: 'pitchPyin',
    key: 'fmax',
    run: (v) => m().pitchPyin(pureTone300, SR, 1024, 256, 65, v, 0.1, false),
  },
  {
    entry: 'piptrack',
    key: 'fmax',
    run: (v) => m().piptrack(pureTone300, SR, 512, 256, 65, v, 0.1),
  },
  {
    entry: 'piptrack',
    key: 'threshold',
    run: (v) => m().piptrack(pureTone300, SR, 512, 256, 65, 3600, v),
  },
  {
    entry: 'analyzeSections',
    key: 'minSectionSec',
    run: (v) => m().analyzeSections(threePartials, SR, 512, 256, v),
  },
  {
    entry: 'analyzeMelody',
    key: 'fmax',
    run: (v) => m().analyzeMelody(pureTone300, SR, 65, v, 1024, 256, 0.1, false, true),
  },
  {
    entry: 'analyzeMelody',
    key: 'threshold',
    run: (v) => m().analyzeMelody(pureTone300, SR, 65, 3600, 1024, 256, v, false, true),
  },
  {
    entry: 'pseudoCqt',
    key: 'fmin',
    run: (v) => m().pseudoCqt(pureTone300, SR, 256, v, 12, 12),
  },
  {
    entry: 'hybridCqt',
    key: 'fmin',
    run: (v) => m().hybridCqt(pureTone300, SR, 256, v, 12, 12),
  },
  {
    entry: 'nnlsChroma',
    key: 'stftBlendWeight',
    run: (v) => m().nnlsChroma(threePartials, SR, true, v, 512),
  },
  {
    entry: 'nnlsChromaEx',
    key: 'stftBlendWeight',
    run: (v) => m().nnlsChromaEx(threePartials, SR, true, v, 512, 256),
  },
];

beforeAll(async () => {
  await init();
});

describe('every converted float parameter refuses a value the float type cannot hold', () => {
  it('covers one field per formerly-plain-float feature parameter', () => {
    // A field dropped from the table stops being covered without anything
    // going red, which is the one way this file could quietly shrink.
    expect(FLOAT_FIELDS).toHaveLength(18);
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

// A dedicated signal for the mel filterbank tests below, matched to the exact
// configuration (22050 Hz, 8192 samples, three equal-amplitude partials at
// 300/1200/3200 Hz) the ordering was measured against, rather than the
// SR=8000 signal used elsewhere in this file: the direction below is NOT the
// naive "wider band admits more partials, so the total grows" story, and the
// measurement is configuration-specific enough that reusing it verbatim is
// safer than re-deriving it under a different sample rate.
const MEL_SR = 22050;
const melFilterbankSignal = mixTones(
  [
    { freqHz: 300, amp: 1 },
    { freqHz: 1200, amp: 1 },
    { freqHz: 3200, amp: 1 },
  ],
  MEL_SR,
  8192,
);

describe('melSpectrogram consumes the fmin/fmax it accepts', () => {
  it('reads three fmax levels as three DECREASING captured-power totals', () => {
    // With nMels fixed at 16, narrowing fmax does not just admit fewer
    // partials -- it also packs the same 16 triangular filters into less
    // spectrum. Slaney (area) normalization gives each filter a gain of
    // `enorm = 2 / width` (mel.cpp), and a low fmax concentrates every filter
    // into the region where the mel scale is most compressed, where Hz-width
    // per filter is smallest and so `enorm` is largest. That per-filter gain
    // growth dominates the loss of the excluded high partials, so the total
    // FALLS as fmax widens -- do not "fix" this back to growing with fmax.
    const narrow = m().melSpectrogram(melFilterbankSignal, MEL_SR, 512, 256, 16, 0, 500, false);
    const mid = m().melSpectrogram(melFilterbankSignal, MEL_SR, 512, 256, 16, 0, 2000, false);
    const wide = m().melSpectrogram(melFilterbankSignal, MEL_SR, 512, 256, 16, 0, 8000, false);
    const total = (r: { power: Float32Array }) => sumAbs(r.power);
    expect(total(mid)).toBeLessThan(total(narrow));
    expect(total(wide)).toBeLessThan(total(mid));
  });

  it('reads three fmin levels as three decreasing captured-power totals', () => {
    // Raising fmin moves the whole filterbank away from that same
    // low-frequency, gain-rich region into flatter mel territory (larger
    // per-filter Hz-width, smaller `enorm`), while also excluding the low
    // partials outright -- both effects push the total down as fmin rises,
    // the opposite-signed effect from the fmax sweep above despite sharing
    // the same underlying normalization.
    const fromZero = m().melSpectrogram(melFilterbankSignal, MEL_SR, 512, 256, 16, 0, 8000, false);
    const fromMid = m().melSpectrogram(
      melFilterbankSignal,
      MEL_SR,
      512,
      256,
      16,
      1000,
      8000,
      false,
    );
    const fromHigh = m().melSpectrogram(
      melFilterbankSignal,
      MEL_SR,
      512,
      256,
      16,
      2500,
      8000,
      false,
    );
    const total = (r: { power: Float32Array }) => sumAbs(r.power);
    expect(total(fromMid)).toBeLessThan(total(fromZero));
    expect(total(fromHigh)).toBeLessThan(total(fromMid));
  });
});

describe('mfcc consumes the fmin/fmax/lifter it accepts', () => {
  it('reads two fmax bounds as two different coefficient totals', () => {
    const narrow = m().mfcc(threePartials, SR, 512, 256, 16, 8, 0, 800, false, 0);
    const wide = m().mfcc(threePartials, SR, 512, 256, 16, 8, 0, 4000, false, 0);
    expect(sumAbs(narrow.coefficients)).not.toBeCloseTo(sumAbs(wide.coefficients), 2);
  });

  it('reads two fmin bounds as two different coefficient totals', () => {
    const fromZero = m().mfcc(threePartials, SR, 512, 256, 16, 8, 0, 4000, false, 0);
    const fromHigh = m().mfcc(threePartials, SR, 512, 256, 16, 8, 2000, 4000, false, 0);
    expect(sumAbs(fromZero.coefficients)).not.toBeCloseTo(sumAbs(fromHigh.coefficients), 2);
  });

  it('reads two lifter values as two different lift magnitudes', () => {
    // Both nonzero: mel_spectrogram.cpp only branches on `lifter > 0.0f`, so a
    // zero-vs-nonzero pair would show liftering is toggled without showing the
    // VALUE is read. Two positive lifters put a different sin() argument at
    // every coefficient, so the totals cannot coincide by construction.
    const short = m().mfcc(threePartials, SR, 512, 256, 16, 8, 0, 0, false, 10);
    const long = m().mfcc(threePartials, SR, 512, 256, 16, 8, 0, 0, false, 40);
    expect(sumAbs(short.coefficients)).not.toBeCloseTo(sumAbs(long.coefficients), 2);
  });
});

describe('mfccToMel and mfccToAudio consume the lifter they accept', () => {
  it('mfccToMel reads two lifter values as two different mel power totals', () => {
    const short = m().mfccToMel(mfccCoeffs(), N_MFCC, N_FRAMES(), 16, 10);
    const long = m().mfccToMel(mfccCoeffs(), N_MFCC, N_FRAMES(), 16, 40);
    expect(sumAbs(short.power)).not.toBeCloseTo(sumAbs(long.power), 2);
  });

  it('mfccToAudio reads two lifter values as two different reconstructions', () => {
    const short = m().mfccToAudio(
      mfccCoeffs(),
      N_MFCC,
      N_FRAMES(),
      16,
      SR,
      512,
      256,
      0,
      4000,
      4,
      false,
      10,
    );
    const long = m().mfccToAudio(
      mfccCoeffs(),
      N_MFCC,
      N_FRAMES(),
      16,
      SR,
      512,
      256,
      0,
      4000,
      4,
      false,
      40,
    );
    expect(sumAbs(short)).not.toBeCloseTo(sumAbs(long), 2);
  });
});

describe('pitchYin and pitchPyin consume the fmax they accept', () => {
  it('reads a fmax excluding vs including a 300 Hz tone as two different median f0', () => {
    const included = m().pitchYin(pureTone300, SR, 1024, 256, 65, 1000, 0.1, false);
    const excluded = m().pitchYin(pureTone300, SR, 1024, 256, 65, 200, 0.1, false);
    expect(Math.abs(included.medianF0 - 300)).toBeLessThan(30);
    expect(Math.abs(excluded.medianF0 - 300)).toBeGreaterThan(30);
  });

  it('pitchPyin reads the same bound the same way', () => {
    const included = m().pitchPyin(pureTone300, SR, 1024, 256, 65, 1000, 0.1, false);
    const excluded = m().pitchPyin(pureTone300, SR, 1024, 256, 65, 200, 0.1, false);
    expect(Math.abs(included.medianF0 - 300)).toBeLessThan(30);
    expect(Math.abs(excluded.medianF0 - 300)).toBeGreaterThan(30);
  });
});

describe('piptrack consumes the fmax/threshold it accepts', () => {
  it('reads a fmax excluding vs including the tone as two different magnitude totals', () => {
    const included = m().piptrack(pureTone300, SR, 512, 256, 65, 1000, 0.1);
    const excluded = m().piptrack(pureTone300, SR, 512, 256, 65, 200, 0.1);
    expect(sumAbs(included.magnitudes)).toBeGreaterThan(sumAbs(excluded.magnitudes));
  });

  it('reads a lenient vs strict threshold as two different magnitude totals', () => {
    // A dominant 300 Hz tone plus a much weaker 1200 Hz partial: threshold=0
    // keeps both local maxima, a stricter gate prunes the weak one.
    const twoPeaks = mixTones(
      [
        { freqHz: 300, amp: 1.0 },
        { freqHz: 1200, amp: 0.08 },
      ],
      SR,
      N,
    );
    const lenient = m().piptrack(twoPeaks, SR, 512, 256, 65, 4000, 0);
    const strict = m().piptrack(twoPeaks, SR, 512, 256, 65, 4000, 0.7);
    expect(sumAbs(lenient.magnitudes)).toBeGreaterThan(sumAbs(strict.magnitudes));
  });
});

// analyzeSections has no acceptance case here: minSectionSec has no read-back
// control on the inputs tried. Measured on 2s of 220 Hz followed by 2s of
// 880 Hz at 22050 Hz, every minSectionSec from 0.05 to 10 returned exactly one
// section spanning the whole clip (start 0, end 4) -- the boundary detector
// never found a boundary at all on that signal, so minSectionSec had nothing
// to act on and no comparison across its values can be observed. A
// silence/loud-tone signal was not re-tried against this specific claim; the
// refusal cases for minSectionSec (FLOAT_FIELDS, 'analyzeSections') still
// cover the guard itself and are kept.

describe('analyzeMelody consumes the fmax/threshold it accepts', () => {
  it('reads a fmax excluding vs including the tone as two different mean frequencies', () => {
    const included = m().analyzeMelody(pureTone300, SR, 65, 1000, 1024, 256, 0.1, false, true);
    const excluded = m().analyzeMelody(pureTone300, SR, 65, 200, 1024, 256, 0.1, false, true);
    expect(Math.abs(included.meanFrequency - 300)).toBeLessThan(30);
    expect(Math.abs(excluded.meanFrequency - 300)).toBeGreaterThan(30);
  });

  it('reads a lenient vs strict threshold as two different mean frequencies on an octave-ambiguous tone', () => {
    // A weak fundamental under a strong second harmonic is the classic YIN
    // octave-error setup: a lenient threshold accepts the harmonic's shallow
    // CMNDF dip first, a strict one holds out for the fundamental's deep one.
    const octaveAmbiguous = mixTones(
      [
        { freqHz: 220, amp: 0.3 },
        { freqHz: 440, amp: 1.0 },
      ],
      SR,
      N,
    );
    const lenient = m().analyzeMelody(octaveAmbiguous, SR, 65, 2000, 1024, 256, 0.9, false, true);
    const strict = m().analyzeMelody(octaveAmbiguous, SR, 65, 2000, 1024, 256, 0.05, false, true);
    expect(Math.abs(lenient.meanFrequency - strict.meanFrequency)).toBeGreaterThan(20);
  });
});

describe('pseudoCqt and hybridCqt consume the fmin they accept', () => {
  it('pseudoCqt reads a fmin band including vs excluding a 100 Hz tone as two different magnitude totals', () => {
    const tone100 = mixTones([{ freqHz: 100, amp: 0.8 }], SR, N);
    const inBand = m().pseudoCqt(tone100, SR, 256, 65.4, 12, 12); // band ~65-131 Hz
    const outOfBand = m().pseudoCqt(tone100, SR, 256, 800, 12, 12); // band ~800-1600 Hz
    expect(sumAbs(inBand.magnitude)).toBeGreaterThan(sumAbs(outOfBand.magnitude) * 3);
  });

  it('hybridCqt reads the same bound the same way', () => {
    const tone100 = mixTones([{ freqHz: 100, amp: 0.8 }], SR, N);
    const inBand = m().hybridCqt(tone100, SR, 256, 65.4, 12, 12);
    const outOfBand = m().hybridCqt(tone100, SR, 256, 800, 12, 12);
    expect(sumAbs(inBand.magnitude)).toBeGreaterThan(sumAbs(outOfBand.magnitude) * 3);
  });
});

// A dedicated signal for the blend-weight tests: a plain 220 Hz tone, 2s at
// 22050 Hz, matching the configuration the graded L1 distances below were
// measured against.
const nnlsBlendSignal = mixTones([{ freqHz: 220, amp: 0.8 }], 22050, 44100);

describe('nnlsChroma and nnlsChromaEx consume the stftBlendWeight they accept', () => {
  it('nnlsChroma reads three weights as three increasing L1 distances from the NNLS-only chroma', () => {
    // 0/0.5/1 rather than a 0-vs-1 binary: a two-point comparison cannot tell
    // "the weight is read as a blend fraction" from "the weight is compared
    // against some cutoff". The three ordered distances rule that out.
    const base = m().nnlsChroma(nnlsBlendSignal, 22050, true, 0, 512).data;
    const mid = m().nnlsChroma(nnlsBlendSignal, 22050, true, 0.5, 512).data;
    const full = m().nnlsChroma(nnlsBlendSignal, 22050, true, 1, 512).data;
    const toMid = l1Distance(base, mid);
    const toFull = l1Distance(base, full);
    expect(toMid).toBeGreaterThan(0);
    expect(toFull).toBeGreaterThan(toMid);
  });

  it('nnlsChroma ignores the weight when enableStftBlend is false', () => {
    // The blend branch in nnls_chroma.cpp only runs when enable_stft_blend is
    // true; with it false, stftBlendWeight has nothing left to select
    // between, so the two weights must produce bit-identical output.
    const w0 = m().nnlsChroma(nnlsBlendSignal, 22050, false, 0, 512).data;
    const w1 = m().nnlsChroma(nnlsBlendSignal, 22050, false, 1, 512).data;
    expect(l1Distance(w0, w1)).toBe(0);
  });

  it('nnlsChromaEx reads the same weight the same way', () => {
    const base = m().nnlsChromaEx(nnlsBlendSignal, 22050, true, 0, 512, 256).data;
    const mid = m().nnlsChromaEx(nnlsBlendSignal, 22050, true, 0.5, 512, 256).data;
    const full = m().nnlsChromaEx(nnlsBlendSignal, 22050, true, 1, 512, 256).data;
    const toMid = l1Distance(base, mid);
    const toFull = l1Distance(base, full);
    expect(toMid).toBeGreaterThan(0);
    expect(toFull).toBeGreaterThan(toMid);
  });
});
