/**
 * `nonFiniteSubstitutionCount` reaches the TypeScript facade on every mastering
 * result the WASM module builds, and on the streaming chain as a method.
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
  masterAudioStereoWithProgress,
  masterAudioWithProgress,
  mastering,
  masteringChain,
  masteringChainStereo,
  masteringChainStereoWithProgress,
  masteringChainWithProgress,
  masteringPairProcess,
  masteringProcess,
  masteringProcessStereo,
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
    expectCleanCount(mastering(left, sampleRate, { targetLufs: -14 }).nonFiniteSubstitutionCount);
    expectCleanCount(
      masteringProcess('maximizer.truePeakLimiter', left, sampleRate).nonFiniteSubstitutionCount,
    );
    expectCleanCount(
      masteringPairProcess('match.abCrossfade', left, reference, sampleRate, { mix: 0.25 })
        .nonFiniteSubstitutionCount,
    );
  });

  it('reports 0 on the stereo one-shot result', () => {
    expectCleanCount(
      masteringProcessStereo('maximizer.truePeakLimiter', left, right, sampleRate)
        .nonFiniteSubstitutionCount,
    );
  });

  it('reports 0 on the chain results', () => {
    const config = { maximizer: { truePeakLimiter: { enabled: true } } };
    expectCleanCount(masteringChain(left, sampleRate, config).nonFiniteSubstitutionCount);
    expectCleanCount(
      masteringChainStereo(left, right, sampleRate, config).nonFiniteSubstitutionCount,
    );
    expectCleanCount(
      masteringChainWithProgress(left, sampleRate, config, () => {}).nonFiniteSubstitutionCount,
    );
    expectCleanCount(
      masteringChainStereoWithProgress(left, right, sampleRate, config, () => {})
        .nonFiniteSubstitutionCount,
    );
  });

  it('reports 0 on the preset results', () => {
    expectCleanCount(masterAudio(left, sampleRate, 'speech').nonFiniteSubstitutionCount);
    expectCleanCount(
      masterAudioStereo(left, right, sampleRate, 'speech').nonFiniteSubstitutionCount,
    );
    expectCleanCount(
      masterAudioWithProgress(left, sampleRate, 'speech', null, () => {})
        .nonFiniteSubstitutionCount,
    );
    expectCleanCount(
      masterAudioStereoWithProgress(left, right, sampleRate, 'speech', null, () => {})
        .nonFiniteSubstitutionCount,
    );
  });

  it('reports 0 across a clean stream and stays reachable after prepare', () => {
    const chain = new StreamingMasteringChain({
      maximizer: { truePeakLimiter: { enabled: true } },
    });
    try {
      chain.prepare(sampleRate, 512, 1);
      expectCleanCount(chain.nonFiniteSubstitutionCount());
      const block = new Float32Array(512).fill(0.1);
      chain.processMono(block);
      chain.processMono(block);
      expectCleanCount(chain.nonFiniteSubstitutionCount());
      // prepare() rebuilds the stages, so the counter is readable and still
      // clean on the rebuilt chain rather than throwing on a stale stage list.
      chain.prepare(sampleRate, 512, 1);
      expectCleanCount(chain.nonFiniteSubstitutionCount());
    } finally {
      chain.delete();
    }
  });
});
