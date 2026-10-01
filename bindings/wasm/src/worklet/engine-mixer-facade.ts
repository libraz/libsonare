import { sidechainSourceKindCode } from '../codes';
import type {
  EngineBus,
  EngineTrackLane,
  EngineTrackSend,
  RealtimeEngine,
  SidechainSourceKind,
} from '../index';
import { normalizeTrackLanes } from './engine-offline';
import { buildMixerLanes, resolveTargetId } from './engine-sync';
import type { SonareEngineSyncMessage, SonareEngineSyncMixerInsertParamOverride } from './messages';
import { ENGINE_MIXER_PARAM_FADER_DB, engineMixerBusTarget } from './protocol';

/** A latest by-name insert value retained across a later topology replay. */
export interface InsertParamOverride {
  target: StripJsonTarget;
  insertIndex: number;
  paramName: string;
  value: number;
}

export type InsertParamOverrideMap = Map<string, InsertParamOverride>;

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
  readonly trackSourceChannelLayout: Map<number, number>;
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
  readonly insertParamOverrides: InsertParamOverrideMap;
  clearInsertAutomationLanes(target: StripJsonTarget, forgetResolvedIds?: boolean): void;
  flushOfflineMirror(): void;
  postSync(message: SonareEngineSyncMessage): void;
  ensureTrackLane(target: string | number): number;
  ensureBus(busId: number): number;
  mixerLanes(): EngineTrackLane[];
  syncMixer(forceInsertResets?: StripJsonTarget[]): void;
  sendSmoothedParam(paramId: number, value: number): boolean;
  getMasterStripJson(): string | undefined;
  cacheMasterStripJson(sceneJson: string): void;
}

/** Addresses one strip's cached scene JSON, the state a syncMixer re-post replays. */
export type StripJsonTarget =
  | { kind: 'track'; trackId: number }
  | { kind: 'bus'; busId: number }
  | { kind: 'master' };

interface MixerRoutingDraft {
  trackLaneIds: number[];
  trackSends: Map<number, EngineTrackSend[]>;
  trackOutputBus: Map<number, number>;
  trackSourceChannelLayout: Map<number, number>;
}

/** An empty construction scene used only within an explicit strip replacement. */
export function emptyStripJson(target: StripJsonTarget): string {
  switch (target.kind) {
    case 'track':
      return `{"version":1,"strips":[{"id":"track-${target.trackId}"}],"buses":[],"connections":[]}`;
    case 'bus':
      return `{"version":1,"strips":[],"buses":[{"id":"bus-${target.busId}"}],"connections":[]}`;
    case 'master':
      return '{"version":1,"strips":[{"id":"master"}],"buses":[],"connections":[]}';
  }
}

/** Drop bindings whose insert was removed by an explicit scene replacement. */
export function pruneStripSidechains(
  ctx: EngineMixerContext,
  target: StripJsonTarget,
  sceneJson: string,
): void {
  const scene = JSON.parse(sceneJson) as {
    strips?: Array<{ inserts?: unknown[] }>;
    buses?: Array<{ inserts?: unknown[] }>;
  };
  const entry = target.kind === 'bus' ? scene.buses?.[0] : scene.strips?.[0];
  const insertCount = Array.isArray(entry?.inserts) ? entry.inserts.length : 0;
  if (target.kind === 'track') {
    for (const [key, binding] of ctx.laneSidechains) {
      if (binding.trackId === target.trackId && binding.insertIndex >= insertCount) {
        ctx.laneSidechains.delete(key);
      }
    }
  } else if (target.kind === 'bus') {
    for (const [key, binding] of ctx.busSidechains) {
      if (binding.busId === target.busId && binding.insertIndex >= insertCount) {
        ctx.busSidechains.delete(key);
      }
    }
  } else {
    for (const [key, binding] of ctx.masterSidechains) {
      if (binding.insertIndex >= insertCount) {
        ctx.masterSidechains.delete(key);
      }
    }
  }
}

export function hasInsertParamOverrides(
  overrides: InsertParamOverrideMap,
  target: StripJsonTarget,
): boolean {
  for (const override of overrides.values()) {
    if (sameStripTarget(override.target, target)) {
      return true;
    }
  }
  return false;
}

/** Apply the replacement first to validate it, then force fresh insert state. */
export function applyFullStripJson(
  engine: RealtimeEngine,
  target: StripJsonTarget,
  sceneJson: string,
  resetInserts: boolean,
): void {
  const apply = (json: string): void => {
    switch (target.kind) {
      case 'track':
        engine.setTrackStripJson(target.trackId, json);
        break;
      case 'bus':
        engine.setBusStripJson(target.busId, json);
        break;
      case 'master':
        engine.setMasterStripJson(json);
        break;
    }
  };
  engine.applyCommandsDueNowPreservingFuture();
  apply(sceneJson);
  if (resetInserts) {
    switch (target.kind) {
      case 'track':
        engine.clearTrackInsertParameterBases(target.trackId);
        break;
      case 'bus':
        engine.clearBusInsertParameterBases(target.busId);
        break;
      case 'master':
        engine.clearMasterInsertParameterBases();
        break;
    }
    apply(emptyStripJson(target));
    apply(sceneJson);
  }
}

/** Builds a collision-safe key for one target/insert/parameter tuple. */
export function insertParamOverrideKey(override: InsertParamOverride): string {
  const targetId =
    override.target.kind === 'track'
      ? override.target.trackId
      : override.target.kind === 'bus'
        ? override.target.busId
        : 'master';
  return JSON.stringify([override.target.kind, targetId, override.insertIndex, override.paramName]);
}

/** Removes all retained insert values for one strip after a full scene replacement. */
export function clearInsertParamOverrides(
  overrides: InsertParamOverrideMap,
  target: StripJsonTarget,
): void {
  for (const [key, override] of overrides) {
    if (sameStripTarget(override.target, target)) {
      overrides.delete(key);
    }
  }
}

function sameStripTarget(left: StripJsonTarget, right: StripJsonTarget): boolean {
  if (left.kind !== right.kind) {
    return false;
  }
  if (left.kind === 'track' && right.kind === 'track') {
    return left.trackId === right.trackId;
  }
  if (left.kind === 'bus' && right.kind === 'bus') {
    return left.busId === right.busId;
  }
  return left.kind === 'master' && right.kind === 'master';
}

function flattenInsertParamOverride(
  override: InsertParamOverride,
): SonareEngineSyncMixerInsertParamOverride {
  switch (override.target.kind) {
    case 'track':
      return {
        kind: 'track',
        trackId: override.target.trackId,
        insertIndex: override.insertIndex,
        paramName: override.paramName,
        value: override.value,
      };
    case 'bus':
      return {
        kind: 'bus',
        busId: override.target.busId,
        insertIndex: override.insertIndex,
        paramName: override.paramName,
        value: override.value,
      };
    case 'master':
      return {
        kind: 'master',
        insertIndex: override.insertIndex,
        paramName: override.paramName,
        value: override.value,
      };
  }
}

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
  return buildMixerLanes(
    ctx.trackLaneIds,
    ctx.trackSends,
    ctx.trackOutputBus,
    ctx.trackSourceChannelLayout,
  );
}

function cloneMixerRouting(ctx: EngineMixerContext): MixerRoutingDraft {
  return {
    trackLaneIds: [...ctx.trackLaneIds],
    trackSends: new Map(
      Array.from(ctx.trackSends, ([trackId, sends]) => [
        trackId,
        sends.map((send) => ({ ...send })),
      ]),
    ),
    trackOutputBus: new Map(ctx.trackOutputBus),
    trackSourceChannelLayout: new Map(ctx.trackSourceChannelLayout),
  };
}

function commitMixerRouting(ctx: EngineMixerContext, draft: MixerRoutingDraft): void {
  ctx.trackLaneIds.splice(0, ctx.trackLaneIds.length, ...draft.trackLaneIds);
  ctx.trackSends.clear();
  for (const [trackId, sends] of draft.trackSends) {
    ctx.trackSends.set(
      trackId,
      sends.map((send) => ({ ...send })),
    );
  }
  ctx.trackOutputBus.clear();
  for (const [trackId, busId] of draft.trackOutputBus) {
    ctx.trackOutputBus.set(trackId, busId);
  }
  ctx.trackSourceChannelLayout.clear();
  for (const [trackId, layout] of draft.trackSourceChannelLayout) {
    ctx.trackSourceChannelLayout.set(trackId, layout);
  }
}

function buildDraftMixerLanes(draft: MixerRoutingDraft): EngineTrackLane[] {
  return buildMixerLanes(
    draft.trackLaneIds,
    draft.trackSends,
    draft.trackOutputBus,
    draft.trackSourceChannelLayout,
  );
}

function applyMixerRouting(
  ctx: EngineMixerContext,
  draft: MixerRoutingDraft,
  busesAlreadyApplied: boolean,
): { lanes: EngineTrackLane[]; buses: EngineBus[] } {
  const lanes = buildDraftMixerLanes(draft);
  const buses = ctx.buses.map((bus) => ({ ...bus }));
  if (!busesAlreadyApplied) {
    ctx.offlineEngine.setTrackBuses(buses);
  }
  if (lanes.length > 0) {
    // The native setter validates the candidate before clearing insert
    // automation. Apply it first so rejected routing leaves active ramps
    // untouched; settle successful insert targets before postMixerSync replays
    // cached by-name values.
    ctx.offlineEngine.setTrackLanes(lanes);
    ctx.offlineEngine.settleInsertParameters();
  }
  return { lanes, buses };
}

function postMixerSync(
  ctx: EngineMixerContext,
  lanes: EngineTrackLane[],
  buses: EngineBus[],
  forceInsertResets: StripJsonTarget[],
): void {
  if (forceInsertResets.length > 0) {
    // The temporary empty strip drops native sidechain bindings. Restore the
    // offline mirror in the same order the worklet restores them below.
    for (const binding of ctx.laneSidechains.values()) {
      ctx.offlineEngine.setLaneSidechain(
        binding.trackId,
        binding.insertIndex,
        binding.sourceTrackId,
      );
    }
    for (const binding of ctx.busSidechains.values()) {
      ctx.offlineEngine.setBusSidechain(
        binding.busId,
        binding.insertIndex,
        binding.sourceKind,
        binding.sourceId,
      );
    }
    for (const binding of ctx.masterSidechains.values()) {
      ctx.offlineEngine.setMasterSidechain(
        binding.insertIndex,
        binding.sourceKind,
        binding.sourceId,
      );
    }
  }
  replayInsertParamOverrides(ctx, lanes, buses);
  const trackStrips = Array.from(ctx.trackStripJson, ([trackId, sceneJson]) => ({
    trackId,
    sceneJson,
  }));
  const busStrips = Array.from(ctx.busStripJson, ([busId, sceneJson]) => ({
    busId,
    sceneJson,
  }));
  const insertParamOverrides = Array.from(
    ctx.insertParamOverrides.values(),
    flattenInsertParamOverride,
  );
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
    ...(insertParamOverrides.length > 0 ? { insertParamOverrides } : {}),
    ...(forceInsertResets.length > 0 ? { forceInsertResets } : {}),
  });
}

function syncMixerDraft(
  ctx: EngineMixerContext,
  draft: MixerRoutingDraft,
  busesAlreadyApplied = false,
  forceInsertResets: StripJsonTarget[] = [],
): void {
  const { lanes, buses } = applyMixerRouting(ctx, draft, busesAlreadyApplied);
  commitMixerRouting(ctx, draft);
  postMixerSync(ctx, lanes, buses, forceInsertResets);
}

/**
 * Mirrors the current mixer routing into the offline engine and posts the full
 * mixer-sync message (lanes, buses, strip JSON, sidechains) to the worklet.
 */
export function syncMixer(
  ctx: EngineMixerContext,
  busesAlreadyApplied = false,
  forceInsertResets: StripJsonTarget[] = [],
): void {
  syncMixerDraft(ctx, cloneMixerRouting(ctx), busesAlreadyApplied, forceInsertResets);
}

/**
 * Reapplies retained by-name values after native topology setters clear their
 * smoother slots. This runs once per mixer sync, never once per knob tick.
 */
export function replayInsertParamOverrides(
  ctx: EngineMixerContext,
  lanes = mixerLanes(ctx),
  buses = ctx.buses,
): void {
  const activeTrackIds = new Set(lanes.map((lane) => lane.trackId));
  const activeBusIds = new Set(buses.map((bus) => bus.busId));
  for (const [key, override] of ctx.insertParamOverrides) {
    const active =
      override.target.kind === 'track'
        ? activeTrackIds.has(override.target.trackId)
        : override.target.kind === 'bus'
          ? activeBusIds.has(override.target.busId)
          : true;
    if (!active) {
      ctx.insertParamOverrides.delete(key);
      continue;
    }
    switch (override.target.kind) {
      case 'track':
        ctx.offlineEngine.restoreTrackStripInsertParamByName(
          override.target.trackId,
          override.insertIndex,
          override.paramName,
          override.value,
        );
        break;
      case 'bus':
        ctx.offlineEngine.restoreBusStripInsertParamByName(
          override.target.busId,
          override.insertIndex,
          override.paramName,
          override.value,
        );
        break;
      case 'master':
        ctx.offlineEngine.restoreMasterStripInsertParamByName(
          override.insertIndex,
          override.paramName,
          override.value,
        );
        break;
    }
  }
}

/**
 * Declares the mixer track lanes in an explicit order.
 *
 * Lane indices are append-only: once a track id occupies a lane, its index
 * stays fixed for the engine's lifetime. The given list must therefore start
 * with the already-declared lane ids in their current order and may only
 * append new track ids after them. Entries carrying `sends` replace that
 * track's send list; entries without `sends` leave existing sends untouched.
 * The same omission rule preserves an existing source channel layout.
 *
 * @param lanes Track ids or lane descriptors in the desired lane order.
 */
export function setTrackLanes(
  ctx: EngineMixerContext,
  lanes: ReadonlyArray<number | EngineTrackLane>,
): void {
  const draft = cloneMixerRouting(ctx);
  const { entries, ids } = normalizeTrackLanes(draft.trackLaneIds, lanes);
  for (const entry of entries) {
    if (entry.sends) {
      draft.trackSends.set(
        entry.trackId,
        entry.sends.map((send) => ({ ...send })),
      );
    }
    if (entry.outputBusId !== undefined) {
      if (entry.outputBusId === 0) {
        draft.trackOutputBus.delete(entry.trackId);
      } else {
        draft.trackOutputBus.set(entry.trackId, entry.outputBusId);
      }
    }
    if (entry.sourceChannelLayout !== undefined) {
      draft.trackSourceChannelLayout.set(entry.trackId, entry.sourceChannelLayout);
    }
  }
  draft.trackLaneIds = ids;
  syncMixerDraft(ctx, draft, true);
}

function ensureTrackLaneInDraft(draft: MixerRoutingDraft, target: string | number): number {
  const trackId = resolveTargetId(target);
  if (!Number.isInteger(trackId) || trackId <= 0) {
    throw new RangeError(`Invalid track id for mixer lane: ${String(target)}`);
  }
  const existing = draft.trackLaneIds.indexOf(trackId);
  if (existing >= 0) {
    return existing;
  }
  draft.trackLaneIds.push(trackId);
  return draft.trackLaneIds.length - 1;
}

/** Ensures a lane exists, committing it only after native routing accepts it. */
export function ensureTrackLane(ctx: EngineMixerContext, target: string | number): number {
  const draft = cloneMixerRouting(ctx);
  const laneIndex = ensureTrackLaneInDraft(draft, target);
  if (draft.trackLaneIds.length !== ctx.trackLaneIds.length) {
    syncMixerDraft(ctx, draft, true);
  }
  return laneIndex;
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
  const draft = cloneMixerRouting(ctx);
  const laneIndex = ensureTrackLaneInDraft(draft, target);
  const trackId = draft.trackLaneIds[laneIndex];
  if (busId === 0) {
    draft.trackOutputBus.delete(trackId);
  } else {
    draft.trackOutputBus.set(trackId, busId);
  }
  syncMixerDraft(ctx, draft, true);
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
    sidechainDelta: true,
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
    sidechainDelta: true,
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
    sidechainDelta: true,
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
  const draft = cloneMixerRouting(ctx);
  const laneIndex = ensureTrackLaneInDraft(draft, target);
  const trackId = draft.trackLaneIds[laneIndex];
  draft.trackSends.set(
    trackId,
    sends.map((send) => ({ ...send })),
  );
  syncMixerDraft(ctx, draft, true);
}

export function setTrackBuses(ctx: EngineMixerContext, buses: EngineBus[]): void {
  // Validate and apply before pruning retained scenes. Native rejects a bus
  // removal while a lane still routes to it; that failure must leave JS state
  // untouched so a later valid edit can still replay the existing inserts.
  ctx.offlineEngine.setTrackBuses(buses);
  const retainedBusIds = new Set(buses.map((bus) => bus.busId));
  for (const bus of ctx.buses) {
    if (!retainedBusIds.has(bus.busId)) {
      ctx.clearInsertAutomationLanes({ kind: 'bus', busId: bus.busId }, true);
    }
  }
  ctx.buses.splice(0, ctx.buses.length, ...buses.map((bus) => ({ ...bus })));
  const activeBusIds = new Set(ctx.buses.map((bus) => bus.busId));
  for (const busId of ctx.busStripJson.keys()) {
    if (!activeBusIds.has(busId)) {
      ctx.busStripJson.delete(busId);
    }
  }
  for (const [key, binding] of ctx.busSidechains) {
    if (
      !activeBusIds.has(binding.busId) ||
      (binding.sourceKind === 1 && !activeBusIds.has(binding.sourceId))
    ) {
      ctx.busSidechains.delete(key);
    }
  }
  for (const [key, binding] of ctx.masterSidechains) {
    if (binding.sourceKind === 1 && !activeBusIds.has(binding.sourceId)) {
      ctx.masterSidechains.delete(key);
    }
  }
  syncMixer(ctx, true);
}

export function setBusGain(ctx: EngineMixerContext, busId: number, db: number): boolean {
  const busIndex = ctx.ensureBus(busId);
  ctx.buses[busIndex] = { ...ctx.buses[busIndex], busId, gainDb: db };
  // The reserved smoothed parameter updates both engines. Rebuilding the bus
  // topology for every fader tick would reset insert slots and their tails.
  return ctx.sendSmoothedParam(engineMixerBusTarget(busIndex, ENGINE_MIXER_PARAM_FADER_DB), db);
}

export function setBusStripJson(ctx: EngineMixerContext, busId: number, sceneJson: string): void {
  ctx.ensureBus(busId);
  const target: StripJsonTarget = { kind: 'bus', busId };
  const resetInserts =
    (ctx.busStripJson.has(busId) && ctx.busStripJson.get(busId) !== sceneJson) ||
    hasInsertParamOverrides(ctx.insertParamOverrides, target);
  applyFullStripJson(ctx.offlineEngine, target, sceneJson, resetInserts);
  if (resetInserts) {
    ctx.clearInsertAutomationLanes(target);
  }
  pruneStripSidechains(ctx, target, sceneJson);
  ctx.busStripJson.set(busId, sceneJson);
  clearInsertParamOverrides(ctx.insertParamOverrides, target);
  ctx.syncMixer(resetInserts ? [target] : []);
}
