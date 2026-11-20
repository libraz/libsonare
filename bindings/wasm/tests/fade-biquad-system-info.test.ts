/**
 * `fade`, `biquadFilter` and `systemInfo` on the WASM surface, over the
 * sonare_fade, sonare_biquad_filter and sonare_system_info_json C entry points.
 */

import { beforeAll, describe, expect, it } from 'vitest';
import { biquadFilter, fade, init, systemInfo } from '../dist/index.js';
import { expectRefusalOf, sine, unknownKeyText } from './_helpers';

const sampleRate = 22050;

function rms(samples: Float32Array): number {
  let sum = 0;
  for (const v of samples) sum += v * v;
  return Math.sqrt(sum / samples.length);
}

describe('fade', () => {
  beforeAll(async () => {
    await init();
  });

  it('ramps both ends of a constant signal and leaves the middle', () => {
    const samples = new Float32Array(sampleRate).fill(1);
    const out = fade({ samples, sampleRate, fadeInSec: 0.1, fadeOutSec: 0.1 });
    expect(out.length).toBe(samples.length);
    expect(out[0]).toBeCloseTo(0, 3);
    expect(out[out.length - 1]).toBeCloseTo(0, 2);
    expect(out[out.length >> 1]).toBeCloseTo(1, 5);
  });

  it('defaults both lengths to 0 and returns the input unchanged', () => {
    const samples = new Float32Array(1000).fill(1);
    const out = fade({ samples, sampleRate });
    expect(Array.from(out)).toEqual(Array.from(samples));
  });

  it('refuses a negative length with a RangeError', () => {
    const samples = new Float32Array(1000).fill(1);
    const caught = expectRefusalOf(() => fade({ samples, sampleRate, fadeInSec: -1 }), RangeError);
    expect(caught.message).toContain('fadeInSec must be');
  });

  it('refuses an unknown key with a TypeError', () => {
    const samples = new Float32Array(1000).fill(1);
    const caught = expectRefusalOf(
      () => fade({ samples, sampleRate, fadeSec: 1 } as never),
      TypeError,
    );
    expect(caught.message).toMatch(unknownKeyText('fade: unknown key fadeSec'));
  });
});

describe('biquadFilter', () => {
  beforeAll(async () => {
    await init();
  });

  const tone = sine(50, 1, { sampleRate });

  it('high-pass at 1000 Hz removes a 50 Hz sine', () => {
    const out = biquadFilter({ samples: tone, sampleRate, type: 'highPass', cutoffHz: 1000 });
    expect(rms(out.subarray(sampleRate >> 2)) / rms(tone)).toBeLessThan(0.1);
  });

  it('low-pass at 1000 Hz passes a 50 Hz sine', () => {
    const out = biquadFilter({ samples: tone, sampleRate, type: 'lowPass', cutoffHz: 1000 });
    expect(
      rms(out.subarray(sampleRate >> 2)) / rms(tone.subarray(sampleRate >> 2)),
    ).toBeGreaterThan(0.9);
  });

  it('refuses a cutoff the band response does not read', () => {
    const caught = expectRefusalOf(
      () =>
        biquadFilter({
          samples: tone,
          sampleRate,
          type: 'bandPass',
          cutoffHz: 200,
          centerHz: 200,
          bandwidthHz: 50,
        }),
      RangeError,
    );
    expect(caught.message).toBe(
      'biquadFilter: cutoffHz does not apply when type is bandPass or notch',
    );
  });

  it('refuses order 4 for a band response', () => {
    const caught = expectRefusalOf(
      () =>
        biquadFilter({
          samples: tone,
          sampleRate,
          type: 'bandPass',
          order: 4,
          centerHz: 200,
          bandwidthHz: 50,
        }),
      RangeError,
    );
    expect(caught.message).toBe('biquadFilter: order must be 2 when type is bandPass or notch');
  });

  it('refuses an unknown type with a RangeError naming the tokens', () => {
    const caught = expectRefusalOf(
      () => biquadFilter({ samples: tone, sampleRate, type: 'foo' as never }),
      RangeError,
    );
    expect(caught.message).toBe(
      'biquadFilter: type must be one of highPass, lowPass, bandPass, notch',
    );
  });

  it('refuses an unknown key with a TypeError', () => {
    const caught = expectRefusalOf(
      () => biquadFilter({ samples: tone, sampleRate, type: 'lowPass', cutoff: 1 } as never),
      TypeError,
    );
    expect(caught.message).toMatch(unknownKeyText('biquadFilter: unknown key cutoff'));
  });
});

describe('systemInfo', () => {
  beforeAll(async () => {
    await init();
  });

  it('reports the parallel section and only facts the platform gave', () => {
    const info = systemInfo();
    expect(typeof info.parallel.enabled).toBe('boolean');
    expect(Number.isInteger(info.parallel.workers)).toBe(true);
    expect(info.parallel.workers).toBeGreaterThanOrEqual(1);
    expect(typeof info.parallel.strategy).toBe('string');
    for (const value of [
      info.cpu.logicalCores,
      info.cpu.physicalCores,
      info.memory.totalGb,
      info.memory.availableGb,
    ]) {
      expect(value === undefined || value > 0).toBe(true);
    }
  });
});
