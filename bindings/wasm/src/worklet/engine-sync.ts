import type {
  EngineParameterInfo,
  EngineTempoSegment,
  EngineTimeSignatureSegment,
  EngineTrackLane,
  EngineTrackSend,
} from '../index';
import type { SonareEngineSyncTempoMessage } from './messages';

/**
 * Builds the ordered mixer-lane descriptors for a sync message.
 *
 * Each declared track id is paired with its current send list and output bus
 * (both omitted when absent), defensively copying the send entries.
 *
 * @param trackLaneIds Lane order (track ids) declared on the engine.
 * @param trackSends Per-track send lists.
 * @param trackOutputBus Per-track output bus routing.
 * @returns Lane descriptors in lane order.
 */
export function buildMixerLanes(
  trackLaneIds: readonly number[],
  trackSends: ReadonlyMap<number, EngineTrackSend[]>,
  trackOutputBus: ReadonlyMap<number, number>,
): EngineTrackLane[] {
  return trackLaneIds.map((trackId) => {
    const sends = trackSends.get(trackId);
    const outputBusId = trackOutputBus.get(trackId);
    return {
      trackId,
      ...(sends && sends.length > 0 ? { sends: sends.map((send) => ({ ...send })) } : {}),
      ...(outputBusId !== undefined ? { outputBusId } : {}),
    };
  });
}

/**
 * Builds the out-of-band tempo/time-signature sync message, deep-copying the
 * tempo and time-signature segments so the consumer cannot alias engine state.
 */
export function buildTempoSync(
  tempoBpm: number,
  timeSignature: { numerator: number; denominator: number },
  tempoSegments: readonly EngineTempoSegment[],
  timeSignatureSegments: readonly EngineTimeSignatureSegment[],
): SonareEngineSyncTempoMessage {
  return {
    type: 'syncTempo',
    bpm: tempoBpm,
    timeSignature: { ...timeSignature },
    tempoSegments: tempoSegments.map((segment) => ({ ...segment })),
    timeSignatureSegments: timeSignatureSegments.map((segment) => ({ ...segment })),
  };
}

/**
 * Resolves a target id given as a number, or as a string naming one.
 *
 * Both spellings reach one refusal. Reading the string with `Number.parseInt`
 * stops at the first non-digit, so `"3.5"` resolves to `3` and `"5abc"` to `5`,
 * and an unparseable name resolves to `0` -- all ids the caller never named.
 * `0` is a legal destination, so the native check these feed cannot tell the
 * substitution from a deliberate choice and never reports one.
 */
export function resolveTargetId(target: string | number): number {
  // An empty or blank string is not a spelling of zero, which is what Number
  // reads it as.
  const value =
    typeof target === 'number' ? target : target.trim() === '' ? Number.NaN : Number(target);
  if (!Number.isInteger(value)) {
    throw new RangeError(`target id must be an integer, got ${JSON.stringify(target)}`);
  }
  return value;
}

/** Resolves a registered parameter name or passes through a numeric id. */
export function resolveParamId(
  parameters: readonly EngineParameterInfo[],
  nodeId: string,
  param: string | number,
): number {
  if (typeof param === 'number') {
    return param;
  }
  const byName = parameters.find((info) => info.name === param);
  if (byName) {
    return byName.id;
  }
  throw new RangeError(`Unknown engine parameter ${JSON.stringify(param)} for node ${nodeId}`);
}

/** Encodes an automation curve descriptor to the engine's numeric curve code. */
export function curveCode(curve: number | 'linear' | 'exponential'): number {
  if (typeof curve === 'number') {
    return curve;
  }
  return curve === 'exponential' ? 1 : 0;
}
