/**
 * Bounce content, loudness-target and dither behaviour of the offline export.
 *
 * A bounce result's shape (frames, channels, sample rate, length) is silent on
 * whether the scheduled material reached the export, whether a requested
 * loudness target was applied, and whether the requested dither ran. This
 * surface also maps the dither type integer in its own wrapper rather than
 * through the C ABI, so each type is identified here by what it does to the
 * signal.
 */

import { beforeAll, describe, expect, it } from 'vitest';
import type { EngineBounceOptions } from '../dist/index.js';
import { init, RealtimeEngine } from '../dist/index.js';

const SAMPLE_RATE = 48000;
const BLOCK_SIZE = 128;
const FRAMES = 256;
const AMPLITUDE = 0.5;
// Depth at which dither is unmistakable in a float32 buffer: one LSB is 1/128,
// far above the float resolution of a 0.5-amplitude sample.
const DITHER_BITS = 8;
const LSB = 1 / (1 << (DITHER_BITS - 1));

describe('RealtimeEngine offline bounce content', () => {
  beforeAll(async () => {
    await init();
  });

  const tone = (): Float32Array => {
    const out = new Float32Array(FRAMES);
    for (let i = 0; i < FRAMES; i++) {
      out[i] = AMPLITUDE * Math.sin((2 * Math.PI * 440 * i) / SAMPLE_RATE);
    }
    return out;
  };

  const bounce = (
    options: Partial<EngineBounceOptions> = {},
  ): { samples: Float32Array; lufs: number } => {
    const engine = new RealtimeEngine(SAMPLE_RATE, BLOCK_SIZE);
    const channel = tone();
    engine.setClips([
      { id: 1, channels: [channel, channel.slice()], startPpq: 0, lengthSamples: FRAMES },
    ]);
    engine.play();
    const result = engine.bounceOffline({
      totalFrames: FRAMES,
      blockSize: BLOCK_SIZE,
      numChannels: 2,
      sourceSampleRate: SAMPLE_RATE,
      targetSampleRate: SAMPLE_RATE,
      ...options,
    });
    engine.destroy();
    return { samples: result.interleaved, lufs: result.integratedLufs };
  };

  const peak = (samples: Float32Array): number =>
    samples.reduce((acc, sample) => Math.max(acc, Math.abs(sample)), 0);
  const maxAbsDiff = (a: Float32Array, b: Float32Array): number =>
    a.reduce((acc, sample, index) => Math.max(acc, Math.abs(sample - b[index])), 0);
  const onGrid = (samples: Float32Array): number =>
    samples.reduce(
      (acc, sample) => acc + (Math.abs(sample / LSB - Math.round(sample / LSB)) < 1e-4 ? 1 : 0),
      0,
    );

  it('exports the scheduled clip content', () => {
    const { samples, lufs } = bounce();
    expect(samples.length).toBe(FRAMES * 2);
    // A bounce that rendered silence would satisfy every shape assertion, and
    // would report -Infinity here rather than a real loudness.
    expect(peak(samples)).toBeCloseTo(AMPLITUDE, 3);
    expect(samples.some((sample) => sample !== 0)).toBe(true);
    expect(Number.isFinite(lufs)).toBe(true);
  });

  it('normalizes to the requested loudness and honours the default sentinel', () => {
    for (const targetLufs of [-20, -9]) {
      expect(bounce({ normalizeLufs: true, targetLufs }).lufs).toBeCloseTo(targetLufs, 1);
    }
    // 0 is the documented "use default" sentinel, not a literal 0 LUFS target.
    expect(bounce({ normalizeLufs: true, targetLufs: 0 }).lufs).toBeCloseTo(-14, 1);
  });

  it('applies each dither type as documented', () => {
    const plain = bounce();
    const none = bounce({ dither: 0, ditherBits: DITHER_BITS, ditherSeed: 1 });
    const rpdf = bounce({ dither: 1, ditherBits: DITHER_BITS, ditherSeed: 1 });
    const tpdf = bounce({ dither: 2, ditherBits: DITHER_BITS, ditherSeed: 1 });
    const shaped = bounce({ dither: 3, ditherBits: DITHER_BITS, ditherSeed: 1 });

    // Identifying each type by its effect keeps a remapped integer from
    // passing: the two noise types perturb without quantizing, the triangular
    // one being the wider of the two, and only the shaped type snaps every
    // sample onto the target-depth grid.
    expect(maxAbsDiff(none.samples, plain.samples)).toBe(0);
    expect(maxAbsDiff(rpdf.samples, none.samples)).toBeGreaterThan(LSB / 4);
    expect(maxAbsDiff(tpdf.samples, none.samples)).toBeGreaterThan(
      maxAbsDiff(rpdf.samples, none.samples),
    );
    expect(onGrid(rpdf.samples)).toBeLessThan(rpdf.samples.length / 2);
    expect(onGrid(shaped.samples)).toBe(shaped.samples.length);
  });

  it('reproduces a fixed dither seed and reacts to a different one', () => {
    const first = bounce({ dither: 2, ditherBits: DITHER_BITS, ditherSeed: 1 });
    const repeat = bounce({ dither: 2, ditherBits: DITHER_BITS, ditherSeed: 1 });
    const other = bounce({ dither: 2, ditherBits: DITHER_BITS, ditherSeed: 2 });
    expect(Array.from(repeat.samples)).toEqual(Array.from(first.samples));
    expect(maxAbsDiff(other.samples, first.samples)).toBeGreaterThan(0);
  });
});
