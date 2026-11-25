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
  version: 2,
  strips: [
    {
      id: 'master',
      inserts: [
        {
          slot: 'pre',
          processor: 'dynamics.limiter',
          params: { thresholdDb: 0, lookaheadMs: 0, releaseMs: 50 },
        },
      ],
    },
  ],
  buses: [],
});

function withEngine(body: (engine: RealtimeEngine) => void): void {
  const engine = new RealtimeEngine({ sampleRate: 48000, maxBlockSize: 128 });
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
  it('names the ref of a missing track strip', () => {
    withEngine((engine) => {
      engine.setTrackLanes([{ trackId: 10 }]);
      engine.setStrip(10, { id: 'track-10' });
      engine.setStripPan(10, 0);
      const error = thrown(() => engine.setStripPan(99, 0));
      expect(error).toBeInstanceOf(RangeError);
      expect((error as Error).message).toBe(
        'RealtimeEngine.setStripPan: ref must be the id of a track in this engine',
      );
    });
  });

  it('preserves the value refusal when a nonzero ref is also missing', () => {
    withEngine((engine) => {
      for (const [call, text] of [
        [() => engine.setStripPan(99, 2), 'setStripPan: pan must be finite and in [-1, 1]'],
        [
          () => engine.setStripDualPan(99, 2, 0),
          'setStripDualPan: leftPan must be finite and in [-1, 1]',
        ],
        [
          () => engine.setStripChannelDelaySamples(99, -1),
          'setStripChannelDelaySamples: delaySamples must be an integer in [0, 192000]',
        ],
        [
          () => engine.setStripInsertParam(99, 0, { name: '', value: 0 }),
          'setStripInsertParam: param.name must not be empty',
        ],
      ] as const) {
        const error = thrown(call);
        expect(error).toBeInstanceOf(RangeError);
        expect((error as Error).message).toBe(`RealtimeEngine.${text}`);
      }
    });
  });

  it('refuses unknown mixer, clip and marker ids', () => {
    withEngine((engine) => {
      engine.setTrackLanes([{ trackId: 10 }]);
      for (const call of [
        () => engine.setStripPan(99, 0),
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
      engine.setTrackLanes([{ trackId: 10 }]);
      const patch = thrown(() =>
        engine.setSynthInstrument(8, {
          engineMode: 'bowedString',
          engineParams: { noSuchKey: 1 },
        }),
      );
      expect(patch).toBeInstanceOf(RangeError);
      expect(isSonareError(patch)).toBe(false);
      expect((patch as Error).message).toContain('noSuchKey');
      const typo = '{"version":2,"strips":[{"id":"s","faderDB":-3}],"buses":[]}';
      // A key refused inside the settings object is a refused argument, not a document error.
      const strip = thrown(() => engine.setStrip(10, JSON.parse(typo).strips[0]));
      expect(strip).toBeInstanceOf(TypeError);
      expect(isSonareError(strip)).toBe(false);
      expect((strip as Error).message).toMatch(
        /^RealtimeEngine\.setStrip: unknown key settings\.faderDB\b/,
      );
    });
  });

  it('refuses a destination with no instrument and a binding with no axis', () => {
    withEngine((engine) => {
      expectRefusalOf(() => engine.setPartRig({ destinationId: 0, part: 0, mode: 'none' }));
      engine.setSynthInstrument(0, { preset: 'clarinet' });
      expectRefusalOf(() =>
        engine.bindController(0, { input: 'controlChange', index: 2, axis: 'none' }),
      );
      // The control: the same binding with an axis is accepted.
      expect(() =>
        engine.bindController(0, { input: 'controlChange', index: 2, axis: 'excitation' }),
      ).not.toThrow();
    });
  });
});

describe('RealtimeEngine state refusals stay a coded InvalidState', () => {
  it('edits the default master strip before any setStrip, refusing an insert it lacks as the argument', () => {
    withEngine((engine) => {
      expect(() => engine.setStripEqBand('master', 0, { type: 'peak' })).not.toThrow();
      // The default master strip carries no insert, so index 0 is the argument at fault.
      expectRefusalOf(() => engine.setStripInsertBypassed('master', 0, { bypassed: true }));
      expectRefusalOf(() => engine.resolveStripInsertAutomationId('master', 0, 'thresholdDb'));
      engine.setStrip('master', JSON.parse(LIMITER_MASTER).strips[0]);
      expect(() => engine.setStripInsertBypassed('master', 0, { bypassed: true })).not.toThrow();
      expectRefusalOf(() => engine.setStripInsertBypassed('master', 99, { bypassed: true }));
    });
  });

  it('refuses a call on a destroyed engine', () => {
    const engine = new RealtimeEngine({ sampleRate: 48000, maxBlockSize: 128 });
    engine.destroy();
    expectInvalidState(() => engine.marker(1));
  });
});

describe('an instrument config that repeats the destination id', () => {
  // RED WHEN: a destinationId inside the config is read, dropped, or refused in other words than
  // the Node package uses.
  it('is a TypeError naming the first argument on every instrument binder', () => {
    const engine = new RealtimeEngine({ sampleRate: 48000, maxBlockSize: 256 });
    try {
      const refusal = (entry: string) =>
        new TypeError(
          `RealtimeEngine.${entry}: destinationId is the first argument; remove it from the config`,
        );
      expect(() =>
        engine.setBuiltinInstrument(0, { destinationId: 1, waveform: 'saw' } as never),
      ).toThrow(refusal('setBuiltinInstrument'));
      expect(() => engine.setSynthInstrument(0, { destinationId: 1 } as never)).toThrow(
        refusal('setSynthInstrument'),
      );
      expect(() => engine.setSf2Instrument(0, { destinationId: 1 } as never)).toThrow(
        refusal('setSf2Instrument'),
      );
    } finally {
      engine.destroy();
    }
  });
});

describe('setMarkers refuses a marker id outside the uint32 ids from 1', () => {
  beforeAll(async () => {
    await init();
  });

  const range = 'RealtimeEngine.setMarkers: markers[0].id must be an integer in [1, 4294967295]';

  it.each([0, -1, 1.5, 2 ** 32, Number.NaN, Number.POSITIVE_INFINITY])('id %s', (id) => {
    withEngine((engine) => {
      expect(() => engine.setMarkers([{ id, ppq: 0 }])).toThrow(new RangeError(range));
    });
  });

  it('refuses a non-number id as a TypeError', () => {
    withEngine((engine) => {
      expect(() => engine.setMarkers([{ id: '1' as never, ppq: 0 }])).toThrow(
        new TypeError('RealtimeEngine.setMarkers: markers[0].id must be an integer'),
      );
    });
  });

  it('refuses a repeated id before reading that marker position', () => {
    withEngine((engine) => {
      expect(() =>
        engine.setMarkers([
          { id: 7, ppq: 0 },
          { id: 7, ppq: -1 },
        ]),
      ).toThrow(
        new RangeError('RealtimeEngine.setMarkers: markers[1].id must be unique across markers'),
      );
    });
  });
});
