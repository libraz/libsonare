import type { EngineAutomationPoint, RealtimeEngine } from '../index.js';
import { requireListBudget } from '../internal/list_budget.js';
import type { SonareEngineSyncMessage } from '../internal/worklet_messages.js';
import { requestObject } from '../validation.js';
import { commitStore } from './engine-commit.js';

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
  point: EngineAutomationPoint,
): void {
  requestObject('scheduleParam', point, 'point', true);
  const paramId = ctx.resolveParamId(nodeId, param);
  const lane = [...(ctx.automationLanes.get(paramId) ?? [])];
  lane.push({ ...point });
  lane.sort((a, b) => a.ppq - b.ppq);
  // Lanes can exceed the fixed-size SAB command record, so the live engine gets
  // them through an out-of-band 'syncAutomation' message.
  commitLane(ctx, paramId, lane);
}

export function addAutomationPoint(
  ctx: EngineAutomationContext,
  laneId: string | number,
  point: EngineAutomationPoint,
): void {
  scheduleParam(ctx, '', laneId, point);
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
 *   engine mixer target from resolveStripAutomationId/resolveBusAutomationId).
 * @param points Lane breakpoints; order does not matter.
 */
export function setAutomationLane(
  ctx: EngineAutomationContext,
  paramId: number,
  points: ReadonlyArray<EngineAutomationPoint>,
): void {
  if (Array.isArray(points)) {
    requireListBudget(points, 'points');
  }
  const sorted = points.map((point) => ({ ...point })).sort((a, b) => a.ppq - b.ppq);
  commitLane(ctx, paramId, sorted);
}

/** Stages `lane` (empty removes it), lets the offline engine validate it, then caches and posts it. */
function commitLane(
  ctx: EngineAutomationContext,
  paramId: number,
  lane: EngineAutomationPoint[],
): void {
  const staged = new Map(ctx.automationLanes);
  if (lane.length === 0) {
    staged.delete(paramId);
  } else {
    staged.set(paramId, lane);
  }
  commitStore(
    ctx,
    ctx.automationLanes,
    staged,
    (offline) => offline.setAutomationLane(paramId, lane),
    () => ctx.postSync({ type: 'syncAutomation', paramId, points: lane }),
  );
}
