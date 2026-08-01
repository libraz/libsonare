import {
  createOpfsClipPageProvider,
  type OpfsClipPageProviderBinding,
  type OpfsClipPageProviderOptions,
} from './opfs_clip_pages';
import type { ClipPageProvider, ClipPageRequest, RealtimeEngine } from './realtime_engine';

/**
 * Minimal engine surface the streamer drives. {@link RealtimeEngine} satisfies
 * this structurally; tests can supply a lightweight stand-in.
 */
export interface ClipPageStreamerEngine {
  /** Drain one pending audio-thread page-miss request, or `null` when empty. */
  popClipPageRequest(): ClipPageStreamerRequest | null;
}

/**
 * A page miss reported by either a direct engine (`sample`) or the worklet
 * bridge (`pageIndex`). Exactly one position form is required.
 */
export interface ClipPageStreamerRequest {
  clipId: number;
  sample?: number;
  pageIndex?: number;
}

/** A paged clip the streamer keeps fed from its backing store. */
export interface ClipPageStreamSource {
  /** Clip schedule id passed to `setClips` (matches {@link ClipPageRequest.clipId}). */
  clipId: number;
  /** OPFS-backed page provider binding for this clip. */
  binding: OpfsClipPageProviderBinding;
  /** Page size in frames (must equal the provider's `pageFrames`). */
  pageFrames: number;
  /** Total sample count of the clip source. */
  numSamples: number;
}

export interface ClipPageStreamerOptions {
  /**
   * Pages to prefetch ahead of the page a miss was reported for. Larger values
   * hide fetch latency at the cost of more resident memory. Default 2.
   */
  readAheadPages?: number;
  /**
   * Pages to retain behind the playback frontier before eviction, so a small
   * backward seek does not immediately miss. Default 1.
   */
  retainBehindPages?: number;
  /**
   * Upper bound on requests drained per {@link ClipPageStreamer.pump} call, so a
   * burst of misses cannot spin unbounded. Default 256.
   */
  maxRequestsPerPump?: number;
}

interface SourceState {
  source: ClipPageStreamSource;
  lastPage: number;
  /** Playback-window generation; advanced on reset or backward discontinuity. */
  generation: number;
  /** Most recently serviced playback frontier, or null before the first miss. */
  lastFrontier: number | null;
  /** Resident page -> generation, so in-flight stale fetches cannot alias. */
  resident: Map<number, number>;
}

/**
 * Keeps OPFS-paged clips fed within a bounded sliding window around the live
 * playback position, so a multitrack arrangement never holds its full PCM in
 * WASM memory.
 *
 * The audio thread reports a page miss whenever the {@link ClipPlayer} reads a
 * sample whose page is not resident. {@link pump} drains those requests, fetches
 * the missing page plus a read-ahead window from each clip's backing store, and
 * evicts pages that fall outside the window via the provider's `clear`. The
 * resident set per clip is therefore bounded to
 * `retainBehindPages + readAheadPages + 1` pages regardless of clip length.
 *
 * Call {@link pump} on a cadence that keeps up with playback — typically once
 * per animation frame or per worklet control tick on the main/control thread
 * (never the audio thread; fetches are asynchronous).
 */
export class ClipPageStreamer {
  private readonly engine: ClipPageStreamerEngine;
  private readonly readAheadPages: number;
  private readonly retainBehindPages: number;
  private readonly maxRequestsPerPump: number;
  private readonly sources = new Map<number, SourceState>();
  private closed = false;

  constructor(engine: ClipPageStreamerEngine, options: ClipPageStreamerOptions = {}) {
    this.engine = engine;
    this.readAheadPages = Math.max(0, Math.floor(options.readAheadPages ?? 2));
    this.retainBehindPages = Math.max(0, Math.floor(options.retainBehindPages ?? 1));
    this.maxRequestsPerPump = Math.max(1, Math.floor(options.maxRequestsPerPump ?? 256));
  }

  /**
   * Register a paged clip. Pages already supplied to the provider before
   * registration (for example a primed first page) should be passed in
   * `initialResidentPages` so they participate in eviction.
   */
  addSource(source: ClipPageStreamSource, initialResidentPages: Iterable<number> = []): void {
    if (source.pageFrames <= 0 || source.numSamples <= 0) {
      throw new Error('pageFrames and numSamples must be positive');
    }
    const lastPage = Math.ceil(source.numSamples / source.pageFrames) - 1;
    const previous = this.sources.get(source.clipId);
    if (previous) {
      this.resetState(previous);
    }
    this.sources.set(source.clipId, {
      source,
      lastPage,
      generation: 0,
      lastFrontier: null,
      resident: new Map(Array.from(initialResidentPages, (page) => [page, 0])),
    });
  }

  /** Stop tracking a clip. Does not close its binding (the caller owns that). */
  removeSource(clipId: number): void {
    const state = this.sources.get(clipId);
    if (state) {
      this.resetState(state);
    }
    this.sources.delete(clipId);
  }

  /**
   * Explicitly start a new playback generation after a host seek/loop. Resident
   * pages are evicted and any older in-flight fetch is cleared when it settles.
   * The next miss establishes the new bounded window.
   */
  resetSource(clipId: number): void {
    const state = this.sources.get(clipId);
    if (state) {
      this.resetState(state);
    }
  }

  /**
   * Drain pending page-miss requests, fetch the missing pages plus their
   * read-ahead window, and evict out-of-window pages. Resolves once this round's
   * fetches settle. Concurrent fetches are serialized inside each binding.
   */
  async pump(): Promise<void> {
    if (this.closed) {
      return;
    }
    // Collapse this round's misses to the latest observed frontier per clip.
    // Queue order reflects playback order, so a backward seek/loop request that
    // follows a stale high-page miss must win even though its page index is
    // smaller. Multiple channel misses still collapse to one service window.
    const frontiers = new Map<number, number>();
    for (let drained = 0; drained < this.maxRequestsPerPump; ++drained) {
      const request = this.engine.popClipPageRequest();
      if (!request) {
        break;
      }
      const state = this.sources.get(request.clipId);
      if (!state) {
        continue;
      }
      const page =
        request.pageIndex !== undefined
          ? request.pageIndex
          : Math.floor((request.sample ?? Number.NaN) / state.source.pageFrames);
      if (!Number.isInteger(page) || page < 0 || page > state.lastPage) {
        continue;
      }
      frontiers.set(request.clipId, page);
    }

    const fetches: Promise<unknown>[] = [];
    for (const [clipId, frontier] of frontiers) {
      const state = this.sources.get(clipId);
      if (!state) {
        continue;
      }
      fetches.push(...this.serviceFrontier(state, frontier));
    }
    await Promise.all(fetches);
  }

  /** Close every registered clip's binding and stop tracking. */
  close(): void {
    if (this.closed) {
      return;
    }
    this.closed = true;
    for (const state of this.sources.values()) {
      this.resetState(state);
      state.source.binding.close();
    }
    this.sources.clear();
  }

  private serviceFrontier(state: SourceState, frontier: number): Promise<unknown>[] {
    // A lower frontier after a previously serviced high page is a seek/loop
    // discontinuity. Advance generation before scheduling its new window so an
    // older asynchronous fetch can never become resident in the new window.
    if (state.lastFrontier !== null && frontier < state.lastFrontier) {
      this.resetState(state);
    }
    state.lastFrontier = frontier;
    const generation = state.generation;
    const low = Math.max(0, frontier - this.retainBehindPages);
    const high = Math.min(state.lastPage, frontier + this.readAheadPages);

    // Evict pages outside the window first so a burst of fetches never exceeds
    // the bound by transiently holding old pages alongside new ones.
    for (const page of state.resident.keys()) {
      if (page < low || page > high) {
        this.clearPage(state, page);
        state.resident.delete(page);
      }
    }

    const fetches: Promise<unknown>[] = [];
    for (let page = low; page <= high; ++page) {
      if (state.resident.get(page) === generation) {
        continue;
      }
      // Mark resident eagerly so the same page is not fetched twice across
      // overlapping windows; drop it again if the fetch reports a miss.
      state.resident.set(page, generation);
      const pageIndex = page;
      fetches.push(
        state.source.binding.supplyPage(pageIndex).then(
          (ok) => {
            if (state.generation !== generation) {
              // The fetch crossed a seek/reset boundary. supplyPage may have
              // installed it after resetState's clear pass, so clear it again.
              this.clearPage(state, pageIndex);
            } else if (!ok && state.resident.get(pageIndex) === generation) {
              state.resident.delete(pageIndex);
            }
            return ok;
          },
          (error) => {
            if (state.resident.get(pageIndex) === generation) {
              state.resident.delete(pageIndex);
            }
            throw error;
          },
        ),
      );
    }
    return fetches;
  }

  private resetState(state: SourceState): void {
    state.generation += 1;
    state.lastFrontier = null;
    for (const page of state.resident.keys()) {
      this.clearPage(state, page);
    }
    state.resident.clear();
  }

  private clearPage(state: SourceState, pageIndex: number): void {
    // `clearPage` mirrors eviction into the AudioWorklet. Keep the older
    // provider-only form working for direct-engine callers and test doubles.
    if (state.source.binding.clearPage) {
      state.source.binding.clearPage(pageIndex);
    } else {
      state.source.binding.provider.clear(pageIndex);
    }
  }
}

export interface OpfsClipStreamOptions extends OpfsClipPageProviderOptions {
  /** Clip schedule id used in `setClips` (matches the page-miss request clipId). */
  clipId: number;
  /**
   * Leading pages fetched synchronously before returning, so playback can start
   * without an immediate miss. Default 1.
   */
  primePages?: number;
}

export interface OpfsClipStream {
  binding: OpfsClipPageProviderBinding;
  /** Pass to `setClips({ pageProvider })` to schedule the streaming clip. */
  provider: ClipPageProvider;
}

/** Structural seam implemented by the AudioWorklet-backed `SonareEngine`. */
export interface WorkletOpfsClipStreamHost {
  attachOpfsClipStream(options: OpfsClipStreamOptions): Promise<OpfsClipStream>;
}

/**
 * One-call wiring of an OPFS-backed streaming clip: creates the page provider,
 * primes the leading pages, and registers it with `streamer` so later misses are
 * serviced within the bounded window. Returns the binding (for `close`) and the
 * provider to schedule via `setClips({ pageProvider })`.
 *
 * @param streamer Shared streamer pumped on the control thread.
 * @param engine Engine the provider is created on (the same one `streamer` drives).
 * @param options Provider options plus `clipId` and optional `primePages`.
 */
export function attachOpfsClipStream(
  streamer: ClipPageStreamer,
  engine: RealtimeEngine,
  options: OpfsClipStreamOptions,
): Promise<OpfsClipStream>;
/**
 * Worklet overload: delegates to `SonareEngine.attachOpfsClipStream`, which
 * creates the remote provider and drives this same sliding-window policy from
 * worklet page-miss messages.
 */
export function attachOpfsClipStream(
  engine: WorkletOpfsClipStreamHost,
  options: OpfsClipStreamOptions,
): Promise<OpfsClipStream>;
export async function attachOpfsClipStream(
  streamerOrEngine: ClipPageStreamer | WorkletOpfsClipStreamHost,
  engineOrOptions: RealtimeEngine | OpfsClipStreamOptions,
  maybeOptions?: OpfsClipStreamOptions,
): Promise<OpfsClipStream> {
  if (!(streamerOrEngine instanceof ClipPageStreamer)) {
    return streamerOrEngine.attachOpfsClipStream(engineOrOptions as OpfsClipStreamOptions);
  }
  const streamer = streamerOrEngine;
  const engine = engineOrOptions as RealtimeEngine;
  const options = maybeOptions;
  if (!options) {
    throw new Error('attachOpfsClipStream requires options.');
  }
  const { clipId, primePages = 1, ...providerOptions } = options;
  const binding = createOpfsClipPageProvider(engine, providerOptions);
  const lastPage = Math.ceil(providerOptions.numSamples / providerOptions.pageFrames) - 1;
  const primed: number[] = [];
  for (let page = 0; page < primePages && page <= lastPage; ++page) {
    if (await binding.supplyPage(page)) {
      primed.push(page);
    }
  }
  streamer.addSource(
    {
      clipId,
      binding,
      pageFrames: providerOptions.pageFrames,
      numSamples: providerOptions.numSamples,
    },
    primed,
  );
  return { binding, provider: binding.provider };
}
