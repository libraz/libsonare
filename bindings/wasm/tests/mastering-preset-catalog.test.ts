/**
 * The mastering preset catalog: `capabilityCatalog().masteringPresets`
 * against the tracked `tools/capability-catalog.json`, and
 * `masteringPresetParams` against `masteringAssistantSuggestChain`'s key
 * space.
 */

import { readFileSync } from 'node:fs';
import { beforeAll, describe, expect, it } from 'vitest';
import {
  capabilityCatalog,
  init,
  masteringAssistantSuggestChain,
  masteringPresetNames,
  masteringPresetParams,
} from '../dist/index.js';

const TRACKED_CATALOG = JSON.parse(
  readFileSync(new URL('../../../tools/capability-catalog.json', import.meta.url).pathname, 'utf8'),
);

describe('mastering preset catalog', () => {
  beforeAll(async () => {
    await init();
  });

  it('matches the tracked capability catalog masteringPresets entry for entry', () => {
    expect(capabilityCatalog().masteringPresets).toEqual(TRACKED_CATALOG.masteringPresets);
  });

  it('carries exactly the 30 built-in presets, 5 of them restoration', () => {
    const presets = capabilityCatalog().masteringPresets;
    expect(presets).toHaveLength(30);
    const restoration = presets.filter((preset) => preset.kind === 'restoration');
    expect(restoration).toHaveLength(5);
    for (const preset of restoration) {
      expect(preset.targetLufs).toBeNull();
      expect(preset.truePeakCeilingDb).toBeNull();
      expect(preset.maxLimiterGainReductionDb).toBeNull();
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
      suggestedKeys = Object.keys(masteringAssistantSuggestChain({ samples, sampleRate })).sort();
    });

    it('does not throw for any built-in preset', () => {
      for (const preset of masteringPresetNames()) {
        expect(() => masteringPresetParams(preset)).not.toThrow();
      }
    });

    it("matches masteringAssistantSuggestChain's key set for every preset", () => {
      expect(suggestedKeys.length).toBeGreaterThan(0);
      for (const preset of masteringPresetNames()) {
        const keys = Object.keys(masteringPresetParams(preset)).sort();
        expect(keys).toEqual(suggestedKeys);
      }
    });

    it('rejects an unknown preset name', () => {
      // @ts-expect-error deliberately invalid preset for the throw assertion
      expect(() => masteringPresetParams('not-a-preset')).toThrow();
    });
  });
});
