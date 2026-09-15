/**
 * Every mastering result reports how many non-finite input samples a processor
 * replaced with a finite in-domain one. The substituted output is finite, in
 * range and error-free, so the count is the only thing separating a degraded
 * result from a clean one — which makes the zero it reports on finite input
 * part of the contract, not an incidental value.
 */

import { describe, expect, it } from 'vitest';
import {
  StreamingMasteringChain,
  masterAudio,
  masterAudioAsync,
  masterAudioStereo,
  masterAudioStereoAsync,
  mastering,
  masteringChain,
  masteringChainStereo,
  masteringPairProcess,
  masteringProcess,
  masteringProcessStereo,
} from '../src/index.js';

const SR = 44100;

function sine(n: number, freq = 220, amp = 0.3): Float32Array {
  const out = new Float32Array(n);
  for (let i = 0; i < n; i++) {
    out[i] = amp * Math.sin((2 * Math.PI * freq * i) / SR);
  }
  return out;
}

describe('nonFiniteSubstitutionCount on offline mastering results', () => {
  it('is zero for the simple loudness-optimizing mastering()', () => {
    const result = mastering(sine(4096), SR, { targetLufs: -14 });
    expect(result.nonFiniteSubstitutionCount).toBe(0);
  });

  // The true-peak limiter is one of the processors that actually substitutes, so
  // the zero it reports here is a measured zero rather than a stage that could
  // never report anything else.
  it('is zero for a named solo processor, mono and stereo', () => {
    const x = sine(4096);
    const params = { ceilingDb: -1 };
    const mono = masteringProcess('maximizer.truePeakLimiter', x, SR, params);
    expect(mono.nonFiniteSubstitutionCount).toBe(0);

    const stereo = masteringProcessStereo('maximizer.truePeakLimiter', x, sine(4096, 330), SR, {
      ...params,
    });
    expect(stereo.nonFiniteSubstitutionCount).toBe(0);
  });

  it('is zero for a two-input pair processor', () => {
    const paired = masteringPairProcess('match.abCrossfade', sine(4096), sine(4096, 880), SR, {
      mix: 0.25,
    });
    expect(paired.nonFiniteSubstitutionCount).toBe(0);
  });

  it('is zero for the mono and stereo chains, with and without progress', () => {
    const x = sine(4096);
    const y = sine(4096, 330);
    const config = {
      dynamics: { compressor: { thresholdDb: -30, ratio: 4 } },
      maximizer: { truePeakLimiter: { enabled: true, ceilingDb: -1 } },
    };

    expect(masteringChain(x, SR, config).nonFiniteSubstitutionCount).toBe(0);
    expect(masteringChainStereo(x, y, SR, config).nonFiniteSubstitutionCount).toBe(0);

    // The progress path builds its result through the C-ABI chain structs
    // rather than the C++ ones, so it is a separate builder to cover.
    const withProgress = masteringChain(x, SR, config, () => {});
    expect(withProgress.nonFiniteSubstitutionCount).toBe(0);
    const stereoWithProgress = masteringChainStereo(x, y, SR, config, () => {});
    expect(stereoWithProgress.nonFiniteSubstitutionCount).toBe(0);
  });

  it('is zero for the preset chains, with and without progress', () => {
    const x = sine(4096);
    const y = sine(4096, 330);

    expect(masterAudio(x, SR, 'pop').nonFiniteSubstitutionCount).toBe(0);
    expect(masterAudioStereo(x, y, SR, 'pop').nonFiniteSubstitutionCount).toBe(0);

    const withProgress = masterAudio(x, SR, 'pop', {}, () => {});
    expect(withProgress.nonFiniteSubstitutionCount).toBe(0);
    const stereoWithProgress = masterAudioStereo(x, y, SR, 'pop', {}, () => {});
    expect(stereoWithProgress.nonFiniteSubstitutionCount).toBe(0);
  });

  it('is zero on the async preset chains, which serialize their result separately', async () => {
    const x = sine(4096);
    const y = sine(4096, 330);

    const mono = await masterAudioAsync(x, SR, 'pop');
    expect(mono.nonFiniteSubstitutionCount).toBe(0);
    const stereo = await masterAudioStereoAsync(x, y, SR, 'pop');
    expect(stereo.nonFiniteSubstitutionCount).toBe(0);
  });
});

describe('StreamingMasteringChain.nonFiniteSubstitutionCount', () => {
  it('is zero across a clean mono stream and its flush', () => {
    const chain = new StreamingMasteringChain({
      'dynamics.compressor.thresholdDb': -24,
      'maximizer.truePeakLimiter.enabled': true,
    });
    chain.prepare(SR, 512, 1);
    expect(chain.nonFiniteSubstitutionCount()).toBe(0);

    const block = sine(512);
    for (let i = 0; i < 4; i++) {
      chain.processMono(block);
    }
    while (chain.flushMono().length > 0) {
      // drain the reported latency
    }
    expect(chain.nonFiniteSubstitutionCount()).toBe(0);
    chain.destroy();
  });

  it('is zero across a clean stereo stream', () => {
    const chain = new StreamingMasteringChain({
      'maximizer.truePeakLimiter.enabled': true,
    });
    chain.prepare(SR, 512, 2);
    chain.processStereo(sine(512), sine(512, 330));
    expect(chain.nonFiniteSubstitutionCount()).toBe(0);
    chain.destroy();
  });

  it('reports zero before prepare and throws after destroy', () => {
    const chain = new StreamingMasteringChain({ 'eq.tilt.tiltDb': 0.5 });
    expect(chain.nonFiniteSubstitutionCount()).toBe(0);
    chain.destroy();
    expect(() => chain.nonFiniteSubstitutionCount()).toThrow();
  });
});
