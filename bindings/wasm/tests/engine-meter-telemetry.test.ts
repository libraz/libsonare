import { beforeAll, expect, it } from 'vitest';
import { init, RealtimeEngine } from '../dist/index.js';
import { meterTelemetryFromNative } from '../src/internal/engine_native.js';
import type { WasmEngineMeterTelemetry } from '../src/sonare.js';

beforeAll(async () => {
  await init();
});

it('maps unmeasured plane values to undefined and trims mono padding', () => {
  const record: WasmEngineMeterTelemetry = {
    targetId: 1,
    renderFrame: 0,
    seq: 1,
    channelCount: 1,
    peakDb: [Number.NaN, -6],
    rmsDb: [Number.NaN, -9],
    truePeakDb: [Number.NaN, -5],
    inputPeakDb: [Number.NaN, -3],
    gainReductionDb: 0,
    droppedRecords: 0,
  };
  const meter = meterTelemetryFromNative(record);
  expect(meter.numChannels).toBe(1);
  expect(meter.peakDb).toEqual([undefined]);
  expect(meter.rmsDb).toEqual([undefined]);
  expect(meter.truePeakDbtp).toEqual([undefined]);
  expect(meter.inputPeakDb).toEqual([undefined]);
});

it.each(['lane', 'bus', 'master'] as const)(
  '%s telemetry exposes pre-trim input peaks and audible gain reduction',
  (target) => {
    const engine = new RealtimeEngine({ sampleRate: 48000, maxBlockSize: 256 });
    try {
      engine.setTrackBuses([{ busId: 1, channelLayout: 'mono' }]);
      engine.setTrackLanes([{ trackId: 10, outputBusId: 1 }]);
      const strip = {
        id: 'measured',
        inputTrimDb: -6,
        inserts: [
          {
            processor: 'dynamics.compressor',
            params: { thresholdDb: -30, ratio: 10, attackMs: 0.1, releaseMs: 100 },
          },
        ],
      };
      const scene = JSON.stringify({
        version: 2,
        strips: target === 'bus' ? [] : [strip],
        buses: target === 'bus' ? [strip] : [],
        connections: [],
      });
      if (target === 'lane') {
        engine.setStrip(10, JSON.parse(scene).strips[0]);
      }
      if (target === 'bus') {
        engine.setBusStrip(1, JSON.parse(scene).buses[0]);
      }
      if (target === 'master') {
        engine.setStrip('master', JSON.parse(scene).strips[0]);
      }
      engine.setClips([
        {
          id: 1,
          trackId: 10,
          channels: [new Float32Array(256 * 24).fill(0.8)],
          startPpq: 0,
          lengthSamples: 256 * 24,
        },
      ]);
      engine.play();
      const targetId = target === 'lane' ? 1 : target === 'bus' ? 33 : 0;
      let last: ReturnType<RealtimeEngine['drainMeterTelemetry']>[number] | undefined;
      for (let block = 0; block < 20; ++block) {
        engine.process([new Float32Array(256)]);
        last = engine.drainMeterTelemetry().find((record) => record.targetId === targetId) ?? last;
      }
      expect(last).toBeDefined();
      expect(last?.numChannels).toBe(1);
      for (const plane of [last?.peakDb, last?.rmsDb, last?.truePeakDbtp, last?.inputPeakDb]) {
        expect(plane).toHaveLength(1);
        expect(plane?.some(Number.isNaN)).toBe(false);
        expect(plane).not.toContain(-120);
      }
      const inputPeak = last?.inputPeakDb[0] ?? Number.NEGATIVE_INFINITY;
      const peak = last?.peakDb[0] ?? Number.NEGATIVE_INFINITY;
      expect(inputPeak).toBeGreaterThan(-10);
      expect(last?.inputPeakDb[1]).toBeUndefined();
      expect(last?.gainReductionDb).toBeLessThan(-10);
      expect(peak).toBeLessThan(inputPeak - 10);
    } finally {
      engine.destroy();
    }
  },
);

it('resets master integrated loudness while retaining the other meter windows', () => {
  const blockSize = 1024;
  const engine = new RealtimeEngine({ sampleRate: 48000, maxBlockSize: blockSize });
  try {
    const source = Float32Array.from(
      { length: blockSize * 170 },
      (_, index) => 0.5 * Math.sin((2 * Math.PI * 1000 * index) / 48000),
    );
    engine.setClips([
      { id: 1, trackId: 10, channels: [source], startPpq: 0, lengthSamples: source.length },
    ]);
    engine.play();
    let before: ReturnType<RealtimeEngine['drainMeterTelemetry']>[number] | undefined;
    for (let block = 0; block < 160; ++block) {
      engine.process([new Float32Array(blockSize)]);
      before = engine.drainMeterTelemetry().find((record) => record.targetId === 0) ?? before;
    }
    expect(before?.integratedLufs).toBeGreaterThan(-40);
    engine.resetMasterLoudnessMeter();
    engine.process([new Float32Array(blockSize)]);
    const after = engine.drainMeterTelemetry().find((record) => record.targetId === 0);
    // No gating block has completed since the reset: the key is absent.
    expect(after).toBeDefined();
    expect('integratedLufs' in (after ?? {})).toBe(false);
    expect(after?.momentaryLufs).toBeGreaterThan(-40);
    expect(after?.shortTermLufs).toBeGreaterThan(-40);
    expect(after?.maxTruePeakDbtp).toBeGreaterThan(-12);
    expect(after?.inputPeakDb[0] ?? Number.NEGATIVE_INFINITY).toBeGreaterThan(-12);
  } finally {
    engine.destroy();
  }
});
