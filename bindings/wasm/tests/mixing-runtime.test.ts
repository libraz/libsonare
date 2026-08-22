/**
 * Runtime coverage for the live Mixer controls exposed in WASM:
 * type contracts of meter/goniometer readers, string-union mapping for
 * pan law / meter tap / automation curve, and solo / solo-safe behavior.
 */

import { beforeAll, describe, expect, it } from 'vitest';
import { init, Mixer, mixingScenePresetJson } from '../dist/index.js';
import { assertStripIndex } from './_helpers';

const SR = 48000;
const BLOCK = 512;

function blockEnergy(r: { left: Float32Array; right: Float32Array }): number {
  let sum = 0;
  for (let i = 0; i < r.left.length; i++) {
    sum += r.left[i] * r.left[i] + r.right[i] * r.right[i];
  }
  return sum;
}

function silentBlocks(count: number): Float32Array[] {
  const blocks: Float32Array[] = [];
  for (let i = 0; i < count; i++) {
    blocks.push(new Float32Array(BLOCK));
  }
  return blocks;
}

function serialTailScene(): string {
  const delay = (ms: number) => ({
    slot: 'post',
    processor: 'effects.delay.stereo',
    params: JSON.stringify({
      delayTimeLMs: ms,
      delayTimeRMs: ms,
      feedback: 0,
      dryWet: 1,
    }),
  });
  return JSON.stringify({
    version: 1,
    strips: [
      {
        id: 'source',
        inserts: [delay(10)],
        sends: [{ id: 'to-aux', destinationBusId: 'aux', sendDb: 0, timing: 'post' }],
      },
    ],
    buses: [
      { id: 'aux', role: 'aux', inserts: [delay(20)] },
      { id: 'master', role: 'master', inserts: [delay(30)] },
    ],
    connections: [{ source: 'aux', destination: 'master' }],
  });
}

describe('Mixer runtime controls (WASM)', () => {
  beforeAll(async () => {
    await init();
  });

  describe('type contracts of readers and setters', () => {
    it('forwards asymmetric dual-pan and polarity fields through scene JSON', () => {
      const mixer = Mixer.fromSceneJson(
        JSON.stringify({
          version: 1,
          strips: [
            {
              id: 'asymmetric',
              panMode: 2,
              dualPanLeft: -0.25,
              dualPanRight: 0.75,
              polarityInvertLeft: true,
              polarityInvertRight: false,
            },
          ],
          buses: [{ id: 'master', role: 'master' }],
          connections: [],
        }),
        SR,
        BLOCK,
      );
      try {
        const scene = JSON.parse(mixer.toSceneJson()) as {
          strips: Array<{
            id: string;
            panMode: number;
            dualPanLeft: number;
            dualPanRight: number;
            polarityInvertLeft: boolean;
            polarityInvertRight: boolean;
          }>;
        };
        expect(scene.strips).toHaveLength(1);
        expect(scene.strips[0]).toMatchObject({
          id: 'asymmetric',
          panMode: 2,
          dualPanLeft: -0.25,
          dualPanRight: 0.75,
          polarityInvertLeft: true,
          polarityInvertRight: false,
        });
      } finally {
        mixer.delete();
      }
    });

    it('reports the longest serial send tail', () => {
      const mixer = Mixer.fromSceneJson(serialTailScene(), SR, BLOCK);
      try {
        expect(mixer.tailSamples()).toBeGreaterThan(1440);
      } finally {
        mixer.delete();
      }
    });

    it('exposes setters, meter readers, and goniometer with the documented shapes', () => {
      const mixer = Mixer.fromSceneJson(mixingScenePresetJson('vocalReverbSend'), SR, BLOCK);
      try {
        // stripById resolves a scene id to a numeric index in [0, stripCount()).
        const vocal = mixer.stripById('vocal');
        assertStripIndex(vocal, 'vocal');
        expect(typeof vocal).toBe('number');
        expect(Number.isInteger(vocal)).toBe(true);
        expect(vocal).toBeGreaterThanOrEqual(0);
        expect(vocal).toBeLessThan(mixer.stripCount());

        // Every new setter must accept its documented arguments without throwing.
        expect(() => mixer.setSoloed(vocal, false)).not.toThrow();
        expect(() => mixer.setSoloSafe(vocal, true)).not.toThrow();
        expect(() => mixer.setPolarityInvert(vocal, false, true)).not.toThrow();
        expect(() => mixer.setPanLaw(vocal, 'const4.5dB')).not.toThrow();
        expect(() => mixer.setChannelDelaySamples(vocal, 0)).not.toThrow();
        expect(() => mixer.setVcaOffsetDb(vocal, -1.5)).not.toThrow();
        expect(() => mixer.setDualPan(vocal, -0.3, 0.4)).not.toThrow();
        expect(() =>
          mixer.setSurroundPan(vocal, { azimuth: -45, divergence: 0.25, lfe: 0.5 }),
        ).not.toThrow();

        const sendIndex = mixer.addSend(vocal, 'rt-send', 'vocal-verb', -20, 'postFader');
        expect(typeof sendIndex).toBe('number');
        expect(Number.isInteger(sendIndex)).toBe(true);
        expect(sendIndex).toBeGreaterThanOrEqual(0);
        expect(() => mixer.setSendDb(vocal, sendIndex, -12)).not.toThrow();

        // Automation schedulers accept the union curve type.
        expect(() => mixer.scheduleFaderAutomation(vocal, 0, -6, 'linear')).not.toThrow();
        expect(() => mixer.schedulePanAutomation(vocal, 0, 0.1, 'exponential')).not.toThrow();
        expect(() => mixer.scheduleWidthAutomation(vocal, 0, 1.2, 'linear')).not.toThrow();
        expect(() =>
          mixer.scheduleSendAutomation(vocal, sendIndex, 0, -18, 'exponential'),
        ).not.toThrow();

        expect(() => mixer.setFaderDb(vocal, Number.NaN)).toThrow();
        expect(() => mixer.setPan(vocal, Number.POSITIVE_INFINITY)).toThrow();
        expect(() => mixer.scheduleFaderAutomation(vocal, 0, Number.NaN, 'linear')).toThrow();
        expect(() => mixer.setFaderDb(vocal, -3)).not.toThrow();
        expect(() => mixer.setPan(vocal, 0.25)).not.toThrow();

        mixer.compile();

        // Drive one block so the meters and goniometer have data.
        const vocalL = new Float32Array(BLOCK);
        const vocalR = new Float32Array(BLOCK);
        for (let i = 0; i < BLOCK; i++) {
          const v = 0.5 * Math.sin((2 * Math.PI * 440 * i) / SR);
          vocalL[i] = v;
          vocalR[i] = v;
        }
        const returnL = new Float32Array(BLOCK);
        const returnR = new Float32Array(BLOCK);
        mixer.processStereo([vocalL, returnL], [vocalR, returnR]);

        for (const tap of ['preFader', 'postFader'] as const) {
          const meter = mixer.meterTap(vocal, tap);
          expect(typeof meter).toBe('object');
          for (const field of ['peakDbL', 'peakDbR', 'rmsDbL', 'rmsDbR', 'correlation'] as const) {
            expect(Number.isFinite(meter[field])).toBe(true);
          }

          const stripMeter = mixer.stripMeter(vocal, tap);
          expect(Number.isFinite(stripMeter.peakDbL)).toBe(true);
          expect(Number.isFinite(stripMeter.rmsDbL)).toBe(true);
        }

        const goniometer = mixer.readGoniometerLatest(vocal, 8);
        expect(Array.isArray(goniometer)).toBe(true);
        expect(goniometer.length).toBeGreaterThan(0);
        expect(goniometer.length).toBeLessThanOrEqual(8);
        for (const point of goniometer) {
          expect(Number.isFinite(point.left)).toBe(true);
          expect(Number.isFinite(point.right)).toBe(true);
        }

        // maxPoints is a request, not an allocation size. Sizing the working
        // vector from it directly meant a metering UI deriving the count from a
        // window size could take the module down -- and in WASM that is not a
        // catchable error, it is an out-of-memory abort of the whole instance,
        // so there would be nothing left to assert against. The buffer is
        // bounded by the strip's goniometer ring instead. Matches Node
        // (mixing-runtime.test.ts) and Python (test_mixing.py).
        //
        // These run against the built module: they fail against a `dist/` older
        // than the fix in src/wasm/bindings/mixing/mixing_automation.cpp --
        // most likely by aborting rather than by a clean assertion failure.
        const huge = mixer.readGoniometerLatest(vocal, Number.MAX_SAFE_INTEGER);
        expect(Array.isArray(huge)).toBe(true);
        expect(huge.length).toBeGreaterThan(0);
        expect(huge.length).toBeLessThanOrEqual(4096);
        expect(huge.every((point) => Number.isFinite(point.left))).toBe(true);

        // Anything that is not a finite non-negative integer is a catchable
        // error, never a silent clamp to zero and never an abort.
        for (const invalid of [
          -1,
          -0.5,
          1.5,
          Number.NaN,
          Number.POSITIVE_INFINITY,
          Number.MAX_VALUE,
        ]) {
          expect(() => mixer.readGoniometerLatest(vocal, invalid)).toThrow();
        }

        // Zero stays a legal empty read.
        expect(mixer.readGoniometerLatest(vocal, 0)).toEqual([]);
      } finally {
        mixer.delete();
      }
    });

    it('accepts every pan-law, meter-tap, and send-timing string union value', () => {
      const mixer = Mixer.fromSceneJson(mixingScenePresetJson('vocalReverbSend'), SR, BLOCK);
      try {
        const vocal = mixer.stripById('vocal');
        assertStripIndex(vocal, 'vocal');
        for (const law of ['const3dB', 'const4.5dB', 'const6dB', 'linear0dB'] as const) {
          expect(() => mixer.setPanLaw(vocal, law)).not.toThrow();
        }
        for (const timing of ['preFader', 'postFader'] as const) {
          expect(typeof mixer.addSend(vocal, `s-${timing}`, 'vocal-verb', -18, timing)).toBe(
            'number',
          );
        }
        mixer.compile();
        const vocalL = new Float32Array(BLOCK);
        vocalL[0] = 1;
        const out = mixer.processStereo(
          [vocalL, new Float32Array(BLOCK)],
          [vocalL, new Float32Array(BLOCK)],
        );
        // 'const6dB' / 'postFader' resolved end-to-end and produced audio.
        expect(blockEnergy(out)).toBeGreaterThan(0);
        expect(Number.isFinite(mixer.meterTap(vocal, 'preFader').peakDbL)).toBe(true);
      } finally {
        mixer.delete();
      }
    });

    it('removeSend drops a previously-added send and shifts higher indices down', () => {
      const mixer = Mixer.fromSceneJson(mixingScenePresetJson('vocalReverbSend'), SR, BLOCK);
      try {
        const vocal = mixer.stripById('vocal');
        assertStripIndex(vocal, 'vocal');
        // Add two sends to the same bus; remove the first so the second shifts
        // down into index 0.
        const first = mixer.addSend(vocal, 'rt-send-a', 'vocal-verb', -20, 'postFader');
        const second = mixer.addSend(vocal, 'rt-send-b', 'vocal-verb', -24, 'postFader');
        expect(second).toBeGreaterThan(first);

        // setSendDb on the highest index is valid before removal.
        expect(() => mixer.setSendDb(vocal, second, -18)).not.toThrow();

        // Remove the first send: the second shifts down by one, so the old
        // highest index is now out of range and addressing it throws.
        expect(() => mixer.removeSend(vocal, first)).not.toThrow();
        expect(() => mixer.setSendDb(vocal, second, -18)).toThrow();
        // The shifted-down send is still addressable at the lower index.
        expect(() => mixer.setSendDb(vocal, first, -18)).not.toThrow();

        mixer.compile();
        const vocalL = new Float32Array(BLOCK);
        vocalL[0] = 1;
        const out = mixer.processStereo(
          [vocalL, new Float32Array(BLOCK)],
          [vocalL, new Float32Array(BLOCK)],
        );
        expect(blockEnergy(out)).toBeGreaterThan(0);
      } finally {
        mixer.delete();
      }
    });
  });

  describe('solo and solo-safe', () => {
    // The drum strips all route through a shared bus whose inserts (parallel
    // compressor + tape) carry internal state. To compare energy between two
    // input strips without cross-contamination, each measurement uses a fresh
    // mixer with kick soloed and the bus return marked solo-safe.
    function measureWithSoloedKick(feedStripId: string): number {
      const mixer = Mixer.fromSceneJson(mixingScenePresetJson('drumBusSubgroup'), SR, BLOCK);
      try {
        const kick = mixer.stripById('kick');
        const busReturn = mixer.stripById('drum-bus-return');
        const target = mixer.stripById(feedStripId);
        assertStripIndex(kick, 'kick');
        assertStripIndex(busReturn, 'drum-bus-return');
        assertStripIndex(target, feedStripId);
        expect(kick).toBeGreaterThanOrEqual(0);
        expect(busReturn).toBeGreaterThanOrEqual(0);
        expect(target).toBeGreaterThanOrEqual(0);

        // Keep the shared bus return in the path while kick is soloed.
        mixer.setSoloSafe(busReturn, true);
        mixer.setSoloed(kick, true);
        mixer.compile();

        const stripCount = mixer.stripCount();
        const left = silentBlocks(stripCount);
        const right = silentBlocks(stripCount);
        for (let i = 0; i < BLOCK; i++) {
          const v = 0.5 * Math.sin((2 * Math.PI * 220 * i) / SR);
          left[target][i] = v;
          right[target][i] = v;
        }

        // Process a few blocks to let the bus inserts settle, sum the tail energy.
        let energy = 0;
        for (let block = 0; block < 8; block++) {
          const out = mixer.processStereo(left, right);
          if (block >= 4) {
            energy += blockEnergy(out);
          }
        }
        return energy;
      } finally {
        mixer.delete();
      }
    }

    it('silences non-soloed strips in the master while solo-safe strips stay audible', () => {
      // Feeding the soloed strip (kick) reaches the master.
      const soloedEnergy = measureWithSoloedKick('kick');
      // Feeding a non-soloed, non-solo-safe source (snare) is implied-muted.
      const mutedEnergy = measureWithSoloedKick('snare');

      expect(soloedEnergy).toBeGreaterThan(1e-6);
      expect(mutedEnergy).toBeLessThan(soloedEnergy * 1e-3);
    });
  });
});
