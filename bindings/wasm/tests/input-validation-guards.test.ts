/**
 * WASM input-validation guards that mirror the C ABI. The C-ABI TU is not
 * linked into the WASM build, so these wrappers must re-implement the guards
 * the C ABI performs — otherwise WASM would silently accept invalid input that
 * every other surface rejects.
 */

import { beforeAll, describe, expect, it } from 'vitest';
import { init, RealtimeEngine, RealtimeVoiceChanger } from '../dist/index.js';

beforeAll(async () => {
  await init();
});

describe('RealtimeEngine prepare/time-signature/loop guards', () => {
  it('rejects a non-positive sample rate or block size in the constructor', () => {
    expect(() => new RealtimeEngine(0, 128)).toThrow();
    expect(() => new RealtimeEngine(48000, 0)).toThrow();
  });

  it('rejects a non-positive sample rate or block size in prepare()', () => {
    const engine = new RealtimeEngine(48000, 128);
    expect(() => engine.prepare(0, 0)).toThrow();
    expect(() => engine.prepare(48000, 0)).toThrow();
  });

  it('rejects a non-positive time signature', () => {
    const engine = new RealtimeEngine(48000, 128);
    expect(() => engine.setTimeSignature(0, 4)).toThrow();
    expect(() => engine.setTimeSignature(4, 0)).toThrow();
    // A valid signature does not throw.
    expect(() => engine.setTimeSignature(3, 4)).not.toThrow();
  });

  it('rejects an invalid loop range', () => {
    const engine = new RealtimeEngine(48000, 128);
    expect(() => engine.setLoop(-1, 4, true)).toThrow();
    expect(() => engine.setLoop(4, 4, true)).toThrow(); // empty
    expect(() => engine.setLoop(8, 4, true)).toThrow(); // inverted
    expect(() => engine.setLoop(0, 4, true)).not.toThrow();
  });
});

describe('RealtimeVoiceChanger legacy block-size guard', () => {
  it('rejects a block larger than the prepared max block size', () => {
    const vc = new RealtimeVoiceChanger('neutral-monitor');
    vc.prepare(48000, 128, 1);
    // Within the prepared max: fine.
    expect(() => vc.processMono(new Float32Array(128))).not.toThrow();
    // Larger than the prepared max: must throw, not emit stale/garbage scratch.
    expect(() => vc.processMono(new Float32Array(256))).toThrow();
  });
});
