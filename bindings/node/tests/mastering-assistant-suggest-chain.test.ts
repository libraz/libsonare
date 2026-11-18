/**
 * `masteringAssistantSuggestChain(Stereo)` return the same nested chain config
 * `masteringAssistantSuggest(Stereo)` carries as its `chainConfig`, in a shape
 * that plugs straight into `masterAudio(Stereo)`'s `overrides`.
 */

import { describe, expect, it } from 'vitest';
import {
  type MasteringChainConfig,
  masterAudio,
  masterAudioStereo,
  masteringAssistantSuggest,
  masteringAssistantSuggestChain,
  masteringAssistantSuggestChainStereo,
  masteringAssistantSuggestStereo,
} from '../src/index.js';

const sampleRate = 22050;

function sine(seconds: number, freq = 440, amp = 0.2): Float32Array {
  const samples = new Float32Array(Math.round(sampleRate * seconds));
  for (let i = 0; i < samples.length; i += 1) {
    samples[i] = amp * Math.sin((2 * Math.PI * freq * i) / sampleRate);
  }
  return samples;
}

describe('masteringAssistantSuggestChain', () => {
  it('matches the chain config masteringAssistantSuggest carries', () => {
    const samples = sine(0.4);
    const document = masteringAssistantSuggest({ samples, sampleRate });

    const chain = masteringAssistantSuggestChain({ samples, sampleRate });

    expect(chain).toEqual(document.chainConfig);
  });

  it('can be applied as masterAudio overrides', () => {
    const samples = sine(0.4);
    const chain = masteringAssistantSuggestChain({ samples, sampleRate });

    const result = masterAudio({
      samples,
      sampleRate,
      overrides: chain as MasteringChainConfig,
    });

    expect(result.samples).toBeInstanceOf(Float32Array);
    expect(result.samples.length).toBe(samples.length);
  });
});

describe('masteringAssistantSuggestChainStereo', () => {
  it('matches the chain config masteringAssistantSuggestStereo carries', () => {
    const left = sine(0.3, 220);
    const right = sine(0.3, 220, 0.18);
    const document = masteringAssistantSuggestStereo({ left, right, sampleRate });

    const chain = masteringAssistantSuggestChainStereo({ left, right, sampleRate });

    expect(chain).toEqual(document.chainConfig);
  });

  it('can be applied as masterAudioStereo overrides', () => {
    const left = sine(0.3, 220);
    const right = sine(0.3, 220, 0.18);
    const chain = masteringAssistantSuggestChainStereo({ left, right, sampleRate });

    const result = masterAudioStereo({
      left,
      right,
      sampleRate,
      overrides: chain as MasteringChainConfig,
    });

    expect(result.left).toBeInstanceOf(Float32Array);
    expect(result.right).toBeInstanceOf(Float32Array);
    expect(result.left.length).toBe(left.length);
  });
});
