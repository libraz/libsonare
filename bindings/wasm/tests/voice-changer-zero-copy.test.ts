/**
 * Tests for the zero-copy "prepared" path on RealtimeVoiceChanger.
 *
 * The realtime buffer API exposes the WASM-heap-backed input/output scratch as
 * typed_memory_view Float32Arrays, so AudioWorklet code can fill the input
 * view directly and read the output view back without any JS↔C++ per-sample
 * crossings or per-call allocations.
 */

import { beforeAll, describe, expect, it } from 'vitest';
import { init, RealtimeVoiceChanger, voiceChangerAbiVersion } from '../src/index';

const SR = 48000;
const BLOCK = 128;

describe('RealtimeVoiceChanger prepared (zero-copy) API', () => {
  beforeAll(async () => {
    await init();
  });

  const make = (channels: 1 | 2 = 1, config: 'brightIdol' | 'neutralMonitor' = 'brightIdol') =>
    new RealtimeVoiceChanger({
      sampleRate: SR,
      maxBlockSize: BLOCK,
      numChannels: channels,
      config,
    });

  it('is prepared directly by the constructor', () => {
    const changer = make(1, 'neutralMonitor');
    try {
      expect(() => changer.processMono(new Float32Array(BLOCK))).not.toThrow();
    } finally {
      changer.destroy();
    }
  });

  it('mono prepared path matches processMonoInto bit-for-bit', () => {
    const changerA = make();
    const changerB = make();

    const input = new Float32Array(BLOCK);
    for (let i = 0; i < BLOCK; i++) {
      input[i] = 0.25 * Math.sin((2 * Math.PI * 440 * i) / SR);
    }

    // Reference path (allocating Into version).
    const outA = new Float32Array(BLOCK);
    changerA.processMonoInto(input, outA);

    // Prepared path.
    const bufB = changerB.createRealtimeMonoBuffer(BLOCK);
    bufB.input.set(input);
    bufB.process();
    const outB = bufB.output;

    for (let i = 0; i < BLOCK; i++) {
      expect(outB[i]).toBe(outA[i]);
    }
    changerA.destroy();
    changerB.destroy();
  });

  it('input/output views are typed_memory_view onto the WASM heap', () => {
    const changer = make();
    const buf = changer.createRealtimeMonoBuffer(BLOCK);
    const inView = buf.input;
    const outView = buf.output;
    expect(inView).toBeInstanceOf(Float32Array);
    expect(outView).toBeInstanceOf(Float32Array);
    expect(inView.length).toBe(BLOCK);
    expect(outView.length).toBe(BLOCK);
    // typed_memory_view shares the heap ArrayBuffer; mutating inView before
    // the next call must persist into the C++ scratch and be visible to the
    // next process().
    inView.fill(0);
    buf.process();
    // outView is now valid; values must be finite (silent input -> small but
    // finite output after the DSP chain settles).
    for (let i = 0; i < BLOCK; i++) {
      expect(Number.isFinite(outView[i])).toBe(true);
    }
    changer.destroy();
  });

  it('rejects out-of-range prepared call', () => {
    const changer = make();
    // numSamples > maxBlockSize must throw.
    expect(() => changer.createRealtimeMonoBuffer(BLOCK * 4).process()).toThrow();
    changer.destroy();
  });

  it('rejects an interleaved channel count above the prepared layout', () => {
    // The changer is prepared for 1 channel; feeding 2 interleaved channels
    // exceeds the prepared maximum and must be rejected (matching the C-ABI
    // oracle, which rejects num_channels > handle->num_channels) instead of
    // silently growing the scratch buffers and processing a layout the changer
    // was never prepared for.
    const changer = make();
    const stereo = new Float32Array(BLOCK * 2);
    expect(() => changer.processInterleavedInto(stereo, 2, new Float32Array(BLOCK * 2))).toThrow();
    expect(() => changer.createRealtimeInterleavedBuffer(BLOCK, 2)).toThrow();
    // The prepared count still works.
    expect(() =>
      changer.processInterleavedInto(new Float32Array(BLOCK), 1, new Float32Array(BLOCK)),
    ).not.toThrow();
    changer.destroy();
  });

  it('accepts a narrower interleaved channel count than the prepared layout', () => {
    // Prepared for stereo, but a mono block (channels=1) must be accepted, just
    // like the C ABI / Node / Python surfaces (which accept 1 <= channels <=
    // prepared).
    const changer = make(2);
    const mono = new Float32Array(BLOCK);
    expect(() => changer.processInterleavedInto(mono, 1, new Float32Array(BLOCK))).not.toThrow();
    expect(() => changer.createRealtimeInterleavedBuffer(BLOCK, 1).process()).not.toThrow();
    // The full prepared width still works too.
    expect(() =>
      changer.processInterleavedInto(new Float32Array(BLOCK * 2), 2, new Float32Array(BLOCK * 2)),
    ).not.toThrow();
    // A count above the prepared maximum is still rejected.
    const wide = new Float32Array(BLOCK * 3);
    expect(() => changer.processInterleavedInto(wide, 3 as never, wide.slice())).toThrow();
    // A non-positive count is still rejected.
    expect(() => changer.processInterleavedInto(mono, 0 as never, mono.slice())).toThrow();
    changer.destroy();
  });

  it('interleaved stereo prepared path matches processInterleavedInto', () => {
    const channels = 2;
    const changerA = make(channels);
    const changerB = make(channels);

    const interleaved = new Float32Array(BLOCK * channels);
    for (let frame = 0; frame < BLOCK; frame++) {
      interleaved[frame * 2 + 0] = 0.25 * Math.sin((2 * Math.PI * 440 * frame) / SR);
      interleaved[frame * 2 + 1] = 0.25 * Math.sin((2 * Math.PI * 660 * frame) / SR);
    }

    const outA = new Float32Array(BLOCK * channels);
    changerA.processInterleavedInto(interleaved, channels, outA);

    const bufB = changerB.createRealtimeInterleavedBuffer(BLOCK, channels);
    bufB.input.set(interleaved);
    bufB.process();
    const outB = bufB.output;

    for (let i = 0; i < BLOCK * channels; i++) {
      expect(outB[i]).toBe(outA[i]);
    }
    changerA.destroy();
    changerB.destroy();
  });

  it('planar stereo prepared path matches the interleaved prepared path', () => {
    const channels = 2;
    const changerA = make(channels);
    const changerB = make(channels);

    // Reference: interleaved prepared path.
    const bufA = changerA.createRealtimeInterleavedBuffer(BLOCK, channels);
    const inA = bufA.input;
    for (let frame = 0; frame < BLOCK; frame++) {
      inA[frame * 2 + 0] = 0.25 * Math.sin((2 * Math.PI * 440 * frame) / SR);
      inA[frame * 2 + 1] = 0.25 * Math.sin((2 * Math.PI * 660 * frame) / SR);
    }
    bufA.process();
    const outA = bufA.output;

    // Planar prepared path.
    const bufB = changerB.createRealtimePlanarBuffer(BLOCK, channels);
    const leftIn = bufB.channels[0] as Float32Array;
    const rightIn = bufB.channels[1] as Float32Array;
    for (let frame = 0; frame < BLOCK; frame++) {
      leftIn[frame] = 0.25 * Math.sin((2 * Math.PI * 440 * frame) / SR);
      rightIn[frame] = 0.25 * Math.sin((2 * Math.PI * 660 * frame) / SR);
    }
    bufB.process();

    // Planar buffers carry output in place after process().
    for (let frame = 0; frame < BLOCK; frame++) {
      expect(leftIn[frame]).toBe(outA[frame * 2 + 0]);
      expect(rightIn[frame]).toBe(outA[frame * 2 + 1]);
    }
    changerA.destroy();
    changerB.destroy();
  });

  it('createRealtimePlanarBuffer wires up reusable views and a process thunk', () => {
    const channels = 2;
    const changer = make(channels);
    const buf = changer.createRealtimePlanarBuffer(BLOCK, channels);
    expect(buf.channels.length).toBe(channels);
    expect(buf.channels[0]?.length).toBe(BLOCK);
    expect(buf.channels[1]?.length).toBe(BLOCK);
    buf.channels[0]?.fill(0);
    buf.channels[1]?.fill(0);
    buf.process();
    for (let i = 0; i < BLOCK; i++) {
      expect(Number.isFinite(buf.channels[0]?.[i] ?? Number.NaN)).toBe(true);
      expect(Number.isFinite(buf.channels[1]?.[i] ?? Number.NaN)).toBe(true);
    }
    changer.destroy();
  });

  it('voiceChangerAbiVersion is exported and finite', () => {
    const v = voiceChangerAbiVersion();
    expect(Number.isFinite(v)).toBe(true);
    expect(v).toBeGreaterThan(0);
  });
});
