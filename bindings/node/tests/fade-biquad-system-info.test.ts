import { describe, expect, it } from 'vitest';
import { biquadFilter, fade, systemInfo } from '../src/index.js';

const SR = 48000;

function sine(freq: number, length: number): Float32Array {
  const out = new Float32Array(length);
  for (let i = 0; i < length; i++) {
    out[i] = Math.sin((2 * Math.PI * freq * i) / SR);
  }
  return out;
}

function rms(x: Float32Array, from = 0, to = x.length): number {
  let sum = 0;
  for (let i = from; i < to; i++) {
    sum += x[i] * x[i];
  }
  return Math.sqrt(sum / (to - from));
}

describe('fade', () => {
  const ones = new Float32Array(SR).fill(1);

  it('ramps in from silence and leaves the middle untouched', () => {
    const out = fade({ samples: ones, sampleRate: SR, fadeInSec: 0.1 });
    expect(out.length).toBe(ones.length);
    expect(Math.abs(out[0])).toBeLessThan(1e-3);
    expect(out[Math.floor(SR * 0.05)]).toBeGreaterThan(0.3);
    expect(out[Math.floor(SR * 0.05)]).toBeLessThan(0.7);
    expect(out[SR / 2]).toBeCloseTo(1, 5);
    expect(out[SR - 1]).toBeCloseTo(1, 5);
  });

  it('ramps out to silence at the end', () => {
    const out = fade({ samples: ones, sampleRate: SR, fadeOutSec: 0.1 });
    expect(out[0]).toBeCloseTo(1, 5);
    expect(out[SR / 2]).toBeCloseTo(1, 5);
    expect(Math.abs(out[SR - 1])).toBeLessThan(1e-3);
  });

  it('defaults both lengths to zero', () => {
    const out = fade({ samples: ones, sampleRate: SR });
    expect(Array.from(out)).toEqual(Array.from(ones));
  });

  it('refuses a negative length', () => {
    expect(() => fade({ samples: ones, sampleRate: SR, fadeInSec: -1 })).toThrow(RangeError);
    expect(() => fade({ samples: ones, sampleRate: SR, fadeInSec: -1 })).toThrow(
      'fadeInSec must be',
    );
  });

  it('refuses an unknown key and a positional call', () => {
    expect(() => fade({ samples: ones, sampleRate: SR, fadeSec: 1 } as never)).toThrow(TypeError);
    expect(() => (fade as (...args: unknown[]) => unknown)(ones, SR)).toThrow(TypeError);
  });
});

describe('biquadFilter', () => {
  const low = sine(50, SR);
  const tail = [SR / 2, SR] as const;

  it('attenuates a 50 Hz sine with a 1 kHz high-pass', () => {
    const out = biquadFilter({ samples: low, sampleRate: SR, type: 'highPass', cutoffHz: 1000 });
    expect(rms(out, ...tail) / rms(low, ...tail)).toBeLessThan(0.1);
  });

  it('passes a 50 Hz sine through a 1 kHz low-pass', () => {
    const out = biquadFilter({ samples: low, sampleRate: SR, type: 'lowPass', cutoffHz: 1000 });
    expect(rms(out, ...tail) / rms(low, ...tail)).toBeGreaterThan(0.9);
  });

  it('accepts order 4, zero phase and a band-pass', () => {
    expect(
      biquadFilter({
        samples: low,
        sampleRate: SR,
        type: 'highPass',
        cutoffHz: 1000,
        order: 4,
        zeroPhase: true,
      }).length,
    ).toBe(low.length);
    expect(
      biquadFilter({
        samples: low,
        sampleRate: SR,
        type: 'notch',
        centerHz: 1000,
        bandwidthHz: 100,
      }).length,
    ).toBe(low.length);
  });

  it('refuses a frequency field the type does not read, naming it as the caller spelled it', () => {
    const request = {
      samples: low,
      sampleRate: SR,
      type: 'bandPass',
      centerHz: 1000,
      bandwidthHz: 100,
      cutoffHz: 200,
    } as const;
    expect(() => biquadFilter(request)).toThrow(RangeError);
    expect(() => biquadFilter(request)).toThrow(
      'biquadFilter: cutoffHz does not apply when type is bandPass or notch',
    );
  });

  it('refuses order 4 for a band-pass', () => {
    expect(() =>
      biquadFilter({
        samples: low,
        sampleRate: SR,
        type: 'bandPass',
        centerHz: 1000,
        bandwidthHz: 100,
        order: 4,
      }),
    ).toThrow('biquadFilter: order must be 2 when type is bandPass or notch');
  });

  it('refuses a missing frequency the type reads', () => {
    expect(() => biquadFilter({ samples: low, sampleRate: SR, type: 'lowPass' })).toThrow(
      RangeError,
    );
  });

  it('refuses an unknown type, a non-boolean zeroPhase and an unknown key', () => {
    expect(() =>
      biquadFilter({ samples: low, sampleRate: SR, type: 'foo' as never, cutoffHz: 1000 }),
    ).toThrow('type must be one of highPass, lowPass, bandPass, notch');
    expect(() =>
      biquadFilter({
        samples: low,
        sampleRate: SR,
        type: 'lowPass',
        cutoffHz: 1000,
        zeroPhase: 1 as never,
      }),
    ).toThrow(TypeError);
    expect(() =>
      biquadFilter({ samples: low, sampleRate: SR, type: 'lowPass', cutoff: 1000 } as never),
    ).toThrow(TypeError);
  });
});

describe('systemInfo', () => {
  it('reports the parallel facts and any core counts the platform gives', () => {
    const info = systemInfo();
    expect(Number.isInteger(info.parallel.workers)).toBe(true);
    expect(info.parallel.workers).toBeGreaterThanOrEqual(1);
    expect(typeof info.parallel.enabled).toBe('boolean');
    expect(typeof info.parallel.strategy).toBe('string');
    if (info.cpu.logicalCores !== undefined) {
      expect(Number.isInteger(info.cpu.logicalCores)).toBe(true);
      expect(info.cpu.logicalCores).toBeGreaterThanOrEqual(1);
    }
    if (info.memory.totalGb !== undefined) {
      expect(info.memory.totalGb).toBeGreaterThan(0);
    }
  });
});
