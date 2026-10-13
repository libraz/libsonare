/**
 * The mixer accepts at most one input pair per strip; missing strips are silent.
 * Both process entry points follow the same rule and name both counts on refusal.
 */

import { beforeAll, describe, expect, it } from 'vitest';
import { init, Mixer, mixingScenePresetJson } from '../dist/index.js';

const SR = 48000;
const BLOCK = 128;

function peak(channel: Float32Array): number {
  return channel.reduce((max, sample) => Math.max(max, Math.abs(sample)), 0);
}

function inputs(count: number): Float32Array[] {
  return Array.from({ length: count }, () => new Float32Array(BLOCK).fill(0.25));
}

describe('Mixer input count (WASM)', () => {
  let mixer: Mixer;
  let strips: number;

  beforeAll(async () => {
    await init();
    mixer = Mixer.fromSceneJson(mixingScenePresetJson('commentaryDucking'), SR, BLOCK);
    strips = mixer.stripCount();
  });

  it('processStereo takes fewer, equal and refuses more inputs than strips', () => {
    expect(strips).toBeGreaterThan(1);
    expect(peak(mixer.processStereo([], []).left)).toBe(0);
    expect(() => mixer.processStereo(inputs(1), inputs(1))).not.toThrow();
    expect(() => mixer.processStereo(inputs(strips), inputs(strips))).not.toThrow();
    expect(() => mixer.processStereo(inputs(strips + 1), inputs(strips + 1))).toThrow(
      new RangeError(`got ${strips + 1} input channels for a ${strips}-strip mixer`),
    );
  });

  it('processStereoInto follows the same rule', () => {
    const outL = new Float32Array(BLOCK);
    const outR = new Float32Array(BLOCK);
    expect(() => mixer.processStereoInto(inputs(1), inputs(1), outL, outR)).not.toThrow();
    expect(() => mixer.processStereoInto(inputs(strips), inputs(strips), outL, outR)).not.toThrow();
    expect(() =>
      mixer.processStereoInto(inputs(strips + 1), inputs(strips + 1), outL, outR),
    ).toThrow(new RangeError(`got ${strips + 1} input channels for a ${strips}-strip mixer`));
  });

  it('a missing strip is silent: fewer inputs match explicit silence on the rest', () => {
    const fresh = () => Mixer.fromSceneJson(mixingScenePresetJson('commentaryDucking'), SR, BLOCK);
    const silent = () => Array.from({ length: strips - 1 }, () => new Float32Array(BLOCK));
    const a = fresh();
    const b = fresh();
    const partial = a.processStereo(inputs(1), inputs(1));
    const padded = b.processStereo([...inputs(1), ...silent()], [...inputs(1), ...silent()]);
    expect(peak(partial.left)).toBeGreaterThan(0);
    expect(Array.from(partial.left)).toEqual(Array.from(padded.left));
    a.destroy();
    b.destroy();
  });
});
