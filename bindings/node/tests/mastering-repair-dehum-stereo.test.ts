import { describe, expect, it } from 'vitest';
import { masteringRepairDehum, masteringRepairDehumStereo } from '../src/index.js';

const SR = 22050;

function sine(freq: number, seconds: number, amp: number): Float32Array {
  const n = Math.floor(SR * seconds);
  const out = new Float32Array(n);
  for (let i = 0; i < n; i += 1) {
    out[i] = amp * Math.sin((2 * Math.PI * freq * i) / SR);
  }
  return out;
}

function add(a: Float32Array, b: Float32Array): Float32Array {
  const out = new Float32Array(a.length);
  for (let i = 0; i < a.length; i += 1) {
    out[i] = a[i] + b[i];
  }
  return out;
}

// Both channels carry the SAME 50 Hz hum (so a single shared config notches
// both correctly) but different content tones, far past the 16-harmonic
// series (16 * 50 = 800 Hz), so the tones survive the notch cascade and keep
// the two channels distinguishable after repair.
function fixedFixture(): { left: Float32Array; right: Float32Array } {
  const hum = sine(50, 0.6, 0.3);
  const left = add(hum, sine(1000, 0.6, 0.2));
  const right = add(hum, sine(1500, 0.6, 0.2));
  return { left, right };
}

describe('masteringRepairDehumStereo', () => {
  it('filters each channel independently at the configured frequency (default, non-adaptive)', () => {
    const { left, right } = fixedFixture();
    const result = masteringRepairDehumStereo({ left, right, sampleRate: SR });

    expect(result.left).toHaveLength(left.length);
    expect(result.right).toHaveLength(right.length);
    expect(result.left.every(Number.isFinite)).toBe(true);
    expect(result.right.every(Number.isFinite)).toBe(true);

    // The default search window [48, 52] Hz has 50.0 exactly on its grid, so
    // the search locks onto the actual hum frequency exactly, not merely
    // near it.
    expect(result.leftReport.detected.fundamentalHz).toBe(50);
    expect(result.rightReport.detected.fundamentalHz).toBe(50);

    // Only the fundamental clears the harmonic-detection margin; the higher
    // k*f0 slots (100, 150, ... Hz) carry nothing but numerical floor.
    expect(result.leftReport.detected.harmonics).toBe(1);
    expect(result.rightReport.detected.harmonics).toBe(1);

    // Fixed mode always applies the configured frequency verbatim, and never
    // tracks, so drift is the measurement of "none", not an unset field.
    expect(result.leftReport.appliedFundamentalHz).toBe(50);
    expect(result.rightReport.appliedFundamentalHz).toBe(50);
    expect(result.leftReport.fundamentalDriftHz).toBe(0);
    expect(result.rightReport.fundamentalDriftHz).toBe(0);

    // Default harmonics = 4, all of them under Nyquist.
    expect(result.leftReport.notchedHarmonics).toBe(4);
    expect(result.rightReport.notchedHarmonics).toBe(4);

    // Every one of the 16 measured slots is reported, and the fundamental
    // reads far above an unexcited harmonic slot.
    expect(result.leftReport.detected.harmonicDbfs).toHaveLength(16);
    expect(result.rightReport.detected.harmonicDbfs).toHaveLength(16);
    expect(result.leftReport.detected.harmonicDbfs[0]).toBeGreaterThan(
      result.leftReport.detected.harmonicDbfs[5],
    );

    // The two channels' own content tones (1000 Hz / 1500 Hz) sit well past
    // the notch cascade, so they survive and keep the outputs distinguishable.
    expect(Array.from(result.left)).not.toEqual(Array.from(result.right));
  });

  it('is bit-identical to two independent mono calls when adaptive is off', () => {
    // Nothing is shared between channels without adaptive tracking, so the
    // stereo entry point is two independent mono passes: a cheap equivalence
    // control that would fail if any state leaked across channels. Half of
    // the sharing rule; see the adaptive test below for the other half.
    const { left, right } = fixedFixture();
    const stereo = masteringRepairDehumStereo({ left, right, sampleRate: SR });
    const monoLeft = masteringRepairDehum(left, SR);
    const monoRight = masteringRepairDehum(right, SR);

    expect(Array.from(stereo.left)).toEqual(Array.from(monoLeft));
    expect(Array.from(stereo.right)).toEqual(Array.from(monoRight));
  });

  it('shares the tracked fundamental across channels when adaptive, while detection stays per-channel', () => {
    // Different hum per channel is what makes shared tracking observable:
    // each channel's own `detected` still reflects its own content, but the
    // filtering frequency is one shared value derived from both. The other
    // half of the sharing rule -- nothing shared without adaptive tracking --
    // is the mono-equivalence test above.
    const left = add(sine(50, 0.6, 0.3), sine(2500, 0.6, 0.2));
    const right = add(sine(80, 0.6, 0.3), sine(3500, 0.6, 0.2));

    // The anchor (65 Hz) sits between the two real hum frequencies and
    // matches neither, so the tracker is forced to move off it -- a broken
    // "just echo the config" implementation would report zero drift here.
    const result = masteringRepairDehumStereo({
      left,
      right,
      sampleRate: SR,
      adaptive: true,
      fundamentalHz: 65,
      searchRangeHz: 35,
      harmonics: 1,
    });

    expect(result.left).toHaveLength(left.length);
    expect(result.right).toHaveLength(right.length);
    expect(result.left.every(Number.isFinite)).toBe(true);
    expect(result.right.every(Number.isFinite)).toBe(true);

    // Detection is per-channel and asymmetric: left's own hum sits near
    // 50 Hz, right's near 80 Hz. This is the non-vacuity guard for the
    // assertion below -- without it, two searches that happened to converge
    // to the same value would pass the equality check for the wrong reason.
    expect(
      Math.abs(
        result.leftReport.detected.fundamentalHz - result.rightReport.detected.fundamentalHz,
      ),
    ).toBeGreaterThan(10);

    // Applied frequency and drift are shared by construction: one tracker
    // run over the channel mean, copied to both reports.
    expect(result.leftReport.appliedFundamentalHz).toBe(result.rightReport.appliedFundamentalHz);
    expect(result.leftReport.fundamentalDriftHz).toBe(result.rightReport.fundamentalDriftHz);
    expect(result.leftReport.fundamentalDriftHz).toBeGreaterThan(0);

    // The content tones (2500 Hz / 3500 Hz) are far outside the tracked
    // notch, so they survive and keep the two channels distinguishable.
    expect(Array.from(result.left)).not.toEqual(Array.from(result.right));
  });

  it('refuses bad arguments with a catchable error', () => {
    const { left, right } = fixedFixture();
    expect(() =>
      masteringRepairDehumStereo({ left, right: right.slice(0, 10), sampleRate: SR }),
    ).toThrow();
    expect(() =>
      masteringRepairDehumStereo({
        left: undefined as unknown as Float32Array,
        right,
        sampleRate: SR,
      }),
    ).toThrow();
    expect(() =>
      masteringRepairDehumStereo({
        left,
        right: [1, 2, 3] as unknown as Float32Array,
        sampleRate: SR,
      }),
    ).toThrow();
    expect(() => masteringRepairDehumStereo({ left, right, sampleRate: Number.NaN })).toThrow();
  });
});
