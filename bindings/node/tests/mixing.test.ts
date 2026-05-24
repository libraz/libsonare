import { describe, expect, it } from 'vitest';
import { mixingScenePresetJson, mixingScenePresetNames, mixStereo } from '../src/index.js';

describe('mixing native binding', () => {
  it('exposes scene presets and renders a muted stereo mix', () => {
    expect(mixingScenePresetNames()).toContain('vocalReverbSend');
    expect(mixingScenePresetJson('vocalReverbSend')).toContain('"vocal"');

    const left = new Float32Array([1, 1]);
    const right = new Float32Array([0, 0]);
    const result = mixStereo([left], [right], 48000, { muted: true });
    expect(result.left).toBeInstanceOf(Float32Array);
    expect(result.right).toBeInstanceOf(Float32Array);
    expect(Array.from(result.left)).toEqual([0, 0]);
    expect(Array.from(result.right)).toEqual([0, 0]);
    expect(result.meters).toHaveLength(1);
    expect(Number.isFinite(result.meters[0].peakDbL)).toBe(true);
    expect(typeof result.meters[0].likelyMonoCompatible).toBe('boolean');
  });
});
