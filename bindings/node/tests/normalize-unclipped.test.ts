import { describe, expect, it } from 'vitest';
import { normalize, normalizeStereo } from '../src/index.js';

const SR = 22050;
const LENGTH = 4096;

const db = (linear: number): number => 20 * Math.log10(linear);

function peak(samples: Float32Array): number {
  let found = 0;
  for (const value of samples) found = Math.max(found, Math.abs(value));
  return found;
}

function rms(...channels: Float32Array[]): number {
  let sum = 0;
  let count = 0;
  for (const channel of channels) {
    for (const value of channel) sum += value * value;
    count += channel.length;
  }
  return Math.sqrt(sum / count);
}

/** A quiet bed with one spike, so an RMS target lifts the spike far past full scale. */
function highCrest(spike = 0.9): Float32Array {
  const out = new Float32Array(LENGTH);
  for (let i = 0; i < LENGTH; i += 1) out[i] = i % 2 === 0 ? 0.01 : -0.01;
  out[LENGTH / 2] = spike;
  return out;
}

describe('normalize returns the signal unclipped', () => {
  it('accepts a peak target above 0 dB and leaves the peak above full scale', () => {
    const out = normalize({ samples: highCrest(), sampleRate: SR, targetDb: 3 });
    expect(db(peak(out))).toBeCloseTo(3, 3);
    expect(peak(out)).toBeGreaterThan(1);
  });

  it('brings the RMS exactly to target on a high-crest input, peaks above full scale', () => {
    const out = normalize({ samples: highCrest(), sampleRate: SR, targetDb: -20, mode: 'rms' });
    expect(Math.abs(db(rms(out)) + 20)).toBeLessThan(1e-3);
    expect(peak(out)).toBeGreaterThan(1);
  });

  it('accepts an RMS target above 0 dB', () => {
    const out = normalize({ samples: highCrest(), sampleRate: SR, targetDb: 1, mode: 'rms' });
    expect(Math.abs(db(rms(out)) - 1)).toBeLessThan(1e-3);
  });
});

describe('normalizeStereo returns the pair unclipped', () => {
  it('accepts a peak target above 0 dB and leaves the louder peak above full scale', () => {
    const result = normalizeStereo({
      left: highCrest(),
      right: highCrest(0.45),
      sampleRate: SR,
      targetDb: 2,
    });
    expect(db(peak(result.left))).toBeCloseTo(2, 3);
    expect(peak(result.left)).toBeGreaterThan(1);
  });

  it('brings the joint RMS exactly to target on a high-crest pair, peaks above full scale', () => {
    const result = normalizeStereo({
      left: highCrest(),
      right: highCrest(0.45),
      sampleRate: SR,
      targetDb: -20,
      mode: 'rms',
    });
    expect(Math.abs(db(rms(result.left, result.right)) + 20)).toBeLessThan(1e-3);
    expect(peak(result.left)).toBeGreaterThan(1);
  });

  it('accepts an RMS target above 0 dB', () => {
    const result = normalizeStereo({
      left: highCrest(),
      right: highCrest(0.45),
      sampleRate: SR,
      targetDb: 1,
      mode: 'rms',
    });
    expect(Math.abs(db(rms(result.left, result.right)) - 1)).toBeLessThan(1e-3);
  });
});
