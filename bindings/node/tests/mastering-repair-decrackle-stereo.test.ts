import { describe, expect, it } from 'vitest';
import { masteringRepairDecrackleStereo } from '../src/index.js';

const SR = 22050;

// Isolated spikes, spaced far enough apart that no two spikes ever share a
// 3-sample median window: each one contributes exactly one detected sample,
// so detected.sampleCount is an exact count rather than a loose inequality.
const LEFT_SPIKE_POSITIONS = [3000, 6000, 9000];
const RIGHT_SPIKE_POSITIONS = [4000, 8000];

function sine(freq: number, seconds: number, amp: number): Float32Array {
  const n = Math.floor(SR * seconds);
  const out = new Float32Array(n);
  for (let i = 0; i < n; i += 1) {
    out[i] = amp * Math.sin((2 * Math.PI * freq * i) / SR);
  }
  return out;
}

// Different tones and different spike counts in the two channels, so
// "decrackle one channel and return it twice" fails on content alone. Base
// amplitude 0.1 stays well clear of the default 0.4 median-deviation
// threshold, so nothing but the spikes triggers detection.
function fixture(): { left: Float32Array; right: Float32Array } {
  const left = sine(440, 0.6, 0.1);
  const right = sine(660, 0.6, 0.1);
  for (const i of LEFT_SPIKE_POSITIONS) {
    left[i] = 0.95;
  }
  for (const i of RIGHT_SPIKE_POSITIONS) {
    right[i] = -0.95;
  }
  return { left, right };
}

describe('masteringRepairDecrackleStereo', () => {
  it('decrackles each channel on its own with median mode', () => {
    const { left, right } = fixture();
    const result = masteringRepairDecrackleStereo({ left, right, sampleRate: SR });

    expect(result.left).toHaveLength(left.length);
    expect(result.right).toHaveLength(right.length);
    expect(result.left.every(Number.isFinite)).toBe(true);
    expect(result.right.every(Number.isFinite)).toBe(true);

    // Exact counts: a fixture that stops witnessing its spikes must go red,
    // not merely satisfy a loose ">0".
    expect(result.leftReport.detected.sampleCount).toBe(LEFT_SPIKE_POSITIONS.length);
    expect(result.rightReport.detected.sampleCount).toBe(RIGHT_SPIKE_POSITIONS.length);

    // Median mode: the filter overwrote exactly the samples it detected.
    expect(result.leftReport.replacedSamples).toBe(result.leftReport.detected.sampleCount);
    expect(result.rightReport.replacedSamples).toBe(result.rightReport.detected.sampleCount);

    // Wavelet-only fields stay unfilled in median mode.
    expect(result.leftReport.detailCoefficients).toBe(0);
    expect(result.leftReport.shrunkCoefficients).toBe(0);
    expect(result.leftReport.noiseSigma).toBe(0);

    // Not one channel restated: no run is ever widened to match the other
    // side, so there is no linking to witness here, only independent content.
    expect(Array.from(result.left)).not.toEqual(Array.from(result.right));
    for (const i of LEFT_SPIKE_POSITIONS) {
      expect(result.left[i]).not.toBe(0.95);
    }
  });

  it('reports through the wavelet-only fields in waveletShrinkage mode', () => {
    const { left, right } = fixture();
    const result = masteringRepairDecrackleStereo({
      left,
      right,
      sampleRate: SR,
      mode: 'waveletShrinkage',
    });

    expect(result.left).toHaveLength(left.length);
    expect(result.right).toHaveLength(right.length);

    // `detected` is always the median criterion, whatever mode ran: still
    // exact, and nonzero even though the median filter itself never runs.
    expect(result.leftReport.detected.sampleCount).toBe(LEFT_SPIKE_POSITIONS.length);
    expect(result.rightReport.detected.sampleCount).toBe(RIGHT_SPIKE_POSITIONS.length);

    // Median-only field stays unfilled in wavelet mode.
    expect(result.leftReport.replacedSamples).toBe(0);
    expect(result.rightReport.replacedSamples).toBe(0);

    // Wavelet-only fields are the ones actually witnessed by this mode.
    expect(result.leftReport.detailCoefficients).toBeGreaterThan(0);
    expect(result.leftReport.shrunkCoefficients).toBeGreaterThan(0);
    expect(result.leftReport.noiseSigma).toBeGreaterThan(0);
    expect(result.rightReport.detailCoefficients).toBeGreaterThan(0);
    expect(result.rightReport.shrunkCoefficients).toBeGreaterThan(0);
    expect(result.rightReport.noiseSigma).toBeGreaterThan(0);
  });

  it('carries threshold through to the median detector', () => {
    const { left, right } = fixture();

    // Two separate claims: threshold is range-checked to be finite and
    // positive, so an invalid value proves only that the wrapper passed it
    // along to the validator, not to the detector.
    expect(() =>
      masteringRepairDecrackleStereo({ left, right, sampleRate: SR, threshold: 0 }),
    ).toThrow();

    // A lower threshold makes more of the sine's own variation read as
    // crackle, moving the detected count past the spike-only baseline. That
    // is the detector consuming the value, not just the validator.
    const strict = masteringRepairDecrackleStereo({ left, right, sampleRate: SR });
    const loose = masteringRepairDecrackleStereo({
      left,
      right,
      sampleRate: SR,
      threshold: 0.01,
    });
    expect(loose.leftReport.detected.sampleCount).toBeGreaterThan(
      strict.leftReport.detected.sampleCount,
    );
  });

  it('carries threshold through to the wavelet shrinkage as a cap on BayesShrink', () => {
    const { left, right } = fixture();

    // In wavelet mode threshold caps bayes_shrink_threshold's own computed
    // value (min(configured, noise_variance / signal_sigma)); the library
    // default of 0.4 measured well above that computed value on this
    // fixture and never binds, so shrunkCoefficients does not move until the
    // configured value drops below it. 0.001 measured below it, moving
    // shrunkCoefficients from 3647 to 873 (left) and 7082 to 598 (right).
    const unbound = masteringRepairDecrackleStereo({
      left,
      right,
      sampleRate: SR,
      mode: 'waveletShrinkage',
    });
    const bound = masteringRepairDecrackleStereo({
      left,
      right,
      sampleRate: SR,
      mode: 'waveletShrinkage',
      threshold: 0.001,
    });
    expect(bound.leftReport.shrunkCoefficients).toBeLessThan(unbound.leftReport.shrunkCoefficients);
    expect(bound.rightReport.shrunkCoefficients).toBeLessThan(
      unbound.rightReport.shrunkCoefficients,
    );
  });

  it('refuses bad arguments with a catchable error', () => {
    const { left, right } = fixture();
    expect(() =>
      masteringRepairDecrackleStereo({ left, right: right.slice(0, 10), sampleRate: SR }),
    ).toThrow();
    expect(() =>
      masteringRepairDecrackleStereo({
        left: undefined as unknown as Float32Array,
        right,
        sampleRate: SR,
      }),
    ).toThrow();
    expect(() =>
      masteringRepairDecrackleStereo({
        left,
        right: [1, 2, 3] as unknown as Float32Array,
        sampleRate: SR,
      }),
    ).toThrow();
    expect(() => masteringRepairDecrackleStereo({ left, right, sampleRate: Number.NaN })).toThrow();
  });
});
