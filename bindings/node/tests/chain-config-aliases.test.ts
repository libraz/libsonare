import { describe, expect, it } from 'vitest';
import { flattenChainConfig, STREAMING_ONLY_CONFIG_KEYS } from '../src/_chain_config.js';
import { StreamingMasteringChain } from '../src/index.js';
import type { MasteringChainConfig } from '../src/types.js';
import { streamingSkippedConfigKeys } from './_addon_sources.js';

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

  // The dot-notation spelling is part of the accepted input, not an accident of
  // the walk: MasteringChainConfig declares it and the Python binding documents
  // it, so both surfaces must land on the same core parameter.
  it('carries a caller-supplied dot-notation key through unchanged', () => {
    const nested = flattenChainConfig({ loudness: { targetLufs: -20 }, eq: { tiltDb: 2 } });
    const dotted = flattenChainConfig({ 'loudness.targetLufs': -20, 'eq.tiltDb': 2 });
    expect(dotted).toEqual(nested);
  });

  it('merges the two spellings in one config', () => {
    const mixed = flattenChainConfig({ loudness: { targetLufs: -20 }, 'eq.tiltDb': 2 });
    expect(mixed).toEqual({ 'loudness.targetLufs': -20, 'eq.tiltDb': 2 });
  });
});

// The streaming chain used to hand its config straight to the addon, whose
// flattener keeps number and boolean leaves and skips everything else without
// a word. A `ceilingDb: '-1.0'` from a JSON preset therefore built a chain at
// the stage default on the streaming path and threw a TypeError on the offline
// path — the same object, two outcomes, one of them silent.
describe('streaming chain config rejects the same leaves as the offline chain', () => {
  const badConfig = { maximizer: { truePeakLimiter: { ceilingDb: '-1.0' } } };

  it('offline flattening names the offending dotted path', () => {
    expect(() => flattenChainConfig(badConfig as unknown as MasteringChainConfig)).toThrow(
      /maximizer\.truePeakLimiter\.ceilingDb/,
    );
  });

  it('the streaming constructor rejects it the same way', () => {
    expect(() => new StreamingMasteringChain(badConfig)).toThrow(TypeError);
    expect(() => new StreamingMasteringChain(badConfig)).toThrow(
      /maximizer\.truePeakLimiter\.ceilingDb/,
    );
  });

  it('a well-typed value still constructs on both paths', () => {
    const good = { maximizer: { truePeakLimiter: { enabled: true, ceilingDb: -1 } } };
    expect(flattenChainConfig(good as unknown as MasteringChainConfig)).toEqual({
      'maximizer.truePeakLimiter.enabled': true,
      'maximizer.truePeakLimiter.ceilingDb': -1,
    });
    const chain = new StreamingMasteringChain(good);
    try {
      chain.prepare(48000, 512, 1);
      expect(chain.stageNames()).toContain('maximizer.truePeakLimiter');
    } finally {
      chain.destroy();
    }
  });

  it('the streaming-only top-level options are not rejected as chain leaves', () => {
    const chain = new StreamingMasteringChain({
      loudnessStaticGainDb: -3,
      loudnessStaticGainPeakDb: -0.5,
      eq: { tilt: { tiltDb: 1 } },
    });
    try {
      chain.prepare(48000, 512, 1);
      expect(chain.stageNames()).toContain('eq.tilt');
    } finally {
      chain.destroy();
    }
  });

  it("the skipped-key list is the addon's, not a hand-copy that can drift", () => {
    // The TypeScript constant mirrors a C++ literal this package cannot import.
    // Read the C++ back rather than trusting the copy; the non-empty assertion
    // keeps a regex that stopped matching from passing by comparing nothing.
    const fromAddon = streamingSkippedConfigKeys();
    expect(fromAddon.length).toBeGreaterThan(0);
    expect([...STREAMING_ONLY_CONFIG_KEYS].sort()).toEqual(fromAddon);
  });
});
