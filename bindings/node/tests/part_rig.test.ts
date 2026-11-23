/**
 * Pins the part-rig facade: the project set/get/clear round trip, the refusals
 * the facade and the C ABI keep apart, undo, and the engine's answer for an
 * instrument that carries no part rigs.
 */

import { describe, expect, it } from 'vitest';
import { PART_RIG_ALL_PARTS, PART_RIG_MODES, Project, RealtimeEngine } from '../src/index.js';
import { expectRefusal } from './_helpers.js';

const escapeRegExp = (text: string): string => text.replace(/[.*+?^${}()|[\]\\]/g, '\\$&');

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
    using project = new Project();
    expect(project.partRig({ destinationId: 4, part: 0 })).toBeNull();
  });

  it('round-trips none, bank and chain entries', () => {
    using project = new Project();
    project.setPartRig({ destinationId: 4, part: 1, mode: 'none' });
    project.setPartRig({ destinationId: 4, part: PART_RIG_ALL_PARTS, mode: 'bank' });
    project.setPartRig({ destinationId: 4, part: 2, mode: 'chain', inserts: chain });
    expect(project.partRig({ destinationId: 4, part: 1 })).toEqual({ mode: 'none' });
    expect(project.partRig({ destinationId: 4, part: PART_RIG_ALL_PARTS })).toEqual({
      mode: 'bank',
    });
    const got = project.partRig({ destinationId: 4, part: 2 });
    expect(got?.mode).toBe('chain');
    expect(got?.inserts?.map((i) => i.processor)).toEqual(['saturation.overdrive']);
    expect((got?.inserts?.[0]?.params as { gainDb: number } | undefined)?.gainDb).toBe(12);
  });

  it('refuses params as a JSON string', () => {
    using project = new Project();
    const request = {
      destinationId: 1,
      part: 0,
      mode: 'chain',
      inserts: [{ processor: 'saturation.overdrive', params: '{"gainDb":6}' }],
    };
    expect(() => project.setPartRig(request as never)).toThrow(TypeError);
    expect(() => project.setPartRig(request as never)).toThrow(
      /inserts\[0\]\.params must be an object/,
    );
    expect(project.partRig({ destinationId: 1, part: 0 })).toBeNull();
  });

  it('defaults omitted insert params to an empty object', () => {
    using project = new Project();
    project.setPartRig({
      destinationId: 1,
      part: 0,
      mode: 'chain',
      inserts: [{ processor: 'saturation.overdrive' }],
    });
    expect(project.partRig({ destinationId: 1, part: 0 })).toEqual({
      mode: 'chain',
      inserts: [{ processor: 'saturation.overdrive', params: {} }],
    });
  });

  it('clears an entry and undo restores it', () => {
    using project = new Project();
    project.setPartRig({ destinationId: 4, part: 3, mode: 'none' });
    project.clearPartRig({ destinationId: 4, part: 3 });
    expect(project.partRig({ destinationId: 4, part: 3 })).toBeNull();
    project.undo();
    expect(project.partRig({ destinationId: 4, part: 3 })).toEqual({ mode: 'none' });
    project.undo();
    expect(project.partRig({ destinationId: 4, part: 3 })).toBeNull();
    project.redo();
    expect(project.partRig({ destinationId: 4, part: 3 })).toEqual({ mode: 'none' });
  });

  it('refuses invalid input by name', () => {
    using project = new Project();
    const set = (request: object) => () =>
      project.setPartRig({ destinationId: 4, part: 0, mode: 'none', ...request } as never);
    const refused = (request: object, kind: ErrorConstructor, text: string) => {
      expectRefusal(
        set(request),
        kind,
        new RegExp(`^Project\\.setPartRig: ${escapeRegExp(text)}$`),
      );
    };
    expect(() => set({ mode: 'loud' })()).toThrow(/mode/);
    expect(() => set({ part: 'x' })()).toThrow(/part/);
    expect(() => set({ destinationId: 'x' })()).toThrow(/destinationId/);
    refused({ mode: 'chain', inserts: 'nope' }, TypeError, 'inserts must be an array');
    refused(
      { mode: 'chain', inserts: [{ params: {} }] },
      TypeError,
      'inserts[0].processor must be given',
    );
    for (const params of [null, 1, '{']) {
      refused(
        { mode: 'chain', inserts: [{ processor: 'saturation.overdrive', params }] },
        TypeError,
        'inserts[0].params must be an object',
      );
    }
    expect(set({ part: 16 })).toThrow(RangeError);
    // Content defects of the rig inserts are argument refusals, rooted at `inserts`.
    refused(
      { mode: 'chain', inserts: [] },
      RangeError,
      'inserts must hold 1 to 8 entries for a chain rig',
    );
    refused(
      { mode: 'chain', inserts: [{ processor: 'no.such', params: {} }] },
      RangeError,
      'inserts[0].processor must name an insert processor',
    );
    // Inserts beside a mode that takes none refuse which keys were given: a shape error.
    expect(set({ mode: 'none', inserts: chain })).toThrow(TypeError);
    expect(project.partRig({ destinationId: 4, part: 0 })).toBeNull();
  });
});

describe('part rig on the realtime engine', () => {
  it('throws NotSupported on a builtin instrument', () => {
    const engine = new RealtimeEngine({ sampleRate: 48000, maxBlockSize: 128 });
    engine.setBuiltinInstrument(3, {});
    expect(codeNameOf(() => engine.setPartRig({ destinationId: 3, part: 0, mode: 'none' }))).toBe(
      'NotSupported',
    );
    engine.destroy();
  });

  it('accepts a rig on a NativeSynth', () => {
    const engine = new RealtimeEngine({ sampleRate: 48000, maxBlockSize: 128 });
    engine.setSynthInstrument(3, {});
    engine.setPartRig({ destinationId: 3, part: 0, mode: 'chain', inserts: chain });
    engine.setPartRig({ destinationId: 3, mode: 'bank' } as never);
    engine.destroy();
  });

  it('accepts omitted insert params on a NativeSynth', () => {
    const engine = new RealtimeEngine({ sampleRate: 48000, maxBlockSize: 128 });
    engine.setSynthInstrument(3, {});
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

describe('part rig request form', () => {
  const louder = [{ processor: 'saturation.overdrive', params: { gainDb: 24 } }];

  it('stores, reads and clears entries through the request object', () => {
    using project = new Project();
    project.setPartRig({ destinationId: 4, part: 2, mode: 'chain', inserts: chain });
    const stored = project.partRig({ destinationId: 4, part: 2 });
    expect(stored?.inserts?.[0].params).toEqual({ gainDb: 12 });
    // The control: other inserts read back differently, so the equality below
    // cannot hold because the inserts were dropped.
    project.setPartRig({ destinationId: 4, part: 3, mode: 'chain', inserts: louder });
    expect(project.partRig({ destinationId: 4, part: 3 })).not.toEqual(stored);
    project.clearPartRig({ destinationId: 4, part: 2 });
    expect(project.partRig({ destinationId: 4, part: 2 })).toBeNull();
    expect(project.partRig({ destinationId: 4, part: 3 })?.inserts?.[0].params).toEqual({
      gainDb: 24,
    });
  });

  it('refuses the removed positional forms', () => {
    using project = new Project();
    // @ts-expect-error the positional form is gone.
    expect(() => project.setPartRig(4, 2, 'chain', chain)).toThrow(TypeError);
    // @ts-expect-error the positional form is gone.
    expect(() => project.partRig(4, 2)).toThrow(TypeError);
    // @ts-expect-error the positional form is gone.
    expect(() => project.clearPartRig(4, 2)).toThrow(TypeError);
    expect(project.partRig({ destinationId: 4, part: 2 })).toBeNull();
  });

  it.each([
    ['an unknown mode', { mode: 'loud' }, /mode/],
    ['a non-numeric part', { part: 'x' }, /part/],
    ['a non-numeric destination', { destinationId: 'x' }, /destinationId/],
    ['a non-array inserts', { mode: 'chain', inserts: 'nope' }, /inserts/],
    ['an insert without a processor', { mode: 'chain', inserts: [{ params: {} }] }, /processor/],
    ['an out-of-range part', { part: 16 }, RangeError],
  ])('refuses %s', (_, change, message) => {
    using project = new Project();
    const r = { destinationId: 4, part: 0, mode: 'none', ...change };
    expect(() => project.setPartRig(r as never)).toThrow(message);
    expect(project.partRig({ destinationId: 4, part: 0 })).toBeNull();
  });

  it('refuses an unknown request key by name', () => {
    using project = new Project();
    expect(() =>
      project.setPartRig({ destinationId: 4, part: 0, mode: 'none', extra: 1 } as never),
    ).toThrow(/unknown key extra \(expected /);
    expect(() => project.partRig({ destinationId: 4, part: 0, extra: 1 } as never)).toThrow(
      /unknown key extra \(expected /,
    );
  });

  it('takes only a request object on the engine', () => {
    const engine = new RealtimeEngine({ sampleRate: 48000, maxBlockSize: 128 });
    engine.setBuiltinInstrument(3, {});
    expect(codeNameOf(() => engine.setPartRig({ destinationId: 3, part: 0, mode: 'none' }))).toBe(
      'NotSupported',
    );
    engine.setSynthInstrument(3, {});
    expect(() =>
      engine.setPartRig({ destinationId: 3, part: 0, mode: 'chain', inserts: chain }),
    ).not.toThrow();
    expect(() =>
      engine.setPartRig({ destinationId: 3, part: 0, mode: 'none', inserts: chain }),
    ).toThrow();
    // A number is not a mode, a stray key is refused by name, and params are an object.
    expect(() => engine.setPartRig({ destinationId: 3, part: 0, mode: 1 as never })).toThrow(
      TypeError,
    );
    expect(() =>
      engine.setPartRig({ destinationId: 3, part: 0, mode: 'bank', extra: 1 } as never),
    ).toThrow(/unknown key extra \(expected /);
    expect(() =>
      engine.setPartRig({
        destinationId: 3,
        part: 0,
        mode: 'chain',
        inserts: [{ processor: 'saturation.overdrive', params: '{}' as never }],
      }),
    ).toThrow(TypeError);
    // @ts-expect-error the positional form is gone.
    expect(() => engine.setPartRig(3, 0, 'none')).toThrow(TypeError);
    engine.destroy();
  });
});
