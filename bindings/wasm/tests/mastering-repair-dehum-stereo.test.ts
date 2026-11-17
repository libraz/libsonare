/**
 * Tests for the stereo dehum WASM wrapper (`masteringRepairDehumStereo`).
 *
 * The sharing rule is conditional on `adaptive`: with it set, both channels
 * track one shared fundamental (mains hum is one physical source), so
 * `appliedFundamentalHz` and `fundamentalDriftHz` come back identical in both
 * reports by construction even though each channel carries its own hum
 * frequency and `detected` still measures each channel's own input. With
 * `adaptive` clear, the default, nothing is shared and the two channels are
 * filtered independently. A default-only fixture would never witness the
 * sharing rule, so both states are covered here.
 */

import { beforeAll, describe, expect, it } from 'vitest';
import { init, masteringRepairDehumStereo } from '../src/index';

const SR = 22050;
// Exactly one second: the detector's frame is min(size, sampleRate * 1s), so
// this is also exactly one detection window -- no averaging across frames to
// reason through.
const FRAMES = SR;

// Both land exactly on the 17-point search grid the detector and the tracker
// share (window [48, 52] Hz split into 16 steps of 0.25 Hz), so the search
// picks them up without interpolation error.
const LEFT_FUNDAMENTAL_HZ = 48.5;
const RIGHT_FUNDAMENTAL_HZ = 51.0;

// Program content far past the highest harmonic bin the detector ever reads
// (16 * 51 Hz = 816 Hz, plus its floor probes), so it cannot leak into any
// harmonic or floor measurement and is left untouched by every notch.
const LEFT_DISTINCT_HZ = 3000;
const RIGHT_DISTINCT_HZ = 3300;

function sine(freq: number, frames: number, amp: number): Float32Array {
  const out = new Float32Array(frames);
  for (let i = 0; i < frames; i++) {
    out[i] = amp * Math.sin((2 * Math.PI * freq * i) / SR);
  }
  return out;
}

function addInto(target: Float32Array, source: Float32Array): void {
  for (let i = 0; i < target.length; i++) {
    target[i] = (target[i] ?? 0) + (source[i] ?? 0);
  }
}

// A fundamental, its second harmonic, and a distinguishing high tone the
// notch cascade never touches -- per channel, at a different hum frequency
// so the two channels stay distinguishable through the repair.
function buildHumChannels(): { left: Float32Array; right: Float32Array } {
  const left = sine(LEFT_FUNDAMENTAL_HZ, FRAMES, 0.3);
  addInto(left, sine(2 * LEFT_FUNDAMENTAL_HZ, FRAMES, 0.15));
  addInto(left, sine(LEFT_DISTINCT_HZ, FRAMES, 0.05));

  const right = sine(RIGHT_FUNDAMENTAL_HZ, FRAMES, 0.3);
  addInto(right, sine(2 * RIGHT_FUNDAMENTAL_HZ, FRAMES, 0.15));
  addInto(right, sine(RIGHT_DISTINCT_HZ, FRAMES, 0.05));

  return { left, right };
}

describe('masteringRepairDehumStereo (WASM)', () => {
  beforeAll(async () => {
    await init();
  });

  it('dehums each channel independently with adaptive tracking off (the default)', () => {
    const { left, right } = buildHumChannels();
    const result = masteringRepairDehumStereo({ left, right, sampleRate: SR });

    expect(result.channels[0]).toBeInstanceOf(Float32Array);
    expect(result.channels[1]).toBeInstanceOf(Float32Array);
    expect(result.channels[0].length).toBe(left.length);
    expect(result.channels[1].length).toBe(right.length);
    // Different hum frequencies and distinct high tones per channel: an
    // implementation that dehummed one channel and returned it for both
    // would make these equal.
    expect(result.channels[0]).not.toEqual(result.channels[1]);
    // The notch cascade only touches narrow bands around the hum harmonics;
    // the distinguishing high tone survives, so the repaired channel is not
    // simply zeroed out.
    expect(Array.from(result.channels[0]).some((v) => v !== 0)).toBe(true);
    expect(Array.from(result.channels[1]).some((v) => v !== 0)).toBe(true);

    // detect_hum always runs the estimation path on the channel's own raw
    // input, whatever `adaptive` says, so this holds in both configurations.
    expect(result.reports[0].detected.fundamentalHz).toBeCloseTo(LEFT_FUNDAMENTAL_HZ, 3);
    expect(result.reports[1].detected.fundamentalHz).toBeCloseTo(RIGHT_FUNDAMENTAL_HZ, 3);
    expect(result.reports[0].detected.harmonics).toBeGreaterThanOrEqual(2);
    expect(result.reports[1].detected.harmonics).toBeGreaterThanOrEqual(2);

    // Fixed mode notches every configured harmonic (default 4) below Nyquist,
    // using the configured fundamental verbatim -- not the detected one.
    expect(result.reports[0].notchedHarmonics).toBe(4);
    expect(result.reports[1].notchedHarmonics).toBe(4);
    expect(result.reports[0].appliedFundamentalHz).toBe(50);
    expect(result.reports[1].appliedFundamentalHz).toBe(50);
    // The measurement, not an unset field: no tracking ran.
    expect(result.reports[0].fundamentalDriftHz).toBe(0);
    expect(result.reports[1].fundamentalDriftHz).toBe(0);

    for (const report of [result.reports[0], result.reports[1]]) {
      expect(report.detected.harmonicDbfs).toBeInstanceOf(Float32Array);
      expect(report.detected.harmonicDbfs.length).toBe(16);
      expect(Array.from(report.detected.harmonicDbfs).every((v) => Number.isFinite(v))).toBe(true);
      // The two harmonics actually planted stand well above a slot with
      // nothing behind it.
      expect(report.detected.harmonicDbfs[0]).toBeGreaterThan(report.detected.harmonicDbfs[15]);
      expect(report.detected.harmonicDbfs[1]).toBeGreaterThan(report.detected.harmonicDbfs[15]);
    }
  });

  it('shares one tracked fundamental across channels when adaptive tracking is on', () => {
    const { left, right } = buildHumChannels();
    const result = masteringRepairDehumStereo({ left, right, sampleRate: SR, adaptive: true });

    expect(result.channels[0].length).toBe(left.length);
    expect(result.channels[1].length).toBe(right.length);
    expect(result.channels[0]).not.toEqual(result.channels[1]);

    // Pinned before the equality below, because two reports agreeing on a
    // frequency the tracker never left satisfies it while witnessing nothing.
    // Measured here: 50.71 against the configured 50.
    const fixedResult = masteringRepairDehumStereo({ left, right, sampleRate: SR });
    expect(result.reports[0].appliedFundamentalHz).not.toBe(
      fixedResult.reports[0].appliedFundamentalHz,
    );

    // Mains hum is one physical source: the tracker reads the channel mean
    // and both cascades follow the one frequency it finds, so these agree
    // exactly by construction.
    expect(result.reports[0].appliedFundamentalHz).toBe(result.reports[1].appliedFundamentalHz);
    expect(result.reports[0].fundamentalDriftHz).toBe(result.reports[1].fundamentalDriftHz);

    // The drift cannot exceed the search range the tracker is allowed to roam,
    // and at the default range this fixture drives it to the boundary exactly,
    // so a bound on it here is a statement about the range rather than about
    // the signal. It becomes an independent measurement only past a range of
    // about 4 Hz, where the tracker stops hitting its rails.
    expect(result.reports[0].fundamentalDriftHz).toBeLessThanOrEqual(2);

    // Each report's detection still measures that channel's own input, so
    // the two channels' own hum frequencies remain visibly different even
    // though the applied frequency was shared.
    expect(result.reports[0].detected.fundamentalHz).not.toBe(
      result.reports[1].detected.fundamentalHz,
    );
    expect(result.reports[0].detected.fundamentalHz).toBeCloseTo(LEFT_FUNDAMENTAL_HZ, 3);
    expect(result.reports[1].detected.fundamentalHz).toBeCloseTo(RIGHT_FUNDAMENTAL_HZ, 3);

    for (const report of [result.reports[0], result.reports[1]]) {
      expect(report.detected.harmonicDbfs.length).toBe(16);
    }
  });

  it('is request-only and refuses the positional call form', () => {
    const { left, right } = buildHumChannels();
    const positional = masteringRepairDehumStereo as unknown as (...args: unknown[]) => unknown;
    expect(() => positional(left, right, SR)).toThrow(TypeError);
  });

  it('rejects mismatched channel lengths', () => {
    const { left, right } = buildHumChannels();
    expect(() =>
      masteringRepairDehumStereo({
        left: left,
        right: right.slice(0, right.length - 1),
        sampleRate: SR,
      }),
    ).toThrow();
  });

  it('rejects an empty channel pair', () => {
    expect(() =>
      masteringRepairDehumStereo({
        left: new Float32Array(0),
        right: new Float32Array(0),
        sampleRate: SR,
      }),
    ).toThrow();
  });

  it('rejects a non-finite sample in either channel', () => {
    const { left, right } = buildHumChannels();
    const badLeft = left.slice();
    badLeft[10] = Number.NaN;
    expect(() =>
      masteringRepairDehumStereo({ left: badLeft, right: right, sampleRate: SR }),
    ).toThrow();

    const badRight = right.slice();
    badRight[10] = Number.POSITIVE_INFINITY;
    expect(() =>
      masteringRepairDehumStereo({ left: left, right: badRight, sampleRate: SR }),
    ).toThrow();
  });

  it('rejects a NaN sample rate', () => {
    const { left, right } = buildHumChannels();
    expect(() => masteringRepairDehumStereo({ left, right, sampleRate: Number.NaN })).toThrow();
  });

  it('rejects a wrong-typed sample rate', () => {
    const { left, right } = buildHumChannels();
    expect(() =>
      masteringRepairDehumStereo({ left, right, sampleRate: 'not-a-number' as unknown as number }),
    ).toThrow();
  });
});
