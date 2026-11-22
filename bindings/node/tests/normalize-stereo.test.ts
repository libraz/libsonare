import { readFileSync } from 'node:fs';
import { describe, expect, it } from 'vitest';
import { normalize, normalizeStereo } from '../src/index.js';

const SR = 22050;
const LENGTH = 4096;

/**
 * A square wave, so the peak and the RMS are both exactly `amp` and every
 * figure below can be derived in closed form. A sampled sine's peak lands
 * wherever the grid puts it, which would leave every dB assertion carrying a
 * tolerance wide enough to hide the thing it measures.
 */
function square(period: number, amp: number): Float32Array {
  const out = new Float32Array(LENGTH);
  for (let i = 0; i < LENGTH; i += 1) {
    out[i] = i % period < period / 2 ? amp : -amp;
  }
  return out;
}

function peak(samples: Float32Array): number {
  let found = 0;
  for (const value of samples) {
    found = Math.max(found, Math.abs(value));
  }
  return found;
}

const db = (linear: number): number => 20 * Math.log10(linear);

// 12.04 dB apart, and different periods so a result that returned one channel
// twice fails on content rather than only on level.
const LOUD_AMP = 0.5;
const QUIET_AMP = 0.125;
const loud = () => square(4, LOUD_AMP);
const quiet = () => square(8, QUIET_AMP);

describe('normalizeStereo peak mode', () => {
  const TARGET_DB = -1;
  // The pair's peak is the louder channel's: -6.0206 dBFS, so -1 dBFS costs
  // +5.0206 dB and the quiet channel lands 12.0412 dB under the target.
  const EXPECTED_GAIN_DB = TARGET_DB - db(LOUD_AMP);
  const EXPECTED_LOUD_PEAK = LOUD_AMP * 10 ** (EXPECTED_GAIN_DB / 20);
  const EXPECTED_QUIET_PEAK = QUIET_AMP * 10 ** (EXPECTED_GAIN_DB / 20);

  it('drives the louder channel to the target and moves the other by the same gain', () => {
    const result = normalizeStereo({
      left: loud(),
      right: quiet(),
      sampleRate: SR,
      targetDb: TARGET_DB,
    });

    expect(result.left).toHaveLength(LENGTH);
    expect(result.right).toHaveLength(LENGTH);
    expect(result.appliedGainDb).toBeCloseTo(EXPECTED_GAIN_DB, 4);
    expect(db(peak(result.left))).toBeCloseTo(TARGET_DB, 4);
    expect(peak(result.right)).toBeCloseTo(EXPECTED_QUIET_PEAK, 5);
  });

  it('leaves the level difference between the channels where it was', () => {
    const result = normalizeStereo({
      left: loud(),
      right: quiet(),
      sampleRate: SR,
      targetDb: TARGET_DB,
    });

    const before = db(peak(quiet())) - db(peak(loud()));
    const after = db(peak(result.right)) - db(peak(result.left));
    expect(before).toBeCloseTo(-12.0412, 4);
    expect(after).toBeCloseTo(before, 4);
  });

  it('puts the quiet channel somewhere a per-channel gain never would', () => {
    // The control: normalizing that same channel ALONE is what a per-channel
    // gain does, and it is what this entry must not do. Without it "the quiet
    // channel is at -13.04 dBFS" is a number with nothing to fail against.
    const alone = normalize({ samples: quiet(), sampleRate: SR, targetDb: TARGET_DB });
    expect(db(peak(alone))).toBeCloseTo(TARGET_DB, 4);

    const result = normalizeStereo({
      left: loud(),
      right: quiet(),
      sampleRate: SR,
      targetDb: TARGET_DB,
    });
    expect(peak(result.right)).toBeCloseTo(EXPECTED_QUIET_PEAK, 5);
    // 0.2228 against 0.8913: a factor of four apart, not a tolerance apart.
    expect(peak(result.right)).toBeLessThan(peak(alone) / 3);
  });

  it('is what the left channel gets too, so the fixture proves both halves', () => {
    const result = normalizeStereo({
      left: loud(),
      right: quiet(),
      sampleRate: SR,
      targetDb: TARGET_DB,
    });
    expect(peak(result.left)).toBeCloseTo(EXPECTED_LOUD_PEAK, 5);
  });
});

describe('normalizeStereo rms mode', () => {
  const TARGET_DB = -20;
  // Every sample sits at +/-amp, so each channel's RMS is its amplitude. The
  // pair's RMS is the quadratic mean of the two, 0.364434 (-8.7740 dBFS);
  // their arithmetic mean would be 0.3125 (-10.1030 dBFS), 1.33 dB away.
  const JOINT_RMS = Math.sqrt((LOUD_AMP ** 2 + QUIET_AMP ** 2) / 2);
  const MEAN_RMS = (LOUD_AMP + QUIET_AMP) / 2;
  const EXPECTED_GAIN_DB = TARGET_DB - db(JOINT_RMS);
  const ARITHMETIC_MEAN_GAIN_DB = TARGET_DB - db(MEAN_RMS);

  it('measures the root mean square over both channels together', () => {
    const result = normalizeStereo({
      left: loud(),
      right: quiet(),
      sampleRate: SR,
      targetDb: TARGET_DB,
      mode: 'rms',
    });

    expect(result.appliedGainDb).toBeCloseTo(EXPECTED_GAIN_DB, 3);
    // The discriminator: the two derivations are 1.33 dB apart, so a gain that
    // matched the quadratic mean by coincidence would have to miss by that.
    expect(Math.abs(result.appliedGainDb - ARITHMETIC_MEAN_GAIN_DB)).toBeGreaterThan(1);
  });

  it('keeps the balance under the shared gain', () => {
    const result = normalizeStereo({
      left: loud(),
      right: quiet(),
      sampleRate: SR,
      targetDb: TARGET_DB,
      mode: 'rms',
    });
    expect(db(peak(result.right)) - db(peak(result.left))).toBeCloseTo(-12.0412, 4);
  });
});

describe('normalize and normalizeStereo silence', () => {
  // Silence has no level to scale toward, so no gain is a guess; both refuse it
  // naming the signal the caller passed, in that caller's own spelling.
  const silence = new Float32Array(LENGTH);

  it.each(['peak', 'rms'] as const)('refuses a silent take in %s mode', (mode) => {
    const call = () => normalize({ samples: silence, sampleRate: SR, mode });
    expect(call).toThrow(RangeError);
    expect(call).toThrow(/^normalize: samples has no measurable level$/);
  });

  it('refuses a silent pair', () => {
    const call = () => normalizeStereo({ left: silence, right: silence, sampleRate: SR });
    expect(call).toThrow(RangeError);
    expect(call).toThrow(/^normalizeStereo: audio has no measurable level$/);
  });

  it('accepts a quiet but measurable take, so the refusal is about silence', () => {
    const quiet = square(64, 1e-4);
    expect(normalize({ samples: quiet, sampleRate: SR })).toHaveLength(LENGTH);
    expect(
      normalizeStereo({ left: quiet, right: silence, sampleRate: SR }).appliedGainDb,
    ).toBeGreaterThan(70);
  });
});

describe('normalizeStereo refusals', () => {
  const invalidParameter = (call: () => unknown): Error => {
    let caught: unknown;
    try {
      call();
    } catch (error) {
      caught = error;
    }
    expect(caught).toBeInstanceOf(RangeError);
    // Every refusal names the entry once, the core's as well as the facade's.
    expect((caught as Error).message).toMatch(/^normalizeStereo: (?!normalizeStereo: )/);
    return caught as Error;
  };

  it('accepts the pair every refusal below is a mutation of', () => {
    // The positive control. Each refusal changes exactly one thing about this
    // call, so a refusal that fired for an unrelated reason shows up here.
    const result = normalizeStereo({ left: loud(), right: quiet(), sampleRate: SR });
    expect(result.left).toHaveLength(LENGTH);
  });

  it('refuses channels of unequal length', () => {
    const error = invalidParameter(() =>
      normalizeStereo({ left: loud(), right: quiet().subarray(0, LENGTH - 1), sampleRate: SR }),
    );
    expect(error.message).toMatch(/same length/);
  });

  // Empty audio is the facade's own RangeError, not a code the core sent back.
  it('refuses an empty left channel', () => {
    expect(() =>
      normalizeStereo({ left: new Float32Array(0), right: new Float32Array(0), sampleRate: SR }),
    ).toThrow(RangeError);
  });

  it('refuses an empty right channel beside a channel that has signal', () => {
    expect(() =>
      normalizeStereo({ left: loud(), right: new Float32Array(0), sampleRate: SR }),
    ).toThrow(RangeError);
  });

  it('refuses a target above full scale', () => {
    const error = invalidParameter(() =>
      normalizeStereo({ left: loud(), right: quiet(), sampleRate: SR, targetDb: 1 }),
    );
    expect(error.message).toBe('normalizeStereo: targetDb must be at most 0 when clip is set');
  });

  it('refuses a non-finite target', () => {
    invalidParameter(() =>
      normalizeStereo({
        left: loud(),
        right: quiet(),
        sampleRate: SR,
        targetDb: Number.NaN,
      }),
    );
  });

  it('refuses a sample rate outside the supported range, naming this entry', () => {
    expect(() => normalizeStereo({ left: loud(), right: quiet(), sampleRate: 1 })).toThrow(
      /normalizeStereo: sampleRate/,
    );
  });

  it('refuses a mode it does not know', () => {
    expect(() =>
      normalizeStereo({
        left: loud(),
        right: quiet(),
        sampleRate: SR,
        mode: 'loudness' as 'peak',
      }),
    ).toThrow(RangeError);
    expect(() =>
      normalizeStereo({
        left: loud(),
        right: quiet(),
        sampleRate: SR,
        mode: 'loudness' as 'peak',
      }),
    ).toThrow(/^normalizeStereo: mode must be one of peak, rms$/);
    expect(() =>
      normalizeStereo({
        left: loud(),
        right: quiet(),
        sampleRate: SR,
        mode: 7 as unknown as 'peak',
      }),
    ).toThrow(TypeError);
  });
});

describe('normalizeStereo default target', () => {
  it('defaults rms mode to -20 dB rather than to the peak default of 0', () => {
    const omitted = normalizeStereo({ left: loud(), right: quiet(), sampleRate: SR, mode: 'rms' });
    const atMinus20 = normalizeStereo({
      left: loud(),
      right: quiet(),
      sampleRate: SR,
      mode: 'rms',
      targetDb: -20,
    });
    expect(omitted.appliedGainDb).toBeCloseTo(atMinus20.appliedGainDb, 6);

    // Control: 0 is the other candidate — it is what the mono `normalize` on
    // this surface defaults to in both modes — and it is 20 dB away, so this
    // assertion distinguishes the two rather than observing that some gain was
    // applied.
    const atZero = normalizeStereo({
      left: loud(),
      right: quiet(),
      sampleRate: SR,
      mode: 'rms',
      targetDb: 0,
    });
    expect(atZero.appliedGainDb - omitted.appliedGainDb).toBeCloseTo(20, 6);
  });

  it('still defaults peak mode to 0 dB', () => {
    const omitted = normalizeStereo({ left: loud(), right: quiet(), sampleRate: SR });
    const atZero = normalizeStereo({
      left: loud(),
      right: quiet(),
      sampleRate: SR,
      targetDb: 0,
    });
    expect(omitted.appliedGainDb).toBeCloseTo(atZero.appliedGainDb, 6);
  });
});

// The per-mode default target comes from the shared corpus every surface reads, so a
// default moved on one surface alone fails here.
describe('normalize default target per mode', () => {
  const defaults = (
    JSON.parse(
      readFileSync(
        new URL('../../../tests/conformance/binding_defaults.json', import.meta.url),
        'utf8',
      ),
    ) as { defaults: Record<string, number> }
  ).defaults;
  const rate = 22050;
  const tone = Float32Array.from(
    { length: 4096 },
    (_, i) => 0.3 * Math.sin((2 * Math.PI * 440 * i) / rate),
  );
  const other = Float32Array.from(
    { length: 4096 },
    (_, i) => 0.2 * Math.sin((2 * Math.PI * 660 * i) / rate),
  );

  it.each(['peak', 'rms'] as const)(
    'omitting targetDb in %s mode renders the corpus default',
    (mode) => {
      const targetDb = defaults[`normalize.target_db.${mode}`];
      expect(targetDb).toBeTypeOf('number');
      expect(normalize({ samples: tone, sampleRate: rate, mode })).toEqual(
        normalize({ samples: tone, sampleRate: rate, mode, targetDb }),
      );
      expect(normalizeStereo({ left: tone, right: other, sampleRate: rate, mode })).toEqual(
        normalizeStereo({ left: tone, right: other, sampleRate: rate, mode, targetDb }),
      );
    },
  );
});
