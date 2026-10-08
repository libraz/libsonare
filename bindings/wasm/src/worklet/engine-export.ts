import type {
  EngineBounceOptions,
  EngineBounceResult,
  EngineClip,
  EngineFreezeOptions,
  EngineFreezeResult,
  RealtimeEngine,
} from '../index.js';
import type { OpfsClipPageProviderBinding } from '../opfs_clip_pages.js';
import type { OwnerEpoch } from '../owner_epoch.js';
import { requireIntegerOption } from './guards.js';

/** Request form of {@link SonareEngine.renderOffline}. */
export interface SonareRenderOfflineRequest {
  /** Length of the render in frames. */
  totalFrames: number;
  /**
   * Render block size. Defaults to the engine's offline block size and must not
   * exceed it; a larger value throws a `RangeError`.
   */
  blockSize?: number;
  /**
   * Whether this call ends the timeline. `true` (the default) releases every
   * sounding note and flushes the delay lines before returning. `false` renders
   * one chunk of a longer timeline; end it with {@link SonareEngine.finishOfflineRender}.
   * The transport position is restored when the timeline ends, not between chunks.
   */
  finalize?: boolean;
}

/** An OPFS-streamed clip's page bookkeeping, keyed by its page provider id. */
export interface OpfsExportSource {
  binding: OpfsClipPageProviderBinding;
  pageFrames: number;
  numSamples: number;
  /** Pages currently supplied to the mirror and the worklet. */
  residentPages: Set<number>;
}

/** Collaborator surface the offline export entry points need from the owning engine. */
export interface EngineExportContext {
  readonly offlineEngine: RealtimeEngine;
  readonly offlineBlockSize: number;
  readonly offlineChannelCount: number;
  readonly clips: ReadonlyMap<number, EngineClip>;
  readonly opfsSources: ReadonlyMap<number, OpfsExportSource>;
  /** `chunkOrigin` is the transport position the mirror returns to once a chunked render ends. */
  readonly session: { chunkOrigin: number | undefined };
  /** Owning engine's epoch; a render that resumes after destroy stops. */
  readonly epoch: OwnerEpoch;
  flushOfflineMirror(): void;
  /** Replaces the facade clip store and the worklet's clips with the frozen clip. */
  commitFrozenClip(clip: EngineClip, previousClipIds: number[]): void;
}

function mirrorPosition(ctx: EngineExportContext): number {
  ctx.flushOfflineMirror();
  return ctx.offlineEngine.getTransportState().samplePosition;
}

function restorePosition(ctx: EngineExportContext, position: number): void {
  ctx.offlineEngine.seekSample(position);
  ctx.flushOfflineMirror();
}

function resolveBlockSize(ctx: EngineExportContext, blockSize: number | undefined): number {
  const resolved = requireIntegerOption(blockSize, ctx.offlineBlockSize, 'blockSize', 1);
  if (resolved > ctx.offlineBlockSize) {
    throw new RangeError(
      `blockSize must not exceed the offline block size (${ctx.offlineBlockSize})`,
    );
  }
  return resolved;
}

/** Inclusive range of source pages a clip reads over the span, or null when it is silent there. */
function sourcePageRange(
  ctx: EngineExportContext,
  clip: EngineClip,
  source: OpfsExportSource,
  spanStart: number,
  spanEnd: number,
): [number, number] | null {
  const lastPage = Math.ceil(source.numSamples / source.pageFrames) - 1;
  const clipStart = ctx.offlineEngine.sampleAtPpq(clip.startPpq);
  if (clipStart >= spanEnd) {
    return null;
  }
  // A warped or looping clip reads the source out of timeline order.
  if (
    clip.loop ||
    (clip.warpMode !== undefined && clip.warpMode !== 'off' && clip.warpMode !== 0)
  ) {
    return [0, lastPage];
  }
  const offset = clip.clipOffsetSamples ?? 0;
  const length = clip.lengthSamples ? clip.lengthSamples : source.numSamples - offset;
  if (clipStart + length <= spanStart) {
    return null;
  }
  const first = offset + Math.max(0, spanStart - clipStart);
  const last = offset + Math.min(length, spanEnd - clipStart) - 1;
  return [
    Math.min(lastPage, Math.floor(first / source.pageFrames)),
    Math.min(lastPage, Math.floor(last / source.pageFrames)),
  ];
}

/**
 * Supplies every OPFS page the span reads that is not already resident and
 * returns a release function that evicts exactly those pages again.
 */
async function prefetchOpfsSpan(
  ctx: EngineExportContext,
  startSample: number,
  frames: number,
): Promise<() => void> {
  const supplied: Array<{ source: OpfsExportSource; page: number }> = [];
  const release = () => {
    for (const { source, page } of supplied) {
      source.binding.clearPage?.(page);
    }
    supplied.length = 0;
  };
  if (ctx.opfsSources.size === 0 || frames <= 0) {
    return release;
  }
  const token = ctx.epoch.current();
  try {
    for (const clip of ctx.clips.values()) {
      const providerId =
        typeof clip.pageProvider === 'object' ? clip.pageProvider?.id : clip.pageProvider;
      const source = providerId === undefined ? undefined : ctx.opfsSources.get(providerId);
      if (!source) {
        continue;
      }
      const range = sourcePageRange(ctx, clip, source, startSample, startSample + frames);
      if (!range) {
        continue;
      }
      for (let page = range[0]; page <= range[1]; page++) {
        if (source.residentPages.has(page)) {
          continue;
        }
        const ok = await source.binding.supplyPage(page);
        ctx.epoch.assertCurrent(token);
        if (!ok) {
          throw new Error(
            `Failed to page in OPFS clip ${clip.id ?? '?'} page ${page} for offline render.`,
          );
        }
        supplied.push({ source, page });
      }
    }
  } catch (error) {
    release();
    throw error;
  }
  return release;
}

/** Ends a chunked render: puts the mirror back where the first chunk started. */
function endTimeline(ctx: EngineExportContext, origin: number): void {
  restorePosition(ctx, origin);
  ctx.session.chunkOrigin = undefined;
}

export async function renderOffline(
  ctx: EngineExportContext,
  request: number | SonareRenderOfflineRequest,
): Promise<Float32Array[]> {
  const numeric = typeof request === 'number';
  const frames = numeric
    ? Math.max(0, Math.floor(request))
    : requireIntegerOption(request.totalFrames, Number.NaN, 'totalFrames', 0);
  const blockSize = resolveBlockSize(ctx, numeric ? undefined : request.blockSize);
  const finalize = numeric ? true : (request.finalize ?? true);
  const continuing = ctx.session.chunkOrigin !== undefined;
  const origin = ctx.session.chunkOrigin ?? mirrorPosition(ctx);
  const release = await prefetchOpfsSpan(ctx, mirrorPosition(ctx), frames);
  try {
    const channels: Float32Array[] = [];
    for (let ch = 0; ch < ctx.offlineChannelCount; ch++) {
      channels.push(new Float32Array(frames));
    }
    // A continuing chunk keeps the processor state the previous chunk left.
    if (!continuing) {
      ctx.offlineEngine.primeOfflineParameters(ctx.offlineChannelCount, blockSize);
    }
    const planes = ctx.offlineEngine.renderOffline({ channels, blockSize, finalize });
    if (finalize) {
      endTimeline(ctx, origin);
    } else {
      ctx.session.chunkOrigin = origin;
    }
    return planes;
  } catch (error) {
    endTimeline(ctx, origin);
    throw error;
  } finally {
    release();
  }
}

export function finishOfflineRender(ctx: EngineExportContext): void {
  ctx.offlineEngine.finishOfflineRender();
  if (ctx.session.chunkOrigin !== undefined) {
    endTimeline(ctx, ctx.session.chunkOrigin);
  }
}

function spanFrames(totalFrames: number): number {
  return Number.isFinite(totalFrames) ? Math.max(0, Math.floor(totalFrames)) : 0;
}

export async function bounceOffline(
  ctx: EngineExportContext,
  options: EngineBounceOptions,
): Promise<EngineBounceResult> {
  const blockSize = resolveBlockSize(ctx, options.blockSize);
  const origin = ctx.session.chunkOrigin ?? mirrorPosition(ctx);
  const release = await prefetchOpfsSpan(ctx, mirrorPosition(ctx), spanFrames(options.totalFrames));
  try {
    return ctx.offlineEngine.bounceOffline({ ...options, blockSize });
  } finally {
    release();
    endTimeline(ctx, origin);
  }
}

export async function freezeOffline(
  ctx: EngineExportContext,
  options: EngineFreezeOptions,
): Promise<EngineFreezeResult> {
  const blockSize = resolveBlockSize(ctx, options.blockSize);
  const origin = ctx.session.chunkOrigin ?? mirrorPosition(ctx);
  const release = await prefetchOpfsSpan(ctx, mirrorPosition(ctx), spanFrames(options.totalFrames));
  try {
    const previousClipIds = Array.from(ctx.clips.keys());
    const result = ctx.offlineEngine.freezeOffline({ ...options, blockSize });
    const channels = ctx.offlineEngine.prebakedClipChannels(result.clipId);
    if (channels === null) {
      throw new Error('The frozen clip audio is unavailable.');
    }
    ctx.commitFrozenClip(
      {
        id: result.clipId,
        channels,
        startPpq: options.startPpq ?? 0,
        gain: options.gain ?? 1,
        clipOffsetSamples: 0,
        lengthSamples: result.frames,
        loop: false,
      },
      previousClipIds,
    );
    return result;
  } finally {
    release();
    endTimeline(ctx, origin);
  }
}
