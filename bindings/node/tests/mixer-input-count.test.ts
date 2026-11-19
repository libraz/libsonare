/**
 * The mixer accepts at most one input pair per strip; missing strips are silent.
 */

import { describe, expect, it } from 'vitest';
import { Mixer, mixingScenePreset } from '../src/index.js';

const BLOCK = 128;

function inputs(count: number): Float32Array[] {
  return Array.from({ length: count }, () => new Float32Array(BLOCK).fill(0.25));
}

describe('Mixer input count (Node)', () => {
  const mixer = Mixer.fromJson(JSON.stringify(mixingScenePreset('commentaryDucking')), {
    sampleRate: 48000,
    maxBlockSize: BLOCK,
  });
  const strips = mixer.stripCount;

  it('takes fewer and equal, and refuses more inputs than strips, naming both counts', () => {
    expect(strips).toBeGreaterThan(1);
    expect(() => mixer.processStereo(inputs(1), inputs(1))).not.toThrow();
    expect(() => mixer.processStereo(inputs(strips), inputs(strips))).not.toThrow();
    expect(() => mixer.processStereo(inputs(strips + 1), inputs(strips + 1))).toThrow(
      new RangeError(
        `Mixer.processStereo: got ${strips + 1} input channels for a ${strips}-strip mixer`,
      ),
    );
  });
});

describe('Mixer block size (Node)', () => {
  it('reports the library default block size when maxBlockSize is omitted, and refuses an explicit 0', () => {
    const scene = JSON.stringify(mixingScenePreset('commentaryDucking'));
    expect(() => Mixer.fromJson(scene, { sampleRate: 48000, maxBlockSize: 0 })).toThrow(RangeError);
    const mixer = Mixer.fromJson(scene, { sampleRate: 48000 });
    try {
      const max = mixer.maxBlockSize;
      expect(max).toBeGreaterThan(0);
      expect(mixer.drainTailStereo(max).left).toHaveLength(max);
      expect(() => mixer.drainTailStereo(max + 1)).toThrow(RangeError);
    } finally {
      mixer.destroy();
    }
  });

  it('reports the given block size', () => {
    const scene = JSON.stringify(mixingScenePreset('commentaryDucking'));
    const mixer = Mixer.fromJson(scene, { sampleRate: 48000, maxBlockSize: 256 });
    try {
      expect(mixer.maxBlockSize).toBe(256);
    } finally {
      mixer.destroy();
    }
  });
});
