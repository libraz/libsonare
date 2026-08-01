import { panLawCode, panModeCode } from '../codes';
import type { EqBand, PanLaw, PanMode, RealtimeEngine } from '../index';
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
}

function trackIdFor(ctx: EngineStripContext, target: string | number): number {
  return ctx.trackLaneIds[ctx.ensureTrackLane(target)];
}

export function setTrackStripJson(
  ctx: EngineStripContext,
  trackId: number,
  sceneJson: string,
  trackStripJson: Map<number, string>,
): void {
  ctx.offlineEngine.setTrackStripJson(trackId, sceneJson);
  trackStripJson.set(trackId, sceneJson);
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
  ctx.offlineEngine.setTrackStripInsertParamByName(trackId, insertIndex, paramName, value);
  ctx.postSync({ type: 'syncTrackStripInsertParamByName', trackId, insertIndex, paramName, value });
}

export function setTrackStripPan(
  ctx: EngineStripContext,
  target: string | number,
  pan: number,
): void {
  const trackId = trackIdFor(ctx, target);
  ctx.offlineEngine.setTrackStripPan(trackId, pan);
  ctx.postSync({ type: 'syncTrackStripPan', trackId, pan });
}

export function setTrackStripPanLaw(
  ctx: EngineStripContext,
  target: string | number,
  panLaw: PanLaw | number,
): void {
  const trackId = trackIdFor(ctx, target);
  const code = panLawCode(panLaw);
  ctx.offlineEngine.setTrackStripPanLaw(trackId, code);
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
  ctx.postSync({ type: 'syncTrackStripDualPan', trackId, leftPan, rightPan });
}

export function setTrackStripChannelDelaySamples(
  ctx: EngineStripContext,
  target: string | number,
  delaySamples: number,
): void {
  const trackId = trackIdFor(ctx, target);
  ctx.offlineEngine.setTrackStripChannelDelaySamples(trackId, delaySamples);
  ctx.postSync({ type: 'syncTrackStripChannelDelaySamples', trackId, delaySamples });
}

export function setMasterStripEqBand(
  ctx: EngineStripContext,
  bandIndex: number,
  band: EqBand | string,
): void {
  const bandJson = typeof band === 'string' ? band : JSON.stringify(band);
  ctx.offlineEngine.setMasterStripEqBandJson(bandIndex, bandJson);
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
  ctx.offlineEngine.setMasterStripInsertParamByName(insertIndex, paramName, value);
  ctx.postSync({ type: 'syncMasterStripInsertParamByName', insertIndex, paramName, value });
}

export function setBusStripInsertParamByName(
  ctx: EngineStripContext,
  busId: number,
  insertIndex: number,
  paramName: string,
  value: number,
): void {
  ctx.offlineEngine.setBusStripInsertParamByName(busId, insertIndex, paramName, value);
  ctx.postSync({ type: 'syncBusStripInsertParamByName', busId, insertIndex, paramName, value });
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

export function pushMidiUmp(
  ctx: EngineStripContext,
  trackId: string | number,
  word0: number,
  renderFrame: number,
): void {
  const destinationId = ctx.resolveTargetId(trackId);
  ctx.offlineEngine.pushMidiUmp(destinationId, word0, renderFrame);
  ctx.postSync({ type: 'syncMidiUmp', destinationId, word0, renderFrame });
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
  config: { destinationId?: number; gain?: number; polyphony?: number },
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
