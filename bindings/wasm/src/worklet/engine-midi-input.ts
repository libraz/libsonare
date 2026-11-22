import { refuseType } from '../internal/entry.js';
import type { RequestFn } from '../request_keys.js';
import { assertNonNegativeScalar, requestObject, resolveRenderFrame } from '../validation.js';

/** Trailing options of every timed MIDI push on {@link SonareEngine}, live input and track-addressed alike. */
export interface SonareEngineMidiTimingOptions {
  /**
   * AudioContext time in seconds (the clock of `context.currentTime`) the event is due at.
   * The worklet maps it to the render frame it renders at that time, so the event lands on
   * its own sample whichever block is rendering when it arrives; a time that
   * has already passed plays at the start of the next block and counts towards
   * {@link SonareEngine.lateMidiEventCount}. Exclusive with the push's
   * `offsetInBlockSamples` or `renderFrame`.
   */
  contextTimeSec?: number;
}

/** The request shape name of each live MIDI input push. */
export type MidiInputPushName = Extract<RequestFn, `SonareEngine.pushMidiInput${string}`>;

/** The request shape name of each track-addressed MIDI push. */
export type MidiTrackPushName = Exclude<
  Extract<RequestFn, `SonareEngine.pushMidi${string}`>,
  MidiInputPushName
>;

/** The block offset and context time a live MIDI input push resolves to. */
export interface MidiInputTiming {
  offsetInBlockSamples: number;
  contextTimeSec?: number;
}

/** The render frame (`-1` for none) and context time a track-addressed MIDI push resolves to. */
export interface MidiTrackTiming {
  renderFrame: number;
  contextTimeSec?: number;
}

/** The context time of validated timing options, refused beside the push's own position `positionName`. */
function contextTimeOption(
  method: MidiInputPushName | MidiTrackPushName,
  options: SonareEngineMidiTimingOptions | null | undefined,
  positionName: string,
  positionGiven: boolean,
): number | undefined {
  if (options === undefined || options === null) {
    return undefined;
  }
  requestObject(method, options, 'options', true);
  const contextTimeSec = options.contextTimeSec;
  if (contextTimeSec === undefined || contextTimeSec === null) {
    return undefined;
  }
  if (positionGiven) {
    throw refuseType(`${positionName} and contextTimeSec must not both be given`);
  }
  assertNonNegativeScalar(contextTimeSec, 'contextTimeSec');
  return contextTimeSec;
}

/** Validates the offset and trailing options of a live MIDI input push; both at once is a `TypeError`. */
export function resolveMidiInputTiming(
  method: MidiInputPushName,
  offsetInBlockSamples: number | undefined,
  options: SonareEngineMidiTimingOptions | null | undefined,
): MidiInputTiming {
  const contextTimeSec = contextTimeOption(
    method,
    options,
    'offsetInBlockSamples',
    offsetInBlockSamples !== undefined,
  );
  return contextTimeSec === undefined
    ? { offsetInBlockSamples: offsetInBlockSamples ?? 0 }
    : { offsetInBlockSamples: 0, contextTimeSec };
}

/** Validates the render frame and trailing options of a track-addressed MIDI push; both at once is a `TypeError`. */
export function resolveMidiTrackTiming(
  method: MidiTrackPushName,
  renderFrame: number | null | undefined,
  options: SonareEngineMidiTimingOptions | null | undefined,
): MidiTrackTiming {
  const contextTimeSec = contextTimeOption(
    method,
    options,
    'renderFrame',
    renderFrame !== undefined && renderFrame !== null,
  );
  const frame = resolveRenderFrame(renderFrame);
  return contextTimeSec === undefined
    ? { renderFrame: frame }
    : { renderFrame: frame, contextTimeSec };
}
