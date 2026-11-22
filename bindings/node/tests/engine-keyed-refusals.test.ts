import { describe, expect, it } from 'vitest';
import { RealtimeEngine } from '../src/index.js';

function withEngine(body: (engine: RealtimeEngine) => void): void {
  const engine = new RealtimeEngine({ sampleRate: 48000, maxBlockSize: 128 });
  try {
    body(engine);
  } finally {
    engine.destroy();
  }
}

const CURVES = 'linear, exponential, hold, sCurve';
const clip = { id: 1, channels: [new Float32Array(16)], startPpq: 0 };

describe("an engine refusal names the field by its path in the caller's argument", () => {
  it.each([
    [
      'setClips',
      (e: RealtimeEngine) => e.setClips([{ ...clip, warpMode: 'bogus' as never }]),
      'clips[0].warpMode must be one of off, repitch, tempoSync, timeStretch',
    ],
    [
      'upsertClip',
      (e: RealtimeEngine) => e.upsertClip({ ...clip, warpMode: 'bogus' as never }),
      'clip.warpMode must be one of off, repitch, tempoSync, timeStretch',
    ],
    [
      'setTrackLanes',
      (e: RealtimeEngine) =>
        e.setTrackLanes([{ trackId: 1, sends: [{ busId: 2, sendTiming: 'bogus' as never }] }]),
      'lanes[0].sends[0].sendTiming must be one of postFader, preFader',
    ],
    [
      'setAutomationLane',
      (e: RealtimeEngine) =>
        e.setAutomationLane(1, [
          { ppq: 0, value: 0 },
          { ppq: 1, value: 1, curveToNext: 'bogus' as never },
        ]),
      `points[1].curveToNext must be one of ${CURVES}`,
    ],
    [
      'setMarkers',
      (e: RealtimeEngine) => e.setMarkers([{ ppq: 0, kind: 'bogus' as never }]),
      'markers[0].kind must be one of marker, text, lyric, cuePoint, keySignature',
    ],
    [
      'addParameter',
      (e: RealtimeEngine) =>
        e.addParameter({ id: 1, name: 'x', defaultCurve: 'bogus' as never } as never),
      `info.defaultCurve must be one of ${CURVES}`,
    ],
    [
      'setTrackMonitorMode',
      (e: RealtimeEngine) => e.setTrackMonitorMode(99, 'off'),
      'trackId must be the id of a track in this engine',
    ],
  ])('%s', (method, call, text) => {
    withEngine((engine) => {
      expect(() => call(engine)).toThrow(new RangeError(`RealtimeEngine.${method}: ${text}`));
    });
  });

  it('refuses a wrong-typed token by listing the set under the same path', () => {
    withEngine((engine) => {
      expect(() => engine.upsertClip({ ...clip, warpMode: 3 as never })).toThrow(
        new TypeError(
          'RealtimeEngine.upsertClip: clip.warpMode must be one of off, repitch, tempoSync, timeStretch',
        ),
      );
    });
  });
});

describe('an automation point refuses a key it does not declare', () => {
  const points = [
    { ppq: 0, value: 0 },
    { ppq: 1, value: 1 },
  ];

  it('names the key by its path and lists the declared fields', () => {
    withEngine((engine) => {
      expect(() =>
        engine.setAutomationLane(1, [points[0], { ...points[1], curve: 'hold' } as never]),
      ).toThrow(
        new TypeError(
          'RealtimeEngine.setAutomationLane: unknown key points[1].curve (expected curveToNext, ppq, value)',
        ),
      );
    });
  });

  it('accepts the same points without the stray key', () => {
    withEngine((engine) => {
      engine.addParameter({ id: 1, name: 'p', minValue: 0, maxValue: 1, defaultValue: 0 });
      engine.setAutomationLane(1, [points[0], { ...points[1], curveToNext: 'hold' }]);
      expect(engine.automationLaneCount).toBe(1);
    });
  });
});
