import { beforeAll, describe, expect, it } from 'vitest';
import {
  init,
  masterAudio,
  masterAudioStereo,
  masteringAssistantSuggest,
  masteringAssistantSuggestChain,
  masteringAssistantSuggestChainStereo,
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

describe('masteringAssistantSuggestChain', () => {
  it('matches the chainConfig.params the full assistant document carries', () => {
    const params = { targetLufs: -13, ceilingDb: -0.8, enableRepair: true };
    const document = JSON.parse(masteringAssistantSuggest(samples, sampleRate, params));
    const chain = masteringAssistantSuggestChain({ samples, sampleRate, params });

    expect(chain).toEqual(document.chainConfig.params);
  });

  it('is usable directly as masterAudio overrides', () => {
    const chain = masteringAssistantSuggestChain({ samples, sampleRate });
    const result = masterAudio({ samples, sampleRate, preset: 'pop', overrides: chain });

    expect(result.samples).toBeInstanceOf(Float32Array);
    expect(result.samples.length).toBe(samples.length);
    expect(Number.isFinite(result.outputLufs)).toBe(true);
  });
});

describe('masteringAssistantSuggestChainStereo', () => {
  it('matches the chainConfig.params the full stereo assistant document carries', () => {
    const params = { targetLufs: -14, enableRepair: true };
    const document = JSON.parse(
      masteringAssistantSuggestStereo({ left, right, sampleRate, params }),
    );
    const chain = masteringAssistantSuggestChainStereo({ left, right, sampleRate, params });

    expect(chain).toEqual(document.chainConfig.params);
  });

  it('is usable directly as masterAudioStereo overrides', () => {
    const chain = masteringAssistantSuggestChainStereo({ left, right, sampleRate });
    const result = masterAudioStereo({ left, right, sampleRate, preset: 'pop', overrides: chain });

    expect(result.left).toBeInstanceOf(Float32Array);
    expect(result.right).toBeInstanceOf(Float32Array);
    expect(result.left.length).toBe(left.length);
    expect(Number.isFinite(result.outputLufs)).toBe(true);
  });
});
