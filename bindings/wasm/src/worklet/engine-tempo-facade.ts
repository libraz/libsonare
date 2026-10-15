import type {
  EngineTempoSegment,
  EngineTimeSignatureSegment,
  EngineTransportState,
  RealtimeEngine,
} from '../index.js';
import { commitCommand, commitSync, type EngineCommitContext } from './engine-commit.js';
import type { SonareRealtimeEngineNode } from './engine-node.js';
import { buildTempoSync } from './engine-sync.js';
import type { SonareEngineSyncMessage } from './messages.js';
import { SonareEngineCommandType } from './protocol.js';

interface TimeSignature {
  numerator: number;
  denominator: number;
}

/**
 * Collaborator surface the tempo / time-signature / loop / transport helpers
 * need from the owning {@link SonareEngine}: the offline engine they mirror
 * into, the realtime node they command and query, the out-of-band sync poster,
 * and getters/setters for the tempo-map and cached-transport state the engine
 * holds (so the helpers mutate it by reference).
 */
export interface EngineTempoContext extends EngineCommitContext {
  readonly offlineEngine: RealtimeEngine;
  readonly realtimeNode: SonareRealtimeEngineNode;
  postSync(message: SonareEngineSyncMessage): void;
  getTempoBpm(): number;
  setTempoBpm(bpm: number): void;
  getTimeSignature(): TimeSignature;
  setTimeSignature(signature: TimeSignature): void;
  getTempoSegments(): EngineTempoSegment[];
  setTempoSegments(segments: EngineTempoSegment[]): void;
  getTimeSignatureSegments(): EngineTimeSignatureSegment[];
  setTimeSignatureSegments(segments: EngineTimeSignatureSegment[]): void;
  setLatestTransportState(state: EngineTransportState): void;
  getLatestTransportState(): EngineTransportState | undefined;
}

// Posts the full tempo/time-signature map to the worklet engine processor.
export function postTempoSync(ctx: EngineTempoContext): void {
  ctx.postSync(
    buildTempoSync(
      ctx.getTempoBpm(),
      ctx.getTimeSignature(),
      ctx.getTempoSegments(),
      ctx.getTimeSignatureSegments(),
    ),
  );
}

export function setTempo(ctx: EngineTempoContext, bpm: number): void {
  commitSync(
    ctx,
    (offline) => offline.setTempo(bpm),
    () => {
      ctx.setTempoBpm(bpm);
      ctx.setTempoSegments([{ startPpq: 0, bpm }]);
      postTempoSync(ctx);
    },
  );
}

export function setTempoSegments(
  ctx: EngineTempoContext,
  segments: readonly EngineTempoSegment[],
): void {
  const copied = segments.map((segment) => ({ ...segment }));
  commitSync(
    ctx,
    (offline) => offline.setTempoSegments(copied),
    () => {
      ctx.setTempoSegments(copied);
      ctx.setTempoBpm(copied[0]?.bpm ?? ctx.getTempoBpm());
      postTempoSync(ctx);
    },
  );
}

export function setTimeSignature(
  ctx: EngineTempoContext,
  numerator: number,
  denominator: number,
): void {
  commitSync(
    ctx,
    (offline) => offline.setTimeSignature(numerator, denominator),
    () => {
      ctx.setTimeSignature({ numerator, denominator });
      ctx.setTimeSignatureSegments([{ startPpq: 0, numerator, denominator }]);
      postTempoSync(ctx);
    },
  );
}

export function setTimeSignatureSegments(
  ctx: EngineTempoContext,
  segments: readonly EngineTimeSignatureSegment[],
): void {
  const copied = segments.map((segment) => ({ ...segment }));
  commitSync(
    ctx,
    (offline) => offline.setTimeSignatureSegments(copied),
    () => {
      ctx.setTimeSignatureSegments(copied);
      const first = copied[0];
      if (first) {
        ctx.setTimeSignature({ numerator: first.numerator, denominator: first.denominator });
      }
      postTempoSync(ctx);
    },
  );
}

export function setLoop(
  ctx: EngineTempoContext,
  startPpq: number,
  endPpq: number,
  enabled = true,
): boolean {
  // Both bounds travel as full doubles (argFloat and argFloat2), so the live
  // loop is bit-identical to the offline one.
  return commitCommand(
    ctx,
    {
      type: SonareEngineCommandType.SetLoop,
      targetId: enabled ? 1 : 0,
      sampleTime: -1,
      argFloat: startPpq,
      argFloat2: endPpq,
    },
    (offline) => offline.setLoop(startPpq, endPpq, enabled),
  );
}

export function countInEndSample(
  ctx: EngineTempoContext,
  startSample: number,
  bars: number,
): number {
  return ctx.offlineEngine.countInEndSample(startSample, bars);
}

export async function getTransportState(ctx: EngineTempoContext): Promise<EngineTransportState> {
  const state = await ctx.realtimeNode.requestTransportState();
  ctx.setLatestTransportState(state);
  return state;
}

export function cachedTransportState(ctx: EngineTempoContext): EngineTransportState | undefined {
  return ctx.getLatestTransportState();
}
