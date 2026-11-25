// Converters between the realtime engine's public records and the shapes the addon reads and returns.

import type {
  AutomationCurve,
  EngineBounceOptions,
  EngineBus,
  EngineClip,
  EngineGraphSpec,
  EngineMarker,
  EngineMeterTelemetry,
  EngineParameterInfo,
  EngineTelemetry,
  EngineTrackLane,
  EngineTrackSend,
} from '../types.js';
import { requestObject } from '../validation.js';
import {
  AUTOMATION_CURVE_VALUES,
  automationCurveValue,
  CHANNEL_LAYOUT_VALUES,
  SEND_TIMING_VALUES,
} from '../value_coercion.js';
import { nativeClipPageProviderOf } from './clip_page_native.js';
import { refuse, refuseType } from './entry.js';
import { requireListBudget } from './list_budget.js';
import { optionalCount, optionalId } from './optional_ints.js';
import { tokenName, tokenOrdinal } from './project_tokens.js';
import { isAbsent, isPlainRecord } from './value_guards.js';

/** What omitting an output bus id does, for its `0` refusal. */
export const MASTER_MIX_HINT = 'to route to the master mix';

/** A graph node name: a string, and never empty. */
function graphName(path: string, value: unknown): void {
  if (typeof value !== 'string') {
    throw refuseType(`${path} must be a string`);
  }
  if (value === '') {
    throw refuse(`${path} must not be empty`);
  }
}

const DITHER = { rpdf: 1, tpdf: 2, noiseShaped: 3 } as const;
const GRAPH_NODE_TYPE = { passThrough: 0, gain: 1 } as const;

const GRAPH_MIX = { replace: 0, add: 1 } as const;
const MARKER_KIND = { marker: 0, text: 1, lyric: 2, cuePoint: 3, keySignature: 4 } as const;
const WARP_MODE = { off: 0, repitch: 1, tempoSync: 2, timeStretch: 3 } as const;
const TELEMETRY_TYPE = { processBlock: 0, error: 1 } as const;
const TELEMETRY_ERROR = {
  none: 0,
  commandQueueOverflow: 1,
  pendingCommandOverflow: 2,
  boundaryOverflow: 3,
  telemetryOverflow: 4,
  captureOverflow: 5,
  maxBlockExceeded: 6,
  unknownTarget: 7,
  nonRealtimeSafeParameter: 8,
  notPrepared: 9,
  nonQueueableCommand: 10,
  automationBindTargetOverflow: 11,
  staleAutomationLanes: 12,
  smoothedParameterCapacity: 13,
  commandBacklogDeferred: 14,
  clipPageUnderrun: 15,
  insertAutomationOverflow: 16,
  midiClockOverflow: 17,
  metronomeOverflow: 18,
  invalidCommand: 19,
  maxChannelsExceeded: 20,
  parameterBaseOverflow: 21,
} as const;

/** Fixed-point scale of the latency the native engine reports (1/256 sample). */
export const LATENCY_Q8_SCALE = 256;

/** The addon reads a page provider through its native handle and a warp mode by ordinal. */
export function toNativeClip(clip: EngineClip, label: string): unknown {
  const native: Record<string, unknown> = {
    ...clip,
  };
  if (clip.pageProvider !== undefined) {
    native.pageProvider = nativeClipPageProviderOf(clip.pageProvider);
  }
  if (!isAbsent(clip.warpMode)) {
    native.warpMode = tokenOrdinal(clip.warpMode, WARP_MODE, `${label}.warpMode`);
  }
  return native;
}

/** Resolves each send's `sendTiming` spelling to its C ABI ordinal. */
export function normalizeSends(sends: EngineTrackSend[], where: string): EngineTrackSend[] {
  requireListBudget(sends, where);
  return sends.map((send, i) =>
    isAbsent(send.sendTiming)
      ? send
      : {
          ...send,
          sendTiming: tokenOrdinal(
            send.sendTiming,
            SEND_TIMING_VALUES,
            `${where}[${i}].sendTiming`,
          ) as never,
        },
  );
}

/** Output bus of a lane or bus: absent is the master mix, an explicit `0` is refused. */
function outputBusField(value: unknown, label: string): number | undefined {
  return optionalId(`${label}.outputBusId`, value, MASTER_MIX_HINT);
}

export function laneToNative(
  requestName: 'setTrackLanes',
  lane: EngineTrackLane,
  index: number,
): unknown {
  const label = `lanes[${index}]`;
  if (!isPlainRecord(lane as unknown)) {
    throw refuseType(`${label} must be an object`);
  }
  const record = requestObject(requestName, lane, 'lane', true);
  const native: Record<string, unknown> = { ...record };
  const outputBusId = outputBusField(record.outputBusId, label);
  if (outputBusId === undefined) {
    delete native.outputBusId;
  }
  if (!isAbsent(record.sourceChannelLayout)) {
    native.sourceChannelLayout = tokenOrdinal(
      record.sourceChannelLayout,
      CHANNEL_LAYOUT_VALUES,
      `${label}.sourceChannelLayout`,
    );
  }
  if (Array.isArray(record.sends)) {
    native.sends = normalizeSends(record.sends as EngineTrackSend[], `${label}.sends`);
  }
  return native;
}

export function busToNative(requestName: 'setTrackBuses', bus: EngineBus, index: number): unknown {
  const label = `buses[${index}]`;
  if (!isPlainRecord(bus as unknown)) {
    throw refuseType(`${label} must be an object`);
  }
  const record = requestObject(requestName, bus, 'bus', true);
  const native: Record<string, unknown> = { ...record };
  if (outputBusField(record.outputBusId, label) === undefined) {
    delete native.outputBusId;
  }
  if (!isAbsent(record.channelLayout)) {
    native.channelLayout = tokenOrdinal(
      record.channelLayout,
      CHANNEL_LAYOUT_VALUES,
      `${label}.channelLayout`,
    );
  }
  if (Array.isArray(record.sends)) {
    native.sends = normalizeSends(record.sends as EngineTrackSend[], `${label}.sends`);
  }
  return native;
}

function curveName(ordinal: number): AutomationCurve {
  return tokenName(AUTOMATION_CURVE_VALUES, ordinal, 'automation curve') as AutomationCurve;
}

export function parameterToNative(info: EngineParameterInfo): unknown {
  if (typeof info !== 'object' || info === null || isAbsent(info.defaultCurve)) {
    return info;
  }
  return { ...info, defaultCurve: automationCurveValue(info.defaultCurve, 'info.defaultCurve') };
}

export function parameterFromNative(
  native: Record<string, unknown>,
): Required<EngineParameterInfo> {
  return {
    ...native,
    defaultCurve: curveName(native.defaultCurve as number),
  } as Required<EngineParameterInfo>;
}

export function markerToNative(marker: EngineMarker, i: number): unknown {
  if (typeof marker !== 'object' || marker === null) {
    return marker;
  }
  const { name, ...rest } = marker;
  if (name === '') {
    throw refuse(`markers[${i}].name must not be empty; omit it for an unnamed marker`);
  }
  const named = isAbsent(name) ? rest : { ...rest, name };
  if (isAbsent(marker.kind)) {
    return named;
  }
  return { ...named, kind: tokenOrdinal(marker.kind, MARKER_KIND, `markers[${i}].kind`) };
}

/** An unnamed marker reads back without a `name` key, so the record can be passed to `setMarkers` again. */
export function markerFromNative(native: Record<string, unknown>): EngineMarker {
  const { name, ...rest } = native;
  return {
    ...rest,
    ...(name === '' ? {} : { name }),
    kind: tokenName(MARKER_KIND, native.kind as number, 'marker kind'),
  } as EngineMarker;
}

export function telemetryFromNative(native: Record<string, unknown>): EngineTelemetry {
  const { graphLatencySamplesQ8, type, error, ...rest } = native as {
    graphLatencySamplesQ8: number;
    type: number;
    error: number;
  } & Record<string, unknown>;
  return {
    ...rest,
    type: tokenName(TELEMETRY_TYPE, type, 'telemetry type'),
    error: tokenName(TELEMETRY_ERROR, error, 'telemetry error'),
    graphLatencySamples: graphLatencySamplesQ8 / LATENCY_Q8_SCALE,
  } as unknown as EngineTelemetry;
}

export function meterTelemetryFromNative(native: Record<string, unknown>): EngineMeterTelemetry {
  const {
    channelCount,
    peakDb: nativePeakDb,
    rmsDb: nativeRmsDb,
    truePeakDb: nativeTruePeakDb,
    inputPeakDb: nativeInputPeakDb,
    maxTruePeakDb,
    ...rest
  } = native;
  const numChannels = channelCount as number;
  const peakDb = meterLevels(nativePeakDb, numChannels);
  const rmsDb = meterLevels(nativeRmsDb, numChannels);
  const truePeakDbtp = meterLevels(nativeTruePeakDb, numChannels);
  const inputPeakDb = meterLevels(nativeInputPeakDb, numChannels);
  return {
    ...rest,
    numChannels,
    peakDb,
    rmsDb,
    truePeakDbtp,
    inputPeakDb,
    // An absent native max stays absent.
    ...(maxTruePeakDb !== undefined ? { maxTruePeakDbtp: maxTruePeakDb } : {}),
  } as unknown as EngineMeterTelemetry;
}

function meterLevels(values: unknown, numChannels: number): Array<number | undefined> {
  const levels = Array.isArray(values) ? values : [];
  return Array.from({ length: numChannels }, (_, index) => {
    const level = levels[index];
    return typeof level === 'number' && !Number.isNaN(level) ? level : undefined;
  });
}

export function graphToNative(spec: EngineGraphSpec): unknown {
  const record = requestObject('setGraph', spec, 'spec', true);
  const nodes = Array.isArray(record.nodes)
    ? (record.nodes as unknown as Array<Record<string, unknown>>)
    : record.nodes;
  const connections = Array.isArray(record.connections)
    ? (record.connections as unknown as Array<Record<string, unknown>>)
    : record.connections;
  graphName('inputNode', record.inputNode);
  graphName('outputNode', record.outputNode);
  const numChannels = optionalCount('numChannels', record.numChannels);
  const { numChannels: _numChannels, ...base } = record;
  return {
    ...base,
    ...(numChannels === undefined ? {} : { numChannels }),
    nodes: Array.isArray(nodes)
      ? nodes.map((node, i) => {
          if (typeof node !== 'object' || node === null) {
            return node;
          }
          graphName(`nodes[${i}].id`, node.id);
          const { type, ...rest } = node;
          return {
            ...rest,
            ...(isAbsent(type)
              ? {}
              : { type: tokenOrdinal(type, GRAPH_NODE_TYPE, `nodes[${i}].type`) }),
          };
        })
      : nodes,
    connections: Array.isArray(connections)
      ? connections.map((connection, i) => {
          if (typeof connection !== 'object' || connection === null) {
            return connection;
          }
          graphName(`connections[${i}].sourceNode`, connection.sourceNode);
          graphName(`connections[${i}].destNode`, connection.destNode);
          const { mix, ...rest } = connection;
          return {
            ...rest,
            ...(isAbsent(mix)
              ? {}
              : { mix: tokenOrdinal(mix, GRAPH_MIX, `connections[${i}].mix`) }),
          };
        })
      : connections,
  };
}

export function bounceToNative(options: EngineBounceOptions): Record<string, unknown> {
  const fnName = 'bounceOffline';
  const request = requestObject(fnName, options, 'options', true);
  const native: Record<string, unknown> = { ...request };
  if (!isAbsent(request.dither)) {
    native.dither = tokenOrdinal(request.dither, DITHER, 'dither');
  }
  return native;
}
