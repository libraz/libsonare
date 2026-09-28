import { sidechainSourceKindCode } from '../codes';
import type {
  EngineBus,
  EngineTrackLane,
  EngineTrackSend,
  RealtimeEngine,
  SidechainSourceKind,
} from '../index';
import { normalizeTrackLanes } from './engine-offline';
import { buildMixerLanes } from './engine-sync';
import type { SonareEngineSyncMessage } from './messages';
import { ENGINE_MIXER_PARAM_FADER_DB, engineMixerBusTarget } from './protocol';

/**
 * Collaborator surface the mixer/routing setters need from the owning
 * {@link SonareEngine}: the offline engine they mirror writes into, the mutable
 * routing stores (held by reference), the out-of-band sync poster, and the
 * lane/bus declaration and mixer-sync helpers.
 */
export interface EngineMixerContext {
  readonly offlineEngine: RealtimeEngine;
  readonly trackLaneIds: number[];
  readonly trackSends: Map<number, EngineTrackSend[]>;
  readonly trackOutputBus: Map<number, number>;
  readonly laneSidechains: Map<
    string,
    { trackId: number; insertIndex: number; sourceTrackId: number }
  >;
  readonly busSidechains: Map<
    string,
    { busId: number; insertIndex: number; sourceKind: number; sourceId: number }
  >;
  readonly masterSidechains: Map<
    number,
    { insertIndex: number; sourceKind: number; sourceId: number }
  >;
  readonly buses: EngineBus[];
  readonly trackStripJson: Map<number, string>;
  readonly busStripJson: Map<number, string>;
  postSync(message: SonareEngineSyncMessage): void;
  ensureTrackLane(target: string | number): number;
  ensureBus(busId: number): number;
  mixerLanes(): EngineTrackLane[];
  syncMixer(): void;
  sendSmoothedParam(paramId: number, value: number): boolean;
  getMasterStripJson(): string | undefined;
  cacheMasterStripJson(sceneJson: string): void;
}

/** Addresses one strip's cached scene JSON, the state a syncMixer re-post replays. */
export type StripJsonTarget =
  | { kind: 'track'; trackId: number }
  | { kind: 'bus'; busId: number }
  | { kind: 'master' };

/** Reads a strip's cached scene JSON; undefined when none was ever set. */
export function cachedStripJson(
  ctx: EngineMixerContext,
  target: StripJsonTarget,
): string | undefined {
  switch (target.kind) {
    case 'track':
      return ctx.trackStripJson.get(target.trackId);
    case 'bus':
      return ctx.busStripJson.get(target.busId);
    case 'master':
      return ctx.getMasterStripJson();
  }
}

/** Replaces a strip's cached scene JSON without syncing it. */
export function cacheStripJson(
  ctx: EngineMixerContext,
  target: StripJsonTarget,
  sceneJson: string,
): void {
  switch (target.kind) {
    case 'track':
      ctx.trackStripJson.set(target.trackId, sceneJson);
      return;
    case 'bus':
      ctx.busStripJson.set(target.busId, sceneJson);
      return;
    case 'master':
      ctx.cacheMasterStripJson(sceneJson);
      return;
  }
}

/** Builds the engine's track-lane descriptors from the current routing stores. */
export function mixerLanes(ctx: EngineMixerContext): EngineTrackLane[] {
  return buildMixerLanes(ctx.trackLaneIds, ctx.trackSends, ctx.trackOutputBus);
}

/**
 * Mirrors the current mixer routing into the offline engine and posts the full
 * mixer-sync message (lanes, buses, strip JSON, sidechains) to the worklet.
 */
export function syncMixer(ctx: EngineMixerContext): void {
  const lanes = mixerLanes(ctx);
  const buses = ctx.buses.map((bus) => ({ ...bus }));
  ctx.offlineEngine.setTrackBuses(buses);
  if (lanes.length > 0) {
    ctx.offlineEngine.setTrackLanes(lanes);
  }
  const trackStrips = Array.from(ctx.trackStripJson, ([trackId, sceneJson]) => ({
    trackId,
    sceneJson,
  }));
  const busStrips = Array.from(ctx.busStripJson, ([busId, sceneJson]) => ({
    busId,
    sceneJson,
  }));
  ctx.postSync({
    type: 'syncMixer',
    lanes,
    buses,
    trackStrips,
    laneSidechains: Array.from(ctx.laneSidechains.values()),
    busStrips,
    masterStripJson: ctx.getMasterStripJson(),
    busSidechains: Array.from(ctx.busSidechains.values()),
    masterSidechains: Array.from(ctx.masterSidechains.values()),
  });
}

/**
 * Declares the mixer track lanes in an explicit order.
 *
 * Lane indices are append-only: once a track id occupies a lane, its index
 * stays fixed for the engine's lifetime. The given list must therefore start
 * with the already-declared lane ids in their current order and may only
 * append new track ids after them. Entries carrying `sends` replace that
 * track's send list; entries without `sends` leave existing sends untouched.
 *
 * @param lanes Track ids or lane descriptors in the desired lane order.
 */
export function setTrackLanes(
  ctx: EngineMixerContext,
  lanes: ReadonlyArray<number | EngineTrackLane>,
): void {
  const { entries, ids } = normalizeTrackLanes(ctx.trackLaneIds, lanes);
  for (const entry of entries) {
    if (entry.sends) {
      ctx.trackSends.set(
        entry.trackId,
        entry.sends.map((send) => ({ ...send })),
      );
    }
    if (entry.outputBusId !== undefined) {
      if (entry.outputBusId === 0) {
        ctx.trackOutputBus.delete(entry.trackId);
      } else {
        ctx.trackOutputBus.set(entry.trackId, entry.outputBusId);
      }
    }
  }
  ctx.trackLaneIds.splice(0, ctx.trackLaneIds.length, ...ids);
  ctx.syncMixer();
}

/**
 * Routes a track lane's post-fader output into a declared bus instead of
 * the master mix (group/folder routing); busId 0 restores the master mix.
 */
export function setTrackOutputBus(
  ctx: EngineMixerContext,
  target: string | number,
  busId: number,
): void {
  const laneIndex = ctx.ensureTrackLane(target);
  const trackId = ctx.trackLaneIds[laneIndex];
  if (busId === 0) {
    ctx.trackOutputBus.delete(trackId);
  } else {
    ctx.trackOutputBus.set(trackId, busId);
  }
  ctx.syncMixer();
}

/**
 * Keys one insert of a lane strip from another lane's post-strip pre-fader
 * audio (ducking/sidechainRouter inserts). sourceTarget null removes the
 * binding.
 */
export function setLaneSidechain(
  ctx: EngineMixerContext,
  target: string | number,
  insertIndex: number,
  sourceTarget: string | number | null,
): void {
  const laneIndex = ctx.ensureTrackLane(target);
  const trackId = ctx.trackLaneIds[laneIndex];
  const key = `${trackId}:${insertIndex}`;
  let sourceTrackId = 0;
  if (sourceTarget !== null) {
    const sourceIndex = ctx.ensureTrackLane(sourceTarget);
    sourceTrackId = ctx.trackLaneIds[sourceIndex];
  }
  ctx.offlineEngine.setLaneSidechain(trackId, insertIndex, sourceTrackId);
  if (sourceTrackId === 0) {
    ctx.laneSidechains.delete(key);
  } else {
    ctx.laneSidechains.set(key, { trackId, insertIndex, sourceTrackId });
  }
  ctx.postSync({
    type: 'syncMixer',
    lanes: ctx.mixerLanes(),
    laneSidechains: [{ trackId, insertIndex, sourceTrackId }],
  });
}

/**
 * Keys one insert of a bus strip from a track lane or another bus
 * (ducking/sidechainRouter inserts). `sourceId` 0 removes the binding. The
 * binding is cached for resync only once the engine has accepted it.
 */
export function setBusSidechain(
  ctx: EngineMixerContext,
  busId: number,
  insertIndex: number,
  kind: SidechainSourceKind | number,
  sourceId: number,
): void {
  const sourceKind = sidechainSourceKindCode(kind);
  ctx.ensureBus(busId);
  ensureSidechainSource(ctx, sourceKind, sourceId);
  ctx.offlineEngine.setBusSidechain(busId, insertIndex, sourceKind, sourceId);
  const key = `${busId}:${insertIndex}`;
  if (sourceId === 0) {
    ctx.busSidechains.delete(key);
  } else {
    ctx.busSidechains.set(key, { busId, insertIndex, sourceKind, sourceId });
  }
  ctx.postSync({
    type: 'syncMixer',
    lanes: ctx.mixerLanes(),
    busSidechains: [{ busId, insertIndex, sourceKind, sourceId }],
  });
}

/**
 * Keys one insert of the master strip from a track lane or a bus. Same source
 * rules as {@link setBusSidechain}.
 */
export function setMasterSidechain(
  ctx: EngineMixerContext,
  insertIndex: number,
  kind: SidechainSourceKind | number,
  sourceId: number,
): void {
  const sourceKind = sidechainSourceKindCode(kind);
  ensureSidechainSource(ctx, sourceKind, sourceId);
  ctx.offlineEngine.setMasterSidechain(insertIndex, sourceKind, sourceId);
  if (sourceId === 0) {
    ctx.masterSidechains.delete(insertIndex);
  } else {
    ctx.masterSidechains.set(insertIndex, { insertIndex, sourceKind, sourceId });
  }
  ctx.postSync({
    type: 'syncMixer',
    lanes: ctx.mixerLanes(),
    masterSidechains: [{ insertIndex, sourceKind, sourceId }],
  });
}

function ensureSidechainSource(
  ctx: EngineMixerContext,
  sourceKind: number,
  sourceId: number,
): void {
  if (sourceId === 0) {
    return;
  }
  if (sourceKind === 1) {
    ctx.ensureBus(sourceId);
  } else {
    ctx.ensureTrackLane(sourceId);
  }
}

export function setSends(
  ctx: EngineMixerContext,
  target: string | number,
  sends: EngineTrackSend[],
): void {
  const laneIndex = ctx.ensureTrackLane(target);
  const trackId = ctx.trackLaneIds[laneIndex];
  ctx.trackSends.set(
    trackId,
    sends.map((send) => ({ ...send })),
  );
  ctx.syncMixer();
}

export function setTrackBuses(ctx: EngineMixerContext, buses: EngineBus[]): void {
  ctx.buses.splice(0, ctx.buses.length, ...buses.map((bus) => ({ ...bus })));
  ctx.syncMixer();
}

export function setBusGain(ctx: EngineMixerContext, busId: number, db: number): boolean {
  const busIndex = ctx.ensureBus(busId);
  ctx.buses[busIndex] = { ...ctx.buses[busIndex], busId, gainDb: db };
  ctx.offlineEngine.setTrackBuses(ctx.buses);
  return ctx.sendSmoothedParam(engineMixerBusTarget(busIndex, ENGINE_MIXER_PARAM_FADER_DB), db);
}

export function setBusStripJson(ctx: EngineMixerContext, busId: number, sceneJson: string): void {
  ctx.ensureBus(busId);
  ctx.offlineEngine.setBusStripJson(busId, sceneJson);
  ctx.busStripJson.set(busId, sceneJson);
  ctx.syncMixer();
}
