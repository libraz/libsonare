import { RealtimeEngine } from '../dist/index.js';
import type { SonareWorkletMeterSnapshot } from '../src/worklet/protocol';
import {
  describe,
  expect,
  it,
  SonareRealtimeEngineWorkletProcessor,
  setupWorklet,
} from './_worklet_helpers';

const BLOCK = 256;
const LANE_TARGET = 1;

type EngineLike = Pick<
  RealtimeEngine,
  'setTrackBuses' | 'setTrackLanes' | 'setTrackStripJson' | 'setClips' | 'play'
>;

function seedCompressorLimiterLane(engine: EngineLike): void {
  engine.setTrackBuses([{ busId: 1, channelLayout: 0 }]);
  engine.setTrackLanes([{ trackId: 10, outputBusId: 1 }]);
  engine.setTrackStripJson(
    10,
    JSON.stringify({
      version: 1,
      strips: [
        {
          id: 'rack',
          inserts: [
            {
              processor: 'dynamics.compressor',
              params: { thresholdDb: -30, ratio: 10, attackMs: 0.1, releaseMs: 100 },
            },
            { processor: 'dynamics.limiter', params: { thresholdDb: -20 } },
          ],
        },
      ],
      buses: [],
      connections: [],
    }),
  );
  engine.setClips([
    {
      id: 1,
      trackId: 10,
      channels: [new Float32Array(BLOCK * 24).fill(0.8)],
      startPpq: 0,
      lengthSamples: BLOCK * 24,
    },
  ]);
  engine.play();
}

describe('insert gain reduction query', () => {
  setupWorklet();

  it('returns per-insert entries whose minimum tracks the strip record', () => {
    const engine = new RealtimeEngine(48000, BLOCK);
    try {
      seedCompressorLimiterLane(engine);
      let record: ReturnType<RealtimeEngine['drainMeterTelemetry']>[number] | undefined;
      for (let block = 0; block < 20; ++block) {
        engine.process([new Float32Array(BLOCK)]);
        record = engine.drainMeterTelemetry().find((r) => r.targetId === LANE_TARGET) ?? record;
      }
      const entries = engine.meterTargetInsertGainReduction(LANE_TARGET);
      expect(entries).toHaveLength(2);
      for (const entry of entries) {
        expect(entry).toBeLessThanOrEqual(0);
      }
      expect(record).toBeDefined();
      expect(Math.min(...entries)).toBeLessThan(-1);
      expect(Math.min(...entries)).toBeCloseTo(record?.gainReductionDb ?? 0, 0);
    } finally {
      engine.destroy();
    }
  });

  it('returns an empty array for the monitor target and throws out of range', () => {
    const engine = new RealtimeEngine(48000, BLOCK);
    try {
      expect(engine.meterTargetInsertGainReduction(0xffff)).toEqual([]);
      expect(() => engine.meterTargetInsertGainReduction(41)).toThrow();
    } finally {
      engine.destroy();
    }
  });

  it('attaches insertGainReductionDb to postMessage meter snapshots', () => {
    const meters: SonareWorkletMeterSnapshot[] = [];
    const processor = new SonareRealtimeEngineWorkletProcessor(
      { sampleRate: 48000, blockSize: BLOCK, channelCount: 2, meterIntervalFrames: BLOCK },
      { onMeter: (meter) => meters.push(meter), postMessage: () => undefined },
    );
    try {
      const engine = (processor as unknown as { engine: RealtimeEngine }).engine;
      seedCompressorLimiterLane(engine);
      for (let block = 0; block < 20; ++block) {
        processor.process([[]], [[new Float32Array(BLOCK), new Float32Array(BLOCK)]]);
      }
      const lane = meters.filter((m) => m.targetId === LANE_TARGET).at(-1);
      expect(lane).toBeDefined();
      expect(lane?.insertGainReductionDb).toHaveLength(2);
      expect(lane?.insertGainReductionDb).toEqual(
        engine.meterTargetInsertGainReduction(LANE_TARGET),
      );
      for (const meter of meters) {
        expect(Array.isArray(meter.insertGainReductionDb)).toBe(true);
      }
    } finally {
      processor.destroy();
    }
  });
});
