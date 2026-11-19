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
      expect(project.partRig({ destinationId: 3, part: 0 })).toBeNull();

      project.setPartRig({ destinationId: 3, part: 0, mode: 'chain', inserts: [LIMITER] });
      expect(project.partRig({ destinationId: 3, part: 0 })).toEqual({
        mode: 'chain',
        inserts: [{ processor: 'dynamics.limiter', params: LIMITER.params }],
      });

      // An already-serialized params string is refused, and nothing is stored.
      expect(() =>
        project.setPartRig({
          destinationId: 3,
          part: 1,
          mode: 'chain',
          inserts: [{ processor: LIMITER.processor, params: '{"thresholdDb":-3}' as never }],
        }),
      ).toThrow(/inserts\[0\]\.params must be an object/);
      expect(project.partRig({ destinationId: 3, part: 1 })).toBeNull();
      project.setPartRig({
        destinationId: 3,
        part: 1,
        mode: 'chain',
        inserts: [{ processor: LIMITER.processor, params: { thresholdDb: -3 } }],
      });
      expect(project.partRig({ destinationId: 3, part: 1 })?.inserts?.[0].params).toEqual({
        thresholdDb: -3,
      });

      // kBank is an explicit entry, not an absent one, and the default is its own key.
      project.setPartRig({ destinationId: 3, part: PART_RIG_ALL_PARTS, mode: 'bank' });
      expect(project.partRig({ destinationId: 3, part: PART_RIG_ALL_PARTS })).toEqual({
        mode: 'bank',
      });
      project.setPartRig({ destinationId: 3, part: 2, mode: 'none' });
      expect(project.partRig({ destinationId: 3, part: 2 })).toEqual({ mode: 'none' });
      expect(project.partRig({ destinationId: 4, part: 0 })).toBeNull();

      project.clearPartRig({ destinationId: 3, part: 0 });
      expect(project.partRig({ destinationId: 3, part: 0 })).toBeNull();
      expect(project.partRig({ destinationId: 3, part: 1 })).not.toBeNull();
      // Clearing what is not stored is not an error.
      project.clearPartRig({ destinationId: 3, part: 0 });
    } finally {
      project.destroy();
    }
  });

  it('refuses invalid input and leaves the stored rig alone', () => {
    const project = new Project();
    try {
      project.setPartRig({ destinationId: 0, part: 0, mode: 'none' });
      expect(() => project.setPartRig({ destinationId: 0, part: 16, mode: 'none' })).toThrow(
        /out of range/,
      );
      expect(() => project.setPartRig({ destinationId: 0, part: 256, mode: 'none' })).toThrow(
        /out of range/,
      );
      expect(() => project.partRig({ destinationId: 0, part: 16 })).toThrow(/out of range/);
      // @ts-expect-error an unknown mode name is rejected at runtime
      expect(() => project.setPartRig({ destinationId: 0, part: 0, mode: 'wet' })).toThrow(/wet/);
      // The mode is a string token, never an ordinal.
      expect(() => project.setPartRig({ destinationId: 0, part: 0, mode: 3 as never })).toThrow(
        TypeError,
      );
      expect(() =>
        project.setPartRig({ destinationId: 0, part: 0, mode: 2 as never, inserts: [LIMITER] }),
      ).toThrow(/string token/);
      // Inserts belong to the chain mode only, and the chain needs them.
      expect(() =>
        project.setPartRig({ destinationId: 0, part: 0, mode: 'none', inserts: [LIMITER] }),
      ).toThrow();
      expect(() => project.setPartRig({ destinationId: 0, part: 0, mode: 'chain' })).toThrow();
      expect(() =>
        project.setPartRig({ destinationId: 0, part: 0, mode: 'chain', inserts: [] }),
      ).toThrow();
      expect(() =>
        project.setPartRig({
          destinationId: 0,
          part: 0,
          mode: 'chain',
          inserts: [{ processor: 'no.such.processor' }],
        }),
      ).toThrow();
      expect(() =>
        project.setPartRig({
          destinationId: 0,
          part: 0,
          mode: 'chain',
          inserts: Array(9).fill(LIMITER),
        }),
      ).toThrow();
      expect(() =>
        project.setPartRig({
          destinationId: 0,
          part: 0,
          mode: 'chain',
          inserts: [{ processor: LIMITER.processor, params: '{not json' as never }],
        }),
      ).toThrow(TypeError);
      expect(project.partRig({ destinationId: 0, part: 0 })).toEqual({ mode: 'none' });
    } finally {
      project.destroy();
    }
  });

  it('undoes and redoes a set and a clear', () => {
    const project = new Project();
    try {
      const key = { destinationId: 1, part: 0 };
      project.setPartRig({ ...key, mode: 'chain', inserts: [LIMITER] });
      project.setPartRig({ ...key, mode: 'none' });
      project.undo();
      expect(project.partRig(key)?.mode).toBe('chain');
      project.undo();
      expect(project.partRig(key)).toBeNull();
      project.redo();
      expect(project.partRig(key)?.mode).toBe('chain');

      project.clearPartRig(key);
      expect(project.partRig(key)).toBeNull();
      project.undo();
      expect(project.partRig(key)?.mode).toBe('chain');
    } finally {
      project.destroy();
    }
  });

  it('engine.setPartRig keeps its refusals apart', () => {
    const engine = new RealtimeEngine({ sampleRate: 48000, maxBlockSize: 128 });
    try {
      // Nothing is bound to destination 0 yet.
      expect(() => engine.setPartRig({ destinationId: 0, part: 0, mode: 'none' })).toThrow(
        /no MIDI instrument/,
      );

      // A builtin has no part rigs: refused rather than accepted quietly.
      engine.setBuiltinInstrument(0, {});
      expect(() => engine.setPartRig({ destinationId: 0, part: 0, mode: 'none' })).toThrow(
        /no part rigs/,
      );

      // The shape is judged before the instrument is consulted.
      expect(() => engine.setPartRig({ destinationId: 0, part: 16, mode: 'none' })).toThrow(
        /out of range/,
      );
      expect(() => engine.setPartRig({ destinationId: 0, part: 0, mode: 'chain' })).toThrow();
      expect(() =>
        engine.setPartRig({
          destinationId: 0,
          part: 0,
          mode: 'chain',
          inserts: [{ processor: 'no.such.processor' }],
        }),
      ).toThrow();
    } finally {
      engine.destroy();
    }
  });

  it('engine.setPartRig reaches a NativeSynth', () => {
    const engine = new RealtimeEngine({ sampleRate: 48000, maxBlockSize: 128 });
    try {
      engine.setSynthInstrument(0, { engineMode: 'reed' });
      engine.setPartRig({ destinationId: 0, part: 0, mode: 'none' });
      engine.setPartRig({
        destinationId: 0,
        part: PART_RIG_ALL_PARTS,
        mode: 'chain',
        inserts: [LIMITER],
      });
      engine.setPartRig({ destinationId: 0, part: 0, mode: 'bank' });
    } finally {
      engine.destroy();
    }
  });
});

describe('Sonare WASM part rig request form', () => {
  beforeAll(async () => {
    await init();
    const createModule = (await import('../dist/sonare.js')).default;
    setSonareModule(await createModule());
  });

  const LOUDER: PartRigInsert = { processor: 'dynamics.limiter', params: { thresholdDb: -12 } };

  it('stores, reads and clears entries independently per key', () => {
    const project = new Project();
    try {
      project.setPartRig({ destinationId: 4, part: 2, mode: 'chain', inserts: [LIMITER] });
      const stored = project.partRig({ destinationId: 4, part: 2 });
      expect(stored?.inserts?.[0].params).toEqual(LIMITER.params);
      // The control: other inserts read back differently, so the entry is not
      // a constant the read returns whatever was stored.
      project.setPartRig({ destinationId: 4, part: 3, mode: 'chain', inserts: [LOUDER] });
      expect(project.partRig({ destinationId: 4, part: 3 })).not.toEqual(stored);
      project.clearPartRig({ destinationId: 4, part: 2 });
      expect(project.partRig({ destinationId: 4, part: 2 })).toBeNull();
      expect(project.partRig({ destinationId: 4, part: 3 })).not.toBeNull();
    } finally {
      project.destroy();
    }
  });

  it.each([
    ['an unknown mode', { mode: 'loud' }],
    ['a non-numeric part', { part: 'x' }],
    ['a non-numeric destination', { destinationId: 'x' }],
    ['a non-array inserts', { mode: 'chain', inserts: 'nope' }],
    ['an insert without a processor', { mode: 'chain', inserts: [{ params: {} }] }],
    ['an out-of-range part', { part: 16 }],
  ])('refuses %s', (_, change) => {
    const project = new Project();
    try {
      const r = { destinationId: 4, part: 0, mode: 'none', ...change };
      expect(() => project.setPartRig(r as never)).toThrow();
      expect(project.partRig({ destinationId: 4, part: 0 })).toBeNull();
    } finally {
      project.destroy();
    }
  });

  it('refuses an unknown key field on a read or a clear', () => {
    const project = new Project();
    try {
      expect(() => project.partRig({ destinationId: 4, part: 0, mode: 'none' } as never)).toThrow(
        new TypeError('Project.partRig: unknown key key.mode (expected destinationId, part)'),
      );
      expect(() =>
        project.clearPartRig({ destinationId: 4, part: 0, mode: 'none' } as never),
      ).toThrow(
        new TypeError('Project.clearPartRig: unknown key key.mode (expected destinationId, part)'),
      );
      expect(() => project.partRig(4 as never)).toThrow(TypeError);
    } finally {
      project.destroy();
    }
  });

  it('takes the engine rig as a request only', () => {
    const engine = new RealtimeEngine({ sampleRate: 48000, maxBlockSize: 128 });
    try {
      engine.setSynthInstrument(0, { engineMode: 'reed' });
      expect(() =>
        engine.setPartRig({
          destinationId: 0,
          part: PART_RIG_ALL_PARTS,
          mode: 'chain',
          inserts: [LIMITER],
        }),
      ).not.toThrow();
      expect(() =>
        engine.setPartRig({ destinationId: 0, part: 0, mode: 'none', inserts: [LIMITER] }),
      ).toThrow();
      expect(() =>
        engine.setPartRig({ destinationId: 0, part: 0, mode: 'none', extra: 1 } as never),
      ).toThrow(TypeError);
      expect(() => engine.setPartRig({ destinationId: 0, part: 0, mode: 0 as never })).toThrow(
        TypeError,
      );
    } finally {
      engine.destroy();
    }
  });
});
