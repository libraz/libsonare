/**
 * Tests for the stereo dereverb WASM wrapper
 * (`masteringRepairDereverbClassicalStereo`).
 *
 * Its sibling entry `masteringRepairDenoiseClassicalStereo` has the same call
 * shape and the same one-shared-`report` result, and behaves oppositely on two
 * counts this file pins down: every field here is a ratio or a fraction, so
 * nothing shifts with the channel count, and a buffer shorter than `nFft` is
 * padded rather than rejected.
 */

import { beforeAll, describe, expect, it } from 'vitest';
import { init, masteringRepairDereverbClassicalStereo } from '../src/index';

const SR = 22050;
// Half a second: 43 hops at the default 256-sample hop, comfortably more than
// the ~4 hops the default 50 ms late lag spans.
const FRAMES = SR / 2;

const LEFT_TONE_HZ = 440;
const RIGHT_TONE_HZ = 660;

// The tail has to decay SLOWER across the late lag than the dry material does,
// or adding it steepens the measured decay instead of flattening it and the
// fixture inverts the property it is built to show.
//
// Three taps rather than one: a single feedback comb notches the spectrum at
// its own period, and the bins sitting in a notch carry no tail at all, so the
// low percentile this statistic takes reads them as steep decay and the gap
// collapses. Measured 2.2 dB with one tap against 9-plus with three. The gains
// sum below 1, which is what keeps the recursion stable.
const TAIL_DELAYS_SAMPLES = [1201, 1601, 2003];
const TAIL_GAINS = [0.4, 0.3, 0.2];

// 20 ms, short enough that the dry signal is nearly gone one late lag later.
const PLUCK_DECAY_SAMPLES = SR * 0.02;

function pluck(freq: number, frames: number): Float32Array {
  const out = new Float32Array(frames);
  for (let i = 0; i < frames; i++) {
    // A struck tone: energy at the start and near-silence afterwards. A dry
    // offset has to be what a late tail is told apart FROM, so it must not
    // sustain across the lag itself.
    out[i] = 0.2 * Math.exp(-i / PLUCK_DECAY_SAMPLES) * Math.sin((2 * Math.PI * freq * i) / SR);
  }
  return out;
}

// A feedback-delay tail: the cheapest thing that sustains across the late lag
// the way a room does. Not a room model -- only the sustain matters here.
function reverberate(samples: Float32Array): Float32Array {
  const out = Float32Array.from(samples);
  for (let i = 0; i < out.length; i++) {
    for (let t = 0; t < TAIL_DELAYS_SAMPLES.length; t++) {
      const delay = TAIL_DELAYS_SAMPLES[t] ?? 0;
      if (i >= delay) {
        out[i] = (out[i] ?? 0) + (TAIL_GAINS[t] ?? 0) * (out[i - delay] ?? 0);
      }
    }
  }
  return out;
}

function buildDryChannels(): { left: Float32Array; right: Float32Array } {
  return { left: pluck(LEFT_TONE_HZ, FRAMES), right: pluck(RIGHT_TONE_HZ, FRAMES) };
}

function buildWetChannels(): { left: Float32Array; right: Float32Array } {
  const dry = buildDryChannels();
  return { left: reverberate(dry.left), right: reverberate(dry.right) };
}

describe('masteringRepairDereverbClassicalStereo (WASM)', () => {
  beforeAll(async () => {
    await init();
  });

  it('dereverberates both channels under one mask and reports it once', () => {
    const { left, right } = buildWetChannels();
    const result = masteringRepairDereverbClassicalStereo({ left, right, sampleRate: SR });

    expect(result.left).toBeInstanceOf(Float32Array);
    expect(result.right).toBeInstanceOf(Float32Array);
    expect(result.left.length).toBe(left.length);
    expect(result.right.length).toBe(right.length);
    expect(result.left).not.toEqual(result.right);

    // One shared report, as on the denoise pair and unlike the four earlier
    // repair stereo entries, whose reports come in a per-channel pair.
    expect(result.report).toBeDefined();
    expect('leftReport' in result).toBe(false);
    expect('rightReport' in result).toBe(false);

    expect(Number.isFinite(result.report.detected.lateDecayRatioDb)).toBe(true);
    expect(Number.isFinite(result.report.meanReductionDb)).toBe(true);
    expect(result.report.suppressedFraction).toBeGreaterThanOrEqual(0);
    expect(result.report.suppressedFraction).toBeLessThanOrEqual(1);
  });

  it('reports no absolute level: doubling the pair moves nothing', () => {
    const { left } = buildWetChannels();
    const silent = new Float32Array(left.length);

    const pair = masteringRepairDereverbClassicalStereo({ left, right: left, sampleRate: SR });
    const single = masteringRepairDereverbClassicalStereo({
      left,
      right: silent,
      sampleRate: SR,
    });

    // The same transformation moves the denoise pair's floorDbfs by exactly
    // 3.0103 dB. Every field here is a ratio or a fraction, so both come back
    // identical -- which is why a stereo figure from this entry is comparable
    // against a mono one.
    expect(pair.report.detected.lateDecayRatioDb).toBe(single.report.detected.lateDecayRatioDb);
    expect(pair.report.meanReductionDb).toBe(single.report.meanReductionDb);
  });

  it('reads a reverberant input higher on lateDecayRatioDb than the same material dry', () => {
    const dry = buildDryChannels();
    const wet = buildWetChannels();

    const dryResult = masteringRepairDereverbClassicalStereo({ ...dry, sampleRate: SR });
    const wetResult = masteringRepairDereverbClassicalStereo({ ...wet, sampleRate: SR });

    // Less negative means the material sustains across the module's late lag,
    // which a tail does and a dry offset does not -- the opposite of what the
    // field's name suggests.
    //
    // A bare `>` would also be satisfied by a fixture whose tail barely
    // outlasts its source, which is the state this file was in when the tail
    // was one comb: -13.07 against -15.31, a 2.2 dB gap that says almost
    // nothing. The margin is what makes this an observation of the property
    // rather than of a rounding direction. Measured -3.99 against -22.91.
    expect(wetResult.report.detected.lateDecayRatioDb).toBeGreaterThan(
      dryResult.report.detected.lateDecayRatioDb + 5,
    );
  });

  it('leaves both WPE fields at exactly zero unless wpeEnabled is set', () => {
    const { left, right } = buildWetChannels();

    // wpeEnabled is clear by default, so these two zeros are the measurement,
    // not unfilled fields.
    const off = masteringRepairDereverbClassicalStereo({ left, right, sampleRate: SR });
    expect(off.report.detected.latePredictability).toBe(0);
    expect(off.report.wpePredictorNorm).toBe(0);

    // The control: with the stage on, both fill in, and the post-clamp figure
    // sits below the pre-clamp one, which is the only observation of that
    // otherwise silent branch.
    const on = masteringRepairDereverbClassicalStereo({
      left,
      right,
      sampleRate: SR,
      wpeEnabled: true,
    });
    expect(on.report.detected.latePredictability).toBeGreaterThan(0);
    expect(on.report.wpePredictorNorm).toBeGreaterThan(0);
    expect(on.report.wpePredictorNorm).toBeLessThan(on.report.detected.latePredictability);
  });

  it('validates threshold and attenuation to a closed [0, 1], so 1 is the strongest gate', () => {
    const { left, right } = buildWetChannels();
    const base = { left, right, sampleRate: SR };

    // A "close the gate" fixture that passes a large number hits the validator
    // rather than the gate, and would prove nothing about the knob.
    expect(() => masteringRepairDereverbClassicalStereo({ ...base, threshold: 1.5 })).toThrow();
    expect(() => masteringRepairDereverbClassicalStereo({ ...base, attenuation: 1.5 })).toThrow();
    expect(() => masteringRepairDereverbClassicalStereo({ ...base, threshold: -0.1 })).toThrow();

    // The endpoint itself, pinned: the bound is `> 1` rather than `>= 1`, so a
    // refusal here would mean the interval is open and every gate assertion
    // below is one step short of the knob's real range.
    const open = masteringRepairDereverbClassicalStereo({ ...base, threshold: 0 });
    const tight = masteringRepairDereverbClassicalStereo({ ...base, threshold: 1 });
    expect(open.report.suppressedFraction).toBeGreaterThan(0);
    expect(tight.report.suppressedFraction).toBeLessThan(open.report.suppressedFraction);
  });

  it('pads an input shorter than nFft instead of rejecting it, unlike the denoise pair', () => {
    const { left, right } = buildWetChannels();
    const shortLeft = left.slice(0, 512);
    const shortRight = right.slice(0, 512);

    const result = masteringRepairDereverbClassicalStereo(shortLeft, shortRight, SR, {
      nFft: 1024,
    });
    expect(result.left.length).toBe(512);
    expect(result.right.length).toBe(512);
  });

  it('accepts the positional call form identically to the request form', () => {
    const { left, right } = buildWetChannels();
    const positional = masteringRepairDereverbClassicalStereo(left, right, SR);
    const request = masteringRepairDereverbClassicalStereo({ left, right, sampleRate: SR });
    expect(positional.left).toEqual(request.left);
    expect(positional.right).toEqual(request.right);
    expect(positional.report.meanReductionDb).toBe(request.report.meanReductionDb);
    expect(positional.report.detected.lateDecayRatioDb).toBe(
      request.report.detected.lateDecayRatioDb,
    );
  });

  it('rejects mismatched channel lengths', () => {
    const { left, right } = buildWetChannels();
    expect(() =>
      masteringRepairDereverbClassicalStereo(left, right.slice(0, right.length - 1), SR),
    ).toThrow();
  });

  it('rejects an empty channel pair', () => {
    expect(() =>
      masteringRepairDereverbClassicalStereo(new Float32Array(0), new Float32Array(0), SR),
    ).toThrow();
  });

  it('rejects a non-finite sample in either channel', () => {
    const { left, right } = buildWetChannels();
    const badLeft = left.slice();
    badLeft[10] = Number.NaN;
    expect(() => masteringRepairDereverbClassicalStereo(badLeft, right, SR)).toThrow();

    const badRight = right.slice();
    badRight[10] = Number.POSITIVE_INFINITY;
    expect(() => masteringRepairDereverbClassicalStereo(left, badRight, SR)).toThrow();
  });

  it('rejects a NaN sample rate', () => {
    const { left, right } = buildWetChannels();
    expect(() => masteringRepairDereverbClassicalStereo(left, right, Number.NaN)).toThrow();
  });

  it('rejects a wrong-typed sample rate', () => {
    const { left, right } = buildWetChannels();
    expect(() =>
      masteringRepairDereverbClassicalStereo(left, right, 'not-a-number' as unknown as number),
    ).toThrow();
  });

  it('rejects a non-power-of-two nFft and a hopLength outside (0, nFft]', () => {
    const { left, right } = buildWetChannels();
    expect(() =>
      masteringRepairDereverbClassicalStereo(left, right, SR, { nFft: 1500, hopLength: 256 }),
    ).toThrow();
    expect(() =>
      masteringRepairDereverbClassicalStereo(left, right, SR, { nFft: 1024, hopLength: 0 }),
    ).toThrow();
    expect(() =>
      masteringRepairDereverbClassicalStereo(left, right, SR, { nFft: 1024, hopLength: 2048 }),
    ).toThrow();
  });
});
