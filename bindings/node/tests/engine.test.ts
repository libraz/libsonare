import { mkdtempSync, rmSync, writeFileSync } from 'node:fs';
import { tmpdir } from 'node:os';
import { join } from 'node:path';
import { Worker } from 'node:worker_threads';
import { describe, expect, it } from 'vitest';
import type { EngineBounceOptions } from '../src/index.js';
import {
  Audio,
  ErrorCode,
  engineAbiVersion,
  isSonareError,
  MarkerKind,
  masteringInsertParamInfo,
  masteringProcessorCatalog,
  RealtimeEngine,
  voiceChangerAbiVersion,
} from '../src/index.js';

describe('RealtimeEngine native binding', () => {
  const rms = (data: Float32Array): number => {
    let sum = 0;
    for (const value of data) {
      sum += value * value;
    }
    return Math.sqrt(sum / data.length);
  };

  const midi1Word = (status: number, channel: number, data0: number, data1: number): number =>
    (0x2 << 28) | ((status & 0xf) << 20) | ((channel & 0xf) << 16) | (data0 << 8) | data1;

  it('exposes engine ABI version', () => {
    expect(engineAbiVersion()).toBeGreaterThan(0);
  });

  it('exposes voice changer ABI version', () => {
    const v = voiceChangerAbiVersion();
    expect(Number.isFinite(v)).toBe(true);
    expect(v).toBeGreaterThan(0);
  });

  it('rejects nonfinite and out-of-range engine sample rates', () => {
    for (const sampleRate of [Number.NaN, 7999, 384001]) {
      expect(() => new RealtimeEngine(sampleRate, 128)).toThrow();
    }
    const engine = new RealtimeEngine(48000, 128);
    try {
      for (const sampleRate of [Number.NaN, 7999, 384001]) {
        expect(() => engine.prepare(sampleRate, 128)).toThrow();
      }
    } finally {
      engine.destroy();
    }
  });

  it('accepts a declared channel capacity and rejects invalid capacities', () => {
    const engine = new RealtimeEngine(48000, 128);
    try {
      expect(() => engine.prepare(48000, 128, 1024, 1024, 2)).not.toThrow();
      expect(() => engine.prepare(48000, 128, 1024, 1024, 0)).toThrow();
      expect(() => engine.prepare(48000, 128, 1024, 1024, 65)).toThrow();
    } finally {
      engine.destroy();
    }
    expect(() => new RealtimeEngine(48000, 128, 1024, 1024, 0)).toThrow();
  });

  it('reports oversized process channels after prepare as max-channel telemetry', () => {
    const engine = new RealtimeEngine(48000, 128);
    try {
      engine.prepare(48000, 128, 1024, 1024, 2);
      engine.process([new Float32Array(128), new Float32Array(128), new Float32Array(128)]);

      expect(engine.drainTelemetry()).toContainEqual(
        expect.objectContaining({ error: 20, value: 3 }),
      );
    } finally {
      engine.destroy();
    }
  });

  it('keeps Audio factories usable after a worker loads and exits the addon', async () => {
    const addonPath = join(process.cwd(), 'build', 'Release', 'sonare-node.node');
    await new Promise<void>((resolve, reject) => {
      const worker = new Worker(`require(${JSON.stringify(addonPath)});`, { eval: true });
      worker.once('error', reject);
      worker.once('exit', (code) => {
        if (code === 0) {
          resolve();
        } else {
          reject(new Error(`worker exited with code ${code}`));
        }
      });
    });

    const audio = Audio.fromBuffer(new Float32Array([0.25, -0.25]), 48000);
    expect(audio.getLength()).toBe(2);
    audio.destroy();
  });

  it('installs tempo and time-signature segments and validates them', () => {
    const engine = new RealtimeEngine(48000, 128);
    // Valid ramp then a valid time-signature map.
    engine.setTempoSegments([
      { startPpq: 0, bpm: 120 },
      { startPpq: 1920, bpm: 120, endBpm: 140 },
    ]);
    engine.setTimeSignatureSegments([
      { startPpq: 0, numerator: 4, denominator: 4 },
      { startPpq: 1920, numerator: 3, denominator: 4 },
    ]);
    // Empty arrays clear the maps without error.
    engine.setTempoSegments([]);
    engine.setTimeSignatureSegments([]);
    // Invalid input is rejected (matching the C ABI and Python).
    expect(() => engine.setTempoSegments([{ startPpq: 0, bpm: 0 }])).toThrow();
    expect(() => engine.setTempoSegments([{ startPpq: Number.NaN, bpm: 120 }])).toThrow();
    expect(() => engine.setTempoSegments([{ startPpq: 0, bpm: 100000.1 }])).toThrow();
    expect(() => engine.setTempoSegments([{ startPpq: 0, bpm: 120, endBpm: 100000.1 }])).toThrow();
    expect(() => engine.setTempo(100000)).not.toThrow();
    expect(() => engine.setTempo(100000.1)).toThrow();
    expect(() =>
      engine.setTimeSignatureSegments([{ startPpq: 0, numerator: 0, denominator: 4 }]),
    ).toThrow();
  });

  it('rejects hostile metronome click lengths', () => {
    const engine = new RealtimeEngine(48000, 128);
    expect(() => engine.setMetronome({ enabled: true, clickSamples: 2_000_000_000 })).toThrow();
    expect(() =>
      engine.setMetronome({ enabled: true, clickSamples: 0, clickSeconds: 2 }),
    ).toThrow();
    expect(() =>
      engine.setMetronome({ enabled: true, clickSamples: 0, clickSeconds: Number.NaN }),
    ).toThrow();
  });

  it('rejects out-of-range automation curve ordinals', () => {
    const engine = new RealtimeEngine(48000, 128);
    engine.addParameter({
      id: 9,
      name: 'gain',
      unit: 'dB',
      minValue: 0,
      maxValue: 1,
      defaultValue: 0,
      rtSafe: true,
      defaultCurve: 0,
    });
    // An out-of-range breakpoint curve is rejected, not clamped.
    // @ts-expect-error deliberately out-of-range curve ordinal; the addon must reject it.
    expect(() => engine.setAutomationLane(9, [{ ppq: 0, value: 0.5, curveToNext: 99 }])).toThrow();
    // An out-of-range default curve is rejected on registration.
    expect(() =>
      engine.addParameter({
        id: 10,
        name: 'pan',
        unit: '',
        minValue: -1,
        maxValue: 1,
        defaultValue: 0,
        rtSafe: true,
        // @ts-expect-error deliberately out-of-range curve ordinal; the addon must reject it.
        defaultCurve: 99,
      }),
    ).toThrow();
  });

  it('rejects a malformed parameter object without registering a bogus parameter', () => {
    const engine = new RealtimeEngine(48000, 128);
    const before = engine.parameterCount();
    // A non-number id must be rejected outright (previously it coerced to 0 and
    // still registered a zero-value parameter).
    expect(() =>
      // biome-ignore lint/suspicious/noExplicitAny: intentionally malformed input
      engine.addParameter({ name: 'bad', unit: '', defaultCurve: 0 } as any),
    ).toThrow();
    // A wrong-typed numeric field must also be rejected, not silently defaulted.
    expect(() =>
      engine.addParameter({
        id: 42,
        name: 'gain',
        unit: 'dB',
        // biome-ignore lint/suspicious/noExplicitAny: intentionally malformed input
        minValue: 'loud' as any,
        maxValue: 1,
        defaultValue: 0,
        rtSafe: true,
        defaultCurve: 0,
      }),
    ).toThrow();
    // Neither malformed call may have leaked a registration.
    expect(engine.parameterCount()).toBe(before);
  });

  it('processes a block and drains telemetry', () => {
    const engine = new RealtimeEngine(48000, 128);
    engine.setTempo(60);
    engine.setTimeSignature(3, 4);
    engine.setMarkers([
      { id: 11, ppq: 1, name: 'intro' },
      { id: 12, ppq: 2, name: 'out' },
    ]);
    expect(engine.markerCount()).toBe(2);
    expect(engine.markerByIndex(0).name).toBe('intro');
    expect(engine.marker(12).ppq).toBe(2);
    engine.setLoopFromMarkers(11, 12);
    engine.setMetronome({ enabled: true, beatGain: 0.25, accentGain: 0.75, clickSamples: 16 });
    expect(engine.metronome().enabled).toBe(true);
    expect(engine.metronome().clickSamples).toBe(16);
    engine.setMetronome({ enabled: true });
    expect(engine.metronome().clickSamples).toBe(0);
    expect(engine.countInEndSample(0, 2)).toBe(288000);
    engine.setMetronome({ enabled: false });
    engine.addParameter({
      id: 7,
      name: 'gain',
      unit: 'dB',
      minValue: -60,
      maxValue: 12,
      defaultValue: 0,
      rtSafe: true,
      defaultCurve: 0, // canonical AutomationCurve::Linear
    });
    expect(engine.parameterCount()).toBe(1);
    expect(engine.parameterInfo(7).name).toBe('gain');
    expect(engine.parameterInfoByIndex(0).unit).toBe('dB');
    engine.setAutomationLane(7, [
      { ppq: 0, value: 0 },
      { ppq: 1, value: 6.0205999, curveToNext: 0 }, // Linear
    ]);
    expect(engine.automationLaneCount()).toBe(1);
    engine.setGraph({
      nodes: [
        { id: 'in', numPorts: 2 },
        { id: 'gain', type: 1, gainDb: 0, numPorts: 2 },
        { id: 'out', numPorts: 2 },
      ],
      connections: [
        { sourceNode: 'in', sourcePort: 0, destNode: 'gain', destPort: 0 },
        { sourceNode: 'in', sourcePort: 1, destNode: 'gain', destPort: 1 },
        { sourceNode: 'gain', sourcePort: 0, destNode: 'out', destPort: 0 },
        { sourceNode: 'gain', sourcePort: 1, destNode: 'out', destPort: 1 },
      ],
      inputNode: 'in',
      outputNode: 'out',
      numChannels: 2,
      parameterBindings: [{ paramId: 7, nodeId: 'gain' }],
    });
    expect(engine.graphNodeCount()).toBe(3);
    expect(engine.graphConnectionCount()).toBe(4);
    engine.setClips([
      {
        id: 101,
        channels: [new Float32Array(128).fill(0.125), new Float32Array(128).fill(-0.125)],
        startPpq: 1,
        lengthSamples: 128,
      },
    ]);
    expect(engine.clipCount()).toBe(1);
    const captureLeft = new Float32Array(128);
    const captureRight = new Float32Array(128);
    engine.setCaptureBuffer([captureLeft, captureRight]);
    engine.setCapturePunch(48000, 48128);
    engine.armCapture();
    engine.seekMarker(11);
    engine.play();

    const left = new Float32Array(128).fill(0.25);
    const right = new Float32Array(128).fill(-0.25);
    const processed = engine.process([left, right]);

    expect(processed).toHaveLength(2);
    expect(processed[0]).not.toBe(left);
    expect(processed[1]).not.toBe(right);
    expect(processed[0][0]).toBeCloseTo(0.75, 4);
    expect(processed[1][0]).toBeCloseTo(-0.75, 4);
    expect(left[0]).toBeCloseTo(0.25, 4);
    expect(right[0]).toBeCloseTo(-0.25, 4);
    const captureStatus = engine.captureStatus();
    expect(captureStatus.capturedFrames).toBe(128);
    expect(captureStatus.overflowCount).toBe(0);
    expect(captureStatus.armed).toBe(true);
    expect(captureStatus.source).toBe('output');
    expect(captureStatus.recordOffsetSamples).toBe(0);
    const captured = engine.capturedAudio();
    expect(captureLeft[0]).toBe(0);
    expect(captureRight[0]).toBe(0);
    expect(captured[0][0]).toBeCloseTo(0.75, 4);
    expect(captured[1][0]).toBeCloseTo(-0.75, 4);
    engine.resetCapture();
    expect(engine.captureStatus().capturedFrames).toBe(0);

    const telemetry = engine.drainTelemetry();
    expect(telemetry.length).toBeGreaterThan(0);
    expect(telemetry.at(-1)?.type).toBe(0);
    expect(telemetry.at(-1)?.error).toBe(0);
    expect(telemetry.at(-1)?.renderFrame).toBe(0);
    expect(telemetry.at(-1)?.timelineSample).toBe(48000 + 128);

    engine.setCaptureSource('input');
    engine.setRecordOffsetSamples(-37);
    engine.armCapture();
    engine.seekMarker(11);
    engine.process([left, right]);
    const inputCaptureStatus = engine.captureStatus();
    expect(inputCaptureStatus.source).toBe('input');
    expect(inputCaptureStatus.recordOffsetSamples).toBe(-37);
    expect(captureLeft[0]).toBe(0);
    expect(engine.capturedAudio()[0][0]).toBeCloseTo(0.25, 4);
    expect(engine.drainMeterTelemetry().some((record) => record.targetId === 0xffff)).toBe(true);

    engine.setCaptureSource(0);
    expect(engine.captureStatus().source).toBe('output');
    expect(captureRight[0]).toBe(0);
    expect(engine.capturedAudio()[1][0]).toBeCloseTo(-0.25, 4);

    engine.setInputMonitor(false);
    engine.resetCapture();
    engine.armCapture();
    engine.seekMarker(11);
    let monitored = engine.process([
      new Float32Array(128).fill(0.25),
      new Float32Array(128).fill(-0.25),
    ]);
    expect(monitored[0][0]).toBeCloseTo(0.25, 4);
    expect(monitored[1][0]).toBeCloseTo(-0.25, 4);
    expect(captureLeft[0]).toBe(0);
    expect(engine.capturedAudio()[0][0]).toBeCloseTo(0.25, 4);

    engine.setInputMonitor(true, 0.5);
    engine.seekMarker(11);
    monitored = engine.process([
      new Float32Array(128).fill(0.25),
      new Float32Array(128).fill(-0.25),
    ]);
    expect(monitored[0][0]).toBeCloseTo(0.5, 4);
    expect(monitored[1][0]).toBeCloseTo(-0.5, 4);
    let badMonitorGainError: unknown;
    try {
      engine.setInputMonitor(true, Number.NaN);
    } catch (error) {
      badMonitorGainError = error;
    }
    expect(isSonareError(badMonitorGainError)).toBe(true);
    if (!isSonareError(badMonitorGainError)) {
      throw new Error('expected SonareError');
    }
    expect(badMonitorGainError.code).toBe(ErrorCode.InvalidParameter);

    engine.destroy();
  });

  it('configures and drains scope telemetry', () => {
    const sampleRate = 48000;
    const blockSize = 256;
    const engine = new RealtimeEngine(sampleRate, blockSize);
    expect(engine.configureScopeTelemetry(256, 32)).toBe(32);

    const toneFreq = 1000;
    const frames = blockSize * 16;
    const left = new Float32Array(frames);
    const right = new Float32Array(frames);
    for (let i = 0; i < frames; i++) {
      const sample = 0.5 * Math.sin((2 * Math.PI * toneFreq * i) / sampleRate);
      left[i] = sample;
      right[i] = sample;
    }
    engine.setClips([
      {
        id: 1,
        trackId: 10,
        channels: [left, right],
        startPpq: 0,
        lengthSamples: frames,
      },
    ]);
    engine.setTrackLanes([10]);
    engine.play();

    let records: ReturnType<typeof engine.drainScopeTelemetry> = [];
    for (let block = 0; block < 12; block++) {
      engine.process([new Float32Array(blockSize), new Float32Array(blockSize)]);
      records = records.concat(engine.drainScopeTelemetry());
    }

    const master = records.find((record) => record.targetId === 0);
    expect(master).toBeDefined();
    if (!master) {
      throw new Error('expected a master scope record');
    }
    expect(master.bands).toHaveLength(32);
    const argmax = master.bands.reduce(
      (best, value, index) => (value > master.bands[best] ? index : best),
      0,
    );
    expect(argmax).toBeLessThanOrEqual(2);
    expect(master.points.length).toBeGreaterThan(0);

    engine.destroy();
  });

  it('round-trips marker kind and key signature', () => {
    const engine = new RealtimeEngine(48000, 128);
    engine.setMarkers([
      { id: 1, ppq: 0, name: 'intro' },
      {
        id: 2,
        ppq: 4,
        name: 'G major',
        kind: MarkerKind.KeySignature,
        keyFifths: 1,
        keyMinor: false,
      },
      {
        id: 3,
        ppq: 8,
        name: 'A minor',
        kind: MarkerKind.KeySignature,
        keyFifths: 0,
        keyMinor: true,
      },
    ]);
    // A marker with no kind defaults to MarkerKind.Marker with neutral key fields.
    const intro = engine.markerByIndex(0);
    expect(intro.kind).toBe(MarkerKind.Marker);
    expect(intro.keyFifths).toBe(0);
    expect(intro.keyMinor).toBe(false);

    const gMajor = engine.marker(2);
    expect(gMajor.kind).toBe(MarkerKind.KeySignature);
    expect(gMajor.keyFifths).toBe(1);
    expect(gMajor.keyMinor).toBe(false);

    const aMinor = engine.markerByIndex(2);
    expect(aMinor.kind).toBe(MarkerKind.KeySignature);
    expect(aMinor.keyFifths).toBe(0);
    expect(aMinor.keyMinor).toBe(true);
    engine.destroy();
  });

  it('rejects clips without channels or a page provider without aborting', () => {
    const engine = new RealtimeEngine(48000, 128);
    expect(() => engine.setClips([{ id: 1, startPpq: 0 }])).toThrow(
      /clip requires non-empty channels or a pageProvider/,
    );
    expect(() => engine.setClips([{ id: 2, startPpq: 0, channels: [] }])).toThrow(
      /clip requires non-empty channels or a pageProvider/,
    );
    engine.destroy();
  });

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

    engine.setParameterSmoothed(0x4d580001, -12, -1);
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
    expect(() => engine.setTrackLanes([{ trackId: 10, sourceChannelLayout: 2 }])).not.toThrow();

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

  it('streams paged clip providers and drains page requests', () => {
    const engine = new RealtimeEngine(48000, 8);
    const provider = engine.createClipPageProvider(1, 8, 4);
    provider.supply(0, [new Float32Array([1, 2, 3, 4])]);

    engine.setClips([
      {
        id: 123,
        pageProvider: provider,
        startPpq: 0,
      },
    ]);
    engine.play();
    const first = engine.process([new Float32Array(8)]);
    expect(Array.from(first[0])).toEqual([1, 2, 3, 4, 0, 0, 0, 0]);

    const request = engine.popClipPageRequest();
    expect(request).toEqual({ clipId: 123, channel: 0, sample: 4 });
    expect(
      engine.drainTelemetry().some((record) => record.type === 1 && record.value === 123),
    ).toBe(true);

    provider.supply(1, [new Float32Array([5, 6, 7, 8])]);
    engine.seekSample(0);
    const second = engine.process([new Float32Array(8)]);
    expect(Array.from(second[0])).toEqual([1, 2, 3, 4, 5, 6, 7, 8]);

    provider.destroy();
    engine.destroy();
  });

  it('rejects paged providers whose metadata exceeds shared limits', () => {
    const engine = new RealtimeEngine(48000, 8);
    expect(() => engine.createClipPageProvider(1, 1_000_000_000_000, 1)).toThrow();
    expect(() => engine.createClipPageProvider(65, 8, 4)).toThrow();
    engine.destroy();
  });

  it('feeds paged clips from raw float32 files', () => {
    const tmpDir = mkdtempSync(join(tmpdir(), 'sonare-paged-'));
    const rawPath = join(tmpDir, 'clip.f32');
    try {
      const interleaved = new Float32Array([1, 2, 3, 4, 5, 6, 7, 8]);
      writeFileSync(rawPath, Buffer.from(interleaved.buffer));

      const engine = new RealtimeEngine(48000, 8);
      const provider = engine.createFileClipPageProvider(rawPath, {
        numChannels: 1,
        numSamples: 8,
        pageFrames: 4,
      });
      provider.supplyPage(0);
      engine.setClips([{ id: 124, pageProvider: provider, startPpq: 0 }]);
      engine.play();
      const first = engine.process([new Float32Array(8)]);
      expect(Array.from(first[0])).toEqual([1, 2, 3, 4, 0, 0, 0, 0]);

      const request = engine.popClipPageRequest();
      expect(request).toEqual({ clipId: 124, channel: 0, sample: 4 });
      expect(request && provider.supplyRequest(request)).toBe(true);
      engine.seekSample(0);
      const second = engine.process([new Float32Array(8)]);
      expect(Array.from(second[0])).toEqual([1, 2, 3, 4, 5, 6, 7, 8]);

      provider.destroy();
      engine.destroy();
    } finally {
      rmSync(tmpDir, { recursive: true, force: true });
    }
  });

  it('reuses destroyed clip page provider slots', () => {
    const engine = new RealtimeEngine(48000, 8);
    expect(() =>
      engine.createFileClipPageProvider('/definitely/missing/sonare-clip.f32', {
        numChannels: 1,
        numSamples: 8,
        pageFrames: 4,
      }),
    ).toThrow();
    const afterFailedOpen = engine.createClipPageProvider(1, 8, 4);
    expect(afterFailedOpen.id).toBe(1);
    afterFailedOpen.destroy();

    const first = engine.createClipPageProvider(1, 8, 4);
    const second = engine.createClipPageProvider(1, 8, 4);
    expect(first.id).toBe(1);
    expect(second.id).toBe(2);
    first.destroy();
    const reused = engine.createClipPageProvider(1, 8, 4);
    expect(reused.id).toBe(1);
    second.destroy();
    reused.destroy();
    engine.destroy();
  });

  it('renders repitch warped clips from anchors', () => {
    const engine = new RealtimeEngine(48000, 4);
    engine.setClips([
      {
        id: 303,
        channels: [new Float32Array([0, 10, 20, 30])],
        startPpq: 0,
        lengthSamples: 4,
        warpMode: 'repitch',
        warpAnchors: [
          { warpSample: 0, sourceSample: 0 },
          { warpSample: 3, sourceSample: 1.5 },
        ],
      },
    ]);
    engine.play();

    const processed = engine.process([new Float32Array(4)]);
    expect(processed[0][0]).toBeCloseTo(0, 4);
    expect(processed[0][1]).toBeCloseTo(5, 4);
    expect(processed[0][2]).toBeCloseTo(10, 4);
    expect(processed[0][3]).toBeCloseTo(15, 4);
    engine.destroy();

    const badLoopWarpEngine = new RealtimeEngine(48000, 4);
    expect(() =>
      badLoopWarpEngine.setClips([
        {
          id: 3031,
          channels: [new Float32Array([0, 10, 20, 30])],
          startPpq: 0,
          lengthSamples: 8,
          loop: true,
          warpMode: 'repitch',
          warpAnchors: [
            { warpSample: 0, sourceSample: 0 },
            { warpSample: 3, sourceSample: 1.5 },
          ],
        },
      ]),
    ).toThrow();
    badLoopWarpEngine.destroy();

    const badWarpModeEngine = new RealtimeEngine(48000, 4);
    badWarpModeEngine.setClips([
      {
        id: 3032,
        channels: [new Float32Array([0.25, 0.5, 0.75, 1.0])],
        startPpq: 0,
        lengthSamples: 4,
      },
    ]);
    badWarpModeEngine.play();
    expect(() =>
      badWarpModeEngine.setClips([
        {
          id: 3033,
          channels: [new Float32Array([0, 10, 20, 30])],
          startPpq: 0,
          lengthSamples: 4,
        },
        {
          id: 3034,
          channels: [new Float32Array([0, 10, 20, 30])],
          startPpq: 0,
          lengthSamples: 4,
          warpMode: 'typo' as 'repitch',
        },
      ]),
    ).toThrow(/warpMode must be/);
    expect(badWarpModeEngine.clipCount()).toBe(1);
    badWarpModeEngine.seekSample(0);
    const afterBadWarpMode = badWarpModeEngine.process([new Float32Array(4)]);
    expect(Array.from(afterBadWarpMode[0])).toEqual([0.25, 0.5, 0.75, 1.0]);
    badWarpModeEngine.destroy();

    const tempoEngine = new RealtimeEngine(48000, 8192);
    const tempoSource = new Float32Array(4096);
    for (let i = 0; i < tempoSource.length; i++) {
      tempoSource[i] = Math.sin(i * 0.02);
    }
    tempoEngine.setClips([
      {
        id: 304,
        channels: [tempoSource],
        startPpq: 0,
        lengthSamples: 8192,
        warpMode: 'tempo-sync',
        warpAnchors: [
          { warpSample: 0, sourceSample: 0 },
          { warpSample: 2048, sourceSample: 1024 },
          { warpSample: 8192, sourceSample: 4096 },
        ],
      },
    ]);
    tempoEngine.play();
    const tempoSynced = tempoEngine.process([new Float32Array(8192)]);
    expect(tempoSynced[0].some((v) => Math.abs(v) > 0.1)).toBe(true);
    tempoEngine.destroy();
  });

  it('renders time-stretch warped clips at the source pitch', () => {
    const sr = 48000;
    const sourceSamples = 12000;
    const outputSamples = 24000;
    const source = new Float32Array(sourceSamples);
    for (let i = 0; i < sourceSamples; i++) {
      source[i] = 0.5 * Math.sin((2 * Math.PI * 440 * i) / sr);
    }
    const engine = new RealtimeEngine(sr, outputSamples);
    engine.setClips([
      {
        id: 305,
        channels: [source],
        startPpq: 0,
        lengthSamples: outputSamples,
        warpMode: 'time-stretch',
        warpAnchors: [
          { warpSample: 0, sourceSample: 0 },
          { warpSample: outputSamples, sourceSample: sourceSamples },
        ],
      },
    ]);
    engine.play();
    const out = engine.process([new Float32Array(outputSamples)])[0];

    // Goertzel: a resampling warp at half rate would move the tone to 220 Hz.
    const power = (hz: number): number => {
      const w = (2 * Math.PI * hz) / sr;
      const coeff = 2 * Math.cos(w);
      let s1 = 0;
      let s2 = 0;
      for (let i = 4096; i < outputSamples - 4096; i++) {
        const s0 = out[i] + coeff * s1 - s2;
        s2 = s1;
        s1 = s0;
      }
      const real = s1 - s2 * Math.cos(w);
      const imag = s2 * Math.sin(w);
      return real * real + imag * imag;
    };
    expect(power(440)).toBeGreaterThan(100 * power(220));
    engine.destroy();
  });

  it('offline render matches repeated process output', () => {
    const frames = 256;
    const left = new Float32Array(frames);
    const right = new Float32Array(frames);
    for (let i = 0; i < frames; i++) {
      left[i] = Math.sin(i * 0.01);
      right[i] = -left[i];
    }

    const realtime = new RealtimeEngine(48000, 128);
    realtime.play();
    const rtLeft: number[] = [];
    const rtRight: number[] = [];
    for (let offset = 0; offset < frames; offset += 128) {
      const block = realtime.process([
        left.slice(offset, offset + 128),
        right.slice(offset, offset + 128),
      ]);
      rtLeft.push(...block[0]);
      rtRight.push(...block[1]);
    }
    realtime.destroy();

    const offline = new RealtimeEngine(48000, 128);
    offline.play();
    const rendered = offline.renderOffline([left, right], 128);
    offline.destroy();

    expect(Array.from(rendered[0])).toEqual(rtLeft);
    expect(Array.from(rendered[1])).toEqual(rtRight);

    const bounce = new RealtimeEngine(48000, 128);
    bounce.play();
    const bounced = bounce.bounceOffline({
      totalFrames: 256,
      blockSize: 128,
      numChannels: 2,
      sourceSampleRate: 48000,
      targetSampleRate: 24000,
    });
    bounce.destroy();

    expect(bounced.frames).toBe(128);
    expect(bounced.numChannels).toBe(2);
    expect(bounced.sampleRate).toBe(24000);
    expect(bounced.interleaved.length).toBe(256);
    // Silent input integrates to -Infinity LUFS, which is the correct gating
    // result for digital silence; just assert a numeric value is reported.
    expect(typeof bounced.integratedLufs).toBe('number');

    const freeze = new RealtimeEngine(48000, 128);
    freeze.setClips([
      {
        id: 7,
        channels: [new Float32Array(128).fill(0.125), new Float32Array(128).fill(-0.25)],
        startPpq: 0,
        lengthSamples: 128,
      },
    ]);
    freeze.play();
    const frozen = freeze.freezeOffline({
      totalFrames: 128,
      blockSize: 128,
      numChannels: 2,
      clipId: 77,
    });
    expect(frozen.clipId).toBe(77);
    expect(frozen.frames).toBe(128);
    expect(frozen.numChannels).toBe(2);
    expect(freeze.clipCount()).toBe(1);
    freeze.seekSample(0);
    const frozenRendered = freeze.renderOffline(
      [new Float32Array(128), new Float32Array(128)],
      128,
    );
    freeze.destroy();
    expect(frozenRendered[0][0]).toBeCloseTo(0.125, 4);
    expect(frozenRendered[1][0]).toBeCloseTo(-0.25, 4);
  });

  it('bounces clip content, hits the loudness target and applies dither', () => {
    const frames = 256;
    const amplitude = 0.5;
    const tone = new Float32Array(frames);
    for (let i = 0; i < frames; i++) {
      tone[i] = amplitude * Math.sin((2 * Math.PI * 440 * i) / 48000);
    }

    const bounce = (
      options: Partial<EngineBounceOptions> = {},
    ): { samples: Float32Array; lufs: number } => {
      const engine = new RealtimeEngine(48000, 128);
      engine.setClips([
        { id: 1, channels: [tone, tone.slice()], startPpq: 0, lengthSamples: frames },
      ]);
      engine.play();
      const result = engine.bounceOffline({
        totalFrames: frames,
        blockSize: 128,
        numChannels: 2,
        sourceSampleRate: 48000,
        targetSampleRate: 48000,
        ...options,
      });
      engine.destroy();
      return { samples: result.interleaved, lufs: result.integratedLufs };
    };

    const peak = (samples: Float32Array): number =>
      samples.reduce((acc, sample) => Math.max(acc, Math.abs(sample)), 0);
    const maxAbsDiff = (a: Float32Array, b: Float32Array): number =>
      a.reduce((acc, sample, index) => Math.max(acc, Math.abs(sample - b[index])), 0);

    // The scheduled clip must actually reach the bounce: a bounce that renders
    // silence would satisfy every shape assertion while exporting nothing.
    const plain = bounce();
    expect(plain.samples.length).toBe(frames * 2);
    expect(peak(plain.samples)).toBeCloseTo(amplitude, 3);
    expect(plain.samples.some((sample) => sample !== 0)).toBe(true);
    expect(Number.isFinite(plain.lufs)).toBe(true);

    // Requested loudness is reached, and the documented 0 sentinel resolves to
    // the shared -14 LUFS default rather than to a literal 0 LUFS target.
    for (const targetLufs of [-20, -9]) {
      const normalized = bounce({ normalizeLufs: true, targetLufs });
      expect(normalized.lufs).toBeCloseTo(targetLufs, 1);
    }
    expect(bounce({ normalizeLufs: true, targetLufs: 0 }).lufs).toBeCloseTo(-14, 1);

    // Dither types are identified by what they do to the signal rather than by
    // the integer alone, so a remapped type cannot pass: RPDF and TPDF add
    // noise without quantizing (TPDF is the wider of the two, being the sum of
    // two uniform draws), and only the noise-shaped type quantizes onto the
    // target-depth grid.
    const bits = 8;
    const lsb = 1 / (1 << (bits - 1));
    const none = bounce({ dither: 0, ditherBits: bits, ditherSeed: 1 });
    const rpdf = bounce({ dither: 1, ditherBits: bits, ditherSeed: 1 });
    const tpdf = bounce({ dither: 2, ditherBits: bits, ditherSeed: 1 });
    const shaped = bounce({ dither: 3, ditherBits: bits, ditherSeed: 1 });
    const onGrid = (samples: Float32Array): number =>
      samples.reduce(
        (acc, sample) => acc + (Math.abs(sample / lsb - Math.round(sample / lsb)) < 1e-4 ? 1 : 0),
        0,
      );

    expect(maxAbsDiff(none.samples, plain.samples)).toBe(0);
    expect(maxAbsDiff(rpdf.samples, none.samples)).toBeGreaterThan(lsb / 4);
    expect(maxAbsDiff(tpdf.samples, none.samples)).toBeGreaterThan(
      maxAbsDiff(rpdf.samples, none.samples),
    );
    // Every non-None type lands the output on the target-depth grid -- that is
    // what the shared bit-depth field means -- so the type selects only the
    // noise, the triangular one being wider than the rectangular.
    expect(onGrid(rpdf.samples)).toBe(rpdf.samples.length);
    expect(onGrid(tpdf.samples)).toBe(tpdf.samples.length);
    expect(onGrid(shaped.samples)).toBe(shaped.samples.length);

    // A fixed seed is reproducible and a different seed is not, so the seed
    // reaches the generator instead of being dropped.
    const repeat = bounce({ dither: 2, ditherBits: bits, ditherSeed: 1 });
    expect(Array.from(repeat.samples)).toEqual(Array.from(tpdf.samples));
    const otherSeed = bounce({ dither: 2, ditherBits: bits, ditherSeed: 2 });
    expect(maxAbsDiff(otherSeed.samples, tpdf.samples)).toBeGreaterThan(0);
  });

  it('reads captured audio out of the capture buffer', () => {
    const engine = new RealtimeEngine(48000, 128);
    const captureLeft = new Float32Array(128);
    const captureRight = new Float32Array(128);
    engine.setCaptureBuffer([captureLeft, captureRight]);
    engine.armCapture();
    engine.play();

    engine.process([new Float32Array(128).fill(0.5), new Float32Array(128).fill(-0.5)]);
    const status = engine.captureStatus();
    expect(status.capturedFrames).toBe(128);

    const captured = engine.capturedAudio();
    expect(captured).toHaveLength(2);
    expect(captured[0].length).toBe(128);
    expect(captured[1].length).toBe(128);
    expect(captured[0][0]).toBeCloseTo(0.5, 4);
    expect(captured[1][0]).toBeCloseTo(-0.5, 4);
    engine.destroy();
  });

  it('allocates capture storage from the canonical channel/count form', () => {
    const engine = new RealtimeEngine(48000, 128);
    engine.setCaptureBuffer(2, 128);
    engine.armCapture();
    engine.play();

    engine.process([new Float32Array(128).fill(0.25), new Float32Array(128).fill(-0.25)]);
    const captured = engine.capturedAudio();
    expect(captured).toHaveLength(2);
    expect(captured[0].length).toBe(128);
    expect(captured[1].length).toBe(128);
    expect(captured[0][0]).toBeCloseTo(0.25, 4);
    expect(captured[1][0]).toBeCloseTo(-0.25, 4);
    expect(() => engine.setCaptureBuffer(0, 128)).toThrow(RangeError);
    expect(() => engine.setCaptureBuffer(2, 0)).toThrow(RangeError);
    engine.destroy();
  });

  it('keeps capture storage valid when the source ArrayBuffers are transferred', () => {
    const engine = new RealtimeEngine(48000, 128);
    const captureLeft = new Float32Array(128);
    const captureRight = new Float32Array(128);
    engine.setCaptureBuffer([captureLeft, captureRight]);

    structuredClone(captureLeft, { transfer: [captureLeft.buffer] });
    structuredClone(captureRight, { transfer: [captureRight.buffer] });
    expect(captureLeft.byteLength).toBe(0);
    expect(captureRight.byteLength).toBe(0);

    engine.armCapture();
    engine.play();
    engine.process([new Float32Array(128).fill(0.25), new Float32Array(128).fill(-0.25)]);
    const captured = engine.capturedAudio();
    expect(captured[0][0]).toBeCloseTo(0.25, 4);
    expect(captured[1][0]).toBeCloseTo(-0.25, 4);
    engine.destroy();
  });

  it('exposes transport state and live parameter injection', () => {
    const engine = new RealtimeEngine(48000, 128);
    engine.setTempo(60);
    expect(engine.sampleAtPpq(1.5)).toBe(72000);
    let badPpqError: unknown;
    try {
      engine.sampleAtPpq(Number.NaN);
    } catch (error) {
      badPpqError = error;
    }
    expect(isSonareError(badPpqError)).toBe(true);
    if (!isSonareError(badPpqError)) {
      throw new Error('expected SonareError');
    }
    expect(badPpqError.code).toBe(ErrorCode.InvalidParameter);
    expect(() => engine.sampleAtPpq(1e300)).toThrow();
    expect(() => engine.seekPpq(1e300)).toThrow();
    expect(() => engine.setLoop(0, 1e300, true)).toThrow();
    engine.setTempo(90);
    engine.addParameter({
      id: 3,
      name: 'gain',
      unit: 'dB',
      minValue: -60,
      maxValue: 12,
      defaultValue: 0,
      rtSafe: true,
      defaultCurve: 0, // canonical AutomationCurve::Linear
    });
    // Live parameter injection must not throw.
    engine.setParameter(3, 3.0);
    engine.setParameterSmoothed(3, -3.0, -1);

    engine.play();
    engine.process([new Float32Array(128), new Float32Array(128)]);

    const transport = engine.getTransportState();
    expect(transport.playing).toBe(true);
    expect(transport.isPlaying).toBe(true);
    expect(transport.bpm).toBeCloseTo(90, 3);
    expect(transport.sampleRate).toBeCloseTo(48000, 1);
    expect(transport.samplePosition).toBeGreaterThanOrEqual(128);
    expect(Number.isFinite(transport.ppq)).toBe(true);
    // Musical beat readout: `beat` is one-based, `beatFraction` in [0, 1).
    expect(Number.isInteger(transport.beat)).toBe(true);
    expect(transport.beat).toBeGreaterThanOrEqual(1);
    expect(transport.beatFraction).toBeGreaterThanOrEqual(0);
    expect(transport.beatFraction).toBeLessThan(1);

    const meterRecords = engine.drainMeterTelemetry();
    expect(Array.isArray(meterRecords)).toBe(true);
    for (const record of meterRecords) {
      expect(typeof record.peakDbL).toBe('number');
      expect(typeof record.peakDbR).toBe('number');
      expect(typeof record.rmsDbL).toBe('number');
      expect(typeof record.rmsDbR).toBe('number');
      expect(Number.isFinite(record.integratedLufs)).toBe(true);
    }
    engine.destroy();
  });

  it('renders scheduled MIDI clips through built-in instruments', () => {
    const engine = new RealtimeEngine(48000, 128);
    engine.setBuiltinInstrument({ gain: 0.5 }, 5);
    engine.setMidiClips([
      {
        id: 1,
        trackId: 5,
        destinationId: 5,
        lengthSamples: 8192,
        events: [
          { renderFrame: 0, word0: midi1Word(0x9, 0, 60, 100), wordCount: 1 },
          { renderFrame: 4096, word0: midi1Word(0x8, 0, 60, 0), wordCount: 1 },
        ],
      },
    ]);
    engine.play();
    const out = engine.process([new Float32Array(128), new Float32Array(128)]);
    expect(Math.max(rms(out[0]), rms(out[1]))).toBeGreaterThan(0);

    let badGroupError: unknown;
    try {
      engine.setMidiClips([
        {
          id: 2,
          trackId: 5,
          destinationId: 5,
          events: [{ renderFrame: 0, word0: midi1Word(0x9, 0, 60, 100), wordCount: 1, group: 16 }],
        },
      ]);
    } catch (error) {
      badGroupError = error;
    }
    expect(isSonareError(badGroupError)).toBe(true);
    if (!isSonareError(badGroupError)) {
      throw new Error('expected SonareError');
    }
    expect(badGroupError.code).toBe(ErrorCode.InvalidParameter);

    let badChannelError: unknown;
    try {
      engine.pushMidiNoteOn(5, 0, 16, 60, 100);
    } catch (error) {
      badChannelError = error;
    }
    expect(isSonareError(badChannelError)).toBe(true);
    if (!isSonareError(badChannelError)) {
      throw new Error('expected SonareError');
    }
    expect(badChannelError.code).toBe(ErrorCode.InvalidParameter);

    let badSoundFontError: unknown;
    try {
      engine.loadSoundFont(new Uint8Array([0x6e, 0x6f, 0x74, 0x20, 0x73, 0x66, 0x32]));
    } catch (error) {
      badSoundFontError = error;
    }
    expect(isSonareError(badSoundFontError)).toBe(true);
    if (!isSonareError(badSoundFontError)) {
      throw new Error('expected SonareError');
    }
    expect(badSoundFontError.code).toBe(ErrorCode.InvalidFormat);

    engine.setMidiClips([]);
    engine.destroy();
  });

  it('rejects MIDI byte arguments that would wrap through the uint8 cast', () => {
    const engine = new RealtimeEngine(48000, 128);
    engine.setMidiInputSource(0);
    // group 256 must NOT silently wrap to a valid group 0 — it must throw.
    expect(() => engine.pushMidiInputNoteOn(256, 0, 60, 100)).toThrow();
    // A channel of 256 likewise wraps to 0 under a plain cast; reject it.
    expect(() => engine.pushMidiInputNoteOn(0, 256, 60, 100)).toThrow();
    // A valid in-range call still works once an input source is enabled.
    expect(() => engine.pushMidiInputNoteOn(0, 0, 60, 100)).not.toThrow();
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

  it('drains external MIDI routing to the host', () => {
    const engine = new RealtimeEngine(48000, 128);
    // Route destination 5 to the external queue instead of an instrument.
    engine.setMidiDestinationExternal(5, true);
    engine.setMidiClips([
      {
        id: 7,
        trackId: 5,
        destinationId: 5,
        lengthSamples: 256,
        events: [
          { renderFrame: 0, word0: midi1Word(0x9, 1, 64, 110), wordCount: 1 },
          { renderFrame: 48, word0: midi1Word(0x8, 1, 64, 0), wordCount: 1 },
        ],
      },
    ]);
    engine.play();
    engine.process([new Float32Array(128), new Float32Array(128)]);

    const drained = engine.drainExternalMidi(16);
    expect(drained.length).toBe(2);
    expect(drained[0].destinationId).toBe(5);
    expect(drained[0].bytes.length).toBe(3);
    expect(drained[0].bytes[0] & 0xf0).toBe(0x90); // note on
    expect(drained[0].renderFrame).toBe(0);
    expect(drained[1].destinationId).toBe(5);
    expect(drained[1].bytes[0] & 0xf0).toBe(0x80); // note off
    expect(drained[1].renderFrame).toBe(48);

    expect(engine.externalMidiDroppedCount()).toBe(0);
    engine.destroy();
  });

  it('reports external-destination table overflow instead of silently routing internally', () => {
    const engine = new RealtimeEngine(48000, 128);
    for (let id = 0; id < 16; id++) {
      engine.setMidiDestinationExternal(id, true);
    }
    // Re-marking an existing destination is idempotent.
    expect(() => engine.setMidiDestinationExternal(3, true)).not.toThrow();
    // The 17th distinct destination overflows the slot table.
    expect(() => engine.setMidiDestinationExternal(99, true)).toThrow();
    // Freeing a slot lets the next mark succeed.
    engine.setMidiDestinationExternal(3, false);
    expect(() => engine.setMidiDestinationExternal(99, true)).not.toThrow();
    engine.destroy();
  });

  it('accepts a custom smoothed-parameter ramp time and rejects bad values', () => {
    const engine = new RealtimeEngine(48000, 128);
    expect(() => engine.setParamSmoothingMs(0)).not.toThrow();
    expect(() => engine.setParamSmoothingMs(75)).not.toThrow();
    expect(() => engine.setParamSmoothingMs(-1)).toThrow();
    expect(() => engine.setParamSmoothingMs(Number.NaN)).toThrow();
    engine.destroy();
  });

  it('drains external MIDI losslessly when maxRecords is below the queued count', () => {
    // Regression for the over-drain data-loss bug: a small maxRecords cap must
    // not discard events the native call already dequeued. Queue many events,
    // drain in batches with a cap that is neither a multiple of the internal
    // 256-slot buffer nor large enough to take everything at once, and assert
    // every event survives across repeated calls.
    const engine = new RealtimeEngine(48000, 128);
    engine.setMidiDestinationExternal(5, true);
    const events = [];
    const kNotes = 24; // 48 events total (on+off), well above the cap below
    for (let i = 0; i < kNotes; i++) {
      events.push({ renderFrame: i, word0: midi1Word(0x9, 1, 40 + i, 100), wordCount: 1 });
      events.push({ renderFrame: i, word0: midi1Word(0x8, 1, 40 + i, 0), wordCount: 1 });
    }
    engine.setMidiClips([{ id: 7, trackId: 5, destinationId: 5, lengthSamples: 256, events }]);
    engine.play();
    engine.process([new Float32Array(128), new Float32Array(128)]);

    const collected = [];
    for (let guard = 0; guard < 1000; guard++) {
      const batch = engine.drainExternalMidi(5); // cap < count, not a 256 multiple
      if (batch.length === 0) {
        break;
      }
      expect(batch.length).toBeLessThanOrEqual(5);
      for (const ev of batch) {
        collected.push(ev);
      }
    }

    expect(collected.length).toBe(2 * kNotes);
    expect(engine.externalMidiDroppedCount()).toBe(0);
    // Losslessness: every queued note-on and note-off survives the capped drain.
    const noteOns = new Set();
    const noteOffs = new Set();
    for (const ev of collected) {
      const status = ev.bytes[0] & 0xf0;
      if (status === 0x90) {
        noteOns.add(ev.bytes[1]);
      } else if (status === 0x80) {
        noteOffs.add(ev.bytes[1]);
      }
    }
    for (let i = 0; i < kNotes; i++) {
      expect(noteOns.has(40 + i)).toBe(true);
      expect(noteOffs.has(40 + i)).toBe(true);
    }
    engine.destroy();
  });

  it('forwards MIDI clock/transport to the external queue', () => {
    const engine = new RealtimeEngine(48000, 24000);
    engine.setTempo(120);
    engine.setExternalMidiClockEnabled(true);
    engine.play(0);

    engine.process([new Float32Array(24000), new Float32Array(24000)]);

    // One Start plus 24 clock ticks (one every 1000 samples at 120 BPM).
    const drained = engine.drainExternalMidi(64);
    expect(drained.length).toBe(25);
    for (const event of drained) {
      expect(event.destinationId).toBe(0xffffffff);
      expect(event.bytes.length).toBe(1);
    }
    expect(drained[0].bytes[0]).toBe(0xfa); // Start
    expect(drained[1].bytes[0]).toBe(0xf8); // Clock
    engine.destroy();
  });
});
