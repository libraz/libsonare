import { refuseType } from '../internal/entry.js';
import type { RequestFn } from '../request_keys.js';
import { assertNonNegativeScalar, requestObject } from '../validation.js';

/** Trailing options of every live MIDI input push on {@link SonareEngine}. */
export interface SonareEngineMidiInputOptions {
  /**
   * AudioContext time in seconds (the clock of `context.currentTime`) the event is due at.
   * The worklet maps it to the render frame it renders at that time, so the event lands on
   * its own sample whichever block is rendering when it arrives; a time that
   * has already passed plays at the start of the next block and counts towards
   * {@link SonareEngine.lateMidiInputCount}. Exclusive with `offsetInBlockSamples`.
   */
  contextTimeSec?: number;
}

/** The request shape name of each live MIDI input push. */
export type MidiInputPushName = Extract<RequestFn, `SonareEngine.pushMidiInput${string}`>;

/** The block offset and context time a live MIDI input push resolves to. */
export interface MidiInputTiming {
  offsetInBlockSamples: number;
  contextTimeSec?: number;
}

/** Validates the offset and trailing options of a live MIDI input push; both at once is a `TypeError`. */
export function resolveMidiInputTiming(
  method: MidiInputPushName,
  offsetInBlockSamples: number | undefined,
  options: SonareEngineMidiInputOptions | null | undefined,
): MidiInputTiming {
  if (options === undefined || options === null) {
    return { offsetInBlockSamples: offsetInBlockSamples ?? 0 };
  }
  requestObject(method, options, 'options', true);
  const contextTimeSec = options.contextTimeSec;
  if (contextTimeSec === undefined || contextTimeSec === null) {
    return { offsetInBlockSamples: offsetInBlockSamples ?? 0 };
  }
  if (offsetInBlockSamples !== undefined) {
    throw refuseType('offsetInBlockSamples and contextTimeSec must not both be given');
  }
  assertNonNegativeScalar(contextTimeSec, 'contextTimeSec');
  return { offsetInBlockSamples: 0, contextTimeSec };
}
