import { beforeAll, expect, it } from 'vitest';
import { init, RealtimeEngine } from '../dist/index.js';

beforeAll(async () => {
  await init();
});

it.each(['lane', 'bus', 'master'] as const)(
  '%s telemetry exposes pre-trim input peaks and audible gain reduction',
  (target) => {
    const engine = new RealtimeEngine(48000, 256);
    try {
      engine.setTrackBuses([{ busId: 1, channelLayout: 0 }]);
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
        version: 1,
        strips: target === 'bus' ? [] : [strip],
        buses: target === 'bus' ? [strip] : [],
        connections: [],
      });
      if (target === 'lane') {
        engine.setTrackStripJson(10, scene);
      }
      if (target === 'bus') {
        engine.setBusStripJson(1, scene);
      }
      if (target === 'master') {
        engine.setMasterStripJson(scene);
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
      expect(last?.inputPeakDbL).toBeGreaterThan(-10);
      expect(last?.inputPeakDbR).toBe(-120);
      expect(last?.gainReductionDb).toBeLessThan(-10);
      expect(last?.peakDbL).toBeLessThan((last?.inputPeakDbL ?? -120) - 10);
    } finally {
      engine.destroy();
    }
  },
);

it('resets master integrated loudness while retaining the other meter windows', () => {
  const blockSize = 1024;
  const engine = new RealtimeEngine(48000, blockSize);
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
    expect(after?.integratedLufs).toBe(-120);
    expect(after?.momentaryLufs).toBeGreaterThan(-40);
    expect(after?.shortTermLufs).toBeGreaterThan(-40);
    expect(after?.maxTruePeakDb).toBeGreaterThan(-12);
    expect(after?.inputPeakDbL).toBeGreaterThan(-12);
  } finally {
    engine.destroy();
  }
});
