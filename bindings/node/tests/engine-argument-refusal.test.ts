import { describe, expect, it } from 'vitest';
import type { MixSceneStrip } from '../src/index.js';
import { ErrorCode, RealtimeEngine, SonareError } from '../src/index.js';
import { expectRefusal } from './_helpers.js';

/** First strip / bus of a scene document: the object the engine strip setters take. */
const sceneStrip = (json: string): MixSceneStrip => JSON.parse(json).strips[0];

const UNKNOWN_ID = 987654;

/** Runs `body` against a fresh engine with one track lane and one bus, then destroys it. */
function withEngine(body: (engine: RealtimeEngine) => void): void {
  const engine = new RealtimeEngine({ sampleRate: 48000, maxBlockSize: 256 });
  try {
    engine.setTrackBuses([{ busId: 1, gainDb: 0 }]);
    engine.setTrackLanes([{ trackId: 10 }]);
    body(engine);
  } finally {
    engine.destroy();
  }
}

describe('RealtimeEngine value refusals are RangeError, not SonareError', () => {
  const cases: Array<[string, (e: RealtimeEngine) => void]> = [
    [
      'setSynthInstrument with an unknown engineParams key',
      (e) =>
        e.setSynthInstrument(0, { engineMode: 'bowedString', engineParams: { noSuchField: 1 } }),
    ],
    ['setTrackSends on an unknown track', (e) => e.setTrackSends(UNKNOWN_ID, [])],
    ['setTrackOutputBus on an unknown bus', (e) => e.setTrackOutputBus(10, UNKNOWN_ID)],
    ['setBusStripPan on an unknown bus', (e) => e.setBusPan(UNKNOWN_ID, 0)],
    ['setBusStripPan with a non-finite value', (e) => e.setBusPan(1, Number.NaN)],
    [
      'setTrackStripInsertBypassed past the insert count',
      (e) => {
        e.setStrip(
          10,
          sceneStrip('{"version":2,"strips":[{"id":"s"}],"buses":[],"connections":[]}'),
        );
        e.setStripInsertBypassed(10, 7, { bypassed: true });
      },
    ],
    ['sampleAtPpq with a non-finite position', (e) => e.sampleAtPpq(Number.NaN)],
  ];

  it.each(cases)('%s', (_name, invoke) => {
    withEngine((engine) => {
      let caught: unknown;
      try {
        invoke(engine);
      } catch (error) {
        caught = error;
      }
      expect(caught).toBeInstanceOf(RangeError);
      expect(caught).not.toBeInstanceOf(SonareError);
      expect((caught as Error).message.length).toBeGreaterThan(0);
    });
  });

  it('carries the C detail in the message', () => {
    withEngine((engine) => {
      expect(() => engine.setTrackSends(UNKNOWN_ID, [])).toThrow(
        new RangeError(
          'RealtimeEngine.setTrackSends: trackId must be the id of a track in this engine',
        ),
      );
    });
  });
});

describe('RealtimeEngine state refusals stay coded SonareError', () => {
  it('master strip setters before any master strip is configured are InvalidState', () => {
    withEngine((engine) => {
      expect(() => engine.setStripInsertBypassed('master', 0, { bypassed: true })).toThrow(
        expect.objectContaining({ code: ErrorCode.InvalidState }),
      );
      expect(() => engine.setStripEqBand('master', 0, { type: 'peak', enabled: true })).toThrow(
        expect.objectContaining({ code: ErrorCode.InvalidState }),
      );
      expect(() => engine.resolveStripInsertAutomationId('master', 0, 'thresholdDb')).toThrow(
        expect.objectContaining({ code: ErrorCode.InvalidState }),
      );
    });
  });

  it('an unknown strip key is a document content defect, coded InvalidFormat', () => {
    withEngine((engine) => {
      expect(() =>
        engine.setStrip(
          10,
          sceneStrip(
            '{"version":2,"strips":[{"id":"s","faderDB":-3}],"buses":[],"connections":[]}',
          ),
        ),
      ).toThrow(expect.objectContaining({ code: ErrorCode.InvalidFormat }));
    });
  });

  it('a strip that is not an object is a TypeError, not a coded error', () => {
    withEngine((engine) => {
      expect(() => engine.setStrip(10, '{"strips":[]}' as never)).toThrow(TypeError);
      expect(() => engine.setBusStrip(1, null as never)).toThrow(TypeError);
      // Digit-only strings are not track ids: only a number or 'master' addresses a strip.
      expect(() => engine.setStrip('10', {} as never)).toThrow(RangeError);
      expect(() => engine.setStripPan('master', 0)).toThrow(RangeError);
    });
  });

  it('a destroyed engine refuses with InvalidState', () => {
    const engine = new RealtimeEngine({ sampleRate: 48000, maxBlockSize: 256 });
    engine.destroy();
    expect(() => engine.tailSamples).toThrow(
      expect.objectContaining({ code: ErrorCode.InvalidState }),
    );
    expect(() => engine.setBusPan(1, 0)).toThrow(
      expect.objectContaining({ code: ErrorCode.InvalidState }),
    );
  });
});

describe('RealtimeEngine strip controls refuse an omitted required argument', () => {
  it('names sceneJson when it is missing', () => {
    withEngine((engine) => {
      const native = (engine as unknown as { native: Record<string, (...a: unknown[]) => void> })
        .native;
      expectRefusal(() => native.setMasterStripJson(undefined), TypeError, /sceneJson/);
      expectRefusal(() => native.setTrackStripJson(10), TypeError, /sceneJson/);
    });
  });

  it('refuses a missing bypassed flag and leaves the insert state alone', () => {
    withEngine((engine) => {
      engine.setStrip(
        10,
        sceneStrip('{"version":2,"strips":[{"id":"s"}],"buses":[],"connections":[]}'),
      );
      const native = (engine as unknown as { native: Record<string, (...a: unknown[]) => void> })
        .native;
      expectRefusal(() => native.setTrackStripInsertBypassed(10, 0), TypeError, /bypassed/);
    });
  });
});

describe('an instrument config that repeats the destination id', () => {
  // RED WHEN: a destinationId inside the config is read, dropped, or refused in other words than
  // the WASM package uses.
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
