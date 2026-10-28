/**
 * The error class of a refused RealtimeEngine call.
 *
 * A call refused for its own argument -- an out-of-range value, an unknown id or
 * name, a malformed patch or strip -- throws a `RangeError`, never a coded
 * `SonareError`. A call refused because of state earlier calls left keeps the
 * coded `InvalidState`, as does a call on a destroyed engine.
 */

import { beforeAll, describe, expect, it } from 'vitest';
import { ErrorCode, init, isSonareError, RealtimeEngine } from '../dist/index.js';
import { expectRefusalOf } from './_helpers';

const LIMITER_MASTER = JSON.stringify({
  version: 1,
  strips: [
    {
      id: 'master',
      inserts: [
        {
          slot: 'pre',
          processor: 'dynamics.limiter',
          params: JSON.stringify({ thresholdDb: 0, lookaheadMs: 0, releaseMs: 50 }),
        },
      ],
    },
  ],
  buses: [],
});

function withEngine(body: (engine: RealtimeEngine) => void): void {
  const engine = new RealtimeEngine(48000, 128);
  try {
    body(engine);
  } finally {
    engine.destroy();
  }
}

function thrown(call: () => unknown): unknown {
  try {
    call();
  } catch (error) {
    return error;
  }
  throw new Error('expected the call to throw');
}

function expectInvalidState(call: () => unknown): void {
  const error = thrown(call);
  expect(isSonareError(error)).toBe(true);
  expect(error).not.toBeInstanceOf(RangeError);
  expect((error as { code: number }).code).toBe(ErrorCode.InvalidState);
}

beforeAll(async () => {
  await init();
});

describe('RealtimeEngine value refusals are RangeErrors', () => {
  it('refuses unknown mixer, clip and marker ids', () => {
    withEngine((engine) => {
      engine.setTrackLanes([10]);
      for (const call of [
        () => engine.setTrackStripPan(99, 0),
        () => engine.setTrackLanes([{ trackId: 10 }, { trackId: 10 }]),
        () => engine.removeClip(42),
        () => engine.marker(999),
        () => engine.setParameter(0x4d580101, -6),
      ]) {
        const error = expectRefusalOf(call);
        expect(isSonareError(error)).toBe(false);
      }
    });
  });

  it('refuses a malformed patch and strip, including what the library reader refuses', () => {
    withEngine((engine) => {
      engine.setTrackLanes([10]);
      const patch = thrown(() =>
        engine.setSynthInstrument(
          { engineMode: 'bowed-string', engineParams: { noSuchKey: 1 } },
          8,
        ),
      );
      expect(patch).toBeInstanceOf(RangeError);
      expect(isSonareError(patch)).toBe(false);
      expect((patch as Error).message).toContain('noSuchKey');
      const typo = '{"version":1,"strips":[{"id":"s","faderDB":-3}],"buses":[]}';
      // A document content defect is the loader's InvalidFormat, a coded SonareError.
      const strip = thrown(() => engine.setTrackStripJson(10, typo));
      expect(isSonareError(strip)).toBe(true);
      expect((strip as { code: number }).code).toBe(ErrorCode.InvalidFormat);
      expect((strip as Error).message).toContain("unknown strip key 'strips[0].faderDB'");
    });
  });

  it('refuses a destination with no instrument and a binding with no axis', () => {
    withEngine((engine) => {
      expectRefusalOf(() => engine.setPartRig(0, 0, 'none'));
      engine.setSynthInstrument('clarinet', 0);
      expectRefusalOf(() =>
        engine.bindController(0, { input: 'control-change', index: 2, axis: 'none' }),
      );
      // The control: the same binding with an axis is accepted.
      expect(() =>
        engine.bindController(0, { input: 'control-change', index: 2, axis: 'excitation' }),
      ).not.toThrow();
    });
  });
});

describe('RealtimeEngine state refusals stay a coded InvalidState', () => {
  it('refuses master-strip edits before a master strip is configured', () => {
    withEngine((engine) => {
      expectInvalidState(() => engine.setMasterStripInsertBypassed(0, true));
      expectInvalidState(() => engine.setMasterStripEqBand(0, { type: 'Peak' }));
      expectInvalidState(() => engine.resolveMasterInsertAutomationId(0, 'thresholdDb'));
      // Once a master strip exists, the same setter refuses a bad index as the argument.
      engine.setMasterStripJson(LIMITER_MASTER);
      expect(() => engine.setMasterStripInsertBypassed(0, true)).not.toThrow();
      expectRefusalOf(() => engine.setMasterStripInsertBypassed(99, true));
    });
  });

  it('refuses a call on a destroyed engine', () => {
    const engine = new RealtimeEngine(48000, 128);
    engine.destroy();
    expectInvalidState(() => engine.marker(1));
  });
});
