/**
 * `capabilityCatalog().masteringPresets` matches the tracked
 * `tools/capability-catalog.json`, and `masteringPresetParams` returns the
 * same flat key space `masteringAssistantSuggestChain` does, for every
 * built-in preset.
 */

import { readFileSync } from 'node:fs';
import { dirname, resolve } from 'node:path';
import { fileURLToPath } from 'node:url';
import { describe, expect, it } from 'vitest';
import {
  capabilityCatalog,
  type MasteringChainConfig,
  masterAudio,
  masteringAssistantSuggestChain,
  masteringPresetNames,
  masteringPresetParams,
} from '../src/index.js';

const nodeRoot = resolve(dirname(fileURLToPath(import.meta.url)), '..');
const repoRoot = resolve(nodeRoot, '..', '..');

const SR = 22050;

function sine(seconds: number, freq = 440, amp = 0.2): Float32Array {
  const samples = new Float32Array(Math.round(SR * seconds));
  for (let i = 0; i < samples.length; i += 1) {
    samples[i] = amp * Math.sin((2 * Math.PI * freq * i) / SR);
  }
  return samples;
}

describe('capabilityCatalog().masteringPresets', () => {
  it('deep-equals the tracked capability-catalog.json entry', () => {
    const tracked = JSON.parse(
      readFileSync(resolve(repoRoot, 'tools/capability-catalog.json'), 'utf-8'),
    );
    expect(capabilityCatalog().masteringPresets).toEqual(tracked.masteringPresets);
  });

  it('reports null loudness fields for restoration presets', () => {
    const catalog = capabilityCatalog();
    const vinyl = catalog.masteringPresets.find((preset) => preset.name === 'vinyl');
    expect(vinyl).toMatchObject({
      kind: 'restoration',
      targetLufs: null,
      truePeakCeilingDb: null,
      maxLimiterGainReductionDb: null,
    });
    const pop = catalog.masteringPresets.find((preset) => preset.name === 'pop');
    expect(pop?.kind).toBe('mastering');
    expect(typeof pop?.targetLufs).toBe('number');
  });
});

describe('masteringPresetParams', () => {
  it('matches masteringAssistantSuggestChain key set for every preset', () => {
    const samples = sine(0.4);
    const referenceKeys = new Set(
      Object.keys(masteringAssistantSuggestChain({ samples, sampleRate: SR })),
    );
    for (const preset of masteringPresetNames()) {
      const params = masteringPresetParams(preset);
      expect(new Set(Object.keys(params))).toEqual(referenceKeys);
    }
  });

  it('applies unchanged as masterAudio overrides', () => {
    const samples = sine(0.4);
    const params = masteringPresetParams('pop');
    const result = masterAudio({
      samples,
      sampleRate: SR,
      overrides: params as unknown as MasteringChainConfig,
    });
    expect(result.samples).toBeInstanceOf(Float32Array);
    expect(result.samples.length).toBe(samples.length);
  });

  it('throws for an unknown preset name', () => {
    expect(() => masteringPresetParams('not-a-real-preset' as never)).toThrow();
  });
});
