import { describe, expect, it } from 'vitest';
import type {
  EngineAutomationPoint,
  MasteringProcessorCatalogEntry,
  MixSceneBus,
  MixSceneStrip,
} from '../src/index.js';
import {
  ErrorCode,
  isSonareError,
  MAX_TRACK_LANES,
  masteringInsertParamInfo,
  masteringProcessorCatalog,
  RealtimeEngine,
} from '../src/index.js';
import { rms } from './_engine_signals.js';
import { expectRefusal } from './_helpers.js';

/** First strip / bus of a scene document: the object the engine strip setters take. */
const sceneStrip = (json: string): MixSceneStrip => JSON.parse(json).strips[0];
// The facade takes documents as objects, so malformed JSON can only reach the
// addon's parser through the native handle.
const nativeOf = (engine: RealtimeEngine) =>
  (
    engine as unknown as {
      native: {
        setBusStripJson(busId: number, json: string): void;
        setTrackStripJson(trackId: number, json: string): void;
        setMasterStripJson(json: string): void;
      };
    }
  ).native;
const sceneBus = (json: string): MixSceneBus => JSON.parse(json).buses[0];

describe('RealtimeEngine native binding', () => {
  /** Asserts that `call` is refused as an argument error (RangeError, not SonareError). */
  const expectArgumentRefusal = (call: () => void): void => {
    let error: unknown;
    try {
      call();
    } catch (thrown) {
      error = thrown;
    }
    expect(error).toBeInstanceOf(RangeError);
    expect(isSonareError(error)).toBe(false);
  };

  it('routes track clips through lanes and lane commands', () => {
    const engine = new RealtimeEngine({ sampleRate: 48000, maxBlockSize: 256 });
    const frames = 256 * 10;
    engine.setClips([
      {
        id: 1,
        trackId: 10,
        channels: [new Float32Array(frames).fill(1), new Float32Array(frames).fill(1)],
        startPpq: 0,
        lengthSamples: frames,
      },
      {
        id: 2,
        trackId: 20,
        channels: [new Float32Array(frames).fill(1), new Float32Array(frames).fill(1)],
        startPpq: 0,
        lengthSamples: frames,
      },
    ]);
    engine.setTrackLanes([{ trackId: 10 }, { trackId: 20 }]);
    let duplicateLaneError: unknown;
    try {
      engine.setTrackLanes([{ trackId: 10 }, { trackId: 10 }]);
    } catch (error) {
      duplicateLaneError = error;
    }
    expect(duplicateLaneError).toBeInstanceOf(RangeError);
    expect(isSonareError(duplicateLaneError)).toBe(false);
    engine.setTrackLanes([{ trackId: 10 }, { trackId: 20 }]);

    engine.play();
    let processed = engine.process([new Float32Array(256), new Float32Array(256)]);
    expect(processed[0].at(-1)).toBeCloseTo(2, 4);
    expect(processed[1].at(-1)).toBeCloseTo(2, 4);

    engine.setTrackSoloMute(10, { solo: true, mute: false });
    for (let block = 0; block < 4; block += 1) {
      processed = engine.process([new Float32Array(256), new Float32Array(256)]);
    }
    expect(processed[0].at(-1)).toBeGreaterThan(0.75);
    expect(processed[0].at(-1)).toBeLessThan(1.25);

    engine.setParameter(engine.resolveStripAutomationId(10, 'faderDb'), -12, undefined, {
      smoothed: true,
    });
    for (let block = 0; block < 6; block += 1) {
      processed = engine.process([new Float32Array(256), new Float32Array(256)]);
    }
    expect(processed[0].at(-1)).toBeLessThan(0.45);
    expect(processed[1].at(-1)).toBeLessThan(0.45);
    engine.destroy();
  });

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

    // A surround bus/source layout flows through to the native struct fields
    // (the native side validates the enum range).
    expect(() =>
      engine.setTrackBuses([{ busId: 1, gainDb: 0, channelLayout: '5.1' }]),
    ).not.toThrow();
    // Only a stereo (or omitted) lane layout is accepted; the others are refused.
    expect(() =>
      engine.setTrackLanes([{ trackId: 10, sourceChannelLayout: 'stereo' }]),
    ).not.toThrow();
    expect(() => engine.setTrackLanes([{ trackId: 10 }])).not.toThrow();
    for (const layout of ['mono', '5.1', '7.1'] as const) {
      expect(() => engine.setTrackLanes([{ trackId: 10, sourceChannelLayout: layout }])).toThrow(
        expect.objectContaining({
          code: ErrorCode.NotSupported,
          message:
            'RealtimeEngine.setTrackLanes: lanes[0].sourceChannelLayout other than stereo is not supported',
        }),
      );
    }
    // The lane cap is exported and its refusal names it.
    const lanes = Array.from({ length: MAX_TRACK_LANES + 1 }, (_, i) => ({ trackId: i + 1 }));
    expect(MAX_TRACK_LANES).toBe(32);
    expect(() => engine.setTrackLanes(lanes.slice(0, MAX_TRACK_LANES))).not.toThrow();
    expect(() => engine.setTrackLanes(lanes)).toThrow(
      'RealtimeEngine.setTrackLanes: lanes.length must be an integer in [0, 32]',
    );
    expect(() => engine.setTrackLanes([{ trackId: 10 }])).not.toThrow();
    // A layout is a name: an ordinal never reaches the narrowing in the addon.
    // @ts-expect-error deliberately a number where a layout name belongs.
    expect(() => engine.setTrackLanes([{ trackId: 10, sourceChannelLayout: 257 }])).toThrow(
      TypeError,
    );
    // @ts-expect-error deliberately a number where a layout name belongs.
    expect(() => engine.setTrackBuses([{ busId: 1, gainDb: 0, channelLayout: 257 }])).toThrow(
      TypeError,
    );
    expect(() =>
      engine.setTrackBuses([{ busId: 1, gainDb: 0, channelLayout: 'quad' as never }]),
    ).toThrow(RangeError);

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
    // A 5.1 bus feeding a narrower (mono) output downmixes rather than
    // truncating to the front channels, so the unity send adds half the direct level.
    // Direct 1 + half of the unity send (1.5).
    expect(out.at(-1)).toBeGreaterThan(1.49);
    expect(out.at(-1)).toBeLessThan(1.51);
    const meterTargets = new Set(engine.drainMeterTelemetry().map((record) => record.targetId));
    expect(meterTargets.has(1)).toBe(true);
    expect(meterTargets.has(33)).toBe(true);
    expect(meterTargets.has(0)).toBe(true);

    engine.setTrackLanes([{ trackId: 10, sends: [{ busId: 1, levelDb: -6.0206 }] }]);
    engine.seekSample(0);
    [out] = engine.process([new Float32Array(256)]);
    // 1 + 0.5 * 10^(-6.0206/20) = 1.25.
    expect(out.at(-1)).toBeGreaterThan(1.24);
    expect(out.at(-1)).toBeLessThan(1.26);

    engine.setTrackLanes([{ trackId: 10, sends: [{ busId: 1, levelDb: 0, enabled: false }] }]);
    engine.seekSample(0);
    [out] = engine.process([new Float32Array(256)]);
    expect(out.at(-1)).toBeGreaterThan(0.99);
    expect(out.at(-1)).toBeLessThan(1.01);

    // The settings text is a per-call argument, so malformed text is a refused argument.
    expect(() => nativeOf(engine).setBusStripJson(1, '{bad json')).toThrow(RangeError);
    expect(() => nativeOf(engine).setBusStripJson(1, '{bad json')).toThrow(
      /^RealtimeEngine\.setBusStripJson: settings must be valid JSON: /,
    );
    engine.setBusStrip(
      1,
      sceneBus('{"version":2,"strips":[],"buses":[{"id":"1","inserts":[]}],"connections":[]}'),
    );
    engine.destroy();
  });

  it('drains per-plane meter telemetry for a surround group bus', () => {
    const engine = new RealtimeEngine({ sampleRate: 48000, maxBlockSize: 256 });
    const frames = 256;
    engine.setClips([
      {
        id: 1,
        trackId: 10,
        channels: [new Float32Array(frames).fill(0.5)],
        startPpq: 0,
        lengthSamples: frames,
      },
    ]);
    // 5.1 group bus; the lane routes into it and is panned hard to Ls.
    engine.setTrackBuses([{ busId: 1, gainDb: 0, channelLayout: '5.1' }]);
    engine.setTrackLanes([{ trackId: 10, outputBusId: 1 }]);
    engine.setStrip(
      10,
      sceneStrip(
        '{"version":2,"buses":[{"id":"master","role":"master"}],"strips":[{"id":"s","surroundPan":{"azimuth":-110}}]}',
      ),
    );
    engine.play();
    engine.process([
      new Float32Array(frames),
      new Float32Array(frames),
      new Float32Array(frames),
      new Float32Array(frames),
      new Float32Array(frames),
      new Float32Array(frames),
    ]);
    const wide = engine.drainMeterTelemetry();
    const busMeter = wide.find((record) => record.targetId === 33);
    expect(busMeter).toBeDefined();
    if (!busMeter) {
      throw new Error('expected a 5.1 bus meter');
    }
    expect(busMeter.numChannels).toBe(6);
    expect(busMeter.peakDb).toHaveLength(6);
    // Ls (plane 4) carries the panned lane, well above the silent front-left.
    const rearLeftPeak = busMeter.peakDb[4];
    const frontLeftPeak = busMeter.peakDb[0];
    if (rearLeftPeak === undefined || frontLeftPeak === undefined) {
      throw new Error('expected measured levels for the silent and panned planes');
    }
    expect(rearLeftPeak).toBeGreaterThan(frontLeftPeak + 10);
    engine.destroy();
  });

  it('refuses an out-of-range surround pan on a track strip', () => {
    const engine = new RealtimeEngine({ sampleRate: 48000, maxBlockSize: 256 });
    try {
      engine.setTrackLanes([{ trackId: 10 }]);
      engine.setStrip(
        10,
        sceneStrip('{"version":2,"strips":[{"id":"s"}],"buses":[],"connections":[]}'),
      );
      expectArgumentRefusal(() => engine.setStripSurroundPan(10, { azimuth: 200 }));
      expectArgumentRefusal(() => engine.setStripSurroundPan(10, { lfe: 2 }));
    } finally {
      engine.destroy();
    }
  });

  it('moves a lane across surround planes live with setTrackStripSurroundPan', () => {
    const frames = 256;
    const blocks = 16;
    const engine = new RealtimeEngine({ sampleRate: 48000, maxBlockSize: frames });
    engine.setClips([
      {
        id: 1,
        trackId: 10,
        channels: [new Float32Array(frames * blocks).fill(0.5)],
        startPpq: 0,
        lengthSamples: frames * blocks,
      },
    ]);
    engine.setTrackBuses([{ busId: 1, gainDb: 0, channelLayout: '5.1' }]);
    engine.setTrackLanes([{ trackId: 10, outputBusId: 1 }]);
    engine.setStrip(
      10,
      sceneStrip('{"version":2,"buses":[{"id":"master","role":"master"}],"strips":[{"id":"s"}]}'),
    );
    const run = (): Float32Array[] =>
      engine.process(Array.from({ length: 6 }, () => new Float32Array(frames)));
    const tailPower = (out: Float32Array[]): number[] =>
      out.map((plane) => {
        let sum = 0;
        for (let i = frames / 2; i < frames; i++) {
          sum += plane[i] * plane[i];
        }
        return sum / (frames / 2);
      });
    const Ls = 4;
    const Rs = 5;
    const C = 2;

    engine.play();
    engine.setStripSurroundPan(10, { azimuth: -110 });
    let left = tailPower(run());
    // 5 ms glide: after 5 blocks (~27 ms) the move has settled.
    for (let b = 0; b < 5; b++) {
      left = tailPower(run());
    }
    const leftTotal = left.reduce((a, b) => a + b, 0);
    expect(leftTotal).toBeGreaterThan(0.01);
    expect(left[Ls]).toBeGreaterThan(0.3 * leftTotal);
    expect(left[Rs]).toBeLessThan(0.01 * leftTotal);
    expect(left[C]).toBeLessThan(0.05 * leftTotal);

    // Steady total power of the settled left placement, taken from the last frame.
    const framePower = (out: Float32Array[], i: number): number =>
      out.reduce((sum, plane) => sum + plane[i] * plane[i], 0);
    const settled = run();
    const steadyPower = framePower(settled, frames - 1);
    expect(steadyPower).toBeGreaterThan(0.01);

    engine.setStripSurroundPan(10, { azimuth: 110 });
    let minRatio = Number.POSITIVE_INFINITY;
    let maxRatio = 0;
    let crossfadeFrames = 0;
    let right: number[] = [];
    for (let b = 0; b < 6; b++) {
      const out = run();
      right = tailPower(out);
      for (let i = 0; i < frames; i++) {
        const ratio = framePower(out, i) / steadyPower;
        minRatio = Math.min(minRatio, ratio);
        maxRatio = Math.max(maxRatio, ratio);
        if (Math.abs(out[Ls][i]) > 0.25 && Math.abs(out[Rs][i]) > 0.25) {
          crossfadeFrames++;
        }
      }
    }
    // Non-vacuity: the window contains frames where both planes carry signal.
    expect(crossfadeFrames).toBeGreaterThan(0);
    // The gain vector is renormalized every sample: per-frame total power holds
    // the steady value through the move (float32 scale).
    expect(minRatio).toBeGreaterThanOrEqual(1 - 1e-5);
    expect(maxRatio).toBeLessThanOrEqual(1 + 1e-5);
    const rightTotal = right.reduce((a, b) => a + b, 0);
    expect(right[Rs]).toBeGreaterThan(0.3 * rightTotal);
    expect(right[Ls]).toBeLessThan(0.01 * rightTotal);
    expect(right[C]).toBeLessThan(0.05 * rightTotal);
    // A power-preserving pan keeps the summed plane power through the move.
    expect(Math.abs(rightTotal - leftTotal)).toBeLessThan(0.05 * leftTotal);

    expectRefusal(
      () => engine.setStripSurroundPan(10, { azimuth: Number.NaN }),
      RangeError,
      /^RealtimeEngine\.setStripSurroundPan: azimuth must be finite and in \[-180, 180\]$/,
    );
    expectRefusal(
      () => engine.setStripSurroundPan(999, { azimuth: 0 }),
      RangeError,
      /^RealtimeEngine\.setStripSurroundPan: ref must be the id of a track in this engine$/,
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
    engine.setStrip(10, sceneStrip(sceneJson));
    // The settings text is a per-call argument, so malformed text is a refused argument.
    expect(() => nativeOf(engine).setTrackStripJson(10, '{bad json')).toThrow(RangeError);
    expect(() => nativeOf(engine).setTrackStripJson(10, '{bad json')).toThrow(
      /^RealtimeEngine\.setTrackStripJson: settings must be valid JSON: /,
    );
    let badProcessorError: unknown;
    try {
      engine.setStrip(
        10,
        sceneStrip(
          '{"version":2,"strips":[{"id":"track-10","inserts":[{"slot":"pre","processor":"missing.processor","params":{}}]}],"buses":[],"connections":[]}',
        ),
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
        sceneStrip(
          '{"version":2,"strips":[{"id":"track-10","inserts":[{"slot":"pre","processor":"eq.parametric","params":{"band0.gainDb":"loud"}}]}],"buses":[],"connections":[]}',
        ),
      );
    } catch (error) {
      badParamError = error;
    }
    expect(badParamError).toBeInstanceOf(RangeError);
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

  it('applies realtime track strip pan setters', () => {
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
    ]);
    engine.setTrackLanes([{ trackId: 10 }]);
    engine.setStrip(
      10,
      sceneStrip(
        '{"version":2,"strips":[{"id":"track-10","panLaw":"linear0Db"}],"buses":[],"connections":[]}',
      ),
    );

    expect(() => engine.setStripPanLaw(10, 'linear0Db')).not.toThrow();
    expect(() => engine.setStripPanMode(10, 'balance')).not.toThrow();
    expect(() => engine.setStripChannelDelaySamples(10, 0)).not.toThrow();
    expect(() => engine.setStripDualPan(10, -1, -1)).not.toThrow();

    // Hard-left pan: the left output channel must dominate the right.
    engine.setStripPanMode(10, 'balance');
    engine.setStripPan(10, -1);
    engine.settleParameters();
    engine.play();
    let panned: Float32Array[] = [new Float32Array(256), new Float32Array(256)];
    for (let block = 0; block < 4; block += 1) {
      panned = engine.process([new Float32Array(256), new Float32Array(256)]);
    }
    const leftLevel = Math.abs(panned[0].at(-1) ?? 0);
    const rightLevel = Math.abs(panned[1].at(-1) ?? 0);
    expect(leftLevel).toBeGreaterThan(rightLevel);
    engine.destroy();
  });

  it('routes a stereo clip through realtime dual-pan directions', () => {
    const renderDualPan = (leftPan: number, rightPan: number): [number, number] => {
      const engine = new RealtimeEngine({ sampleRate: 48000, maxBlockSize: 256 });
      const frames = 256 * 4;
      try {
        engine.setClips([
          {
            id: 1,
            trackId: 10,
            channels: [new Float32Array(frames).fill(1), new Float32Array(frames)],
            startPpq: 0,
            lengthSamples: frames,
          },
        ]);
        engine.setTrackLanes([{ trackId: 10 }]);
        engine.setStrip(
          10,
          sceneStrip(
            '{"version":2,"strips":[{"id":"track-10","panLaw":"linear0Db"}],"buses":[],"connections":[]}',
          ),
        );
        engine.setStripPanMode(10, 'dualPan');
        engine.setStripDualPan(10, leftPan, rightPan);
        engine.settleParameters();
        engine.play();

        let output: Float32Array[] = [];
        for (let block = 0; block < 4; block += 1) {
          output = engine.process([new Float32Array(256), new Float32Array(256)]);
        }
        return [rms(output[0]), rms(output[1])];
      } finally {
        engine.destroy();
      }
    };

    const leftInputPannedRight = renderDualPan(1, -1);
    expect(leftInputPannedRight[1]).toBeGreaterThan(0.9);
    expect(leftInputPannedRight[0]).toBeLessThan(1e-5);

    const leftInputPannedLeft = renderDualPan(-1, 1);
    expect(leftInputPannedLeft[0]).toBeGreaterThan(0.9);
    expect(leftInputPannedLeft[1]).toBeLessThan(1e-5);
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
      sceneStrip(
        '{"version":2,"strips":[{"id":"track-10","inserts":[{"slot":"pre","processor":"eq.parametric","params":{"band0.type":1,"band0.frequencyHz":1000,"band0.gainDb":12,"band0.enabled":1}}]}],"buses":[],"connections":[]}',
      ),
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
    let eqOut: Float32Array<ArrayBufferLike> = new Float32Array(256);
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
      sceneStrip('{"version":2,"strips":[{"id":"track-10"}],"buses":[],"connections":[]}'),
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
    let eqOut: Float32Array<ArrayBufferLike> = new Float32Array(256);
    for (let block = 0; block < 6; block += 1) {
      [eqOut] = engine.process([new Float32Array(256)]);
    }
    expect(rms(eqOut)).toBeGreaterThan(rms(flatOut) * 1.5);
    engine.destroy();
  });

  it('applies realtime bus strip pan setters', () => {
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
    ]);
    engine.setTrackBuses([{ busId: 1, gainDb: 0 }]);
    engine.setTrackLanes([{ trackId: 10, outputBusId: 1 }]);

    expect(() => engine.setBusPanLaw(1, 'linear0Db')).not.toThrow();
    expect(() => engine.setBusPanMode(1, 'balance')).not.toThrow();
    expect(() => engine.setBusDualPan(1, -1, -1)).not.toThrow();

    // Hard-left pan on the bus: the left output channel must dominate the right.
    engine.setBusPanMode(1, 'balance');
    engine.setBusPan(1, -1);
    engine.settleParameters();
    engine.play();
    let panned: Float32Array[] = [new Float32Array(256), new Float32Array(256)];
    for (let block = 0; block < 4; block += 1) {
      panned = engine.process([new Float32Array(256), new Float32Array(256)]);
    }
    const leftLevel = Math.abs(panned[0].at(-1) ?? 0);
    const rightLevel = Math.abs(panned[1].at(-1) ?? 0);
    expect(leftLevel).toBeGreaterThan(rightLevel);
    engine.destroy();
  });

  it('updates bus strip EQ band', () => {
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
    engine.setTrackBuses([{ busId: 1, gainDb: 0 }]);
    engine.setTrackLanes([{ trackId: 10, outputBusId: 1 }]);
    expectArgumentRefusal(() => engine.setBusEqBand(1, 99, { type: 'peak', enabled: true }));

    engine.play();
    const [flatOut] = engine.process([new Float32Array(256)]);
    engine.setBusEqBand(1, 0, {
      type: 'peak',
      frequencyHz: 1000,
      gainDb: 12,
      q: 1,
      enabled: true,
    });
    engine.seekSample(0);
    let eqOut: Float32Array<ArrayBufferLike> = new Float32Array(256);
    for (let block = 0; block < 6; block += 1) {
      [eqOut] = engine.process([new Float32Array(256)]);
    }
    expect(rms(eqOut)).toBeGreaterThan(rms(flatOut) * 1.5);
    engine.destroy();
  });

  it('rejects bus strip pan setters for bus id 0, an unknown bus, and non-finite values', () => {
    const engine = new RealtimeEngine({ sampleRate: 48000, maxBlockSize: 256 });
    engine.setTrackBuses([{ busId: 1, gainDb: 0 }]);

    expectArgumentRefusal(() => engine.setBusPan(0, -1));
    expectArgumentRefusal(() => engine.setBusPan(99, -1));
    expectArgumentRefusal(() => engine.setBusPan(1, Number.NaN));
    expectArgumentRefusal(() => engine.setBusPanLaw(99, 'const6Db'));
    expectArgumentRefusal(() => engine.setBusPanMode(99, 'stereoPan'));
    expectArgumentRefusal(() => engine.setBusDualPan(99, -1, 1));
    expectArgumentRefusal(() => engine.setBusDualPan(1, Number.POSITIVE_INFINITY, 1));

    engine.destroy();
  });

  it('refuses bus strip pan setters on a surround bus, but still allows EQ', () => {
    const engine = new RealtimeEngine({ sampleRate: 48000, maxBlockSize: 256 });
    engine.setTrackBuses([{ busId: 1, gainDb: 0, channelLayout: '5.1' }]); // 5.1

    expectArgumentRefusal(() => engine.setBusPan(1, -1));
    expectArgumentRefusal(() => engine.setBusPanLaw(1, 'const6Db'));
    expectArgumentRefusal(() => engine.setBusPanMode(1, 'stereoPan'));
    expectArgumentRefusal(() => engine.setBusDualPan(1, -1, 1));

    expect(() =>
      engine.setBusEqBand(1, 0, {
        type: 'peak',
        frequencyHz: 1000,
        gainDb: 6,
        q: 1,
        enabled: true,
      }),
    ).not.toThrow();

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
    engine.setStrip('master', sceneStrip(sceneJson));
    // The settings text is a per-call argument, so malformed text is a refused argument.
    expect(() => nativeOf(engine).setMasterStripJson('{bad json')).toThrow(RangeError);
    expect(() => nativeOf(engine).setMasterStripJson('{bad json')).toThrow(
      /^RealtimeEngine\.setMasterStripJson: settings must be valid JSON: /,
    );
    let badParamError: unknown;
    try {
      engine.setStrip(
        'master',
        sceneStrip(
          '{"version":2,"strips":[{"id":"master","inserts":[{"slot":"pre","processor":"eq.parametric","params":{"band0.gainDb":"loud"}}]}],"buses":[],"connections":[]}',
        ),
      );
    } catch (error) {
      badParamError = error;
    }
    expect(badParamError).toBeInstanceOf(RangeError);
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

  it('refuses a reserved mixer id that names no strip and keeps the master and per-strip ids', () => {
    const engine = new RealtimeEngine({ sampleRate: 48000, maxBlockSize: 256 });
    engine.setTrackLanes([{ trackId: 5 }, { trackId: 7 }]);
    const point: EngineAutomationPoint[] = [{ ppq: 0, value: -6, curveToNext: 'linear' }];
    // The retired positional encoding, for the lane that now holds track 7.
    for (const positional of [0x4d580101, 0x4d580001, 0x4d58fe00]) {
      for (const call of [
        () => engine.setParameter(positional, -6, undefined, { smoothed: true }),
        () => engine.setParameter(positional, -6),
        () => engine.setAutomationLane(positional, point),
      ]) {
        let error: unknown;
        try {
          call();
        } catch (e) {
          error = e;
        }
        expect(error).toBeInstanceOf(RangeError);
        expect(isSonareError(error)).toBe(false);
      }
    }
    const laneFader = engine.resolveStripAutomationId(7, 'faderDb');
    expect(() => engine.setParameter(laneFader, -6, undefined, { smoothed: true })).not.toThrow();
    expect(() => engine.setAutomationLane(laneFader, point)).not.toThrow();
    expect(() => engine.setParameter(0x4d58ff01, -6, undefined, { smoothed: true })).not.toThrow();
    expect(() => engine.setAutomationLane(0x4d58ff01, point)).not.toThrow();
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
      sceneStrip('{"version":2,"strips":[{"id":"master"}],"buses":[],"connections":[]}'),
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
    let eqOut: Float32Array<ArrayBufferLike> = new Float32Array(256);
    for (let block = 0; block < 6; block += 1) {
      [eqOut] = engine.process([new Float32Array(256)]);
    }
    expect(rms(eqOut)).toBeGreaterThan(rms(flatOut) * 1.5);
    engine.destroy();
  });

  it('exposes the mastering processor catalog with role and capability flags', () => {
    const catalog = masteringProcessorCatalog();
    expect(catalog.length).toBeGreaterThan(0);

    const byId = (id: string) => catalog.find((e) => e.id === id);

    const compressor = byId('dynamics.compressor');
    expect(compressor).toBeDefined();
    expect(compressor?.kind).toBe('realtime');
    expect(compressor?.realtimeInsertable).toBe(true);
    expect(typeof compressor?.latencySamples).toBe('number');
    expect(typeof compressor?.tailSamples).toBe('number');
    expect(compressor?.realtimeCost).toBe('low');
    // Per-channel/linked processors process every plane in one call.
    expect(compressor?.channelPolicy).toBe('multichannel');

    const abCrossfade = byId('match.abCrossfade');
    expect(abCrossfade).toBeDefined();
    expect(abCrossfade?.kind).toBe('pair');

    const loudnessOptimize = byId('maximizer.loudnessOptimize');
    expect(loudnessOptimize).toBeDefined();
    expect(loudnessOptimize?.kind).toBe('offline');
    expect(loudnessOptimize?.realtimeInsertable).toBe(false);

    const midSide = byId('eq.midSide');
    expect(midSide).toBeDefined();
    expect(midSide?.stereoOnly).toBe(true);
    // Inherently-stereo processors are wrapped on the front L/R pair.
    expect(midSide?.channelPolicy).toBe('stereoPairOnly');

    const imager = byId('stereo.imager');
    expect(imager?.channelPolicy).toBe('stereoPairOnly');

    expect(byId('stereo.haasEnhancer')?.tailSamples).toBe(576);
    expect(byId('stereo.phaseAlign')?.tailSamples).toBe(0);
    expect(byId('effects.reverb.velvet')?.realtimeCost).toBe('high');
    expect(byId('effects.reverb.fdn')?.realtimeCost).toBe('moderate');

    // A repair stage publishes the bounds its own validation enforces, and says whether it can be causal.
    expect(byId('repair.declick')?.causal).toBe(false);
    expect(byId('repair.declip')?.causal).toBe(false);
    for (const id of [
      'repair.decrackle',
      'repair.dehum',
      'repair.denoiseClassical',
      'repair.dereverbClassical',
    ]) {
      expect(byId(id)?.causal).toBe(true);
    }
    expect(byId('dynamics.compressor')?.causal).toBe(true);
    const clickRun = byId('repair.declick')?.params.find(
      (param) => param.name === 'maxClickSamples',
    );
    expect(clickRun).toMatchObject({
      min: 1,
      max: 512,
      unit: 'samples',
      rtSafe: false,
    });
    expect(clickRun).not.toHaveProperty('id');

    // The voice changer is a stereo-pair insert whose bounds are the ones construction refuses
    // outside of; its latency-changing switches are construction-only.
    const voice = byId('voice.changer');
    expect(voice).toMatchObject({
      kind: 'realtime',
      category: 'voice',
      channelPolicy: 'stereoPairOnly',
      causal: true,
    });
    expect(voice?.latencySamples).toBeGreaterThan(0);
    const voiceParam = (name: string) => voice?.params.find((param) => param.name === name);
    expect(voiceParam('retunePitchSemitones')).toMatchObject({
      min: -24,
      max: 24,
      unit: 'semitones',
      rtSafe: true,
    });
    for (const name of ['retuneGrainSizeSamples', 'limiterEnableIspLimiter', 'reverbSeed']) {
      expect(voiceParam(name)).toMatchObject({ rtSafe: false });
      expect(voiceParam(name)).not.toHaveProperty('id');
    }

    // The registry emits `category` and `params` unconditionally, but the TS
    // interface stopped at `channelPolicy`, so reading either was a TS2339 on a
    // value that was already there. Compare the runtime key set against the
    // declared one rather than spot-checking, so the next added field cannot go
    // undeclared either.
    const declared: Record<keyof MasteringProcessorCatalogEntry, true> = {
      id: true,
      kind: true,
      forms: true,
      realtimeInsertable: true,
      stereoOnly: true,
      latencySamples: true,
      tailSamples: true,
      realtimeCost: true,
      channelPolicy: true,
      category: true,
      causal: true,
      params: true,
      slots: true,
      offlineParams: true,
    };
    // `realtimeCost` is absent for a processor that is not an insert, and
    // `offlineParams` unless the offline form declares its own set.
    const optional = new Set(['realtimeCost', 'offlineParams']);
    for (const entry of catalog) {
      const keys = Object.keys(entry);
      expect(keys.filter((key) => !(key in declared))).toEqual([]);
      expect(
        Object.keys(declared).filter((key) => !optional.has(key) && !keys.includes(key)),
      ).toEqual([]);
    }
    expect(compressor?.category).toBe('dynamics');
    expect(abCrossfade?.category).toBe('reference');
    expect(compressor?.params.map((param) => param.name).sort()).toEqual(
      masteringInsertParamInfo({ insert: 'dynamics.compressor' })
        .map((param) => param.name)
        .sort(),
    );
    // Non-insertable entries carry an empty list, not a missing key.
    expect(loudnessOptimize?.params).toEqual([]);
    // A crossover band past the default split is listed with the cutoffs it needs.
    const band3 = byId('multiband.compressor')?.slots.find((slot) => slot.name === 'band3');
    expect(band3).toEqual({
      name: 'band3',
      activation: 'always',
      minCrossoverCutoffs: 3,
    });
    expect(band3).not.toHaveProperty('parent');
  });

  it('reports realtime insert param descriptors and changes them live', () => {
    const info = masteringInsertParamInfo({ insert: 'effects.reverb.fdn' });
    if (info.length === 0) {
      return; // FX not built in this configuration.
    }
    const dryWet = info.find((d) => d.name === 'dryWet');
    expect(dryWet).toBeDefined();
    expect(dryWet?.rtSafe).toBe(true);
    expect(typeof dryWet?.id).toBe('number');
    expect(masteringInsertParamInfo({ insert: 'nope.nope' as never })).toEqual([]);

    const engine = new RealtimeEngine({ sampleRate: 48000, maxBlockSize: 256 });
    const frames = 256 * 16;
    const source = new Float32Array(frames);
    for (let i = 0; i < frames; i++) {
      source[i] = Math.sin((2 * Math.PI * 1000 * i) / 48000);
    }
    engine.setClips([
      { id: 1, trackId: 10, channels: [source], startPpq: 0, lengthSamples: frames },
    ]);
    engine.setTrackLanes([{ trackId: 10 }]);
    engine.setStrip(
      10,
      sceneStrip(
        JSON.stringify({
          version: 2,
          strips: [
            {
              id: 'track-10',
              inserts: [
                {
                  slot: 'pre',
                  processor: 'effects.reverb.fdn',
                  params: { dryWet: 0.0, decaySec: 2.0 },
                },
              ],
            },
          ],
          buses: [],
          connections: [],
        }),
      ),
    );

    // Bad arguments are rejected.
    expectRefusal(
      () => engine.setStripInsertParam(0, 0, { name: 'dryWet', value: 1 }),
      RangeError,
      /^RealtimeEngine\.setStripInsertParam: ref must be the id of a track in this engine$/,
    );
    expectRefusal(
      () => engine.setStripInsertParam(10, 0, { name: 'bogusParam', value: 1 }),
      RangeError,
      /^RealtimeEngine\.setStripInsertParam: param\.name must be the name of a parameter of the insert that can be set while running$/,
    );

    engine.play();
    let dry: Float32Array<ArrayBufferLike> = new Float32Array(256);
    for (let b = 0; b < 8; b++) {
      dry = engine.process([new Float32Array(256)])[0];
    }
    const dryRms = rms(dry);

    engine.setStripInsertParam(10, 0, { name: 'dryWet', value: 1.0 });
    let wet: Float32Array<ArrayBufferLike> = new Float32Array(256);
    for (let b = 0; b < 8; b++) {
      wet = engine.process([new Float32Array(256)])[0];
    }
    const wetRms = rms(wet);

    expect(dryRms).toBeGreaterThan(0);
    expect(Math.abs(wetRms - dryRms)).toBeGreaterThan(0.05 * dryRms);
    engine.destroy();
  });

  it('resolves and sets bus insert automation by name', () => {
    const engine = new RealtimeEngine({
      sampleRate: 48000,
      maxBlockSize: 256,
      commandCapacity: 64,
      telemetryCapacity: 64,
    });
    engine.setTrackBuses([{ busId: 1, gainDb: 0, channelLayout: 'stereo' }]);
    engine.setBusStrip(
      1,
      sceneBus(
        JSON.stringify({
          version: 2,
          strips: [],
          buses: [
            {
              id: '1',
              inserts: [
                {
                  slot: 'pre',
                  processor: 'eq.parametric',
                  params: {
                    'band0.type': 1,
                    'band0.frequencyHz': 1000,
                    'band0.gainDb': 0,
                    'band0.enabled': 1,
                  },
                },
              ],
            },
          ],
          connections: [],
        }),
      ),
    );

    // Resolve the bus insert parameter to its reserved automation id (top 3 bits 111).
    const automationId = engine.resolveBusInsertAutomationId(1, 0, 'band0.gainDb');
    expect(automationId).toBeGreaterThan(0);
    expect(automationId >>> 29).toBe(0x7);

    // The reserved id drives an automation lane and a one-off parameter set.
    engine.setAutomationLane(automationId, [{ ppq: 0, value: 6 }]);
    engine.setParameter(automationId, 3);

    // And the by-name manual set reaches the same target.
    engine.setBusInsertParam(1, 0, { name: 'band0.gainDb', value: -3 });

    // Unknown bus / insert / name resolve to -1.
    expect(() => engine.resolveBusInsertAutomationId(9, 0, 'band0.gainDb')).toThrow(RangeError);
    expect(() => engine.resolveBusInsertAutomationId(1, 0, 'nope')).toThrow(RangeError);

    let badBusError: unknown;
    try {
      engine.setBusInsertParam(9, 0, { name: 'band0.gainDb', value: 1 });
    } catch (error) {
      badBusError = error;
    }
    expect(badBusError).toBeInstanceOf(RangeError);
    expect(isSonareError(badBusError)).toBe(false);
    engine.destroy();
  });

  it('applies, restores and clears retained insert parameters on track, master and bus strips', () => {
    const engine = new RealtimeEngine({
      sampleRate: 48000,
      maxBlockSize: 256,
      commandCapacity: 64,
      telemetryCapacity: 64,
    });
    engine.setTrackLanes([{ trackId: 10 }]);
    engine.setStrip(
      10,
      sceneStrip(
        JSON.stringify({
          version: 2,
          strips: [
            {
              id: 'track-10',
              inserts: [{ slot: 'pre', processor: 'effects.reverb.fdn', params: { dryWet: 0.0 } }],
            },
          ],
          buses: [],
          connections: [],
        }),
      ),
    );
    engine.setStrip(
      'master',
      sceneStrip(
        JSON.stringify({
          version: 2,
          strips: [
            {
              id: 'master',
              inserts: [
                {
                  slot: 'pre',
                  processor: 'eq.parametric',
                  params: {
                    'band0.type': 1,
                    'band0.frequencyHz': 1000,
                    'band0.gainDb': 0,
                    'band0.enabled': 1,
                  },
                },
              ],
            },
          ],
          buses: [],
          connections: [],
        }),
      ),
    );
    engine.setTrackBuses([{ busId: 1, gainDb: 0, channelLayout: 'stereo' }]);
    engine.setBusStrip(
      1,
      sceneBus(
        JSON.stringify({
          version: 2,
          strips: [],
          buses: [
            {
              id: '1',
              inserts: [
                {
                  slot: 'pre',
                  processor: 'eq.parametric',
                  params: {
                    'band0.type': 1,
                    'band0.frequencyHz': 1000,
                    'band0.gainDb': 0,
                    'band0.enabled': 1,
                  },
                },
              ],
            },
          ],
          connections: [],
        }),
      ),
    );

    // apply-now returns true for a known insert/param and false (never a throw)
    // for an unknown target or name.
    expect(engine.applyStripInsertParamNow(10, 0, { name: 'dryWet', value: 1.0 })).toBe(true);
    expect(engine.applyStripInsertParamNow(10, 0, { name: 'bogusParam', value: 1.0 })).toBe(false);
    expect(engine.applyStripInsertParamNow(99, 0, { name: 'dryWet', value: 1.0 })).toBe(false);
    expect(engine.applyStripInsertParamNow('master', 0, { name: 'band0.gainDb', value: -3 })).toBe(
      true,
    );
    expect(engine.applyStripInsertParamNow('master', 0, { name: 'bogusParam', value: -3 })).toBe(
      false,
    );
    expect(engine.applyBusInsertParamNow(1, 0, { name: 'band0.gainDb', value: -3 })).toBe(true);
    expect(engine.applyBusInsertParamNow(9, 0, { name: 'band0.gainDb', value: -3 })).toBe(false);

    // restore applies exactly and throws for an unknown track/bus.
    expect(() =>
      engine.restoreStripInsertParam(10, 0, { name: 'dryWet', value: 0.5 }),
    ).not.toThrow();
    expect(() =>
      engine.restoreStripInsertParam('master', 0, { name: 'band0.gainDb', value: 1 }),
    ).not.toThrow();
    expect(() =>
      engine.restoreBusInsertParam(1, 0, { name: 'band0.gainDb', value: 1 }),
    ).not.toThrow();

    let restoreTrackError: unknown;
    try {
      engine.restoreStripInsertParam(99, 0, { name: 'dryWet', value: 0.5 });
    } catch (error) {
      restoreTrackError = error;
    }
    expect(restoreTrackError).toBeInstanceOf(RangeError);
    expect(isSonareError(restoreTrackError)).toBe(false);

    let restoreBusError: unknown;
    try {
      engine.restoreBusInsertParam(99, 0, { name: 'band0.gainDb', value: 1 });
    } catch (error) {
      restoreBusError = error;
    }
    expect(restoreBusError).toBeInstanceOf(RangeError);
    expect(isSonareError(restoreBusError)).toBe(false);

    // clear works for a known target and throws for an unknown track/bus.
    expect(() => engine.clearStripInsertParameterBases(10)).not.toThrow();
    expect(() => engine.clearStripInsertParameterBases('master')).not.toThrow();
    expect(() => engine.clearBusInsertParameterBases(1)).not.toThrow();

    let clearTrackError: unknown;
    try {
      engine.clearStripInsertParameterBases(99);
    } catch (error) {
      clearTrackError = error;
    }
    expect(clearTrackError).toBeInstanceOf(RangeError);
    expect(isSonareError(clearTrackError)).toBe(false);

    let clearBusError: unknown;
    try {
      engine.clearBusInsertParameterBases(99);
    } catch (error) {
      clearBusError = error;
    }
    expect(clearBusError).toBeInstanceOf(RangeError);
    expect(isSonareError(clearBusError)).toBe(false);

    engine.destroy();
  });
});

describe('RealtimeEngine prime, reset, tail and latency', () => {
  const block = 256;
  const blocks = 24;
  const frames = block * blocks;

  const delayStripJson = JSON.stringify({
    version: 2,
    strips: [
      {
        id: 'track-10',
        inserts: [
          {
            slot: 'post',
            processor: 'effects.delay.stereo',
            params: {
              delayTimeLMs: 5,
              delayTimeRMs: 7,
              feedback: 0.6,
              dryWet: 0.5,
            },
          },
        ],
      },
    ],
    buses: [],
    connections: [],
  });

  /** A burst followed by silence, so a delay tail is still ringing when playback stops. */
  function makeRig(withDelay: boolean): RealtimeEngine {
    const engine = new RealtimeEngine({ sampleRate: 48000, maxBlockSize: block });
    const samples = new Float32Array(frames);
    for (let i = 0; i < block * 4; i += 1) {
      samples[i] = 0.5 * Math.sin(i * 0.37);
    }
    engine.setClips([
      { id: 1, trackId: 10, channels: [samples], startPpq: 0, lengthSamples: frames },
    ]);
    engine.setTrackLanes([{ trackId: 10 }]);
    if (withDelay) {
      engine.setStrip(10, sceneStrip(delayStripJson));
    }
    return engine;
  }

  function renderFromTop(engine: RealtimeEngine, count: number): Float32Array {
    engine.seekSample(0);
    engine.play();
    const out = new Float32Array(count * block);
    for (let b = 0; b < count; b += 1) {
      const [left] = engine.process([new Float32Array(block), new Float32Array(block)]);
      out.set(left, b * block);
    }
    engine.stop();
    return out;
  }

  function expectClose(actual: Float32Array, expected: Float32Array): void {
    expect(actual.length).toBe(expected.length);
    let maxDiff = 0;
    let peak = 0;
    for (let i = 0; i < expected.length; i += 1) {
      maxDiff = Math.max(maxDiff, Math.abs(actual[i] - expected[i]));
      peak = Math.max(peak, Math.abs(expected[i]));
    }
    expect(peak).toBeGreaterThan(0.05);
    expect(maxDiff).toBeLessThanOrEqual(1e-6 * Math.max(1, peak));
  }

  it('renders a primed engine like a fresh primed one after a dirty pass', () => {
    const fresh = makeRig(true);
    fresh.primeOfflineParameters(2, block);
    const reference = renderFromTop(fresh, 8);
    fresh.destroy();

    const dirty = makeRig(true);
    renderFromTop(dirty, 3);
    dirty.primeOfflineParameters(2, block);
    expectClose(renderFromTop(dirty, 8), reference);
    dirty.destroy();
  });

  it('renders after resetProcessorState like a fresh engine', () => {
    const fresh = makeRig(true);
    const reference = renderFromTop(fresh, 8);
    fresh.destroy();

    const dirty = makeRig(true);
    renderFromTop(dirty, 3);
    dirty.resetProcessorState();
    expectClose(renderFromTop(dirty, 8), reference);
    dirty.destroy();

    const explicitFrame = makeRig(true);
    renderFromTop(explicitFrame, 3);
    explicitFrame.resetProcessorState(0);
    expectClose(renderFromTop(explicitFrame, 8), reference);
    explicitFrame.destroy();
  });

  it('leaves the delay tail in place without a reset', () => {
    const fresh = makeRig(true);
    const reference = renderFromTop(fresh, 8);
    fresh.destroy();

    const dirty = makeRig(true);
    renderFromTop(dirty, 3);
    const rerun = renderFromTop(dirty, 8);
    let maxDiff = 0;
    for (let i = 0; i < reference.length; i += 1) {
      maxDiff = Math.max(maxDiff, Math.abs(rerun[i] - reference[i]));
    }
    expect(maxDiff).toBeGreaterThan(1e-4);
    dirty.destroy();
  });

  it('validates primeOfflineParameters arguments', () => {
    const engine = makeRig(false);
    expectRefusal(
      () => engine.primeOfflineParameters(0, block),
      RangeError,
      /^RealtimeEngine\.primeOfflineParameters: numChannels must be an integer in \[1, 64\]$/,
    );
    expectRefusal(
      () => engine.primeOfflineParameters(2, 0),
      RangeError,
      /^RealtimeEngine\.primeOfflineParameters: blockSize must be an integer in \[1, 2147483647\]$/,
    );
    expectRefusal(
      () => engine.primeOfflineParameters(1000, block),
      RangeError,
      /^RealtimeEngine\.primeOfflineParameters: numChannels must be an integer in \[1, 64\]$/,
    );
    expect(() => engine.primeOfflineParameters(2, block)).not.toThrow();
    engine.destroy();
  });

  it('reports a tail only when an insert holds one', () => {
    const plain = makeRig(false);
    expect(plain.tailSamples).toBe(0);
    plain.destroy();

    const delayed = makeRig(true);
    expect(delayed.tailSamples).toBeGreaterThan(0);
    delayed.destroy();
  });

  it('reports the graph latency as a number', () => {
    const engine = makeRig(false);
    const latency = engine.graphLatencySamples;
    expect(typeof latency).toBe('number');
    expect(Number.isInteger(latency)).toBe(true);
    engine.destroy();
  });
});
