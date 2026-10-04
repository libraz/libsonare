import { describe, expect, it } from 'vitest';
import { RealtimeEngine } from '../src/index.js';

const stripJson =
  '{"version":1,"strips":[{"id":"track-10","inserts":[' +
  '{"slot":"pre","processor":"eq.parametric","params":"{\\"band0.type\\":1,' +
  '\\"band0.frequencyHz\\":1000,\\"band0.gainDb\\":-3,\\"band0.enabled\\":1}"}]}],' +
  '"buses":[],"connections":[]}';

describe('engine meter input peaks and constructed insert values', () => {
  it('publishes input peaks and resets the integrated master loudness', () => {
    const block = 1024;
    const engine = new RealtimeEngine(48000, block);
    try {
      engine.setInputMonitor(true);
      engine.play();
      const tone = new Float32Array(block);
      for (let i = 0; i < block; i += 1) {
        tone[i] = 0.25 * Math.sin((2 * Math.PI * 1000 * i) / 48000);
      }
      const master = () => engine.drainMeterTelemetry().filter((record) => record.targetId === 0);
      // More than the 3 s short-term window, so integrated loudness is gated in.
      for (let i = 0; i < 160; i += 1) {
        engine.process([tone, tone]);
      }
      const before = master().at(-1);
      expect(before?.inputPeakDbL).toBeCloseTo(20 * Math.log10(0.25), 1);
      expect(before?.inputPeakDbR).toBeCloseTo(20 * Math.log10(0.25), 1);
      expect(before?.integratedLufs).toBeGreaterThan(-60);

      engine.resetMasterLoudnessMeter();
      engine.process([tone, tone]);
      const after = master().at(-1);
      expect(after?.integratedLufs).toBeLessThanOrEqual(-100);
      expect(after?.momentaryLufs).toBeGreaterThan(-60);
    } finally {
      engine.destroy();
    }
  });

  it('publishes input peaks on the wide meter surface', () => {
    const engine = new RealtimeEngine(48000, 256);
    const frames = 256;
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
      engine.setTrackBuses([{ busId: 1, gainDb: 0, channelLayout: 2 }]);
      engine.setTrackLanes([{ trackId: 10, outputBusId: 1 }]);
      engine.setTrackStripJson(
        10,
        '{"version":1,"buses":[{"id":"master","role":"master"}],"strips":[' +
          '{"id":"s","surroundPan":{"azimuth":-110}}]}',
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
      const busMeter = engine.drainMeterTelemetryWide().find((record) => record.targetId === 33);
      expect(busMeter).toBeDefined();
      if (busMeter) {
        expect(busMeter.inputPeakDb).toHaveLength(busMeter.channelCount);
        expect(Math.max(...busMeter.inputPeakDb)).toBeGreaterThan(-60);
      }
    } finally {
      engine.destroy();
    }
  });

  it('exposes constructed insert values with invalid-id errors', () => {
    const engine = new RealtimeEngine(48000, 128);
    try {
      engine.setTrackLanes([10]);
      engine.setTrackStripJson(10, stripJson);
      const paramId = engine.resolveTrackInsertAutomationId(10, 0, 'band0.gainDb');
      expect(paramId).toBeGreaterThan(0);

      expect(engine.insertParameterConstructedValue(paramId)).toBeCloseTo(-3, 5);
      expect(() => engine.insertParameterConstructedValue(0)).toThrow();
    } finally {
      engine.destroy();
    }
  });
});

describe('per-insert gain reduction query', () => {
  const insert = (slot: string, processor: string, params: Record<string, number>) => ({
    slot,
    processor,
    params: JSON.stringify(params),
  });
  const sceneJson = JSON.stringify({
    version: 1,
    strips: [
      {
        id: 'track-10',
        inserts: [
          insert('pre', 'dynamics.compressor', {
            thresholdDb: -30,
            ratio: 10,
            attackMs: 0.1,
            releaseMs: 100,
          }),
          insert('post', 'dynamics.limiter', { thresholdDb: -20, lookaheadMs: 0, releaseMs: 50 }),
        ],
      },
    ],
    buses: [],
    connections: [],
  });

  it('reports each insert in order and agrees with the meter record', () => {
    const block = 512;
    const engine = new RealtimeEngine(48000, block);
    try {
      engine.setClips([
        {
          id: 1,
          trackId: 10,
          channels: [new Float32Array(block * 64).fill(0.9)],
          startPpq: 0,
          lengthSamples: block * 64,
        },
      ]);
      engine.setTrackLanes([10]);
      engine.setTrackStripJson(10, sceneJson);
      engine.play();
      for (let i = 0; i < 32; i += 1) {
        engine.process([new Float32Array(block), new Float32Array(block)]);
      }
      const records = engine.drainMeterTelemetry();
      const lane = records.filter((r) => r.gainReductionDb < 0).at(-1);
      expect(lane).toBeDefined();
      const entries = engine.meterTargetInsertGainReduction(lane?.targetId ?? -1);
      expect(entries).toHaveLength(2);
      for (const db of entries) {
        expect(db).toBeLessThanOrEqual(0);
      }
      expect(Math.min(...entries)).toBeCloseTo(lane?.gainReductionDb ?? 0, 3);
      expect(entries[0]).toBeLessThan(0);
      expect(entries[1]).toBeLessThan(0);

      expect(engine.meterTargetInsertGainReduction(0xffff)).toEqual([]);
      expect(() => engine.meterTargetInsertGainReduction(41)).toThrow();
      expect(() => engine.meterTargetInsertGainReduction('1' as unknown as number)).toThrow(
        TypeError,
      );
    } finally {
      engine.destroy();
    }
  });
});
