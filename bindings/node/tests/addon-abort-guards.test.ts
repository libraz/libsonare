/**
 * The addon is built with NAPI_DISABLE_CPP_EXCEPTIONS, so a failed typed read
 * (`value.As<Napi::Number>().DoubleValue()` on a missing field) does not raise a
 * C++ exception — it leaves a pending JS exception and returns a dummy value.
 * Two consequences make bad input lethal rather than merely wrong:
 *
 *  1. A second N-API throw raised while an exception is already pending is a
 *     `FATAL ERROR ... napi_throw` abort (exit 134). An entry point that keeps
 *     parsing after the first bad field therefore kills the whole process,
 *     straight through any `try`/`catch`, with no stack trace and no chance to
 *     report the error.
 *  2. A C++ exception thrown inside the callback (a `std::length_error` from
 *     `std::vector<T> v(n)` after a negative count wrapped to SIZE_MAX)
 *     terminates the process for the same reason.
 *
 * These tests pin both exits shut: every covered entry point must convert the
 * FIRST offending field into exactly one catchable `TypeError` (or `RangeError`
 * for an out-of-domain count) and return, and the process must still be alive
 * afterwards. The suite runs the whole matrix in a child process too, so
 * "survived" is asserted against a real exit code and not just against the test
 * runner happening to continue.
 */

import { spawnSync } from 'node:child_process';
import { describe, expect, it } from 'vitest';
import { addon } from '../src/native.js';

const SR = 48000;
const BLOCK = 128;

interface NativeEngine {
  destroy(): void;
  process(channels: Float32Array[]): unknown;
  setGraph(spec: unknown): void;
  setClips(clips: unknown): void;
  setTrackLanes(lanes: unknown): void;
  setTrackBuses(buses: unknown): void;
  setTempoSegments(segments: unknown): void;
  setTimeSignatureSegments(segments: unknown): void;
  graphNodeCount(): number;
  drainTelemetry(maxRecords?: unknown): unknown[];
  drainMeterTelemetry(maxRecords?: unknown): unknown[];
  drainMeterTelemetryWide(maxRecords?: unknown): unknown[];
  drainScopeTelemetry(maxRecords?: unknown): unknown[];
}

interface NativeProject {
  destroy(): void;
  addMidiClip(startPpq: number, lengthPpq: number): { trackId: number; clipId: number };
  trackCount(): number;
  setTempoSegments(segments: unknown): void;
  setTimeSignatures(segments: unknown): void;
  setMidiEvents(clipId: number, events: unknown): void;
  setWarpMap(map: unknown): void;
  addAutomationLane(trackId: number, desc: unknown): number;
  editAutomationLane(trackId: number, targetParamId: number, desc: unknown): void;
}

/**
 * These tests drive the native addon directly rather than the TypeScript
 * facade: several facades (`Project.setMidiEvents`, `Project.addAutomationLane`)
 * validate the same fields in JS first, so a facade-level call would never reach
 * the native guard under test.
 */
function withEngine<T>(body: (engine: NativeEngine) => T): T {
  const engine = new addon.RealtimeEngine(SR, BLOCK) as NativeEngine;
  try {
    return body(engine);
  } finally {
    engine.destroy();
  }
}

function withProject<T>(body: (project: NativeProject) => T): T {
  const project = new addon.Project() as NativeProject;
  try {
    return body(project);
  } finally {
    project.destroy();
  }
}

/**
 * One covered entry point. `missingRequired` omits a required field; `badOptional`
 * feeds a wrong-typed OPTIONAL field to more than one array element, which is the
 * shape that used to abort — the first element left a pending exception and the
 * second element's read threw on top of it.
 */
interface AbortGuardCase {
  name: string;
  missingRequired: Array<{ field: string; call: () => void }>;
  badOptional?: () => void;
}

const samples = (n = 4): Float32Array => new Float32Array(n);

const CASES: AbortGuardCase[] = [
  {
    name: 'RealtimeEngine.setGraph',
    missingRequired: [
      {
        field: 'nodes[].id',
        call: () =>
          withEngine((e) =>
            e.setGraph({ nodes: [{}], connections: [], inputNode: 'a', outputNode: 'a' }),
          ),
      },
      {
        field: 'connections[].sourceNode',
        call: () =>
          withEngine((e) =>
            e.setGraph({
              nodes: [{ id: 'a' }],
              connections: [{ destNode: 'a', sourcePort: 0, destPort: 0 }],
              inputNode: 'a',
              outputNode: 'a',
            }),
          ),
      },
      {
        field: 'inputNode',
        call: () => withEngine((e) => e.setGraph({ nodes: [{ id: 'a' }], connections: [] })),
      },
    ],
    badOptional: () =>
      withEngine((e) =>
        e.setGraph({
          nodes: [
            { id: 'a', gainDb: {} },
            { id: 'b', gainDb: {} },
          ],
          connections: [],
          inputNode: 'a',
          outputNode: 'a',
        }),
      ),
  },
  {
    name: 'RealtimeEngine.setClips',
    missingRequired: [
      { field: 'id', call: () => withEngine((e) => e.setClips([{ channels: [samples()] }])) },
      {
        field: 'startPpq',
        call: () => withEngine((e) => e.setClips([{ id: 1, channels: [samples()] }])),
      },
      {
        field: 'warpAnchors[].sourceSample',
        call: () =>
          withEngine((e) =>
            e.setClips([
              {
                id: 1,
                startPpq: 0,
                channels: [samples()],
                warpMode: 'tempo-sync',
                warpAnchors: [{ warpSample: 0 }],
              },
            ]),
          ),
      },
    ],
    badOptional: () =>
      withEngine((e) =>
        e.setClips([
          { id: 1, startPpq: 0, gain: {}, channels: [samples()] },
          { id: 2, startPpq: 0, gain: {}, channels: [samples()] },
        ]),
      ),
  },
  {
    name: 'RealtimeEngine.setTrackLanes',
    missingRequired: [
      { field: 'trackId', call: () => withEngine((e) => e.setTrackLanes([{}])) },
      {
        field: 'sends[].busId',
        call: () => withEngine((e) => e.setTrackLanes([{ trackId: 1, sends: [{}] }])),
      },
    ],
    badOptional: () =>
      withEngine((e) =>
        e.setTrackLanes([
          { trackId: 1, outputBusId: {} },
          { trackId: 2, outputBusId: {} },
        ]),
      ),
  },
  {
    name: 'RealtimeEngine.setTrackBuses',
    missingRequired: [{ field: 'busId', call: () => withEngine((e) => e.setTrackBuses([{}])) }],
    badOptional: () =>
      withEngine((e) =>
        e.setTrackBuses([
          { busId: 1, gainDb: {} },
          { busId: 2, gainDb: {} },
        ]),
      ),
  },
  {
    name: 'RealtimeEngine.setTempoSegments',
    missingRequired: [
      { field: 'bpm', call: () => withEngine((e) => e.setTempoSegments([{ startPpq: 0 }])) },
      { field: 'startPpq', call: () => withEngine((e) => e.setTempoSegments([{ bpm: 120 }])) },
    ],
    badOptional: () =>
      withEngine((e) =>
        e.setTempoSegments([
          { startPpq: 0, bpm: 120, endBpm: {} },
          { startPpq: 4, bpm: 130, endBpm: {} },
        ]),
      ),
  },
  {
    name: 'RealtimeEngine.setTimeSignatureSegments',
    missingRequired: [
      {
        field: 'denominator',
        call: () => withEngine((e) => e.setTimeSignatureSegments([{ startPpq: 0, numerator: 4 }])),
      },
      {
        field: 'startPpq',
        call: () =>
          withEngine((e) => e.setTimeSignatureSegments([{ numerator: 4, denominator: 4 }])),
      },
    ],
    badOptional: () =>
      withEngine((e) =>
        e.setTimeSignatureSegments([
          { startPpq: 0, numerator: {}, denominator: 4 },
          { startPpq: 4, numerator: {}, denominator: 4 },
        ]),
      ),
  },
  {
    name: 'Project.setTempoSegments',
    missingRequired: [
      { field: 'bpm', call: () => withProject((p) => p.setTempoSegments([{ startPpq: 0 }])) },
      { field: 'startPpq', call: () => withProject((p) => p.setTempoSegments([{ bpm: 120 }])) },
    ],
    badOptional: () =>
      withProject((p) =>
        p.setTempoSegments([
          { startPpq: 0, bpm: 120, endBpm: {} },
          { startPpq: 4, bpm: 130, endBpm: {} },
        ]),
      ),
  },
  {
    name: 'Project.setTimeSignatures',
    missingRequired: [
      {
        field: 'denominator',
        call: () => withProject((p) => p.setTimeSignatures([{ startPpq: 0, numerator: 4 }])),
      },
      {
        field: 'startPpq',
        call: () => withProject((p) => p.setTimeSignatures([{ numerator: 4, denominator: 4 }])),
      },
    ],
  },
  {
    name: 'Project.setMidiEvents',
    missingRequired: [
      {
        field: 'data0',
        call: () =>
          withProject((p) => {
            const { clipId } = p.addMidiClip(0, 4);
            p.setMidiEvents(clipId, [{ ppq: 0 }]);
          }),
      },
      {
        field: 'tuple[2]',
        call: () =>
          withProject((p) => {
            const { clipId } = p.addMidiClip(0, 4);
            p.setMidiEvents(clipId, [[0, 1]]);
          }),
      },
    ],
  },
  {
    name: 'Project.addAutomationLane',
    missingRequired: [
      {
        field: 'points[].value',
        call: () =>
          withProject((p) => {
            const { trackId } = p.addMidiClip(0, 4);
            p.addAutomationLane(trackId, { targetParamId: 1, points: [{ ppq: 0 }] });
          }),
      },
    ],
    badOptional: () =>
      withProject((p) => {
        const { trackId } = p.addMidiClip(0, 4);
        p.addAutomationLane(trackId, {
          targetParamId: 1,
          points: [
            { ppq: 0, value: 0, curve: {} },
            { ppq: 1, value: 1, curve: {} },
          ],
        });
      }),
  },
  {
    name: 'Project.editAutomationLane',
    missingRequired: [
      {
        field: 'points[].ppq',
        call: () =>
          withProject((p) => {
            const { trackId } = p.addMidiClip(0, 4);
            p.addAutomationLane(trackId, { targetParamId: 1, points: [{ ppq: 0, value: 0.5 }] });
            p.editAutomationLane(trackId, 1, { targetParamId: 1, points: [{ value: 0.5 }] });
          }),
      },
    ],
  },
  {
    name: 'Project.setWarpMap',
    missingRequired: [
      { field: 'id', call: () => withProject((p) => p.setWarpMap({ anchors: [] })) },
      {
        field: 'anchors[].sourceSample',
        call: () => withProject((p) => p.setWarpMap({ id: 1, anchors: [{ warpSample: 0 }] })),
      },
    ],
  },
];

/** Proves the process is still usable, not merely that no exception escaped. */
function expectNativeStillWorks(): void {
  expect(withEngine((engine) => engine.graphNodeCount())).toBe(0);
  expect(withProject((project) => project.trackCount())).toBe(0);
}

describe('addon object readers stop at the first bad field', () => {
  it('covers every entry point named by the finding', () => {
    // A vacuous table would make every assertion below pass by iterating
    // nothing, so pin the shape of the table itself.
    expect(CASES.length).toBeGreaterThanOrEqual(9);
    expect(CASES.every((entry) => entry.missingRequired.length > 0)).toBe(true);
  });

  for (const { name, missingRequired, badOptional } of CASES) {
    for (const { field, call } of missingRequired) {
      it(`${name}: a missing ${field} throws a catchable TypeError`, () => {
        expect(call).toThrow(TypeError);
        expectNativeStillWorks();
      });
    }

    if (badOptional !== undefined) {
      it(`${name}: a wrong-typed optional field on several entries throws once`, () => {
        expect(badOptional).toThrow(TypeError);
        expectNativeStillWorks();
      });
    }
  }
});

const DRAINS = [
  'drainTelemetry',
  'drainMeterTelemetry',
  'drainMeterTelemetryWide',
  'drainScopeTelemetry',
] as const;

describe('telemetry drains bound their allocation by a validated budget', () => {
  for (const drain of DRAINS) {
    it(`${drain}: rejects a negative maxRecords with a RangeError`, () => {
      expect(() => withEngine((engine) => engine[drain](-1))).toThrow(RangeError);
      expectNativeStillWorks();
    });

    it(`${drain}: rejects a non-integer maxRecords with a RangeError`, () => {
      expect(() => withEngine((engine) => engine[drain](1.5))).toThrow(RangeError);
      expect(() => withEngine((engine) => engine[drain](Number.NaN))).toThrow(RangeError);
      expect(() => withEngine((engine) => engine[drain](Number.POSITIVE_INFINITY))).toThrow(
        RangeError,
      );
      expectNativeStillWorks();
    });

    it(`${drain}: rejects a non-number maxRecords with a TypeError`, () => {
      expect(() => withEngine((engine) => engine[drain]('1024'))).toThrow(TypeError);
      expectNativeStillWorks();
    });

    it(`${drain}: returns an empty array for maxRecords 0`, () => {
      expect(withEngine((engine) => engine[drain](0))).toEqual([]);
    });

    it(`${drain}: survives a 2**31 maxRecords without a proportional allocation`, () => {
      const before = process.memoryUsage().rss;
      const drained = withEngine((engine) => engine[drain](2 ** 31));
      const grew = process.memoryUsage().rss - before;
      expect(Array.isArray(drained)).toBe(true);
      // A budget-sized buffer would be 2**31 records: hundreds of gigabytes for
      // the wide meter record, which is what used to kill the process. The
      // working buffer is capped at a fixed chunk instead, so nothing near the
      // budget is reserved.
      expect(grew).toBeLessThan(64 * 1024 * 1024);
      expectNativeStillWorks();
    });

    it(`${drain}: treats an omitted maxRecords as the default budget`, () => {
      expect(withEngine((engine) => engine[drain]())).toEqual([]);
      expect(withEngine((engine) => engine[drain](undefined))).toEqual([]);
    });
  }

  it('drainTelemetry honours the budget exactly and drains past one chunk', () => {
    withEngine((engine) => {
      const left = new Float32Array(BLOCK);
      const right = new Float32Array(BLOCK);
      // More than the 256-record internal chunk, so a huge budget has to loop.
      const blocks = 600;
      for (let i = 0; i < blocks; i++) {
        engine.process([left, right]);
      }
      expect(engine.drainTelemetry(2 ** 31)).toHaveLength(blocks);

      for (let i = 0; i < blocks; i++) {
        engine.process([left, right]);
      }
      expect(engine.drainTelemetry(1)).toHaveLength(1);
      expect(engine.drainTelemetry(5)).toHaveLength(5);
      expect(engine.drainTelemetry(2 ** 31)).toHaveLength(blocks - 6);
      expect(engine.drainTelemetry(2 ** 31)).toHaveLength(0);
    });
  });
});

describe('the hostile-input matrix leaves the process alive', () => {
  it('detects an aborting child, so the exit-code assertion is not blind', () => {
    const result = spawnSync(process.execPath, ['-e', 'process.abort()'], { encoding: 'utf8' });
    expect(result.status === 134 || result.signal === 'SIGABRT').toBe(true);
  });

  it('exits 0 in a child process (not 134)', () => {
    // vitest reports a dead worker, but only as an opaque failure. Asserting a
    // real exit code is what distinguishes "threw and recovered" from "aborted".
    const script = `
      const addon = require(${JSON.stringify(new URL('../build/Release/sonare-node.node', import.meta.url).pathname)});
      const swallow = (fn) => { try { fn(); } catch { /* a catchable error is the point */ } };
      for (let round = 0; round < 2; round++) {
        const e = new addon.RealtimeEngine(${SR}, ${BLOCK});
        swallow(() => e.setGraph({ nodes: [{}, {}], connections: [{}, {}] }));
        swallow(() => e.setGraph({ nodes: [{ id: {} }, { id: {} }], connections: [], inputNode: {}, outputNode: {} }));
        swallow(() => e.setClips([{}, {}]));
        swallow(() => e.setTrackLanes([{}, {}]));
        swallow(() => e.setTrackBuses([{}, {}]));
        swallow(() => e.setTempoSegments([{}, {}]));
        swallow(() => e.setTimeSignatureSegments([{}, {}]));
        for (const drain of ${JSON.stringify(DRAINS)}) {
          for (const arg of [-1, -2147483648, 1.5, Number.NaN, Number.POSITIVE_INFINITY, 'x', 0, 2 ** 31, 2 ** 53]) {
            swallow(() => e[drain](arg));
          }
        }
        e.destroy();
        const p = new addon.Project();
        swallow(() => p.setTempoSegments([{}, {}]));
        swallow(() => p.setTimeSignatures([{}, {}]));
        swallow(() => p.setWarpMap({ anchors: [{}, {}] }));
        const { trackId, clipId } = p.addMidiClip(0, 4);
        swallow(() => p.setMidiEvents(clipId, [{}, {}]));
        swallow(() => p.setMidiEvents(clipId, [[], []]));
        swallow(() => p.addAutomationLane(trackId, { targetParamId: 1, points: [{}, {}] }));
        swallow(() => p.editAutomationLane(trackId, 1, { targetParamId: 1, points: [{}, {}] }));
        p.destroy();
      }
      process.exit(0);
    `;
    const result = spawnSync(process.execPath, ['-e', script], { encoding: 'utf8' });
    expect(
      { status: result.status, signal: result.signal },
      `child stderr:\n${result.stderr}`,
    ).toEqual({ status: 0, signal: null });
  });
});
