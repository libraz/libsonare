/**
 * The mastering assistant's delivery target. `targetPlatform` is a name on this
 * surface and reaches the shared table that decides the loudness, so a caller
 * asking for a broadcast master gets one instead of the streaming default.
 */

import { describe, expect, it } from 'vitest';
import {
  masteringAssistantSuggest,
  masteringAssistantSuggestStereo,
  masteringPlatformNames,
} from '../src/index.js';

function sine(n: number, freq = 220, amp = 0.2): Float32Array {
  const out = new Float32Array(n);
  for (let i = 0; i < n; i += 1) {
    out[i] = amp * Math.sin((2 * Math.PI * freq * i) / 48000);
  }
  return out;
}

describe('mastering assistant target platform', () => {
  const sampleRate = 48000;
  const samples = sine(sampleRate);

  it('exposes the accepted targets, and every one of them is accepted', () => {
    const names = masteringPlatformNames();
    expect(names).toContain('streaming');
    expect(names).toContain('broadcast');
    expect(names).toContain('club');
    // The discoverable list is the list that works: a name from it must never
    // be rejected by the entry point that consumes it.
    for (const name of names) {
      expect(() =>
        masteringAssistantSuggest({ samples, sampleRate, params: { targetPlatform: name } }),
      ).not.toThrow();
    }
  });

  it('returns the streaming default when no target is given', () => {
    expect(masteringAssistantSuggest({ samples, sampleRate })).toContain(
      '"loudness.targetLufs":-14',
    );
  });

  it('follows a named delivery target', () => {
    const json = masteringAssistantSuggest({
      samples,
      sampleRate,
      params: { targetPlatform: 'broadcast' },
    });
    expect(json).toContain('"loudness.targetLufs":-23');
  });

  it('moves the ceiling for a loud delivery format', () => {
    // Broadcast and podcast ask for the ceiling the default already carries, so
    // the ceiling is only observable on the loud formats.
    const json = masteringAssistantSuggest({
      samples,
      sampleRate,
      params: { targetPlatform: 'club' },
    });
    expect(json).toContain('"loudness.targetLufs":-9');
    expect(json).toContain('"loudness.ceilingDb":-0.3');
  });

  it('applies the target through the stereo entry point too', () => {
    const json = masteringAssistantSuggestStereo({
      left: samples,
      right: samples,
      sampleRate,
      params: { targetPlatform: 'broadcast' },
    });
    expect(json).toContain('"loudness.targetLufs":-23');
  });

  it('rejects an unknown target instead of silently keeping the default', () => {
    expect(() =>
      masteringAssistantSuggest({ samples, sampleRate, params: { targetPlatform: 'vinyl' } }),
    ).toThrow(/vinyl/);
  });

  it('rejects a numeric target: the index is a C-ABI transport detail', () => {
    expect(() =>
      // biome-ignore lint/suspicious/noExplicitAny: deliberately passing the wrong type.
      masteringAssistantSuggest({ samples, sampleRate, params: { targetPlatform: 2 as any } }),
    ).toThrow(/targetPlatform/);
  });

  it('leaves the other numeric params working alongside a target', () => {
    // An explicit loudness the caller chose wins over the target's suggestion.
    const json = masteringAssistantSuggest({
      samples,
      sampleRate,
      params: { targetPlatform: 'broadcast', targetLufs: -13, ceilingDb: -0.8 },
    });
    expect(json).toContain('"loudness.targetLufs":-13');
    expect(json).toContain('"loudness.ceilingDb":-0.8');
  });
});
