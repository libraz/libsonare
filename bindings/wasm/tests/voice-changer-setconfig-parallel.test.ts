/**
 * Regression tests for the RealtimeVoiceChanger setConfig / process()
 * interaction.
 *
 * Background: commit 1d8e05e ("fix(wasm): Apply RealtimeVoiceChanger setConfig
 * from message handler, not process()") moved setConfig out of process() and
 * into the worklet's message handler. If that fix regresses, the most likely
 * failure modes are:
 *
 *   1. setConfig races a concurrent process_block and corrupts internal state
 *      (crash, or NaN / Inf samples appearing in the output).
 *   2. The reconfigure transient is so large that successive output blocks
 *      have wildly different energy, even with the same input — i.e. an audible
 *      pop on every preset switch.
 *
 * These tests exercise the underlying RealtimeVoiceChanger class directly
 * (the lowest layer that the worklet wraps) as well as the worklet processor
 * itself, alternating setConfig and process to make sure neither layer
 * crashes or emits non-finite samples.
 */

import { beforeAll, describe, expect, it } from 'vitest';
import {
  init as initWorklet,
  SonareRealtimeVoiceChangerWorkletProcessor,
} from '../dist/worklet.js';
import { init, RealtimeVoiceChanger } from '../src/index';

const SR = 48000;
const BLOCK = 128;

/** Fill `buf` with a deterministic, bounded sine so blocks are comparable. */
function fillSine(buf: Float32Array, freqHz: number, sr: number, amp = 0.2): void {
  for (let i = 0; i < buf.length; i++) {
    buf[i] = amp * Math.sin((2 * Math.PI * freqHz * i) / sr);
  }
}

function isAllFinite(buf: Float32Array): boolean {
  for (let i = 0; i < buf.length; i++) {
    if (!Number.isFinite(buf[i])) {
      return false;
    }
  }
  return true;
}

function rms(buf: Float32Array): number {
  let acc = 0;
  for (let i = 0; i < buf.length; i++) {
    acc += buf[i] * buf[i];
  }
  return Math.sqrt(acc / Math.max(1, buf.length));
}

describe('RealtimeVoiceChanger setConfig / process() interleaving (regression)', () => {
  beforeAll(async () => {
    await init();
    await initWorklet();
  });

  it('scenario A: setConfig interleaved between process blocks keeps output finite', () => {
    // Mirrors the original crash: process is running, host posts a config
    // change, process continues. The fix requires setConfig to be safe to
    // call between two process() invocations on the same instance.
    const changer = new RealtimeVoiceChanger('bright-idol');
    changer.prepare(SR, BLOCK, 1);
    try {
      const input = new Float32Array(BLOCK);
      fillSine(input, 440, SR);
      const output = new Float32Array(BLOCK);

      const presets = ['bright-idol', 'dark-villain', 'soft-whisper', 'deep-narrator'] as const;
      const totalBlocks = 16;
      for (let block = 0; block < totalBlocks; block++) {
        // Swap presets every 4 blocks, right between process() calls. This is
        // the exact pattern the buggy implementation regressed on.
        if (block > 0 && block % 4 === 0) {
          changer.setConfig(presets[(block / 4) % presets.length]);
        }
        expect(() => changer.processMonoInto(input, output)).not.toThrow();
        expect(isAllFinite(output)).toBe(true);
      }
    } finally {
      changer.delete();
    }
  });

  it('scenario B: 100x setConfig/process alternation stays finite and crash-free', () => {
    // Exercises the high-frequency switching case where a UI control fires
    // setConfig on every render quantum. setConfig must be idempotent enough
    // and the DSP state must not accumulate NaN over many transitions.
    const changer = new RealtimeVoiceChanger('neutral-monitor');
    changer.prepare(SR, BLOCK, 1);
    try {
      const input = new Float32Array(BLOCK);
      fillSine(input, 220, SR);
      const output = new Float32Array(BLOCK);

      const presets = [
        'neutral-monitor',
        'bright-idol',
        'dark-villain',
        'soft-whisper',
        'deep-narrator',
        'robot-mascot',
      ] as const;
      for (let i = 0; i < 100; i++) {
        changer.setConfig(presets[i % presets.length]);
        expect(() => changer.processMonoInto(input, output)).not.toThrow();
        expect(isAllFinite(output)).toBe(true);
      }
    } finally {
      changer.delete();
    }
  });

  it('scenario C: RMS across preset switches stays within a reasonable envelope', () => {
    // After the reconfigure, the per-block RMS should not blow up (no /0,
    // no log(0), no runaway feedback). We do not assert preset *equality*
    // — different presets legitimately produce different gain — only that
    // every block's RMS sits inside a sane band.
    const changer = new RealtimeVoiceChanger('neutral-monitor');
    changer.prepare(SR, BLOCK, 1);
    try {
      const input = new Float32Array(BLOCK);
      fillSine(input, 330, SR, 0.15);
      const output = new Float32Array(BLOCK);

      const presets = ['neutral-monitor', 'bright-idol', 'dark-villain', 'soft-whisper'] as const;
      let maxBlockRms = 0;
      for (let block = 0; block < 32; block++) {
        if (block % 2 === 0) {
          changer.setConfig(presets[(block / 2) % presets.length]);
        }
        changer.processMonoInto(input, output);
        expect(isAllFinite(output)).toBe(true);
        const r = rms(output);
        // Per-block RMS must be finite and bounded; loose envelope only
        // catches catastrophic regressions (denormal storms, NaN propagation,
        // runaway IIR feedback), not normal preset-to-preset gain variation.
        expect(Number.isFinite(r)).toBe(true);
        expect(r).toBeLessThan(10.0);
        if (r > maxBlockRms) {
          maxBlockRms = r;
        }
      }
      // Sanity: the test signal is non-silent on input, so at least one
      // block should produce non-silent output (no all-zero regression).
      expect(maxBlockRms).toBeGreaterThan(0);
    } finally {
      changer.delete();
    }
  });

  it('scenario A (stereo): planar process + setConfig between blocks stays finite', () => {
    // Same shape as scenario A but on the planar stereo path the worklet
    // actually uses, so a regression that only shows up under the planar
    // codepath (e.g. per-channel scratch lifetime mismatch) is caught.
    const changer = new RealtimeVoiceChanger('bright-idol');
    changer.prepare(SR, BLOCK, 2);
    try {
      const leftIn = changer.getPlanarChannelBuffer(0, BLOCK);
      const rightIn = changer.getPlanarChannelBuffer(1, BLOCK);
      fillSine(leftIn, 440, SR);
      fillSine(rightIn, 660, SR);

      const presets = ['bright-idol', 'dark-villain', 'deep-narrator'] as const;
      for (let block = 0; block < 12; block++) {
        if (block > 0 && block % 3 === 0) {
          changer.setConfig(presets[(block / 3) % presets.length]);
        }
        // Re-prime input buffers (output overwrites them in place).
        fillSine(leftIn, 440, SR);
        fillSine(rightIn, 660, SR);
        expect(() => changer.processPreparedPlanar(BLOCK)).not.toThrow();
        expect(isAllFinite(leftIn)).toBe(true);
        expect(isAllFinite(rightIn)).toBe(true);
      }
    } finally {
      changer.delete();
    }
  });
});

describe('SonareRealtimeVoiceChangerWorkletProcessor setConfig interleaving (regression)', () => {
  beforeAll(async () => {
    await init();
    await initWorklet();
  });

  it('setConfig posted between process() blocks never causes a crash or non-finite sample', () => {
    // This is the worklet-level repro for the original bug: receiveMessage
    // applies the config synchronously, then the next process() must keep
    // running on the new config without producing NaN / Inf.
    const processor = new SonareRealtimeVoiceChangerWorkletProcessor({
      preset: 'neutral-monitor',
      sampleRate: SR,
      blockSize: BLOCK,
      channelCount: 1,
    });
    try {
      const input = new Float32Array(BLOCK);
      fillSine(input, 220, SR, 0.2);
      const output = new Float32Array(BLOCK);

      const presets = [
        'bright-idol',
        'dark-villain',
        'soft-whisper',
        'deep-narrator',
        'robot-mascot',
        'neutral-monitor',
      ] as const;

      const totalBlocks = 64;
      for (let block = 0; block < totalBlocks; block++) {
        // Post a setConfig roughly every other block to simulate a host
        // hammering on a control surface while audio is rendering.
        if (block % 2 === 1) {
          expect(() =>
            processor.receiveMessage({
              type: 'setConfig',
              preset: presets[block % presets.length],
            }),
          ).not.toThrow();
        }
        expect(processor.process([[input]], [[output]])).toBe(true);
        expect(isAllFinite(output)).toBe(true);
      }
    } finally {
      processor.destroy();
    }
  });

  it('100x setConfig/process alternation through the worklet is stable', () => {
    const processor = new SonareRealtimeVoiceChangerWorkletProcessor({
      preset: 'neutral-monitor',
      sampleRate: SR,
      blockSize: BLOCK,
      channelCount: 2,
    });
    try {
      const left = new Float32Array(BLOCK);
      const right = new Float32Array(BLOCK);
      fillSine(left, 180, SR, 0.15);
      fillSine(right, 220, SR, 0.15);
      const outLeft = new Float32Array(BLOCK);
      const outRight = new Float32Array(BLOCK);

      const presets = [
        'neutral-monitor',
        'bright-idol',
        'dark-villain',
        'soft-whisper',
        'deep-narrator',
      ] as const;
      for (let i = 0; i < 100; i++) {
        processor.receiveMessage({ type: 'setConfig', preset: presets[i % presets.length] });
        expect(processor.process([[left, right]], [[outLeft, outRight]])).toBe(true);
        expect(isAllFinite(outLeft)).toBe(true);
        expect(isAllFinite(outRight)).toBe(true);
      }
    } finally {
      processor.destroy();
    }
  });
});
