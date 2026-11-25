/**
 * Track sends, buses and strips through the realtime engine, and what the
 * monitor path returns beside the program output.
 */

import { beforeAll, describe, expect, it } from 'vitest';
import { ErrorCode, init, isSonareError, RealtimeEngine } from '../dist/index.js';
import { sidechainCheckFromCode } from '../src/codes';
import { refusalOf } from '../src/internal/entry.js';
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
      const engine = new RealtimeEngine({ sampleRate: 48000, maxBlockSize: 256 });
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
      expect(duplicateBusError).toBeInstanceOf(RangeError);
      expect(isSonareError(duplicateBusError)).toBe(false);

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
        expect(laneError).toBeInstanceOf(RangeError);
        expect(isSonareError(laneError)).toBe(false);
      }

      engine.play();
      let [out] = engine.process([new Float32Array(256)]);
      // Unit clip: direct 1 + unity send 1 (a centred mono strip is unity).
      expect(out.at(-1)).toBeGreaterThan(1.99);
      expect(out.at(-1)).toBeLessThan(2.01);
      const meterTargets = new Set(engine.drainMeterTelemetry().map((record) => record.targetId));
      expect(meterTargets.has(1)).toBe(true);
      expect(meterTargets.has(33)).toBe(true);
      expect(meterTargets.has(0)).toBe(true);

      engine.setTrackLanes([{ trackId: 10, sends: [{ busId: 1, levelDb: -6.0206 }] }]);
      engine.seekSample(0);
      [out] = engine.process([new Float32Array(256)]);
      // 1 + 10^(-6.0206/20) = 1.5.
      expect(out.at(-1)).toBeGreaterThan(1.49);
      expect(out.at(-1)).toBeLessThan(1.51);

      engine.setTrackLanes([{ trackId: 10, sends: [{ busId: 1, levelDb: 0, enabled: false }] }]);
      engine.seekSample(0);
      [out] = engine.process([new Float32Array(256)]);
      expect(out.at(-1)).toBeGreaterThan(0.99);
      expect(out.at(-1)).toBeLessThan(1.01);

      expect(() => engine.setBusStrip(1, '{bad json' as never)).toThrow(TypeError);
      engine.setBusStrip(
        1,
        JSON.parse('{"version":2,"strips":[],"buses":[{"id":"1","inserts":[]}],"connections":[]}')
          .buses[0],
      );
      engine.destroy();
    });

    it('applies track strip JSON to a lane', () => {
      const engine = new RealtimeEngine({ sampleRate: 48000, maxBlockSize: 256 });
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
      engine.setTrackLanes([{ trackId: 10 }, { trackId: 20 }]);
      const sceneJson =
        '{"version":2,"strips":[{"id":"track-10","faderDb":-12,"panLaw":"linear0Db"}],"buses":[],"connections":[]}';
      engine.setStrip(10, JSON.parse(sceneJson).strips[0]);
      expect(() => engine.setStrip(10, '{bad json' as never)).toThrow(TypeError);
      let badProcessorError: unknown;
      try {
        engine.setStrip(
          10,
          JSON.parse(
            '{"version":2,"strips":[{"id":"track-10","inserts":[{"slot":"pre","processor":"missing.processor","params":{}}]}],"buses":[],"connections":[]}',
          ).strips[0],
        );
      } catch (error) {
        badProcessorError = error;
      }
      expect(badProcessorError).toBeInstanceOf(RangeError);
      expect(isSonareError(badProcessorError)).toBe(false);
      let badParamError: unknown;
      try {
        engine.setStrip(
          10,
          JSON.parse(
            '{"version":2,"strips":[{"id":"track-10","inserts":[{"slot":"pre","processor":"eq.parametric","params":{"band0.gainDb":"loud"}}]}],"buses":[],"connections":[]}',
          ).strips[0],
        );
      } catch (error) {
        badParamError = error;
      }
      expect(badParamError).toBeInstanceOf(TypeError);
      expect(isSonareError(badParamError)).toBe(false);
      let badBypassError: unknown;
      try {
        engine.setStripInsertBypassed('master', 0, { bypassed: true });
      } catch (error) {
        badBypassError = error;
      }
      // The default master strip carries no insert, so index 0 is the argument at fault.
      expect(badBypassError).toBeInstanceOf(RangeError);
      expect(isSonareError(badBypassError)).toBe(false);

      engine.play();
      const processed = engine.process([new Float32Array(256)]);
      expect(processed[0].at(-1)).toBeGreaterThan(1.2);
      expect(processed[0].at(-1)).toBeLessThan(1.4);
      engine.destroy();
    });

    it('applies a master fader set before any setStrip, in realtime and in an offline bounce', () => {
      const frames = 256 * 64;
      const withClip = (masterFaderDb: number | undefined, body: (e: RealtimeEngine) => number) => {
        const engine = new RealtimeEngine({ sampleRate: 48000, maxBlockSize: 256 });
        try {
          engine.setClips([
            {
              id: 1,
              trackId: 10,
              channels: [new Float32Array(frames).fill(0.5)],
              startPpq: 0,
              lengthSamples: frames,
            },
          ]);
          engine.setTrackLanes([{ trackId: 10 }]);
          if (masterFaderDb !== undefined) {
            engine.setStripFaderDb('master', masterFaderDb);
          }
          return body(engine);
        } finally {
          engine.destroy();
        }
      };
      const realtime = (engine: RealtimeEngine) => {
        engine.play();
        let last = 0;
        for (let block = 0; block < 32; block += 1) {
          last = engine.process([new Float32Array(256)])[0].at(-1) ?? 0;
        }
        return last;
      };
      const bounced = (engine: RealtimeEngine) =>
        engine
          .bounceOffline({ totalFrames: 256 * 32, blockSize: 256, numChannels: 2 })
          .channels[0].at(-1) ?? 0;
      const gain = 10 ** (-6 / 20);
      for (const [label, body] of [
        ['realtime', realtime],
        ['bounce', bounced],
      ] as const) {
        const unity = withClip(undefined, body);
        expect(unity, label).toBeGreaterThan(0.1);
        expect(withClip(-6, body) / unity, label).toBeCloseTo(gain, 3);
      }
    });

    it('toggles track strip insert bypass', () => {
      const engine = new RealtimeEngine({ sampleRate: 48000, maxBlockSize: 256 });
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
      engine.setTrackLanes([{ trackId: 10 }]);
      engine.setStrip(
        10,
        JSON.parse(
          '{"version":2,"strips":[{"id":"track-10","inserts":[{"slot":"pre","processor":"eq.parametric","params":{"band0.type":1,"band0.frequencyHz":1000,"band0.gainDb":12,"band0.enabled":1}}]}],"buses":[],"connections":[]}',
        ).strips[0],
      );
      let badIndexError: unknown;
      try {
        engine.setStripInsertBypassed(10, 7, { bypassed: true });
      } catch (error) {
        badIndexError = error;
      }
      expect(badIndexError).toBeInstanceOf(RangeError);
      expect(isSonareError(badIndexError)).toBe(false);

      engine.play();
      let eqOut: Float32Array = new Float32Array(256);
      for (let block = 0; block < 6; block += 1) {
        [eqOut] = engine.process([new Float32Array(256)]);
      }
      engine.setStripInsertBypassed(10, 0, { bypassed: true, resetOnBypass: true });
      engine.seekSample(0);
      const [bypassedOut] = engine.process([new Float32Array(256)]);
      expect(rms(eqOut)).toBeGreaterThan(rms(bypassedOut) * 1.5);
      engine.destroy();
    });

    it('updates track strip EQ band', () => {
      const engine = new RealtimeEngine({ sampleRate: 48000, maxBlockSize: 256 });
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
      engine.setTrackLanes([{ trackId: 10 }]);
      engine.setStrip(
        10,
        JSON.parse('{"version":2,"strips":[{"id":"track-10"}],"buses":[],"connections":[]}')
          .strips[0],
      );
      let badIndexError: unknown;
      try {
        engine.setStripEqBand(10, 99, { type: 'peak', enabled: true });
      } catch (error) {
        badIndexError = error;
      }
      expect(badIndexError).toBeInstanceOf(RangeError);
      expect(isSonareError(badIndexError)).toBe(false);

      engine.play();
      const [flatOut] = engine.process([new Float32Array(256)]);
      engine.setStripEqBand(10, 0, {
        type: 'peak',
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
      const engine = new RealtimeEngine({ sampleRate: 48000, maxBlockSize: 256 });
      const frames = 256 * 64;
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
        '{"version":2,"strips":[{"id":"master","faderDb":-12,"panLaw":"linear0Db"}],"buses":[],"connections":[]}';
      engine.setStrip('master', JSON.parse(sceneJson).strips[0]);
      expect(() => engine.setStrip('master', '{bad json' as never)).toThrow(TypeError);
      let badParamError: unknown;
      try {
        engine.setStrip(
          'master',
          JSON.parse(
            '{"version":2,"strips":[{"id":"master","inserts":[{"slot":"pre","processor":"eq.parametric","params":{"band0.gainDb":"loud"}}]}],"buses":[],"connections":[]}',
          ).strips[0],
        );
      } catch (error) {
        badParamError = error;
      }
      expect(badParamError).toBeInstanceOf(TypeError);
      expect(isSonareError(badParamError)).toBe(false);

      engine.play();
      const processed = engine.process([new Float32Array(256)]);
      // Two unit clips (2) through the -12 dB master fader; a centred mono master is unity.
      expect(processed[0].at(-1)).toBeCloseTo(2 * 10 ** (-12 / 20), 3);
      engine.setParameter(0x4d58ff01, -24, undefined, { smoothed: true });
      engine.setParameter(0x4d58ff02, 0.25);
      // A first smoothed set ramps from the -12 dB the fader holds rather than stepping.
      let attenuated = engine.process([new Float32Array(256)]);
      expect(attenuated[0][0]).toBeCloseTo(2 * 10 ** (-12 / 20), 2);
      expect(attenuated[0].at(-1)).toBeGreaterThan(2 * 10 ** (-24 / 20) * 2);
      for (let block = 0; block < 40; block += 1) {
        attenuated = engine.process([new Float32Array(256)]);
      }
      // 2 * 10^(-24/20) * sqrt((0.75^2 + 1) / 2): the linear law at pan 0.25 against its centre.
      expect(attenuated[0].at(-1)).toBeCloseTo(2 * 10 ** (-24 / 20) * Math.sqrt(1.5625 / 2), 3);
      engine.destroy();
    });

    it('updates master strip EQ band', () => {
      const engine = new RealtimeEngine({ sampleRate: 48000, maxBlockSize: 256 });
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
      engine.setStrip(
        'master',
        JSON.parse('{"version":2,"strips":[{"id":"master"}],"buses":[],"connections":[]}')
          .strips[0],
      );
      let badIndexError: unknown;
      try {
        engine.setStripEqBand('master', 99, { type: 'peak', enabled: true });
      } catch (error) {
        badIndexError = error;
      }
      expect(badIndexError).toBeInstanceOf(RangeError);
      expect(isSonareError(badIndexError)).toBe(false);

      engine.play();
      const [flatOut] = engine.process([new Float32Array(256)]);
      engine.setStripEqBand('master', 0, {
        type: 'peak',
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

    it('names engine strip EQ DSP frequency refusals under band.frequencyHz', () => {
      const engine = new RealtimeEngine({ sampleRate: 48000, maxBlockSize: 256 });
      try {
        engine.setTrackBuses([{ busId: 1, gainDb: 0 }]);
        engine.setTrackLanes([{ trackId: 10, outputBusId: 1 }]);
        engine.setStrip(
          10,
          JSON.parse('{"version":2,"strips":[{"id":"track-10"}],"buses":[],"connections":[]}')
            .strips[0],
        );
        engine.setBusStrip(
          1,
          JSON.parse('{"version":2,"strips":[],"buses":[{"id":"1"}],"connections":[]}').buses[0],
        );
        engine.setStrip(
          'master',
          JSON.parse('{"version":2,"strips":[{"id":"master"}],"buses":[],"connections":[]}')
            .strips[0],
        );

        const bandAt = (frequencyHz: number) => ({
          type: 'peak' as const,
          frequencyHz,
          enabled: true,
        });
        const routes: Array<[string, (band: ReturnType<typeof bandAt>) => void]> = [
          ['RealtimeEngine.setStripEqBand', (band) => engine.setStripEqBand(10, 0, band)],
          ['RealtimeEngine.setStripEqBand', (band) => engine.setStripEqBand('master', 0, band)],
          ['RealtimeEngine.setBusEqBand', (band) => engine.setBusEqBand(1, 0, band)],
        ];
        for (const [entry, setBand] of routes) {
          expect(() => setBand(bandAt(24000))).toThrow(
            new RangeError(`${entry}: band.frequencyHz must be finite and in (0, 24000)`),
          );
          expect(() => setBand(bandAt(23999))).not.toThrow();
        }
      } finally {
        engine.destroy();
      }
    });

    it('names the offending band when applying whole-strip EQ settings', () => {
      const engine = new RealtimeEngine({ sampleRate: 48000, maxBlockSize: 256 });
      try {
        engine.setTrackBuses([{ busId: 1, gainDb: 0 }]);
        engine.setTrackLanes([{ trackId: 10, outputBusId: 1 }]);

        const eqAt = (frequencyHz: number) => ({
          bands: [{ type: 'peak' as const, frequencyHz, gainDb: 3, q: 1, enabled: true }],
        });
        const routes: Array<[string, (frequencyHz: number) => void]> = [
          ['setStrip', (hz) => engine.setStrip(10, { id: 'track-10', eq: eqAt(hz) })],
          ['setStrip', (hz) => engine.setStrip('master', { id: 'master', eq: eqAt(hz) })],
          ['setBusStrip', (hz) => engine.setBusStrip(1, { id: '1', eq: eqAt(hz) })],
        ];
        for (const [entry, setEq] of routes) {
          const subject = 'settings.eq.bands[0].frequencyHz';
          expect(() => setEq(1000)).not.toThrow();
          const error = expectRefusalOf(() => setEq(30000));
          expect(error.message).toBe(
            `RealtimeEngine.${entry}: ${subject} must be finite and in (0, 24000)`,
          );
          expect(refusalOf(error)?.subjects).toEqual([subject]);
        }
      } finally {
        engine.destroy();
      }
    });

    it('updates bus strip pan and EQ band', () => {
      // Four whole 1 kHz periods per block, so a block's RMS is phase-independent.
      const blockSize = 192;
      const engine = new RealtimeEngine({ sampleRate: 48000, maxBlockSize: blockSize });
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
      engine.play();
      const [flatLeft, flatRight] = settle();
      expect(rms(flatLeft)).toBeGreaterThan(0.05);
      expect(rms(flatRight)).toBeCloseTo(rms(flatLeft), 5);

      engine.setBusPan(1, 1);
      const [pannedLeft, pannedRight] = settle();
      expect(rms(pannedLeft)).toBeLessThan(rms(flatLeft) * 0.01);
      expect(rms(pannedRight)).toBeGreaterThan(rms(flatRight) * 0.9);

      engine.setBusPanMode(1, 'dualPan');
      engine.setBusDualPan(1, -1, -1);
      const [dualLeft, dualRight] = settle();
      expect(rms(dualRight)).toBeLessThan(rms(flatRight) * 0.01);
      expect(rms(dualLeft)).toBeGreaterThan(rms(flatLeft) * 0.9);
      engine.setBusPanLaw(1, 'const6Db');
      engine.setBusPanMode(1, 'balance');
      engine.setBusPan(1, 0);
      const [centredLeft] = settle();
      expect(rms(centredLeft)).toBeCloseTo(rms(flatLeft), 3);

      engine.setBusEqBand(1, 0, {
        type: 'peak',
        frequencyHz: 1000,
        gainDb: 12,
        q: 1,
        enabled: true,
      });
      const [boostedLeft] = settle();
      expect(rms(boostedLeft)).toBeGreaterThan(rms(flatLeft) * 1.5);
      engine.setBusEqBand(1, 0, { type: 'peak', frequencyHz: 1000, gainDb: 0 });
      const [restoredLeft] = settle();
      expect(rms(restoredLeft)).toBeCloseTo(rms(flatLeft), 3);

      expectRefusalOf(() => engine.setBusPan(99, 0.5));
      expectRefusalOf(() => engine.setBusPanLaw(99, 'const3Db'));
      expectRefusalOf(() => engine.setBusPanMode(99, 'balance'));
      expectRefusalOf(() => engine.setBusDualPan(99, -1, 1));
      expectRefusalOf(() => engine.setBusEqBand(99, 0, { type: 'peak' }));
      expectRefusalOf(() => engine.setBusEqBand(1, 99, { type: 'peak' }));
      expectRefusalOf(() => engine.setBusEqBand(1, -1, { type: 'peak' }));
      // A surround bus has no stereo image to pan; its EQ is still addressable.
      engine.setTrackBuses([
        { busId: 1, gainDb: 0 },
        { busId: 2, gainDb: 0, channelLayout: '5.1' },
      ]);
      expectRefusalOf(() => engine.setBusPan(2, 0.5));
      expectRefusalOf(() => engine.setBusDualPan(2, -1, 0));
      engine.setBusEqBand(2, 0, { type: 'peak', frequencyHz: 500, gainDb: 3 });
      engine.destroy();
    });

    it('process takes withMonitor as an option', () => {
      const engine = new RealtimeEngine({ sampleRate: 48000, maxBlockSize: 16 });
      const result = engine.process(
        [new Float32Array(16).fill(0.25), new Float32Array(16).fill(-0.25)],
        { withMonitor: true },
      );
      expect(result.output).toHaveLength(2);
      expect(result.monitor).toHaveLength(2);
      expect(result.output[0][0]).toBeCloseTo(0.25);
      expect(result.output[1][0]).toBeCloseTo(-0.25);
      expect(result.monitor[0][0]).toBeCloseTo(0);
      expect(result.monitor[1][0]).toBeCloseTo(0);
      const plain = engine.process([new Float32Array(16), new Float32Array(16)], {
        withMonitor: false,
      });
      expect(Array.isArray(plain)).toBe(true);
      expect(plain).not.toHaveProperty('monitor');
      const planes = [new Float32Array(16), new Float32Array(16)];
      expect(() => engine.process(planes, { withMonitor: 1 as never })).toThrow(TypeError);
      expect(() => engine.process(planes, { nope: true } as never)).toThrow(TypeError);
      expect(() => engine.processPrepared(16, { nope: true } as never)).toThrow(TypeError);
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
      `{"version":2,"strips":[{"id":"${id}","inserts":[${insert}]}],"buses":[]}`;
    const limiter = JSON.stringify({
      slot: 'pre',
      processor: 'dynamics.limiter',
      params: { thresholdDb: 0, lookaheadMs: 0, releaseMs: 50 },
    });

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
      const engine = new RealtimeEngine({ sampleRate: SR, maxBlockSize: BLOCK });
      engine.setClips([{ trackId: TRACK, channels: burst(), startPpq: 0, lengthSamples: 96000 }]);
      engine.setTrackLanes([{ trackId: TRACK }]);
      engine.setStrip(TRACK, JSON.parse(stripJson('s', PLATE)).strips[0]);
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
      const out = fresh.renderOffline({
        channels: [new Float32Array(RENDER), new Float32Array(RENDER)],
        blockSize: BLOCK,
      });
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
      const first = engine.renderOffline({
        channels: [new Float32Array(RENDER), new Float32Array(RENDER)],
        blockSize: BLOCK,
      });
      engine.seekSample(START);
      engine.primeOfflineParameters(2, BLOCK);
      const second = engine.renderOffline({
        channels: [new Float32Array(RENDER), new Float32Array(RENDER)],
        blockSize: BLOCK,
      });
      engine.destroy();
      const { diff, tol } = maxDiff(second, first);
      expect(diff).toBeLessThanOrEqual(tol);
    });

    it('primeOfflineParameters refuses bad counts and resetProcessorState a full queue', () => {
      const engine = new RealtimeEngine({ sampleRate: SR, maxBlockSize: BLOCK });
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
      const engine = new RealtimeEngine({ sampleRate: SR, maxBlockSize: BLOCK });
      engine.setTrackBuses([
        { busId: 1, gainDb: 0 },
        { busId: 2, gainDb: 0 },
      ]);
      engine.setTrackLanes([{ trackId: 10 }, { trackId: 11 }]);
      const busStrip = (id: string): string =>
        `{"version":2,"strips":[],"buses":[{"id":"${id}","inserts":[${limiter}]}]}`;
      engine.setBusStrip(1, JSON.parse(busStrip('1')).buses[0]);
      engine.setBusStrip(2, JSON.parse(busStrip('2')).buses[0]);
      engine.setStrip('master', JSON.parse(stripJson('master', limiter)).strips[0]);
      engine.setStrip(10, JSON.parse(stripJson('t10', limiter)).strips[0]);
      engine.setStrip(11, JSON.parse(stripJson('t11', limiter)).strips[0]);

      const ok = { ok: true };
      const no = (reason: string) => ({ ok: false, reason });
      expect(engine.canSetStripSidechain(0, 0, { kind: 'track', id: 11 })).toEqual(
        no('invalidTarget'),
      );
      expect(engine.canSetStripSidechain(10, 0, { kind: 'track', id: 10 })).toEqual(no('selfKey'));
      expect(engine.canSetStripSidechain(10, 0, { kind: 'track', id: 11 })).toEqual(ok);
      // Removal is spelled null; a source id of 0 is refused.
      expect(() => engine.canSetStripSidechain(10, 0, { kind: 'track', id: 0 })).toThrow(
        RangeError,
      );
      expect(engine.canSetStripSidechain(10, 0, { kind: 'track', id: 99 })).toEqual(
        no('undeclaredSource'),
      );
      expect(engine.canSetStripSidechain(10, 1, { kind: 'track', id: 11 })).toEqual(
        no('insertOutOfRange'),
      );
      expectRefusalOf(() => engine.setStripSidechain(10, 0, { kind: 'track', id: 99 }));
      expectRefusalOf(() => engine.setStripSidechain(10, 1, { kind: 'track', id: 11 }));
      engine.setStripSidechain(10, 0, { kind: 'track', id: 11 });
      expect(engine.canSetStripSidechain(11, 0, { kind: 'track', id: 10 })).toEqual(no('cycle'));
      expectRefusalOf(() => engine.setStripSidechain(11, 0, { kind: 'track', id: 10 }));

      expect(engine.canSetBusSidechain(99, 0, { kind: 'bus', id: 1 })).toEqual(no('invalidTarget'));
      expect(engine.canSetBusSidechain(1, 5, { kind: 'bus', id: 2 })).toEqual(
        no('insertOutOfRange'),
      );
      expect(engine.canSetBusSidechain(1, 0, { kind: 'bus', id: 77 })).toEqual(
        no('undeclaredSource'),
      );
      // The TypeScript layer refuses an unknown kind by name before the engine is asked.
      expect(() => engine.canSetBusSidechain(1, 0, { kind: 7 as never, id: 2 })).toThrow(TypeError);
      expect(engine.canSetBusSidechain(1, 0, { kind: 'bus', id: 1 })).toEqual(no('selfKey'));
      expect(engine.canSetBusSidechain(1, 0, { kind: 'bus', id: 2 })).toEqual(ok);
      engine.setBusSidechain(1, 0, { kind: 'bus', id: 2 });
      expect(engine.canSetBusSidechain(2, 0, { kind: 'bus', id: 1 })).toEqual(no('cycle'));
      expectRefusalOf(() => engine.setBusSidechain(2, 0, { kind: 'bus', id: 1 }));

      expect(engine.canSetStripSidechain('master', 5, { kind: 'bus', id: 1 })).toEqual(
        no('insertOutOfRange'),
      );
      expect(engine.canSetStripSidechain('master', 0, { kind: 'bus', id: 77 })).toEqual(
        no('undeclaredSource'),
      );
      expect(() => engine.canSetStripSidechain('master', 0, { kind: 7 as never, id: 1 })).toThrow(
        TypeError,
      );
      expect(engine.canSetStripSidechain('master', 0, { kind: 'track', id: 10 })).toEqual(ok);

      expect(() =>
        engine.canSetStripSidechain('10' as unknown as number, 0, { kind: 'track', id: 11 }),
      ).toThrow(/ref must be a track id/);
      expect(() =>
        engine.canSetBusSidechain(1, 0, { kind: 'bus', id: '2' as unknown as number }),
      ).toThrow();
      engine.destroy();
    });

    it('refuses a reserved mixer id that names no strip and keeps the live ids', () => {
      const engine = new RealtimeEngine({ sampleRate: SR, maxBlockSize: BLOCK });
      engine.setTrackLanes([{ trackId: 5 }, { trackId: 7 }]);
      const point = [{ ppq: 0, value: -6, curveToNext: 'linear' as const }];
      for (const positional of [0x4d580101, 0x4d580001, 0x4d58fe00]) {
        expectRefusalOf(() => engine.setParameter(positional, -6, undefined, { smoothed: true }));
        expectRefusalOf(() => engine.setParameter(positional, -6));
        expectRefusalOf(() => engine.setAutomationLane(positional, point));
      }
      const laneFader = engine.resolveStripAutomationId(7, 'faderDb');
      expect(() => engine.setParameter(laneFader, -6, undefined, { smoothed: true })).not.toThrow();
      expect(() => engine.setAutomationLane(laneFader, point)).not.toThrow();
      expect(() =>
        engine.setParameter(0x4d58ff01, -6, undefined, { smoothed: true }),
      ).not.toThrow();
      expect(() => engine.setAutomationLane(0x4d58ff01, point)).not.toThrow();
      engine.destroy();
    });

    it('refuses a negative render frame on the raw embind class', () => {
      const engine = new RealtimeEngine({ sampleRate: SR, maxBlockSize: BLOCK });
      const native = (
        engine as unknown as {
          native: {
            setParameter(id: number, value: number, frame?: number, smoothed?: boolean): void;
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
        () => native.setParameter(0x4d58ff01, -6, -1, true),
        () => native.pushMidiNoteOn(1, 0, 0, 60, 100, -1),
        () => native.resetProcessorState(-1),
      ];
      for (const call of calls) {
        expect(call).toThrow(RangeError);
        expect(call).toThrow(
          /^RealtimeEngine\.\w+: renderFrame must be an integer of at least 0 \(omit it to apply immediately\)$/,
        );
      }
      // Omitted means immediate, as does a frame of 0.
      expect(() => native.setParameter(0x4d58ff01, -6)).not.toThrow();
      expect(() => native.setParameter(0x4d58ff01, -6, 0)).not.toThrow();
      engine.destroy();
    });

    it('maps every refusal code to its name and rejects an unknown one', () => {
      expect(sidechainCheckFromCode(0)).toEqual({ ok: true });
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
      let caught: unknown;
      try {
        sidechainCheckFromCode(9);
      } catch (error) {
        caught = error;
      }
      expect(isSonareError(caught) && caught.code).toBe(ErrorCode.InvalidFormat);
      expect((caught as Error).message).toBe('unknown sidechain refusal ordinal: 9');
    });

    it('reports a tail from a delay insert and a latency in 1/256 samples', () => {
      const bare = new RealtimeEngine({ sampleRate: SR, maxBlockSize: BLOCK });
      expect(bare.tailSamples).toBe(0);
      expect(bare.graphLatencySamples).toBe(0);
      bare.destroy();

      const engine = new RealtimeEngine({ sampleRate: SR, maxBlockSize: BLOCK });
      engine.setTrackLanes([{ trackId: TRACK }]);
      engine.setStrip(
        TRACK,
        JSON.parse(
          stripJson(
            's',
            '{"slot":"pre","processor":"effects.delay.stereo","params":{"feedback":0.6,"delayTimeLMs":37,"delayTimeRMs":53,"dryWet":0.5}}',
          ),
        ).strips[0],
      );
      // The longer channel is 53 ms, so any bound is at least that long.
      expect(engine.tailSamples).toBeGreaterThanOrEqual(Math.round(0.053 * SR));
      engine.destroy();
    });
  });

  describe('strip JSON error codes', () => {
    const scene = (kind: 'strips' | 'buses', id: string, slot: string): string =>
      JSON.stringify({
        version: 2,
        strips: kind === 'strips' ? [{ id, inserts: [{ slot, processor: 'utility.gain' }] }] : [],
        buses: kind === 'buses' ? [{ id, inserts: [{ slot, processor: 'utility.gain' }] }] : [],
        connections: [],
      });
    const codeOf = (fn: () => void): number | undefined => {
      try {
        fn();
      } catch (error) {
        return isSonareError(error) ? error.code : -1;
      }
      return undefined;
    };

    it('refuses a bad slot in a well-formed document and a non-object strip', () => {
      const engine = new RealtimeEngine({ sampleRate: 48000, maxBlockSize: 256 });
      try {
        engine.setTrackBuses([{ busId: 1, gainDb: 0 }]);
        engine.setTrackLanes([{ trackId: 10 }]);
        // A bad slot name inside the settings object is a refused argument, not a document error.
        expect(() =>
          engine.setBusStrip(1, JSON.parse(scene('buses', '1', 'sideways')).buses[0]),
        ).toThrow(RangeError);
        expect(() =>
          engine.setStrip(10, JSON.parse(scene('strips', 't', 'sideways')).strips[0]),
        ).toThrow(/^RealtimeEngine\.setStrip: settings\.inserts\[0\]\.slot /);
        expect(
          codeOf(() =>
            engine.setStrip('master', JSON.parse(scene('strips', 'm', 'sideways')).strips[0]),
          ),
        ).toBe(-1);
        // A non-object strip is an argument of the wrong type.
        expect(() => engine.setStrip('master', 7 as never)).toThrow(TypeError);
      } finally {
        engine.destroy();
      }
    });
  });
});
