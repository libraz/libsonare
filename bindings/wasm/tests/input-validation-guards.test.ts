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

describe('RealtimeEngine clip/parameter/metronome guards mirror the C ABI', () => {
  const validClip = () => ({
    id: 1,
    channels: [new Float32Array(128).fill(0.1)],
    startPpq: 0,
    lengthSamples: 128,
  });

  it('rejects a non-finite or negative clip gain', () => {
    const engine = new RealtimeEngine(48000, 128);
    expect(() => engine.setClips([{ ...validClip(), gain: Number.NaN }])).toThrow();
    expect(() => engine.setClips([{ ...validClip(), gain: -1 }])).toThrow();
    // A valid non-negative gain is accepted.
    expect(() => engine.setClips([{ ...validClip(), gain: 0.5 }])).not.toThrow();
  });

  it('rejects negative clip fade lengths', () => {
    const engine = new RealtimeEngine(48000, 128);
    expect(() => engine.setClips([{ ...validClip(), fadeInSamples: -1 }])).toThrow();
    expect(() => engine.setClips([{ ...validClip(), fadeOutSamples: -1 }])).toThrow();
  });

  it('rejects a non-finite setParameter / setParameterSmoothed value', () => {
    const engine = new RealtimeEngine(48000, 128);
    expect(() => engine.setParameter(7, Number.NaN, 0)).toThrow();
    expect(() => engine.setParameter(7, Number.POSITIVE_INFINITY, 0)).toThrow();
    expect(() => engine.setParameterSmoothed(7, Number.NaN, 0)).toThrow();
  });

  it('rejects an inverted addParameter range', () => {
    const engine = new RealtimeEngine(48000, 128);
    expect(() =>
      engine.addParameter({
        id: 7,
        name: 'gain',
        unit: 'dB',
        minValue: 1,
        maxValue: 0,
        defaultValue: 0,
        rtSafe: true,
        defaultCurve: 1,
      }),
    ).toThrow();
  });

  it('rejects negative metronome gains or click length', () => {
    const engine = new RealtimeEngine(48000, 128);
    expect(() => engine.setMetronome({ enabled: true, beatGain: -1 })).toThrow();
    expect(() => engine.setMetronome({ enabled: true, clickSamples: -1 })).toThrow();
    expect(() => engine.setMetronome({ enabled: true, beatGain: 0.3 })).not.toThrow();
  });

  it('rejects a non-finite MIDI clip startPpq', () => {
    const engine = new RealtimeEngine(48000, 128);
    engine.setBuiltinInstrument({ gain: 0.5 }, 5);
    expect(() =>
      engine.setMidiClips([
        {
          id: 1,
          trackId: 5,
          destinationId: 5,
          lengthSamples: 8192,
          startPpq: Number.NaN,
          events: [],
        },
      ]),
    ).toThrow();
  });

  it('rejects a non-positive renderOffline block size', () => {
    const engine = new RealtimeEngine(48000, 128);
    expect(() => engine.renderOffline([new Float32Array(128)], 0)).toThrow();
    expect(() => engine.renderOffline([new Float32Array(128)], -1)).toThrow();
  });
});

describe('StreamingRetune sanitizes non-finite input', () => {
  it('does not propagate NaN into the grain history', async () => {
    const { StreamingRetune } = await import('../dist/index.js');
    const retune = new StreamingRetune({ semitones: 2 });
    retune.prepare(48000, 256);
    const bad = new Float32Array(256).fill(0.1);
    bad[10] = Number.NaN;
    bad[20] = Number.POSITIVE_INFINITY;
    const first = retune.processMono(bad);
    expect(Array.from(first).every((v) => Number.isFinite(v))).toBe(true);
    // A subsequent clean block must also stay finite (no poisoned ring state).
    const clean = retune.processMono(new Float32Array(256).fill(0.1));
    expect(Array.from(clean).every((v) => Number.isFinite(v))).toBe(true);
  });
});
