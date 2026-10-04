import { RealtimeEngine } from '../dist/index.js';
import type { SonareWorkletMeterSnapshot } from '../src/worklet/protocol';
import {
  createSonareMeterRingBuffer,
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
      for (let block = 0; block < 20; ++block) {
        engine.process([new Float32Array(BLOCK)]);
        // Query beside the drain so the values and the record describe the same block.
        const record = engine.drainMeterTelemetry().find((r) => r.targetId === LANE_TARGET);
        const entries = engine.meterTargetInsertGainReduction(LANE_TARGET);
        if (!record || block < 19) {
          continue;
        }
        expect(entries).toHaveLength(2);
        for (const entry of entries) {
          expect(entry).toBeLessThanOrEqual(0);
        }
        expect(Math.min(...entries)).toBeLessThan(-1);
        const scale = Math.max(1, Math.abs(record.gainReductionDb));
        expect(Math.abs(Math.min(...entries) - record.gainReductionDb)).toBeLessThan(1e-3 * scale);
      }
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
      const entries = lane?.insertGainReductionDb ?? [];
      const scale = Math.max(1, Math.abs(lane?.gainReductionDb ?? 0));
      expect(Math.abs(Math.min(...entries) - (lane?.gainReductionDb ?? 0))).toBeLessThan(
        1e-3 * scale,
      );
    } finally {
      processor.destroy();
    }
  });

  it('attaches the field only to the newest record per target in one drain', () => {
    const meters: SonareWorkletMeterSnapshot[] = [];
    const processor = new SonareRealtimeEngineWorkletProcessor(
      { sampleRate: 48000, blockSize: BLOCK, channelCount: 2, meterIntervalFrames: 1 },
      { onMeter: (meter) => meters.push(meter), postMessage: () => undefined },
    );
    try {
      const engine = (processor as unknown as { engine: RealtimeEngine }).engine;
      seedCompressorLimiterLane(engine);
      // Back up several records, then drain them in one publish.
      for (let block = 0; block < 4; ++block) {
        engine.process([new Float32Array(BLOCK), new Float32Array(BLOCK)]);
      }
      (processor as unknown as { publishMeters(): void }).publishMeters();
      const lane = meters.filter((m) => m.targetId === LANE_TARGET);
      expect(lane.length).toBeGreaterThan(1);
      expect(lane.at(-1)?.insertGainReductionDb).toHaveLength(2);
      for (const older of lane.slice(0, -1)) {
        expect(older.insertGainReductionDb).toBeUndefined();
      }
    } finally {
      processor.destroy();
    }
  });

  it('answers insertGainReductionRequest in shared-ring meter mode', () => {
    const ring = createSonareMeterRingBuffer(64);
    const posted: unknown[] = [];
    const processor = new SonareRealtimeEngineWorkletProcessor(
      {
        sampleRate: 48000,
        blockSize: BLOCK,
        channelCount: 2,
        meterIntervalFrames: BLOCK,
        meterSharedBuffer: ring.sharedBuffer,
      },
      { postMessage: (message) => posted.push(message) },
    );
    try {
      const engine = (processor as unknown as { engine: RealtimeEngine }).engine;
      seedCompressorLimiterLane(engine);
      for (let block = 0; block < 20; ++block) {
        processor.process([[]], [[new Float32Array(BLOCK), new Float32Array(BLOCK)]]);
      }
      processor.receiveInsertGainReductionRequest({
        type: 'insertGainReductionRequest',
        requestId: 5,
        targetId: LANE_TARGET,
      });
      const ok = posted.at(-1) as {
        type: string;
        requestId: number;
        ok: boolean;
        values: number[];
      };
      expect(ok).toMatchObject({ type: 'insertGainReductionResponse', requestId: 5, ok: true });
      expect(ok.values).toHaveLength(2);
      expect(Math.min(...ok.values)).toBeLessThan(-1);

      processor.receiveInsertGainReductionRequest({
        type: 'insertGainReductionRequest',
        requestId: 6,
        targetId: 41,
      });
      expect(posted.at(-1)).toMatchObject({
        type: 'insertGainReductionResponse',
        requestId: 6,
        ok: false,
      });
      expect((posted.at(-1) as { error?: string }).error).toEqual(expect.any(String));
    } finally {
      processor.destroy();
    }
  });
});
