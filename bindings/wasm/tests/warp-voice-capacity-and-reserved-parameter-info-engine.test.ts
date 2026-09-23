/**
 * Warp-voice capacity control-thread API and parameterInfo's reach into the
 * three reserved automation namespaces (instrument, insert, mixer).
 */

import { beforeAll, describe, expect, it } from 'vitest';
import {
  ErrorCode,
  init,
  isSonareError,
  masteringInsertParamInfo,
  RealtimeEngine,
} from '../dist/index.js';
import { SonareEngineTelemetryError } from '../dist/worklet.js';

describe('Sonare WASM Module', () => {
  beforeAll(async () => {
    await init();
  });

  describe('warp voice capacity', () => {
    it('sets and reads the capacity, rejecting out-of-domain values', () => {
      const engine = new RealtimeEngine(48000, 128);
      try {
        expect(engine.warpVoiceCapacity()).toBe(8);

        engine.setWarpVoiceCapacity(12);
        expect(engine.warpVoiceCapacity()).toBe(12);

        engine.setWarpVoiceCapacity(0);
        expect(engine.warpVoiceCapacity()).toBe(0);

        // Rejected values leave the previously accepted capacity (0) in place.
        expect(() => engine.setWarpVoiceCapacity(65)).toThrow();
        expect(engine.warpVoiceCapacity()).toBe(0);

        expect(() => engine.setWarpVoiceCapacity(-1)).toThrow();
        expect(engine.warpVoiceCapacity()).toBe(0);

        expect(() => engine.setWarpVoiceCapacity(1.5)).toThrow();
        expect(engine.warpVoiceCapacity()).toBe(0);
      } finally {
        engine.destroy();
      }
    });
  });

  describe('parameterInfo over the reserved namespaces', () => {
    it('describes a NativeSynth continuous parameter', () => {
      const engine = new RealtimeEngine(48000, 256);
      try {
        engine.setSynthInstrument('saw-lead', 1);
        const id = engine.resolveInstrumentAutomationId(1, 'cutoffHz');
        expect(id).toBeGreaterThanOrEqual(0);

        // Reads the built-in patch default, not whatever 'saw-lead' loaded --
        // the control thread cannot read the audio thread's live patch.
        const info = engine.parameterInfo(id);
        expect(info).toMatchObject({
          name: 'cutoffHz',
          unit: 'Hz',
          minValue: 10,
          maxValue: 22000,
          defaultValue: 12000,
          rtSafe: true,
        });
      } finally {
        engine.destroy();
      }
    });

    it('describes a compressor insert identically on a track, bus and master strip', () => {
      const engine = new RealtimeEngine(48000, 256);
      try {
        const compressorInsert = (slot: string) =>
          `{"slot":"${slot}","processor":"dynamics.compressor","params":"{\\"thresholdDb\\":-18,\\"ratio\\":2,\\"attackMs\\":10,\\"releaseMs\\":100,\\"kneeDb\\":0}"}`;
        // A leading pre-fader insert puts the compressor at index 1 on the
        // track/master strips, exercising the PreFader-then-PostFader count.
        const eqInsert = '{"slot":"pre","processor":"eq.parametric","params":"{}"}';

        engine.setTrackLanes([10]);
        engine.setTrackStripJson(
          10,
          `{"version":1,"strips":[{"id":"track-10","inserts":[${eqInsert},${compressorInsert('post')}]}],"buses":[],"connections":[]}`,
        );

        engine.setTrackBuses([{ busId: 1, gainDb: 0 }]);
        engine.setBusStripJson(
          1,
          `{"version":1,"strips":[],"buses":[{"id":"1","inserts":[${compressorInsert('post')}]}],"connections":[]}`,
        );

        engine.setMasterStripJson(
          `{"version":1,"strips":[{"id":"master","inserts":[${eqInsert},${compressorInsert('post')}]}],"buses":[],"connections":[]}`,
        );

        const trackId = engine.resolveTrackInsertAutomationId(10, 1, 'thresholdDb');
        const busId = engine.resolveBusInsertAutomationId(1, 0, 'thresholdDb');
        const masterId = engine.resolveMasterInsertAutomationId(1, 'thresholdDb');
        expect(trackId).toBeGreaterThanOrEqual(0);
        expect(busId).toBeGreaterThanOrEqual(0);
        expect(masterId).toBeGreaterThanOrEqual(0);

        // Ground truth read directly off the insert catalog, not through
        // parameterInfo -- the same catalog describe_reserved_parameter reads.
        const threshold = masteringInsertParamInfo('dynamics.compressor').find(
          (entry) => entry.name === 'thresholdDb',
        );
        if (!threshold) {
          throw new Error('dynamics.compressor has no thresholdDb entry');
        }
        const expectedDefault =
          typeof threshold.default === 'boolean'
            ? threshold.default
              ? 1
              : 0
            : (threshold.default ?? 0);

        for (const id of [trackId, busId, masterId]) {
          const info = engine.parameterInfo(id);
          expect(info.name).toBe('thresholdDb');
          expect(info.unit).toBe(threshold.unit ?? '');
          expect(info.minValue).toBe(threshold.min ?? Number.NEGATIVE_INFINITY);
          expect(info.maxValue).toBe(threshold.max ?? Number.POSITIVE_INFINITY);
          expect(info.defaultValue).toBe(expectedDefault);
        }
      } finally {
        engine.destroy();
      }
    });

    it('describes the lane fader target and rejects an unassigned reserved id', () => {
      const engine = new RealtimeEngine(48000, 256);
      try {
        engine.setTrackLanes([10]);
        // Reserved mixer namespace: 0x4D580000 | (laneIndex << 8) | kind, kind
        // 1 = faderDb (same encoding realtime-engine-mixer.test.ts's siblings use).
        const laneFaderId = 0x4d580001;
        const fader = engine.parameterInfo(laneFaderId);
        expect(fader).toMatchObject({
          name: 'faderDb',
          unit: 'dB',
          minValue: -120,
          maxValue: 24,
          defaultValue: 0,
        });

        // Instrument namespace: tag 0xC0000000, slot 31 -- no resolve call ever
        // minted it, so it stays unresolvable, same as route_engine_parameter.
        const unassignedId = 0xc01f0000;
        let error: unknown;
        try {
          engine.parameterInfo(unassignedId);
        } catch (caught) {
          error = caught;
        }
        expect(isSonareError(error)).toBe(true);
        if (!isSonareError(error)) {
          throw new Error('expected SonareError');
        }
        expect(error.code).toBe(ErrorCode.InvalidParameter);
      } finally {
        engine.destroy();
      }
    });
  });

  describe('telemetry error ordinals', () => {
    it('mirrors the core parameter-base-overflow code', () => {
      expect(SonareEngineTelemetryError.ParameterBaseOverflow).toBe(21);
    });
  });
});
