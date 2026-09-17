import { beforeAll, describe, expect, it } from 'vitest';
import { init, normalize, normalizeStereo } from '../src/index';

const sampleRate = 22050;

function sine(amplitude: number, length = 8192): Float32Array {
  const samples = new Float32Array(length);
  for (let i = 0; i < samples.length; i++) {
    samples[i] = amplitude * Math.sin((2 * Math.PI * 440 * i) / sampleRate);
  }
  return samples;
}

function peakDb(samples: Float32Array): number {
  let peak = 0;
  for (const value of samples) {
    peak = Math.max(peak, Math.abs(value));
  }
  return 20 * Math.log10(peak);
}

function rms(samples: Float32Array): number {
  let sum = 0;
  for (const value of samples) {
    sum += value * value;
  }
  return Math.sqrt(sum / samples.length);
}

describe('normalizeStereo', () => {
  beforeAll(async () => {
    await init();
  });

  // 12 dB apart, so a shared gain and a per-channel gain leave the quiet side in
  // two places that no tolerance can confuse.
  const left = sine(0.5);
  const right = sine(0.125);

  it('moves both channels by a single gain', () => {
    const before = peakDb(left) - peakDb(right);
    expect(before).toBeCloseTo(12, 1);

    const result = normalizeStereo({ left, right, sampleRate, targetDb: -1 });

    expect(peakDb(result.left)).toBeCloseTo(-1, 1);
    expect(peakDb(result.left) - peakDb(result.right)).toBeCloseTo(before, 2);
    expect(result.appliedGainDb).toBeCloseTo(-1 - peakDb(left), 1);

    // Control: this is where a per-channel gain would put the quiet side, so the
    // assertions above can only hold for one of the two.
    const alone = normalize({ samples: right, sampleRate, targetDb: -1 });
    expect(peakDb(alone)).toBeCloseTo(-1, 1);
    expect(peakDb(result.right)).toBeLessThan(peakDb(alone) - 10);
  });

  it('measures the two channels together in rms mode', () => {
    const result = normalizeStereo({ left, right, sampleRate, targetDb: -20, mode: 'rms' });

    const l = rms(result.left);
    const r = rms(result.right);
    const jointDb = 20 * Math.log10(Math.sqrt((l * l + r * r) / 2));
    expect(jointDb).toBeCloseTo(-20, 1);

    // Control: neither channel lands on the target by itself, so the figure
    // driven there is the joint one.
    expect(Math.abs(20 * Math.log10(l) + 20)).toBeGreaterThan(1);
    expect(Math.abs(20 * Math.log10(r) + 20)).toBeGreaterThan(1);
  });

  it('refuses a pair it cannot process', () => {
    expect(() =>
      normalizeStereo({ left, right: sine(0.125, 4096), sampleRate, targetDb: -1 }),
    ).toThrow(RangeError);
    expect(() =>
      normalizeStereo({ left: new Float32Array(0), right, sampleRate, targetDb: -1 }),
    ).toThrow();

    // Control: the same pair with neither fault is accepted, so the refusals are
    // about the pair rather than about the entry point.
    expect(() => normalizeStereo({ left, right, sampleRate, targetDb: -1 })).not.toThrow();
  });

  it('names itself when refusing a mode it does not know', () => {
    expect(() =>
      // @ts-expect-error -- the refusal is the point; a caller could reach it from JS
      normalizeStereo({ left, right, sampleRate, mode: 'loudness' }),
    ).toThrow(/normalizeStereo: mode/);
  });

  it('leaves a silent pair alone', () => {
    const silence = new Float32Array(1024);
    const result = normalizeStereo({ left: silence, right: silence, sampleRate, targetDb: -1 });

    expect(result.appliedGainDb).toBe(0);
    expect(result.left[512]).toBe(0);

    // Control: a quiet-but-not-silent pair does move, so the assertion above is
    // about the silence short-circuit rather than about a no-op.
    const quiet = sine(0.01);
    expect(
      normalizeStereo({ left: quiet, right: quiet, sampleRate, targetDb: -1 }).appliedGainDb,
    ).toBeGreaterThan(10);
  });
});

describe('normalizeStereo default target', () => {
  beforeAll(async () => {
    await init();
  });

  const left = sine(0.5);
  const right = sine(0.125);

  it('defaults rms mode to -20 dB rather than to the peak default of 0', () => {
    const omitted = normalizeStereo({ left, right, sampleRate, mode: 'rms' });
    const atMinus20 = normalizeStereo({ left, right, sampleRate, mode: 'rms', targetDb: -20 });
    expect(omitted.appliedGainDb).toBeCloseTo(atMinus20.appliedGainDb, 6);

    // Control: 0 is the other candidate — it is what the mono `normalize` on
    // this surface defaults to in both modes — and it is 20 dB away, so this
    // assertion distinguishes the two rather than observing that some gain was
    // applied.
    const atZero = normalizeStereo({ left, right, sampleRate, mode: 'rms', targetDb: 0 });
    expect(atZero.appliedGainDb - omitted.appliedGainDb).toBeCloseTo(20, 6);
  });

  it('still defaults peak mode to 0 dB', () => {
    const omitted = normalizeStereo({ left, right, sampleRate });
    const atZero = normalizeStereo({ left, right, sampleRate, targetDb: 0 });
    expect(omitted.appliedGainDb).toBeCloseTo(atZero.appliedGainDb, 6);
  });
});
