import type { EngineClip, EngineMidiClipSchedule, RealtimeEngine } from '../index';
import type { SonareEngineSyncMessage } from './messages';

/**
 * Collaborator surface the audio/MIDI clip scheduling helpers need from the
 * owning {@link SonareEngine}: the clip stores they mutate, the offline engine
 * they mirror into, the sync poster, and the clip-id allocator / lane resolvers.
 */
export interface EngineClipContext {
  readonly offlineEngine: RealtimeEngine;
  readonly clips: Map<number, EngineClip>;
  readonly midiClips: Map<number, EngineMidiClipSchedule>;
  allocateClipId(): number;
  postSync(message: SonareEngineSyncMessage): void;
  resolveTargetId(target: string | number): number;
  ensureTrackLane(target: string | number): number;
}

export function addClip(
  ctx: EngineClipContext,
  trackId: string | number,
  buffer: Float32Array[],
  startPpq: number,
  opts: Partial<Omit<EngineClip, 'channels' | 'startPpq'>> = {},
): number {
  const id = opts.id ?? ctx.allocateClipId();
  const clip: EngineClip = {
    ...opts,
    id,
    channels: buffer,
    startPpq,
    trackId: ctx.resolveTargetId(trackId),
  };
  ctx.ensureTrackLane(trackId);
  ctx.clips.set(id, clip);
  syncClipsDelta(ctx, [clip], []);
  return id;
}

export function removeClip(ctx: EngineClipContext, clipId: number): void {
  ctx.clips.delete(clipId);
  syncClipsDelta(ctx, [], [clipId]);
}

export function setMidiClips(
  ctx: EngineClipContext,
  clips: readonly EngineMidiClipSchedule[],
): void {
  ctx.midiClips.clear();
  for (const clip of clips) {
    const id = clip.id ?? ctx.allocateClipId();
    ctx.midiClips.set(id, { ...clip, id, events: clip.events.map((event) => ({ ...event })) });
  }
  syncMidiClips(ctx);
}

function syncClipsDelta(ctx: EngineClipContext, upserts: EngineClip[], removeIds: number[]): void {
  const clips = Array.from(ctx.clips.values());
  ctx.offlineEngine.setClips(clips);
  ctx.postSync({
    type: 'syncClipsDelta',
    upserts,
    removeIds,
  });
}

function syncMidiClips(ctx: EngineClipContext): void {
  const clips = Array.from(ctx.midiClips.values());
  ctx.offlineEngine.setMidiClips(clips);
  ctx.postSync({ type: 'syncMidiClips', clips });
}
