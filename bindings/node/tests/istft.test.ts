import { describe, expect, it } from 'vitest';
import { istft, type Matrix2D, stft } from '../src/index.js';

const sampleRate = 22050;
const nFft = 512;
const hopLength = 128;
const samples = new Float32Array(2048).map((_, i) => 0.8 * Math.sin((2 * Math.PI * 7 * i) / nFft));

function phaseStft() {
  return stft({ samples, sampleRate, nFft, hopLength, phase: true });
}

describe('inverse STFT', () => {
  it('names a missing plane and refuses a surplus argument', () => {
    const call = istft as (...args: unknown[]) => unknown;
    const plane = { data: new Float32Array(6), rows: 3, cols: 2 };
    const cases: [unknown[], string][] = [
      [[{ phase: plane }], 'magnitude is required'],
      [[{ magnitude: plane, phase: null }], 'phase is required'],
      [[{ magnitude: plane, phase: plane }, 1], 'unexpected argument after the request object'],
    ];
    for (const [args, message] of cases) {
      expect(() => call(...args)).toThrow(TypeError);
      expect(() => call(...args)).toThrow(new TypeError(`istft: ${message}`));
    }
  });

  it('reconstructs the signal with inferred FFT size and quarter hop', () => {
    const transformed = phaseStft();
    const reconstructed = istft({
      magnitude: transformed.magnitude,
      phase: transformed.phase as Matrix2D,
    });
    expect(reconstructed).toHaveLength(samples.length);

    const scale = Math.max(...samples.map(Math.abs));
    const tolerance = scale * 5e-5;
    for (let i = 0; i < samples.length; i += 1) {
      expect(Math.abs(reconstructed[i] - samples[i])).toBeLessThan(tolerance);
    }
  });

  it('accepts a shorter output length and refuses an explicit zero', () => {
    const transformed = phaseStft();
    const request = {
      magnitude: transformed.magnitude,
      phase: transformed.phase as Matrix2D,
      nFft,
      hopLength,
    };
    expect(istft({ ...request, lengthSamples: 1 })).toHaveLength(1);
    expect(() => istft({ ...request, lengthSamples: 0 })).toThrow(/lengthSamples/);
  });

  it('leaves the COLA pair and output cap to the native validator', () => {
    const transformed = phaseStft();
    const request = {
      magnitude: transformed.magnitude,
      phase: transformed.phase as Matrix2D,
      nFft,
    };
    expect(() => istft({ ...request, hopLength: 257 })).toThrow(/nFft.*hopLength|hopLength.*nFft/);
    expect(() => istft({ ...request, nFft: 256, hopLength: 64 })).toThrow(RangeError);
    expect(() => istft({ ...request, hopLength, lengthSamples: 500_000_001 })).toThrow(RangeError);
  });

  it('refuses matrices whose shapes do not match', () => {
    const transformed = phaseStft();
    const phase = transformed.phase as Matrix2D;
    const reshaped: Matrix2D = {
      data: phase.data,
      rows: phase.data.length,
      cols: 1,
    };
    expect(() =>
      istft({ magnitude: transformed.magnitude, phase: reshaped, nFft, hopLength }),
    ).toThrow(/magnitude and phase must have the same shape/);
    expect(() =>
      istft({
        magnitude: transformed.magnitude,
        phase: { ...phase, data: phase.data.subarray(1) },
      }),
    ).toThrow(RangeError);
  });
});
