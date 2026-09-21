/**
 * The native handles the abort-guard tests drive, and the fixtures that put
 * one into a known state.
 *
 * The typed surfaces here are deliberately structural rather than the addon's
 * own declarations: a test that reached for those would be asserting against
 * the same types the addon claims, and the whole point is to call it with
 * arguments those types forbid. The snapshots are what prove a rejected call
 * changed nothing, so they read observable state back rather than trusting a
 * setter's return.
 */

import { addon } from '../src/native.js';

export const SR = 48000;
export const BLOCK = 128;

export interface NativeCaptureStatus {
  capturedFrames: number;
  overflowCount: number;
  armed: boolean;
  punchEnabled: boolean;
  source: string;
  recordOffsetSamples: number;
}

export interface NativeTransportState {
  playing: boolean;
  looping: boolean;
  samplePosition: number;
  ppq: number;
  bpm: number;
}

export interface NativeEngine {
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
  pushMidiPitchBend(
    destinationId: unknown,
    group: unknown,
    channel: unknown,
    bend14: unknown,
    renderFrame?: unknown,
  ): void;
  pushMidiChannelPressure(
    destinationId: unknown,
    group: unknown,
    channel: unknown,
    pressure: unknown,
    renderFrame?: unknown,
  ): void;
  pushMidiPolyPressure(
    destinationId: unknown,
    group: unknown,
    channel: unknown,
    note: unknown,
    pressure: unknown,
    renderFrame?: unknown,
  ): void;
  pushMidiInputPitchBend(
    group: unknown,
    channel: unknown,
    bend14: unknown,
    portTimeSamples?: unknown,
  ): void;
  pushMidiInputChannelPressure(
    group: unknown,
    channel: unknown,
    pressure: unknown,
    portTimeSamples?: unknown,
  ): void;
  pushMidiInputPolyPressure(
    group: unknown,
    channel: unknown,
    note: unknown,
    pressure: unknown,
    portTimeSamples?: unknown,
  ): void;
  setArticulation(destinationId: unknown, channel: unknown, articulation: unknown): void;
  articulation(destinationId: unknown, channel: unknown): unknown;
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

export interface NativeSampleBank {
  destroy(): void;
  addSample(data: unknown, desc?: unknown): number;
  addZone(setIndex: unknown, zone?: unknown): void;
  sampleCount(): number;
  setCount(): number;
}

export interface NativeProject {
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
  setMidiFx(clipId: unknown, config?: unknown): void;
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
export function withEngine<T>(body: (engine: NativeEngine) => T): T {
  const engine = new addon.RealtimeEngine(SR, BLOCK) as NativeEngine;
  try {
    return body(engine);
  } finally {
    engine.destroy();
  }
}

export function withProject<T>(body: (project: NativeProject) => T): T {
  const project = new addon.Project() as NativeProject;
  try {
    return body(project);
  } finally {
    project.destroy();
  }
}

export function withSampleBank<T>(body: (bank: NativeSampleBank) => T): T {
  const bank = new addon.SampleBank() as NativeSampleBank;
  try {
    return body(bank);
  } finally {
    bank.destroy();
  }
}

/**
 * A track lane, a bus and the master strip, each carrying one `eq.parametric`
 * insert, plus a capture session parked in a NON-default state. Every field the
 * snapshot below reads is deliberately moved off its zero value, so a call that
 * reaches the C ABI with a dummy argument moves the snapshot instead of landing
 * on the value it already had.
 */
export const TRACK_ID = 10;
export const BUS_ID = 1;
export const PREFETCH_FRAMES = 4096;
export const RECORD_OFFSET = 64;

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

export const trackStripJson = (gainDb = 0) =>
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

export function withConfiguredEngine<T>(body: (engine: NativeEngine) => T): T {
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
export function engineStateSnapshot(engine: NativeEngine): string {
  return JSON.stringify({
    capture: engine.captureStatus(),
    prefetchFrames: engine.clipPagePrefetchFrames(),
    trackInsertId: engine.resolveTrackInsertAutomationId(TRACK_ID, 0, 'band0.gainDb'),
    busInsertId: engine.resolveBusInsertAutomationId(BUS_ID, 0, 'band0.gainDb'),
    masterInsertId: engine.resolveMasterInsertAutomationId(0, 'band0.gainDb'),
    clipCount: engine.clipCount(),
  });
}

export const rms = (block: Float32Array): number =>
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
export const MARKER_ID = 3;
export const PARAM_ID = 5;

export function pump(engine: NativeEngine, blocks = 4): void {
  for (let i = 0; i < blocks; i++) {
    engine.process([new Float32Array(BLOCK), new Float32Array(BLOCK)]);
  }
}

export function withPreparedEngine<T>(body: (engine: NativeEngine) => T): T {
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
export function transportSnapshot(engine: NativeEngine): string {
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
export const PROJECT_SR = 48000;
export const PROJECT_TRACK_GAIN = 0.5;

export interface ProjectFixture {
  project: NativeProject;
  trackId: number;
  clipId: number;
  midiTrackId: number;
  midiClipId: number;
  sourceId: number;
  markerId: number;
  laneParamId: number;
}

export function withConfiguredProject<T>(body: (fixture: ProjectFixture) => T): T {
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
