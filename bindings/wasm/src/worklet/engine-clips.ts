import type { EngineClip, EngineMidiClipSchedule, RealtimeEngine } from '../index.js';
import type { ClipPageProvider } from '../realtime_engine.js';
import { commitStore } from './engine-commit.js';
import type { SonareEngineSyncMessage } from './messages.js';

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
  postSync(message: SonareEngineSyncMessage, transfer?: Transferable[]): void;
  resolveTargetId(target: string | number): number;
  ensureTrackLane(target: string | number): number;
  /**
   * The OPFS stream (primed through the worklet's pull bridge) whose provider
   * `clip` schedules, or undefined; throws when the clip id is not the stream's.
   */
  workletClipStream(clip: EngineClip): { clipId: number; streamKey: number } | undefined;
  /** True while an OPFS stream owns the worklet provider slot of `clipId`. */
  hasClipStream(clipId: number): boolean;
}

// Keep control-message work bounded even for long tempo-synced clips. The
// worklet creates a native page provider, receives these transferable PCM
// chunks, and only schedules the clip after all pages arrive.
const PREBAKED_CLIP_PAGE_THRESHOLD = 16_384;
const PREBAKED_CLIP_PAGE_FRAMES = 4_096;

/** Omitted, `0` and `'off'` all mean an unwarped clip, as the native reader treats them. */
function isWarpOff(mode: EngineClip['warpMode']): boolean {
  return mode === undefined || mode === 'off' || mode === 0;
}

export function addClip(
  ctx: EngineClipContext,
  trackId: string | number,
  buffer: Float32Array[] | ClipPageProvider,
  startPpq: number,
  opts: Partial<Omit<EngineClip, 'channels' | 'pageProvider' | 'startPpq'>> = {},
): number {
  const id = opts.id ?? ctx.allocateClipId();
  const clip: EngineClip = {
    ...opts,
    id,
    ...(Array.isArray(buffer) ? { channels: buffer } : { pageProvider: buffer }),
    startPpq,
    trackId: ctx.resolveTargetId(trackId),
  };
  ctx.ensureTrackLane(trackId);
  const staged = new Map(ctx.clips);
  staged.set(id, clip);
  // One clip changes, so neither engine copies the audio of the others again.
  commitClips(ctx, staged, [clip], [], (offline) => offline.upsertClip(clip));
  return id;
}

export function removeClip(ctx: EngineClipContext, clipId: number): void {
  const staged = new Map(ctx.clips);
  staged.delete(clipId);
  commitClips(ctx, staged, [], [clipId], (offline) => {
    if (ctx.clips.has(clipId)) {
      offline.removeClip(clipId);
    }
  });
}

/** Replaces the whole clip store with one clip and syncs the worklet, dropping every previous id. */
export function replaceClips(
  ctx: EngineClipContext,
  clip: EngineClip,
  previousClipIds: readonly number[],
): void {
  const staged = new Map<number, EngineClip>();
  if (clip.id !== undefined) {
    staged.set(clip.id, clip);
  }
  const clips = Array.from(staged.values());
  commitClips(
    ctx,
    staged,
    [clip],
    previousClipIds.filter((id) => id !== clip.id),
    (offline) => offline.setClips(clips),
  );
}

export function setMidiClips(
  ctx: EngineClipContext,
  clips: readonly EngineMidiClipSchedule[],
): void {
  const staged = new Map<number, EngineMidiClipSchedule>();
  for (const clip of clips) {
    const id = clip.id ?? ctx.allocateClipId();
    staged.set(id, { ...clip, id, events: clip.events.map((event) => ({ ...event })) });
  }
  const scheduled = Array.from(staged.values());
  commitStore(
    ctx,
    ctx.midiClips,
    staged,
    (offline) => offline.setMidiClips(scheduled),
    () => ctx.postSync({ type: 'syncMidiClips', clips: scheduled }),
  );
}

/**
 * Lets the offline engine validate the edit through `applyOffline`, then caches
 * the staged clip set and posts the delta. A page-provider upsert must name an
 * attached OPFS stream, which is checked before anything changes.
 */
function commitClips(
  ctx: EngineClipContext,
  staged: Map<number, EngineClip>,
  upserts: EngineClip[],
  removeIds: number[],
  applyOffline: (offline: RealtimeEngine) => void,
): void {
  for (const clip of upserts) {
    if (!clip.channels && clip.pageProvider !== undefined && !ctx.workletClipStream(clip)) {
      throw new Error('A pageProvider on SonareEngine must be created by attachOpfsClipStream().');
    }
  }
  commitStore(ctx, ctx.clips, staged, applyOffline, () => postClipsDelta(ctx, upserts, removeIds));
}

function postClipsDelta(ctx: EngineClipContext, upserts: EngineClip[], removeIds: number[]): void {
  const preparedById = new Map<number, EngineClip>();
  for (const clip of upserts) {
    if (clip.id === undefined) {
      continue;
    }
    const bakedChannels = ctx.offlineEngine.prebakedClipChannels(clip.id);
    preparedById.set(
      clip.id,
      bakedChannels === null
        ? clip
        : {
            ...clip,
            channels: bakedChannels,
            clipOffsetSamples: 0,
            lengthSamples: bakedChannels[0]?.length ?? 0,
            loop: false,
            warpMode: 'off',
            warpAnchors: undefined,
          },
    );
  }
  const inlineUpserts: EngineClip[] = [];
  for (const clip of upserts) {
    const prepared = clip.id === undefined ? clip : (preparedById.get(clip.id) ?? clip);
    const channels = prepared.channels;
    const stream =
      !channels && prepared.pageProvider !== undefined
        ? ctx.workletClipStream(prepared)
        : undefined;
    if (stream) {
      ctx.postSync({
        type: 'syncClipPageCommit',
        clipId: stream.clipId,
        streamKey: stream.streamKey,
        clip: { ...prepared, channels: undefined, pageProvider: undefined },
      });
      continue;
    }
    if (
      prepared.id === undefined ||
      // The worklet holds one provider per clip id; a stream's is not displaced.
      ctx.hasClipStream(prepared.id) ||
      !isWarpOff(prepared.warpMode) ||
      !channels ||
      channels.length === 0 ||
      channels[0].length <= PREBAKED_CLIP_PAGE_THRESHOLD
    ) {
      inlineUpserts.push(prepared);
      continue;
    }
    const numSamples = channels[0].length;
    ctx.postSync({
      type: 'syncClipPageProvider',
      clipId: prepared.id,
      clip: { ...prepared, channels: undefined, pageProvider: undefined },
      numChannels: channels.length,
      numSamples,
      pageFrames: PREBAKED_CLIP_PAGE_FRAMES,
    });
    for (
      let start = 0, pageIndex = 0;
      start < numSamples;
      start += PREBAKED_CLIP_PAGE_FRAMES, pageIndex++
    ) {
      const page = channels.map((channel) =>
        channel.slice(start, start + PREBAKED_CLIP_PAGE_FRAMES),
      );
      ctx.postSync(
        { type: 'syncClipPage', clipId: prepared.id, pageIndex, channels: page },
        page.map((channel) => channel.buffer as Transferable),
      );
    }
    ctx.postSync({ type: 'syncClipPageCommit', clipId: prepared.id });
  }
  ctx.postSync({
    type: 'syncClipsDelta',
    upserts: inlineUpserts,
    removeIds,
  });
}
