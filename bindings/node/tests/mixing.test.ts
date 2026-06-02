import { describe, expect, it } from 'vitest';
import { mixingScenePresetJson, mixingScenePresetNames, mixStereo } from '../src/index.js';

describe('mixing native binding', () => {
  it('exposes scene presets and renders a stereo mix with input trim', () => {
    expect(mixingScenePresetNames()).toContain('vocalReverbSend');
    expect(mixingScenePresetJson('vocalReverbSend')).toContain('"vocal"');

    const left = new Float32Array([1, 1]);
    const right = new Float32Array([0, 0]);
    const result = mixStereo([left], [right], 48000, { inputTrimDb: 6.0206, faderDb: -6.0206 });
    expect(result.left).toBeInstanceOf(Float32Array);
    expect(result.right).toBeInstanceOf(Float32Array);
    // +6.02 dB trim and -6.02 dB fader cancel to unity. With the Balance pan law
    // no longer attenuating a centered signal by 3 dB, the output passes through
    // at unity instead of sqrt(0.5).
    expect(result.left[0]).toBeCloseTo(1.0, 2);
    expect(result.left[1]).toBeCloseTo(1.0, 2);
    expect(Array.from(result.right)).toEqual([0, 0]);
    expect(result.meters).toHaveLength(1);
    expect(Number.isFinite(result.meters[0].peakDbL)).toBe(true);
    expect(typeof result.meters[0].likelyMonoCompatible).toBe('boolean');
  });
});
