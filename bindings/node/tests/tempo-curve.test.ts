import { describe, expect, it } from 'vitest';
import { analyze } from '../src/index.js';

const SR = 22050;

/** A click track whose tempo sweeps linearly from bpm0 to bpm1. */
function sweepingClicks(bpm0: number, bpm1: number, duration: number, sr = SR): Float32Array {
  const n = Math.floor(sr * duration);
  const samples = new Float32Array(n);
  const clickLength = Math.floor(sr / 100);
  let t = 0;
  while (t < duration) {
    const start = Math.floor(t * sr);
    for (let i = 0; i < clickLength && start + i < n; i++) {
      samples[start + i] = (1 - i / clickLength) * 0.9;
    }
    t += 60 / (bpm0 + (bpm1 - bpm0) * (t / duration));
  }
  return samples;
}

const span = (curve: number[]): number => {
  const min = Math.min(...curve);
  return (Math.max(...curve) - min) / min;
};

describe('analyze() tempo curve', () => {
  it('withholds the curve until it is asked for', () => {
    const samples = sweepingClicks(120, 120, 6);

    const without = analyze(samples, SR);
    expect(Array.isArray(without.beatLocalBpm)).toBe(true);
    expect(without.beatLocalBpm).toHaveLength(0);

    const with_ = analyze(samples, SR, { computeTempoCurve: true });
    expect(with_.beatLocalBpm).toHaveLength(with_.beats.length);
    expect(with_.beatLocalBpm.length).toBeGreaterThan(0);
    for (const bpm of with_.beatLocalBpm) {
      expect(Number.isFinite(bpm)).toBe(true);
      expect(bpm).toBeGreaterThan(0);
    }
  });

  it('adds an output without moving the analysis under it', () => {
    const samples = sweepingClicks(120, 120, 6);
    const without = analyze(samples, SR);
    const with_ = analyze(samples, SR, { computeTempoCurve: true });

    expect(with_.bpm).toBe(without.bpm);
    expect(with_.beats.length).toBe(without.beats.length);
    expect(with_.timeSignature).toEqual(without.timeSignature);
  });

  it('follows a sweeping tempo only when beat tracking follows it too', () => {
    // The curve is faithful to the beat grid it was decoded from, so with beat
    // tracking holding a fixed prior it reports a nearly flat tempo on material
    // that is not. That is the trap the option documents; pin it as behaviour
    // rather than leaving it to the prose.
    const samples = sweepingClicks(90, 150, 25);

    const held = analyze(samples, SR, { computeTempoCurve: true });
    const tracked = analyze(samples, SR, { computeTempoCurve: true, adaptiveTempo: true });

    expect(span(tracked.beatLocalBpm)).toBeGreaterThan(0.25);
    expect(span(tracked.beatLocalBpm)).toBeGreaterThan(span(held.beatLocalBpm) * 2);
    expect(tracked.beatLocalBpm[0]).toBeGreaterThan(80);
    expect(tracked.beatLocalBpm[0]).toBeLessThan(105);
    expect(tracked.beatLocalBpm[tracked.beatLocalBpm.length - 1]).toBeGreaterThan(125);
  });
});
