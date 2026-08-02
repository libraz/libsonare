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
 * Resolves a target id from a string or number; non-numeric strings resolve to
 * 0, mirroring the engine's integer-id namespace.
 */
export function resolveTargetId(target: string | number): number {
  if (typeof target === 'number') {
    return target;
  }
  const parsed = Number.parseInt(target, 10);
  return Number.isFinite(parsed) ? parsed : 0;
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
