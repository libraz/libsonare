/**
 * `nonFiniteSubstitutionCount` reaches the TypeScript facade on every mastering
 * result the WASM module builds, and on the streaming chain as a getter.
 *
 * A facade that rebuilds a result object field by field drops a newly added
 * property without any error, and the property is a `number` either way — so
 * `undefined` is what a dropped one looks like, and these assertions separate it
 * from the 0 a clean run owes. The clean-run value is the only one exercised
 * here: producing a non-zero count needs a poisoned input, which belongs with
 * the substitution behaviour rather than with the plumbing.
 */

import { beforeAll, describe, expect, it } from 'vitest';
import {
  init,
  masterAudio,
  masterAudioStereo,
  masteringApplyPairProcessor,
  masteringApplyProcessor,
  masteringApplyProcessorStereo,
  masteringNormalizeLoudness,
  StreamingMasteringChain,
} from '../src/index';

const sampleRate = 22_050;

function tone(freq: number, amp = 0.18, durationSec = 0.25): Float32Array {
  const n = Math.floor(sampleRate * durationSec);
  return Float32Array.from(
    { length: n },
    (_, i) => amp * Math.sin((2 * Math.PI * freq * i) / sampleRate),
  );
}

const left = tone(220);
const right = tone(330, 0.12);
const reference = tone(440, 0.12);

/** Fails on a dropped property (`undefined`) as well as on a wrong count. */
function expectCleanCount(count: number): void {
  expect(typeof count).toBe('number');
  expect(count).toBe(0);
}

describe('mastering nonFiniteSubstitutionCount (WASM)', () => {
  beforeAll(async () => {
    await init();
  });

  it('reports 0 on the mono one-shot results', () => {
    expectCleanCount(
      masteringNormalizeLoudness({ samples: left, sampleRate, targetLufs: -14 })
        .nonFiniteSubstitutionCount,
    );
    expectCleanCount(
      masteringApplyProcessor({
        processor: 'maximizer.truePeakLimiter',
        samples: left,
        sampleRate,
      }).nonFiniteSubstitutionCount,
    );
    expectCleanCount(
      masteringApplyPairProcessor({
        processor: 'match.abCrossfade',
        source: left,
        reference,
        sampleRate,
        params: { mix: 0.25 },
      }).nonFiniteSubstitutionCount,
    );
  });

  it('reports 0 on the stereo one-shot result', () => {
    expectCleanCount(
      masteringApplyProcessorStereo({
        processor: 'maximizer.truePeakLimiter',
        left,
        right,
        sampleRate,
      }).nonFiniteSubstitutionCount,
    );
  });

  it('reports 0 on the chain results', () => {
    const overrides = { maximizer: { truePeakLimiter: { enabled: true } } };
    expectCleanCount(
      masterAudio({ samples: left, sampleRate, overrides }).nonFiniteSubstitutionCount,
    );
    expectCleanCount(
      masterAudioStereo({ left, right, sampleRate, overrides }).nonFiniteSubstitutionCount,
    );
    expectCleanCount(
      masterAudio({ samples: left, sampleRate, overrides, onProgress: () => {} })
        .nonFiniteSubstitutionCount,
    );
    expectCleanCount(
      masterAudioStereo({ left, right, sampleRate, overrides, onProgress: () => {} })
        .nonFiniteSubstitutionCount,
    );
  });

  it('reports 0 on the preset results', () => {
    const preset = 'speech';
    expectCleanCount(masterAudio({ samples: left, sampleRate, preset }).nonFiniteSubstitutionCount);
    expectCleanCount(
      masterAudioStereo({ left, right, sampleRate, preset }).nonFiniteSubstitutionCount,
    );
    expectCleanCount(
      masterAudio({ samples: left, sampleRate, preset, onProgress: () => {} })
        .nonFiniteSubstitutionCount,
    );
    expectCleanCount(
      masterAudioStereo({ left, right, sampleRate, preset, onProgress: () => {} })
        .nonFiniteSubstitutionCount,
    );
  });

  it('reports 0 across a clean stream', () => {
    const chain = new StreamingMasteringChain({
      sampleRate,
      maxBlockSize: 512,
      maximizer: { truePeakLimiter: { enabled: true } },
    });
    try {
      expectCleanCount(chain.nonFiniteSubstitutionCount);
      const block = new Float32Array(512).fill(0.1);
      chain.processMono(block);
      chain.processMono(block);
      expectCleanCount(chain.nonFiniteSubstitutionCount);
    } finally {
      chain.destroy();
    }
  });
});
