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
    const result = masteringAssistantSuggest({ samples, sampleRate });
    expect(result.explanation.join('\n')).toContain('base preset: streaming');
    expect(result).not.toHaveProperty('genreCandidates');
  });

  it('starts from the named preset', () => {
    const result = masteringAssistantSuggest({ samples, sampleRate, preset: 'classical' });
    expect(result.explanation.join('\n')).toContain('base preset: classical');
  });

  it('applies the preset through the stereo entry point too', () => {
    const result = masteringAssistantSuggestStereo({
      left: samples,
      right: samples,
      sampleRate,
      preset: 'jazz',
    });
    expect(result.explanation.join('\n')).toContain('base preset: jazz');
  });

  it('rejects an unknown preset and a restoration preset with a RangeError', () => {
    expect(() =>
      masteringAssistantSuggest({ samples, sampleRate, preset: 'notAPreset' as never }),
    ).toThrow(RangeError);
    let message = '';
    try {
      masteringAssistantSuggest({ samples, sampleRate, preset: 'vinyl' as never });
    } catch (error) {
      expect(error).toBeInstanceOf(RangeError);
      message = (error as Error).message;
    }
    expect(message).toMatch(/^masteringAssistantSuggest: preset must be one of pop, /);
    expect(message).toContain('gameOst');
    expect(message).not.toContain('vinyl');
  });

  it('rejects a numeric preset: the index is a C-ABI transport detail', () => {
    expect(() => masteringAssistantSuggest({ samples, sampleRate, preset: 3 as never })).toThrow(
      TypeError,
    );
  });
});
