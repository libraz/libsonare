/**
 * Insert automation id lifetimes on the Node RealtimeEngine. Mirrors the C ABI
 * cases in tests/api/sonare_c_engine_strip_test.cpp.
 */

import { describe, expect, it } from 'vitest';
import { RealtimeEngine } from '../src/index.js';

function insertScene(kind: 'strips' | 'buses', id: string, processors: string[]): string {
  const inserts = processors.map((processor, index) => ({
    slot: index < 32 ? 'pre' : 'post',
    processor,
    params: '{}',
  }));
  const strip = { id, inserts };
  return JSON.stringify({
    version: 1,
    strips: kind === 'strips' ? [strip] : [],
    buses: kind === 'buses' ? [strip] : [],
    connections: [],
  });
}

function alive(engine: RealtimeEngine, id: number): boolean {
  try {
    engine.parameterInfo(id);
    return true;
  } catch {
    return false;
  }
}

describe('insert automation ids (Node)', () => {
  it('keeps a track id across reorders and the removal of another track', () => {
    const engine = new RealtimeEngine(48000, 256);
    try {
      engine.setTrackLanes([10, 20]);
      engine.setTrackStripJson(10, insertScene('strips', 'track-10', ['utility.gain']));
      engine.setTrackStripJson(20, insertScene('strips', 'track-20', ['utility.gain']));
      const id10 = engine.resolveTrackInsertAutomationId(10, 0, 'levelDb');
      const id20 = engine.resolveTrackInsertAutomationId(20, 0, 'levelDb');
      expect(id10).toBeGreaterThan(0);
      expect(id20).not.toBe(id10);

      engine.setTrackLanes([20, 10]);
      expect(engine.resolveTrackInsertAutomationId(10, 0, 'levelDb')).toBe(id10);

      engine.setTrackLanes([10]);
      expect(alive(engine, id10)).toBe(true);
      expect(alive(engine, id20)).toBe(false);

      engine.setTrackLanes([20, 10]);
      engine.setTrackStripJson(20, insertScene('strips', 'track-20', ['utility.gain']));
      expect(alive(engine, id20)).toBe(false);
      const readded = engine.resolveTrackInsertAutomationId(20, 0, 'levelDb');
      expect(readded).toBeGreaterThan(0);
      expect(readded).not.toBe(id20);
    } finally {
      engine.destroy();
    }
  });

  it('keeps a bus id across bus reorders and retires it when the bus is removed', () => {
    const engine = new RealtimeEngine(48000, 256);
    try {
      engine.setTrackBuses([{ busId: 1 }, { busId: 2 }]);
      engine.setBusStripJson(1, insertScene('buses', '1', ['utility.gain']));
      engine.setBusStripJson(2, insertScene('buses', '2', ['utility.gain']));
      const id1 = engine.resolveBusInsertAutomationId(1, 0, 'levelDb');
      const id2 = engine.resolveBusInsertAutomationId(2, 0, 'levelDb');
      expect(id1).toBeGreaterThan(0);
      expect(id2).not.toBe(id1);

      engine.setTrackBuses([{ busId: 2 }, { busId: 1 }]);
      expect(engine.resolveBusInsertAutomationId(1, 0, 'levelDb')).toBe(id1);
      expect(alive(engine, id2)).toBe(true);

      engine.setTrackBuses([{ busId: 2 }]);
      expect(alive(engine, id2)).toBe(true);
      expect(alive(engine, id1)).toBe(false);
      engine.setTrackBuses([{ busId: 2 }, { busId: 1 }]);
      engine.setBusStripJson(1, insertScene('buses', '1', ['utility.gain']));
      expect(alive(engine, id1)).toBe(false);
      expect(engine.resolveBusInsertAutomationId(1, 0, 'levelDb')).not.toBe(id1);
    } finally {
      engine.destroy();
    }
  });

  it('retires track, bus and master ids when the slot changes processor type', () => {
    const engine = new RealtimeEngine(48000, 256);
    try {
      engine.setTrackLanes([10]);
      engine.setTrackBuses([{ busId: 1 }]);
      const setAll = (processor: string) => {
        engine.setTrackStripJson(10, insertScene('strips', 'track-10', [processor]));
        engine.setBusStripJson(1, insertScene('buses', '1', [processor]));
        engine.setMasterStripJson(insertScene('strips', 'master', [processor]));
      };
      const resolveAll = (key: string) => [
        engine.resolveTrackInsertAutomationId(10, 0, key),
        engine.resolveBusInsertAutomationId(1, 0, key),
        engine.resolveMasterInsertAutomationId(0, key),
      ];

      setAll('utility.gain');
      const gainIds = resolveAll('levelDb');
      for (const id of gainIds) expect(id).toBeGreaterThan(0);
      setAll('utility.gain');
      expect(resolveAll('levelDb')).toEqual(gainIds);

      setAll('dynamics.compressor');
      for (const id of gainIds) expect(alive(engine, id)).toBe(false);
      const compressorIds = resolveAll('thresholdDb');
      compressorIds.forEach((id, index) => {
        expect(alive(engine, id)).toBe(true);
        expect(id).not.toBe(gainIds[index]);
      });
      setAll('utility.gain');
      for (const id of gainIds) expect(alive(engine, id)).toBe(false);
    } finally {
      engine.destroy();
    }
  });

  it('refuses a strip change that would overflow the id table', () => {
    const engine = new RealtimeEngine(48000, 64);
    try {
      // 64 slots per rebuild against 8192 entries: rebuild 129 cannot fit.
      const gain = insertScene('strips', 'master', Array(64).fill('utility.gain'));
      const tilt = insertScene('strips', 'master', Array(64).fill('eq.tilt'));
      for (let round = 0; round < 128; round += 1) {
        engine.setMasterStripJson(round % 2 === 0 ? gain : tilt);
      }
      const before = engine.resolveMasterInsertAutomationId(0, 'tiltDb');
      expect(() => engine.setMasterStripJson(gain)).toThrow();
      expect(alive(engine, before)).toBe(true);
      expect(() => engine.setMasterStripJson(tilt)).not.toThrow();
    } finally {
      engine.destroy();
    }
  });
});
