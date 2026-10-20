import { describe, expect, it } from 'vitest';
import { ErrorCode, RealtimeEngine, SonareError } from '../src/index.js';

const UNKNOWN_ID = 987654;

/** Runs `body` against a fresh engine with one track lane and one bus, then destroys it. */
function withEngine(body: (engine: RealtimeEngine) => void): void {
  const engine = new RealtimeEngine(48000, 256);
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
        e.setSynthInstrument({ engineMode: 'bowed-string', engineParams: { noSuchField: 1 } }, 0),
    ],
    ['setTrackSends on an unknown track', (e) => e.setTrackSends(UNKNOWN_ID, [])],
    ['setTrackOutputBus on an unknown bus', (e) => e.setTrackOutputBus(10, UNKNOWN_ID)],
    ['setBusStripPan on an unknown bus', (e) => e.setBusStripPan(UNKNOWN_ID, 0)],
    ['setBusStripPan with a non-finite value', (e) => e.setBusStripPan(1, Number.NaN)],
    [
      'setTrackStripJson with an unknown strip key',
      (e) =>
        e.setTrackStripJson(
          10,
          '{"version":1,"strips":[{"id":"s","faderDB":-3}],"buses":[],"connections":[]}',
        ),
    ],
    [
      'setTrackStripInsertBypassed past the insert count',
      (e) => {
        e.setTrackStripJson(10, '{"version":1,"strips":[{"id":"s"}],"buses":[],"connections":[]}');
        e.setTrackStripInsertBypassed(10, 7, true);
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
        new RangeError(`unknown track id ${UNKNOWN_ID}`),
      );
    });
  });
});

describe('RealtimeEngine state refusals stay coded SonareError', () => {
  it('master strip setters before any master strip is configured are InvalidState', () => {
    withEngine((engine) => {
      expect(() => engine.setMasterStripInsertBypassed(0, true)).toThrow(
        expect.objectContaining({ code: ErrorCode.InvalidState }),
      );
      expect(() => engine.setMasterStripEqBand(0, { type: 'Peak', enabled: true })).toThrow(
        expect.objectContaining({ code: ErrorCode.InvalidState }),
      );
    });
  });

  it('a malformed strip JSON document keeps its InvalidFormat code', () => {
    withEngine((engine) => {
      expect(() => engine.setTrackStripJson(10, '{bad json')).toThrow(
        expect.objectContaining({ code: ErrorCode.InvalidFormat }),
      );
    });
  });

  it('a destroyed engine refuses with InvalidState', () => {
    const engine = new RealtimeEngine(48000, 256);
    engine.destroy();
    expect(() => engine.tailSamples()).toThrow(
      expect.objectContaining({ code: ErrorCode.InvalidState }),
    );
    expect(() => engine.setBusStripPan(1, 0)).toThrow(
      expect.objectContaining({ code: ErrorCode.InvalidState }),
    );
  });
});
