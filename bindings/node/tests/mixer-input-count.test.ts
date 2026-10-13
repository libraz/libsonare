/**
 * The mixer accepts at most one input pair per strip; missing strips are silent.
 */

import { describe, expect, it } from 'vitest';
import { Mixer, mixingScenePresetJson } from '../src/index.js';

const BLOCK = 128;

function inputs(count: number): Float32Array[] {
  return Array.from({ length: count }, () => new Float32Array(BLOCK).fill(0.25));
}

describe('Mixer input count (Node)', () => {
  const mixer = Mixer.fromSceneJson(mixingScenePresetJson('commentaryDucking'), 48000, BLOCK);
  const strips = mixer.stripCount();

  it('takes fewer and equal, and refuses more inputs than strips, naming both counts', () => {
    expect(strips).toBeGreaterThan(1);
    expect(() => mixer.processStereo(inputs(1), inputs(1))).not.toThrow();
    expect(() => mixer.processStereo(inputs(strips), inputs(strips))).not.toThrow();
    expect(() => mixer.processStereo(inputs(strips + 1), inputs(strips + 1))).toThrow(
      new RangeError(`got ${strips + 1} input channels for a ${strips}-strip mixer`),
    );
  });
});
