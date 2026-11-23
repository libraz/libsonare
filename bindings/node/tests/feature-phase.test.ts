import { describe, expect, it } from 'vitest';
import { stft } from '../src/index.js';

const sampleRate = 22050;
const samples = new Float32Array(1024).map(
  (_, i) => 0.4 * Math.sin((2 * Math.PI * 440 * i) / sampleRate),
);

describe('STFT phase output', () => {
  it('returns a phase matrix only when requested', () => {
    const without = stft({ samples, sampleRate, nFft: 256, hopLength: 64 });
    expect(Object.hasOwn(without, 'phase')).toBe(false);

    const withPhase = stft({ samples, sampleRate, nFft: 256, hopLength: 64, phase: true });
    expect(withPhase.phase?.rows).toBe(129);
    expect(withPhase.phase?.cols).toBe(withPhase.magnitude.cols);
    expect(withPhase.phase?.data).toHaveLength(withPhase.magnitude.data.length);
  });

  it('requires phase to be a boolean', () => {
    expect(() => stft({ samples, sampleRate, phase: 'yes' as unknown as boolean })).toThrow(
      TypeError,
    );
  });
});
