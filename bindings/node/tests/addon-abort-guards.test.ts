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

interface NativeCaptureStatus {
  capturedFrames: number;
  overflowCount: number;
  armed: boolean;
  punchEnabled: boolean;
  source: string;
  recordOffsetSamples: number;
}

interface NativeEngine {
  destroy(): void;
  process(channels: Float32Array[]): Float32Array[];
  play(): void;
  seekSample(sample: number): void;
  setGraph(spec: unknown): void;
  setClips(clips: unknown): void;
  setTrackLanes(lanes: unknown): void;
  setTrackBuses(buses: unknown): void;
  setTempoSegments(segments: unknown): void;
  setTimeSignatureSegments(segments: unknown): void;
  graphNodeCount(): number;
  clipCount(): number;
  drainTelemetry(maxRecords?: unknown): unknown[];
  drainMeterTelemetry(maxRecords?: unknown): unknown[];
  drainMeterTelemetryWide(maxRecords?: unknown): unknown[];
  drainScopeTelemetry(maxRecords?: unknown): unknown[];
  setLaneSidechain(trackId: unknown, insertIndex: unknown, sourceTrackId: unknown): void;
  setBusStripJson(busId: unknown, sceneJson: unknown): void;
  setTrackStripJson(trackId: unknown, sceneJson: unknown): void;
  setTrackStripEqBandJson(trackId: unknown, bandIndex: unknown, bandJson: unknown): void;
  setTrackStripInsertBypassed(
    trackId: unknown,
    insertIndex: unknown,
    bypassed: unknown,
    resetOnBypass?: unknown,
  ): void;
  setMasterStripJson(sceneJson: unknown): void;
  setMasterStripEqBandJson(bandIndex: unknown, bandJson: unknown): void;
  setMasterStripInsertBypassed(
    insertIndex: unknown,
    bypassed: unknown,
    resetOnBypass?: unknown,
  ): void;
  setTrackStripInsertParamByName(
    trackId: unknown,
    insertIndex: unknown,
    paramName: unknown,
    value: unknown,
  ): void;
  setMasterStripInsertParamByName(insertIndex: unknown, paramName: unknown, value: unknown): void;
  setBusStripInsertParamByName(
    busId: unknown,
    insertIndex: unknown,
    paramName: unknown,
    value: unknown,
  ): void;
  setBusStripInsertBypassed(
    busId: unknown,
    insertIndex: unknown,
    bypassed: unknown,
    resetOnBypass?: unknown,
  ): void;
  resolveTrackInsertAutomationId(
    trackId: unknown,
    insertIndex: unknown,
    paramName: unknown,
  ): number;
  resolveMasterInsertAutomationId(insertIndex: unknown, paramName: unknown): number;
  resolveBusInsertAutomationId(busId: unknown, insertIndex: unknown, paramName: unknown): number;
  resolveInstrumentAutomationId(destinationId: unknown, paramName: unknown): number;
  setTrackStripPan(trackId: unknown, pan: unknown): void;
  setTrackStripPanLaw(trackId: unknown, panLaw: unknown): void;
  setTrackStripPanMode(trackId: unknown, panMode: unknown): void;
  setTrackStripDualPan(trackId: unknown, leftPan: unknown, rightPan: unknown): void;
  setTrackStripChannelDelaySamples(trackId: unknown, delaySamples: unknown): void;
  createClipPageProvider(numChannels: unknown, numSamples: unknown, pageFrames: unknown): number;
  supplyClipPage(providerId: unknown, pageIndex: unknown, channels: unknown): void;
  clearClipPage(providerId: unknown, pageIndex: unknown): void;
  destroyClipPageProvider(providerId: unknown): void;
  setClipPagePrefetchFrames(frames: unknown): void;
  clipPagePrefetchFrames(): number;
  setCaptureBuffer(channels: unknown): void;
  armCapture(armed?: unknown): void;
  setCapturePunch(startSample: unknown, endSample: unknown, enabled?: unknown): void;
  setCaptureSource(source: unknown): void;
  setRecordOffsetSamples(offsetSamples: unknown): void;
  setInputMonitor(enabled: unknown, gain?: unknown): void;
  captureStatus(): NativeCaptureStatus;
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
 * A track lane, a bus and the master strip, each carrying one `eq.parametric`
 * insert, plus a capture session parked in a NON-default state. Every field the
 * snapshot below reads is deliberately moved off its zero value, so a call that
 * reaches the C ABI with a dummy argument moves the snapshot instead of landing
 * on the value it already had.
 */
const TRACK_ID = 10;
const BUS_ID = 1;
const PREFETCH_FRAMES = 4096;
const RECORD_OFFSET = 64;

const eqInsert = (gainDb: number) => ({
  slot: 'pre',
  processor: 'eq.parametric',
  params: JSON.stringify({
    'band0.type': 1,
    'band0.frequencyHz': 1000,
    'band0.gainDb': gainDb,
    'band0.q': 1,
    'band0.enabled': 1,
  }),
});

const trackStripJson = (gainDb = 0) =>
  JSON.stringify({
    version: 1,
    strips: [{ id: `track-${TRACK_ID}`, inserts: [eqInsert(gainDb)] }],
    buses: [],
    connections: [],
  });

const busStripJson = JSON.stringify({
  version: 1,
  strips: [],
  buses: [{ id: String(BUS_ID), inserts: [eqInsert(0)] }],
  connections: [],
});

const masterStripJson = JSON.stringify({
  version: 1,
  strips: [{ id: 'master', inserts: [eqInsert(0)] }],
  buses: [],
  connections: [],
});

function withConfiguredEngine<T>(body: (engine: NativeEngine) => T): T {
  const engine = new addon.RealtimeEngine(SR, BLOCK) as NativeEngine;
  try {
    engine.setTrackBuses([{ busId: BUS_ID, gainDb: 0, channelLayout: 1 }]);
    engine.setTrackLanes([{ trackId: TRACK_ID, outputBusId: BUS_ID }]);
    engine.setBusStripJson(BUS_ID, busStripJson);
    engine.setTrackStripJson(TRACK_ID, trackStripJson());
    engine.setMasterStripJson(masterStripJson);
    engine.setCaptureBuffer([new Float32Array(BLOCK), new Float32Array(BLOCK)]);
    engine.setCaptureSource('input');
    engine.setRecordOffsetSamples(RECORD_OFFSET);
    engine.setCapturePunch(0, BLOCK, true);
    engine.armCapture(true);
    engine.setClipPagePrefetchFrames(PREFETCH_FRAMES);
    return body(engine);
  } finally {
    engine.destroy();
  }
}

/**
 * Everything about the engine these entry points can move that the addon also
 * exposes a getter for. The strip readbacks are automation-id resolutions: a
 * strip rebuilt from the empty JSON string a failed read used to hand the C ABI
 * loses its insert, so the id collapses to the -1 sentinel.
 */
function engineStateSnapshot(engine: NativeEngine): string {
  return JSON.stringify({
    capture: engine.captureStatus(),
    prefetchFrames: engine.clipPagePrefetchFrames(),
    trackInsertId: engine.resolveTrackInsertAutomationId(TRACK_ID, 0, 'band0.gainDb'),
    busInsertId: engine.resolveBusInsertAutomationId(BUS_ID, 0, 'band0.gainDb'),
    masterInsertId: engine.resolveMasterInsertAutomationId(0, 'band0.gainDb'),
    clipCount: engine.clipCount(),
  });
}

const rms = (block: Float32Array): number =>
  Math.sqrt(block.reduce((sum, value) => sum + value * value, 0) / block.length);

/**
 * One covered entry point. `missingRequired` omits a required field; `badOptional`
 * feeds a wrong-typed OPTIONAL field to more than one array element, which is the
 * shape that used to abort — the first element left a pending exception and the
 * second element's read threw on top of it.
 *
 * `badArguments` covers the other reader family: entry points that read POSITIONAL
 * arguments (`info[i]`) rather than object keys. There the failed typed read
 * yielded a dummy `0` / `""` / `false` that was then handed straight to the C ABI,
 * so the engine moved to the OPPOSITE of what the caller asked for before the
 * error was reported — `armCapture(1)` disarmed, `setTrackStripInsertBypassed(t,
 * i, 1)` un-bypassed. Each entry runs against `withConfiguredEngine` and its
 * observable state is compared either side of the rejected call.
 */
interface AbortGuardCase {
  name: string;
  missingRequired: Array<{ field: string; call: () => void }>;
  badOptional?: () => void;
  badArguments?: Array<{ argument: string; call: (engine: NativeEngine) => void }>;
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
  {
    name: 'RealtimeEngine.setLaneSidechain',
    missingRequired: [],
    badArguments: [
      { argument: 'sourceTrackId', call: (e) => e.setLaneSidechain(TRACK_ID, 0, '1') },
    ],
  },
  {
    name: 'RealtimeEngine.setBusStripJson',
    missingRequired: [],
    badArguments: [{ argument: 'sceneJson', call: (e) => e.setBusStripJson(BUS_ID, 42) }],
  },
  {
    name: 'RealtimeEngine.setTrackStripJson',
    missingRequired: [],
    badArguments: [{ argument: 'sceneJson', call: (e) => e.setTrackStripJson(TRACK_ID, 42) }],
  },
  {
    name: 'RealtimeEngine.setTrackStripEqBandJson',
    missingRequired: [],
    badArguments: [
      { argument: 'bandJson', call: (e) => e.setTrackStripEqBandJson(TRACK_ID, 0, 42) },
      { argument: 'bandIndex', call: (e) => e.setTrackStripEqBandJson(TRACK_ID, '0', '{}') },
    ],
  },
  {
    name: 'RealtimeEngine.setTrackStripInsertBypassed',
    missingRequired: [],
    badArguments: [
      { argument: 'bypassed', call: (e) => e.setTrackStripInsertBypassed(TRACK_ID, 0, 1) },
      {
        argument: 'resetOnBypass',
        call: (e) => e.setTrackStripInsertBypassed(TRACK_ID, 0, true, 1),
      },
    ],
  },
  {
    name: 'RealtimeEngine.setMasterStripJson',
    missingRequired: [],
    badArguments: [{ argument: 'sceneJson', call: (e) => e.setMasterStripJson(42) }],
  },
  {
    name: 'RealtimeEngine.setMasterStripEqBandJson',
    missingRequired: [],
    badArguments: [{ argument: 'bandJson', call: (e) => e.setMasterStripEqBandJson(0, 42) }],
  },
  {
    name: 'RealtimeEngine.setMasterStripInsertBypassed',
    missingRequired: [],
    badArguments: [{ argument: 'bypassed', call: (e) => e.setMasterStripInsertBypassed(0, 1) }],
  },
  {
    name: 'RealtimeEngine.setTrackStripInsertParamByName',
    missingRequired: [],
    badArguments: [
      {
        argument: 'paramName',
        call: (e) => e.setTrackStripInsertParamByName(TRACK_ID, 0, 42, 1),
      },
      {
        argument: 'value',
        call: (e) => e.setTrackStripInsertParamByName(TRACK_ID, 0, 'band0.gainDb', '1'),
      },
    ],
  },
  {
    name: 'RealtimeEngine.setMasterStripInsertParamByName',
    missingRequired: [],
    badArguments: [
      { argument: 'paramName', call: (e) => e.setMasterStripInsertParamByName(0, 42, 1) },
    ],
  },
  {
    name: 'RealtimeEngine.setBusStripInsertParamByName',
    missingRequired: [],
    badArguments: [
      { argument: 'paramName', call: (e) => e.setBusStripInsertParamByName(BUS_ID, 0, 42, 1) },
    ],
  },
  {
    name: 'RealtimeEngine.setBusStripInsertBypassed',
    missingRequired: [],
    badArguments: [
      { argument: 'bypassed', call: (e) => e.setBusStripInsertBypassed(BUS_ID, 0, 1) },
    ],
  },
  {
    name: 'RealtimeEngine.resolveTrackInsertAutomationId',
    missingRequired: [],
    badArguments: [
      { argument: 'paramName', call: (e) => e.resolveTrackInsertAutomationId(TRACK_ID, 0, 42) },
    ],
  },
  {
    name: 'RealtimeEngine.resolveMasterInsertAutomationId',
    missingRequired: [],
    badArguments: [
      { argument: 'paramName', call: (e) => e.resolveMasterInsertAutomationId(0, 42) },
    ],
  },
  {
    name: 'RealtimeEngine.resolveBusInsertAutomationId',
    missingRequired: [],
    badArguments: [
      { argument: 'paramName', call: (e) => e.resolveBusInsertAutomationId(BUS_ID, 0, 42) },
    ],
  },
  {
    name: 'RealtimeEngine.resolveInstrumentAutomationId',
    missingRequired: [],
    badArguments: [{ argument: 'paramName', call: (e) => e.resolveInstrumentAutomationId(1, 42) }],
  },
  {
    name: 'RealtimeEngine.setTrackStripPan',
    missingRequired: [],
    badArguments: [{ argument: 'pan', call: (e) => e.setTrackStripPan(TRACK_ID, '0.5') }],
  },
  {
    name: 'RealtimeEngine.setTrackStripPanLaw',
    missingRequired: [],
    badArguments: [{ argument: 'panLaw', call: (e) => e.setTrackStripPanLaw(TRACK_ID, '3') }],
  },
  {
    name: 'RealtimeEngine.setTrackStripPanMode',
    missingRequired: [],
    badArguments: [{ argument: 'panMode', call: (e) => e.setTrackStripPanMode(TRACK_ID, '2') }],
  },
  {
    name: 'RealtimeEngine.setTrackStripDualPan',
    missingRequired: [],
    badArguments: [
      { argument: 'leftPan', call: (e) => e.setTrackStripDualPan(TRACK_ID, '0.5', 0) },
      { argument: 'rightPan', call: (e) => e.setTrackStripDualPan(TRACK_ID, 0, '0.5') },
    ],
  },
  {
    name: 'RealtimeEngine.setTrackStripChannelDelaySamples',
    missingRequired: [],
    badArguments: [
      {
        argument: 'delaySamples',
        call: (e) => e.setTrackStripChannelDelaySamples(TRACK_ID, '64'),
      },
    ],
  },
  {
    name: 'RealtimeEngine.createClipPageProvider',
    missingRequired: [],
    badArguments: [
      { argument: 'numChannels', call: (e) => e.createClipPageProvider('1', 1024, 256) },
      { argument: 'pageFrames', call: (e) => e.createClipPageProvider(1, 1024, '256') },
    ],
  },
  {
    name: 'RealtimeEngine.supplyClipPage',
    missingRequired: [],
    badArguments: [
      { argument: 'providerId', call: (e) => e.supplyClipPage('1', 0, [samples(64)]) },
      { argument: 'pageIndex', call: (e) => e.supplyClipPage(1, '0', [samples(64)]) },
    ],
  },
  {
    name: 'RealtimeEngine.clearClipPage',
    missingRequired: [],
    badArguments: [{ argument: 'providerId', call: (e) => e.clearClipPage('1', 0) }],
  },
  {
    name: 'RealtimeEngine.destroyClipPageProvider',
    missingRequired: [],
    badArguments: [{ argument: 'providerId', call: (e) => e.destroyClipPageProvider('1') }],
  },
  {
    name: 'RealtimeEngine.setClipPagePrefetchFrames',
    missingRequired: [],
    badArguments: [{ argument: 'frames', call: (e) => e.setClipPagePrefetchFrames('4096') }],
  },
  {
    name: 'RealtimeEngine.armCapture',
    missingRequired: [],
    badArguments: [{ argument: 'armed', call: (e) => e.armCapture(1) }],
  },
  {
    name: 'RealtimeEngine.setCapturePunch',
    missingRequired: [],
    badArguments: [
      { argument: 'enabled', call: (e) => e.setCapturePunch(0, BLOCK, 1) },
      { argument: 'startSample', call: (e) => e.setCapturePunch('0', BLOCK, true) },
    ],
  },
  {
    name: 'RealtimeEngine.setRecordOffsetSamples',
    missingRequired: [],
    badArguments: [{ argument: 'offsetSamples', call: (e) => e.setRecordOffsetSamples('128') }],
  },
  {
    name: 'RealtimeEngine.setInputMonitor',
    missingRequired: [],
    badArguments: [
      { argument: 'enabled', call: (e) => e.setInputMonitor(1) },
      { argument: 'gain', call: (e) => e.setInputMonitor(true, '0.5') },
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
    expect(
      CASES.every(
        (entry) => entry.missingRequired.length > 0 || (entry.badArguments?.length ?? 0) > 0,
      ),
    ).toBe(true);
    // The strip / capture / clip-page group is the whole point of the
    // positional half of the table; a shrunken list must fail here rather than
    // quietly stop covering entry points.
    expect(CASES.filter((entry) => entry.badArguments !== undefined).length).toBeGreaterThanOrEqual(
      30,
    );
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

describe('addon positional-argument readers bail out before touching native state', () => {
  it('parks the fixture engine off its default state, so the snapshot can move', () => {
    // Every field the snapshot reads has to be able to change, or comparing it
    // either side of a rejected call would assert nothing. This is that proof.
    withConfiguredEngine((engine) => {
      const status = engine.captureStatus();
      expect(status.armed).toBe(true);
      expect(status.punchEnabled).toBe(true);
      expect(status.source).toBe('input');
      expect(status.recordOffsetSamples).toBe(RECORD_OFFSET);
      expect(engine.clipPagePrefetchFrames()).toBe(PREFETCH_FRAMES);
      expect(engine.resolveTrackInsertAutomationId(TRACK_ID, 0, 'band0.gainDb')).toBeGreaterThan(0);
      expect(engine.resolveBusInsertAutomationId(BUS_ID, 0, 'band0.gainDb')).toBeGreaterThan(0);
      expect(engine.resolveMasterInsertAutomationId(0, 'band0.gainDb')).toBeGreaterThan(0);
    });
  });

  it('moves the snapshot for a well-typed call, so equality is not free', () => {
    withConfiguredEngine((engine) => {
      const before = engineStateSnapshot(engine);
      engine.armCapture(false);
      expect(engineStateSnapshot(engine)).not.toBe(before);
      // The same strip rebuilt without its insert loses the automation id,
      // which is what a swallowed bad `sceneJson` used to cause.
      const withInsert = engineStateSnapshot(engine);
      engine.setTrackStripJson(
        TRACK_ID,
        JSON.stringify({
          version: 1,
          strips: [{ id: `track-${TRACK_ID}` }],
          buses: [],
          connections: [],
        }),
      );
      expect(engine.resolveTrackInsertAutomationId(TRACK_ID, 0, 'band0.gainDb')).toBe(-1);
      expect(engineStateSnapshot(engine)).not.toBe(withInsert);
    });
  });

  for (const { name, badArguments } of CASES) {
    for (const { argument, call } of badArguments ?? []) {
      it(`${name}: a wrong-typed ${argument} throws a TypeError and moves no engine state`, () => {
        withConfiguredEngine((engine) => {
          const before = engineStateSnapshot(engine);
          expect(() => call(engine)).toThrow(TypeError);
          expect(engineStateSnapshot(engine)).toBe(before);
        });
        expectNativeStillWorks();
      });
    }
  }

  // Insert bypass has no addon getter, so the snapshot above cannot see it. The
  // audible difference between a bypassed and an active insert can, and the
  // reversal this guards against (`bypassed` read as `false` from a truthy
  // number) is precisely a swap between those two states.
  it('leaves a bypassed insert bypassed when the bypassed flag is wrong-typed', () => {
    const engine = new addon.RealtimeEngine(SR, BLOCK) as NativeEngine;
    try {
      const frames = BLOCK * 16;
      const source = new Float32Array(frames).map((_, i) =>
        Math.sin((2 * Math.PI * 1000 * i) / SR),
      );
      engine.setClips([
        { id: 1, trackId: TRACK_ID, channels: [source], startPpq: 0, lengthSamples: frames },
      ]);
      engine.setTrackLanes([TRACK_ID]);
      engine.setTrackStripJson(TRACK_ID, trackStripJson(12));
      engine.play();

      // Rewind before each measurement so every settle reads the same stretch
      // of the clip; only the strip state differs between them.
      const settle = (): number => {
        engine.seekSample(0);
        let block: Float32Array<ArrayBufferLike> = new Float32Array(BLOCK);
        for (let i = 0; i < 8; i++) {
          block = engine.process([new Float32Array(BLOCK)])[0];
        }
        return rms(block);
      };

      engine.setTrackStripInsertBypassed(TRACK_ID, 0, false);
      const activeRms = settle();
      engine.setTrackStripInsertBypassed(TRACK_ID, 0, true);
      const bypassedRms = settle();
      // Positive control: if the boost were inaudible the assertion below would
      // hold no matter what the rejected call did.
      expect(activeRms).toBeGreaterThan(0);
      expect(Math.abs(activeRms - bypassedRms)).toBeGreaterThan(0.05 * activeRms);

      expect(() => engine.setTrackStripInsertBypassed(TRACK_ID, 0, 1)).toThrow(TypeError);
      expect(settle()).toBeCloseTo(bypassedRms, 3);
    } finally {
      engine.destroy();
    }
  });
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
        swallow(() => e.setLaneSidechain(10, 0, '1'));
        swallow(() => e.setBusStripJson(1, 42));
        swallow(() => e.setTrackStripJson(10, 42));
        swallow(() => e.setTrackStripEqBandJson(10, '0', 42));
        swallow(() => e.setTrackStripInsertBypassed(10, 0, 1, 1));
        swallow(() => e.setMasterStripJson(42));
        swallow(() => e.setMasterStripEqBandJson('0', 42));
        swallow(() => e.setMasterStripInsertBypassed(0, 1, 1));
        swallow(() => e.setTrackStripInsertParamByName(10, 0, 42, '1'));
        swallow(() => e.setMasterStripInsertParamByName(0, 42, '1'));
        swallow(() => e.setBusStripInsertParamByName(1, 0, 42, '1'));
        swallow(() => e.setBusStripInsertBypassed(1, 0, 1, 1));
        swallow(() => e.resolveTrackInsertAutomationId(10, '0', 42));
        swallow(() => e.resolveMasterInsertAutomationId('0', 42));
        swallow(() => e.resolveBusInsertAutomationId(1, '0', 42));
        swallow(() => e.resolveInstrumentAutomationId('1', 42));
        swallow(() => e.setTrackStripPan(10, '0.5'));
        swallow(() => e.setTrackStripPanLaw(10, '3'));
        swallow(() => e.setTrackStripPanMode(10, '2'));
        swallow(() => e.setTrackStripDualPan(10, '0.5', '0.5'));
        swallow(() => e.setTrackStripChannelDelaySamples(10, '64'));
        swallow(() => e.createClipPageProvider('1', '1024', '256'));
        swallow(() => e.supplyClipPage('1', '0', 'x'));
        swallow(() => e.clearClipPage('1', '0'));
        swallow(() => e.destroyClipPageProvider('1'));
        swallow(() => e.setClipPagePrefetchFrames('4096'));
        swallow(() => e.armCapture(1));
        swallow(() => e.setCapturePunch('0', '128', 1));
        swallow(() => e.setRecordOffsetSamples('64'));
        swallow(() => e.setInputMonitor(1, '0.5'));
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
