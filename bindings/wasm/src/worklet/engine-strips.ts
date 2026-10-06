import { panLawCode, panModeCode } from '../codes';
import type { EqBand, PanLawInput, PanMode, RealtimeEngine, UmpWords } from '../index';
import type { InsertParamOverrideMap } from './engine-mixer-facade';
import {
  emptyStripJson,
  type InsertParamOverride,
  insertParamOverrideKey,
  replaceStripScene,
  type StripJsonTarget,
} from './engine-mixer-facade';
import type { SonareEngineInstrumentSyncMessage, SonareEngineSyncMessage } from './messages';

/**
 * Collaborator surface the strip/pan/EQ/insert/instrument/MIDI setters need from
 * the owning {@link SonareEngine}: the offline engine they mirror writes into,
 * the out-of-band sync posters, and the lane-resolution helpers.
 */
export interface EngineStripContext {
  readonly offlineEngine: RealtimeEngine;
  readonly trackLaneIds: number[];
  postSync(message: SonareEngineSyncMessage): void;
  postInstrumentSync(message: SonareEngineInstrumentSyncMessage): void;
  ensureTrackLane(target: string | number): number;
  resolveTargetId(target: string | number): number;
  readStripJson(target: StripJsonTarget): string | undefined;
  writeStripJson(target: StripJsonTarget, sceneJson: string): void;
  readonly insertParamOverrides: InsertParamOverrideMap;
  clearInsertAutomationLanes(target: StripJsonTarget): void;
}

function trackIdFor(ctx: EngineStripContext, target: string | number): number {
  return ctx.trackLaneIds[ctx.ensureTrackLane(target)];
}

type SceneEntry = Record<string, unknown>;

/**
 * Folds a setter's change into the strip's cached scene JSON, so the next
 * syncMixer re-post replays it instead of reverting it on the worklet.
 */
function mergeStripJson(
  ctx: EngineStripContext,
  target: StripJsonTarget,
  update: (entry: SceneEntry) => void,
): void {
  const scene = JSON.parse(ctx.readStripJson(target) ?? emptyStripJson(target)) as {
    strips: SceneEntry[];
    buses: SceneEntry[];
  };
  update(target.kind === 'bus' ? scene.buses[0] : scene.strips[0]);
  ctx.writeStripJson(target, JSON.stringify(scene));
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
  sceneJson: string,
  trackStripJson: Map<number, string>,
): { insertBaseResets: StripJsonTarget[]; constructionOverrides: InsertParamOverride[] } {
  const target: StripJsonTarget = { kind: 'track', trackId };
  const result = replaceStripScene(ctx, target, trackStripJson.get(trackId), sceneJson);
  trackStripJson.set(trackId, sceneJson);
  return result;
}

export function setTrackStripEqBand(
  ctx: EngineStripContext,
  target: string | number,
  bandIndex: number,
  band: EqBand | string,
): void {
  const trackId = trackIdFor(ctx, target);
  const bandJson = typeof band === 'string' ? band : JSON.stringify(band);
  ctx.offlineEngine.setTrackStripEqBandJson(trackId, bandIndex, bandJson);
  mergeEqBand(ctx, { kind: 'track', trackId }, bandIndex, bandJson);
  ctx.postSync({ type: 'syncTrackStripEqBand', trackId, bandIndex, bandJson });
}

export function setTrackStripInsertBypassed(
  ctx: EngineStripContext,
  target: string | number,
  insertIndex: number,
  bypassed: boolean,
  resetOnBypass: boolean,
): void {
  const trackId = trackIdFor(ctx, target);
  ctx.offlineEngine.setTrackStripInsertBypassed(trackId, insertIndex, bypassed, resetOnBypass);
  ctx.postSync({
    type: 'syncTrackStripInsertBypassed',
    trackId,
    insertIndex,
    bypassed,
    resetOnBypass,
  });
}

export function setTrackStripInsertParamByName(
  ctx: EngineStripContext,
  target: string | number,
  insertIndex: number,
  paramName: string,
  value: number,
): void {
  const trackId = trackIdFor(ctx, target);
  setInsertParamByName(
    ctx,
    { kind: 'track', trackId },
    insertIndex,
    paramName,
    value,
    () =>
      ctx.offlineEngine.restoreTrackStripInsertParamByName(trackId, insertIndex, paramName, value),
    { type: 'syncTrackStripInsertParamByName', trackId, insertIndex, paramName, value },
  );
}

export function setTrackStripPan(
  ctx: EngineStripContext,
  target: string | number,
  pan: number,
): void {
  const trackId = trackIdFor(ctx, target);
  ctx.offlineEngine.setTrackStripPan(trackId, pan);
  mergeStripJson(ctx, { kind: 'track', trackId }, (entry) => {
    entry.pan = pan;
  });
  ctx.postSync({ type: 'syncTrackStripPan', trackId, pan });
}

export function setTrackStripPanLaw(
  ctx: EngineStripContext,
  target: string | number,
  panLaw: PanLawInput,
): void {
  const trackId = trackIdFor(ctx, target);
  const code = panLawCode(panLaw);
  ctx.offlineEngine.setTrackStripPanLaw(trackId, code);
  mergeStripJson(ctx, { kind: 'track', trackId }, (entry) => {
    entry.panLaw = code;
  });
  ctx.postSync({ type: 'syncTrackStripPanLaw', trackId, panLaw: code });
}

export function setTrackStripPanMode(
  ctx: EngineStripContext,
  target: string | number,
  panMode: PanMode | number,
): void {
  const trackId = trackIdFor(ctx, target);
  const code = panModeCode(panMode);
  ctx.offlineEngine.setTrackStripPanMode(trackId, code);
  mergeStripJson(ctx, { kind: 'track', trackId }, (entry) => {
    entry.panMode = code;
  });
  ctx.postSync({ type: 'syncTrackStripPanMode', trackId, panMode: code });
}

export function setTrackStripDualPan(
  ctx: EngineStripContext,
  target: string | number,
  leftPan: number,
  rightPan: number,
): void {
  const trackId = trackIdFor(ctx, target);
  ctx.offlineEngine.setTrackStripDualPan(trackId, leftPan, rightPan);
  mergeStripJson(ctx, { kind: 'track', trackId }, (entry) => {
    entry.dualPanLeft = leftPan;
    entry.dualPanRight = rightPan;
  });
  ctx.postSync({ type: 'syncTrackStripDualPan', trackId, leftPan, rightPan });
}

export function setTrackStripChannelDelaySamples(
  ctx: EngineStripContext,
  target: string | number,
  delaySamples: number,
): void {
  const trackId = trackIdFor(ctx, target);
  ctx.offlineEngine.setTrackStripChannelDelaySamples(trackId, delaySamples);
  mergeStripJson(ctx, { kind: 'track', trackId }, (entry) => {
    entry.channelDelaySamples = delaySamples;
  });
  ctx.postSync({ type: 'syncTrackStripChannelDelaySamples', trackId, delaySamples });
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

export function setMasterStripEqBand(
  ctx: EngineStripContext,
  bandIndex: number,
  band: EqBand | string,
): void {
  const bandJson = typeof band === 'string' ? band : JSON.stringify(band);
  ctx.offlineEngine.setMasterStripEqBandJson(bandIndex, bandJson);
  mergeEqBand(ctx, { kind: 'master' }, bandIndex, bandJson);
  ctx.postSync({ type: 'syncMasterStripEqBand', bandIndex, bandJson });
}

export function setMasterStripInsertBypassed(
  ctx: EngineStripContext,
  insertIndex: number,
  bypassed: boolean,
  resetOnBypass: boolean,
): void {
  ctx.offlineEngine.setMasterStripInsertBypassed(insertIndex, bypassed, resetOnBypass);
  ctx.postSync({ type: 'syncMasterStripInsertBypassed', insertIndex, bypassed, resetOnBypass });
}

export function setMasterStripInsertParamByName(
  ctx: EngineStripContext,
  insertIndex: number,
  paramName: string,
  value: number,
): void {
  setInsertParamByName(
    ctx,
    { kind: 'master' },
    insertIndex,
    paramName,
    value,
    () => ctx.offlineEngine.restoreMasterStripInsertParamByName(insertIndex, paramName, value),
    { type: 'syncMasterStripInsertParamByName', insertIndex, paramName, value },
  );
}

export function setBusStripInsertParamByName(
  ctx: EngineStripContext,
  busId: number,
  insertIndex: number,
  paramName: string,
  value: number,
): void {
  setInsertParamByName(
    ctx,
    { kind: 'bus', busId },
    insertIndex,
    paramName,
    value,
    () => ctx.offlineEngine.restoreBusStripInsertParamByName(busId, insertIndex, paramName, value),
    { type: 'syncBusStripInsertParamByName', busId, insertIndex, paramName, value },
  );
}

export function setBusStripInsertBypassed(
  ctx: EngineStripContext,
  busId: number,
  insertIndex: number,
  bypassed: boolean,
  resetOnBypass: boolean,
): void {
  ctx.offlineEngine.setBusStripInsertBypassed(busId, insertIndex, bypassed, resetOnBypass);
  ctx.postSync({ type: 'syncBusStripInsertBypassed', busId, insertIndex, bypassed, resetOnBypass });
}

export function setBusStripEqBand(
  ctx: EngineStripContext,
  busId: number,
  bandIndex: number,
  band: EqBand | string,
): void {
  const bandJson = typeof band === 'string' ? band : JSON.stringify(band);
  ctx.offlineEngine.setBusStripEqBandJson(busId, bandIndex, bandJson);
  mergeEqBand(ctx, { kind: 'bus', busId }, bandIndex, bandJson);
  ctx.postSync({ type: 'syncBusStripEqBand', busId, bandIndex, bandJson });
}

export function setBusStripPan(ctx: EngineStripContext, busId: number, pan: number): void {
  ctx.offlineEngine.setBusStripPan(busId, pan);
  mergeStripJson(ctx, { kind: 'bus', busId }, (entry) => {
    entry.pan = pan;
  });
  ctx.postSync({ type: 'syncBusStripPan', busId, pan });
}

export function setBusStripPanLaw(
  ctx: EngineStripContext,
  busId: number,
  panLaw: PanLawInput,
): void {
  const code = panLawCode(panLaw);
  ctx.offlineEngine.setBusStripPanLaw(busId, code);
  mergeStripJson(ctx, { kind: 'bus', busId }, (entry) => {
    entry.panLaw = code;
  });
  ctx.postSync({ type: 'syncBusStripPanLaw', busId, panLaw: code });
}

export function setBusStripPanMode(
  ctx: EngineStripContext,
  busId: number,
  panMode: PanMode | number,
): void {
  const code = panModeCode(panMode);
  ctx.offlineEngine.setBusStripPanMode(busId, code);
  mergeStripJson(ctx, { kind: 'bus', busId }, (entry) => {
    entry.panMode = code;
  });
  ctx.postSync({ type: 'syncBusStripPanMode', busId, panMode: code });
}

export function setBusStripDualPan(
  ctx: EngineStripContext,
  busId: number,
  leftPan: number,
  rightPan: number,
): void {
  ctx.offlineEngine.setBusStripDualPan(busId, leftPan, rightPan);
  mergeStripJson(ctx, { kind: 'bus', busId }, (entry) => {
    entry.dualPanLeft = leftPan;
    entry.dualPanRight = rightPan;
  });
  ctx.postSync({ type: 'syncBusStripDualPan', busId, leftPan, rightPan });
}

export function pushMidiNoteOn(
  ctx: EngineStripContext,
  trackId: string | number,
  group: number,
  channel: number,
  note: number,
  velocity: number,
  renderFrame: number,
): void {
  const destinationId = ctx.resolveTargetId(trackId);
  ctx.offlineEngine.pushMidiNoteOn(destinationId, group, channel, note, velocity, renderFrame);
  ctx.postSync({
    type: 'syncMidiNoteOn',
    destinationId,
    group,
    channel,
    note,
    velocity,
    renderFrame,
  });
}

export function pushMidiNoteOff(
  ctx: EngineStripContext,
  trackId: string | number,
  group: number,
  channel: number,
  note: number,
  velocity: number,
  renderFrame: number,
): void {
  const destinationId = ctx.resolveTargetId(trackId);
  ctx.offlineEngine.pushMidiNoteOff(destinationId, group, channel, note, velocity, renderFrame);
  ctx.postSync({
    type: 'syncMidiNoteOff',
    destinationId,
    group,
    channel,
    note,
    velocity,
    renderFrame,
  });
}

export function pushMidiCc(
  ctx: EngineStripContext,
  trackId: string | number,
  group: number,
  channel: number,
  controller: number,
  value: number,
  renderFrame: number,
): void {
  const destinationId = ctx.resolveTargetId(trackId);
  ctx.offlineEngine.pushMidiCc(destinationId, group, channel, controller, value, renderFrame);
  ctx.postSync({
    type: 'syncMidiCc',
    destinationId,
    group,
    channel,
    controller,
    value,
    renderFrame,
  });
}

export function pushMidiPitchBend(
  ctx: EngineStripContext,
  trackId: string | number,
  group: number,
  channel: number,
  bend14: number,
  renderFrame: number,
): void {
  const destinationId = ctx.resolveTargetId(trackId);
  ctx.offlineEngine.pushMidiPitchBend(destinationId, group, channel, bend14, renderFrame);
  ctx.postSync({
    type: 'syncMidiPitchBend',
    destinationId,
    group,
    channel,
    data0: bend14,
    data1: 0,
    renderFrame,
  });
}

export function pushMidiChannelPressure(
  ctx: EngineStripContext,
  trackId: string | number,
  group: number,
  channel: number,
  pressure: number,
  renderFrame: number,
): void {
  const destinationId = ctx.resolveTargetId(trackId);
  ctx.offlineEngine.pushMidiChannelPressure(destinationId, group, channel, pressure, renderFrame);
  ctx.postSync({
    type: 'syncMidiChannelPressure',
    destinationId,
    group,
    channel,
    data0: pressure,
    data1: 0,
    renderFrame,
  });
}

export function pushMidiPolyPressure(
  ctx: EngineStripContext,
  trackId: string | number,
  group: number,
  channel: number,
  note: number,
  pressure: number,
  renderFrame: number,
): void {
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
    renderFrame,
  });
}

export function pushMidiUmp(
  ctx: EngineStripContext,
  trackId: string | number,
  word0: number | UmpWords,
  renderFrame: number,
): void {
  const destinationId = ctx.resolveTargetId(trackId);
  if (typeof word0 === 'number') {
    ctx.offlineEngine.pushMidiUmp(destinationId, word0, renderFrame);
    ctx.postSync({ type: 'syncMidiUmp', destinationId, word0, renderFrame });
    return;
  }
  const words = word0 instanceof Uint32Array ? new Uint32Array(word0) : word0.slice();
  ctx.offlineEngine.pushMidiUmp(destinationId, words, renderFrame);
  ctx.postSync({ type: 'syncMidiUmp', destinationId, words, renderFrame });
}

export function setBuiltinInstrument(
  ctx: EngineStripContext,
  trackId: string | number,
  config: { destinationId?: number } & Record<string, unknown>,
): void {
  const destinationId = ctx.resolveTargetId(trackId);
  ctx.offlineEngine.setBuiltinInstrument(config, destinationId);
  ctx.postInstrumentSync({ type: 'syncBuiltinInstrument', destinationId, config });
}

export function setSynthInstrument(
  ctx: EngineStripContext,
  trackId: string | number,
  patch: Record<string, unknown> | string,
): void {
  const destinationId = ctx.resolveTargetId(trackId);
  ctx.offlineEngine.setSynthInstrument(patch, destinationId);
  ctx.postInstrumentSync({ type: 'syncSynthInstrument', destinationId, patch });
}

export function loadSoundFont(ctx: EngineStripContext, data: Uint8Array): void {
  ctx.offlineEngine.loadSoundFont(data);
  ctx.postInstrumentSync({ type: 'syncLoadSoundFont', data });
}

export function setSf2Instrument(
  ctx: EngineStripContext,
  trackId: string | number,
  config: {
    destinationId?: number;
    gain?: number;
    polyphony?: number;
    preferModelForModeledFamilies?: boolean;
    clearBankRig?: boolean;
    gsEfxRealization?: 'modern' | 'classic';
  },
): void {
  const destinationId = ctx.resolveTargetId(trackId);
  ctx.offlineEngine.setSf2Instrument(config, destinationId);
  ctx.postInstrumentSync({ type: 'syncSf2Instrument', destinationId, config });
}

export function setMidiDestinationExternal(
  ctx: EngineStripContext,
  trackId: string | number,
  external: boolean,
): void {
  const destinationId = ctx.resolveTargetId(trackId);
  ctx.offlineEngine.setMidiDestinationExternal(destinationId, external);
  ctx.postSync({ type: 'syncMidiDestinationExternal', destinationId, external });
}

export function setExternalMidiClockEnabled(ctx: EngineStripContext, enabled: boolean): void {
  ctx.offlineEngine.setExternalMidiClockEnabled(enabled);
  ctx.postSync({ type: 'syncExternalMidiClock', enabled });
}

export function setMidiFx(
  ctx: EngineStripContext,
  trackId: string | number,
  configJson: string,
): void {
  const destinationId = ctx.resolveTargetId(trackId);
  ctx.offlineEngine.setMidiFx(destinationId, configJson);
  ctx.postInstrumentSync({ type: 'syncMidiFx', destinationId, configJson });
}

export function clearMidiFx(ctx: EngineStripContext, trackId: string | number): void {
  const destinationId = ctx.resolveTargetId(trackId);
  ctx.offlineEngine.clearMidiFx(destinationId);
  ctx.postInstrumentSync({ type: 'syncClearMidiFx', destinationId });
}

export function pushMidiSysex(
  ctx: EngineStripContext,
  trackId: string | number,
  data: Uint8Array,
  renderFrame: number,
): void {
  const destinationId = ctx.resolveTargetId(trackId);
  ctx.offlineEngine.pushMidiSysex(destinationId, data, renderFrame);
  ctx.postSync({ type: 'syncMidiSysex', destinationId, data, renderFrame });
}

export function pushMidiPanic(ctx: EngineStripContext, renderFrame: number): void {
  ctx.offlineEngine.pushMidiPanic(renderFrame);
  ctx.postSync({ type: 'syncMidiPanic', renderFrame });
}
