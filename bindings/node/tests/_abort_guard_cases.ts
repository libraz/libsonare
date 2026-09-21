/**
 * One entry per addon entry point that must refuse bad input, and the hostile
 * call that proves it.
 *
 * The table is data rather than assertions on purpose: which entry points
 * belong in it is decided by scanning the addon sources, and the coverage
 * check in addon-abort-guards.test.ts fails on an entry point that reads a
 * positional argument and appears neither here nor in a reasoned register.
 */

import { addon } from '../src/native.js';

import {
  BLOCK,
  BUS_ID,
  engineStateSnapshot,
  MARKER_ID,
  type NativeEngine,
  PARAM_ID,
  PROJECT_SR,
  type ProjectFixture,
  SR,
  TRACK_ID,
  transportSnapshot,
  withConfiguredEngine,
  withConfiguredProject,
  withEngine,
  withPreparedEngine,
  withProject,
  withSampleBank,
} from './_abort_guard_handles.js';

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
export interface AbortGuardCase {
  /** `Class.jsName`; the suffix is what the coverage register matches on. */
  name: string;
  missingRequired: Array<{ field: string; call: () => void }>;
  badOptional?: () => void;
  /** Driven against {@link withConfiguredEngine}, compared with {@link engineStateSnapshot}. */
  badArguments?: BadArgument<NativeEngine>[];
  /** Driven against {@link withPreparedEngine}, compared with {@link transportSnapshot}. */
  badTransportArguments?: BadArgument<NativeEngine>[];
  /** Driven against {@link withConfiguredProject}, compared with `toJson()`. */
  badProjectArguments?: BadArgument<ProjectFixture>[];
  /**
   * Free functions and pure getters: they own no handle state to snapshot, so
   * the assertion is the C-1 half only — exactly one catchable error, process
   * still alive.
   */
  rejectsArgument?: BadArgument<void>[];
}

/**
 * One hostile call. `error` defaults to TypeError, which is what a wrong-typed
 * argument raises; a MIDI byte outside [0, 255] is a RangeError instead, and
 * that spelling matters — a value the narrowing cast would wrap is the exact
 * input that used to raise a second throw on top of the first and abort.
 */
export interface BadArgument<T> {
  argument: string;
  call: (target: T) => void;
  error?: ErrorConstructor;
}

const samples = (n = 4): Float32Array => new Float32Array(n);

/** A steady click track, so the tempo entry points have a grid to find. */
export const clickTrack = (bpm = 120, seconds = 3, sampleRate = PROJECT_SR): Float32Array => {
  const block = new Float32Array(Math.floor(sampleRate * seconds));
  const clickLength = Math.floor(sampleRate / 100);
  const period = (60 / bpm) * sampleRate;
  for (let start = 0; start < block.length; start += period) {
    for (let i = 0; i < clickLength && Math.floor(start) + i < block.length; i++) {
      block[Math.floor(start) + i] = (1 - i / clickLength) * 0.9;
    }
  }
  return block;
};

export const CASES: AbortGuardCase[] = [
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
  {
    name: 'RealtimeEngine.setCaptureBuffer',
    missingRequired: [],
    badArguments: [{ argument: 'numChannels', call: (e) => e.setCaptureBuffer('2', 1024) }],
  },
  {
    name: 'RealtimeEngine.prepare',
    missingRequired: [],
    badTransportArguments: [
      { argument: 'sampleRate', call: (e) => e.prepare('48000', BLOCK) },
      // An absent argument used to short-circuit ahead of every reader, so the
      // call returned undefined with nothing pending and the caller saw a
      // silently unprepared engine.
      {
        argument: 'omitted maxBlockSize',
        call: (e) => (e.prepare as unknown as (sampleRate: unknown) => void)(SR),
      },
      {
        argument: 'omitted sampleRate and maxBlockSize',
        call: (e) => (e.prepare as unknown as () => void)(),
      },
    ],
  },
  {
    name: 'RealtimeEngine.play',
    missingRequired: [],
    // The reported reproduction: a stopped transport that starts playing while
    // the caller is being handed a TypeError.
    badTransportArguments: [{ argument: 'renderFrame', call: (e) => e.play('now') }],
  },
  {
    name: 'RealtimeEngine.stop',
    missingRequired: [],
    badTransportArguments: [{ argument: 'renderFrame', call: (e) => e.stop('now') }],
  },
  {
    name: 'RealtimeEngine.seekSample',
    missingRequired: [],
    // The reported reproduction: the play head snapping back to 0 while the
    // caller is being handed a TypeError.
    badTransportArguments: [
      { argument: 'timelineSample', call: (e) => e.seekSample('x') },
      { argument: 'renderFrame', call: (e) => e.seekSample(0, 'x') },
    ],
  },
  {
    name: 'RealtimeEngine.seekPpq',
    missingRequired: [],
    badTransportArguments: [{ argument: 'renderFrame', call: (e) => e.seekPpq(0, 'x') }],
  },
  {
    name: 'RealtimeEngine.seekMarker',
    missingRequired: [],
    badTransportArguments: [{ argument: 'renderFrame', call: (e) => e.seekMarker(MARKER_ID, 'x') }],
  },
  {
    name: 'RealtimeEngine.countInEndSample',
    missingRequired: [],
    badTransportArguments: [{ argument: 'startSample', call: (e) => e.countInEndSample('0', 1) }],
  },
  {
    name: 'RealtimeEngine.setParameter',
    missingRequired: [],
    badTransportArguments: [
      { argument: 'renderFrame', call: (e) => e.setParameter(PARAM_ID, 0.5, 'now') },
    ],
  },
  {
    name: 'RealtimeEngine.setParameterSmoothed',
    missingRequired: [],
    badTransportArguments: [
      { argument: 'renderFrame', call: (e) => e.setParameterSmoothed(PARAM_ID, 0.5, 'now') },
    ],
  },
  {
    name: 'RealtimeEngine.setSoloMute',
    missingRequired: [],
    badTransportArguments: [
      { argument: 'renderFrame', call: (e) => e.setSoloMute(0, true, false, 'now') },
    ],
  },
  {
    name: 'RealtimeEngine.setTrackMonitorMode',
    missingRequired: [],
    badTransportArguments: [
      { argument: 'renderFrame', call: (e) => e.setTrackMonitorMode(0, 1, 'now') },
    ],
  },
  {
    name: 'RealtimeEngine.bindMidiCc',
    missingRequired: [],
    badTransportArguments: [
      { argument: 'channel', call: (e) => e.bindMidiCc('0', 7, PARAM_ID, 0, 1) },
      {
        argument: 'controller out of byte range',
        call: (e) => e.bindMidiCc(300, 300, PARAM_ID, 0, 1),
        error: RangeError,
      },
    ],
  },
  {
    name: 'RealtimeEngine.pushMidiNoteOn',
    missingRequired: [],
    badTransportArguments: [
      // The reported reproduction: a MIDI-learn slider handing 0..1023 through.
      // Two out-of-range bytes in a row is what raised the second throw.
      {
        argument: 'group and channel out of byte range',
        call: (e) => e.pushMidiNoteOn(0, 300, 300, 60, 100),
        error: RangeError,
      },
      { argument: 'note', call: (e) => e.pushMidiNoteOn(0, 0, 0, 'x', 100) },
      { argument: 'renderFrame', call: (e) => e.pushMidiNoteOn(0, 0, 0, 60, 100, 'now') },
    ],
  },
  {
    name: 'RealtimeEngine.setArticulation',
    missingRequired: [],
    badTransportArguments: [
      {
        argument: 'channel out of byte range',
        call: (e) => e.setArticulation(0, 300, 'poly'),
        error: RangeError,
      },
      // An unknown NAME is the RangeError; a value that is neither a name nor
      // an ordinal is the TypeError, and both must land before the C call —
      // the dummy 0 a failed read yields spells 'poly', which is a mode.
      {
        argument: 'articulation name',
        call: (e) => e.setArticulation(0, 0, 'legato'),
        error: RangeError,
      },
      { argument: 'articulation', call: (e) => e.setArticulation(0, 0, {}) },
    ],
  },
  {
    name: 'RealtimeEngine.articulation',
    missingRequired: [],
    badTransportArguments: [
      {
        argument: 'channel out of byte range',
        call: (e) => e.articulation(0, 300),
        error: RangeError,
      },
    ],
  },
  {
    name: 'RealtimeEngine.pushMidiNoteOff',
    missingRequired: [],
    badTransportArguments: [
      {
        argument: 'group and channel out of byte range',
        call: (e) => e.pushMidiNoteOff(0, 300, 300, 60, 0),
        error: RangeError,
      },
      { argument: 'renderFrame', call: (e) => e.pushMidiNoteOff(0, 0, 0, 60, 0, 'now') },
    ],
  },
  {
    name: 'RealtimeEngine.pushMidiCc',
    missingRequired: [],
    badTransportArguments: [
      {
        argument: 'group and channel out of byte range',
        call: (e) => e.pushMidiCc(0, 300, 300, 7, 64),
        error: RangeError,
      },
      { argument: 'renderFrame', call: (e) => e.pushMidiCc(0, 0, 0, 7, 64, 'now') },
    ],
  },
  {
    name: 'RealtimeEngine.pushMidiPitchBend',
    missingRequired: [],
    badTransportArguments: [
      {
        argument: 'group and channel out of byte range',
        call: (e) => e.pushMidiPitchBend(0, 300, 300, 8192),
        error: RangeError,
      },
      {
        // 65536 casts to 0 — a valid bend — so the wrap has to be refused here.
        argument: 'bend14 out of uint16 range',
        call: (e) => e.pushMidiPitchBend(0, 0, 0, 65536),
        error: RangeError,
      },
      { argument: 'renderFrame', call: (e) => e.pushMidiPitchBend(0, 0, 0, 8192, 'now') },
    ],
  },
  {
    name: 'RealtimeEngine.pushMidiChannelPressure',
    missingRequired: [],
    badTransportArguments: [
      {
        argument: 'group and channel out of byte range',
        call: (e) => e.pushMidiChannelPressure(0, 300, 300, 64),
        error: RangeError,
      },
      { argument: 'renderFrame', call: (e) => e.pushMidiChannelPressure(0, 0, 0, 64, 'now') },
    ],
  },
  {
    name: 'RealtimeEngine.pushMidiPolyPressure',
    missingRequired: [],
    badTransportArguments: [
      {
        argument: 'group and channel out of byte range',
        call: (e) => e.pushMidiPolyPressure(0, 300, 300, 60, 64),
        error: RangeError,
      },
      { argument: 'renderFrame', call: (e) => e.pushMidiPolyPressure(0, 0, 0, 60, 64, 'now') },
    ],
  },
  {
    name: 'RealtimeEngine.pushMidiInputNoteOn',
    missingRequired: [],
    badTransportArguments: [
      {
        argument: 'group and channel out of byte range',
        call: (e) => e.pushMidiInputNoteOn(300, 300, 60, 100),
        error: RangeError,
      },
      { argument: 'velocity', call: (e) => e.pushMidiInputNoteOn(0, 0, 60, 'x') },
      { argument: 'portTimeSamples', call: (e) => e.pushMidiInputNoteOn(0, 0, 60, 100, 'now') },
    ],
  },
  {
    name: 'RealtimeEngine.pushMidiInputNoteOff',
    missingRequired: [],
    badTransportArguments: [
      {
        argument: 'group and channel out of byte range',
        call: (e) => e.pushMidiInputNoteOff(300, 300, 60, 0),
        error: RangeError,
      },
      { argument: 'portTimeSamples', call: (e) => e.pushMidiInputNoteOff(0, 0, 60, 0, 'now') },
    ],
  },
  {
    name: 'RealtimeEngine.pushMidiInputCc',
    missingRequired: [],
    badTransportArguments: [
      {
        argument: 'group and channel out of byte range',
        call: (e) => e.pushMidiInputCc(300, 300, 7, 64),
        error: RangeError,
      },
      { argument: 'portTimeSamples', call: (e) => e.pushMidiInputCc(0, 0, 7, 64, 'now') },
    ],
  },
  {
    name: 'RealtimeEngine.pushMidiInputPitchBend',
    missingRequired: [],
    badTransportArguments: [
      {
        argument: 'group and channel out of byte range',
        call: (e) => e.pushMidiInputPitchBend(300, 300, 8192),
        error: RangeError,
      },
      {
        argument: 'bend14 out of uint16 range',
        call: (e) => e.pushMidiInputPitchBend(0, 0, 65536),
        error: RangeError,
      },
      { argument: 'portTimeSamples', call: (e) => e.pushMidiInputPitchBend(0, 0, 8192, 'now') },
    ],
  },
  {
    name: 'RealtimeEngine.pushMidiInputChannelPressure',
    missingRequired: [],
    badTransportArguments: [
      {
        argument: 'group and channel out of byte range',
        call: (e) => e.pushMidiInputChannelPressure(300, 300, 64),
        error: RangeError,
      },
      { argument: 'portTimeSamples', call: (e) => e.pushMidiInputChannelPressure(0, 0, 64, 'now') },
    ],
  },
  {
    name: 'RealtimeEngine.pushMidiInputPolyPressure',
    missingRequired: [],
    badTransportArguments: [
      {
        argument: 'group and channel out of byte range',
        call: (e) => e.pushMidiInputPolyPressure(300, 300, 60, 64),
        error: RangeError,
      },
      {
        argument: 'portTimeSamples',
        call: (e) => e.pushMidiInputPolyPressure(0, 0, 60, 64, 'now'),
      },
    ],
  },
  {
    name: 'RealtimeEngine.pushMidiPanic',
    missingRequired: [],
    badTransportArguments: [{ argument: 'renderFrame', call: (e) => e.pushMidiPanic('now') }],
  },
  {
    name: 'RealtimeEngine.pushMidiSysex',
    missingRequired: [],
    badTransportArguments: [
      {
        argument: 'renderFrame',
        call: (e) => e.pushMidiSysex(0, new Uint8Array([0xf0, 0x7e, 0xf7]), 'now'),
      },
    ],
  },
  {
    name: 'RealtimeEngine.renderOffline',
    missingRequired: [],
    badTransportArguments: [
      {
        argument: 'blockSize',
        call: (e) => e.renderOffline([new Float32Array(BLOCK)], '128'),
      },
    ],
  },
  {
    name: 'RealtimeEngine.setLoop',
    missingRequired: [],
    badTransportArguments: [{ argument: 'enabled', call: (e) => e.setLoop(0, 4, 1) }],
  },
  {
    name: 'RealtimeEngine.setAutomationLane',
    missingRequired: [],
    badTransportArguments: [
      {
        argument: 'paramId',
        call: (e) => e.setAutomationLane(String(PARAM_ID), [{ ppq: 0, value: 0.5 }]),
      },
    ],
  },
  {
    name: 'RealtimeEngine.parameterInfoByIndex',
    missingRequired: [],
    badTransportArguments: [{ argument: 'index', call: (e) => e.parameterInfoByIndex('0') }],
  },
  {
    name: 'RealtimeEngine.markerByIndex',
    missingRequired: [],
    badTransportArguments: [{ argument: 'index', call: (e) => e.markerByIndex('0') }],
  },
  {
    name: 'RealtimeEngine.drainExternalMidi',
    missingRequired: [],
    badTransportArguments: [
      { argument: 'maxRecords', call: (e) => e.drainExternalMidi('16') },
      { argument: 'negative maxRecords', call: (e) => e.drainExternalMidi(-1), error: RangeError },
      // Below the worst-case 3 messages one queued event lowers to, so the
      // drain could never make progress: rejected rather than silently empty.
      {
        argument: 'maxRecords below the forward-progress minimum',
        call: (e) => e.drainExternalMidi(1),
        error: RangeError,
      },
    ],
  },
  {
    name: 'Audio.fromFileChannel',
    missingRequired: [],
    // The channel index is read before the file is opened, so these are refused
    // without a fixture on disk. 2^32 + 1 is the wrap that matters: ToInt32
    // lands it on 1, a legal channel of a stereo file, so it would decode the
    // right channel of the wrong caller's request.
    rejectsArgument: [
      {
        argument: 'channelIndex',
        call: () => addon.Audio.fromFileChannel('/nonexistent.wav', '0'),
      },
      {
        argument: 'omitted channelIndex',
        call: () => addon.Audio.fromFileChannel('/nonexistent.wav'),
      },
      {
        argument: 'channelIndex past the signed range',
        call: () => addon.Audio.fromFileChannel('/nonexistent.wav', 2 ** 32 + 1),
        error: RangeError,
      },
    ],
  },
  {
    name: 'SonareWrap.peakPick',
    missingRequired: [],
    // Six numeric arguments in a row. Under the inline form the first bad one
    // left an exception pending and the second one's throw aborted the process.
    rejectsArgument: [
      { argument: 'preMax', call: () => addon.peakPick(samples(16), 'x', 1, 1, 1, 0.1, 1) },
      {
        argument: 'preMax and postMax together',
        call: () => addon.peakPick(samples(16), 'x', 'y', 1, 1, 0.1, 1),
      },
      { argument: 'delta', call: () => addon.peakPick(samples(16), 1, 1, 1, 1, 'x', 1) },
    ],
  },
  {
    name: 'SonareWrap.noteTargetsFromSmf',
    missingRequired: [],
    // The track index is read before the bytes are parsed, so every rejection
    // here needs no readable file. 2^32 is the wrap that matters: ToInt32 lands
    // it on 0, the default track, so it would read the melody of a file the
    // caller asked nothing about and report success.
    rejectsArgument: [
      { argument: 'trackIndex', call: () => addon.noteTargetsFromSmf(new Uint8Array(8), 'x') },
      {
        argument: 'trackIndex past the signed range',
        call: () => addon.noteTargetsFromSmf(new Uint8Array(8), 2 ** 32),
        error: RangeError,
      },
      {
        argument: 'fractional trackIndex',
        call: () => addon.noteTargetsFromSmf(new Uint8Array(8), 0.5),
        error: RangeError,
      },
    ],
  },
  {
    name: 'SonareWrap.alignTakeToReference',
    missingRequired: [],
    // A stateless alignment over two buffers, so the C-1 half is the whole
    // assertion. The rate is read before either buffer reaches the alignment, so
    // none of these needs an alignable pair. 2^32 + 22050 is the wrap that
    // matters: ToInt32 lands it on 22050, a rate the C entry accepts, so it would
    // align at a rate the caller never asked for.
    rejectsArgument: [
      {
        argument: 'sampleRate',
        call: () => addon.alignTakeToReference(samples(16), samples(16), 'x'),
      },
      {
        argument: 'omitted sampleRate',
        call: () => addon.alignTakeToReference(samples(16), samples(16)),
      },
      {
        argument: 'sampleRate past the signed range',
        call: () => addon.alignTakeToReference(samples(16), samples(16), 2 ** 32 + 22050),
        error: RangeError,
      },
      {
        argument: 'reference',
        call: () => addon.alignTakeToReference([0, 0], samples(16), 22050),
      },
    ],
  },
  {
    name: 'SonareWrap.hpss',
    missingRequired: [],
    // Stateless separation over a buffer, so there is no handle state to
    // snapshot; the C-1 half is the whole assertion. Only a MAGNITUDE the
    // narrowing would wrap is a rejection here — a wrong TYPE falls back to the
    // default and is pinned as such further down, because these two are
    // different contracts over the same argument. The two entry points read the
    // same arguments through one reader, so between them each of its sites is
    // driven once.
    //
    // Each value is chosen so ToInt32 lands it on something the downstream
    // guards ACCEPT, which is what makes the wrap a silent success rather than a
    // refusal by luck: 2^32 + 1 wraps to a legal odd kernel, 2^32 + 2048 to the
    // default framing and 2^32 + 512 to the default hop. Driving nFft with
    // 2^32 + 1 instead proved the point in the wrong direction — it wraps to 1,
    // which the core rejects as not even, so the case would have passed on a
    // guard that has nothing to do with the narrowing.
    rejectsArgument: [
      {
        argument: 'kernelPercussive past the signed range',
        call: () => addon.hpss(samples(2048), SR, 31, 2 ** 32 + 1),
        error: RangeError,
      },
      {
        argument: 'nFft past the signed range',
        call: () => addon.hpss(samples(2048), SR, 31, 31, 2 ** 32 + 2048),
        error: RangeError,
      },
    ],
  },
  {
    name: 'SonareWrap.hpssWithResidual',
    missingRequired: [],
    rejectsArgument: [
      {
        argument: 'kernelHarmonic past the signed range',
        call: () => addon.hpssWithResidual(samples(2048), SR, 2 ** 32 + 1, 31),
        error: RangeError,
      },
      {
        argument: 'hopLength past the signed range',
        call: () => addon.hpssWithResidual(samples(2048), SR, 31, 31, 2048, 2 ** 32 + 512),
        error: RangeError,
      },
    ],
  },
  {
    name: 'SonareWrap.normalizeStereo',
    missingRequired: [],
    // A stateless pair transform, so the C-1 half is the whole assertion. The
    // sample rate is the argument that matters: 2^32 + 48000 wraps to 48000
    // under ToInt32, a rate every downstream guard accepts, so the wrap would
    // be a silent success rather than a refusal by luck.
    rejectsArgument: [
      {
        argument: 'sampleRate',
        call: () => addon.normalizeStereo(samples(4), samples(4), 'x'),
      },
      { argument: 'omitted sampleRate', call: () => addon.normalizeStereo(samples(4), samples(4)) },
      {
        argument: 'sampleRate past the signed range',
        call: () => addon.normalizeStereo(samples(4), samples(4), 2 ** 32 + SR),
        error: RangeError,
      },
      {
        argument: 'targetDb',
        call: () => addon.normalizeStereo(samples(4), samples(4), SR, 'x'),
      },
      {
        argument: 'targetDb and mode together',
        call: () => addon.normalizeStereo(samples(4), samples(4), SR, 'x', 7),
      },
    ],
  },
  {
    name: 'SonareWrap.segmentSubsegment',
    missingRequired: [],
    // A short argument list short-circuited ahead of every reader, so the call
    // answered undefined with nothing pending and the caller saw no error.
    rejectsArgument: [
      { argument: 'omitted boundaries', call: () => addon.segmentSubsegment(samples(4), 2, 2) },
      { argument: 'no arguments at all', call: () => addon.segmentSubsegment() },
      { argument: 'wrong-typed data', call: () => addon.segmentSubsegment('x', 2, 2, [0]) },
    ],
  },
  {
    name: 'SampleBank.addZone',
    missingRequired: [],
    // A bank is built and then read, so it has no live state a snapshot could
    // move; the C-1 half is the whole assertion here. The byte-range row feeds
    // two out-of-range values in a row, which is the shape that aborts when the
    // second reader throws on the first one's pending exception.
    rejectsArgument: [
      { argument: 'setIndex', call: () => withSampleBank((bank) => bank.addZone('0', {})) },
      {
        argument: 'keyLo out of byte range',
        call: () => withSampleBank((bank) => bank.addZone(0, { keyLo: 300, keyHi: 300 })),
        error: RangeError,
      },
    ],
  },
  {
    name: 'Project.addTrack',
    missingRequired: [],
    badProjectArguments: [{ argument: 'kind', call: ({ project }) => project.addTrack('audio') }],
  },
  {
    name: 'Project.addMidiClip',
    missingRequired: [],
    badProjectArguments: [
      { argument: 'startPpq', call: ({ project }) => project.addMidiClip('0', 4) },
      { argument: 'lengthPpq', call: ({ project }) => project.addMidiClip(0, '4') },
    ],
  },
  {
    name: 'Project.setSampleRate',
    missingRequired: [],
    badProjectArguments: [
      { argument: 'sampleRate', call: ({ project }) => project.setSampleRate('44100') },
    ],
  },
  {
    name: 'Project.setOverlapPolicy',
    missingRequired: [],
    badProjectArguments: [
      { argument: 'policy', call: ({ project }) => project.setOverlapPolicy('0') },
    ],
  },
  {
    name: 'Project.setMarker',
    missingRequired: [],
    badProjectArguments: [
      { argument: 'ppq', call: ({ project, markerId }) => project.setMarker(markerId, 'x', 'v') },
    ],
  },
  {
    name: 'Project.markerByIndex',
    missingRequired: [],
    badProjectArguments: [{ argument: 'index', call: ({ project }) => project.markerByIndex('0') }],
  },
  {
    name: 'Project.trackByIndex',
    missingRequired: [],
    badProjectArguments: [{ argument: 'index', call: ({ project }) => project.trackByIndex('0') }],
  },
  {
    name: 'Project.clipByIndex',
    missingRequired: [],
    badProjectArguments: [{ argument: 'index', call: ({ project }) => project.clipByIndex('0') }],
  },
  {
    name: 'Project.sourceByIndex',
    missingRequired: [],
    badProjectArguments: [{ argument: 'index', call: ({ project }) => project.sourceByIndex('0') }],
  },
  {
    name: 'Project.tempoSegmentByIndex',
    missingRequired: [],
    badProjectArguments: [
      { argument: 'index', call: ({ project }) => project.tempoSegmentByIndex('0') },
    ],
  },
  {
    name: 'Project.timeSignatureByIndex',
    missingRequired: [],
    badProjectArguments: [
      { argument: 'index', call: ({ project }) => project.timeSignatureByIndex('0') },
    ],
  },
  {
    name: 'Project.splitClip',
    missingRequired: [],
    badProjectArguments: [
      { argument: 'splitPpq', call: ({ project, clipId }) => project.splitClip(clipId, 'x') },
    ],
  },
  {
    name: 'Project.trimClip',
    missingRequired: [],
    badProjectArguments: [
      { argument: 'newStartPpq', call: ({ project, clipId }) => project.trimClip(clipId, 'x', 4) },
    ],
  },
  {
    name: 'Project.moveClip',
    missingRequired: [],
    badProjectArguments: [
      {
        argument: 'newStartPpq',
        call: ({ project, clipId, trackId }) => project.moveClip(clipId, 'x', trackId),
      },
    ],
  },
  {
    name: 'Project.duplicateClip',
    missingRequired: [],
    badProjectArguments: [
      {
        argument: 'newStartPpq',
        call: ({ project, clipId }) => project.duplicateClip(clipId, 'x'),
      },
    ],
  },
  {
    name: 'Project.removeClip',
    missingRequired: [],
    badProjectArguments: [{ argument: 'clipId', call: ({ project }) => project.removeClip('1') }],
  },
  {
    name: 'Project.removeTrack',
    missingRequired: [],
    badProjectArguments: [{ argument: 'trackId', call: ({ project }) => project.removeTrack('1') }],
  },
  {
    name: 'Project.renameTrack',
    missingRequired: [],
    badProjectArguments: [
      { argument: 'trackId', call: ({ project }) => project.renameTrack('1', 'renamed') },
    ],
  },
  {
    name: 'Project.setTrackRoute',
    missingRequired: [],
    badProjectArguments: [
      {
        argument: 'trackId',
        call: ({ project }) => project.setTrackRoute('1', 'strip-b', 'out-b'),
      },
    ],
  },
  {
    name: 'Project.setTrackKind',
    missingRequired: [],
    badProjectArguments: [
      { argument: 'kind', call: ({ project, trackId }) => project.setTrackKind(trackId, 'midi') },
    ],
  },
  {
    name: 'Project.setTrackGain',
    missingRequired: [],
    // The reported reproduction: the track goes silent while the caller's error
    // handler is running.
    badProjectArguments: [
      { argument: 'gain', call: ({ project, trackId }) => project.setTrackGain(trackId, 'loud') },
    ],
  },
  {
    name: 'Project.setTrackPan',
    missingRequired: [],
    badProjectArguments: [
      { argument: 'pan', call: ({ project, trackId }) => project.setTrackPan(trackId, 'left') },
    ],
  },
  {
    name: 'Project.setTrackMute',
    missingRequired: [],
    badProjectArguments: [
      { argument: 'trackId', call: ({ project }) => project.setTrackMute('1', false) },
    ],
  },
  {
    name: 'Project.setTrackSolo',
    missingRequired: [],
    badProjectArguments: [
      { argument: 'trackId', call: ({ project }) => project.setTrackSolo('1', true) },
    ],
  },
  {
    name: 'Project.setTrackMidiDestination',
    missingRequired: [],
    badProjectArguments: [
      {
        argument: 'destinationId',
        call: ({ project, midiTrackId }) => project.setTrackMidiDestination(midiTrackId, 'x'),
      },
    ],
  },
  {
    name: 'Project.setClipGain',
    missingRequired: [],
    badProjectArguments: [
      { argument: 'gain', call: ({ project, clipId }) => project.setClipGain(clipId, 'loud') },
    ],
  },
  {
    name: 'Project.setClipFade',
    missingRequired: [],
    badProjectArguments: [
      {
        argument: 'clipId',
        call: ({ project }) => project.setClipFade('1', { lengthPpq: 1 }, { lengthPpq: 1 }),
      },
    ],
  },
  {
    name: 'Project.setClipLoop',
    missingRequired: [],
    badProjectArguments: [
      {
        argument: 'loopLengthPpq',
        call: ({ project, clipId }) => project.setClipLoop(clipId, 1, 'x', 0),
      },
    ],
  },
  {
    name: 'Project.setClipSource',
    missingRequired: [],
    badProjectArguments: [
      { argument: 'sourceId', call: ({ project, clipId }) => project.setClipSource(clipId, 'x') },
    ],
  },
  {
    name: 'Project.setClipTakes',
    missingRequired: [],
    badProjectArguments: [
      {
        argument: 'activeTakeId',
        call: ({ project, clipId }) =>
          project.setClipTakes(clipId, [{ id: 1, sourceId: 0 }], 'first'),
      },
    ],
  },
  {
    name: 'Project.setClipCompSegments',
    missingRequired: [],
    badProjectArguments: [
      { argument: 'clipId', call: ({ project }) => project.setClipCompSegments('1', []) },
    ],
  },
  {
    name: 'Project.setClipWarpRef',
    missingRequired: [],
    badProjectArguments: [
      {
        argument: 'warpRefId',
        call: ({ project, clipId }) => project.setClipWarpRef(clipId, 'x'),
      },
    ],
  },
  {
    name: 'Project.setClipWarpMode',
    missingRequired: [],
    badProjectArguments: [
      {
        argument: 'mode',
        call: ({ project, clipId }) => project.setClipWarpMode(clipId, 'tempo-sync'),
      },
    ],
  },
  {
    name: 'Project.removeWarpMap',
    missingRequired: [],
    badProjectArguments: [
      { argument: 'warpRefId', call: ({ project }) => project.removeWarpMap('1') },
    ],
  },
  {
    name: 'Project.setSourceAudio',
    missingRequired: [],
    badProjectArguments: [
      {
        argument: 'channels',
        call: ({ project, sourceId }) =>
          project.setSourceAudio(sourceId, new Float32Array(4), '1', PROJECT_SR),
      },
    ],
  },
  {
    name: 'Project.setAudioSourceMetadata',
    missingRequired: [],
    badProjectArguments: [
      {
        argument: 'sourceId',
        call: ({ project }) => project.setAudioSourceMetadata('1', 'hash-xyz', 'other-role'),
      },
    ],
  },
  {
    name: 'Project.setMidiEvents',
    missingRequired: [],
    badProjectArguments: [
      { argument: 'clipId', call: ({ project }) => project.setMidiEvents('2', []) },
    ],
  },
  {
    name: 'Project.setProgram',
    missingRequired: [],
    badProjectArguments: [
      {
        argument: 'program',
        call: ({ project, midiClipId }) => project.setProgram(midiClipId, 'piano', 0),
      },
    ],
  },
  {
    name: 'Project.setProgramOnChannel',
    missingRequired: [],
    badProjectArguments: [
      {
        argument: 'program',
        call: ({ project, midiClipId }) =>
          project.setProgramOnChannel(midiClipId, 0, 0, 'piano', 0),
      },
      {
        argument: 'group and channel out of byte range',
        call: ({ project, midiClipId }) => project.setProgramOnChannel(midiClipId, 300, 300, 40, 0),
        error: RangeError,
      },
    ],
  },
  {
    name: 'Project.bakeMidiFx',
    missingRequired: [],
    badProjectArguments: [
      { argument: 'clipId', call: ({ project }) => project.bakeMidiFx('2', '{}') },
    ],
  },
  {
    // Delegates to bakeMidiFx, so it reaches the same bail-out reader through a
    // call the entry point itself does not spell.
    name: 'Project.setMidiFx',
    missingRequired: [],
    badProjectArguments: [
      { argument: 'clipId', call: ({ project }) => project.setMidiFx('2', '{}') },
    ],
  },
  {
    name: 'Project.bakeMidiFxWithSourceIndex',
    missingRequired: [],
    badProjectArguments: [
      { argument: 'clipId', call: ({ project }) => project.bakeMidiFxWithSourceIndex('2', '{}') },
    ],
  },
  {
    name: 'Project.previewMidiFxCount',
    missingRequired: [],
    badProjectArguments: [
      { argument: 'clipId', call: ({ project }) => project.previewMidiFxCount('2', '{}') },
    ],
  },
  {
    name: 'Project.validateMidiNotes',
    missingRequired: [],
    badProjectArguments: [
      { argument: 'clipId', call: ({ project }) => project.validateMidiNotes('2') },
    ],
  },
  {
    name: 'Project.addAutomationLane',
    missingRequired: [],
    badProjectArguments: [
      {
        argument: 'trackId',
        call: ({ project }) =>
          project.addAutomationLane('bad', { targetParamId: 1, points: [{ ppq: 0, value: 0.5 }] }),
      },
    ],
  },
  {
    name: 'Project.editAutomationLane',
    missingRequired: [],
    // The reported reproduction: a backend taking the track id from an HTTP
    // query, where every positional argument arrives as a string.
    badProjectArguments: [
      {
        argument: 'trackId',
        call: ({ project }) => project.editAutomationLane('bad', 'bad2', 'not-an-object'),
      },
      {
        argument: 'targetParamId',
        call: ({ project, trackId, laneParamId }) =>
          project.editAutomationLane(trackId, String(laneParamId), {
            targetParamId: laneParamId,
            points: [{ ppq: 0, value: 0.5 }],
          }),
      },
    ],
  },
  {
    name: 'Project.removeAutomationLane',
    missingRequired: [],
    badProjectArguments: [
      {
        argument: 'targetParamId',
        call: ({ project, trackId, laneParamId }) =>
          project.removeAutomationLane(trackId, String(laneParamId)),
      },
    ],
  },
  {
    name: 'Project.getAssistSidecar',
    missingRequired: [],
    badProjectArguments: [
      { argument: 'index', call: ({ project }) => project.getAssistSidecar('0') },
    ],
  },
  {
    name: 'Project.setMaxUndoDepth',
    missingRequired: [],
    badProjectArguments: [
      { argument: 'depth', call: ({ project }) => project.setMaxUndoDepth('8') },
    ],
  },
  {
    name: 'Project.setMaxHistoryBytes',
    missingRequired: [],
    badProjectArguments: [
      { argument: 'bytes', call: ({ project }) => project.setMaxHistoryBytes('1024') },
    ],
  },
  {
    name: 'Project.snapToGrid',
    missingRequired: [],
    badProjectArguments: [
      { argument: 'ppq', call: ({ project }) => project.snapToGrid('1', 1, 1) },
    ],
  },
  {
    name: 'Project.autoTempo',
    missingRequired: [],
    badProjectArguments: [
      {
        argument: 'sampleRate',
        call: ({ project }) => project.autoTempo(clickTrack(), '48000', 0, false),
      },
    ],
  },
  {
    name: 'Project.analyzeTempo',
    missingRequired: [],
    badProjectArguments: [
      {
        argument: 'sampleRate',
        call: ({ project }) => project.analyzeTempo(clickTrack(), '48000'),
      },
    ],
  },

  // Constructors. Named by the class alone, because for a constructor the class
  // IS the jsName the coverage register matches on. Each argument below took the
  // process down before these constructors carried a catch harness, so a
  // regression is not a failed assertion here — it is a dead test worker, and
  // the child-process matrix below turns that into an exit code.
  //
  // The shapes matter as much as the values: `new Mixer(2 ** 32, 128)` never
  // reaches the reader, because the sceneJson type check refuses it first.
  {
    name: 'PolyphonicAnalysis',
    missingRequired: [],
    rejectsArgument: [
      {
        argument: 'nFft past the int range',
        call: () => new addon.PolyphonicAnalysis(samples(1024), 22050, { nFft: 2 ** 32 }),
        error: RangeError,
      },
      {
        argument: 'sampleRate',
        call: () => new addon.PolyphonicAnalysis(samples(1024), '22050'),
      },
    ],
  },
  {
    name: 'RealtimeEngine',
    missingRequired: [],
    rejectsArgument: [
      {
        argument: 'maxBlockSize past the int range',
        call: () => new addon.RealtimeEngine(SR, 2 ** 32),
        error: RangeError,
      },
      {
        argument: 'commandCapacity past the int64 range',
        call: () => new addon.RealtimeEngine(SR, BLOCK, 2 ** 63),
        error: RangeError,
      },
    ],
  },
  {
    name: 'Mixer',
    missingRequired: [],
    rejectsArgument: [
      {
        argument: 'sampleRate past the int range',
        call: () => new addon.Mixer('{"tracks":[]}', 2 ** 32, 512),
        error: RangeError,
      },
      {
        argument: 'blockSize past the int range',
        call: () => new addon.Mixer('{"tracks":[]}', SR, 2 ** 32),
        error: RangeError,
      },
    ],
  },
  {
    name: 'StreamingRetune',
    missingRequired: [],
    rejectsArgument: [
      {
        argument: 'grainSize past the int range',
        call: () => new addon.StreamingRetune({ grainSize: 2 ** 32 }),
        error: RangeError,
      },
      {
        argument: 'semitones past the 32-bit float range',
        call: () => new addon.StreamingRetune({ semitones: 1e40 }),
        error: RangeError,
      },
    ],
  },
];
