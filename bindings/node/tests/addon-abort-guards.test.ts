/**
 * The addon is built with NAPI_DISABLE_CPP_EXCEPTIONS, so a failed typed read
 * (`value.As<Napi::Number>().DoubleValue()` on a missing field) does not raise a
 * C++ exception — it leaves a pending JS exception and returns a dummy value.
 * Three consequences make bad input lethal rather than merely wrong:
 *
 *  1. A second N-API throw raised while an exception is already pending is a
 *     `FATAL ERROR ... napi_throw` abort (exit 134). An entry point that keeps
 *     parsing after the first bad field therefore kills the whole process,
 *     straight through any `try`/`catch`, with no stack trace and no chance to
 *     report the error.
 *  2. A C++ exception thrown inside the callback (a `std::length_error` from
 *     `std::vector<T> v(n)` after a negative count wrapped to SIZE_MAX)
 *     terminates the process for the same reason.
 *  3. The dummy value is a perfectly ordinary `0` / `""` / `false`, so a C-ABI
 *     call built from it runs and succeeds. `ThrowIfError` cannot stop that —
 *     the native call is its ARGUMENT, so it has already run by the time the
 *     pending-exception guard is consulted. The caller gets its TypeError with
 *     the gain already zeroed, the transport already rolling, the play head
 *     already back at 0.
 *
 * These tests pin all three shut: every covered entry point must convert the
 * FIRST offending field or argument into exactly one catchable `TypeError` (or
 * `RangeError` for an out-of-domain count or a MIDI byte the narrowing cast
 * would wrap), issue no C-ABI call, and still be alive afterwards. State is
 * asserted by comparing a snapshot either side of the rejected call — the
 * engine's strips and capture session, the transport and MIDI queues, and the
 * project's whole `toJson()`. The suite runs the matrix in a child process too,
 * so "survived" is asserted against a real exit code and not just against the
 * test runner happening to continue.
 *
 * Which entry points must appear in the table is NOT decided here: the addon
 * sources are scanned for every entry point that reads a positional argument
 * through the bail-out reader family, and one missing from the table (or from
 * the reasoned register) fails. Four generations of this finding were each
 * closed by enumerating the entry points that existed at the time.
 */

import { spawnSync } from 'node:child_process';
import { describe, expect, it } from 'vitest';
import { addon } from '../src/native.js';
import {
  bailoutReaderCalls,
  inlineTypedArgumentReads,
  isBailoutGuarded,
  positionalArgEntryPoints,
  positionalReaderDefinitions,
  SHARED_READER_FILE,
} from './_addon_sources.js';

const SR = 48000;
const BLOCK = 128;

/**
 * Definitions with the `(Napi::CallbackInfo, index, ...)` shape that may live
 * outside {@link SHARED_READER_FILE}, each with the reason the shared bail-out
 * family does not cover it. Keyed by `file:name`. A definition that is neither
 * here nor in the shared header is a file-local positional reader, and that is
 * exactly what went wrong four times: `Uint32Arg`, `NumberArg`, `OptionalInt64`
 * and `MidiByteArg` each read `info[i].As<Napi::Number>()` with no type check
 * and no pending-exception guard, so a rejected argument arrived at the C ABI
 * as a dummy 0 and a second reader's throw aborted the process.
 */
const POSITIONAL_READER_ALLOWLIST: ReadonlyMap<string, string> = new Map([
  [
    'sonare_wrap_utils.h:RequireFloat32Array',
    'Type predicate over a Float32Array argument, reported with a message the call site supplies, not a scalar read. Shared from the other addon-wide header, not a per-file copy.',
  ],
  [
    'engine/common.h:ReadParameter',
    'Fills a whole SonareParameterInfo struct from an object argument; the scalar family has nothing to delegate to. Already guards on IsExceptionPending before returning true, and is shared by the engine TUs from one header.',
  ],
  [
    'engine/common.h:ReadChannels',
    'Copies an array of Float32Array planes into a ChannelBlock; not a scalar read, and shared by the engine TUs from one header.',
  ],
  [
    'effects/mastering.cpp:AssistantConfigFromParams',
    'Not a positional scalar reader: the index names an options OBJECT it flattens into mastering params. Matched only because the shape check is deliberately loose about what follows the index.',
  ],
]);

/**
 * Entry points that can reject a positional argument but are not driven by the
 * table, each with why. This is the visible-gap register: adding a new
 * same-shaped entry point without covering it fails the coverage test rather
 * than silently shipping untested.
 */
/**
 * Inline typed positional reads that may stay inline, each with the reason the
 * accessor cannot fail there. Keyed by `file:enclosing-function`. The scan is
 * deliberately body-local — a guard living in a helper is a real coupling
 * hazard, since editing the helper silently unguards every call site — so a
 * site guarded from a distance has to say so here rather than read as safe.
 */
const INLINE_READ_ALLOWLIST: ReadonlyMap<string, string> = new Map([
  [
    'effects/mixing_assistant.cpp:SuggestMixScene',
    'info[4] is type-checked by ReadTrackArrays (`!info[4].IsNumber()` -> TypeError) at the top of this body, and a false return there exits before the read. A local re-check would be unreachable code, not a guard.',
  ],
]);

const UNCOVERED_POSITIONAL_GUARDS: ReadonlyMap<string, string> = new Map([
  [
    'midiCcLearn',
    'Stateless free function over an event array; it owns no project or engine state to compare, and its argument rejection is driven by public-input-conformance.test.ts.',
  ],
  [
    'readGoniometerLatest',
    'Instance method on Mixer, which needs a configured strip and a running meter; covered by metering-and-scale.test.ts.',
  ],
]);

interface NativeCaptureStatus {
  capturedFrames: number;
  overflowCount: number;
  armed: boolean;
  punchEnabled: boolean;
  source: string;
  recordOffsetSamples: number;
}

interface NativeTransportState {
  playing: boolean;
  looping: boolean;
  samplePosition: number;
  ppq: number;
  bpm: number;
}

interface NativeEngine {
  destroy(): void;
  process(channels: Float32Array[]): Float32Array[];
  prepare(sampleRate: unknown, maxBlockSize: unknown, ...rest: unknown[]): void;
  play(renderFrame?: unknown): void;
  stop(renderFrame?: unknown): void;
  seekSample(sample: unknown, renderFrame?: unknown): void;
  seekPpq(ppq: unknown, renderFrame?: unknown): void;
  seekMarker(markerId: unknown, renderFrame?: unknown): void;
  countInEndSample(startSample: unknown, bars?: unknown): number;
  getTransportState(): NativeTransportState;
  setMarkers(markers: unknown): void;
  markerByIndex(index: unknown): unknown;
  setLoop(startPpq: unknown, endPpq?: unknown, enabled?: unknown): void;
  setAutomationLane(paramId: unknown, points?: unknown): void;
  parameterInfoByIndex(index: unknown): unknown;
  drainExternalMidi(maxRecords?: unknown): unknown[];
  addParameter(parameter: unknown): void;
  setParameter(paramId: unknown, value: unknown, renderFrame?: unknown): void;
  setParameterSmoothed(paramId: unknown, value: unknown, renderFrame?: unknown): void;
  setSoloMute(laneIndex: unknown, solo: unknown, mute: unknown, renderFrame?: unknown): void;
  setTrackMonitorMode(laneIndex: unknown, mode: unknown, renderFrame?: unknown): void;
  setMidiInputSource(destinationId: unknown): void;
  midiInputPendingCount(): number;
  midiCcBindingCount(): number;
  bindMidiCc(
    channel: unknown,
    controller: unknown,
    paramId: unknown,
    minValue?: unknown,
    maxValue?: unknown,
  ): void;
  pushMidiNoteOn(
    destinationId: unknown,
    group: unknown,
    channel: unknown,
    note: unknown,
    velocity: unknown,
    renderFrame?: unknown,
  ): void;
  pushMidiNoteOff(
    destinationId: unknown,
    group: unknown,
    channel: unknown,
    note: unknown,
    velocity: unknown,
    renderFrame?: unknown,
  ): void;
  pushMidiCc(
    destinationId: unknown,
    group: unknown,
    channel: unknown,
    controller: unknown,
    value: unknown,
    renderFrame?: unknown,
  ): void;
  pushMidiInputNoteOn(
    group: unknown,
    channel: unknown,
    note: unknown,
    velocity: unknown,
    renderFrame?: unknown,
  ): void;
  pushMidiInputNoteOff(
    group: unknown,
    channel: unknown,
    note: unknown,
    velocity: unknown,
    renderFrame?: unknown,
  ): void;
  pushMidiInputCc(
    group: unknown,
    channel: unknown,
    controller: unknown,
    value: unknown,
    renderFrame?: unknown,
  ): void;
  pushMidiPanic(renderFrame?: unknown): void;
  pushMidiSysex(destinationId: unknown, bytes: unknown, renderFrame?: unknown): void;
  renderOffline(channels: unknown, blockSize?: unknown, finalize?: unknown): Float32Array[];
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
  setCaptureBuffer(channelsOrCount: unknown, capacityFrames?: unknown): void;
  armCapture(armed?: unknown): void;
  setCapturePunch(startSample: unknown, endSample: unknown, enabled?: unknown): void;
  setCaptureSource(source: unknown): void;
  setRecordOffsetSamples(offsetSamples: unknown): void;
  setInputMonitor(enabled: unknown, gain?: unknown): void;
  captureStatus(): NativeCaptureStatus;
}

interface NativeProject {
  destroy(): void;
  toJson(): string;
  addMidiClip(startPpq: unknown, lengthPpq?: unknown): { trackId: number; clipId: number };
  addTrack(desc?: unknown): number;
  addClip(desc: unknown): number;
  trackCount(): number;
  setTempoSegments(segments: unknown): void;
  setTimeSignatures(segments: unknown): void;
  setMidiEvents(clipId: unknown, events?: unknown): void;
  setWarpMap(map: unknown): void;
  addAutomationLane(trackId: unknown, desc?: unknown): number;
  editAutomationLane(trackId: unknown, targetParamId?: unknown, desc?: unknown): void;
  removeAutomationLane(trackId: unknown, targetParamId?: unknown): void;
  setSampleRate(sampleRate: unknown): void;
  setOverlapPolicy(policy: unknown): void;
  setMarker(markerId: unknown, ppq?: unknown, name?: unknown): number;
  markerByIndex(index: unknown): unknown;
  trackByIndex(index: unknown): unknown;
  clipByIndex(index: unknown): unknown;
  sourceByIndex(index: unknown): unknown;
  tempoSegmentByIndex(index: unknown): unknown;
  timeSignatureByIndex(index: unknown): unknown;
  splitClip(clipId: unknown, ppq?: unknown): number;
  trimClip(clipId: unknown, startPpq?: unknown, lengthPpq?: unknown): void;
  moveClip(clipId: unknown, startPpq?: unknown, trackId?: unknown): void;
  duplicateClip(clipId: unknown, startPpq?: unknown): number;
  removeClip(clipId: unknown): void;
  removeTrack(trackId: unknown): void;
  renameTrack(trackId: unknown, name?: unknown): void;
  setTrackRoute(trackId: unknown, strip?: unknown, output?: unknown): void;
  setTrackKind(trackId: unknown, kind?: unknown): void;
  setTrackGain(trackId: unknown, gain?: unknown): void;
  setTrackPan(trackId: unknown, pan?: unknown): void;
  setTrackMute(trackId: unknown, mute?: unknown): void;
  setTrackSolo(trackId: unknown, solo?: unknown): void;
  setTrackMidiDestination(trackId: unknown, destinationId?: unknown): void;
  setClipGain(clipId: unknown, gain?: unknown): void;
  setClipFade(clipId: unknown, fadeIn?: unknown, fadeOut?: unknown): void;
  setClipLoop(
    clipId: unknown,
    loopMode?: unknown,
    loopLengthPpq?: unknown,
    loopCrossfadePpq?: unknown,
  ): void;
  setClipSource(clipId: unknown, sourceId?: unknown): void;
  setClipTakes(clipId: unknown, takes?: unknown, activeTakeId?: unknown): void;
  setClipCompSegments(clipId: unknown, segments?: unknown): void;
  setClipWarpRef(clipId: unknown, warpMapId?: unknown): void;
  setClipWarpMode(clipId: unknown, warpMode?: unknown): void;
  removeWarpMap(warpMapId: unknown): void;
  setSourceAudio(sourceId: unknown, audio: unknown, channels?: unknown, sampleRate?: unknown): void;
  setAudioSourceMetadata(sourceId: unknown, contentHash?: unknown, stemRole?: unknown): void;
  setProgram(clipId: unknown, program?: unknown, bank?: unknown): void;
  setProgramOnChannel(
    clipId: unknown,
    group?: unknown,
    channel?: unknown,
    program?: unknown,
    bank?: unknown,
  ): void;
  bakeMidiFx(clipId: unknown, config?: unknown): void;
  bakeMidiFxWithSourceIndex(clipId: unknown, config?: unknown): Int32Array;
  previewMidiFxCount(clipId: unknown, config?: unknown): number;
  validateMidiNotes(clipId: unknown): unknown;
  setAssistSidecar(desc: unknown): void;
  getAssistSidecar(index: unknown): unknown;
  setMaxUndoDepth(depth: unknown): void;
  setMaxHistoryBytes(bytes: unknown): void;
  snapToGrid(ppq: unknown, strength?: unknown, division?: unknown): number;
  autoTempo(
    audio: unknown,
    sampleRate?: unknown,
    candidateIndex?: unknown,
    applyTimeSignatures?: unknown,
  ): number;
  analyzeTempo(audio: unknown, sampleRate?: unknown): unknown[];
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
 * A prepared engine parked in a NON-default transport state: stopped, but with
 * the play head moved off zero, a marker table, a registered parameter, one CC
 * binding and an input source. A stopped transport does not advance while the
 * render loop is pumped, so the snapshot below is stable across the pumping that
 * lets a queued command land — which is what makes "unchanged" mean the command
 * was never issued rather than "not applied yet".
 */
const PARK_SAMPLE = 4096;
const MARKER_ID = 3;
const PARAM_ID = 5;

function pump(engine: NativeEngine, blocks = 4): void {
  for (let i = 0; i < blocks; i++) {
    engine.process([new Float32Array(BLOCK), new Float32Array(BLOCK)]);
  }
}

function withPreparedEngine<T>(body: (engine: NativeEngine) => T): T {
  const engine = new addon.RealtimeEngine(SR, BLOCK) as NativeEngine;
  try {
    engine.prepare(SR, BLOCK);
    engine.setMarkers([{ id: MARKER_ID, ppq: 4, name: 'verse', kind: 0 }]);
    engine.addParameter({
      id: PARAM_ID,
      name: 'gain',
      minValue: 0,
      maxValue: 1,
      defaultValue: 0.25,
    });
    engine.bindMidiCc(0, 7, PARAM_ID, 0, 1);
    engine.setMidiInputSource(0);
    engine.play();
    pump(engine);
    engine.seekSample(PARK_SAMPLE);
    pump(engine);
    engine.stop();
    pump(engine);
    return body(engine);
  } finally {
    engine.destroy();
  }
}

/**
 * Everything a rejected transport / MIDI argument used to be able to move: the
 * transport itself (`play` flipped `playing`, `seekSample` reset the position),
 * the pending MIDI input queue (`pushMidiInput*` enqueued an event built from a
 * dummy byte) and the CC binding table (`bindMidiCc` installed a binding).
 */
function transportSnapshot(engine: NativeEngine): string {
  const transport = engine.getTransportState();
  return JSON.stringify({
    playing: transport.playing,
    looping: transport.looping,
    samplePosition: transport.samplePosition,
    ppq: transport.ppq,
    bpm: transport.bpm,
    midiInputPending: engine.midiInputPendingCount(),
    midiCcBindings: engine.midiCcBindingCount(),
  });
}

/**
 * A project parked off its defaults on every field the edit entry points can
 * reach: track gain/pan/mute/route, clip gain/loop/warp/takes, markers, tempo
 * and time-signature maps, an automation lane, MIDI events and a program.
 * `toJson()` serialises all of it, so one string is the whole comparison.
 */
const PROJECT_SR = 48000;
const PROJECT_TRACK_GAIN = 0.5;

interface ProjectFixture {
  project: NativeProject;
  trackId: number;
  clipId: number;
  midiTrackId: number;
  midiClipId: number;
  sourceId: number;
  markerId: number;
  laneParamId: number;
}

function withConfiguredProject<T>(body: (fixture: ProjectFixture) => T): T {
  const project = new addon.Project() as NativeProject;
  try {
    project.setSampleRate(PROJECT_SR);
    const trackId = project.addTrack({ kind: 0, name: 'lead' });
    const audio = new Float32Array(480).map((_, i) => Math.sin(i * 0.05) * 0.25);
    const clipId = project.addClip({
      trackId,
      startPpq: 0,
      lengthPpq: 4,
      gain: 0.8,
      audio,
      audioChannels: 1,
      audioSampleRate: PROJECT_SR,
    });
    const midi = project.addMidiClip(0, 4);
    project.setTrackGain(trackId, PROJECT_TRACK_GAIN);
    project.setTrackPan(trackId, -0.25);
    project.setTrackMute(trackId, true);
    project.setTrackRoute(trackId, 'strip-a', 'out-a');
    project.setTrackMidiDestination(midi.trackId, 3);
    project.setClipGain(clipId, 0.6);
    project.setClipLoop(clipId, 1, 2, 0.25);
    project.setOverlapPolicy(1);
    const markerId = project.setMarker(0, 2, 'verse');
    project.setWarpMap({
      id: 1,
      name: 'w',
      anchors: [
        { warpSample: 0, sourceSample: 0 },
        { warpSample: 100, sourceSample: 120 },
      ],
    });
    project.setClipWarpRef(clipId, 1);
    project.setClipWarpMode(clipId, 1);
    const laneParamId = project.addAutomationLane(trackId, {
      targetParamId: 7,
      points: [
        { ppq: 0, value: 0.25 },
        { ppq: 2, value: 0.75 },
      ],
    });
    project.setClipTakes(
      clipId,
      [
        { id: 1, sourceId: 0, sourceOffsetPpq: 0, name: 'take1' },
        { id: 2, sourceId: 0, sourceOffsetPpq: 0 },
      ],
      2,
    );
    project.setMidiEvents(midi.clipId, [
      [0, (0x2 << 28) | (0x9 << 20) | (60 << 8) | 100, 0],
      [2, (0x2 << 28) | (0x8 << 20) | (60 << 8) | 0, 0],
    ]);
    project.setProgram(midi.clipId, 40, 0);
    project.setTempoSegments([{ startPpq: 0, bpm: 132 }]);
    project.setTimeSignatures([{ startPpq: 0, numerator: 3, denominator: 4 }]);
    project.setAudioSourceMetadata(1, 'hash-abc', 'stem-role');
    project.setAssistSidecar({
      moduleId: 'm',
      schemaVersion: 1,
      targetTrackId: trackId,
      regionStartPpq: 0,
      regionEndPpq: 2,
      payload: new Uint8Array([1, 2, 3]),
    });
    project.setMaxUndoDepth(32);
    project.setMaxHistoryBytes(1 << 20);
    return body({
      project,
      trackId,
      clipId,
      midiTrackId: midi.trackId,
      midiClipId: midi.clipId,
      sourceId: 1,
      markerId,
      laneParamId,
    });
  } finally {
    project.destroy();
  }
}

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
interface BadArgument<T> {
  argument: string;
  call: (target: T) => void;
  error?: ErrorConstructor;
}

const samples = (n = 4): Float32Array => new Float32Array(n);

/** A steady click track, so the tempo entry points have a grid to find. */
const clickTrack = (bpm = 120, seconds = 3, sampleRate = PROJECT_SR): Float32Array => {
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
  {
    name: 'RealtimeEngine.setCaptureBuffer',
    missingRequired: [],
    badArguments: [{ argument: 'numChannels', call: (e) => e.setCaptureBuffer('2', 1024) }],
  },
  {
    name: 'RealtimeEngine.prepare',
    missingRequired: [],
    badTransportArguments: [{ argument: 'sampleRate', call: (e) => e.prepare('48000', BLOCK) }],
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
    badTransportArguments: [{ argument: 'maxRecords', call: (e) => e.drainExternalMidi('16') }],
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
];

/** Proves the process is still usable, not merely that no exception escaped. */
function expectNativeStillWorks(): void {
  expect(withEngine((engine) => engine.graphNodeCount())).toBe(0);
  expect(withProject((project) => project.trackCount())).toBe(0);
}

describe('addon object readers stop at the first bad field', () => {
  for (const { name, rejectsArgument } of CASES) {
    for (const { argument, call, error } of rejectsArgument ?? []) {
      it(`${name}: a wrong-typed ${argument} throws exactly one catchable error`, () => {
        expect(() => call()).toThrow(error ?? TypeError);
        expectNativeStillWorks();
      });
    }
  }

  it('every table entry drives at least one hostile call', () => {
    // A vacuous table would make every assertion below pass by iterating
    // nothing, so pin the shape of the table itself. The coverage register
    // below is what pins its SIZE — a floor would only ever say the table did
    // not shrink, never that it kept up with the addon.
    expect(
      CASES.filter(
        (entry) =>
          entry.missingRequired.length === 0 &&
          entry.badOptional === undefined &&
          (entry.badArguments?.length ?? 0) === 0 &&
          (entry.badTransportArguments?.length ?? 0) === 0 &&
          (entry.badProjectArguments?.length ?? 0) === 0 &&
          (entry.rejectsArgument?.length ?? 0) === 0,
      ).map((entry) => entry.name),
    ).toEqual([]);
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
    for (const { argument, call, error } of badArguments ?? []) {
      it(`${name}: a wrong-typed ${argument} throws and moves no engine state`, () => {
        withConfiguredEngine((engine) => {
          const before = engineStateSnapshot(engine);
          expect(() => call(engine)).toThrow(error ?? TypeError);
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

describe('rejected transport and MIDI arguments issue no command', () => {
  it('parks the prepared engine off its default transport state', () => {
    // The snapshot has to be able to move in both directions, or comparing it
    // either side of a rejected call would assert nothing. This is that proof.
    withPreparedEngine((engine) => {
      const transport = engine.getTransportState();
      expect(transport.playing).toBe(false);
      expect(transport.samplePosition).toBeGreaterThan(0);
      expect(engine.midiCcBindingCount()).toBeGreaterThan(0);
      expect(engine.midiInputPendingCount()).toBe(0);
    });
  });

  it('holds still across pumping, and moves for a well-typed call', () => {
    withPreparedEngine((engine) => {
      const before = transportSnapshot(engine);
      // A stopped transport does not advance, so "unchanged" below means the
      // command was never issued rather than "not applied yet".
      pump(engine);
      expect(transportSnapshot(engine)).toBe(before);

      engine.pushMidiInputNoteOn(0, 0, 60, 100);
      expect(transportSnapshot(engine)).not.toBe(before);
      const afterNote = transportSnapshot(engine);

      engine.bindMidiCc(0, 8, PARAM_ID, 0, 1);
      expect(transportSnapshot(engine)).not.toBe(afterNote);
      const afterBinding = transportSnapshot(engine);

      engine.play();
      pump(engine);
      expect(transportSnapshot(engine)).not.toBe(afterBinding);
      const afterPlay = transportSnapshot(engine);

      engine.seekSample(0);
      pump(engine);
      expect(transportSnapshot(engine)).not.toBe(afterPlay);
    });
  });

  for (const { name, badTransportArguments } of CASES) {
    for (const { argument, call, error } of badTransportArguments ?? []) {
      it(`${name}: a wrong-typed ${argument} throws and issues no command`, () => {
        withPreparedEngine((engine) => {
          const before = transportSnapshot(engine);
          expect(() => call(engine)).toThrow(error ?? TypeError);
          // Pump first: a command that WAS enqueued would land here, so the
          // comparison distinguishes "never issued" from "not yet applied".
          pump(engine);
          expect(transportSnapshot(engine)).toBe(before);
        });
        expectNativeStillWorks();
      });
    }
  }
});

describe('rejected project arguments leave the project byte-identical', () => {
  it('parks the fixture project off its defaults, and moves for a well-typed call', () => {
    withConfiguredProject(({ project, trackId }) => {
      const before = project.toJson();
      expect(before).toContain('"gain":0.5');
      project.setTrackGain(trackId, 0.125);
      expect(project.toJson()).not.toBe(before);
      project.setTrackGain(trackId, PROJECT_TRACK_GAIN);
      expect(project.toJson()).toBe(before);
      // The tempo map is the other half a rejected analysis argument used to
      // rewrite; prove it is reachable too.
      project.autoTempo(clickTrack(), PROJECT_SR, 0, false);
      expect(project.toJson()).not.toBe(before);
    });
  });

  for (const { name, badProjectArguments } of CASES) {
    for (const { argument, call, error } of badProjectArguments ?? []) {
      it(`${name}: a wrong-typed ${argument} throws and leaves the project unchanged`, () => {
        withConfiguredProject((fixture) => {
          const before = fixture.project.toJson();
          expect(() => call(fixture)).toThrow(error ?? TypeError);
          expect(fixture.project.toJson()).toBe(before);
        });
        expectNativeStillWorks();
      });
    }
  }
});

/**
 * Four scans, and between them they define the population this file is
 * responsible for. Stated rather than left to be inferred, because the boundary
 * is the whole question:
 *
 *  1. `positionalReaderDefinitions` — a reader of positional arguments may only
 *     be DEFINED in the shared header. Closes "someone writes another
 *     `Uint32Arg`".
 *  2. `bailoutReaderCalls` — every call to that family must be consumed as
 *     `if (!Reader(...)) return`. Closes "someone calls it and ignores false".
 *  3. `positionalArgEntryPoints` — every entry point reaching that family must
 *     be driven by CASES or registered. Closes "a new entry point uses the
 *     family correctly but is never tested".
 *  4. `inlineTypedArgumentReads` — no entry point may read a positional
 *     argument with `info[i].As<Napi::X>().Value()` unless it type-checks that
 *     index. Closes the gap the first three share: all of them are anchored on
 *     the shared family, so all three are blind to code that simply never uses
 *     it. That is exactly how this class began.
 *
 * What is still OUTSIDE the population, deliberately: the lenient `node_arg_*`
 * family (type-checks and falls back to a default, so it never leaves a pending
 * exception and never hands the C ABI a dummy alongside one), and `.As<Napi::T>()`
 * with no value accessor (an unchecked cast that cannot itself fail). An author
 * can still leave the population by using `node_arg_*`; that is a lenience
 * decision, not an unguarded read, and it is visible in review as one.
 */
describe('the abort-guard table accounts for every rejecting entry point', () => {
  it('self-checks the source scanners, so the registers are not comparing nothing', () => {
    // Every assertion below is a set difference. If a scanner stopped matching,
    // both sides would empty and every one of them would pass vacuously.
    expect(positionalReaderDefinitions().length).toBeGreaterThan(15);
    expect(bailoutReaderCalls().length).toBeGreaterThan(80);
    expect(positionalArgEntryPoints().length).toBeGreaterThan(80);
    // This one reports its whole population, not just its violations, so the
    // floor is what proves a clean sweep swept something.
    expect(inlineTypedArgumentReads().length).toBeGreaterThan(400);
    // Positive control for the bail-out detector: it has to answer "no" to the
    // shape this whole family exists to prevent, or a clean sweep means nothing.
    expect(isBailoutGuarded('  if (!')).toBe(true);
    expect(isBailoutGuarded('  if (!sonare_node::')).toBe(true);
    expect(isBailoutGuarded('      !')).toBe(false);
    expect(isBailoutGuarded('  ThrowIfError(env, sonare_project_set_track_gain(project_, ')).toBe(
      false,
    );
  });

  it('defines no positional-argument reader outside the shared header', () => {
    const unlisted = positionalReaderDefinitions().filter(
      (site) => site.file !== SHARED_READER_FILE && !POSITIONAL_READER_ALLOWLIST.has(site.id),
    );
    expect(
      unlisted.map((site) => `${site.file}:${site.line} ${site.name}(...)`),
      `A reader that takes (Napi::CallbackInfo, index, ...) belongs in ${SHARED_READER_FILE}. ` +
        'Use the Optional*Arg / Required*Arg family there, extend it if the shape you need is ' +
        'missing, or add the site to POSITIONAL_READER_ALLOWLIST with the reason the shared ' +
        'family cannot express it. A file-local copy reads the argument without a pending-' +
        'exception guard and without a bail-out, which is how a wrong-typed argument reached ' +
        'the C ABI as a dummy value and how a second throw aborted the process.',
    ).toEqual([]);
  });

  it('keeps the positional-reader allowlist free of entries that no longer exist', () => {
    const live = new Set(positionalReaderDefinitions().map((site) => site.id));
    expect([...POSITIONAL_READER_ALLOWLIST.keys()].filter((id) => !live.has(id))).toEqual([]);
  });

  it('consumes every bail-out reader call as a bail-out', () => {
    const unguarded = bailoutReaderCalls().filter((site) => !site.guarded);
    expect(
      unguarded.map((site) => `${site.file}:${site.line} ${site.name}(...)`),
      'A bail-out reader returns false without touching its out-parameter, so ignoring that ' +
        'return leaves the caller running on an unread argument with an exception already ' +
        'pending. Every call must read `if (!Reader(...))` and return immediately.',
    ).toEqual([]);
  });

  it('type-checks every inline read of a positional argument', () => {
    const unguarded = inlineTypedArgumentReads().filter(
      (site) => !site.typeChecked && !INLINE_READ_ALLOWLIST.has(site.id),
    );
    expect(
      unguarded.map((site) => `${site.file}:${site.line} ${site.name}`),
      'An `info[i].As<Napi::X>().Value()` with no `info[i].IsX()` check leaves a pending ' +
        'exception and a dummy value when the argument is the wrong type, and the C-ABI call ' +
        'built from it then runs. Read it through the Optional*Arg / Required*Arg family in ' +
        `${SHARED_READER_FILE} and bail out, or type-check the index first. Note that ` +
        'IsUndefined()/IsNull() do not count: every defect in this class was presence-checked ' +
        'and type-blind.',
    ).toEqual([]);
  });

  it('keeps the inline-read allowlist free of entries that no longer exist', () => {
    const live = new Set(
      inlineTypedArgumentReads()
        .filter((site) => !site.typeChecked)
        .map((site) => site.id),
    );
    expect([...INLINE_READ_ALLOWLIST.keys()].filter((id) => !live.has(id))).toEqual([]);
  });

  it('accounts for every entry point that can reject a positional argument', () => {
    const covered = new Set(CASES.map((entry) => entry.name.split('.').pop()));
    const unaccounted = positionalArgEntryPoints().filter(
      (jsName) => !covered.has(jsName) && !UNCOVERED_POSITIONAL_GUARDS.has(jsName),
    );
    expect(
      unaccounted,
      'A new addon entry point that reads a positional argument through the bail-out family ' +
        'must either be driven by CASES or listed in UNCOVERED_POSITIONAL_GUARDS with a reason. ' +
        'Four generations of this finding were each closed by enumerating the entry points that ' +
        'existed at the time, and each time a new same-shaped one arrived that no table named.',
    ).toEqual([]);
  });

  it('keeps the uncovered register free of stale names', () => {
    const live = new Set(positionalArgEntryPoints());
    expect([...UNCOVERED_POSITIONAL_GUARDS.keys()].filter((name) => !live.has(name))).toEqual([]);
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
        swallow(() => e.setCaptureBuffer('2', '1024'));
        // Transport, parameter and MIDI arguments. The MIDI rows use two
        // out-of-byte-range values in a row on purpose: the second reader's
        // throw landing on the first one's pending exception is the abort.
        e.prepare(${SR}, ${BLOCK});
        swallow(() => e.prepare('48000', ${BLOCK}));
        swallow(() => e.play('now'));
        swallow(() => e.stop('now'));
        swallow(() => e.seekSample('x', 'y'));
        swallow(() => e.seekPpq(0, 'x'));
        swallow(() => e.seekMarker('0', 'x'));
        swallow(() => e.countInEndSample('0', '1'));
        swallow(() => e.setParameter(1, 0.5, 'now'));
        swallow(() => e.setParameterSmoothed(1, 0.5, 'now'));
        swallow(() => e.setSoloMute(0, true, false, 'now'));
        swallow(() => e.setTrackMonitorMode(0, 1, 'now'));
        swallow(() => e.bindMidiCc(300, 300, 1, 0, 1));
        swallow(() => e.bindMidiCc('0', 'x', 1, 0, 1));
        swallow(() => e.pushMidiNoteOn(0, 300, 300, 300, 300, 'now'));
        swallow(() => e.pushMidiNoteOff(0, 300, 300, 300, 300, 'now'));
        swallow(() => e.pushMidiCc(0, 300, 300, 300, 300, 'now'));
        swallow(() => e.pushMidiInputNoteOn(300, 300, 300, 300, 'now'));
        swallow(() => e.pushMidiInputNoteOff(300, 300, 300, 300, 'now'));
        swallow(() => e.pushMidiInputCc(300, 300, 300, 300, 'now'));
        swallow(() => e.pushMidiPanic('now'));
        swallow(() => e.pushMidiSysex(0, new Uint8Array([0xf0, 0xf7]), 'now'));
        swallow(() => e.renderOffline([new Float32Array(${BLOCK})], '128', 'yes'));
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
        // Positional project arguments, every one of them a string.
        swallow(() => p.addTrack('audio'));
        swallow(() => p.addMidiClip('0', '4'));
        swallow(() => p.setSampleRate('48000'));
        swallow(() => p.setOverlapPolicy('0'));
        swallow(() => p.setMarker('0', 'x', 'v'));
        swallow(() => p.markerByIndex('0'));
        swallow(() => p.trackByIndex('0'));
        swallow(() => p.clipByIndex('0'));
        swallow(() => p.sourceByIndex('0'));
        swallow(() => p.tempoSegmentByIndex('0'));
        swallow(() => p.timeSignatureByIndex('0'));
        swallow(() => p.splitClip('1', 'x'));
        swallow(() => p.trimClip('1', 'x', 'y'));
        swallow(() => p.moveClip('1', 'x', 'y'));
        swallow(() => p.duplicateClip('1', 'x'));
        swallow(() => p.removeClip('1'));
        swallow(() => p.removeTrack('1'));
        swallow(() => p.renameTrack('1', 'renamed'));
        swallow(() => p.setTrackRoute('1', 'a', 'b'));
        swallow(() => p.setTrackKind('1', 'midi'));
        swallow(() => p.setTrackGain('1', 'loud'));
        swallow(() => p.setTrackPan('1', 'left'));
        swallow(() => p.setTrackMute('1', true));
        swallow(() => p.setTrackSolo('1', true));
        swallow(() => p.setTrackMidiDestination('1', 'x'));
        swallow(() => p.setClipGain('1', 'loud'));
        swallow(() => p.setClipFade('1', {}, {}));
        swallow(() => p.setClipLoop('1', 'x', 'y', 'z'));
        swallow(() => p.setClipSource('1', 'x'));
        swallow(() => p.setClipTakes('1', [], 'x'));
        swallow(() => p.setClipCompSegments('1', []));
        swallow(() => p.setClipWarpRef('1', 'x'));
        swallow(() => p.setClipWarpMode('1', 'tempo-sync'));
        swallow(() => p.removeWarpMap('1'));
        swallow(() => p.setSourceAudio('1', new Float32Array(4), '1', '48000'));
        swallow(() => p.setAudioSourceMetadata('1', 'h', 'r'));
        swallow(() => p.setMidiEvents('1', []));
        swallow(() => p.setProgram('1', 'piano', 'x'));
        swallow(() => p.setProgramOnChannel('1', 300, 300, 'piano', 'x'));
        swallow(() => p.bakeMidiFx('1', '{}'));
        swallow(() => p.bakeMidiFxWithSourceIndex('1', '{}'));
        swallow(() => p.previewMidiFxCount('1', '{}'));
        swallow(() => p.validateMidiNotes('1'));
        swallow(() => p.addAutomationLane('bad', 'bad2'));
        swallow(() => p.editAutomationLane('bad', 'bad2', 'not-an-object'));
        swallow(() => p.removeAutomationLane('bad', 'bad2'));
        swallow(() => p.getAssistSidecar('0'));
        swallow(() => p.setMaxUndoDepth('8'));
        swallow(() => p.setMaxHistoryBytes('1024'));
        swallow(() => p.snapToGrid('1', 'x', 'y'));
        swallow(() => p.autoTempo(new Float32Array(4096), '48000', '0', false));
        swallow(() => p.analyzeTempo(new Float32Array(4096), '48000'));
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
