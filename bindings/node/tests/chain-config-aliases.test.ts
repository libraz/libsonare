import { describe, expect, it } from 'vitest';
import { StreamingMasteringChain } from '../src/index.js';
import { flattenChainConfig, STREAMING_ONLY_CONFIG_KEYS } from '../src/internal/chain_config.js';
import type { MasteringChainConfig } from '../src/types.js';
import { streamingSkippedConfigKeys } from './_addon_sources.js';

describe('flattenChainConfig refuses the removed spellings', () => {
  it.each([
    ['boolean denoise shorthand', { repair: { denoise: true } }, /repair\.denoise/],
    ['flat repair.nFft', { repair: { nFft: 2048 } }, /repair\.nFft/],
    ['flat repair.hopLength', { repair: { hopLength: 512 } }, /repair\.hopLength/],
    ['flat repair.ddAlpha', { repair: { ddAlpha: 0.98 } }, /repair\.ddAlpha/],
    ['flat repair.reductionDb', { repair: { reductionDb: 20 } }, /repair\.reductionDb/],
    ['flat repair.gainFloor', { repair: { gainFloor: 0.1 } }, /repair\.gainFloor/],
    ['eq.tiltDb', { eq: { tiltDb: 2 } }, /eq\.tiltDb/],
    ['eq.pivotHz', { eq: { pivotHz: 1000 } }, /eq\.pivotHz/],
    ['dotted key', { 'loudness.targetLufs': -20 }, /dotted keys are not accepted/],
    [
      'dotted key beside a nested one',
      { loudness: { targetLufs: -20 }, 'eq.tilt.tiltDb': 2 },
      /eq\.tilt\.tiltDb/,
    ],
  ])('%s is a TypeError that names it', (_name, config, message) => {
    expect(() => flattenChainConfig(config as unknown as MasteringChainConfig)).toThrow(TypeError);
    expect(() => flattenChainConfig(config as unknown as MasteringChainConfig)).toThrow(message);
  });

  it('flattens the nested spelling of the replaced fields', () => {
    const flat = flattenChainConfig({
      repair: { denoise: { enabled: false, nFft: 2048, hopLength: 512, ddAlpha: 0.98 } },
      eq: { tilt: { tiltDb: 2, pivotHz: 1000 } },
    });
    expect(flat).toEqual({
      'repair.denoise.enabled': false,
      'repair.denoise.nFft': 2048,
      'repair.denoise.hopLength': 512,
      'repair.denoise.ddAlpha': 0.98,
      'eq.tilt.tiltDb': 2,
      'eq.tilt.pivotHz': 1000,
    });
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
    const config = { sampleRate: 48000, numChannels: 1, ...badConfig } as never;
    expect(() => new StreamingMasteringChain(config)).toThrow(TypeError);
    expect(() => new StreamingMasteringChain(config)).toThrow(
      /maximizer\.truePeakLimiter\.ceilingDb/,
    );
  });

  it('a well-typed value still constructs on both paths', () => {
    const good = { maximizer: { truePeakLimiter: { enabled: true, ceilingDb: -1 } } };
    expect(flattenChainConfig(good as unknown as MasteringChainConfig)).toEqual({
      'maximizer.truePeakLimiter.enabled': true,
      'maximizer.truePeakLimiter.ceilingDb': -1,
    });
    const chain = new StreamingMasteringChain({
      sampleRate: 48000,
      numChannels: 1,
      maxBlockSize: 512,
      ...good,
    });
    try {
      expect(chain.stageNames()).toContain('maximizer.truePeakLimiter');
    } finally {
      chain.destroy();
    }
  });

  it('the streaming-only top-level options are not rejected as chain leaves', () => {
    const chain = new StreamingMasteringChain({
      sampleRate: 48000,
      numChannels: 1,
      maxBlockSize: 512,
      loudnessStaticGainDb: -3,
      loudnessStaticGainPeakDb: -0.5,
      eq: { tilt: { tiltDb: 1 } },
    });
    try {
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
