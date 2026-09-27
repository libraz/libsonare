/**
 * The mastering assistant's base preset. `preset` is a name on this surface; the
 * assistant starts from it and never picks one from the audio.
 */

import { describe, expect, it } from 'vitest';
import { masteringAssistantSuggest, masteringAssistantSuggestStereo } from '../src/index.js';

function sine(n: number, freq = 220, amp = 0.2): Float32Array {
  const out = new Float32Array(n);
  for (let i = 0; i < n; i += 1) {
    out[i] = amp * Math.sin((2 * Math.PI * freq * i) / 48000);
  }
  return out;
}

describe('mastering assistant preset', () => {
  const sampleRate = 48000;
  const samples = sine(sampleRate);

  it('starts from the streaming preset when none is named', () => {
    const result = JSON.parse(masteringAssistantSuggest({ samples, sampleRate }));
    expect(result.explanation).toContain('base preset: streaming');
    expect(result).not.toHaveProperty('genreCandidates');
  });

  it('reads an undefined or null preset as an omitted one', () => {
    const omitted = masteringAssistantSuggest({ samples, sampleRate });
    // biome-ignore lint/suspicious/noExplicitAny: null is what a JSON-shaped caller sends.
    for (const preset of [undefined, null as any]) {
      expect(masteringAssistantSuggest({ samples, sampleRate, params: { preset } })).toEqual(
        omitted,
      );
    }
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
