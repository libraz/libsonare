/**
 * Part-rig WASM binding tests: the project set/get/clear round trip and its
 * undo, the input each entry refuses, and the realtime engine's direct call.
 */

import { beforeAll, describe, expect, it } from 'vitest';
import {
  init,
  PART_RIG_ALL_PARTS,
  PART_RIG_MODES,
  type PartRigInsert,
  Project,
  RealtimeEngine,
} from '../dist/index.js';
import { setSonareModule } from '../src/module_state.js';

const LIMITER: PartRigInsert = {
  processor: 'dynamics.limiter',
  params: { thresholdDb: -6, lookaheadMs: 0, releaseMs: 50 },
};

describe('Sonare WASM part rig', () => {
  beforeAll(async () => {
    await init();
    const createModule = (await import('../dist/sonare.js')).default;
    setSonareModule(await createModule());
  });

  it('exposes the mode table and the all-parts sentinel', () => {
    expect([...PART_RIG_MODES]).toEqual(['bank', 'none', 'chain']);
    expect(PART_RIG_ALL_PARTS).toBe(0xff);
  });

  it('round-trips set, get and clear, per destination and part', () => {
    const project = new Project();
    try {
      expect(project.getPartRig(3, 0)).toBeNull();

      project.setPartRig(3, 0, 'chain', [LIMITER]);
      expect(project.getPartRig(3, 0)).toEqual({
        mode: 'chain',
        inserts: [{ processor: 'dynamics.limiter', params: LIMITER.params }],
      });

      // An already-serialized params string is accepted, and the ordinal stands for the name.
      project.setPartRig(3, 1, 2, [{ processor: LIMITER.processor, params: '{"thresholdDb":-3}' }]);
      expect(project.getPartRig(3, 1)?.inserts?.[0].params).toEqual({ thresholdDb: -3 });

      // kBank is an explicit entry, not an absent one, and the default is its own key.
      project.setPartRig(3, PART_RIG_ALL_PARTS, 'bank');
      expect(project.getPartRig(3, PART_RIG_ALL_PARTS)).toEqual({ mode: 'bank' });
      project.setPartRig(3, 2, 'none');
      expect(project.getPartRig(3, 2)).toEqual({ mode: 'none' });
      expect(project.getPartRig(4, 0)).toBeNull();

      project.clearPartRig(3, 0);
      expect(project.getPartRig(3, 0)).toBeNull();
      expect(project.getPartRig(3, 1)).not.toBeNull();
      // Clearing what is not stored is not an error.
      project.clearPartRig(3, 0);
    } finally {
      project.destroy();
    }
  });

  it('refuses invalid input and leaves the stored rig alone', () => {
    const project = new Project();
    try {
      project.setPartRig(0, 0, 'none');
      expect(() => project.setPartRig(0, 16, 'none')).toThrow(/out of range/);
      expect(() => project.setPartRig(0, 256, 'none')).toThrow(/out of range/);
      expect(() => project.getPartRig(0, 16)).toThrow(/out of range/);
      // @ts-expect-error an unknown mode name is rejected at runtime
      expect(() => project.setPartRig(0, 0, 'wet')).toThrow(/wet/);
      expect(() => project.setPartRig(0, 0, 3)).toThrow(/out of range/);
      // Inserts belong to the chain mode only, and the chain needs them.
      expect(() => project.setPartRig(0, 0, 'none', [LIMITER])).toThrow();
      expect(() => project.setPartRig(0, 0, 'chain')).toThrow();
      expect(() => project.setPartRig(0, 0, 'chain', [])).toThrow();
      expect(() =>
        project.setPartRig(0, 0, 'chain', [{ processor: 'no.such.processor' }]),
      ).toThrow();
      expect(() => project.setPartRig(0, 0, 'chain', Array(9).fill(LIMITER))).toThrow();
      expect(() =>
        project.setPartRig(0, 0, 'chain', [{ processor: LIMITER.processor, params: '{not json' }]),
      ).toThrow();
      expect(project.getPartRig(0, 0)).toEqual({ mode: 'none' });
    } finally {
      project.destroy();
    }
  });

  it('undoes and redoes a set and a clear', () => {
    const project = new Project();
    try {
      project.setPartRig(1, 0, 'chain', [LIMITER]);
      project.setPartRig(1, 0, 'none');
      project.undo();
      expect(project.getPartRig(1, 0)?.mode).toBe('chain');
      project.undo();
      expect(project.getPartRig(1, 0)).toBeNull();
      project.redo();
      expect(project.getPartRig(1, 0)?.mode).toBe('chain');

      project.clearPartRig(1, 0);
      expect(project.getPartRig(1, 0)).toBeNull();
      project.undo();
      expect(project.getPartRig(1, 0)?.mode).toBe('chain');
    } finally {
      project.destroy();
    }
  });

  it('engine.setPartRig keeps its refusals apart', () => {
    const engine = new RealtimeEngine(48000, 128);
    try {
      // Nothing is bound to destination 0 yet.
      expect(() => engine.setPartRig(0, 0, 'none')).toThrow(/no MIDI instrument/);

      // A builtin has no part rigs: refused rather than accepted quietly.
      engine.setBuiltinInstrument({});
      expect(() => engine.setPartRig(0, 0, 'none')).toThrow(/no part rigs/);

      // The shape is judged before the instrument is consulted.
      expect(() => engine.setPartRig(0, 16, 'none')).toThrow(/out of range/);
      expect(() => engine.setPartRig(0, 0, 'chain')).toThrow();
      expect(() =>
        engine.setPartRig(0, 0, 'chain', [{ processor: 'no.such.processor' }]),
      ).toThrow();
    } finally {
      engine.destroy();
    }
  });

  it('engine.setPartRig reaches a NativeSynth', () => {
    const engine = new RealtimeEngine(48000, 128);
    try {
      engine.setSynthInstrument({ engineMode: 'reed' });
      engine.setPartRig(0, 0, 'none');
      engine.setPartRig(0, PART_RIG_ALL_PARTS, 'chain', [LIMITER]);
      engine.setPartRig(0, 0, 'bank');
    } finally {
      engine.destroy();
    }
  });
});
