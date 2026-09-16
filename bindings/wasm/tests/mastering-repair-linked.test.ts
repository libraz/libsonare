/**
 * Tests for the N-channel repair WASM wrappers
 * (`masteringRepairDenoiseClassicalLinked`,
 * `masteringRepairDereverbClassicalLinked`).
 *
 * Both take a channel set and return one output per channel plus one shared
 * report. Three things separate them from the stereo pair they generalize, and
 * each has a case below:
 *
 *  - the denoise `detected` levels are the SET's and absolute, so N identical
 *    channels read 10*log10(N) above one -- which only a third channel can
 *    witness, since N=2 cannot tell 10*log10(2) from 10*log10(3);
 *  - the dereverb report is ratios and fractions throughout, so it does not
 *    move with the channel count at all;
 *  - the two answer a short input oppositely: denoise refuses one, dereverb
 *    pads it.
 *
 * The core scans no channel for a non-finite sample and guards only
 * `channels[0]` itself, so the wrapper's per-channel validation is the whole
 * guard on the set. The NaN case feeds its NaN to a channel PAST the first for
 * that reason.
 */

import { beforeAll, describe, expect, it } from 'vitest';
import {
  init,
  masteringRepairDenoiseClassical,
  masteringRepairDenoiseClassicalLinked,
  masteringRepairDenoiseClassicalStereo,
  masteringRepairDereverbClassical,
  masteringRepairDereverbClassicalLinked,
  masteringRepairDereverbClassicalStereo,
} from '../src/index';

const SR = 22050;
// Half a second: 43 hops at the default 256-sample hop, enough frames for the
// quantile estimator to have a quietest decile and for the late-lag statistic
// to span its delay, and short enough to stay inside the default timeout.
const FRAMES = SR / 2;

// Deterministic LCG, so a level measured in one case is comparable against
// another case's.
function lcg(seed: number): () => number {
  let state = seed >>> 0;
  return () => {
    state = (Math.imul(state, 1664525) + 1013904223) >>> 0;
    return (state >>> 8) / (1 << 24);
  };
}

function noisyTone(freqHz: number, seed: number): Float32Array {
  const next = lcg(seed);
  const out = new Float32Array(FRAMES);
  for (let i = 0; i < FRAMES; i++) {
    out[i] = 0.5 * Math.sin((2 * Math.PI * freqHz * i) / SR) + (next() - 0.5) * 0.4;
  }
  return out;
}

// Gated noise through a decaying feedback comb: a real exponential tail between
// bursts, which is what the late-lag decay statistic measures. A sustained tone
// would give the WPE stage something to predict while leaving that statistic
// nothing to see, and the report would then be about the fixture rather than
// about the pass.
function reverberant(seed: number): Float32Array {
  const next = lcg(seed);
  const out = new Float32Array(FRAMES);
  const delay = Math.round(0.02 * SR);
  const burst = Math.round(SR / 8);
  for (let i = 0; i < FRAMES; i++) {
    const excited = Math.floor(i / burst) % 2 === 0;
    const dry = excited ? (next() - 0.5) * 0.3 : 0;
    out[i] = dry + (i >= delay ? 0.7 * (out[i - delay] as number) : 0);
  }
  return out;
}

const silence = () => new Float32Array(FRAMES);

describe('masteringRepairDenoiseClassicalLinked (WASM)', () => {
  beforeAll(async () => {
    await init();
  });

  it('reproduces the mono entry for one channel, element for element', () => {
    const samples = noisyTone(440, 1);
    const linked = masteringRepairDenoiseClassicalLinked({ channels: [samples], sampleRate: SR });
    const mono = masteringRepairDenoiseClassical({ samples, sampleRate: SR });

    expect(linked.channels).toHaveLength(1);
    expect(linked.channels[0]).toEqual(mono);
  });

  it('reproduces the stereo entry for two channels, plane for plane', () => {
    const left = noisyTone(440, 1);
    const right = noisyTone(660, 7);
    const linked = masteringRepairDenoiseClassicalLinked({
      channels: [left, right],
      sampleRate: SR,
    });
    const stereo = masteringRepairDenoiseClassicalStereo({ left, right, sampleRate: SR });

    expect(linked.channels).toHaveLength(2);
    // The two planes genuinely differ, so a wrapper that swapped them would
    // fail the two equalities below rather than satisfy both.
    expect(linked.channels[0]).not.toEqual(linked.channels[1]);
    expect(linked.channels[0]).toEqual(stereo.left);
    expect(linked.channels[1]).toEqual(stereo.right);
    expect(linked.report.detected.floorDbfs).toBe(stereo.report.detected.floorDbfs);
    expect(linked.report.meanReductionDb).toBe(stereo.report.meanReductionDb);
  });

  it('reports a set-level absolute floor: three equal channels read 10*log10(3) over one', () => {
    const samples = noisyTone(440, 1);
    const one = masteringRepairDenoiseClassicalLinked({ channels: [samples], sampleRate: SR });
    const three = masteringRepairDenoiseClassicalLinked({
      channels: [samples, samples, samples],
      sampleRate: SR,
    });

    // 4.7712 dB, not the 3.0103 a pair reads -- the floor is referred to the
    // summed mean square of the whole set. A "call the stereo path twice"
    // implementation cannot produce this, and a pair cannot distinguish the two
    // constants, which is why the case needs a third channel.
    expect(three.report.detected.floorDbfs - one.report.detected.floorDbfs).toBeCloseTo(
      10 * Math.log10(3),
      3,
    );

    // The shape of the set-level detection, and the fields that are fractions
    // and therefore do NOT move with the count.
    expect(three.report.detected.bandFloorDbfs).toBeInstanceOf(Float32Array);
    expect(three.report.detected.bandFloorDbfs.length).toBe(32);
    expect(three.report.meanReductionDb).toBeCloseTo(one.report.meanReductionDb, 5);
    expect(three.report.maxReductionDb).toBeCloseTo(one.report.maxReductionDb, 5);
  });

  it('returns three identical channels identical to each other', () => {
    const samples = noisyTone(440, 1);
    const { channels } = masteringRepairDenoiseClassicalLinked({
      channels: [samples, samples, samples],
      sampleRate: SR,
    });

    expect(channels).toHaveLength(3);
    // Not vacuous: the pass changed the material, so the equality is about one
    // mask over the set rather than about three untouched copies.
    expect(channels[0]).not.toEqual(samples);
    expect(channels[1]).toEqual(channels[0]);
    expect(channels[2]).toEqual(channels[0]);
  });

  it('routes each channel to its own output plane, leaving silent planes at zero', () => {
    const samples = noisyTone(440, 1);
    // A silent channel's STFT power is exactly 0 and x + 0 == x exactly, so the
    // mask is bit-identical between the two runs and the routing can be
    // asserted exactly rather than within a tolerance.
    const first = masteringRepairDenoiseClassicalLinked({
      channels: [samples, silence(), silence()],
      sampleRate: SR,
    });
    const second = masteringRepairDenoiseClassicalLinked({
      channels: [silence(), samples, silence()],
      sampleRate: SR,
    });

    expect(first.channels[0]).toEqual(second.channels[1]);
    for (const plane of [first.channels[1], first.channels[2], second.channels[0]]) {
      expect(Array.from(plane as Float32Array).every((v) => v === 0)).toBe(true);
    }
    // The control for the equality above: the routed plane is not itself zero,
    // so "every plane is zero" would not satisfy this case.
    expect(Array.from(first.channels[0] as Float32Array).some((v) => v !== 0)).toBe(true);
  });

  it('rejects a non-finite sample in a channel past the first', () => {
    const clean = noisyTone(440, 1);
    const withNan = clean.slice();
    withNan[10] = Number.NaN;
    const withInf = clean.slice();
    withInf[10] = Number.POSITIVE_INFINITY;

    // channels[0] is valid in both sets, so a wrapper that validated only the
    // first channel would let these through: the core guards channels[0] alone
    // and scans no channel for a non-finite sample.
    expect(() =>
      masteringRepairDenoiseClassicalLinked({ channels: [clean, withNan, clean], sampleRate: SR }),
    ).toThrow(/non-finite/);
    expect(() =>
      masteringRepairDenoiseClassicalLinked({ channels: [clean, clean, withInf], sampleRate: SR }),
    ).toThrow(/non-finite/);
    // The positive control: the same three-channel shape with finite samples
    // throughout succeeds, so the refusals above are about the values.
    expect(() =>
      masteringRepairDenoiseClassicalLinked({ channels: [clean, clean, clean], sampleRate: SR }),
    ).not.toThrow();
  });

  it('accepts the positional call form identically to the request form', () => {
    const channels = [noisyTone(440, 1), noisyTone(660, 7)];
    const positional = masteringRepairDenoiseClassicalLinked(channels, SR, { reductionDb: 18 });
    const request = masteringRepairDenoiseClassicalLinked({
      channels,
      sampleRate: SR,
      reductionDb: 18,
    });

    expect(positional.channels[0]).toEqual(request.channels[0]);
    expect(positional.channels[1]).toEqual(request.channels[1]);
    expect(positional.report.detected.floorDbfs).toBe(request.report.detected.floorDbfs);
    expect(positional.report.meanReductionDb).toBe(request.report.meanReductionDb);
    // reductionDb reached the pass through both forms rather than being dropped
    // by the destructuring on either.
    const atDefault = masteringRepairDenoiseClassicalLinked({ channels, sampleRate: SR });
    expect(request.report.maxReductionDb).not.toBeCloseTo(atDefault.report.maxReductionDb, 3);
  });

  it('rejects an empty channel array', () => {
    expect(() => masteringRepairDenoiseClassicalLinked({ channels: [], sampleRate: SR })).toThrow(
      /at least one channel/,
    );
  });

  it('rejects channels that disagree on length', () => {
    const full = noisyTone(440, 1);
    expect(() =>
      masteringRepairDenoiseClassicalLinked({
        channels: [full, full.slice(0, full.length - 1)],
        sampleRate: SR,
      }),
    ).toThrow(/channel lengths must match/);
  });

  it('rejects a NaN sample rate and a non-power-of-two nFft', () => {
    const channels = [noisyTone(440, 1), noisyTone(660, 7)];
    expect(() =>
      masteringRepairDenoiseClassicalLinked({ channels, sampleRate: Number.NaN }),
    ).toThrow();
    expect(() =>
      masteringRepairDenoiseClassicalLinked(channels, SR, { nFft: 1500, hopLength: 256 }),
    ).toThrow();
  });
});

describe('masteringRepairDereverbClassicalLinked (WASM)', () => {
  beforeAll(async () => {
    await init();
  });

  it('reproduces the mono entry for one channel, element for element', () => {
    const samples = reverberant(1);
    const linked = masteringRepairDereverbClassicalLinked({ channels: [samples], sampleRate: SR });
    const mono = masteringRepairDereverbClassical({ samples, sampleRate: SR });

    expect(linked.channels).toHaveLength(1);
    expect(linked.channels[0]).toEqual(mono);
  });

  it('reproduces the stereo entry for two channels, plane for plane', () => {
    const left = reverberant(1);
    const right = reverberant(7);
    const linked = masteringRepairDereverbClassicalLinked({
      channels: [left, right],
      sampleRate: SR,
      wpeEnabled: true,
    });
    const stereo = masteringRepairDereverbClassicalStereo({
      left,
      right,
      sampleRate: SR,
      wpeEnabled: true,
    });

    expect(linked.channels).toHaveLength(2);
    expect(linked.channels[0]).not.toEqual(linked.channels[1]);
    expect(linked.channels[0]).toEqual(stereo.left);
    expect(linked.channels[1]).toEqual(stereo.right);
    expect(linked.report.wpePredictorNorm).toBe(stereo.report.wpePredictorNorm);
  });

  it('reports the same figures at three channels as at one', () => {
    const samples = reverberant(1);
    const options = { sampleRate: SR, wpeEnabled: true };
    const one = masteringRepairDereverbClassicalLinked({ channels: [samples], ...options });
    const three = masteringRepairDereverbClassicalLinked({
      channels: [samples, samples, samples],
      ...options,
    });

    // Non-vacuity first: every field asserted below has a value to be equal
    // about. The WPE stage is enabled so the two predictor fields are
    // measurements rather than the 0 a default-config pass reports.
    expect(one.report.detected.lateDecayRatioDb).not.toBe(0);
    expect(one.report.detected.latePredictability).toBeGreaterThan(0);
    expect(one.report.wpePredictorNorm).toBeGreaterThan(0);
    expect(one.report.meanReductionDb).toBeGreaterThan(0);
    expect(one.report.suppressedFraction).toBeGreaterThan(0);

    // Every field is a ratio or a fraction, so unlike the denoise floor nothing
    // here shifts with the channel count.
    expect(three.report.detected.lateDecayRatioDb).toBeCloseTo(
      one.report.detected.lateDecayRatioDb,
      6,
    );
    expect(three.report.detected.latePredictability).toBeCloseTo(
      one.report.detected.latePredictability,
      6,
    );
    expect(three.report.meanReductionDb).toBeCloseTo(one.report.meanReductionDb, 6);
    expect(three.report.suppressedFraction).toBeCloseTo(one.report.suppressedFraction, 6);
    expect(three.report.wpePredictorNorm).toBeCloseTo(one.report.wpePredictorNorm, 6);
  });

  it('returns three identical channels identical to each other', () => {
    const samples = reverberant(1);
    const { channels } = masteringRepairDereverbClassicalLinked({
      channels: [samples, samples, samples],
      sampleRate: SR,
      wpeEnabled: true,
    });

    expect(channels).toHaveLength(3);
    expect(channels[0]).not.toEqual(samples);
    expect(channels[1]).toEqual(channels[0]);
    expect(channels[2]).toEqual(channels[0]);
  });

  it('routes each channel to its own output plane, leaving silent planes at zero', () => {
    const samples = reverberant(1);
    const first = masteringRepairDereverbClassicalLinked({
      channels: [samples, silence(), silence()],
      sampleRate: SR,
    });
    const second = masteringRepairDereverbClassicalLinked({
      channels: [silence(), samples, silence()],
      sampleRate: SR,
    });

    expect(first.channels[0]).toEqual(second.channels[1]);
    for (const plane of [first.channels[1], first.channels[2], second.channels[0]]) {
      expect(Array.from(plane as Float32Array).every((v) => v === 0)).toBe(true);
    }
    expect(Array.from(first.channels[0] as Float32Array).some((v) => v !== 0)).toBe(true);
  });

  it('rejects a non-finite sample in a channel past the first', () => {
    const clean = reverberant(1);
    const withNan = clean.slice();
    withNan[10] = Number.NaN;
    const withInf = clean.slice();
    withInf[10] = Number.NEGATIVE_INFINITY;

    expect(() =>
      masteringRepairDereverbClassicalLinked({ channels: [clean, withNan, clean], sampleRate: SR }),
    ).toThrow(/non-finite/);
    expect(() =>
      masteringRepairDereverbClassicalLinked({ channels: [clean, clean, withInf], sampleRate: SR }),
    ).toThrow(/non-finite/);
    expect(() =>
      masteringRepairDereverbClassicalLinked({ channels: [clean, clean, clean], sampleRate: SR }),
    ).not.toThrow();
  });

  it('accepts the positional call form identically to the request form', () => {
    const channels = [reverberant(1), reverberant(7)];
    const positional = masteringRepairDereverbClassicalLinked(channels, SR, { wpeEnabled: true });
    const request = masteringRepairDereverbClassicalLinked({
      channels,
      sampleRate: SR,
      wpeEnabled: true,
    });

    expect(positional.channels[0]).toEqual(request.channels[0]);
    expect(positional.channels[1]).toEqual(request.channels[1]);
    expect(positional.report.wpePredictorNorm).toBe(request.report.wpePredictorNorm);
    // wpeEnabled reached the pass through both forms: with the stage off the
    // predictor norm is exactly 0.
    const atDefault = masteringRepairDereverbClassicalLinked({ channels, sampleRate: SR });
    expect(atDefault.report.wpePredictorNorm).toBe(0);
    expect(request.report.wpePredictorNorm).toBeGreaterThan(0);
  });

  it('rejects an empty channel array', () => {
    expect(() => masteringRepairDereverbClassicalLinked({ channels: [], sampleRate: SR })).toThrow(
      /at least one channel/,
    );
  });

  it('rejects channels that disagree on length', () => {
    const full = reverberant(1);
    expect(() =>
      masteringRepairDereverbClassicalLinked({
        channels: [full, full.slice(0, full.length - 1)],
        sampleRate: SR,
      }),
    ).toThrow(/channel lengths must match/);
  });

  it('rejects a NaN sample rate and a non-power-of-two nFft', () => {
    const channels = [reverberant(1), reverberant(7)];
    expect(() =>
      masteringRepairDereverbClassicalLinked({ channels, sampleRate: Number.NaN }),
    ).toThrow();
    expect(() =>
      masteringRepairDereverbClassicalLinked(channels, SR, { nFft: 1500, hopLength: 256 }),
    ).toThrow();
  });
});

describe('the two linked entries answer a short input oppositely', () => {
  beforeAll(async () => {
    await init();
  });

  it('denoise refuses a buffer shorter than nFft; dereverb pads one and succeeds', () => {
    const short = [noisyTone(440, 1).slice(0, 512), noisyTone(660, 7).slice(0, 512)];
    const options = { nFft: 1024, hopLength: 256 };

    expect(() => masteringRepairDenoiseClassicalLinked(short, SR, options)).toThrow();

    const padded = masteringRepairDereverbClassicalLinked(short, SR, options);
    expect(padded.channels).toHaveLength(2);
    // Padded for ANALYSIS only: the outputs come back at the input length.
    expect(padded.channels[0]?.length).toBe(512);
    expect(padded.channels[1]?.length).toBe(512);

    // The control for the refusal above: the same denoise call over a buffer
    // that does reach nFft succeeds, so the throw is about the length.
    expect(() =>
      masteringRepairDenoiseClassicalLinked([noisyTone(440, 1), noisyTone(660, 7)], SR, options),
    ).not.toThrow();
  });
});
