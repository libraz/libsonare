import { describe, expect, it } from 'vitest';
import { flattenChainConfig } from '../src/_chain_config';
import type { MasteringChainConfig } from '../src/types';

describe('flattenChainConfig legacy flat aliases', () => {
  it('normalizes flat repair/eq spellings to the canonical nested keys', () => {
    // The legacy flat spelling and the canonical nested spelling must produce the
    // identical override map so the same config works on Node and WASM alike.
    const flat = flattenChainConfig({
      repair: {
        denoise: true,
        nFft: 2048,
        hopLength: 512,
        ddAlpha: 0.98,
        gainFloor: 0.1,
      },
      eq: { tiltDb: 2, pivotHz: 1000 },
    } as unknown as MasteringChainConfig);

    const nested = flattenChainConfig({
      repair: {
        denoise: {
          enabled: true,
          nFft: 2048,
          hopLength: 512,
          ddAlpha: 0.98,
          gainFloor: 0.1,
        },
      },
      eq: { tilt: { tiltDb: 2, pivotHz: 1000 } },
    } as unknown as MasteringChainConfig);

    expect(flat).toEqual(nested);
    // The legacy keys are rewritten, not left dangling alongside the canonical ones.
    expect(flat['repair.denoise']).toBeUndefined();
    expect(flat['repair.denoise.enabled']).toBe(true);
    expect(flat['eq.tiltDb']).toBeUndefined();
    expect(flat['eq.tilt.tiltDb']).toBe(2);
  });

  it('preserves a falsy legacy value when normalizing', () => {
    const flat = flattenChainConfig({
      repair: { denoise: false },
    } as unknown as MasteringChainConfig);
    expect(flat['repair.denoise.enabled']).toBe(false);
    expect(flat['repair.denoise']).toBeUndefined();
  });
});
