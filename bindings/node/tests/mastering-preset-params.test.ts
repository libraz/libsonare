/**
 * `capabilityCatalog().masteringPresets` matches the tracked
 * `tools/capability-catalog.json`, and `masteringPresetParams` returns the
 * same key space the `chainConfig` of `masteringAssistantSuggest` does, for every
 * built-in preset.
 */

import { readFileSync } from 'node:fs';
import { dirname, resolve } from 'node:path';
import { fileURLToPath } from 'node:url';
import { describe, expect, it } from 'vitest';
import {
  capabilityCatalog,
  masterAudio,
  masteringAssistantSuggest,
  masteringPresetNames,
  masteringPresetParams,
} from '../src/index.js';

const nodeRoot = resolve(dirname(fileURLToPath(import.meta.url)), '..');
const repoRoot = resolve(nodeRoot, '..', '..');

const SR = 22050;

/** Dotted paths of every leaf of a nested chain config. */
function leafPaths(node: unknown, prefix = ''): string[] {
  if (typeof node !== 'object' || node === null) {
    return [prefix];
  }
  return Object.entries(node).flatMap(([key, value]) =>
    leafPaths(value, prefix ? `${prefix}.${key}` : key),
  );
}

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
      // The facade reports the document's null as an absent field.
      function (this: unknown, _key: string, value: unknown) {
        return value === null && !Array.isArray(this) ? undefined : value;
      },
    );
    expect(capabilityCatalog().masteringPresets).toEqual(tracked.masteringPresets);
  });

  it('leaves the loudness fields of a restoration preset absent', () => {
    const catalog = capabilityCatalog();
    const vinyl = catalog.masteringPresets.find((preset) => preset.name === 'vinyl');
    expect(vinyl).toMatchObject({
      kind: 'restoration',
    });
    expect(vinyl).not.toHaveProperty('targetLufs');
    expect(vinyl).not.toHaveProperty('truePeakCeilingDbtp');
    expect(vinyl).not.toHaveProperty('maxLimiterGainReductionDb');
    const pop = catalog.masteringPresets.find((preset) => preset.name === 'pop');
    expect(pop?.kind).toBe('mastering');
    expect(typeof pop?.targetLufs).toBe('number');
  });
});

describe('masteringPresetParams', () => {
  it('matches masteringAssistantSuggest chainConfig key set for every preset', () => {
    const samples = sine(0.4);
    const referenceKeys = new Set(
      leafPaths(masteringAssistantSuggest({ samples, sampleRate: SR }).chainConfig),
    );
    for (const preset of masteringPresetNames()) {
      const params = masteringPresetParams(preset);
      expect(new Set(leafPaths(params))).toEqual(referenceKeys);
    }
  });

  it('returns the nested config and applies unchanged as masterAudio overrides', () => {
    const samples = sine(0.4);
    const params = masteringPresetParams('pop');
    expect(Object.keys(params).some((key) => key.includes('.'))).toBe(false);
    const result = masterAudio({ samples, sampleRate: SR, preset: 'pop', overrides: params });
    expect(result.samples).toBeInstanceOf(Float32Array);
    expect(result.samples.length).toBe(samples.length);
  });

  it('throws for an unknown preset name', () => {
    expect(() => masteringPresetParams('not-a-real-preset' as never)).toThrow();
  });
});
