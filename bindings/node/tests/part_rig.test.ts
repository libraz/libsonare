/**
 * Pins the part-rig facade: the project set/get/clear round trip, the refusals
 * the facade and the C ABI keep apart, undo, and the engine's answer for an
 * instrument that carries no part rigs.
 */

import { describe, expect, it } from 'vitest';
import { PART_RIG_ALL_PARTS, PART_RIG_MODES, Project, RealtimeEngine } from '../src/index.js';

/** The `codeName` of the SonareError a call throws, or a marker when it does not. */
const codeNameOf = (call: () => unknown): string => {
  try {
    call();
  } catch (error) {
    return (error as { codeName?: string }).codeName ?? 'not a SonareError';
  }
  return 'did not throw';
};

const chain = [{ processor: 'saturation.overdrive', params: { gainDb: 12 } }];

describe('part rig on a project', () => {
  it('exports the all-parts sentinel and the mode names', () => {
    expect(PART_RIG_ALL_PARTS).toBe(0xff);
    expect([...PART_RIG_MODES]).toEqual(['bank', 'none', 'chain']);
  });

  it('reports an absent entry as null', () => {
    using project = Project.create();
    expect(project.getPartRig({ destinationId: 4, part: 0 })).toBeNull();
  });

  it('round-trips none, bank and chain entries', () => {
    using project = Project.create();
    project.setPartRig({ destinationId: 4, part: 1, mode: 'none' });
    project.setPartRig({ destinationId: 4, part: PART_RIG_ALL_PARTS, mode: 'bank' });
    project.setPartRig({ destinationId: 4, part: 2, mode: 'chain', inserts: chain });
    expect(project.getPartRig({ destinationId: 4, part: 1 })).toEqual({ mode: 'none' });
    expect(project.getPartRig({ destinationId: 4, part: PART_RIG_ALL_PARTS })).toEqual({
      mode: 'bank',
    });
    const got = project.getPartRig({ destinationId: 4, part: 2 });
    expect(got?.mode).toBe('chain');
    expect(got?.inserts?.map((i) => i.processor)).toEqual(['saturation.overdrive']);
    expect((got?.inserts?.[0]?.params as { gainDb: number } | undefined)?.gainDb).toBe(12);
  });

  it('accepts params as a JSON string', () => {
    using project = Project.create();
    project.setPartRig({
      destinationId: 1,
      part: 0,
      mode: 'chain',
      inserts: [{ processor: 'saturation.overdrive', params: '{"gainDb":6}' }],
    });
    const got = project.getPartRig({ destinationId: 1, part: 0 });
    expect((got?.inserts?.[0]?.params as { gainDb: number } | undefined)?.gainDb).toBe(6);
  });

  it('defaults omitted insert params to an empty object', () => {
    using project = Project.create();
    project.setPartRig({
      destinationId: 1,
      part: 0,
      mode: 'chain',
      inserts: [{ processor: 'saturation.overdrive' }],
    });
    expect(project.getPartRig({ destinationId: 1, part: 0 })).toEqual({
      mode: 'chain',
      inserts: [{ processor: 'saturation.overdrive', params: {} }],
    });
  });

  it('clears an entry and undo restores it', () => {
    using project = Project.create();
    project.setPartRig({ destinationId: 4, part: 3, mode: 'none' });
    project.clearPartRig({ destinationId: 4, part: 3 });
    expect(project.getPartRig({ destinationId: 4, part: 3 })).toBeNull();
    project.undo();
    expect(project.getPartRig({ destinationId: 4, part: 3 })).toEqual({ mode: 'none' });
    project.undo();
    expect(project.getPartRig({ destinationId: 4, part: 3 })).toBeNull();
    project.redo();
    expect(project.getPartRig({ destinationId: 4, part: 3 })).toEqual({ mode: 'none' });
  });

  it('refuses invalid input by name', () => {
    using project = Project.create();
    const set = (request: object) => () =>
      project.setPartRig({ destinationId: 4, part: 0, mode: 'none', ...request } as never);
    expect(() => set({ mode: 'loud' })()).toThrow(/mode/);
    expect(() => set({ part: 'x' })()).toThrow(/part/);
    expect(() => set({ destinationId: 'x' })()).toThrow(/destinationId/);
    expect(() => set({ mode: 'chain', inserts: 'nope' })()).toThrow(/inserts/);
    expect(() => set({ mode: 'chain', inserts: [{ params: {} }] })()).toThrow(/processor/);
    expect(() =>
      set({
        mode: 'chain',
        inserts: [{ processor: 'saturation.overdrive', params: null }],
      })(),
    ).toThrow(/params/);
    expect(() =>
      set({
        mode: 'chain',
        inserts: [{ processor: 'saturation.overdrive', params: 1 }],
      })(),
    ).toThrow(/params/);
    expect(codeNameOf(set({ part: 16 }))).toBe('InvalidParameter');
    expect(codeNameOf(set({ mode: 'chain', inserts: [] }))).toBe('InvalidParameter');
    expect(
      codeNameOf(set({ mode: 'chain', inserts: [{ processor: 'no.such', params: {} }] })),
    ).toBe('InvalidParameter');
    expect(codeNameOf(set({ mode: 'none', inserts: chain }))).toBe('InvalidParameter');
    expect(
      codeNameOf(
        set({ mode: 'chain', inserts: [{ processor: 'saturation.overdrive', params: '{' }] }),
      ),
    ).toBe('InvalidParameter');
    expect(project.getPartRig({ destinationId: 4, part: 0 })).toBeNull();
  });
});

describe('part rig on the realtime engine', () => {
  it('throws NotSupported on a builtin instrument', () => {
    const engine = new RealtimeEngine(48000, 128);
    engine.setBuiltinInstrument({}, 3);
    expect(codeNameOf(() => engine.setPartRig({ destinationId: 3, part: 0, mode: 'none' }))).toBe(
      'NotSupported',
    );
    engine.destroy();
  });

  it('accepts a rig on a NativeSynth', () => {
    const engine = new RealtimeEngine(48000, 128);
    engine.setSynthInstrument({}, 3);
    engine.setPartRig({ destinationId: 3, part: 0, mode: 'chain', inserts: chain });
    engine.setPartRig({ destinationId: 3, part: PART_RIG_ALL_PARTS, mode: 'bank' });
    engine.destroy();
  });

  it('accepts omitted insert params on a NativeSynth', () => {
    const engine = new RealtimeEngine(48000, 128);
    engine.setSynthInstrument({}, 3);
    expect(() =>
      engine.setPartRig({
        destinationId: 3,
        part: 0,
        mode: 'chain',
        inserts: [{ processor: 'saturation.overdrive' }],
      }),
    ).not.toThrow();
    engine.destroy();
  });
});

describe('part rig request and positional forms', () => {
  const louder = [{ processor: 'saturation.overdrive', params: { gainDb: 24 } }];

  it('stores, reads and clears the same entry through either form', () => {
    using byRequest = Project.create();
    using byPosition = Project.create();
    byRequest.setPartRig({ destinationId: 4, part: 2, mode: 'chain', inserts: chain });
    byPosition.setPartRig(4, 2, 'chain', chain);
    const stored = byRequest.getPartRig({ destinationId: 4, part: 2 });
    expect(stored?.inserts?.[0].params).toEqual({ gainDb: 12 });
    expect(byPosition.getPartRig(4, 2)).toEqual(stored);
    expect(byPosition.getPartRig({ destinationId: 4, part: 2 })).toEqual(stored);
    // The control: other inserts read back differently, so the equality above
    // cannot hold because a form dropped the inserts.
    byPosition.setPartRig(4, 3, 'chain', louder);
    expect(byPosition.getPartRig(4, 3)).not.toEqual(stored);
    byRequest.clearPartRig({ destinationId: 4, part: 2 });
    byPosition.clearPartRig(4, 2);
    expect(byRequest.getPartRig(4, 2)).toBeNull();
    expect(byPosition.getPartRig({ destinationId: 4, part: 2 })).toBeNull();
  });

  it.each([
    ['an unknown mode', { mode: 'loud' }],
    ['a non-numeric part', { part: 'x' }],
    ['a non-numeric destination', { destinationId: 'x' }],
    ['a non-array inserts', { mode: 'chain', inserts: 'nope' }],
    ['an insert without a processor', { mode: 'chain', inserts: [{ params: {} }] }],
    ['an out-of-range part', { part: 16 }],
  ])('refuses %s identically in both forms', (_, change) => {
    using project = Project.create();
    const r = { destinationId: 4, part: 0, mode: 'none', ...change } as Record<string, unknown>;
    const capture = (call: () => unknown): string => {
      try {
        call();
      } catch (error) {
        return `${(error as Error).name}: ${(error as Error).message}`;
      }
      return 'did not throw';
    };
    const viaRequest = capture(() => project.setPartRig(r as never));
    const positional = [r.destinationId, r.part, r.mode, r.inserts] as [never, never, never, never];
    expect(viaRequest).not.toBe('did not throw');
    expect(capture(() => project.setPartRig(...positional))).toBe(viaRequest);
  });

  it('refuses a request object followed by positional arguments', () => {
    using project = Project.create();
    expect(() =>
      (project.setPartRig as (...args: unknown[]) => void)(
        { destinationId: 4, part: 0, mode: 'none' },
        0,
      ),
    ).toThrow(new TypeError('setPartRig: a request object takes no further arguments'));
    expect(() => project.getPartRig({ destinationId: 4, part: 0 } as never, 0 as never)).toThrow(
      /getPartRig: a request object/,
    );
  });

  it('answers the engine the same through either form', () => {
    const engine = new RealtimeEngine(48000, 128);
    engine.setBuiltinInstrument({}, 3);
    expect(codeNameOf(() => engine.setPartRig(3, 0, 'none'))).toBe('NotSupported');
    engine.setSynthInstrument({}, 3);
    expect(() => engine.setPartRig(3, 0, 'chain', chain)).not.toThrow();
    expect(() => engine.setPartRig(3, 0, 'none', chain)).toThrow();
    engine.destroy();
  });
});
