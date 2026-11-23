// Converters between the realtime engine's public records and the shapes the embind layer reads and returns.

import { AUTOMATION_CURVE_VALUES, resolveEnumOrdinal } from '../codes.js';
import type {
  EngineClip,
  EngineMarker,
  EngineMeterTelemetry,
  EngineMidiClipSchedule,
  EngineParameterInfo,
  EngineTelemetry,
  EngineTelemetryError,
  EngineTelemetryType,
} from '../public_types_engine.js';
import type { WasmEngineClip, WasmEngineMeterTelemetry } from '../sonare.js';
import { nativeClipPageProviderOf } from './clip_page_native.js';
import { ordinalToken } from './core_tokens.js';
import { refuse } from './entry.js';
import { requireListBudget } from './list_budget.js';
import { measured } from './metering_native.js';
import { MARKER_KINDS, tokenName, WARP_MODES } from './project_tokens.js';
import { isAbsent } from './value_guards.js';

const TELEMETRY_TYPES: readonly EngineTelemetryType[] = ['processBlock', 'error'];
// Indexed by the native error ordinal; 19 is reserved by the worklet protocol.
const TELEMETRY_ERRORS: readonly EngineTelemetryError[] = [
  'none',
  'commandQueueOverflow',
  'pendingCommandOverflow',
  'boundaryOverflow',
  'telemetryOverflow',
  'captureOverflow',
  'maxBlockExceeded',
  'unknownTarget',
  'nonRealtimeSafeParameter',
  'notPrepared',
  'nonQueueableCommand',
  'automationBindTargetOverflow',
  'staleAutomationLanes',
  'smoothedParameterCapacity',
  'commandBacklogDeferred',
  'clipPageUnderrun',
  'insertAutomationOverflow',
  'midiClockOverflow',
  'metronomeOverflow',
  'invalidCommand',
  'maxChannelsExceeded',
  'parameterBaseOverflow',
];

/** The clip shape the embind layer reads: the warp mode travels as its ordinal, a page provider as its embind instance. */
export function nativeClip(clip: EngineClip, label: string): WasmEngineClip {
  const { warpMode, pageProvider, ...rest } = clip;
  return {
    ...rest,
    ...(warpMode === undefined
      ? {}
      : {
          warpMode: resolveEnumOrdinal(warpMode, WARP_MODES, `${label}.warpMode`) as 0 | 1 | 2 | 3,
        }),
    ...(pageProvider === undefined
      ? {}
      : { pageProvider: nativeClipPageProviderOf(pageProvider) as WasmEngineClip['pageProvider'] }),
  };
}

/** The schedule as the native layer reads it: an absent length is a missing key, never a `0`. */
export function midiClipsToNative(clips: ReadonlyArray<EngineMidiClipSchedule>): unknown {
  if (!Array.isArray(clips)) {
    return clips;
  }
  requireListBudget(clips, 'clips');
  return clips.map((clip) => {
    if (typeof clip !== 'object' || clip === null) {
      return clip;
    }
    const { lengthSamples, loopLengthSamples, ...rest } = clip;
    return {
      ...rest,
      ...(isAbsent(lengthSamples) ? {} : { lengthSamples }),
      ...(isAbsent(loopLengthSamples) ? {} : { loopLengthSamples }),
    };
  });
}

export function nativeMarker(marker: EngineMarker, i: number): Record<string, unknown> {
  const { kind, name, ...rest } = marker;
  if (name === '') {
    throw refuse(`markers[${i}].name must not be empty; omit it for an unnamed marker`);
  }
  const named = isAbsent(name) ? rest : { ...rest, name };
  return kind === undefined
    ? named
    : { ...named, kind: resolveEnumOrdinal(kind, MARKER_KINDS, `markers[${i}].kind`) };
}

export function markerFromNative(marker: {
  id: number;
  ppq: number;
  name?: string;
  kind?: number;
  keyFifths?: number;
  keyMinor?: boolean;
}): EngineMarker {
  const { name, ...rest } = marker;
  return {
    ...rest,
    ...(name === undefined || name === '' ? {} : { name }),
    kind: tokenName(MARKER_KINDS, marker.kind ?? 0, 'marker kind'),
  };
}

export function parameterInfoFromNative(info: {
  id: number;
  name: string;
  unit: string;
  minValue: number;
  maxValue: number;
  defaultValue: number;
  rtSafe: boolean;
  defaultCurve: number;
}): Required<EngineParameterInfo> {
  return {
    ...info,
    defaultCurve: tokenName(AUTOMATION_CURVE_VALUES, info.defaultCurve, 'automation curve'),
  };
}

export function telemetryFromNative(record: {
  type: number;
  error: number;
  renderFrame: number;
  timelineSample: number;
  audibleTimelineSample: number;
  graphLatencySamplesQ8: number;
  value: number;
}): EngineTelemetry {
  return {
    type: ordinalToken(TELEMETRY_TYPES, record.type, 'telemetry type'),
    error: ordinalToken(TELEMETRY_ERRORS, record.error, 'telemetry error'),
    renderFrame: record.renderFrame,
    timelineSample: record.timelineSample,
    audibleTimelineSample: record.audibleTimelineSample,
    graphLatencySamples: record.graphLatencySamplesQ8 / 256,
    value: record.value,
  };
}

export function meterTelemetryFromNative(record: WasmEngineMeterTelemetry): EngineMeterTelemetry {
  const { channelCount, truePeakDb, maxTruePeakDb, ...rest } = record;
  return {
    ...rest,
    numChannels: channelCount,
    peakDb: record.peakDb.slice(0, channelCount).map(measured),
    rmsDb: record.rmsDb.slice(0, channelCount).map(measured),
    truePeakDbtp: truePeakDb.slice(0, channelCount).map(measured),
    inputPeakDb: record.inputPeakDb.slice(0, channelCount).map(measured),
    ...(maxTruePeakDb !== undefined && { maxTruePeakDbtp: maxTruePeakDb }),
  };
}
