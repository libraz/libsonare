import type { EngineBus, EngineTrackLane, EngineTrackSend, RealtimeEngine } from '../index';
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
  if (sourceTrackId === 0) {
    ctx.laneSidechains.delete(key);
  } else {
    ctx.laneSidechains.set(key, { trackId, insertIndex, sourceTrackId });
  }
  ctx.offlineEngine.setLaneSidechain(trackId, insertIndex, sourceTrackId);
  ctx.postSync({
    type: 'syncMixer',
    lanes: ctx.mixerLanes(),
    laneSidechains: [{ trackId, insertIndex, sourceTrackId }],
  });
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
