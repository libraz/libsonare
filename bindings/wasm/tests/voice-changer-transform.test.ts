/**
 * Voice transformer surface of RealtimeVoiceChanger: the twelve presets, MIDI notes and the
 * external vocoder carrier.
 */

import { beforeAll, describe, expect, it } from 'vitest';
import {
  ErrorCode,
  init,
  RealtimeVoiceChanger,
  type RealtimeVoiceChangerPodConfig,
  realtimeVoiceChangerPresetConfig,
  realtimeVoiceChangerPresetNames,
  SonareError,
  voiceCharacterPresetId,
} from '../src/index';

const SR = 48000;
const BLOCK = 128;

function basePod(): RealtimeVoiceChangerPodConfig {
  return realtimeVoiceChangerPresetConfig('neutral-monitor');
}

function externalPod(): RealtimeVoiceChangerPodConfig {
  return {
    ...basePod(),
    wetMix: 1,
    vocoderEnabled: true,
    vocoderCarrierSource: 'external',
    limiterEnableIspLimiter: false,
  };
}

function sine(frames: number, hz: number, gain: number, offset = 0): Float32Array {
  const out = new Float32Array(frames);
  for (let i = 0; i < frames; i++) {
    out[i] = gain * Math.sin((2 * Math.PI * hz * (i + offset)) / SR);
  }
  return out;
}

function saw(frames: number, hz: number, gain: number, offset = 0): Float32Array {
  const out = new Float32Array(frames);
  for (let i = 0; i < frames; i++) {
    out[i] = gain * (2 * ((((i + offset) * hz) / SR) % 1) - 1);
  }
  return out;
}

function expectInvalidState(call: () => unknown): void {
  let caught: unknown;
  try {
    call();
  } catch (error) {
    caught = error;
  }
  expect(caught).toBeInstanceOf(SonareError);
  expect((caught as SonareError).code).toBe(ErrorCode.InvalidState);
}

/** Runs `blocks` mono blocks and returns the concatenated output. */
function renderMono(
  changer: RealtimeVoiceChanger,
  blocks: number,
  carrierHz: number | null,
): Float32Array {
  const out = new Float32Array(blocks * BLOCK);
  for (let block = 0; block < blocks; block++) {
    const voice = sine(BLOCK, 220, 0.3, block * BLOCK);
    const rendered =
      carrierHz === null
        ? changer.processMono(voice)
        : changer.processMonoWithCarrier(voice, saw(BLOCK, carrierHz, 0.3, block * BLOCK));
    out.set(rendered, block * BLOCK);
  }
  return out;
}

function energy(samples: Float32Array): number {
  let sum = 0;
  for (const sample of samples) {
    sum += sample * sample;
  }
  return sum;
}

describe('RealtimeVoiceChanger voice transformer', () => {
  beforeAll(async () => {
    await init();
  });

  describe('presets', () => {
    it('lists the twelve presets and maps each ordinal to its id', () => {
      const names = realtimeVoiceChangerPresetNames();
      expect(names).toHaveLength(12);
      for (const [ordinal, id] of names.entries()) {
        expect(voiceCharacterPresetId(ordinal)).toBe(id);
        expect(realtimeVoiceChangerPresetConfig(ordinal)).toEqual(
          realtimeVoiceChangerPresetConfig(id),
        );
      }
      expect(voiceCharacterPresetId(12)).toBeNull();
      expect(() => realtimeVoiceChangerPresetConfig(12)).toThrow(RangeError);
    });

    it('builds the same chain from a preset id and from its flat POD', () => {
      for (const id of realtimeVoiceChangerPresetNames()) {
        const fromId = new RealtimeVoiceChanger(id);
        const fromPod = new RealtimeVoiceChanger(realtimeVoiceChangerPresetConfig(id));
        try {
          expect(fromPod.configJson(), id).toBe(fromId.configJson());
        } finally {
          fromId.delete();
          fromPod.delete();
        }
      }
    });
  });

  describe('MIDI notes', () => {
    it('accepts note events on a prepared psola changer', () => {
      const changer = new RealtimeVoiceChanger(basePod(), SR, BLOCK, 1);
      try {
        expect(() => changer.noteOn(60)).not.toThrow();
        expect(() => changer.noteOn(64, 0.5)).not.toThrow();
        expect(() => changer.noteOn(64, 1)).not.toThrow();
        expect(() => changer.noteOff(60)).not.toThrow();
        expect(() => changer.noteOff(100)).not.toThrow();
        expect(() => changer.allNotesOff()).not.toThrow();
        expect(() => changer.noteOn(0)).not.toThrow();
        expect(() => changer.noteOn(127)).not.toThrow();
        expect(() => changer.processMono(new Float32Array(BLOCK))).not.toThrow();
      } finally {
        changer.delete();
      }
    });

    it('refuses an out-of-domain note or velocity with RangeError and a wrong type with TypeError', () => {
      const changer = new RealtimeVoiceChanger(basePod(), SR, BLOCK, 1);
      try {
        for (const note of [-1, 128, 60.5, Number.NaN, Number.POSITIVE_INFINITY, 2 ** 31]) {
          expect(() => changer.noteOn(note), String(note)).toThrow(RangeError);
          expect(() => changer.noteOff(note), String(note)).toThrow(RangeError);
        }
        for (const velocity of [0, -0.1, 1.01, Number.NaN, Number.POSITIVE_INFINITY]) {
          expect(() => changer.noteOn(60, velocity), String(velocity)).toThrow(RangeError);
        }
        expect(() => changer.noteOn(true as unknown as number)).toThrow(TypeError);
        expect(() => changer.noteOn('60' as unknown as number)).toThrow(TypeError);
        expect(() => changer.noteOn(60, true as unknown as number)).toThrow(TypeError);
        expect(() => changer.noteOff(null as unknown as number)).toThrow(TypeError);
      } finally {
        changer.delete();
      }
    });

    it('reports InvalidState for a grain engine, an unprepared changer and a full event queue', () => {
      const grain = new RealtimeVoiceChanger(
        { ...basePod(), voiceEngineEngine: 'grain' },
        SR,
        BLOCK,
        1,
      );
      const unprepared = new RealtimeVoiceChanger(basePod());
      const psola = new RealtimeVoiceChanger(basePod(), SR, BLOCK, 1);
      try {
        expectInvalidState(() => grain.noteOn(60));
        expectInvalidState(() => grain.noteOff(60));
        expectInvalidState(() => unprepared.noteOn(60));

        // Nothing drains the queue while no block is processed.
        let refused: unknown;
        for (let i = 0; i < 4096 && refused === undefined; i++) {
          try {
            psola.noteOn(60 + (i % 12));
          } catch (error) {
            refused = error;
          }
        }
        expect(refused).toBeInstanceOf(SonareError);
        expect((refused as SonareError).code).toBe(ErrorCode.InvalidState);
        // allNotesOff bypasses the queue, so it still succeeds.
        expect(() => psola.allNotesOff()).not.toThrow();
      } finally {
        grain.delete();
        unprepared.delete();
        psola.delete();
      }
    });

    it('refuses note calls after delete', () => {
      const changer = new RealtimeVoiceChanger(basePod(), SR, BLOCK, 1);
      changer.delete();
      expectInvalidState(() => changer.noteOn(60));
      expectInvalidState(() => changer.noteOff(60));
      expectInvalidState(() => changer.allNotesOff());
    });
  });

  describe('external vocoder carrier', () => {
    it('refuses a carrier-less block while an external carrier is configured', () => {
      const changer = new RealtimeVoiceChanger(externalPod(), SR, BLOCK, 2);
      try {
        expectInvalidState(() => changer.processMono(new Float32Array(BLOCK)));
        expectInvalidState(() => changer.processInterleaved(new Float32Array(BLOCK * 2), 2));
        expectInvalidState(() => changer.processPreparedMono(BLOCK));
        changer.getPlanarChannelBuffer(0, BLOCK);
        changer.getPlanarChannelBuffer(1, BLOCK);
        expectInvalidState(() => changer.processPreparedPlanar(BLOCK));
        const output = new Float32Array(BLOCK).fill(7);
        expectInvalidState(() => changer.processMonoInto(new Float32Array(BLOCK), output));
        expect(output.every((sample) => sample === 7)).toBe(true);
      } finally {
        changer.delete();
      }
    });

    it('refuses a carrier whose length differs from the input', () => {
      const changer = new RealtimeVoiceChanger(externalPod(), SR, BLOCK, 2);
      try {
        const voice = new Float32Array(BLOCK);
        expect(() => changer.processMonoWithCarrier(voice, new Float32Array(BLOCK - 1))).toThrow(
          RangeError,
        );
        expect(() =>
          changer.processInterleavedWithCarrier(
            new Float32Array(BLOCK * 2),
            new Float32Array(BLOCK * 2),
            2,
          ),
        ).toThrow(RangeError);
      } finally {
        changer.delete();
      }
    });

    it('lets the carrier shape the vocoded output', () => {
      const low = new RealtimeVoiceChanger(externalPod(), SR, BLOCK, 1);
      const high = new RealtimeVoiceChanger(externalPod(), SR, BLOCK, 1);
      try {
        const lowOut = renderMono(low, 60, 110);
        const highOut = renderMono(high, 60, 440);
        expect(energy(lowOut)).toBeGreaterThan(0);
        expect(energy(highOut)).toBeGreaterThan(0);
        expect(Array.from(lowOut)).not.toEqual(Array.from(highOut));
      } finally {
        low.delete();
        high.delete();
      }
    });

    it('ignores the carrier while the source is internal', () => {
      const pod = { ...basePod(), vocoderEnabled: true };
      const plain = new RealtimeVoiceChanger(pod, SR, BLOCK, 1);
      const withCarrier = new RealtimeVoiceChanger(pod, SR, BLOCK, 1);
      try {
        const expected = renderMono(plain, 20, null);
        const actual = renderMono(withCarrier, 20, 330);
        expect(Array.from(actual)).toEqual(Array.from(expected));
      } finally {
        plain.delete();
        withCarrier.delete();
      }
    });

    it('matches across the copying and the zero-copy mono paths', () => {
      const copying = new RealtimeVoiceChanger(externalPod(), SR, BLOCK, 1);
      const prepared = new RealtimeVoiceChanger(externalPod(), SR, BLOCK, 1);
      try {
        const expected = renderMono(copying, 20, 220);
        const input = prepared.getMonoInputBuffer(BLOCK);
        const output = prepared.getMonoOutputBuffer(BLOCK);
        const carrier = prepared.getCarrierBuffer(BLOCK);
        const actual = new Float32Array(20 * BLOCK);
        for (let block = 0; block < 20; block++) {
          input.set(sine(BLOCK, 220, 0.3, block * BLOCK));
          carrier.set(saw(BLOCK, 220, 0.3, block * BLOCK));
          prepared.processPreparedMonoWithCarrier(BLOCK);
          actual.set(output, block * BLOCK);
        }
        expect(Array.from(actual)).toEqual(Array.from(expected));
      } finally {
        copying.delete();
        prepared.delete();
      }
    });

    it('shares one mono carrier across the channels of the interleaved and planar paths', () => {
      const interleaved = new RealtimeVoiceChanger(externalPod(), SR, BLOCK, 2);
      const planar = new RealtimeVoiceChanger(externalPod(), SR, BLOCK, 2);
      try {
        const left = sine(BLOCK, 220, 0.3);
        const right = sine(BLOCK, 330, 0.3);
        const carrier = saw(BLOCK, 165, 0.3);
        const frames = new Float32Array(BLOCK * 2);
        for (let i = 0; i < BLOCK; i++) {
          frames[i * 2] = left[i];
          frames[i * 2 + 1] = right[i];
        }
        let interleavedOut = new Float32Array(0);
        let planarLeft = new Float32Array(0);
        let planarRight = new Float32Array(0);
        const planarCarrier = planar.getCarrierBuffer(BLOCK);
        const planes = [
          planar.getPlanarChannelBuffer(0, BLOCK),
          planar.getPlanarChannelBuffer(1, BLOCK),
        ];
        for (let block = 0; block < 8; block++) {
          interleavedOut = interleaved.processInterleavedWithCarrier(frames, carrier, 2);
          planes[0].set(left);
          planes[1].set(right);
          planarCarrier.set(carrier);
          planar.processPreparedPlanarWithCarrier(BLOCK);
          planarLeft = planes[0].slice();
          planarRight = planes[1].slice();
        }
        for (let i = 0; i < BLOCK; i++) {
          expect(interleavedOut[i * 2]).toBe(planarLeft[i]);
          expect(interleavedOut[i * 2 + 1]).toBe(planarRight[i]);
        }
      } finally {
        interleaved.delete();
        planar.delete();
      }
    });
  });
});
