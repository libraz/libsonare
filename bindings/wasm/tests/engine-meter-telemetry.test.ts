import { beforeAll, expect, it } from 'vitest';
import { init, RealtimeEngine } from '../dist/index.js';

beforeAll(async () => {
  await init();
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
      expect(last?.inputPeakDb[0]).toBeGreaterThan(-10);
      expect(last?.inputPeakDb[1] ?? Number.NEGATIVE_INFINITY).toBe(Number.NEGATIVE_INFINITY);
      expect(last?.gainReductionDb).toBeLessThan(-10);
      expect(last?.peakDb[0]).toBeLessThan((last?.inputPeakDb[0] ?? Number.NEGATIVE_INFINITY) - 10);
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
    expect(after?.inputPeakDb[0]).toBeGreaterThan(-12);
  } finally {
    engine.destroy();
  }
});
