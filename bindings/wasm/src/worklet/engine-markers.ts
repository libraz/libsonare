import type { EngineMarker, RealtimeEngine } from '../index.js';
import { commitCommand, commitStore, type EngineCommitContext } from './engine-commit.js';
import { resolveMarkerSet } from './engine-offline.js';
import type { SonareEngineSyncMessage } from './messages.js';
import { SonareEngineCommandType } from './protocol.js';

/**
 * Collaborator surface the marker helpers need from the owning
 * {@link SonareEngine}: the marker store and id counter, the offline engine they
 * mirror into, the sync poster, the realtime command sender, and the loop setter.
 */
export interface EngineMarkerContext extends EngineCommitContext {
  readonly offlineEngine: RealtimeEngine;
  readonly markers: Map<number, EngineMarker>;
  getNextMarkerId(): number;
  setNextMarkerId(value: number): void;
  postSync(message: SonareEngineSyncMessage): void;
  setLoop(startPpq: number, endPpq: number, enabled: boolean): boolean;
}

export function addMarker(ctx: EngineMarkerContext, ppq: number, name = ''): number {
  const id = ctx.getNextMarkerId();
  const staged = new Map(ctx.markers);
  staged.set(id, { id, ppq, name });
  commitMarkers(ctx, staged);
  ctx.setNextMarkerId(id + 1);
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
  commitMarkers(ctx, new Map(resolved.map((marker) => [marker.id, marker])));
  ctx.setNextMarkerId(nextMarkerId);
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
  // The live marker set arrives through 'syncMarkers', so a queued kSeekMarker
  // resolves the id to its frame on the audio thread.
  return commitCommand(
    ctx,
    { type: SonareEngineCommandType.SeekMarker, targetId: markerId, sampleTime: -1 },
    (offline) => offline.seekMarker(markerId),
  );
}

export function setLoopFromMarkers(
  ctx: EngineMarkerContext,
  startMarkerId: number,
  endMarkerId: number,
): boolean {
  // Resolving both ids first refuses an unknown marker before either engine changes.
  const start = ctx.offlineEngine.marker(startMarkerId);
  const end = ctx.offlineEngine.marker(endMarkerId);
  return ctx.setLoop(start.ppq, end.ppq, true);
}

/** Lets the offline engine validate the staged marker set, then caches and posts it. */
function commitMarkers(ctx: EngineMarkerContext, staged: Map<number, EngineMarker>): void {
  const markers = Array.from(staged.values()).sort((a, b) => a.ppq - b.ppq);
  commitStore(
    ctx,
    ctx.markers,
    staged,
    (offline) => offline.setMarkers(markers),
    () => ctx.postSync({ type: 'syncMarkers', markers }),
  );
}
