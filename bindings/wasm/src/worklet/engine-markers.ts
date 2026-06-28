import type { EngineMarker, RealtimeEngine } from '../index';
import { resolveMarkerSet } from './engine-offline';
import type { SonareEngineSyncMessage } from './messages';
import { type SonareEngineCommandRecord, SonareEngineCommandType } from './protocol';

/**
 * Collaborator surface the marker helpers need from the owning
 * {@link SonareEngine}: the marker store and id counter, the offline engine they
 * mirror into, the sync poster, the realtime command sender, and the loop setter.
 */
export interface EngineMarkerContext {
  readonly offlineEngine: RealtimeEngine;
  readonly markers: Map<number, EngineMarker>;
  getNextMarkerId(): number;
  setNextMarkerId(value: number): void;
  postSync(message: SonareEngineSyncMessage): void;
  sendCommand(command: SonareEngineCommandRecord): boolean;
  setLoop(startPpq: number, endPpq: number, enabled: boolean): boolean;
}

export function addMarker(ctx: EngineMarkerContext, ppq: number, name = ''): number {
  const id = ctx.getNextMarkerId();
  ctx.setNextMarkerId(id + 1);
  ctx.markers.set(id, { id, ppq, name });
  syncMarkers(ctx);
  return id;
}

/**
 * Replaces the whole marker set in one call.
 *
 * Entries without an `id` are assigned fresh ids; entries carrying an `id`
 * keep it (ids must be positive and unique within the list). Returns the
 * resolved markers in the order given, so a caller can map its own marker
 * identities to the engine ids used by `seekMarker`/`setLoopFromMarkers`.
 *
 * @param markers The full marker list (an empty list clears all markers).
 * @returns The markers with their resolved engine ids.
 */
export function setMarkers(
  ctx: EngineMarkerContext,
  markers: ReadonlyArray<{ ppq: number; name?: string; id?: number }>,
): EngineMarker[] {
  const { resolved, nextMarkerId } = resolveMarkerSet(markers, ctx.getNextMarkerId());
  ctx.setNextMarkerId(nextMarkerId);
  ctx.markers.clear();
  for (const marker of resolved) {
    ctx.markers.set(marker.id, marker);
  }
  syncMarkers(ctx);
  return resolved.map((marker) => ({ ...marker }));
}

export function markerCount(ctx: EngineMarkerContext): number {
  return ctx.offlineEngine.markerCount();
}

export function markerByIndex(ctx: EngineMarkerContext, index: number): EngineMarker {
  return ctx.offlineEngine.markerByIndex(index);
}

export function marker(ctx: EngineMarkerContext, markerId: number): EngineMarker {
  return ctx.offlineEngine.marker(markerId);
}

export function seekMarker(ctx: EngineMarkerContext, markerId: number): boolean {
  ctx.offlineEngine.seekMarker(markerId);
  // Forward to the live worklet engine. Its marker set is kept in sync via the
  // 'syncMarkers' message (see syncMarkers), so a queued kSeekMarker resolves
  // the marker id to its frame on the audio thread.
  return ctx.sendCommand({
    type: SonareEngineCommandType.SeekMarker,
    targetId: markerId,
    sampleTime: -1,
  });
}

export function setLoopFromMarkers(
  ctx: EngineMarkerContext,
  startMarkerId: number,
  endMarkerId: number,
): boolean {
  ctx.offlineEngine.setLoopFromMarkers(startMarkerId, endMarkerId);
  const start = ctx.offlineEngine.marker(startMarkerId);
  const end = ctx.offlineEngine.marker(endMarkerId);
  return ctx.setLoop(start.ppq, end.ppq, true);
}

function syncMarkers(ctx: EngineMarkerContext): void {
  const markers = Array.from(ctx.markers.values()).sort((a, b) => a.ppq - b.ppq);
  ctx.offlineEngine.setMarkers(markers);
  ctx.postSync({ type: 'syncMarkers', markers });
}
