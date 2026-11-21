/**
 * The `chainConfig` of `masteringAssistantSuggest(Stereo)` plugs straight into
 * `masterAudio(Stereo)`'s `overrides`.
 */

import { describe, expect, it } from 'vitest';
import {
  type MasteringChainConfig,
  masterAudio,
  masterAudioStereo,
  masteringAssistantSuggest,
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

describe('masteringAssistantSuggest chainConfig', () => {
  it('can be applied as masterAudio overrides', () => {
    const samples = sine(0.4);
    const chain = masteringAssistantSuggest({ samples, sampleRate }).chainConfig;

    const result = masterAudio({
      samples,
      sampleRate,
      overrides: chain as MasteringChainConfig,
    });

    expect(result.samples).toBeInstanceOf(Float32Array);
    expect(result.samples.length).toBe(samples.length);
  });
});

describe('masteringAssistantSuggestStereo chainConfig', () => {
  it('can be applied as masterAudioStereo overrides', () => {
    const left = sine(0.3, 220);
    const right = sine(0.3, 220, 0.18);
    const chain = masteringAssistantSuggestStereo({ left, right, sampleRate }).chainConfig;

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
