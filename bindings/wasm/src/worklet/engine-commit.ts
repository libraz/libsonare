import { ErrorCode } from '../errors.js';
import type { RealtimeEngine } from '../index.js';
import { codedError, refuse } from '../internal/entry.js';
import { isValidCommandRecord } from './engine-node.js';
import type { SonareEngineCommandRecord } from './protocol.js';

/**
 * The one transaction every SonareEngine control edit goes through: validate,
 * accept, commit. The offline mirror and the facade caches are mutated only
 * from here, and only once the edit can no longer be refused, so a refused
 * edit leaves the mirror, the caches and the worklet exactly as they were.
 *
 * A command edit is refused (throws) before anything changes when the
 * live engine would not take the command — a record outside the wire domain,
 * or no room for it; the offline engine then validates and applies it, and the
 * command is queued last. A destroyed engine throws `InvalidState` instead. A
 * sync edit lets the offline engine validate first; the caches and the posted
 * message follow only if it accepted.
 */
export interface EngineCommitContext {
  readonly offlineEngine: RealtimeEngine;
  /**
   * Whether the live engine would accept `command` now; queues nothing.
   * @throws SonareError `InvalidState` once the engine is destroyed.
   */
  canSend(command: SonareEngineCommandRecord): boolean;
  /** Queues `command` live, then applies due commands on the offline mirror. */
  sendCommand(command: SonareEngineCommandRecord): boolean;
}

/**
 * Throws `ErrorCode.OutOfMemory` when a command that was well formed did not
 * fit the live command ring.
 */
export function requireQueued(accepted: boolean): void {
  if (!accepted) {
    throw codedError(ErrorCode.OutOfMemory, `the engine command queue is full`);
  }
}

/** The refusal for a command the live engine would not take: a bad value or a full ring. */
function refusedCommand(command: SonareEngineCommandRecord): Error {
  if (!isValidCommandRecord(command)) {
    return refuse(`a value is outside the range the engine accepts`);
  }
  return codedError(ErrorCode.OutOfMemory, `the engine command queue is full`);
}

/**
 * Applies `applyOffline` to the mirror and queues `command` live, or changes
 * nothing and throws: a `RangeError` for a value the live engine cannot carry,
 * `ErrorCode.OutOfMemory` for a full command ring.
 */
export function commitCommand(
  ctx: EngineCommitContext,
  command: SonareEngineCommandRecord,
  applyOffline: (offline: RealtimeEngine) => void,
  commitCache?: () => void,
): void {
  if (!ctx.canSend(command)) {
    throw refusedCommand(command);
  }
  applyOffline(ctx.offlineEngine);
  commitCache?.();
  // The main thread is the only producer, so what was checked above still holds.
  if (!ctx.sendCommand(command)) {
    throw refusedCommand(command);
  }
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
