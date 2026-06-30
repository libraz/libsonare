import type {
  EngineTempoSegment,
  EngineTimeSignatureSegment,
  EngineTransportState,
  RealtimeEngine,
} from '../index';
import type { SonareRealtimeEngineNode } from './engine-node';
import { buildTempoSync } from './engine-sync';
import type { SonareEngineSyncMessage } from './messages';
import { SonareEngineCommandType } from './protocol';

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
export interface EngineTempoContext {
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
  ctx.setTempoBpm(bpm);
  ctx.setTempoSegments([{ startPpq: 0, bpm }]);
  ctx.offlineEngine.setTempo(bpm);
  postTempoSync(ctx);
  ctx.realtimeNode.sendCommand({
    type: SonareEngineCommandType.SetTempoMap,
    sampleTime: -1,
    argFloat: bpm,
  });
}

export function setTempoSegments(
  ctx: EngineTempoContext,
  segments: readonly EngineTempoSegment[],
): void {
  const copied = segments.map((segment) => ({ ...segment }));
  ctx.setTempoSegments(copied);
  ctx.setTempoBpm(copied[0]?.bpm ?? ctx.getTempoBpm());
  ctx.offlineEngine.setTempoSegments(copied);
  postTempoSync(ctx);
}

export function setTimeSignature(
  ctx: EngineTempoContext,
  numerator: number,
  denominator: number,
): void {
  ctx.setTimeSignature({ numerator, denominator });
  ctx.setTimeSignatureSegments([{ startPpq: 0, numerator, denominator }]);
  ctx.offlineEngine.setTimeSignature(numerator, denominator);
  postTempoSync(ctx);
}

export function setTimeSignatureSegments(
  ctx: EngineTempoContext,
  segments: readonly EngineTimeSignatureSegment[],
): void {
  const copied = segments.map((segment) => ({ ...segment }));
  ctx.setTimeSignatureSegments(copied);
  const first = copied[0];
  if (first) {
    ctx.setTimeSignature({ numerator: first.numerator, denominator: first.denominator });
  }
  ctx.offlineEngine.setTimeSignatureSegments(copied);
  postTempoSync(ctx);
}

export function setLoop(
  ctx: EngineTempoContext,
  startPpq: number,
  endPpq: number,
  enabled = true,
): boolean {
  ctx.offlineEngine.setLoop(startPpq, endPpq, enabled);
  // Transport precision contract: the SAB command record carries exactly one
  // Float64 lane (argFloat) and one Int64 lane (argInt). startPpq travels in
  // argFloat with full double precision, matching the offline engine; endPpq
  // is carried as micro-PPQ (round(endPpq * 1e6)) in the integer lane and
  // divided back by 1e6 on the consumer. Loop ENDS are therefore snapped to
  // the nearest 1e-6 PPQ over the realtime transport (max 5e-7 PPQ drift),
  // while loop STARTS and the offline path stay exact. This is intentional:
  // the record has no second free Float64 lane, and a micro-PPQ grid on the
  // loop end is well below audible/sample-accurate resolution at any tempo.
  return ctx.realtimeNode.sendCommand({
    type: SonareEngineCommandType.SetLoop,
    targetId: enabled ? 1 : 0,
    sampleTime: -1,
    argFloat: startPpq,
    argInt: Math.round(endPpq * 1_000_000),
  });
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
