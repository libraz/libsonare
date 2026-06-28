import type { EngineAutomationPoint, RealtimeEngine } from '../index';
import { curveCode } from './engine-sync';
import type { SonareEngineSyncMessage } from './messages';

/**
 * Collaborator surface the automation-lane helpers need from the owning
 * {@link SonareEngine}: the lane store they mutate, the offline engine they
 * mirror into, the sync poster, and the parameter-id resolver.
 */
export interface EngineAutomationContext {
  readonly offlineEngine: RealtimeEngine;
  readonly automationLanes: Map<number, EngineAutomationPoint[]>;
  postSync(message: SonareEngineSyncMessage): void;
  resolveParamId(nodeId: string, param: string | number): number;
}

export function scheduleParam(
  ctx: EngineAutomationContext,
  nodeId: string,
  param: string | number,
  ppq: number,
  value: number,
  curve: number | 'linear' | 'exponential' = 'linear',
): void {
  const paramId = ctx.resolveParamId(nodeId, param);
  const lane = ctx.automationLanes.get(paramId) ?? [];
  lane.push({ ppq, value, curveToNext: curveCode(curve) });
  lane.sort((a, b) => a.ppq - b.ppq);
  ctx.automationLanes.set(paramId, lane);
  ctx.offlineEngine.setAutomationLane(paramId, lane);
  // Mirror the lane to the live worklet engine so scheduled automation plays
  // back in real time, not just in renderOffline(). Lanes can exceed the
  // fixed-size SAB command record, so they ride an out-of-band 'syncAutomation'
  // message applied outside process() (like syncClips/syncMarkers).
  ctx.postSync({ type: 'syncAutomation', paramId, points: lane });
}

export function addAutomationPoint(
  ctx: EngineAutomationContext,
  laneId: string | number,
  ppq: number,
  value: number,
  curve: number | 'linear' | 'exponential' = 'linear',
): void {
  scheduleParam(ctx, '', laneId, ppq, value, curve);
}

/**
 * Replaces the automation lane for `paramId` with the given breakpoints.
 *
 * Unlike scheduleParam (which appends a single point), this sets the whole
 * lane at once; an empty array clears the lane. The points are defensively
 * copied and sorted by ppq before being mirrored to the offline engine and
 * the live worklet engine.
 *
 * @param paramId Automation target id (registered parameter or a reserved
 *   engine mixer target from automationParamId/busAutomationParamId).
 * @param points Lane breakpoints; order does not matter.
 */
export function setAutomationLane(
  ctx: EngineAutomationContext,
  paramId: number,
  points: ReadonlyArray<EngineAutomationPoint>,
): void {
  const sorted = points.map((point) => ({ ...point })).sort((a, b) => a.ppq - b.ppq);
  if (sorted.length === 0) {
    ctx.automationLanes.delete(paramId);
  } else {
    ctx.automationLanes.set(paramId, sorted);
  }
  ctx.offlineEngine.setAutomationLane(paramId, sorted);
  ctx.postSync({ type: 'syncAutomation', paramId, points: sorted });
}
