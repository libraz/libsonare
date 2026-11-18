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
    const result = masteringAssistantSuggest({
      samples,
      sampleRate,
      targetPlatform: 'broadcast',
    });
    expect(result.chainConfig.loudness?.targetLufs).toBe(-23);
  });

  it('moves the ceiling for a loud delivery format', () => {
    // Broadcast and podcast ask for the ceiling the default already carries, so
    // the ceiling is only observable on the loud formats.
    const result = masteringAssistantSuggest({
      samples,
      sampleRate,
      targetPlatform: 'club',
    });
    expect(result.chainConfig.loudness?.targetLufs).toBe(-9);
    expect(result.chainConfig.loudness?.ceilingDbtp).toBeCloseTo(-0.3, 5);
  });

  it('applies the target through the stereo entry point too', () => {
    const result = masteringAssistantSuggestStereo({
      left: samples,
      right: samples,
      sampleRate,
      targetPlatform: 'broadcast',
    });
    expect(result.chainConfig.loudness?.targetLufs).toBe(-23);
  });

  it('rejects an unknown target instead of silently keeping the default', () => {
    expect(() =>
      masteringAssistantSuggest({ samples, sampleRate, targetPlatform: 'vinyl' as never }),
    ).toThrow(/vinyl/);
  });

  it('rejects a numeric target: the index is a C-ABI transport detail', () => {
    expect(() =>
      // biome-ignore lint/suspicious/noExplicitAny: deliberately passing the wrong type.
      masteringAssistantSuggest({ samples, sampleRate, targetPlatform: 2 as any }),
    ).toThrow(/targetPlatform/);
  });

  it('leaves the other numeric params working alongside a target', () => {
    // An explicit loudness the caller chose wins over the target's suggestion.
    const result = masteringAssistantSuggest({
      samples,
      sampleRate,
      targetPlatform: 'broadcast',
      targetLufs: -13,
      ceilingDbtp: -0.8,
    });
    expect(result.chainConfig.loudness?.targetLufs).toBe(-13);
    expect(result.chainConfig.loudness?.ceilingDbtp).toBeCloseTo(-0.8, 5);
  });

  it('an explicit loudness wins even when it equals the default', () => {
    // The precedence used to be decided by comparing the value against the
    // default, so this one slider position -- the position a host UI sits at
    // before the user touches it -- silently lost to the delivery target while
    // every other position won. -1 is the default ceiling, which is the same
    // collision on the other field.
    const result = masteringAssistantSuggest({
      samples,
      sampleRate,
      targetPlatform: 'broadcast',
      targetLufs: -14,
      ceilingDbtp: -1,
    });
    expect(result.chainConfig.loudness?.targetLufs).toBe(-14);
    expect(result.chainConfig.loudness?.ceilingDbtp).toBe(-1);
  });

  it('a target still fills in the field the caller left out', () => {
    // Presence, not value: naming one field must not suppress the target on the
    // other.
    const result = masteringAssistantSuggest({
      samples,
      sampleRate,
      targetPlatform: 'club',
      targetLufs: -14,
    });
    expect(result.chainConfig.loudness?.targetLufs).toBe(-14);
    expect(result.chainConfig.loudness?.ceilingDbtp).toBeCloseTo(-0.3, 5);
  });
});
