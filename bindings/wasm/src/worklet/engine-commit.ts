import type { RealtimeEngine } from '../index.js';
import type { SonareEngineCommandRecord } from './protocol.js';

/**
 * The one transaction every SonareEngine control edit goes through: validate,
 * accept, commit. The offline mirror and the facade caches are mutated only
 * from here, and only once the edit can no longer be refused, so a refused
 * edit leaves the mirror, the caches and the worklet exactly as they were.
 *
 * A command edit is refused (returns false) before anything changes when the
 * live engine has no room for the command; the offline engine then validates
 * and applies it, and the command is queued last. A sync edit lets the offline
 * engine validate first; the caches and the posted message follow only if it
 * accepted.
 */
export interface EngineCommitContext {
  readonly offlineEngine: RealtimeEngine;
  /** Whether the live engine could queue one more command now; queues nothing. */
  hasCommandRoom(): boolean;
  /** Queues `command` live, then applies due commands on the offline mirror. */
  sendCommand(command: SonareEngineCommandRecord): boolean;
}

/** Applies `applyOffline` to the mirror and queues `command` live, or changes nothing. */
export function commitCommand(
  ctx: EngineCommitContext,
  command: SonareEngineCommandRecord,
  applyOffline: (offline: RealtimeEngine) => void,
  commitCache?: () => void,
): boolean {
  if (!ctx.hasCommandRoom()) {
    return false;
  }
  applyOffline(ctx.offlineEngine);
  commitCache?.();
  // The main thread is the only producer, so the room checked above is still there.
  return ctx.sendCommand(command);
}

/**
 * Applies `applyOffline` to the mirror; `publish` (cache writes, then the sync
 * message) runs only once the mirror accepted it. Without `publish` the caller
 * commits a staged draft itself after this returns.
 */
export function commitSync(
  ctx: Pick<EngineCommitContext, 'offlineEngine'>,
  applyOffline: (offline: RealtimeEngine) => void,
  publish?: () => void,
): void {
  applyOffline(ctx.offlineEngine);
  publish?.();
}

/**
 * Stage-then-swap for a keyed cache: `staged` is the proposed content, which
 * the mirror validates through `applyOffline`; `store` takes it only after
 * that, then `publish` posts it.
 */
export function commitStore<K, V>(
  ctx: Pick<EngineCommitContext, 'offlineEngine'>,
  store: Map<K, V>,
  staged: ReadonlyMap<K, V>,
  applyOffline: (offline: RealtimeEngine) => void,
  publish: () => void,
): void {
  applyOffline(ctx.offlineEngine);
  store.clear();
  for (const [key, value] of staged) {
    store.set(key, value);
  }
  publish();
}
