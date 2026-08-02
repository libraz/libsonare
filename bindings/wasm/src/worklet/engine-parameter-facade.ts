import type { EngineAutomationPoint, EngineParameterInfo, RealtimeEngine } from '../index';
import type { SonareEngineSyncMessage } from './messages';
import {
  ENGINE_MIXER_PARAM_FADER_DB,
  ENGINE_MIXER_PARAM_PAN,
  engineMixerBusTarget,
  engineMixerLaneTarget,
  engineMixerMasterTarget,
  SonareEngineCommandType,
} from './protocol';

/**
 * Collaborator surface the parameter / automation-id resolution helpers need
 * from the owning {@link SonareEngine}: the offline engine they mirror into and
 * query, the realtime node they command, the lane/bus declaration helpers, and
 * the declared track-lane id store used to resolve insert-automation ids.
 */
export interface EngineParameterContext {
  readonly offlineEngine: RealtimeEngine;
  sendCommand(command: {
    type: SonareEngineCommandType;
    targetId?: number;
    sampleTime?: number;
    argFloat?: number;
    argInt?: number;
  }): boolean;
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
): boolean {
  const paramId = ctx.resolveParamId(nodeId, param);
  // Mirror the change into the offline engine so a subsequent offline render
  // reflects the live value, then push a sample-accurate command to the
  // realtime runtime (mirrors setTempo/setLoop above).
  ctx.offlineEngine.setParameter(paramId, value);
  return ctx.sendCommand({
    type: SonareEngineCommandType.SetParam,
    targetId: paramId,
    sampleTime: -1,
    argFloat: value,
  });
}

export function setSoloMute(
  ctx: EngineParameterContext,
  target: string | number,
  solo: boolean,
  mute: boolean,
): boolean {
  const laneIndex = ctx.ensureTrackLane(target);
  ctx.offlineEngine.setSoloMute(laneIndex, solo, mute);
  return ctx.sendCommand({
    type: SonareEngineCommandType.SetSoloMute,
    targetId: laneIndex,
    sampleTime: -1,
    argInt: (mute ? 0x1 : 0) | (solo ? 0x2 : 0),
  });
}

/**
 * Returns the automation target id for a mixer strip parameter.
 *
 * The id addresses the engine's reserved mixer namespace, so it can be fed
 * straight to setAutomationLane to automate a fader or pan without
 * registering a parameter.
 *
 * @param target Track id (declares a mixer lane on first use) or 'master'.
 * @param kind Strip parameter to address.
 * @returns Reserved engine parameter id for the strip parameter.
 */
export function automationParamId(
  ctx: EngineParameterContext,
  target: string | number,
  kind: 'faderDb' | 'pan',
): number {
  const paramKind = kind === 'pan' ? ENGINE_MIXER_PARAM_PAN : ENGINE_MIXER_PARAM_FADER_DB;
  if (target === 'master') {
    return engineMixerMasterTarget(paramKind);
  }
  return engineMixerLaneTarget(ctx.ensureTrackLane(target), paramKind);
}

/**
 * Returns the automation target id for a bus fader.
 *
 * @param busId Bus id (declares the mixer bus on first use).
 * @returns Reserved engine parameter id for the bus fader gain (dB).
 */
export function busAutomationParamId(ctx: EngineParameterContext, busId: number): number {
  return engineMixerBusTarget(ctx.ensureBus(busId), ENGINE_MIXER_PARAM_FADER_DB);
}

/**
 * Resolves a track-lane insert parameter (JSON-key name) to the reserved
 * insert-automation id fed straight to setAutomationLane. Declares the track's
 * mixer lane first (like automationParamId) so the offline engine resolves the
 * same strip selector the realtime engine uses.
 *
 * @param target Track id (declares a mixer lane on first use).
 * @param insertIndex Index into the strip's combined insert sequence.
 * @param paramName Processor JSON-key parameter name.
 * @returns Reserved insert-automation id, or -1 when strip/insert/key unknown.
 */
export function resolveTrackInsertAutomationId(
  ctx: EngineParameterContext,
  target: string | number,
  insertIndex: number,
  paramName: string,
): number {
  const laneIndex = ctx.ensureTrackLane(target);
  return ctx.offlineEngine.resolveTrackInsertAutomationId(
    ctx.trackLaneIds[laneIndex],
    insertIndex,
    paramName,
  );
}

/**
 * Resolves a master-strip insert parameter to its reserved insert-automation
 * id.
 *
 * @param insertIndex Index into the master strip's insert sequence.
 * @param paramName Processor JSON-key parameter name.
 * @returns Reserved insert-automation id, or -1 when insert/key unknown.
 */
export function resolveMasterInsertAutomationId(
  ctx: EngineParameterContext,
  insertIndex: number,
  paramName: string,
): number {
  return ctx.offlineEngine.resolveMasterInsertAutomationId(insertIndex, paramName);
}

/**
 * Resolves a bus-strip insert parameter to its reserved insert-automation id.
 * Declares the mixer bus first so the offline engine resolves the same bus
 * selector.
 *
 * @param busId Bus id (declares the mixer bus on first use).
 * @param insertIndex Index into the bus strip's insert sequence.
 * @param paramName Processor JSON-key parameter name.
 * @returns Reserved insert-automation id, or -1 when bus/insert/key unknown.
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
 * Returns the number of automation lanes installed on the engine, including
 * lanes whose breakpoint list is currently empty.
 *
 * @returns Engine-side automation lane count.
 */
export function automationLaneCount(ctx: EngineParameterContext): number {
  return ctx.offlineEngine.automationLaneCount();
}

export function listParameters(ctx: EngineParameterContext): EngineParameterInfo[] {
  const parameters: EngineParameterInfo[] = [];
  for (let index = 0; index < ctx.offlineEngine.parameterCount(); index++) {
    parameters.push(ctx.offlineEngine.parameterInfoByIndex(index));
  }
  return parameters;
}

/** Registers a parameter on both the offline mirror and live worklet engine. */
export function addParameter(ctx: EngineParameterContext, info: EngineParameterInfo): void {
  ctx.offlineEngine.addParameter(info);
  ctx.postSync({ type: 'syncParameters', parameters: listParameters(ctx) });
}

/** Clears registered parameters and their automation lanes on both engines. */
export function clearParameters(ctx: EngineParameterContext): void {
  ctx.offlineEngine.clearParameters();
  ctx.automationLanes.clear();
  ctx.postSync({ type: 'syncParameters', parameters: [] });
}
