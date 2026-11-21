import { beforeAll, describe, expect, it } from 'vitest';
import {
  init,
  masterAudio,
  masterAudioStereo,
  masteringAssistantSuggest,
  masteringAssistantSuggestStereo,
} from '../src/index';

const sampleRate = 22_050;
// 0.5s: long enough for the assistant's tempo/defect analysis, short enough to
// stay out of the slow-test tier.
const durationSamples = Math.floor(sampleRate * 0.5);
const samples = Float32Array.from(
  { length: durationSamples },
  (_, i) => 0.2 * Math.sin((2 * Math.PI * 220 * i) / sampleRate),
);
const left = samples;
const right = Float32Array.from(samples, (value) => value * 0.9);

beforeAll(async () => init());

describe('masteringAssistantSuggest chainConfig', () => {
  it('is usable directly as masterAudio overrides', () => {
    const { chainConfig } = masteringAssistantSuggest({ samples, sampleRate });
    const result = masterAudio({ samples, sampleRate, preset: 'pop', overrides: chainConfig });

    expect(result.samples).toBeInstanceOf(Float32Array);
    expect(result.samples.length).toBe(samples.length);
    expect(Number.isFinite(result.outputLufs)).toBe(true);
  });
});

describe('masteringAssistantSuggestStereo chainConfig', () => {
  it('is usable directly as masterAudioStereo overrides', () => {
    const { chainConfig } = masteringAssistantSuggestStereo({ left, right, sampleRate });
    const result = masterAudioStereo({
      left,
      right,
      sampleRate,
      preset: 'pop',
      overrides: chainConfig,
    });

    expect(result.left).toBeInstanceOf(Float32Array);
    expect(result.right).toBeInstanceOf(Float32Array);
    expect(result.left.length).toBe(left.length);
    expect(Number.isFinite(result.outputLufs)).toBe(true);
  });
});
