import { ErrorCode, SonareError } from './errors.js';

/**
 * Lifetime token of an object whose asynchronous work may resume after the
 * object was closed or replaced.
 *
 * A continuation captures a token before it awaits and calls
 * {@link assertCurrent} once it resumes, so work that outlived its owner stops
 * instead of registering, reviving or releasing anything for a successor.
 * {@link advance} hands ownership to a successor; {@link close} ends the epoch
 * and runs every cleanup registered with {@link onClose}, which is how pending
 * waiters settle and half-built resources are reclaimed.
 */
export class OwnerEpoch {
  private generation = 0;
  private ended = false;
  private readonly cleanups = new Set<() => void>();

  /** @param ownerName Names the owner in the errors a stale token raises. */
  constructor(private readonly ownerName: string) {}

  /** True once {@link close} has run. */
  get closed(): boolean {
    return this.ended;
  }

  /** Token of the current owner generation. */
  current(): number {
    return this.generation;
  }

  /** Starts a new generation, so every earlier token becomes stale. */
  advance(): number {
    this.generation += 1;
    return this.generation;
  }

  isCurrent(token: number): boolean {
    return !this.ended && token === this.generation;
  }

  /** Throws `InvalidState` unless `token` still names the live owner. */
  assertCurrent(token: number): void {
    if (this.ended) {
      throw new SonareError(ErrorCode.InvalidState, 'InvalidState', `${this.ownerName} is closed.`);
    }
    if (token !== this.generation) {
      throw new SonareError(
        ErrorCode.InvalidState,
        'InvalidState',
        `${this.ownerName} was replaced while an operation was pending.`,
      );
    }
  }

  /**
   * Registers `cleanup` to run when the epoch closes; runs it at once when the
   * epoch is already closed. Returns a function that unregisters it.
   */
  onClose(cleanup: () => void): () => void {
    if (this.ended) {
      cleanup();
      return () => undefined;
    }
    this.cleanups.add(cleanup);
    return () => {
      this.cleanups.delete(cleanup);
    };
  }

  /** Ends the epoch and runs every registered cleanup once. Idempotent. */
  close(): void {
    if (this.ended) {
      return;
    }
    this.ended = true;
    this.generation += 1;
    const cleanups = Array.from(this.cleanups);
    this.cleanups.clear();
    let failure: unknown;
    for (const cleanup of cleanups) {
      try {
        cleanup();
      } catch (error) {
        failure ??= error;
      }
    }
    if (failure !== undefined) {
      throw failure;
    }
  }
}
