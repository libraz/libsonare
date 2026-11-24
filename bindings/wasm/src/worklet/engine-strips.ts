import { panLawCode, panModeCode } from '../codes.js';
import { ErrorCode } from '../errors.js';
import type {
  Articulation,
  BuiltinSynthConfig,
  EqBandInput,
  PanLaw,
  PanMode,
  RealtimeEngine,
  Sf2InstrumentConfig,
  SynthPatch,
  UmpWords,
} from '../index.js';
import { codedError, refuse, refuseType } from '../internal/entry.js';
import { eqBandJson } from '../internal/eq_band_json.js';
import { packMidi2ProgramRequest } from '../internal/midi_program_pack.js';
import {
  assertMidiBankShape,
  assertMidiByteShape,
  assertMidiNumberShapes,
  assertMidiTimingShape,
  assertMidiTrackIdShape,
  assertMidiWordListShapes,
} from '../internal/midi_shape.js';
import { boundedUmpWords } from '../internal/ump_events.js';
import type { SonareEngineSyncMessage } from '../internal/worklet_messages.js';
import type { SurroundPan } from '../public_types.js';
import type { MidiFxConfig } from '../public_types_engine.js';
import type { RequestFn } from '../request_keys.js';
import { requestObject, resolveRenderFrame } from '../validation.js';
import { commitSync } from './engine-commit.js';
import {
  type MidiTrackTiming,
  resolveMidiTrackTiming,
  type SonareEngineMidiTimingOptions,
} from './engine-midi-input.js';
import type { InsertParamOverrideMap } from './engine-mixer-facade.js';
import {
  emptyStripJson,
  type InsertParamOverride,
  insertParamOverrideKey,
  replaceStripScene,
  type StripJsonTarget,
} from './engine-mixer-facade.js';

/**
 * Collaborator surface the strip/pan/EQ/insert/instrument/MIDI setters need from
 * the owning {@link SonareEngine}: the offline engine they mirror writes into,
 * the out-of-band sync posters, and the lane-resolution helpers.
 */
export interface EngineStripContext {
  readonly offlineEngine: RealtimeEngine;
  readonly trackLaneIds: number[];
  postSync(message: SonareEngineSyncMessage): void;
  ensureTrackLane(target: string | number): number;
  resolveTargetId(target: string | number): number;
  readStripJson(target: StripJsonTarget): string | undefined;
  writeStripJson(target: StripJsonTarget, settingsJson: string): void;
  readonly insertParamOverrides: InsertParamOverrideMap;
  clearInsertAutomationLanes(target: StripJsonTarget): void;
}

function trackIdFor(ctx: EngineStripContext, target: string | number): number {
  return ctx.trackLaneIds[ctx.ensureTrackLane(target)];
}

type SceneEntry = Record<string, unknown>;

/**
 * Folds a setter's change into the strip's cached settings JSON, so the next
 * syncMixer re-post replays it instead of reverting it on the worklet.
 */
function mergeStripJson(
  ctx: EngineStripContext,
  target: StripJsonTarget,
  update: (entry: SceneEntry) => void,
): void {
  const entry = JSON.parse(ctx.readStripJson(target) ?? emptyStripJson(target)) as SceneEntry;
  update(entry);
  ctx.writeStripJson(target, JSON.stringify(entry));
}

function mergeEqBand(
  ctx: EngineStripContext,
  target: StripJsonTarget,
  bandIndex: number,
  bandJson: string,
): void {
  mergeStripJson(ctx, target, (entry) => {
    const eq = (entry.eq ?? { enabled: true, bands: [] }) as { bands?: unknown[] };
    const bands = eq.bands ?? [];
    while (bands.length < bandIndex) {
      bands.push({});
    }
    bands[bandIndex] = JSON.parse(bandJson);
    entry.eq = { ...eq, bands };
  });
}

function setInsertParamByName(
  ctx: EngineStripContext,
  target: StripJsonTarget,
  insertIndex: number,
  paramName: string,
  value: number,
  applyNative: () => void,
  message: SonareEngineSyncMessage,
): void {
  applyNative();
  // Keep the construction scene unchanged. Rewriting insert params here makes
  // a later syncMixer rebuild stateful effects; the override is replayed after
  // the scene on both engines instead.
  const override: InsertParamOverride = { target, insertIndex, paramName, value };
  ctx.insertParamOverrides.set(insertParamOverrideKey(override), override);
  ctx.postSync(message);
}

export function setTrackStripJson(
  ctx: EngineStripContext,
  trackId: number,
  settingsJson: string,
  trackStripJson: Map<number, string>,
): { insertBaseResets: StripJsonTarget[]; constructionOverrides: InsertParamOverride[] } {
  const target: StripJsonTarget = { kind: 'track', trackId };
  const result = replaceStripScene(ctx, target, trackStripJson.get(trackId), settingsJson);
  trackStripJson.set(trackId, settingsJson);
  return result;
}

/** Refuses `'master'` for a setter the master strip has no form of. */
function trackTarget(ctx: EngineStripContext, target: string | number): number {
  if (target === 'master') {
    throw refuse(`the master strip has no such setter`);
  }
  return trackIdFor(ctx, target);
}

function requireBand(band: unknown): asserts band is EqBandInput {
  if (typeof band !== 'object' || band === null) {
    throw refuseType(`band must be an object`);
  }
}

/** The bypass state as `{ bypassed, resetOnBypass }`, refusing any other shape. */
function bypassState(
  method: RequestFn,
  state: { bypassed: boolean; resetOnBypass?: boolean },
): { bypassed: boolean; resetOnBypass: boolean } {
  requestObject(method, state, 'state', true);
  return { bypassed: state.bypassed, resetOnBypass: state.resetOnBypass ?? false };
}

/** The insert parameter as `{ name, value }`, refusing any other shape. */
function insertParam(
  method: RequestFn,
  param: { name: string; value: number },
): { name: string; value: number } {
  requestObject(method, param, 'param', true);
  return { name: param.name, value: param.value };
}

export function setStripEqBand(
  ctx: EngineStripContext,
  target: string | number,
  bandIndex: number,
  band: EqBandInput,
): void {
  requireBand(band);
  if (target === 'master') {
    const bandJson = eqBandJson(band);
    commitSync(
      ctx,
      (offline) => offline.setStripEqBand('master', bandIndex, band),
      () => {
        mergeEqBand(ctx, { kind: 'master' }, bandIndex, bandJson);
        ctx.postSync({ type: 'syncMasterStripEqBand', bandIndex, bandJson });
      },
    );
    return;
  }
  const trackId = trackIdFor(ctx, target);
  commitSync(
    ctx,
    (offline) => offline.setStripEqBand(trackId, bandIndex, band),
    () => {
      const bandJson = eqBandJson(band);
      mergeEqBand(ctx, { kind: 'track', trackId }, bandIndex, bandJson);
      ctx.postSync({ type: 'syncTrackStripEqBand', trackId, bandIndex, bandJson });
    },
  );
}

export function setBusEqBand(
  ctx: EngineStripContext,
  busId: number,
  bandIndex: number,
  band: EqBandInput,
): void {
  requireBand(band);
  commitSync(
    ctx,
    (offline) => offline.setBusEqBand(busId, bandIndex, band),
    () => {
      const bandJson = eqBandJson(band);
      mergeEqBand(ctx, { kind: 'bus', busId }, bandIndex, bandJson);
      ctx.postSync({ type: 'syncBusStripEqBand', busId, bandIndex, bandJson });
    },
  );
}

export function setStripInsertBypassed(
  ctx: EngineStripContext,
  target: string | number,
  insertIndex: number,
  state: { bypassed: boolean; resetOnBypass?: boolean },
): void {
  const { bypassed, resetOnBypass } = bypassState('setStripInsertBypassed', state);
  if (target === 'master') {
    commitSync(
      ctx,
      (offline) => offline.setStripInsertBypassed('master', insertIndex, state),
      () => {
        ctx.postSync({
          type: 'syncMasterStripInsertBypassed',
          insertIndex,
          bypassed,
          resetOnBypass,
        });
      },
    );
    return;
  }
  const trackId = trackIdFor(ctx, target);
  commitSync(
    ctx,
    (offline) => offline.setStripInsertBypassed(trackId, insertIndex, state),
    () => {
      ctx.postSync({
        type: 'syncTrackStripInsertBypassed',
        trackId,
        insertIndex,
        bypassed,
        resetOnBypass,
      });
    },
  );
}

export function setBusInsertBypassed(
  ctx: EngineStripContext,
  busId: number,
  insertIndex: number,
  state: { bypassed: boolean; resetOnBypass?: boolean },
): void {
  const { bypassed, resetOnBypass } = bypassState('setBusInsertBypassed', state);
  commitSync(
    ctx,
    (offline) => offline.setBusInsertBypassed(busId, insertIndex, state),
    () => {
      ctx.postSync({
        type: 'syncBusStripInsertBypassed',
        busId,
        insertIndex,
        bypassed,
        resetOnBypass,
      });
    },
  );
}

export function setStripInsertParam(
  ctx: EngineStripContext,
  target: string | number,
  insertIndex: number,
  param: { name: string; value: number },
): void {
  const { name, value } = insertParam('setStripInsertParam', param);
  if (target === 'master') {
    setInsertParamByName(
      ctx,
      { kind: 'master' },
      insertIndex,
      name,
      value,
      () => ctx.offlineEngine.restoreStripInsertParam('master', insertIndex, param),
      { type: 'syncMasterStripInsertParamByName', insertIndex, paramName: name, value },
    );
    return;
  }
  const trackId = trackIdFor(ctx, target);
  setInsertParamByName(
    ctx,
    { kind: 'track', trackId },
    insertIndex,
    name,
    value,
    () => ctx.offlineEngine.restoreStripInsertParam(trackId, insertIndex, param),
    { type: 'syncTrackStripInsertParamByName', trackId, insertIndex, paramName: name, value },
  );
}

export function setBusInsertParam(
  ctx: EngineStripContext,
  busId: number,
  insertIndex: number,
  param: { name: string; value: number },
): void {
  const { name, value } = insertParam('setBusInsertParam', param);
  setInsertParamByName(
    ctx,
    { kind: 'bus', busId },
    insertIndex,
    name,
    value,
    () => ctx.offlineEngine.restoreBusInsertParam(busId, insertIndex, param),
    { type: 'syncBusStripInsertParamByName', busId, insertIndex, paramName: name, value },
  );
}

export function setStripPanLaw(
  ctx: EngineStripContext,
  target: string | number,
  panLaw: PanLaw,
): void {
  const trackId = trackTarget(ctx, target);
  const code = panLawCode(panLaw, 'panLaw');
  commitSync(
    ctx,
    (offline) => offline.setStripPanLaw(trackId, panLaw),
    () => {
      mergeStripJson(ctx, { kind: 'track', trackId }, (entry) => {
        entry.panLaw = panLaw;
      });
      ctx.postSync({ type: 'syncTrackStripPanLaw', trackId, panLaw: code });
    },
  );
}

export function setStripPanMode(
  ctx: EngineStripContext,
  target: string | number,
  panMode: PanMode,
): void {
  const trackId = trackTarget(ctx, target);
  const code = panModeCode(panMode, 'panMode');
  commitSync(
    ctx,
    (offline) => offline.setStripPanMode(trackId, panMode),
    () => {
      mergeStripJson(ctx, { kind: 'track', trackId }, (entry) => {
        entry.panMode = panMode;
      });
      ctx.postSync({ type: 'syncTrackStripPanMode', trackId, panMode: code });
    },
  );
}

export function setStripDualPan(
  ctx: EngineStripContext,
  target: string | number,
  leftPan: number,
  rightPan: number,
): void {
  const trackId = trackTarget(ctx, target);
  commitSync(
    ctx,
    (offline) => offline.setStripDualPan(trackId, leftPan, rightPan),
    () => {
      mergeStripJson(ctx, { kind: 'track', trackId }, (entry) => {
        entry.dualPanLeft = leftPan;
        entry.dualPanRight = rightPan;
      });
      ctx.postSync({ type: 'syncTrackStripDualPan', trackId, leftPan, rightPan });
    },
  );
}

export function setStripSurroundPan(
  ctx: EngineStripContext,
  target: string | number,
  pan: SurroundPan,
): void {
  const trackId = trackTarget(ctx, target);
  commitSync(
    ctx,
    (offline) => offline.setStripSurroundPan(trackId, pan),
    () => {
      const resolved = {
        azimuth: pan.azimuth ?? 0,
        divergence: pan.divergence ?? 0,
        lfe: pan.lfe ?? 0,
      };
      mergeStripJson(ctx, { kind: 'track', trackId }, (entry) => {
        entry.surroundPan = { ...resolved };
      });
      ctx.postSync({ type: 'syncTrackStripSurroundPan', trackId, pan: resolved });
    },
  );
}

export function setStripChannelDelaySamples(
  ctx: EngineStripContext,
  target: string | number,
  delaySamples: number,
): void {
  const trackId = trackTarget(ctx, target);
  commitSync(
    ctx,
    (offline) => offline.setStripChannelDelaySamples(trackId, delaySamples),
    () => {
      mergeStripJson(ctx, { kind: 'track', trackId }, (entry) => {
        entry.channelDelaySamples = delaySamples;
      });
      ctx.postSync({ type: 'syncTrackStripChannelDelaySamples', trackId, delaySamples });
    },
  );
}

/**
 * Records a master fader or pan value the parameter path applied, so a master
 * strip re-post does not reset it; a track lane keeps these outside its strip.
 */
export function cacheMasterStripScalar(
  ctx: EngineStripContext,
  field: 'faderDb' | 'pan',
  value: number,
): void {
  mergeStripJson(ctx, { kind: 'master' }, (entry) => {
    entry[field] = value;
  });
}

export function setBusPan(ctx: EngineStripContext, busId: number, pan: number): void {
  commitSync(
    ctx,
    (offline) => offline.setBusPan(busId, pan),
    () => {
      mergeStripJson(ctx, { kind: 'bus', busId }, (entry) => {
        entry.pan = pan;
      });
      ctx.postSync({ type: 'syncBusStripPan', busId, pan });
    },
  );
}

export function setBusPanLaw(ctx: EngineStripContext, busId: number, panLaw: PanLaw): void {
  const code = panLawCode(panLaw, 'panLaw');
  commitSync(
    ctx,
    (offline) => offline.setBusPanLaw(busId, panLaw),
    () => {
      mergeStripJson(ctx, { kind: 'bus', busId }, (entry) => {
        entry.panLaw = panLaw;
      });
      ctx.postSync({ type: 'syncBusStripPanLaw', busId, panLaw: code });
    },
  );
}

export function setBusPanMode(ctx: EngineStripContext, busId: number, panMode: PanMode): void {
  const code = panModeCode(panMode, 'panMode');
  commitSync(
    ctx,
    (offline) => offline.setBusPanMode(busId, panMode),
    () => {
      mergeStripJson(ctx, { kind: 'bus', busId }, (entry) => {
        entry.panMode = panMode;
      });
      ctx.postSync({ type: 'syncBusStripPanMode', busId, panMode: code });
    },
  );
}

export function setBusDualPan(
  ctx: EngineStripContext,
  busId: number,
  leftPan: number,
  rightPan: number,
): void {
  commitSync(
    ctx,
    (offline) => offline.setBusDualPan(busId, leftPan, rightPan),
    () => {
      mergeStripJson(ctx, { kind: 'bus', busId }, (entry) => {
        entry.dualPanLeft = leftPan;
        entry.dualPanRight = rightPan;
      });
      ctx.postSync({ type: 'syncBusStripDualPan', busId, leftPan, rightPan });
    },
  );
}

export function pushMidiNoteOn(
  ctx: EngineStripContext,
  trackId: string | number,
  group: number,
  channel: number,
  note: number,
  velocity: number,
  renderFrame: number | undefined,
  options: SonareEngineMidiTimingOptions | undefined,
): void {
  assertMidiTrackIdShape(trackId);
  assertMidiNumberShapes([
    ['group', group],
    ['channel', channel],
    ['note', note],
    ['velocity', velocity],
  ]);
  assertMidiTimingShape('renderFrame', renderFrame, options);
  const timing = resolveMidiTrackTiming('SonareEngine.pushMidiNoteOn', renderFrame, options);
  const destinationId = ctx.resolveTargetId(trackId);
  ctx.offlineEngine.pushMidiNoteOn(destinationId, group, channel, note, velocity, renderFrame);
  ctx.postSync({
    type: 'syncMidiNoteOn',
    destinationId,
    group,
    channel,
    note,
    velocity,
    ...timing,
  });
}

export function pushMidiNoteOff(
  ctx: EngineStripContext,
  trackId: string | number,
  group: number,
  channel: number,
  note: number,
  velocity: number,
  renderFrame: number | undefined,
  options: SonareEngineMidiTimingOptions | undefined,
): void {
  assertMidiTrackIdShape(trackId);
  assertMidiNumberShapes([
    ['group', group],
    ['channel', channel],
    ['note', note],
    ['velocity', velocity],
  ]);
  assertMidiTimingShape('renderFrame', renderFrame, options);
  const timing = resolveMidiTrackTiming('SonareEngine.pushMidiNoteOff', renderFrame, options);
  const destinationId = ctx.resolveTargetId(trackId);
  ctx.offlineEngine.pushMidiNoteOff(destinationId, group, channel, note, velocity, renderFrame);
  ctx.postSync({
    type: 'syncMidiNoteOff',
    destinationId,
    group,
    channel,
    note,
    velocity,
    ...timing,
  });
}

export function pushMidiCc(
  ctx: EngineStripContext,
  trackId: string | number,
  group: number,
  channel: number,
  controller: number,
  value: number,
  renderFrame: number | undefined,
  options: SonareEngineMidiTimingOptions | undefined,
): void {
  assertMidiTrackIdShape(trackId);
  assertMidiNumberShapes([
    ['group', group],
    ['channel', channel],
    ['controller', controller],
    ['value', value],
  ]);
  assertMidiTimingShape('renderFrame', renderFrame, options);
  const timing = resolveMidiTrackTiming('SonareEngine.pushMidiCc', renderFrame, options);
  const destinationId = ctx.resolveTargetId(trackId);
  ctx.offlineEngine.pushMidiCc(destinationId, group, channel, controller, value, renderFrame);
  ctx.postSync({
    type: 'syncMidiCc',
    destinationId,
    group,
    channel,
    controller,
    value,
    ...timing,
  });
}

export function pushMidiPitchBend(
  ctx: EngineStripContext,
  trackId: string | number,
  group: number,
  channel: number,
  bend14: number,
  renderFrame: number | undefined,
  options: SonareEngineMidiTimingOptions | undefined,
): void {
  assertMidiTrackIdShape(trackId);
  assertMidiNumberShapes([
    ['group', group],
    ['channel', channel],
    ['bend14', bend14],
  ]);
  assertMidiTimingShape('renderFrame', renderFrame, options);
  const timing = resolveMidiTrackTiming('SonareEngine.pushMidiPitchBend', renderFrame, options);
  const destinationId = ctx.resolveTargetId(trackId);
  ctx.offlineEngine.pushMidiPitchBend(destinationId, group, channel, bend14, renderFrame);
  ctx.postSync({
    type: 'syncMidiPitchBend',
    destinationId,
    group,
    channel,
    data0: bend14,
    data1: 0,
    ...timing,
  });
}

export function pushMidiChannelPressure(
  ctx: EngineStripContext,
  trackId: string | number,
  group: number,
  channel: number,
  pressure: number,
  renderFrame: number | undefined,
  options: SonareEngineMidiTimingOptions | undefined,
): void {
  assertMidiTrackIdShape(trackId);
  assertMidiNumberShapes([
    ['group', group],
    ['channel', channel],
    ['pressure', pressure],
  ]);
  assertMidiTimingShape('renderFrame', renderFrame, options);
  const timing = resolveMidiTrackTiming(
    'SonareEngine.pushMidiChannelPressure',
    renderFrame,
    options,
  );
  const destinationId = ctx.resolveTargetId(trackId);
  ctx.offlineEngine.pushMidiChannelPressure(destinationId, group, channel, pressure, renderFrame);
  ctx.postSync({
    type: 'syncMidiChannelPressure',
    destinationId,
    group,
    channel,
    data0: pressure,
    data1: 0,
    ...timing,
  });
}

export function pushMidiPolyPressure(
  ctx: EngineStripContext,
  trackId: string | number,
  group: number,
  channel: number,
  note: number,
  pressure: number,
  renderFrame: number | undefined,
  options: SonareEngineMidiTimingOptions | undefined,
): void {
  assertMidiTrackIdShape(trackId);
  assertMidiNumberShapes([
    ['group', group],
    ['channel', channel],
    ['note', note],
    ['pressure', pressure],
  ]);
  assertMidiTimingShape('renderFrame', renderFrame, options);
  const timing = resolveMidiTrackTiming('SonareEngine.pushMidiPolyPressure', renderFrame, options);
  const destinationId = ctx.resolveTargetId(trackId);
  ctx.offlineEngine.pushMidiPolyPressure(
    destinationId,
    group,
    channel,
    note,
    pressure,
    renderFrame,
  );
  ctx.postSync({
    type: 'syncMidiPolyPressure',
    destinationId,
    group,
    channel,
    data0: note,
    data1: pressure,
    ...timing,
  });
}

export function pushMidiUmp(
  ctx: EngineStripContext,
  trackId: string | number,
  umpWords: UmpWords,
  renderFrame: number | undefined,
  options: SonareEngineMidiTimingOptions | undefined,
): void {
  assertMidiTrackIdShape(trackId);
  assertMidiWordListShapes(umpWords);
  assertMidiTimingShape('renderFrame', renderFrame, options);
  const timing = resolveMidiTrackTiming('SonareEngine.pushMidiUmp', renderFrame, options);
  // A copy, so a caller reusing its buffer cannot change what was posted; the
  // offline engine refuses anything that is not a word array before the post.
  const words =
    umpWords instanceof Uint32Array
      ? new Uint32Array(umpWords)
      : Array.isArray(umpWords)
        ? boundedUmpWords(umpWords).slice()
        : umpWords;
  postMidiUmp(ctx, trackId, words, renderFrame, timing);
}

/** Queues validated UMP words on the offline mirror and posts them with their resolved timing. */
function postMidiUmp(
  ctx: EngineStripContext,
  trackId: string | number,
  words: UmpWords,
  renderFrame: number | undefined,
  timing: MidiTrackTiming,
): void {
  const destinationId = ctx.resolveTargetId(trackId);
  ctx.offlineEngine.pushMidiUmp(destinationId, words, renderFrame);
  ctx.postSync({ type: 'syncMidiUmp', destinationId, words, ...timing });
}

export function pushMidiProgram(
  ctx: EngineStripContext,
  trackId: string | number,
  group: number,
  channel: number,
  program: number,
  bank: { msb: number; lsb: number } | undefined,
  renderFrame: number | undefined,
  options: SonareEngineMidiTimingOptions | undefined,
): void {
  assertMidiTrackIdShape(trackId);
  assertMidiNumberShapes([
    ['group', group],
    ['channel', channel],
    ['program', program],
  ]);
  assertMidiBankShape(bank);
  assertMidiTimingShape('renderFrame', renderFrame, options);
  const timing = resolveMidiTrackTiming('SonareEngine.pushMidiProgram', renderFrame, options);
  const words = packMidi2ProgramRequest('pushMidiProgram', group, channel, program, bank);
  postMidiUmp(ctx, trackId, words, renderFrame, timing);
}

export function setBuiltinInstrument(
  ctx: EngineStripContext,
  trackId: string | number,
  config: Omit<BuiltinSynthConfig, 'destinationId'>,
): void {
  const destinationId = ctx.resolveTargetId(trackId);
  commitSync(
    ctx,
    (offline) => offline.setBuiltinInstrument(destinationId, config),
    () => {
      ctx.postSync({ type: 'syncBuiltinInstrument', destinationId, config: { ...config } });
    },
  );
}

export function setSynthInstrument(
  ctx: EngineStripContext,
  trackId: string | number,
  patch: SynthPatch,
): void {
  const destinationId = ctx.resolveTargetId(trackId);
  // A SampleBank handle cannot cross to the worklet, so refuse it before the
  // offline engine changes rather than let the live delivery fail later.
  if (typeof patch === 'object' && patch !== null && patch.sampleBank != null) {
    throw codedError(
      ErrorCode.NotSupported,
      'a patch with sampleBank cannot be delivered to the AudioWorklet engine',
    );
  }
  commitSync(
    ctx,
    (offline) => offline.setSynthInstrument(destinationId, patch),
    () => {
      ctx.postSync({
        type: 'syncSynthInstrument',
        destinationId,
        patch: { ...patch },
      });
    },
  );
}

export function setArticulation(
  ctx: EngineStripContext,
  trackId: string | number,
  channel: number,
  articulation: Articulation,
): void {
  const destinationId = ctx.resolveTargetId(trackId);
  commitSync(
    ctx,
    (offline) => offline.setArticulation(destinationId, channel, articulation),
    () => {
      ctx.postSync({ type: 'syncArticulation', destinationId, channel, articulation });
    },
  );
}

export function loadSoundFont(ctx: EngineStripContext, data: Uint8Array): void {
  ctx.offlineEngine.loadSoundFont(data);
  ctx.postSync({ type: 'syncLoadSoundFont', data });
}

export function setSf2Instrument(
  ctx: EngineStripContext,
  trackId: string | number,
  config: Omit<Sf2InstrumentConfig, 'destinationId'>,
): void {
  const destinationId = ctx.resolveTargetId(trackId);
  commitSync(
    ctx,
    (offline) => offline.setSf2Instrument(destinationId, config),
    () => {
      ctx.postSync({ type: 'syncSf2Instrument', destinationId, config: { ...config } });
    },
  );
}

export function setMidiDestinationExternal(
  ctx: EngineStripContext,
  trackId: string | number,
  midiDestinationExternal: boolean,
): void {
  const destinationId = ctx.resolveTargetId(trackId);
  commitSync(
    ctx,
    (offline) => offline.setMidiDestinationExternal(destinationId, midiDestinationExternal),
    () => {
      ctx.postSync({
        type: 'syncMidiDestinationExternal',
        destinationId,
        external: midiDestinationExternal,
      });
    },
  );
}

export function setExternalMidiClockEnabled(
  ctx: EngineStripContext,
  externalMidiClockEnabled: boolean,
): void {
  commitSync(
    ctx,
    (offline) => offline.setExternalMidiClockEnabled(externalMidiClockEnabled),
    () => {
      ctx.postSync({ type: 'syncExternalMidiClock', enabled: externalMidiClockEnabled });
    },
  );
}

export function setMidiFx(
  ctx: EngineStripContext,
  trackId: string | number,
  config: MidiFxConfig,
): void {
  const destinationId = ctx.resolveTargetId(trackId);
  commitSync(
    ctx,
    (offline) => offline.setMidiFx(destinationId, config),
    () => {
      ctx.postSync({ type: 'syncMidiFx', destinationId, config });
    },
  );
}

export function clearMidiFx(ctx: EngineStripContext, trackId: string | number): void {
  const destinationId = ctx.resolveTargetId(trackId);
  commitSync(
    ctx,
    (offline) => offline.clearMidiFx(destinationId),
    () => {
      ctx.postSync({ type: 'syncClearMidiFx', destinationId });
    },
  );
}

export function pushMidiSysex(
  ctx: EngineStripContext,
  trackId: string | number,
  data: Uint8Array,
  renderFrame: number | undefined,
  options: SonareEngineMidiTimingOptions | undefined,
): void {
  assertMidiTrackIdShape(trackId);
  assertMidiByteShape(data, 'data');
  assertMidiTimingShape('renderFrame', renderFrame, options);
  const timing = resolveMidiTrackTiming('SonareEngine.pushMidiSysex', renderFrame, options);
  const destinationId = ctx.resolveTargetId(trackId);
  ctx.offlineEngine.pushMidiSysex(destinationId, data, renderFrame);
  ctx.postSync({
    type: 'syncMidiSysex',
    destinationId,
    data,
    ...timing,
  });
}

export function pushMidiPanic(ctx: EngineStripContext, renderFrame: number | undefined): void {
  ctx.offlineEngine.pushMidiPanic(renderFrame);
  ctx.postSync({
    type: 'syncMidiPanic',
    renderFrame: resolveRenderFrame(renderFrame),
  });
}
