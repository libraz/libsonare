import { trackMonitorModeCode } from '../codes.js';
import type {
  EngineAutomationPoint,
  EngineParameterInfo,
  EngineTrackMonitorMode,
  RealtimeEngine,
} from '../index.js';
import { refuseType } from '../internal/entry.js';
import type { SonareEngineSyncMessage } from '../internal/worklet_messages.js';
import { requestObject, resolveRenderFrame } from '../validation.js';
import { commitCommand, commitSync, type EngineCommitContext } from './engine-commit.js';
import { SonareEngineCommandType } from './protocol.js';

/**
 * Collaborator surface the parameter / automation-id resolution helpers need
 * from the owning {@link SonareEngine}: the offline engine they mirror into and
 * query, the realtime node they command, the lane/bus declaration helpers, and
 * the declared track-lane id store used to resolve insert-automation ids.
 */
export interface EngineParameterContext extends EngineCommitContext {
  readonly offlineEngine: RealtimeEngine;
  postSync(message: SonareEngineSyncMessage): void;
  readonly automationLanes: Map<number, EngineAutomationPoint[]>;
  readonly trackLaneIds: number[];
  resolveParamId(nodeId: string, param: string | number): number;
  ensureTrackLane(target: string | number): number;
  ensureBus(busId: number): number;
}

export function setParam(
  ctx: EngineParameterContext,
  nodeId: string,
  param: string | number,
  value: number,
): void {
  const paramId = ctx.resolveParamId(nodeId, param);
  commitCommand(
    ctx,
    { type: SonareEngineCommandType.SetParam, targetId: paramId, renderFrame: -1, argFloat: value },
    (offline) => offline.setParameter(paramId, value),
  );
}

export function setTrackSoloMute(
  ctx: EngineParameterContext,
  target: string | number,
  state: { solo: boolean; mute: boolean },
): void {
  requestObject('setTrackSoloMute', state, 'state', true);
  const laneIndex = ctx.ensureTrackLane(target);
  const { solo, mute } = state;
  commitCommand(
    ctx,
    {
      type: SonareEngineCommandType.SetSoloMute,
      targetId: laneIndex,
      renderFrame: -1,
      argInt: (mute ? 0x1 : 0) | (solo ? 0x2 : 0),
    },
    (offline) => offline.setTrackSoloMute(ctx.trackLaneIds[laneIndex], state),
  );
}

export function setTrackMonitorMode(
  ctx: EngineParameterContext,
  target: string | number,
  mode: EngineTrackMonitorMode,
  renderFrame?: number,
): void {
  // Resolve the mode before declaring a lane or touching either engine. This
  // keeps numbers, booleans and unknown spellings from reaching the queue.
  if (typeof mode === 'number') {
    throw refuseType("mode must be 'off', 'pfl' or 'afl', not a number");
  }
  const modeOrdinal = trackMonitorModeCode(mode);
  const laneIndex = ctx.ensureTrackLane(target);
  commitCommand(
    ctx,
    {
      type: SonareEngineCommandType.SetTrackMonitorMode,
      targetId: laneIndex,
      renderFrame: resolveRenderFrame(renderFrame),
      argInt: modeOrdinal,
    },
    (offline) => offline.setTrackMonitorMode(ctx.trackLaneIds[laneIndex], mode, renderFrame),
  );
}

/**
 * Resolves a strip's fader (`'faderDb'`), pan (`'pan'`) or width (`'width'`) to
 * its reserved automation id, which can be fed straight to setAutomationLane.
 * A track's id is resolved on the offline engine, which numbers it as the
 * worklet engine does, and keeps naming that track across lane reorders.
 *
 * @param target Track id (declares a mixer lane on first use) or 'master'.
 * @param kind Strip parameter to address; a track has no `'width'`.
 * @returns Reserved engine parameter id for the strip parameter.
 */
export function resolveStripAutomationId(
  ctx: EngineParameterContext,
  target: string | number,
  kind: 'faderDb' | 'pan' | 'width',
): number {
  if (target === 'master') {
    return ctx.offlineEngine.resolveStripAutomationId('master', kind);
  }
  const laneIndex = ctx.ensureTrackLane(target);
  return ctx.offlineEngine.resolveStripAutomationId(ctx.trackLaneIds[laneIndex], kind);
}

/**
 * Resolves a bus fader to its reserved automation id. The id is resolved on the
 * offline engine, which numbers it as the worklet engine does, and keeps naming
 * that bus across bus reorders.
 *
 * @param busId Bus id (declares the mixer bus on first use).
 * @param paramName Bus parameter; only `'faderDb'`.
 * @returns Reserved engine parameter id for the bus fader gain (dB).
 */
export function resolveBusAutomationId(
  ctx: EngineParameterContext,
  busId: number,
  paramName: 'faderDb',
): number {
  ctx.ensureBus(busId);
  return ctx.offlineEngine.resolveBusAutomationId(busId, paramName);
}

/**
 * Resolves a strip insert parameter (JSON-key name) to the reserved
 * insert-automation id fed straight to setAutomationLane. A track is declared
 * first so the offline engine resolves the same strip selector the realtime
 * engine uses.
 *
 * @param target Track id (declares a mixer lane on first use) or 'master'.
 * @param insertIndex Index into the strip's combined insert sequence.
 * @param paramName Processor JSON-key parameter name.
 * @returns Reserved insert-automation id.
 * @throws RangeError when the strip, insert or key is unknown.
 */
export function resolveStripInsertAutomationId(
  ctx: EngineParameterContext,
  target: string | number,
  insertIndex: number,
  paramName: string,
): number {
  if (target === 'master') {
    return ctx.offlineEngine.resolveStripInsertAutomationId('master', insertIndex, paramName);
  }
  const laneIndex = ctx.ensureTrackLane(target);
  return ctx.offlineEngine.resolveStripInsertAutomationId(
    ctx.trackLaneIds[laneIndex],
    insertIndex,
    paramName,
  );
}

/**
 * Resolves a bus-strip insert parameter to its reserved insert-automation id.
 * Declares the mixer bus first so the offline engine resolves the same bus
 * selector.
 *
 * @param busId Bus id (declares the mixer bus on first use).
 * @param insertIndex Index into the bus strip's insert sequence.
 * @param paramName Processor JSON-key parameter name.
 * @returns Reserved insert-automation id.
 * @throws RangeError when the bus, insert or key is unknown.
 */
export function resolveBusInsertAutomationId(
  ctx: EngineParameterContext,
  busId: number,
  insertIndex: number,
  paramName: string,
): number {
  ctx.ensureBus(busId);
  return ctx.offlineEngine.resolveBusInsertAutomationId(busId, insertIndex, paramName);
}

/**
 * Resolves a hosted instrument's continuous parameter to its reserved
 * instrument-automation id, so an instrument parameter is driven from a
 * breakpoint lane at audio-block precision like a strip insert.
 *
 * The instrument must already be bound on the offline engine for the
 * destination (the worklet mirrors every instrument sync onto it), otherwise
 * the resolve throws RangeError. Both engines number the id when the instrument is
 * bound, so the worklet engine applies it without resolving.
 *
 * @param destinationId MIDI destination the instrument is bound to.
 * @param paramName Instrument JSON-key parameter name (e.g. `cutoffHz`).
 * @returns Reserved instrument-automation id.
 * @throws RangeError when the destination or key is unknown.
 */
export function resolveInstrumentAutomationId(
  ctx: EngineParameterContext,
  destinationId: number,
  paramName: string,
): number {
  return ctx.offlineEngine.resolveInstrumentAutomationId(destinationId, paramName);
}

/**
 * Returns the number of automation lanes installed on the engine, including
 * lanes whose breakpoint list is currently empty.
 *
 * @returns Engine-side automation lane count.
 */
export function automationLaneCount(ctx: EngineParameterContext): number {
  return ctx.offlineEngine.automationLaneCount;
}

export function parameters(ctx: EngineParameterContext): EngineParameterInfo[] {
  return ctx.offlineEngine.parameters();
}

/** Registers a parameter on both the offline mirror and live worklet engine. */
export function addParameter(ctx: EngineParameterContext, info: EngineParameterInfo): void {
  commitSync(
    ctx,
    (offline) => offline.addParameter(info),
    () => ctx.postSync({ type: 'syncParameters', parameters: parameters(ctx) }),
  );
}

/** Clears registered parameters on both engines; automation lanes stay in place. */
export function clearParameters(ctx: EngineParameterContext): void {
  commitSync(
    ctx,
    (offline) => offline.clearParameters(),
    () => ctx.postSync({ type: 'syncParameters', parameters: [] }),
  );
}
