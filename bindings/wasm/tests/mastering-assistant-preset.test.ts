/**
 * The mastering assistant's base preset. `preset` is a name on this surface; the
 * assistant starts from it and never picks one from the audio.
 */

import { beforeAll, describe, expect, it } from 'vitest';
import { init, masteringAssistantSuggest, masteringAssistantSuggestStereo } from '../src/index';

const sampleRate = 48_000;
const samples = Float32Array.from({ length: sampleRate }, (_, i) =>
  Math.sin((2 * Math.PI * 220 * i) / sampleRate),
);

beforeAll(async () => init());

describe('WASM mastering assistant preset', () => {
  it('starts from the streaming preset when none is named', () => {
    const result = JSON.parse(masteringAssistantSuggest({ samples, sampleRate }));
    expect(result.explanation).toContain('base preset: streaming');
    expect(result).not.toHaveProperty('genreCandidates');
  });

  it('starts from the named preset', () => {
    const result = JSON.parse(
      masteringAssistantSuggest({ samples, sampleRate, params: { preset: 'classical' } }),
    );
    expect(result.explanation).toContain('base preset: classical');
  });

  it('applies the preset through the stereo entry point too', () => {
    const result = JSON.parse(
      masteringAssistantSuggestStereo({
        left: samples,
        right: samples,
        sampleRate,
        params: { preset: 'jazz' },
      }),
    );
    expect(result.explanation).toContain('base preset: jazz');
  });

  it('rejects an unknown preset and a restoration preset', () => {
    expect(() =>
      masteringAssistantSuggest({ samples, sampleRate, params: { preset: 'notAPreset' } }),
    ).toThrow(/notAPreset/);
    expect(() =>
      masteringAssistantSuggest({ samples, sampleRate, params: { preset: 'vinyl' } }),
    ).toThrow(/vinyl/);
  });

  it('rejects a numeric preset: the index is a C-ABI transport detail', () => {
    expect(() =>
      // biome-ignore lint/suspicious/noExplicitAny: deliberately passing the wrong type.
      masteringAssistantSuggest({ samples, sampleRate, params: { preset: 3 as any } }),
    ).toThrow(/preset/);
  });
});
