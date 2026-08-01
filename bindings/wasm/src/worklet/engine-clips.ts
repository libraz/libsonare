import type { EngineClip, EngineMidiClipSchedule, RealtimeEngine } from '../index';
import type { ClipPageProvider } from '../realtime_engine';
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
  postSync(message: SonareEngineSyncMessage, transfer?: Transferable[]): void;
  resolveTargetId(target: string | number): number;
  ensureTrackLane(target: string | number): number;
  /** Commits a provider already primed through the worklet's OPFS pull bridge. */
  commitWorkletClipPageProvider(clip: EngineClip): boolean;
}

// Keep control-message work bounded even for long tempo-synced clips. The
// worklet creates a native page provider, receives these transferable PCM
// chunks, and only schedules the clip after all pages arrive.
const PREBAKED_CLIP_PAGE_THRESHOLD = 16_384;
const PREBAKED_CLIP_PAGE_FRAMES = 4_096;

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
  const preparedById = new Map<number, EngineClip>();
  for (const clip of clips) {
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
    if (!channels && prepared.pageProvider !== undefined) {
      if (ctx.commitWorkletClipPageProvider(prepared)) {
        continue;
      }
      throw new Error('A pageProvider on SonareEngine must be created by attachOpfsClipStream().');
    }
    if (
      prepared.id === undefined ||
      prepared.warpMode !== 'off' ||
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

function syncMidiClips(ctx: EngineClipContext): void {
  const clips = Array.from(ctx.midiClips.values());
  ctx.offlineEngine.setMidiClips(clips);
  ctx.postSync({ type: 'syncMidiClips', clips });
}
