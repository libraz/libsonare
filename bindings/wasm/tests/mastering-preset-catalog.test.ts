/**
 * The mastering preset catalog: `capabilityCatalog().masteringPresets`
 * against the tracked `tools/capability-catalog.json`, and
 * `masteringPresetParams` against `masteringAssistantSuggest`'s nested `chainConfig`
 * key space.
 */

import { readFileSync } from 'node:fs';
import { beforeAll, describe, expect, it } from 'vitest';
import {
  capabilityCatalog,
  init,
  masteringAssistantSuggest,
  masteringPresetNames,
  masteringPresetParams,
} from '../dist/index.js';

const TRACKED_CATALOG = JSON.parse(
  readFileSync(new URL('../../../tools/capability-catalog.json', import.meta.url).pathname, 'utf8'),
  // The facade reports the document's null as an absent field.
  function (this: unknown, _key: string, value: unknown) {
    return value === null && !Array.isArray(this) ? undefined : value;
  },
);

/** Dotted paths of every leaf of a nested chain config. */
function leafPaths(node: unknown, prefix = ''): string[] {
  if (typeof node !== 'object' || node === null) {
    return [prefix];
  }
  return Object.entries(node).flatMap(([key, value]) =>
    leafPaths(value, prefix ? `${prefix}.${key}` : key),
  );
}

describe('mastering preset catalog', () => {
  beforeAll(async () => {
    await init();
  });

  it('matches the tracked capability catalog masteringPresets entry for entry', () => {
    expect(capabilityCatalog().masteringPresets).toEqual(TRACKED_CATALOG.masteringPresets);
  });

  it('matches the tracked capability catalog chain section', () => {
    expect(capabilityCatalog().chain).toEqual(TRACKED_CATALOG.chain);
  });

  it('carries exactly the 30 built-in presets, 5 of them restoration', () => {
    const presets = capabilityCatalog().masteringPresets;
    expect(presets).toHaveLength(30);
    const restoration = presets.filter((preset) => preset.kind === 'restoration');
    expect(restoration).toHaveLength(5);
    for (const preset of restoration) {
      expect(preset).not.toHaveProperty('targetLufs');
      expect(preset).not.toHaveProperty('truePeakCeilingDbtp');
      expect(preset).not.toHaveProperty('maxLimiterGainReductionDb');
    }
  });

  describe('masteringPresetParams', () => {
    const sampleRate = 22050;
    const samples = new Float32Array(sampleRate);
    for (let i = 0; i < samples.length; i++) {
      samples[i] = 0.2 * Math.sin((2 * Math.PI * 440 * i) / sampleRate);
    }
    let suggestedKeys: string[];
    beforeAll(() => {
      // Deferred to beforeAll: init() has not necessarily run when this
      // describe body itself executes.
      suggestedKeys = leafPaths(
        masteringAssistantSuggest({ samples, sampleRate }).chainConfig,
      ).sort();
    });

    it('does not throw for any built-in preset', () => {
      for (const preset of masteringPresetNames()) {
        expect(() => masteringPresetParams(preset)).not.toThrow();
      }
    });

    it("matches masteringAssistantSuggest's chainConfig key set for every preset", () => {
      expect(suggestedKeys.length).toBeGreaterThan(0);
      for (const preset of masteringPresetNames()) {
        const keys = leafPaths(masteringPresetParams(preset)).sort();
        expect(keys).toEqual(suggestedKeys);
      }
    });

    it('rejects an unknown preset name', () => {
      // @ts-expect-error deliberately invalid preset for the throw assertion
      expect(() => masteringPresetParams('not-a-preset')).toThrow();
    });
  });
});
