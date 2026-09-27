/**
 * Track sends, buses and strips through the realtime engine, and what the
 * monitor path returns beside the program output.
 */

import { beforeAll, describe, expect, it } from 'vitest';
import { ErrorCode, init, isSonareError, RealtimeEngine } from '../dist/index.js';

describe('Sonare WASM Module', () => {
  const rms = (data: Float32Array): number => {
    let sum = 0;
    for (const value of data) {
      sum += value * value;
    }
    return Math.sqrt(sum / data.length);
  };

  beforeAll(async () => {
    await init();
  });

  describe('sends, buses and strips', () => {
    it('routes track sends through buses', () => {
      const engine = new RealtimeEngine(48000, 256);
      const frames = 256 * 40;
      engine.setClips([
        {
          id: 1,
          trackId: 10,
          channels: [new Float32Array(frames).fill(1)],
          startPpq: 0,
          lengthSamples: frames,
        },
      ]);
      engine.setTrackBuses([{ busId: 1, gainDb: 0 }]);
      let duplicateBusError: unknown;
      try {
        engine.setTrackBuses([
          { busId: 1, gainDb: 0 },
          { busId: 1, gainDb: 0 },
        ]);
      } catch (error) {
        duplicateBusError = error;
      }
      expect(isSonareError(duplicateBusError)).toBe(true);
      if (!isSonareError(duplicateBusError)) {
        throw new Error('expected SonareError');
      }
      expect(duplicateBusError.code).toBe(ErrorCode.InvalidParameter);

      engine.setTrackLanes([{ trackId: 10, sends: [{ busId: 1, levelDb: 0, enabled: true }] }]);
      for (const lane of [
        { trackId: 10, sends: [{ busId: 99, levelDb: 0, enabled: true }] },
        {
          trackId: 10,
          sends: [
            { busId: 1, levelDb: 0, enabled: true },
            { busId: 1, levelDb: -6, enabled: true },
          ],
        },
        { trackId: 10, sends: [{ busId: 1, levelDb: 99, enabled: true }] },
      ]) {
        let laneError: unknown;
        try {
          engine.setTrackLanes([lane]);
        } catch (error) {
          laneError = error;
        }
        expect(isSonareError(laneError)).toBe(true);
        if (!isSonareError(laneError)) {
          throw new Error('expected SonareError');
        }
        expect(laneError.code).toBe(ErrorCode.InvalidParameter);
      }

      engine.play();
      let [out] = engine.process([new Float32Array(256)]);
      expect(out.at(-1)).toBeGreaterThan(2.82);
      expect(out.at(-1)).toBeLessThan(2.84);
      const meterTargets = new Set(engine.drainMeterTelemetry().map((record) => record.targetId));
      expect(meterTargets.has(1)).toBe(true);
      expect(meterTargets.has(33)).toBe(true);
      expect(meterTargets.has(0)).toBe(true);

      engine.setTrackLanes([{ trackId: 10, sends: [{ busId: 1, levelDb: -6.0206 }] }]);
      engine.seekSample(0);
      [out] = engine.process([new Float32Array(256)]);
      expect(out.at(-1)).toBeGreaterThan(2.11);
      expect(out.at(-1)).toBeLessThan(2.13);

      engine.setTrackLanes([{ trackId: 10, sends: [{ busId: 1, levelDb: 0, enabled: false }] }]);
      engine.seekSample(0);
      [out] = engine.process([new Float32Array(256)]);
      expect(out.at(-1)).toBeGreaterThan(1.41);
      expect(out.at(-1)).toBeLessThan(1.42);

      let badJsonError: unknown;
      try {
        engine.setBusStripJson(1, '{bad json');
      } catch (error) {
        badJsonError = error;
      }
      expect(isSonareError(badJsonError)).toBe(true);
      if (!isSonareError(badJsonError)) {
        throw new Error('expected SonareError');
      }
      expect(badJsonError.code).toBe(ErrorCode.InvalidFormat);
      engine.setBusStripJson(
        1,
        '{"version":1,"strips":[],"buses":[{"id":"1","inserts":[]}],"connections":[]}',
      );
      engine.destroy();
    });

    it('applies track strip JSON to a lane', () => {
      const engine = new RealtimeEngine(48000, 256);
      const frames = 256 * 4;
      engine.setClips([
        {
          id: 1,
          trackId: 10,
          channels: [new Float32Array(frames).fill(1)],
          startPpq: 0,
          lengthSamples: frames,
        },
        {
          id: 2,
          trackId: 20,
          channels: [new Float32Array(frames).fill(1)],
          startPpq: 0,
          lengthSamples: frames,
        },
      ]);
      engine.setTrackLanes([10, 20]);
      const sceneJson =
        '{"version":1,"strips":[{"id":"track-10","faderDb":-12,"panLaw":3}],"buses":[],"connections":[]}';
      engine.setTrackStripJson(10, sceneJson);
      let badJsonError: unknown;
      try {
        engine.setTrackStripJson(10, '{bad json');
      } catch (error) {
        badJsonError = error;
      }
      expect(isSonareError(badJsonError)).toBe(true);
      if (!isSonareError(badJsonError)) {
        throw new Error('expected SonareError');
      }
      expect(badJsonError.code).toBe(ErrorCode.InvalidFormat);
      let badProcessorError: unknown;
      try {
        engine.setTrackStripJson(
          10,
          '{"version":1,"strips":[{"id":"track-10","inserts":[{"slot":"pre","processor":"missing.processor","params":"{}"}]}],"buses":[],"connections":[]}',
        );
      } catch (error) {
        badProcessorError = error;
      }
      expect(isSonareError(badProcessorError)).toBe(true);
      if (!isSonareError(badProcessorError)) {
        throw new Error('expected SonareError');
      }
      expect(badProcessorError.code).toBe(ErrorCode.InvalidParameter);
      let badParamError: unknown;
      try {
        engine.setTrackStripJson(
          10,
          '{"version":1,"strips":[{"id":"track-10","inserts":[{"slot":"pre","processor":"eq.parametric","params":"{\\"band0.gainDb\\":\\"loud\\"}"}]}],"buses":[],"connections":[]}',
        );
      } catch (error) {
        badParamError = error;
      }
      expect(isSonareError(badParamError)).toBe(true);
      if (!isSonareError(badParamError)) {
        throw new Error('expected SonareError');
      }
      expect(badParamError.code).toBe(ErrorCode.InvalidParameter);
      let badBypassError: unknown;
      try {
        engine.setMasterStripInsertBypassed(0, true);
      } catch (error) {
        badBypassError = error;
      }
      expect(isSonareError(badBypassError)).toBe(true);
      if (!isSonareError(badBypassError)) {
        throw new Error('expected SonareError');
      }
      expect(badBypassError.code).toBe(ErrorCode.InvalidParameter);

      engine.play();
      const processed = engine.process([new Float32Array(256)]);
      expect(processed[0].at(-1)).toBeGreaterThan(1.2);
      expect(processed[0].at(-1)).toBeLessThan(1.4);
      engine.destroy();
    });

    it('toggles track strip insert bypass', () => {
      const engine = new RealtimeEngine(48000, 256);
      const frames = 256 * 16;
      const source = new Float32Array(frames);
      for (let i = 0; i < frames; i += 1) {
        source[i] = Math.sin((2 * Math.PI * 1000 * i) / 48000);
      }
      engine.setClips([
        {
          id: 1,
          trackId: 10,
          channels: [source],
          startPpq: 0,
          lengthSamples: frames,
        },
      ]);
      engine.setTrackLanes([10]);
      engine.setTrackStripJson(
        10,
        '{"version":1,"strips":[{"id":"track-10","inserts":[{"slot":"pre","processor":"eq.parametric","params":"{\\"band0.type\\":1,\\"band0.frequencyHz\\":1000,\\"band0.gainDb\\":12,\\"band0.enabled\\":1}"}]}],"buses":[],"connections":[]}',
      );
      let badIndexError: unknown;
      try {
        engine.setTrackStripInsertBypassed(10, 7, true);
      } catch (error) {
        badIndexError = error;
      }
      expect(isSonareError(badIndexError)).toBe(true);
      if (!isSonareError(badIndexError)) {
        throw new Error('expected SonareError');
      }
      expect(badIndexError.code).toBe(ErrorCode.InvalidParameter);

      engine.play();
      let eqOut: Float32Array = new Float32Array(256);
      for (let block = 0; block < 6; block += 1) {
        [eqOut] = engine.process([new Float32Array(256)]);
      }
      engine.setTrackStripInsertBypassed(10, 0, true, true);
      engine.seekSample(0);
      const [bypassedOut] = engine.process([new Float32Array(256)]);
      expect(rms(eqOut)).toBeGreaterThan(rms(bypassedOut) * 1.5);
      engine.destroy();
    });

    it('updates track strip EQ band', () => {
      const engine = new RealtimeEngine(48000, 256);
      const frames = 256 * 16;
      const source = new Float32Array(frames);
      for (let i = 0; i < frames; i += 1) {
        source[i] = Math.sin((2 * Math.PI * 1000 * i) / 48000);
      }
      engine.setClips([
        {
          id: 1,
          trackId: 10,
          channels: [source],
          startPpq: 0,
          lengthSamples: frames,
        },
      ]);
      engine.setTrackLanes([10]);
      engine.setTrackStripJson(
        10,
        '{"version":1,"strips":[{"id":"track-10"}],"buses":[],"connections":[]}',
      );
      let badIndexError: unknown;
      try {
        engine.setTrackStripEqBand(10, 99, { type: 'Peak', enabled: true });
      } catch (error) {
        badIndexError = error;
      }
      expect(isSonareError(badIndexError)).toBe(true);
      if (!isSonareError(badIndexError)) {
        throw new Error('expected SonareError');
      }
      expect(badIndexError.code).toBe(ErrorCode.InvalidParameter);

      engine.play();
      const [flatOut] = engine.process([new Float32Array(256)]);
      engine.setTrackStripEqBand(10, 0, {
        type: 'Peak',
        frequencyHz: 1000,
        gainDb: 12,
        q: 1,
        enabled: true,
      });
      engine.seekSample(0);
      let eqOut: Float32Array = new Float32Array(256);
      for (let block = 0; block < 6; block += 1) {
        [eqOut] = engine.process([new Float32Array(256)]);
      }
      expect(rms(eqOut)).toBeGreaterThan(rms(flatOut) * 1.5);
      engine.destroy();
    });

    it('applies master strip JSON after lane mix', () => {
      const engine = new RealtimeEngine(48000, 256);
      const frames = 256 * 16;
      engine.setClips([
        {
          id: 1,
          channels: [new Float32Array(frames).fill(1)],
          startPpq: 0,
          lengthSamples: frames,
        },
        {
          id: 2,
          channels: [new Float32Array(frames).fill(1)],
          startPpq: 0,
          lengthSamples: frames,
        },
      ]);
      const sceneJson =
        '{"version":1,"strips":[{"id":"master","faderDb":-12,"panLaw":3}],"buses":[],"connections":[]}';
      engine.setMasterStripJson(sceneJson);
      let badJsonError: unknown;
      try {
        engine.setMasterStripJson('{bad json');
      } catch (error) {
        badJsonError = error;
      }
      expect(isSonareError(badJsonError)).toBe(true);
      if (!isSonareError(badJsonError)) {
        throw new Error('expected SonareError');
      }
      expect(badJsonError.code).toBe(ErrorCode.InvalidFormat);
      let badParamError: unknown;
      try {
        engine.setMasterStripJson(
          '{"version":1,"strips":[{"id":"master","inserts":[{"slot":"pre","processor":"eq.parametric","params":"{\\"band0.gainDb\\":\\"loud\\"}"}]}],"buses":[],"connections":[]}',
        );
      } catch (error) {
        badParamError = error;
      }
      expect(isSonareError(badParamError)).toBe(true);
      if (!isSonareError(badParamError)) {
        throw new Error('expected SonareError');
      }
      expect(badParamError.code).toBe(ErrorCode.InvalidParameter);

      engine.play();
      const processed = engine.process([new Float32Array(256)]);
      expect(processed[0].at(-1)).toBeGreaterThan(0.65);
      expect(processed[0].at(-1)).toBeLessThan(0.8);
      engine.setParameterSmoothed(0x4d58ff01, -24);
      engine.setParameter(0x4d58ff02, 0.25);
      let attenuated = processed;
      for (let block = 0; block < 8; block += 1) {
        attenuated = engine.process([new Float32Array(256)]);
      }
      expect(attenuated[0].at(-1)).toBeGreaterThan(0.05);
      expect(attenuated[0].at(-1)).toBeLessThan(0.25);
      engine.destroy();
    });

    it('updates master strip EQ band', () => {
      const engine = new RealtimeEngine(48000, 256);
      const frames = 256 * 16;
      const source = new Float32Array(frames);
      for (let i = 0; i < frames; i += 1) {
        source[i] = Math.sin((2 * Math.PI * 1000 * i) / 48000);
      }
      engine.setClips([
        {
          id: 1,
          channels: [source],
          startPpq: 0,
          lengthSamples: frames,
        },
      ]);
      engine.setMasterStripJson(
        '{"version":1,"strips":[{"id":"master"}],"buses":[],"connections":[]}',
      );
      let badIndexError: unknown;
      try {
        engine.setMasterStripEqBand(99, { type: 'Peak', enabled: true });
      } catch (error) {
        badIndexError = error;
      }
      expect(isSonareError(badIndexError)).toBe(true);
      if (!isSonareError(badIndexError)) {
        throw new Error('expected SonareError');
      }
      expect(badIndexError.code).toBe(ErrorCode.InvalidParameter);

      engine.play();
      const [flatOut] = engine.process([new Float32Array(256)]);
      engine.setMasterStripEqBand(0, {
        type: 'Peak',
        frequencyHz: 1000,
        gainDb: 12,
        q: 1,
        enabled: true,
      });
      engine.seekSample(0);
      let eqOut: Float32Array = new Float32Array(256);
      for (let block = 0; block < 6; block += 1) {
        [eqOut] = engine.process([new Float32Array(256)]);
      }
      expect(rms(eqOut)).toBeGreaterThan(rms(flatOut) * 1.5);
      engine.destroy();
    });

    it('updates bus strip pan and EQ band', () => {
      // Four whole 1 kHz periods per block, so a block's RMS is phase-independent.
      const blockSize = 192;
      const engine = new RealtimeEngine(48000, blockSize);
      const frames = blockSize * 128;
      const source = new Float32Array(frames);
      for (let i = 0; i < frames; i += 1) {
        source[i] = 0.25 * Math.sin((2 * Math.PI * 1000 * i) / 48000);
      }
      engine.setClips([
        {
          id: 1,
          trackId: 10,
          channels: [source, source],
          startPpq: 0,
          lengthSamples: frames,
        },
      ]);
      engine.setTrackBuses([{ busId: 1, gainDb: 0 }]);
      engine.setTrackLanes([{ trackId: 10, outputBusId: 1 }]);
      const settle = (): Float32Array[] => {
        let out: Float32Array[] = [];
        for (let block = 0; block < 16; block += 1) {
          out = engine.process([new Float32Array(blockSize), new Float32Array(blockSize)]);
        }
        return out;
      };
      const expectInvalidParameter = (call: () => void): void => {
        let caught: unknown;
        try {
          call();
        } catch (error) {
          caught = error;
        }
        expect(isSonareError(caught)).toBe(true);
        if (!isSonareError(caught)) {
          throw new Error('expected SonareError');
        }
        expect(caught.code).toBe(ErrorCode.InvalidParameter);
      };
      engine.play();
      const [flatLeft, flatRight] = settle();
      expect(rms(flatLeft)).toBeGreaterThan(0.05);
      expect(rms(flatRight)).toBeCloseTo(rms(flatLeft), 5);

      engine.setBusStripPan(1, 1);
      const [pannedLeft, pannedRight] = settle();
      expect(rms(pannedLeft)).toBeLessThan(rms(flatLeft) * 0.01);
      expect(rms(pannedRight)).toBeGreaterThan(rms(flatRight) * 0.9);

      engine.setBusStripPanMode(1, 'dualPan');
      engine.setBusStripDualPan(1, -1, -1);
      const [dualLeft, dualRight] = settle();
      expect(rms(dualRight)).toBeLessThan(rms(flatRight) * 0.01);
      expect(rms(dualLeft)).toBeGreaterThan(rms(flatLeft) * 0.9);
      engine.setBusStripPanLaw(1, 'const6dB');
      engine.setBusStripPanMode(1, 'balance');
      engine.setBusStripPan(1, 0);
      const [centredLeft] = settle();
      expect(rms(centredLeft)).toBeCloseTo(rms(flatLeft), 3);

      engine.setBusStripEqBand(1, 0, {
        type: 'Peak',
        frequencyHz: 1000,
        gainDb: 12,
        q: 1,
        enabled: true,
      });
      const [boostedLeft] = settle();
      expect(rms(boostedLeft)).toBeGreaterThan(rms(flatLeft) * 1.5);
      engine.setBusStripEqBandJson(1, 0, '{"type":"Peak","frequencyHz":1000,"gainDb":0}');
      const [restoredLeft] = settle();
      expect(rms(restoredLeft)).toBeCloseTo(rms(flatLeft), 3);

      expectInvalidParameter(() => engine.setBusStripPan(99, 0.5));
      expectInvalidParameter(() => engine.setBusStripPanLaw(99, 'const3dB'));
      expectInvalidParameter(() => engine.setBusStripPanMode(99, 'balance'));
      expectInvalidParameter(() => engine.setBusStripDualPan(99, -1, 1));
      expectInvalidParameter(() => engine.setBusStripEqBand(99, 0, { type: 'Peak' }));
      expectInvalidParameter(() => engine.setBusStripEqBand(1, 99, { type: 'Peak' }));
      expectInvalidParameter(() => engine.setBusStripEqBandJson(1, -1, '{"type":"Peak"}'));
      // A surround bus has no stereo image to pan; its EQ is still addressable.
      engine.setTrackBuses([
        { busId: 1, gainDb: 0 },
        { busId: 2, gainDb: 0, channelLayout: 2 },
      ]);
      expectInvalidParameter(() => engine.setBusStripPan(2, 0.5));
      expectInvalidParameter(() => engine.setBusStripDualPan(2, -1, 0));
      engine.setBusStripEqBand(2, 0, { type: 'Peak', frequencyHz: 500, gainDb: 3 });
      engine.destroy();
    });

    it('processWithMonitor returns output and monitor buses', () => {
      const engine = new RealtimeEngine(48000, 16);
      const result = engine.processWithMonitor([
        new Float32Array(16).fill(0.25),
        new Float32Array(16).fill(-0.25),
      ]);
      expect(result.output).toHaveLength(2);
      expect(result.monitor).toHaveLength(2);
      expect(result.output[0][0]).toBeCloseTo(0.25);
      expect(result.output[1][0]).toBeCloseTo(-0.25);
      expect(result.monitor[0][0]).toBeCloseTo(0);
      expect(result.monitor[1][0]).toBeCloseTo(0);
      engine.destroy();
    });
  });
});
