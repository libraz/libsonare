import { beforeEach, describe, expect, it, vi } from 'vitest';
import {
  clearScopeCache,
  computePeaks,
  computeSpectrogram,
  levelDb,
  peaksFor,
  rampFor,
  sampleRamp,
  specFor,
} from '../../src/scope/compute';
import { FFT_SIZE, FLOOR_DB } from '../../src/scope/constants';
import { pcm, sine } from './fakes';

beforeEach(() => clearScopeCache());

describe('peaks', () => {
  it('takes the extremes of each column', () => {
    const data = new Float32Array([0.1, -0.5, 0.9, 0.2, -0.3, 0.4, 0, 0]);
    const p = computePeaks(pcm([data]), 4);
    expect(Array.from(p.hi).map((v) => +v.toFixed(2))).toEqual([0.1, 0.9, 0.4, 0]);
    expect(Array.from(p.lo).map((v) => +v.toFixed(2))).toEqual([-0.5, 0, -0.3, 0]);
  });

  it('averages the two channels', () => {
    const p = computePeaks(pcm([new Float32Array([1, 1]), new Float32Array([-1, 0.5])]), 1);
    expect(p.hi[0]).toBeCloseTo(0.75);
    expect(p.lo[0]).toBe(0);
  });

  it('is flat for silence', () => {
    const p = computePeaks(pcm([new Float32Array(100)]), 10);
    expect(Math.max(...p.hi)).toBe(0);
    expect(Math.min(...p.lo)).toBe(0);
  });

  it('follows a sine amplitude', () => {
    const p = computePeaks(pcm([sine(50, 1, 8000, 0.5)]), 8);
    for (let i = 0; i < 8; i++) {
      expect(p.hi[i]).toBeCloseTo(0.5, 1);
      expect(p.lo[i]).toBeCloseTo(-0.5, 1);
    }
  });
});

describe('spectrogram', () => {
  const peakBin = (mag: Float32Array, frame: number, bins: number) => {
    let best = 0;
    for (let b = 1; b < bins; b++) {
      if ((mag[frame * bins + b] as number) > (mag[frame * bins + best] as number)) {
        best = b;
      }
    }
    return best;
  };

  it('puts a sine in the bin of its frequency', () => {
    const rate = 8000;
    const bin = 64;
    const freq = (bin * rate) / FFT_SIZE;
    const s = computeSpectrogram(pcm([sine(freq, 1, rate)], rate), 10);
    expect(s.bins).toBe(FFT_SIZE / 2);
    expect(s.sampleRate).toBe(rate);
    for (let f = 0; f < s.frames; f++) {
      expect(peakBin(s.mag, f, s.bins)).toBe(bin);
    }
  });

  it('reads a full-scale sine near 0 dB', () => {
    const rate = 8000;
    const freq = (64 * rate) / FFT_SIZE;
    const s = computeSpectrogram(pcm([sine(freq, 1, rate, 1)], rate), 4);
    expect(levelDb(s.mag[64] as number)).toBeGreaterThan(-3);
    expect(levelDb(s.mag[64] as number)).toBeLessThan(3);
  });

  it('sits at the floor for silence', () => {
    const s = computeSpectrogram(pcm([new Float32Array(8000)]), 4);
    expect(Math.max(...s.mag)).toBe(0);
    expect(levelDb(0)).toBeLessThan(FLOOR_DB);
  });

  it('survives a buffer shorter than the window', () => {
    const s = computeSpectrogram(pcm([new Float32Array(100)]), 4);
    expect(s.frames).toBe(1);
  });

  it('thins frames toward the requested columns', () => {
    const s = computeSpectrogram(pcm([sine(440, 4, 8000)]), 10);
    expect(s.frames).toBeLessThanOrEqual(10 * 2);
  });
});

describe('cache', () => {
  it('computes once per epoch, key and width', () => {
    const buf = pcm([sine(100, 0.5)]);
    const spy = vi.spyOn(buf, 'getChannelData');
    const a = peaksFor(1, 'm', buf, 20);
    const calls = spy.mock.calls.length;
    expect(peaksFor(1, 'm', buf, 20)).toBe(a);
    expect(spy.mock.calls.length).toBe(calls);
    expect(peaksFor(1, 'm', buf, 30)).not.toBe(a);
    expect(peaksFor(1, 'r', buf, 20)).not.toBe(a);
  });

  it('recomputes under a new epoch', () => {
    const buf = pcm([sine(100, 0.5)]);
    const a = peaksFor(1, 'm', buf, 20);
    expect(peaksFor(2, 'm', buf, 20)).not.toBe(a);
  });

  it('caches spectrograms the same way', () => {
    const buf = pcm([sine(100, 0.5)]);
    const a = specFor(1, 'm', buf, 5);
    expect(specFor(1, 'm', buf, 5)).toBe(a);
    expect(specFor(2, 'm', buf, 5)).not.toBe(a);
  });
});

describe('ramp', () => {
  const rgb: [number, number, number] = [100, 200, 50];

  it('runs dark, role colour, near-white', () => {
    const stops = rampFor(rgb);
    expect(sampleRamp(stops, 0)).toEqual([18, 20, 24]);
    expect(sampleRamp(stops, 0.62)).toEqual(rgb);
    expect(sampleRamp(stops, 1)).toEqual([246, 242, 236]);
  });

  it('interpolates between stops', () => {
    const [r] = sampleRamp(rampFor(rgb), 0.31);
    expect(r).toBeGreaterThan(18);
    expect(r).toBeLessThan(100);
  });
});
