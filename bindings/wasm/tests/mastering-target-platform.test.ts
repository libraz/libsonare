/**
 * The mastering assistant's delivery target. `targetPlatform` is a name on this
 * surface and reaches the shared table that decides the loudness, so a caller
 * asking for a broadcast master gets one instead of the streaming default.
 */

import { beforeAll, describe, expect, it } from 'vitest';
import {
  init,
  masteringAssistantSuggest,
  masteringAssistantSuggestStereo,
  masteringPlatformNames,
} from '../src/index';

const sampleRate = 48_000;
const samples = Float32Array.from({ length: sampleRate }, (_, i) =>
  Math.sin((2 * Math.PI * 220 * i) / sampleRate),
);

beforeAll(async () => init());

describe('WASM mastering assistant target platform', () => {
  it('exposes the accepted targets, and every one of them is accepted', () => {
    const names = masteringPlatformNames();
    expect(names).toContain('streaming');
    expect(names).toContain('broadcast');
    expect(names).toContain('club');
    // The discoverable list is the list that works: a name from it must never
    // be rejected by the entry point that consumes it.
    for (const name of names) {
      expect(() =>
        masteringAssistantSuggest({ samples, sampleRate, targetPlatform: name }),
      ).not.toThrow();
    }
  });

  it('returns the streaming default when no target is given', () => {
    expect(
      masteringAssistantSuggest({ samples, sampleRate }).chainConfig.loudness?.targetLufs,
    ).toBe(-14);
  });

  it('follows a named delivery target', () => {
    expect(
      masteringAssistantSuggest({ samples, sampleRate, targetPlatform: 'broadcast' }).chainConfig
        .loudness?.targetLufs,
    ).toBe(-23);
  });

  it('moves the ceiling for a loud delivery format', () => {
    // Broadcast and podcast ask for the ceiling the default already carries, so
    // the ceiling is only observable on the loud formats.
    const loudness = masteringAssistantSuggest({ samples, sampleRate, targetPlatform: 'club' })
      .chainConfig.loudness;
    expect(loudness?.targetLufs).toBe(-9);
    expect(loudness?.ceilingDbtp).toBeCloseTo(-0.3, 6);
  });

  it('applies the target through the stereo entry point too', () => {
    expect(
      masteringAssistantSuggestStereo({
        left: samples,
        right: samples,
        sampleRate,
        targetPlatform: 'broadcast',
      }).chainConfig.loudness?.targetLufs,
    ).toBe(-23);
  });

  it('rejects an unknown target instead of silently keeping the default', () => {
    expect(() =>
      masteringAssistantSuggest({ samples, sampleRate, targetPlatform: 'vinyl' as never }),
    ).toThrow(RangeError);
    expect(() =>
      masteringAssistantSuggest({ samples, sampleRate, targetPlatform: 'vinyl' as never }),
    ).toThrow(/^masteringAssistantSuggest: targetPlatform must be one of /);
  });

  it('rejects a numeric target: the index is a C-ABI transport detail', () => {
    expect(() =>
      masteringAssistantSuggest({ samples, sampleRate, targetPlatform: 2 as never }),
    ).toThrow(TypeError);
  });

  it('leaves the other numeric params working alongside a target', () => {
    // An explicit loudness the caller chose wins over the target's suggestion.
    const loudness = masteringAssistantSuggest({
      samples,
      sampleRate,
      targetPlatform: 'broadcast',
      targetLufs: -13,
      ceilingDbtp: -0.8,
    }).chainConfig.loudness;
    expect(loudness?.targetLufs).toBe(-13);
    expect(loudness?.ceilingDbtp).toBeCloseTo(-0.8, 6);
  });
});
