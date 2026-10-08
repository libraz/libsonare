import { describe, expect, it } from 'vitest';
import type { MasteringProcessorCatalogEntry } from '../src/index.js';
import {
  ErrorCode,
  isSonareError,
  masteringInsertParamInfo,
  masteringProcessorCatalog,
  RealtimeEngine,
} from '../src/index.js';
import { rms } from './_engine_signals.js';

describe('RealtimeEngine native binding', () => {
  /** Asserts that `call` throws a SonareError carrying exactly `code`. */
  const expectSonareErrorCode = (call: () => void, code: ErrorCode): void => {
    let error: unknown;
    try {
      call();
    } catch (thrown) {
      error = thrown;
    }
    expect(isSonareError(error)).toBe(true);
    if (!isSonareError(error)) {
      throw new Error('expected SonareError');
    }
    expect(error.code).toBe(code);
  };

  it('routes track clips through lanes and lane commands', () => {
    const engine = new RealtimeEngine(48000, 256);
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
    engine.setTrackLanes([10, { trackId: 20 }]);
    let duplicateLaneError: unknown;
    try {
      engine.setTrackLanes([{ trackId: 10 }, { trackId: 10 }]);
    } catch (error) {
      duplicateLaneError = error;
    }
    expect(isSonareError(duplicateLaneError)).toBe(true);
    if (!isSonareError(duplicateLaneError)) {
      throw new Error('expected SonareError');
    }
    expect(duplicateLaneError.code).toBe(ErrorCode.InvalidParameter);
    engine.setTrackLanes([10, { trackId: 20 }]);

    engine.play();
    let processed = engine.process([new Float32Array(256), new Float32Array(256)]);
    expect(processed[0].at(-1)).toBeCloseTo(2, 4);
    expect(processed[1].at(-1)).toBeCloseTo(2, 4);

    engine.setSoloMute(0, true, false);
    for (let block = 0; block < 4; block += 1) {
      processed = engine.process([new Float32Array(256), new Float32Array(256)]);
    }
    expect(processed[0].at(-1)).toBeGreaterThan(0.75);
    expect(processed[0].at(-1)).toBeLessThan(1.25);

    engine.setParameterSmoothed(engine.resolveTrackLaneAutomationId(10, 'faderDb'), -12);
    for (let block = 0; block < 6; block += 1) {
      processed = engine.process([new Float32Array(256), new Float32Array(256)]);
    }
    expect(processed[0].at(-1)).toBeLessThan(0.45);
    expect(processed[1].at(-1)).toBeLessThan(0.45);
    engine.destroy();
  });

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

    // A surround bus/source layout flows through to the native struct fields
    // (the native side validates the enum range).
    expect(() => engine.setTrackBuses([{ busId: 1, gainDb: 0, channelLayout: 2 }])).not.toThrow();
    // Only a stereo (or omitted) lane layout is accepted; the others are refused.
    expect(() => engine.setTrackLanes([{ trackId: 10, sourceChannelLayout: 1 }])).not.toThrow();
    expect(() => engine.setTrackLanes([{ trackId: 10 }])).not.toThrow();
    for (const layout of [0, 2, 3] as const) {
      expect(() => engine.setTrackLanes([{ trackId: 10, sourceChannelLayout: layout }])).toThrow(
        /must be stereo/,
      );
    }
    // 257/-255 narrow to 1 (Stereo, valid) if the addon range-checks the
    // uint8_t C-ABI field's value AFTER narrowing to it instead of before --
    // the wrap this guards against on both the lane and the bus reader.
    // @ts-expect-error deliberately out-of-range layout ordinal; the addon must reject it.
    expect(() => engine.setTrackLanes([{ trackId: 10, sourceChannelLayout: 257 }])).toThrow(
      /\[0, 255\]/,
    );
    // @ts-expect-error deliberately out-of-range layout ordinal; the addon must reject it.
    expect(() => engine.setTrackBuses([{ busId: 1, gainDb: 0, channelLayout: 257 }])).toThrow(
      /\[0, 255\]/,
    );

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
    // A 5.1 bus feeding a narrower (mono) output downmixes rather than
    // truncating to the front channels, so the unity send adds half the direct level.
    expect(out.at(-1)).toBeGreaterThan(2.12);
    expect(out.at(-1)).toBeLessThan(2.13);
    const meterTargets = new Set(engine.drainMeterTelemetry().map((record) => record.targetId));
    expect(meterTargets.has(1)).toBe(true);
    expect(meterTargets.has(33)).toBe(true);
    expect(meterTargets.has(0)).toBe(true);

    engine.setTrackLanes([{ trackId: 10, sends: [{ busId: 1, levelDb: -6.0206 }] }]);
    engine.seekSample(0);
    [out] = engine.process([new Float32Array(256)]);
    expect(out.at(-1)).toBeGreaterThan(1.76);
    expect(out.at(-1)).toBeLessThan(1.78);

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

  it('drains per-plane meter telemetry for a surround group bus', () => {
    const engine = new RealtimeEngine(48000, 256);
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
    engine.setTrackBuses([{ busId: 1, gainDb: 0, channelLayout: 2 }]);
    engine.setTrackLanes([{ trackId: 10, outputBusId: 1 }]);
    engine.setTrackStripJson(
      10,
      '{"version":1,"buses":[{"id":"master","role":"master"}],"strips":[{"id":"s","surroundPan":{"azimuth":-110}}]}',
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
    const wide = engine.drainMeterTelemetryWide();
    const busMeter = wide.find((record) => record.targetId === 33);
    expect(busMeter).toBeDefined();
    if (!busMeter) {
      throw new Error('expected a 5.1 bus meter');
    }
    expect(busMeter.channelCount).toBe(6);
    expect(busMeter.peakDb).toHaveLength(6);
    // Ls (plane 4) carries the panned lane, well above the silent front-left.
    expect(busMeter.peakDb[4]).toBeGreaterThan(busMeter.peakDb[0] + 10);
    engine.destroy();
  });

  it('moves a lane across surround planes live with setTrackStripSurroundPan', () => {
    const frames = 256;
    const blocks = 16;
    const engine = new RealtimeEngine(48000, frames);
    engine.setClips([
      {
        id: 1,
        trackId: 10,
        channels: [new Float32Array(frames * blocks).fill(0.5)],
        startPpq: 0,
        lengthSamples: frames * blocks,
      },
    ]);
    engine.setTrackBuses([{ busId: 1, gainDb: 0, channelLayout: 2 }]);
    engine.setTrackLanes([{ trackId: 10, outputBusId: 1 }]);
    engine.setTrackStripJson(
      10,
      '{"version":1,"buses":[{"id":"master","role":"master"}],"strips":[{"id":"s"}]}',
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
    engine.setTrackStripSurroundPan(10, { azimuth: -110 });
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

    engine.setTrackStripSurroundPan(10, { azimuth: 110 });
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

    expect(() => engine.setTrackStripSurroundPan(10, { azimuth: Number.NaN })).toThrow();
    expect(() => engine.setTrackStripSurroundPan(999, { azimuth: 0 })).toThrow();
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

  it('applies realtime track strip pan setters', () => {
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
    ]);
    engine.setTrackLanes([10]);
    engine.setTrackStripJson(
      10,
      '{"version":1,"strips":[{"id":"track-10","panLaw":3}],"buses":[],"connections":[]}',
    );

    expect(() => engine.setTrackStripPanLaw(10, 'linear0dB')).not.toThrow();
    expect(() => engine.setTrackStripPanMode(10, 'balance')).not.toThrow();
    expect(() => engine.setTrackStripChannelDelaySamples(10, 0)).not.toThrow();
    expect(() => engine.setTrackStripDualPan(10, -1, -1)).not.toThrow();

    // Hard-left pan: the left output channel must dominate the right.
    engine.setTrackStripPanMode(10, 'balance');
    engine.setTrackStripPan(10, -1);
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
      const engine = new RealtimeEngine(48000, 256);
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
        engine.setTrackLanes([10]);
        engine.setTrackStripJson(
          10,
          '{"version":1,"strips":[{"id":"track-10","panLaw":3}],"buses":[],"connections":[]}',
        );
        engine.setTrackStripPanMode(10, 'dualPan');
        engine.setTrackStripDualPan(10, leftPan, rightPan);
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
    let eqOut: Float32Array<ArrayBufferLike> = new Float32Array(256);
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
    let eqOut: Float32Array<ArrayBufferLike> = new Float32Array(256);
    for (let block = 0; block < 6; block += 1) {
      [eqOut] = engine.process([new Float32Array(256)]);
    }
    expect(rms(eqOut)).toBeGreaterThan(rms(flatOut) * 1.5);
    engine.destroy();
  });

  it('applies realtime bus strip pan setters', () => {
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
    ]);
    engine.setTrackBuses([{ busId: 1, gainDb: 0 }]);
    engine.setTrackLanes([{ trackId: 10, outputBusId: 1 }]);

    expect(() => engine.setBusStripPanLaw(1, 'linear0dB')).not.toThrow();
    expect(() => engine.setBusStripPanMode(1, 'balance')).not.toThrow();
    expect(() => engine.setBusStripDualPan(1, -1, -1)).not.toThrow();

    // Hard-left pan on the bus: the left output channel must dominate the right.
    engine.setBusStripPanMode(1, 'balance');
    engine.setBusStripPan(1, -1);
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
    engine.setTrackBuses([{ busId: 1, gainDb: 0 }]);
    engine.setTrackLanes([{ trackId: 10, outputBusId: 1 }]);
    expectSonareErrorCode(
      () => engine.setBusStripEqBand(1, 99, { type: 'Peak', enabled: true }),
      ErrorCode.InvalidParameter,
    );

    engine.play();
    const [flatOut] = engine.process([new Float32Array(256)]);
    engine.setBusStripEqBand(1, 0, {
      type: 'Peak',
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
    const engine = new RealtimeEngine(48000, 256);
    engine.setTrackBuses([{ busId: 1, gainDb: 0 }]);

    expectSonareErrorCode(() => engine.setBusStripPan(0, -1), ErrorCode.InvalidParameter);
    expectSonareErrorCode(() => engine.setBusStripPan(99, -1), ErrorCode.InvalidParameter);
    expectSonareErrorCode(() => engine.setBusStripPan(1, Number.NaN), ErrorCode.InvalidParameter);
    expectSonareErrorCode(
      () => engine.setBusStripPanLaw(99, 'const6dB'),
      ErrorCode.InvalidParameter,
    );
    expectSonareErrorCode(
      () => engine.setBusStripPanMode(99, 'stereoPan'),
      ErrorCode.InvalidParameter,
    );
    expectSonareErrorCode(() => engine.setBusStripDualPan(99, -1, 1), ErrorCode.InvalidParameter);
    expectSonareErrorCode(
      () => engine.setBusStripDualPan(1, Number.POSITIVE_INFINITY, 1),
      ErrorCode.InvalidParameter,
    );

    engine.destroy();
  });

  it('refuses bus strip pan setters on a surround bus, but still allows EQ', () => {
    const engine = new RealtimeEngine(48000, 256);
    engine.setTrackBuses([{ busId: 1, gainDb: 0, channelLayout: 2 }]); // 5.1

    expectSonareErrorCode(() => engine.setBusStripPan(1, -1), ErrorCode.InvalidParameter);
    expectSonareErrorCode(
      () => engine.setBusStripPanLaw(1, 'const6dB'),
      ErrorCode.InvalidParameter,
    );
    expectSonareErrorCode(
      () => engine.setBusStripPanMode(1, 'stereoPan'),
      ErrorCode.InvalidParameter,
    );
    expectSonareErrorCode(() => engine.setBusStripDualPan(1, -1, 1), ErrorCode.InvalidParameter);

    expect(() =>
      engine.setBusStripEqBand(1, 0, {
        type: 'Peak',
        frequencyHz: 1000,
        gainDb: 6,
        q: 1,
        enabled: true,
      }),
    ).not.toThrow();

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
    expect(clickRun).toMatchObject({ min: 1, max: 512, unit: 'samples', id: null, rtSafe: false });

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
    expect(voiceParam('retuneSemitones')).toMatchObject({
      min: -24,
      max: 24,
      unit: 'semitones',
      rtSafe: true,
    });
    for (const name of ['retuneGrainSize', 'limiterEnableIspLimiter', 'reverbSeed']) {
      expect(voiceParam(name)).toMatchObject({ id: null, rtSafe: false });
    }

    // The registry emits `category` and `params` unconditionally, but the TS
    // interface stopped at `channelPolicy`, so reading either was a TS2339 on a
    // value that was already there. Compare the runtime key set against the
    // declared one rather than spot-checking, so the next added field cannot go
    // undeclared either.
    const declared: Record<keyof MasteringProcessorCatalogEntry, true> = {
      id: true,
      kind: true,
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
    };
    for (const entry of catalog) {
      expect(Object.keys(entry).sort()).toEqual(Object.keys(declared).sort());
    }
    expect(compressor?.category).toBe('dynamics');
    expect(abCrossfade?.category).toBe('reference');
    expect(compressor?.params.map((param) => param.name).sort()).toEqual(
      masteringInsertParamInfo('dynamics.compressor')
        .map((param) => param.name)
        .sort(),
    );
    // Non-insertable entries carry an empty list, not a missing key.
    expect(loudnessOptimize?.params).toEqual([]);
    // A crossover band past the default split is listed with the cutoffs it needs.
    const band3 = byId('multiband.compressor')?.slots.find((slot) => slot.name === 'band3');
    expect(band3).toEqual({
      name: 'band3',
      parent: null,
      activation: 'always',
      minCrossoverCutoffs: 3,
    });
  });

  it('reports realtime insert param descriptors and changes them live', () => {
    const info = masteringInsertParamInfo('effects.reverb.fdn');
    if (info.length === 0) {
      return; // FX not built in this configuration.
    }
    const dryWet = info.find((d) => d.name === 'dryWet');
    expect(dryWet).toBeDefined();
    expect(dryWet?.rtSafe).toBe(true);
    expect(typeof dryWet?.id).toBe('number');
    expect(masteringInsertParamInfo('nope.nope')).toEqual([]);

    const engine = new RealtimeEngine(48000, 256);
    const frames = 256 * 16;
    const source = new Float32Array(frames);
    for (let i = 0; i < frames; i++) {
      source[i] = Math.sin((2 * Math.PI * 1000 * i) / 48000);
    }
    engine.setClips([
      { id: 1, trackId: 10, channels: [source], startPpq: 0, lengthSamples: frames },
    ]);
    engine.setTrackLanes([10]);
    engine.setTrackStripJson(
      10,
      JSON.stringify({
        version: 1,
        strips: [
          {
            id: 'track-10',
            inserts: [
              {
                slot: 'pre',
                processor: 'effects.reverb.fdn',
                params: '{"dryWet":0.0,"decaySec":2.0}',
              },
            ],
          },
        ],
        buses: [],
        connections: [],
      }),
    );

    // Bad arguments are rejected.
    expect(() => engine.setTrackStripInsertParamByName(0, 0, 'dryWet', 1)).toThrow();
    expect(() => engine.setTrackStripInsertParamByName(10, 0, 'bogusParam', 1)).toThrow();

    engine.play();
    let dry: Float32Array<ArrayBufferLike> = new Float32Array(256);
    for (let b = 0; b < 8; b++) {
      dry = engine.process([new Float32Array(256)])[0];
    }
    const dryRms = rms(dry);

    engine.setTrackStripInsertParamByName(10, 0, 'dryWet', 1.0);
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
    const engine = new RealtimeEngine(48000, 256, 64, 64);
    engine.setTrackBuses([{ busId: 1, gainDb: 0, channelLayout: 1 }]);
    engine.setBusStripJson(
      1,
      JSON.stringify({
        version: 1,
        strips: [],
        buses: [
          {
            id: '1',
            inserts: [
              {
                slot: 'pre',
                processor: 'eq.parametric',
                params: JSON.stringify({
                  'band0.type': 1,
                  'band0.frequencyHz': 1000,
                  'band0.gainDb': 0,
                  'band0.enabled': 1,
                }),
              },
            ],
          },
        ],
        connections: [],
      }),
    );

    // Resolve the bus insert parameter to its reserved automation id (top 3 bits 111).
    const automationId = engine.resolveBusInsertAutomationId(1, 0, 'band0.gainDb');
    expect(automationId).toBeGreaterThan(0);
    expect(automationId >>> 29).toBe(0x7);

    // The reserved id drives an automation lane and a one-off parameter set.
    engine.setAutomationLane(automationId, [{ ppq: 0, value: 6 }]);
    engine.setParameter(automationId, 3);

    // And the by-name manual set reaches the same target.
    engine.setBusStripInsertParamByName(1, 0, 'band0.gainDb', -3);

    // Unknown bus / insert / name resolve to -1.
    expect(engine.resolveBusInsertAutomationId(9, 0, 'band0.gainDb')).toBe(-1);
    expect(engine.resolveBusInsertAutomationId(1, 0, 'nope')).toBe(-1);

    let badBusError: unknown;
    try {
      engine.setBusStripInsertParamByName(9, 0, 'band0.gainDb', 1);
    } catch (error) {
      badBusError = error;
    }
    expect(isSonareError(badBusError)).toBe(true);
    if (!isSonareError(badBusError)) {
      throw new Error('expected SonareError');
    }
    expect(badBusError.code).toBe(ErrorCode.InvalidParameter);
    engine.destroy();
  });

  it('applies, restores and clears retained insert parameters on track, master and bus strips', () => {
    const engine = new RealtimeEngine(48000, 256, 64, 64);
    engine.setTrackLanes([10]);
    engine.setTrackStripJson(
      10,
      JSON.stringify({
        version: 1,
        strips: [
          {
            id: 'track-10',
            inserts: [{ slot: 'pre', processor: 'effects.reverb.fdn', params: '{"dryWet":0.0}' }],
          },
        ],
        buses: [],
        connections: [],
      }),
    );
    engine.setMasterStripJson(
      JSON.stringify({
        version: 1,
        strips: [
          {
            id: 'master',
            inserts: [
              {
                slot: 'pre',
                processor: 'eq.parametric',
                params: JSON.stringify({
                  'band0.type': 1,
                  'band0.frequencyHz': 1000,
                  'band0.gainDb': 0,
                  'band0.enabled': 1,
                }),
              },
            ],
          },
        ],
        buses: [],
        connections: [],
      }),
    );
    engine.setTrackBuses([{ busId: 1, gainDb: 0, channelLayout: 1 }]);
    engine.setBusStripJson(
      1,
      JSON.stringify({
        version: 1,
        strips: [],
        buses: [
          {
            id: '1',
            inserts: [
              {
                slot: 'pre',
                processor: 'eq.parametric',
                params: JSON.stringify({
                  'band0.type': 1,
                  'band0.frequencyHz': 1000,
                  'band0.gainDb': 0,
                  'band0.enabled': 1,
                }),
              },
            ],
          },
        ],
        connections: [],
      }),
    );

    // apply-now returns true for a known insert/param and false (never a throw)
    // for an unknown target or name.
    expect(engine.applyTrackStripInsertParamByNameNow(10, 0, 'dryWet', 1.0)).toBe(true);
    expect(engine.applyTrackStripInsertParamByNameNow(10, 0, 'bogusParam', 1.0)).toBe(false);
    expect(engine.applyTrackStripInsertParamByNameNow(99, 0, 'dryWet', 1.0)).toBe(false);
    expect(engine.applyMasterStripInsertParamByNameNow(0, 'band0.gainDb', -3)).toBe(true);
    expect(engine.applyMasterStripInsertParamByNameNow(0, 'bogusParam', -3)).toBe(false);
    expect(engine.applyBusStripInsertParamByNameNow(1, 0, 'band0.gainDb', -3)).toBe(true);
    expect(engine.applyBusStripInsertParamByNameNow(9, 0, 'band0.gainDb', -3)).toBe(false);

    // restore applies exactly and throws for an unknown track/bus.
    expect(() => engine.restoreTrackStripInsertParamByName(10, 0, 'dryWet', 0.5)).not.toThrow();
    expect(() => engine.restoreMasterStripInsertParamByName(0, 'band0.gainDb', 1)).not.toThrow();
    expect(() => engine.restoreBusStripInsertParamByName(1, 0, 'band0.gainDb', 1)).not.toThrow();

    let restoreTrackError: unknown;
    try {
      engine.restoreTrackStripInsertParamByName(99, 0, 'dryWet', 0.5);
    } catch (error) {
      restoreTrackError = error;
    }
    expect(isSonareError(restoreTrackError)).toBe(true);
    if (!isSonareError(restoreTrackError)) {
      throw new Error('expected SonareError');
    }
    expect(restoreTrackError.code).toBe(ErrorCode.InvalidParameter);

    let restoreBusError: unknown;
    try {
      engine.restoreBusStripInsertParamByName(99, 0, 'band0.gainDb', 1);
    } catch (error) {
      restoreBusError = error;
    }
    expect(isSonareError(restoreBusError)).toBe(true);
    if (!isSonareError(restoreBusError)) {
      throw new Error('expected SonareError');
    }
    expect(restoreBusError.code).toBe(ErrorCode.InvalidParameter);

    // clear works for a known target and throws for an unknown track/bus.
    expect(() => engine.clearTrackInsertParameterBases(10)).not.toThrow();
    expect(() => engine.clearMasterInsertParameterBases()).not.toThrow();
    expect(() => engine.clearBusInsertParameterBases(1)).not.toThrow();

    let clearTrackError: unknown;
    try {
      engine.clearTrackInsertParameterBases(99);
    } catch (error) {
      clearTrackError = error;
    }
    expect(isSonareError(clearTrackError)).toBe(true);
    if (!isSonareError(clearTrackError)) {
      throw new Error('expected SonareError');
    }
    expect(clearTrackError.code).toBe(ErrorCode.InvalidParameter);

    let clearBusError: unknown;
    try {
      engine.clearBusInsertParameterBases(99);
    } catch (error) {
      clearBusError = error;
    }
    expect(isSonareError(clearBusError)).toBe(true);
    if (!isSonareError(clearBusError)) {
      throw new Error('expected SonareError');
    }
    expect(clearBusError.code).toBe(ErrorCode.InvalidParameter);

    engine.destroy();
  });
});

describe('RealtimeEngine prime, reset, tail and latency', () => {
  const block = 256;
  const blocks = 24;
  const frames = block * blocks;

  const delayStripJson = JSON.stringify({
    version: 1,
    strips: [
      {
        id: 'track-10',
        inserts: [
          {
            slot: 'post',
            processor: 'effects.delay.stereo',
            params: JSON.stringify({
              delayTimeLMs: 5,
              delayTimeRMs: 7,
              feedback: 0.6,
              dryWet: 0.5,
            }),
          },
        ],
      },
    ],
    buses: [],
    connections: [],
  });

  /** A burst followed by silence, so a delay tail is still ringing when playback stops. */
  function makeRig(withDelay: boolean): RealtimeEngine {
    const engine = new RealtimeEngine(48000, block);
    const samples = new Float32Array(frames);
    for (let i = 0; i < block * 4; i += 1) {
      samples[i] = 0.5 * Math.sin(i * 0.37);
    }
    engine.setClips([
      { id: 1, trackId: 10, channels: [samples], startPpq: 0, lengthSamples: frames },
    ]);
    engine.setTrackLanes([10]);
    if (withDelay) {
      engine.setTrackStripJson(10, delayStripJson);
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
    expect(() => engine.primeOfflineParameters(0, block)).toThrow();
    expect(() => engine.primeOfflineParameters(2, 0)).toThrow();
    expect(() => engine.primeOfflineParameters(1000, block)).toThrow();
    expect(() => engine.primeOfflineParameters(2, block)).not.toThrow();
    engine.destroy();
  });

  it('reports a tail only when an insert holds one', () => {
    const plain = makeRig(false);
    expect(plain.tailSamples()).toBe(0);
    plain.destroy();

    const delayed = makeRig(true);
    expect(delayed.tailSamples()).toBeGreaterThan(0);
    delayed.destroy();
  });

  it('reports the graph latency as a number', () => {
    const engine = makeRig(false);
    const latency = engine.graphLatencySamplesQ8();
    expect(typeof latency).toBe('number');
    expect(Number.isInteger(latency)).toBe(true);
    engine.destroy();
  });
});
