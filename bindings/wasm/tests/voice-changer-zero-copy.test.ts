/**
 * Tests for the zero-copy "prepared" path on RealtimeVoiceChanger.
 *
 * The prepared API exposes the WASM-heap-backed input/output scratch as
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

  it('mono prepared path matches processMonoInto bit-for-bit', () => {
    const changerA = new RealtimeVoiceChanger('bright-idol');
    changerA.prepare(SR, BLOCK, 1);
    const changerB = new RealtimeVoiceChanger('bright-idol');
    changerB.prepare(SR, BLOCK, 1);

    const input = new Float32Array(BLOCK);
    for (let i = 0; i < BLOCK; i++) {
      input[i] = 0.25 * Math.sin((2 * Math.PI * 440 * i) / SR);
    }

    // Reference path (allocating Into version).
    const outA = new Float32Array(BLOCK);
    changerA.processMonoInto(input, outA);

    // Prepared path.
    const inB = changerB.getMonoInputBuffer(BLOCK);
    inB.set(input);
    changerB.processPreparedMono(BLOCK);
    const outB = changerB.getMonoOutputBuffer(BLOCK);

    for (let i = 0; i < BLOCK; i++) {
      expect(outB[i]).toBe(outA[i]);
    }
    changerA.delete();
    changerB.delete();
  });

  it('input/output views are typed_memory_view onto the WASM heap', () => {
    const changer = new RealtimeVoiceChanger('bright-idol');
    changer.prepare(SR, BLOCK, 1);
    const inView = changer.getMonoInputBuffer(BLOCK);
    const outView = changer.getMonoOutputBuffer(BLOCK);
    expect(inView).toBeInstanceOf(Float32Array);
    expect(outView).toBeInstanceOf(Float32Array);
    expect(inView.length).toBe(BLOCK);
    expect(outView.length).toBe(BLOCK);
    // typed_memory_view shares the heap ArrayBuffer; mutating inView before
    // the next call must persist into the C++ scratch and be visible to the
    // next processPreparedMono.
    inView.fill(0);
    changer.processPreparedMono(BLOCK);
    // outView is now valid; values must be finite (silent input → small but
    // finite output after the DSP chain settles).
    for (let i = 0; i < BLOCK; i++) {
      expect(Number.isFinite(outView[i])).toBe(true);
    }
    changer.delete();
  });

  it('rejects out-of-range prepared call', () => {
    const changer = new RealtimeVoiceChanger('bright-idol');
    changer.prepare(SR, BLOCK, 1);
    // numSamples > maxBlockSize must throw.
    expect(() => changer.processPreparedMono(BLOCK * 4)).toThrow();
    changer.delete();
  });

  it('interleaved stereo prepared path matches processInterleavedInto', () => {
    const channels = 2;
    const changerA = new RealtimeVoiceChanger('bright-idol');
    changerA.prepare(SR, BLOCK, channels);
    const changerB = new RealtimeVoiceChanger('bright-idol');
    changerB.prepare(SR, BLOCK, channels);

    const interleaved = new Float32Array(BLOCK * channels);
    for (let frame = 0; frame < BLOCK; frame++) {
      interleaved[frame * 2 + 0] = 0.25 * Math.sin((2 * Math.PI * 440 * frame) / SR);
      interleaved[frame * 2 + 1] = 0.25 * Math.sin((2 * Math.PI * 660 * frame) / SR);
    }

    const outA = new Float32Array(BLOCK * channels);
    changerA.processInterleavedInto(interleaved, channels, outA);

    const inB = changerB.getInterleavedInputBuffer(BLOCK, channels);
    inB.set(interleaved);
    changerB.processPreparedInterleaved(BLOCK, channels);
    const outB = changerB.getInterleavedOutputBuffer(BLOCK, channels);

    for (let i = 0; i < BLOCK * channels; i++) {
      expect(outB[i]).toBe(outA[i]);
    }
    changerA.delete();
    changerB.delete();
  });

  it('planar stereo prepared path matches the interleaved prepared path', () => {
    const channels = 2;
    const changerA = new RealtimeVoiceChanger('bright-idol');
    changerA.prepare(SR, BLOCK, channels);
    const changerB = new RealtimeVoiceChanger('bright-idol');
    changerB.prepare(SR, BLOCK, channels);

    // Reference: interleaved prepared path.
    const inA = changerA.getInterleavedInputBuffer(BLOCK, channels);
    for (let frame = 0; frame < BLOCK; frame++) {
      inA[frame * 2 + 0] = 0.25 * Math.sin((2 * Math.PI * 440 * frame) / SR);
      inA[frame * 2 + 1] = 0.25 * Math.sin((2 * Math.PI * 660 * frame) / SR);
    }
    changerA.processPreparedInterleaved(BLOCK, channels);
    const outA = changerA.getInterleavedOutputBuffer(BLOCK, channels);

    // Planar prepared path.
    const leftIn = changerB.getPlanarChannelBuffer(0, BLOCK);
    const rightIn = changerB.getPlanarChannelBuffer(1, BLOCK);
    for (let frame = 0; frame < BLOCK; frame++) {
      leftIn[frame] = 0.25 * Math.sin((2 * Math.PI * 440 * frame) / SR);
      rightIn[frame] = 0.25 * Math.sin((2 * Math.PI * 660 * frame) / SR);
    }
    changerB.processPreparedPlanar(BLOCK);

    // Planar buffers carry output in place after processPreparedPlanar.
    for (let frame = 0; frame < BLOCK; frame++) {
      expect(leftIn[frame]).toBe(outA[frame * 2 + 0]);
      expect(rightIn[frame]).toBe(outA[frame * 2 + 1]);
    }
    changerA.delete();
    changerB.delete();
  });

  it('createRealtimePlanarBuffer wires up reusable views and a process thunk', () => {
    const channels = 2;
    const changer = new RealtimeVoiceChanger('bright-idol');
    changer.prepare(SR, BLOCK, channels);
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
    changer.delete();
  });

  it('voiceChangerAbiVersion is exported and finite', () => {
    const v = voiceChangerAbiVersion();
    expect(Number.isFinite(v)).toBe(true);
    expect(v).toBeGreaterThan(0);
  });
});
