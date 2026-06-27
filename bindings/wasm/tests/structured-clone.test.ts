// Regression guard for an embind value-marshalling hazard: a value returned
// straight from a `requireModule().fn(...)` call carries a constructor that is NOT
// this realm's Array/Object. `Array.isArray()` / `typeof === 'object'` still pass,
// but structuredClone rejects it with "could not be cloned" — and structuredClone is
// exactly what postMessage uses to move a result out of a Web Worker. This surfaced
// as the music-analysis demo's "Analysis failed". Wrappers must re-root such returns
// (Array.from for vectors, object spread for val::object) before handing them out.

import { beforeAll, describe, expect, it } from 'vitest';
import {
  analyzeSections,
  detectKeyCandidates,
  init,
  masteringInsertParamNames,
  masteringPairAnalysisNames,
  masteringPairProcessorNames,
  masteringPresetNames,
  masteringProcessorNames,
  masteringStereoAnalysisNames,
  mixingScenePresetNames,
  realtimeVoiceChangerPresetNames,
  synthPresetNames,
  synthPresetPatch,
} from '../src/index';

const SR = 22050;

function makeTone(durationSec: number): Float32Array {
  const n = Math.floor(SR * durationSec);
  const out = new Float32Array(n);
  for (let i = 0; i < n; i++) {
    const t = i / SR;
    out[i] = 0.3 * Math.sin(2 * Math.PI * 220 * t) + 0.2 * Math.sin(2 * Math.PI * 440 * t);
    if (i % Math.floor(0.5 * SR) < 80) {
      out[i] += 0.4; // periodic clicks for structure
    }
  }
  return out;
}

/** A foreign-realm array/object is still Array.isArray / typeof object, so assert
 *  both the constructor identity and that structuredClone (≡ postMessage) accepts it. */
function expectCloneable(value: unknown): void {
  if (Array.isArray(value)) {
    expect(value.constructor).toBe(Array);
  } else if (value && typeof value === 'object') {
    expect(value.constructor).toBe(Object);
  }
  expect(() => structuredClone(value)).not.toThrow();
}

describe('structured-clone safety of embind-marshalled returns', () => {
  beforeAll(async () => {
    await init();
  });

  describe('analysis array returns', () => {
    it('analyzeSections is a plain, cloneable Array (and survives slice/map)', () => {
      const sections = analyzeSections(makeTone(8), SR);
      expectCloneable(sections);
      expect(() => structuredClone(sections.slice(0, 4).map((s) => ({ ...s })))).not.toThrow();
    });

    it('detectKeyCandidates is a plain, cloneable Array', () => {
      const candidates = detectKeyCandidates(makeTone(8), SR, { modes: 'all' });
      expectCloneable(candidates);
      expect(() => structuredClone(candidates.slice(0, 8))).not.toThrow();
    });
  });

  describe('name / preset list returns', () => {
    const lists: Array<[string, () => string[]]> = [
      ['masteringProcessorNames', () => masteringProcessorNames()],
      ['masteringPairProcessorNames', () => masteringPairProcessorNames()],
      ['masteringPairAnalysisNames', () => masteringPairAnalysisNames()],
      ['masteringStereoAnalysisNames', () => masteringStereoAnalysisNames()],
      ['masteringPresetNames', () => masteringPresetNames()],
      ['masteringInsertParamNames', () => masteringInsertParamNames(masteringProcessorNames()[0])],
      ['mixingScenePresetNames', () => mixingScenePresetNames()],
      ['synthPresetNames', () => synthPresetNames()],
      ['realtimeVoiceChangerPresetNames', () => realtimeVoiceChangerPresetNames()],
    ];

    for (const [name, fn] of lists) {
      it(`${name} is a plain, cloneable Array`, () => {
        expectCloneable(fn());
      });
    }
  });

  describe('object returns', () => {
    it('synthPresetPatch is a plain, cloneable Object', () => {
      expectCloneable(synthPresetPatch(synthPresetNames()[0]));
    });
  });
});
