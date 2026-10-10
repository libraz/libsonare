/**
 * Track sends, buses and strips through the realtime engine, and what the
 * monitor path returns beside the program output.
 */

import { beforeAll, describe, expect, it } from 'vitest';
import { ErrorCode, init, isSonareError, RealtimeEngine } from '../dist/index.js';
import { sidechainCheckFromCode } from '../src/codes';
import { expectRefusalOf } from './_helpers';

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
  describe('prime, processor reset, tail and sidechain queries', () => {
    const SR = 48000;
    const BLOCK = 128;
    const TRACK = 10;
    const START = 24000;
    const RENDER = BLOCK * 75;
    const PLATE =
      '{"slot":"pre","processor":"effects.reverb.plate","params":{"decaySec":2.0,"modRateHz":1.0,"modDepthSamples":16,"dryWet":0.5}}';
    const stripJson = (id: string, insert: string): string =>
      `{"version":1,"strips":[{"id":"${id}","inserts":[${insert}]}],"buses":[]}`;
    const limiter = JSON.stringify({
      slot: 'pre',
      processor: 'dynamics.limiter',
      params: JSON.stringify({ thresholdDb: 0, lookaheadMs: 0, releaseMs: 50 }),
    });

    const expectInvalidParameter = (call: () => void): void => {
      expect(call).toThrow(expect.objectContaining({ code: ErrorCode.InvalidParameter }));
    };

    const burst = (): Float32Array[] => {
      const left = new Float32Array(96000);
      const right = new Float32Array(96000);
      for (let i = 0; i < left.length; i += 1) {
        const env = i % 4800 < 2400 ? 0.4 : 0;
        left[i] = env * Math.sin((2 * Math.PI * 330 * i) / SR);
        right[i] = env * Math.sin((2 * Math.PI * 495 * i) / SR);
      }
      return [left, right];
    };

    const plateEngine = (): RealtimeEngine => {
      const engine = new RealtimeEngine(SR, BLOCK);
      engine.setClips([{ trackId: TRACK, channels: burst(), startPpq: 0, lengthSamples: 96000 }]);
      engine.setTrackLanes([{ trackId: TRACK }]);
      engine.setTrackStripJson(TRACK, stripJson('s', PLATE));
      return engine;
    };

    const blocks = (engine: RealtimeEngine, count: number): Float32Array[][] => {
      const out: Float32Array[][] = [];
      for (let i = 0; i < count; i += 1) {
        out.push(engine.process([new Float32Array(BLOCK), new Float32Array(BLOCK)]));
      }
      return out;
    };

    const flatten = (chunks: Float32Array[][]): Float32Array[] =>
      [0, 1].map((ch) => {
        const joined = new Float32Array(chunks.length * BLOCK);
        chunks.forEach((chunk, i) => {
          joined.set(chunk[ch], i * BLOCK);
        });
        return joined;
      });

    const maxDiff = (a: Float32Array[], b: Float32Array[]): { diff: number; tol: number } => {
      let diff = 0;
      let peak = 0;
      for (let ch = 0; ch < a.length; ch += 1) {
        for (let i = 0; i < a[ch].length; i += 1) {
          diff = Math.max(diff, Math.abs(a[ch][i] - b[ch][i]));
          peak = Math.max(peak, Math.abs(a[ch][i]));
        }
      }
      expect(peak).toBeGreaterThan(0);
      return { diff, tol: 1e-6 * Math.max(1, peak) };
    };

    const freshPrimed = (): Float32Array[] => {
      const fresh = plateEngine();
      fresh.seekSample(START);
      fresh.primeOfflineParameters(2, BLOCK);
      const out = fresh.renderOffline([new Float32Array(RENDER), new Float32Array(RENDER)], BLOCK);
      fresh.destroy();
      return out;
    };

    const dirtyThenPlay = (reset: boolean): Float32Array[] => {
      const live = plateEngine();
      live.play();
      blocks(live, 100);
      live.stop();
      blocks(live, 37);
      live.seekSample(START);
      if (reset) {
        live.resetProcessorState();
      }
      live.play();
      const out = flatten(blocks(live, RENDER / BLOCK));
      live.destroy();
      return out;
    };

    it('resetProcessorState then play matches a fresh primed render', () => {
      const { diff, tol } = maxDiff(dirtyThenPlay(true), freshPrimed());
      expect(diff).toBeLessThanOrEqual(tol);
    });

    it('without resetProcessorState a ringing plate diverges', () => {
      const { diff, tol } = maxDiff(dirtyThenPlay(false), freshPrimed());
      expect(diff).toBeGreaterThan(tol);
    });

    it('primeOfflineParameters makes a repeated render start from the same state', () => {
      const engine = plateEngine();
      engine.play();
      blocks(engine, 100);
      engine.stop();
      engine.seekSample(START);
      engine.primeOfflineParameters(2, BLOCK);
      const first = engine.renderOffline(
        [new Float32Array(RENDER), new Float32Array(RENDER)],
        BLOCK,
      );
      engine.seekSample(START);
      engine.primeOfflineParameters(2, BLOCK);
      const second = engine.renderOffline(
        [new Float32Array(RENDER), new Float32Array(RENDER)],
        BLOCK,
      );
      engine.destroy();
      const { diff, tol } = maxDiff(second, first);
      expect(diff).toBeLessThanOrEqual(tol);
    });

    it('primeOfflineParameters refuses bad counts and resetProcessorState a full queue', () => {
      const engine = new RealtimeEngine(SR, BLOCK);
      expectRefusalOf(() => engine.primeOfflineParameters(0, BLOCK));
      expectRefusalOf(() => engine.primeOfflineParameters(2, 0));
      expectRefusalOf(() => engine.primeOfflineParameters(10_000, BLOCK));
      expect(() => engine.primeOfflineParameters('2' as unknown as number, BLOCK)).toThrow(
        /numChannels/,
      );
      expect(() => engine.resetProcessorState('0' as unknown as number)).toThrow();
      expect(() => {
        for (let i = 0; i < 100_000; i += 1) {
          engine.resetProcessorState();
        }
      }).toThrow(/queue/);
      engine.destroy();
    });

    it('names every sidechain refusal and keeps the setter in agreement', () => {
      const engine = new RealtimeEngine(SR, BLOCK);
      engine.setTrackBuses([
        { busId: 1, gainDb: 0 },
        { busId: 2, gainDb: 0 },
      ]);
      engine.setTrackLanes([{ trackId: 10 }, { trackId: 11 }]);
      const busStrip = (id: string): string =>
        `{"version":1,"strips":[],"buses":[{"id":"${id}","inserts":[${limiter}]}]}`;
      engine.setBusStripJson(1, busStrip('1'));
      engine.setBusStripJson(2, busStrip('2'));
      engine.setMasterStripJson(stripJson('master', limiter));
      engine.setTrackStripJson(10, stripJson('t10', limiter));
      engine.setTrackStripJson(11, stripJson('t11', limiter));

      const ok = { ok: true, reason: null };
      const no = (reason: string) => ({ ok: false, reason });
      expect(engine.canSetLaneSidechain(0, 0, 11)).toEqual(no('invalidTarget'));
      expect(engine.canSetLaneSidechain(10, 0, 10)).toEqual(no('selfKey'));
      expect(engine.canSetLaneSidechain(10, 0, 11)).toEqual(ok);
      expect(engine.canSetLaneSidechain(10, 0, 0)).toEqual(ok);
      expect(engine.canSetLaneSidechain(10, 0, 99)).toEqual(no('undeclaredSource'));
      expect(engine.canSetLaneSidechain(10, 1, 11)).toEqual(no('insertOutOfRange'));
      expectInvalidParameter(() => engine.setLaneSidechain(10, 0, 99));
      expectInvalidParameter(() => engine.setLaneSidechain(10, 1, 11));
      engine.setLaneSidechain(10, 0, 11);
      expect(engine.canSetLaneSidechain(11, 0, 10)).toEqual(no('cycle'));
      expectInvalidParameter(() => engine.setLaneSidechain(11, 0, 10));

      expect(engine.canSetBusSidechain(99, 0, 'bus', 1)).toEqual(no('invalidTarget'));
      expect(engine.canSetBusSidechain(1, 5, 'bus', 2)).toEqual(no('insertOutOfRange'));
      expect(engine.canSetBusSidechain(1, 0, 'bus', 77)).toEqual(no('undeclaredSource'));
      // The TypeScript layer refuses an unknown kind by name before the engine is asked.
      expect(() => engine.canSetBusSidechain(1, 0, 7, 2)).toThrow(RangeError);
      expect(engine.canSetBusSidechain(1, 0, 'bus', 1)).toEqual(no('selfKey'));
      expect(engine.canSetBusSidechain(1, 0, 'bus', 2)).toEqual(ok);
      engine.setBusSidechain(1, 0, 'bus', 2);
      expect(engine.canSetBusSidechain(2, 0, 'bus', 1)).toEqual(no('cycle'));
      expectInvalidParameter(() => engine.setBusSidechain(2, 0, 'bus', 1));

      expect(engine.canSetMasterSidechain(5, 'bus', 1)).toEqual(no('insertOutOfRange'));
      expect(engine.canSetMasterSidechain(0, 'bus', 77)).toEqual(no('undeclaredSource'));
      expect(() => engine.canSetMasterSidechain(0, 7, 1)).toThrow(RangeError);
      expect(engine.canSetMasterSidechain(0, 'track', 10)).toEqual(ok);

      expect(() => engine.canSetLaneSidechain('10' as unknown as number, 0, 11)).toThrow(/trackId/);
      expect(() => engine.canSetBusSidechain(1, 0, 'bus', '2' as unknown as number)).toThrow();
      engine.destroy();
    });

    it('refuses a reserved mixer id that names no strip and keeps the live ids', () => {
      const engine = new RealtimeEngine(SR, BLOCK);
      engine.setTrackLanes([{ trackId: 5 }, { trackId: 7 }]);
      const point = [{ ppq: 0, value: -6, curveToNext: 0 }];
      for (const positional of [0x4d580101, 0x4d580001, 0x4d58fe00]) {
        expectInvalidParameter(() => engine.setParameterSmoothed(positional, -6));
        expectInvalidParameter(() => engine.setParameter(positional, -6));
        expectInvalidParameter(() => engine.setAutomationLane(positional, point));
      }
      const laneFader = engine.resolveTrackLaneAutomationId(7, 'faderDb');
      expect(() => engine.setParameterSmoothed(laneFader, -6)).not.toThrow();
      expect(() => engine.setAutomationLane(laneFader, point)).not.toThrow();
      expect(() => engine.setParameterSmoothed(0x4d58ff01, -6)).not.toThrow();
      expect(() => engine.setAutomationLane(0x4d58ff01, point)).not.toThrow();
      engine.destroy();
    });

    it('refuses a negative render frame on the raw embind class', () => {
      const engine = new RealtimeEngine(SR, BLOCK);
      const native = (
        engine as unknown as {
          native: {
            setParameter(id: number, value: number, frame?: number): void;
            setParameterSmoothed(id: number, value: number, frame?: number): void;
            pushMidiNoteOn(
              destination: number,
              group: number,
              channel: number,
              note: number,
              velocity: number,
              frame?: number,
            ): void;
            resetProcessorState(frame?: number): void;
          };
        }
      ).native;
      const calls = [
        () => native.setParameter(0x4d58ff01, -6, -1),
        () => native.setParameterSmoothed(0x4d58ff01, -6, -1),
        () => native.pushMidiNoteOn(1, 0, 0, 60, 100, -1),
        () => native.resetProcessorState(-1),
      ];
      for (const call of calls) {
        expect(call).toThrow(RangeError);
        expect(call).toThrow(/renderFrame must not be negative/);
      }
      // Omitted means immediate, as does a frame of 0.
      expect(() => native.setParameter(0x4d58ff01, -6)).not.toThrow();
      expect(() => native.setParameter(0x4d58ff01, -6, 0)).not.toThrow();
      engine.destroy();
    });

    it('maps every refusal code to its name and rejects an unknown one', () => {
      expect(sidechainCheckFromCode(0)).toEqual({ ok: true, reason: null });
      const names = [
        'invalidTarget',
        'insertOutOfRange',
        'undeclaredSource',
        'invalidSourceKind',
        'selfKey',
        'cycle',
        'tableFull',
        'planRefused',
      ];
      names.forEach((name, i) => {
        expect(sidechainCheckFromCode(i + 1)).toEqual({ ok: false, reason: name });
      });
      expect(() => sidechainCheckFromCode(9)).toThrow(RangeError);
    });

    it('reports a tail from a delay insert and a latency in 1/256 samples', () => {
      const bare = new RealtimeEngine(SR, BLOCK);
      expect(bare.tailSamples()).toBe(0);
      expect(bare.graphLatencySamplesQ8()).toBe(0);
      bare.destroy();

      const engine = new RealtimeEngine(SR, BLOCK);
      engine.setTrackLanes([{ trackId: TRACK }]);
      engine.setTrackStripJson(
        TRACK,
        stripJson(
          's',
          '{"slot":"pre","processor":"effects.delay.stereo","params":{"feedback":0.6,"delayTimeLMs":37,"delayTimeRMs":53,"dryWet":0.5}}',
        ),
      );
      // The longer channel is 53 ms, so any bound is at least that long.
      expect(engine.tailSamples()).toBeGreaterThanOrEqual(Math.round(0.053 * SR));
      engine.destroy();
    });
  });
});
