import { describe, expect, it } from 'vitest';
import { flattenChainConfig } from '../src/_chain_config';
import type { MasteringChainConfig } from '../src/types';

describe('flattenChainConfig legacy flat aliases', () => {
  it('leaves legacy flat spellings for the core canonicalizer', () => {
    // canonical_chain_param_key() in the shared core owns these aliases.  The
    // facade only flattens object structure, avoiding a duplicated alias table.
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

    expect(flat).toMatchObject({
      'repair.denoise': true,
      'repair.nFft': 2048,
      'repair.hopLength': 512,
      'repair.ddAlpha': 0.98,
      'repair.gainFloor': 0.1,
      'eq.tiltDb': 2,
      'eq.pivotHz': 1000,
    });
  });

  it('preserves a falsy legacy value for the core canonicalizer', () => {
    const flat = flattenChainConfig({
      repair: { denoise: false },
    } as unknown as MasteringChainConfig);
    expect(flat['repair.denoise']).toBe(false);
  });
});
