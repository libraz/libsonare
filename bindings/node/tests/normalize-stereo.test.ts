import { describe, expect, it } from 'vitest';
import { ErrorCode, normalize, normalizeStereo } from '../src/index.js';

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

describe('normalizeStereo silence', () => {
  it('leaves a silent pair untouched and reports no gain', () => {
    const result = normalizeStereo({
      left: new Float32Array(LENGTH),
      right: new Float32Array(LENGTH),
      sampleRate: SR,
    });

    expect(result.appliedGainDb).toBe(0);
    expect(peak(result.left)).toBe(0);
    expect(peak(result.right)).toBe(0);
    expect(result.left).toHaveLength(LENGTH);
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
    expect(caught).toBeInstanceOf(Error);
    expect((caught as { code?: number }).code).toBe(ErrorCode.InvalidParameter);
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

  it('refuses an empty left channel', () => {
    invalidParameter(() =>
      normalizeStereo({ left: new Float32Array(0), right: new Float32Array(0), sampleRate: SR }),
    );
  });

  it('refuses an empty right channel beside a channel that has signal', () => {
    invalidParameter(() =>
      normalizeStereo({ left: loud(), right: new Float32Array(0), sampleRate: SR }),
    );
  });

  it('refuses a target above full scale', () => {
    const error = invalidParameter(() =>
      normalizeStereo({ left: loud(), right: quiet(), sampleRate: SR, targetDb: 1 }),
    );
    expect(error.message).toMatch(/target_db/);
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
