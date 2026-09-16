/**
 * Tests for the stereo decrackle WASM wrapper (`masteringRepairDecrackleStereo`).
 *
 * Unlike stereo declick and declip, decrackle has no linking: crackle is
 * surface damage with no common event between channels for a shared
 * decision to agree about, so each channel is decrackled entirely on its
 * own. The fixtures therefore plant different spike counts per channel to
 * prove the two passes are independent -- an implementation that decrackled
 * one channel and returned it twice would pass a same-length check but fail
 * on content and on per-channel report counts.
 */

import { beforeAll, describe, expect, it } from 'vitest';
import { init, masteringRepairDecrackleStereo } from '../src/index';

const SR = 22050;
const FRAMES = 2000;
const SPIKE_VALUE = 0.5;
const LEFT_SPIKE_POSITIONS = [100, 300, 500, 700, 900];
const RIGHT_SPIKE_POSITIONS = [150, 450, 750];

// The median detector flags index i when |sample[i] - median(window)| > threshold.
// Every spike sits on an otherwise-silent bed, so its window's median is exactly
// 0 and its own deviation is exactly SPIKE_VALUE: a threshold below that value
// catches every spike, one above it catches none.
const LOW_THRESHOLD = 0.3;
const HIGH_THRESHOLD = 0.6;

function plantSpikes(samples: Float32Array, positions: number[], value: number): void {
  positions.forEach((pos, i) => {
    samples[pos] = i % 2 === 0 ? value : -value;
  });
}

// Isolated spikes over a quiet per-channel tone. The bed cannot be silence:
// median mode replaces every spike with its window median, so two silent beds
// come back as two identical all-zero buffers and "the channels differ" is
// unsatisfiable rather than merely weak.
//
// BED_AMPLITUDE is small enough to leave the exact counts intact. A spike's
// window median is its larger neighbour, so its deviation is
// SPIKE_VALUE - BED_AMPLITUDE = 0.45 -- still above LOW_THRESHOLD and still
// below HIGH_THRESHOLD. Away from a spike the middle sample of three
// consecutive tone samples is its own median except at an extremum, where the
// deviation is BED_AMPLITUDE * (1 - cos w) and stays under 0.002 for both
// frequencies.
const BED_AMPLITUDE = 0.05;
const LEFT_BED_HZ = 440;
const RIGHT_BED_HZ = 880;

function buildSpikeChannels(): { left: Float32Array; right: Float32Array } {
  const left = sine(LEFT_BED_HZ, FRAMES, BED_AMPLITUDE, 0.0);
  const right = sine(RIGHT_BED_HZ, FRAMES, BED_AMPLITUDE, 0.0);
  plantSpikes(left, LEFT_SPIKE_POSITIONS, SPIKE_VALUE);
  plantSpikes(right, RIGHT_SPIKE_POSITIONS, SPIKE_VALUE);
  return { left, right };
}

function sine(freq: number, frames: number, amp: number, phase: number): Float32Array {
  const out = new Float32Array(frames);
  for (let i = 0; i < frames; i++) {
    out[i] = amp * Math.sin((2 * Math.PI * freq * i) / SR + phase);
  }
  return out;
}

function addInto(target: Float32Array, source: Float32Array): void {
  for (let i = 0; i < target.length; i++) {
    target[i] = (target[i] ?? 0) + (source[i] ?? 0);
  }
}

// Real tonal content, not a silent bed: the wavelet cap test needs a
// non-degenerate BayesShrink noise estimate, which a mostly-zero fixture
// never produces (its detail coefficients are mostly exactly zero, so the
// computed threshold degenerates to 0 regardless of the configured cap).
// A high-frequency component well inside the Haar level-0 detail band
// gives the MAD noise estimate real energy to measure.
const TONE_FRAMES = 4096;
function buildToneChannels(): { left: Float32Array; right: Float32Array } {
  const left = sine(440, TONE_FRAMES, 0.3, 0.0);
  addInto(left, sine(5000, TONE_FRAMES, 0.05, 0.0));
  const right = sine(550, TONE_FRAMES, 0.3, 0.35);
  addInto(right, sine(4000, TONE_FRAMES, 0.05, 0.35));
  return { left, right };
}

describe('masteringRepairDecrackleStereo (WASM)', () => {
  beforeAll(async () => {
    await init();
  });

  it('decrackles each channel independently in median mode', () => {
    const { left, right } = buildSpikeChannels();
    const result = masteringRepairDecrackleStereo({
      left,
      right,
      sampleRate: SR,
      threshold: LOW_THRESHOLD,
    });

    expect(result.left).toBeInstanceOf(Float32Array);
    expect(result.right).toBeInstanceOf(Float32Array);
    expect(result.left.length).toBe(left.length);
    expect(result.right.length).toBe(right.length);
    // Different spike counts and positions per channel: an implementation
    // that decrackled one channel and returned it for both would make these
    // equal.
    expect(result.left).not.toEqual(result.right);

    expect(result.leftReport.detected.sampleCount).toBe(LEFT_SPIKE_POSITIONS.length);
    expect(result.rightReport.detected.sampleCount).toBe(RIGHT_SPIKE_POSITIONS.length);
    // Median mode: the detector and the repair share one criterion, so every
    // detected sample is also replaced.
    expect(result.leftReport.replacedSamples).toBe(LEFT_SPIKE_POSITIONS.length);
    expect(result.rightReport.replacedSamples).toBe(RIGHT_SPIKE_POSITIONS.length);
    // Wavelet-only fields stay at their zero default -- that mode never ran.
    for (const report of [result.leftReport, result.rightReport]) {
      expect(report.detailCoefficients).toBe(0);
      expect(report.shrunkCoefficients).toBe(0);
      expect(report.noiseSigma).toBe(0);
    }
  });

  it('threshold reaches the detector: lowering it moves detected.sampleCount', () => {
    const { left, right } = buildSpikeChannels();
    const caught = masteringRepairDecrackleStereo({
      left,
      right,
      sampleRate: SR,
      threshold: LOW_THRESHOLD,
    });
    const missed = masteringRepairDecrackleStereo({
      left,
      right,
      sampleRate: SR,
      threshold: HIGH_THRESHOLD,
    });

    expect(caught.leftReport.detected.sampleCount).toBe(LEFT_SPIKE_POSITIONS.length);
    expect(caught.rightReport.detected.sampleCount).toBe(RIGHT_SPIKE_POSITIONS.length);
    // Every spike's deviation from its window's median is exactly
    // SPIKE_VALUE, so a threshold above it catches none.
    expect(missed.leftReport.detected.sampleCount).toBe(0);
    expect(missed.rightReport.detected.sampleCount).toBe(0);
  });

  it('detects by the median criterion regardless of mode, but only median mode replaces', () => {
    const { left, right } = buildSpikeChannels();
    const median = masteringRepairDecrackleStereo({
      left,
      right,
      sampleRate: SR,
      mode: 'median',
      threshold: LOW_THRESHOLD,
    });
    const wavelet = masteringRepairDecrackleStereo({
      left,
      right,
      sampleRate: SR,
      mode: 'waveletShrinkage',
      threshold: LOW_THRESHOLD,
    });

    // Detection does not depend on the removal method.
    expect(wavelet.leftReport.detected.sampleCount).toBe(median.leftReport.detected.sampleCount);
    expect(wavelet.rightReport.detected.sampleCount).toBe(median.rightReport.detected.sampleCount);

    // Wavelet mode never runs the median replacement loop.
    expect(wavelet.leftReport.replacedSamples).toBe(0);
    expect(wavelet.rightReport.replacedSamples).toBe(0);

    // The Haar analysis over FRAMES=2000 at the default levels=4 produces a
    // fixed coefficient count independent of content or threshold:
    // active sizes [2000, 1000, 500, 250] halve to detail-band sizes
    // [1000, 500, 250, 125], which sum to 1875.
    expect(wavelet.leftReport.detailCoefficients).toBe(1875);
    expect(wavelet.rightReport.detailCoefficients).toBe(1875);
    expect(wavelet.leftReport.noiseSigma).toBeGreaterThanOrEqual(0);
    expect(Number.isFinite(wavelet.leftReport.noiseSigma)).toBe(true);
  });

  it('binds the wavelet shrinkage cap only when the configured threshold is low enough', () => {
    const { left, right } = buildToneChannels();
    // 1e-6 sits far below any BayesShrink threshold this tonal fixture can
    // produce, so it binds as the cap and almost nothing is driven to zero.
    // 1000 sits far above it, so the cap never binds and the shrinkage uses
    // BayesShrink's own (larger, nonzero) computed threshold instead.
    const capped = masteringRepairDecrackleStereo({
      left,
      right,
      sampleRate: SR,
      mode: 'waveletShrinkage',
      threshold: 1e-6,
    });
    const uncapped = masteringRepairDecrackleStereo({
      left,
      right,
      sampleRate: SR,
      mode: 'waveletShrinkage',
      threshold: 1000,
    });

    expect(uncapped.leftReport.shrunkCoefficients).toBeGreaterThan(
      capped.leftReport.shrunkCoefficients,
    );
    expect(uncapped.rightReport.shrunkCoefficients).toBeGreaterThan(
      capped.rightReport.shrunkCoefficients,
    );
  });

  it('accepts the positional call form identically to the request form', () => {
    const { left, right } = buildSpikeChannels();
    const positional = masteringRepairDecrackleStereo(left, right, SR, {
      threshold: LOW_THRESHOLD,
    });
    const request = masteringRepairDecrackleStereo({
      left,
      right,
      sampleRate: SR,
      threshold: LOW_THRESHOLD,
    });
    expect(positional.left).toEqual(request.left);
    expect(positional.right).toEqual(request.right);
  });

  it('rejects mismatched channel lengths', () => {
    const { left, right } = buildSpikeChannels();
    expect(() =>
      masteringRepairDecrackleStereo(left, right.slice(0, right.length - 1), SR, {
        threshold: LOW_THRESHOLD,
      }),
    ).toThrow();
  });

  it('rejects an empty channel pair', () => {
    expect(() =>
      masteringRepairDecrackleStereo(new Float32Array(0), new Float32Array(0), SR, {
        threshold: LOW_THRESHOLD,
      }),
    ).toThrow();
  });

  it('rejects a non-finite sample in either channel', () => {
    const { left, right } = buildSpikeChannels();
    const badLeft = left.slice();
    badLeft[10] = Number.NaN;
    expect(() =>
      masteringRepairDecrackleStereo(badLeft, right, SR, { threshold: LOW_THRESHOLD }),
    ).toThrow();

    const badRight = right.slice();
    badRight[10] = Number.POSITIVE_INFINITY;
    expect(() =>
      masteringRepairDecrackleStereo(left, badRight, SR, { threshold: LOW_THRESHOLD }),
    ).toThrow();
  });

  it('rejects an unknown mode', () => {
    const { left, right } = buildSpikeChannels();
    expect(() =>
      masteringRepairDecrackleStereo(left, right, SR, { mode: 'not-a-mode' as never }),
    ).toThrow();
  });

  it('rejects a NaN sample rate', () => {
    const { left, right } = buildSpikeChannels();
    expect(() =>
      masteringRepairDecrackleStereo(left, right, Number.NaN, { threshold: LOW_THRESHOLD }),
    ).toThrow();
  });

  it('rejects a wrong-typed sample rate', () => {
    const { left, right } = buildSpikeChannels();
    expect(() =>
      masteringRepairDecrackleStereo(left, right, 'not-a-number' as unknown as number, {
        threshold: LOW_THRESHOLD,
      }),
    ).toThrow();
  });
});
