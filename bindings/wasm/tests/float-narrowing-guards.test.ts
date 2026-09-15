/**
 * What a JS number is allowed to become on its way into a `float` facade field.
 *
 * `val::as<float>()` SATURATES: every finite number wider than `FLT_MAX`
 * (~3.4028235e38) arrives as an infinity, and several of these fields document a
 * non-finite value as unspecified — so `1e40`, `1e300` and a deliberate
 * `Infinity` all land on one accepted result and nothing downstream can tell
 * them apart. The reader that refuses this already existed
 * (`checkedFloatFromVal`); what these cases pin is the population that reaches
 * it.
 *
 * The three parameters here are the value's shape, the entry point, and which
 * field of that entry point carries it. The third is NESTED inside the second —
 * `thresholdDb` exists only on the compressor and the gate, `zi` only on the
 * emphasis pair — so (entry point, field) is a fixed list rather than a cross
 * product, the model has two free factors, and pairwise over two factors IS the
 * full cross product. Every field therefore meets every refused shape.
 *
 * Acceptance is asserted by reading the value back, never by "it did not throw".
 * `zi` is where that can be exact: `preemphasis`/`deemphasis` both compute
 * `y[0] = x[0] + zi`, so with `x[0] = 0` the first output sample IS the narrowed
 * value. That is what separates this defect from ordinary float32 rounding —
 * `0.1` must still be accepted and must come back as `Math.fround(0.1)`.
 *
 * Each entry point also carries a positive control: two legitimate values
 * producing two observably different results. Without it a facade that ignored
 * its argument would satisfy every refusal above in exactly the same way.
 */

import { beforeAll, describe, expect, it } from 'vitest';
import {
  type CompressorOptions,
  deemphasis,
  ErrorCode,
  type GateOptions,
  init,
  isSonareError,
  masteringDynamicsCompressor,
  masteringDynamicsGate,
  masteringDynamicsTransientShaper,
  preemphasis,
  type SonareError,
  type SpectralRegionOp,
  spectralEdit,
  type TransientShaperOptions,
} from '../dist/index.js';

const SR = 44100;

/** The suffix every one of these refusals ends with, whatever the key is. */
const RANGE_MESSAGE = 'must be a finite number within the 32-bit float range';

/**
 * Values that saturate onto a legal float. These are the ones that matter: each
 * is a number the caller chose, survives every JS-side check, and used to arrive
 * as an infinity a config guard written `x < 0` waves through.
 */
const SATURATES_ONTO_A_FLOAT = [1e40, -1e40, 3.5e38, 1e300, -1e300];

/** Non-finite values written directly, which must reach the same refusal. */
const NON_FINITE = [Number.NaN, Number.POSITIVE_INFINITY, Number.NEGATIVE_INFINITY];

const REFUSED = [...SATURATES_ONTO_A_FLOAT, ...NON_FINITE];

/** A steady tone, long enough for every entry point's default analysis frame. */
const tone = new Float32Array(4096).map((_, i) => 0.5 * Math.sin((2 * Math.PI * 440 * i) / SR));

/** Silence then a tone, so a timing field has an onset to act on. */
const ONSET = 8192;
const burst = new Float32Array(16384).map((_, i) =>
  i < ONSET ? 0 : 0.9 * Math.sin((2 * Math.PI * 440 * i) / SR),
);

/** First sample zero, so `y[0] = x[0] + zi` reads back the narrowed zi exactly. */
const emphasisInput = new Float32Array([0, 0.5, -0.25, 0.125]);

function peakAbs(values: Float32Array, start = 0, end = values.length): number {
  let peak = 0;
  for (let i = start; i < end; i++) {
    const magnitude = Math.abs(values[i] ?? 0);
    if (magnitude > peak) {
      peak = magnitude;
    }
  }
  return peak;
}

function rms(values: Float32Array, start = 0, end = values.length): number {
  let sum = 0;
  for (let i = start; i < end; i++) {
    sum += (values[i] ?? 0) ** 2;
  }
  return Math.sqrt(sum / (end - start));
}

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

const compressorWith = (options: CompressorOptions, samples = tone): Float32Array =>
  masteringDynamicsCompressor(samples, SR, options).samples;

const gateWith = (options: GateOptions, samples = tone): Float32Array =>
  masteringDynamicsGate(samples, SR, options).samples;

const shaperWith = (options: TransientShaperOptions, samples = tone): Float32Array =>
  masteringDynamicsTransientShaper(samples, SR, options).samples;

const spectralEditWith = (op: SpectralRegionOp, samples = tone): Float32Array =>
  spectralEdit(samples, SR, [op]);

/** One float field of one entry point, and how to drive it with a value. */
interface FloatField {
  entry: string;
  key: string;
  run: (value: number) => unknown;
}

const COMPRESSOR_KEYS = [
  'thresholdDb',
  'ratio',
  'attackMs',
  'releaseMs',
  'kneeDb',
  'makeupGainDb',
  'sidechainHpfHz',
  'pdrTimeMs',
  'pdrReleaseScale',
];

const GATE_KEYS = [
  'thresholdDb',
  'attackMs',
  'releaseMs',
  'rangeDb',
  'holdMs',
  'closeThresholdDb',
  'keyHpfHz',
];

const SHAPER_KEYS = [
  'attackGainDb',
  'sustainGainDb',
  'fastAttackMs',
  'fastReleaseMs',
  'slowAttackMs',
  'slowReleaseMs',
  'sensitivity',
  'maxGainDb',
  'gainSmoothingMs',
  'lookaheadMs',
];

const SPECTRAL_KEYS = ['lowHz', 'highHz', 'gainDb'];

const FLOAT_FIELDS: FloatField[] = [
  ...COMPRESSOR_KEYS.map((key) => ({
    entry: 'masteringDynamicsCompressor',
    key,
    run: (value: number) => compressorWith({ [key]: value } as unknown as CompressorOptions),
  })),
  ...GATE_KEYS.map((key) => ({
    entry: 'masteringDynamicsGate',
    key,
    run: (value: number) => gateWith({ [key]: value } as unknown as GateOptions),
  })),
  ...SHAPER_KEYS.map((key) => ({
    entry: 'masteringDynamicsTransientShaper',
    key,
    run: (value: number) => shaperWith({ [key]: value } as unknown as TransientShaperOptions),
  })),
  ...SPECTRAL_KEYS.map((key) => ({
    entry: 'spectralEdit',
    key,
    run: (value: number) => spectralEditWith({ [key]: value } as unknown as SpectralRegionOp),
  })),
  {
    entry: 'preemphasis',
    key: 'zi',
    run: (value: number) => preemphasis(emphasisInput, 0.97, value),
  },
  {
    entry: 'deemphasis',
    key: 'zi',
    run: (value: number) => deemphasis(emphasisInput, 0.97, value),
  },
];

/** The emphasis pair, typed so both can be driven by one body. */
const EMPHASIS: [string, (x: Float32Array, coef: number, zi: number) => Float32Array][] = [
  ['preemphasis', (x, coef, zi) => preemphasis(x, coef, zi)],
  ['deemphasis', (x, coef, zi) => deemphasis(x, coef, zi)],
];

beforeAll(async () => {
  await init();
});

describe('every float field refuses a value the float type cannot hold', () => {
  it('covers one field per float read these facades make', () => {
    // A field dropped from the table stops being covered without anything going
    // red, which is the one way this file could quietly shrink.
    expect(FLOAT_FIELDS).toHaveLength(31);
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

describe('preemphasis and deemphasis land the initial state they were given', () => {
  // The exact case: with x[0] = 0 both filters compute y[0] = zi, so the first
  // output sample IS the narrowed value rather than a proxy for it.
  it.each(EMPHASIS)('%s returns the float32 of the value it accepted', (_name, filter) => {
    for (const value of [0, 1, -0.25, 3.0e38]) {
      expect(filter(emphasisInput, 0.97, value)[0]).toBe(Math.fround(value));
    }
    // Precision loss is not this defect: 0.1 is accepted and comes back rounded
    // to float32, not refused and not turned into something else.
    expect(filter(emphasisInput, 0.97, 0.1)[0]).toBe(Math.fround(0.1));
    expect(filter(emphasisInput, 0.97, 0.1)[0]).not.toBe(0.1);
  });

  it.each(EMPHASIS)('%s reads two legitimate initial states differently', (_name, filter) => {
    // The positive control. Without it every refusal above would also hold for a
    // facade that ignored zi entirely.
    expect(filter(emphasisInput, 0.97, 0)[0]).toBe(0);
    expect(filter(emphasisInput, 0.97, 1)[0]).toBe(1);
  });
});

describe('the dynamics processors consume the float values they accept', () => {
  it('compressor: two legitimate attack times reach two different envelopes', () => {
    const shared = { thresholdDb: -30, ratio: 10, releaseMs: 10, makeupGainDb: 0 };
    const fast = compressorWith({ ...shared, attackMs: 0.1 }, burst);
    const slow = compressorWith({ ...shared, attackMs: 50 }, burst);
    expect(Array.from(fast)).not.toEqual(Array.from(slow));
    // A slower attack lets more of the onset through, which is what says the
    // 0.1 ms request landed rather than merely being tolerated.
    expect(peakAbs(slow, ONSET, ONSET + 512)).toBeGreaterThan(peakAbs(fast, ONSET, ONSET + 512));
  });

  it('compressor: a threshold near FLT_MAX is accepted and stops the compression', () => {
    const wide = compressorWith({ thresholdDb: 3.0e38, ratio: 10, makeupGainDb: 0 }, burst);
    const low = compressorWith({ thresholdDb: -40, ratio: 10, makeupGainDb: 0 }, burst);
    expect(Array.from(wide)).not.toEqual(Array.from(low));
    expect(peakAbs(wide)).toBeGreaterThan(peakAbs(low));
  });

  it('gate: two legitimate thresholds open and shut the same signal', () => {
    const open = gateWith({ thresholdDb: -60, closeThresholdDb: -60, rangeDb: -80 }, burst);
    const shut = gateWith({ thresholdDb: 0, closeThresholdDb: 0, rangeDb: -80 }, burst);
    expect(Array.from(open)).not.toEqual(Array.from(shut));
    expect(peakAbs(open)).toBeGreaterThan(peakAbs(shut));
  });

  it('transient shaper: two legitimate attack gains reach two different onsets', () => {
    const flat = shaperWith({ attackGainDb: 0, sustainGainDb: 0 }, burst);
    const punched = shaperWith({ attackGainDb: 12, sustainGainDb: 0 }, burst);
    expect(Array.from(flat)).not.toEqual(Array.from(punched));
    expect(peakAbs(punched, ONSET, ONSET + 2048)).toBeGreaterThan(
      peakAbs(flat, ONSET, ONSET + 2048),
    );
  });
});

describe('spectralEdit consumes the region floats it accepts', () => {
  it('reads two legitimate region gains as two different residuals', () => {
    const identity = spectralEditWith({ gainDb: 0 });
    const cut = spectralEditWith({ gainDb: -60 });
    // An omitted endSample covers the whole signal, but the region maps to
    // frames through the last *included sample*, so the final analysis window
    // is outside it and the closing n_fft/2 samples keep some of their level.
    // Measure where the edit applies; EDGE is that window, not a fitted margin.
    const EDGE = 1024;
    const span = tone.length - EDGE;
    expect(rms(cut, 0, span)).toBeLessThan(rms(identity, 0, span) * 0.01);
  });

  it('accepts a gain that needs float32 rounding and applies it', () => {
    // 0.1 dB is a real request, not a rounding artefact: it must raise the
    // residual rather than be refused alongside the overflowing values.
    expect(rms(spectralEditWith({ gainDb: 0.1 }))).toBeGreaterThan(
      rms(spectralEditWith({ gainDb: 0 })),
    );
  });

  it('reads two legitimate band edges as two different residuals', () => {
    const lowBand = spectralEditWith({ lowHz: 0, highHz: 200, gainDb: -60 });
    const overTone = spectralEditWith({ lowHz: 300, highHz: 600, gainDb: -60 });
    expect(rms(lowBand)).toBeGreaterThan(rms(overTone));
  });
});

describe('the refusal is the same one the shared readers already raised', () => {
  it('answers a bag field and a positional field by the same code and wording', () => {
    const fromBag = capture(() => compressorWith({ thresholdDb: 1e300 }));
    const fromPositional = capture(() => preemphasis(emphasisInput, 0.97, 1e300));
    expectRangeRefusal(fromBag, 'thresholdDb', 'compressor thresholdDb');
    expectRangeRefusal(fromPositional, 'zi', 'preemphasis zi');
    expect((fromBag as SonareError).code).toBe((fromPositional as SonareError).code);
    expect((fromBag as SonareError).message.replace('thresholdDb', '<key>')).toBe(
      (fromPositional as SonareError).message.replace('zi', '<key>'),
    );
  });
});
