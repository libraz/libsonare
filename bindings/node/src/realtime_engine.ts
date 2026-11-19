import type { ClipPageSampleRequest } from './clip_page_provider.js';
import { ErrorCode, SonareError } from './errors.js';
import { nativeClipPageProviderOf } from './internal/clip_page_native.js';
import { codedError, entryClass, invalidState, refuse, refuseType } from './internal/entry.js';
import { assignMarkerIds } from './internal/marker_ids.js';
import { assertMidiCcBindingBytes } from './internal/midi_cc_binding.js';
import { midiFxConfigJson } from './internal/midi_fx_config.js';
import { mixSceneBusToNative, mixSceneStripToNative } from './internal/mix_scene.js';
import { requireOneOf } from './internal/one_of.js';
import { deinterleave } from './internal/planar.js';
import { timelineHandle } from './internal/project_handles.js';
import { refuseBuiltinPolyphonyZero, refuseBuiltinSynthOrdinals } from './internal/synth_tokens.js';
import { addon } from './native.js';
import type { ProjectTimeline } from './project.js';
import type {
  Articulation,
  AutomationCurve,
  BuiltinSynthConfig,
  CaptureOptions,
  CapturePunch,
  ControllerBinding,
  EngineAutomationPoint,
  EngineBounceOptions,
  EngineBounceResult,
  EngineBus,
  EngineCaptureStatus,
  EngineClip,
  EngineFreezeOptions,
  EngineFreezeResult,
  EngineGraphSpec,
  EngineLoop,
  EngineMarker,
  EngineMeterTelemetry,
  EngineMeterTelemetryWide,
  EngineMetronomeConfig,
  EngineMidiClipSchedule,
  EngineParameterInfo,
  EngineProcessWithMonitorResult,
  EngineScopeTelemetry,
  EngineTelemetry,
  EngineTempoSegment,
  EngineTimeSignatureSegment,
  EngineTrackLane,
  EngineTrackMonitorMode,
  EngineTrackSend,
  EngineTransportState,
  EqBandInput,
  ExternalMidiEvent,
  MidiFxConfig,
  MixSceneBusInput,
  MixSceneStripInput,
  MpeDimension,
  NoteTracking,
  PanLaw,
  PanMode,
  PartRigInsert,
  PartRigMode,
  PartRigRequest,
  ProjectMidiCcBinding,
  RealtimeEngineConfig,
  RenderOfflineRequest,
  Sf2InstrumentConfig,
  SidechainCheck,
  SidechainRefusal,
  SidechainSource,
  StripRef,
  SurroundPan,
  SynthPatch,
  UmpWords,
} from './types.js';
import {
  assertClosedToken,
  assertFiniteSamples,
  assertNonNegativeSafeInteger,
  C_INT_MAX,
  requestObject,
  requireBoolean,
  resolveRenderFrame,
  resolveSampleBound,
} from './validation.js';
import {
  AUTOMATION_CURVE_VALUES,
  automationCurveValue,
  CHANNEL_LAYOUT_VALUES,
  normalizeSynthInstrument,
  panLawValue,
  panModeValue,
} from './value_coercion.js';

export * from './clip_page_provider.js';

const isAbsent = (value: unknown): value is undefined | null =>
  value === undefined || value === null;

function isObjectValue(value: unknown): value is object {
  return (
    typeof value === 'object' &&
    value !== null &&
    !Array.isArray(value) &&
    !ArrayBuffer.isView(value)
  );
}

function requireFinite(value: unknown, argName: string): number {
  if (typeof value !== 'number') {
    throw refuseType(`${argName} must be a number`);
  }
  if (!Number.isFinite(value)) {
    throw refuse(`${argName} must be finite`);
  }
  return value;
}

/** An integer-typed field: a non-number is a `TypeError`, and a non-finite number a `RangeError`. */
function requireFiniteInteger(value: unknown, argName: string): number {
  if (typeof value !== 'number') {
    throw refuseType(`${argName} must be an integer`);
  }
  if (!Number.isFinite(value)) {
    throw refuse(`${argName} must be finite`);
  }
  return value;
}

/** An optional integer on a field where `0` would mean "the default": absent stays absent, `0` is refused. */
function optionalNonZeroInteger(value: unknown, argName: string): number | undefined {
  if (isAbsent(value)) {
    return undefined;
  }
  const number = requireFiniteInteger(value, argName);
  if (number === 0) {
    throw refuse(`${argName} must not be 0; omit it for the default`);
  }
  return number;
}

/** An optional number on a field where `0` would mean "the default": absent stays absent, `0` is refused. */
function optionalNonZero(value: unknown, argName: string): number | undefined {
  if (isAbsent(value)) {
    return undefined;
  }
  const number = requireFinite(value, argName);
  if (number === 0) {
    throw refuse(`${argName} must not be 0; omit it for the default`);
  }
  return number;
}

/** An optional uint32 id whose `0` would mean "none": absent stays `undefined`, and an explicit `0` is refused. */
function optionalId(argName: string, value: unknown): number | undefined {
  if (isAbsent(value)) {
    return undefined;
  }
  const number = requireFiniteInteger(value, argName);
  if (number === 0) {
    throw refuse(`${argName} must not be 0; omit it for the default`);
  }
  if (!Number.isInteger(number) || number < 1 || number > 0xffffffff) {
    throw refuse(`${argName} must be an integer in [1, 4294967295]`);
  }
  return number;
}

/** An optional positive integer count: absent stays `undefined`, and an explicit `0` is refused. */
function optionalCount(argName: string, value: unknown): number | undefined {
  if (isAbsent(value)) {
    return undefined;
  }
  const number = requireFiniteInteger(value, argName);
  if (number === 0) {
    throw refuse(`${argName} must not be 0; omit it for the default`);
  }
  if (number < 0 || number > C_INT_MAX || !Number.isInteger(number)) {
    throw refuse(`${argName} must be an integer in [1, ${C_INT_MAX}]`);
  }
  return number;
}

/** A graph node name: a string, and never empty. */
function graphName(path: string, value: unknown): void {
  if (typeof value !== 'string') {
    throw refuseType(`${path} must be a string`);
  }
  if (value === '') {
    throw refuse(`${path} must not be empty`);
  }
}

function requireToken<T extends string>(
  value: unknown,
  table: Readonly<Record<T, number>>,
  argName: string,
): number {
  assertClosedToken(value, Object.keys(table) as T[], argName);
  return table[value];
}

function tokenOf<T extends string>(
  table: Readonly<Record<T, number>>,
  ordinal: number,
  what: string,
): T {
  const name = (Object.keys(table) as T[]).find((key) => table[key] === ordinal);
  if (name === undefined) {
    throw codedError(ErrorCode.Unknown, `unknown ${what} ordinal ${ordinal} from the engine`);
  }
  return name;
}

const SEND_TIMING = { postFader: 0, preFader: 1 } as const;
const SIDECHAIN_SOURCE_KIND = { track: 0, bus: 1 } as const;
const TRACK_MONITOR_MODE = { off: 0, pfl: 1, afl: 2 } as const;
const DITHER = { rpdf: 1, tpdf: 2, noiseShaped: 3 } as const;
const GRAPH_NODE_TYPE = { passThrough: 0, gain: 1 } as const;
/** The C ABI tags clock and transport bytes with this destination; the facade omits the field. */
const EVERY_PORT_DESTINATION = 0xffffffff;
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

/** The C ABI value that selects every part of a part rig. */
const PART_RIG_EVERY_PART = 0xff;
/** Fixed-point scale of the latency the native engine reports (1/256 sample). */
const LATENCY_Q8_SCALE = 256;

/** An omitted size stays undefined, which the addon passes on as the library default. */
interface ResolvedEngineConfig {
  sampleRate: number;
  maxBlockSize: number | undefined;
  commandCapacity: number | undefined;
  telemetryCapacity: number | undefined;
  maxChannels: number | undefined;
}

function resolveEngineConfig(
  requestName: 'RealtimeEngine' | 'prepare',
  config: RealtimeEngineConfig,
): ResolvedEngineConfig {
  const request = requestObject(requestName, config, 'config', true);
  if (isAbsent(request.sampleRate)) {
    throw refuseType(`sampleRate must be a number`);
  }
  const sampleRate = requireFinite(request.sampleRate, 'sampleRate');
  return {
    sampleRate,
    maxBlockSize: optionalNonZeroInteger(request.maxBlockSize, 'maxBlockSize'),
    commandCapacity: optionalNonZeroInteger(request.commandCapacity, 'commandCapacity'),
    telemetryCapacity: optionalNonZeroInteger(request.telemetryCapacity, 'telemetryCapacity'),
    maxChannels: optionalNonZeroInteger(request.maxChannels, 'maxChannels'),
  };
}

/** The addon reads a page provider through its native handle and a warp mode by ordinal. */
function toNativeClip(clip: EngineClip, label: string): unknown {
  const native: Record<string, unknown> = {
    ...clip,
  };
  if (clip.pageProvider !== undefined) {
    native.pageProvider = nativeClipPageProviderOf(clip.pageProvider);
  }
  if (!isAbsent(clip.warpMode)) {
    native.warpMode = requireToken(clip.warpMode, WARP_MODE, `${label}.warpMode`);
  }
  return native;
}

/** Resolves each send's `sendTiming` spelling to its C ABI ordinal. */
function normalizeSends(sends: EngineTrackSend[], where: string): EngineTrackSend[] {
  return sends.map((send, i) =>
    isAbsent(send.sendTiming)
      ? send
      : {
          ...send,
          sendTiming: requireToken(
            send.sendTiming,
            SEND_TIMING,
            `${where}[${i}].sendTiming`,
          ) as never,
        },
  );
}

/** Output bus of a lane or bus: absent is the master mix, an explicit `0` is refused. */
function outputBusField(value: unknown, label: string): number | undefined {
  return optionalNonZeroInteger(value, `${label}.outputBusId`);
}

function laneToNative(requestName: 'setTrackLanes', lane: EngineTrackLane, index: number): unknown {
  const label = `lanes[${index}]`;
  const record = requestObject(requestName, lane, 'lane', true);
  const native: Record<string, unknown> = { ...record };
  const outputBusId = outputBusField(record.outputBusId, label);
  if (outputBusId === undefined) {
    delete native.outputBusId;
  }
  if (!isAbsent(record.sourceChannelLayout)) {
    native.sourceChannelLayout = requireToken(
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

function busToNative(requestName: 'setTrackBuses', bus: EngineBus, index: number): unknown {
  const label = `buses[${index}]`;
  const record = requestObject(requestName, bus, 'bus', true);
  const native: Record<string, unknown> = { ...record };
  if (outputBusField(record.outputBusId, label) === undefined) {
    delete native.outputBusId;
  }
  if (!isAbsent(record.channelLayout)) {
    native.channelLayout = requireToken(
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

const SIDECHAIN_REFUSALS: readonly SidechainRefusal[] = [
  'invalidTarget',
  'insertOutOfRange',
  'undeclaredSource',
  'invalidSourceKind',
  'selfKey',
  'cycle',
  'tableFull',
  'planRefused',
];

/** Maps the C ABI `SonareSidechainRefusal` value (0 = accepted) to the facade shape. */
function sidechainCheck(code: number): SidechainCheck {
  if (code === 0) {
    return { ok: true };
  }
  const reason = SIDECHAIN_REFUSALS[code - 1];
  if (reason === undefined) {
    throw refuse(`Unknown sidechain refusal: ${code}`);
  }
  return { ok: false, reason };
}

/** A sidechain source resolved to the native `(kind, id)` pair; `id` 0 is the native "no binding". */
function sidechainSourcePair(
  requestName:
    | 'setBusSidechain'
    | 'setStripSidechain'
    | 'canSetBusSidechain'
    | 'canSetStripSidechain',
  source: SidechainSource | null | undefined,
  allowNull: boolean,
): { kind: number; id: number } {
  if (isAbsent(source)) {
    if (!allowNull) {
      throw refuseType(`source must be an object`);
    }
    return { kind: SIDECHAIN_SOURCE_KIND.track, id: 0 };
  }
  const record = requestObject(requestName, source, 'source', true);
  const kind = requireToken(record.kind, SIDECHAIN_SOURCE_KIND, 'source.kind');
  const id = requireFiniteInteger(record.id, 'source.id');
  if (id === 0) {
    throw refuse(`source.id must not be 0 (pass null to remove the binding)`);
  }
  return { kind, id };
}

/** A strip target: a track id, or the master strip. */
type StripTarget = { master: true } | { master: false; trackId: number };

function stripTarget(ref: StripRef): StripTarget {
  if (typeof ref === 'number') {
    return { master: false, trackId: ref };
  }
  if (ref === 'master') {
    return { master: true };
  }
  if (typeof ref === 'string') {
    throw refuse(`ref must be a track id or 'master'`);
  }
  throw refuseType(`ref must be a track id or 'master'`);
}

/** A strip target for a setter whose native layer has no master form. */
function trackOnly(ref: StripRef): number {
  const target = stripTarget(ref);
  if (target.master) {
    throw refuse('the master strip has no such setter');
  }
  return target.trackId;
}

function eqBandJsonOf(band: EqBandInput): string {
  if (typeof band !== 'object' || band === null || Array.isArray(band)) {
    throw refuseType(`band must be an object`);
  }
  return JSON.stringify(band);
}

function sceneStripJson(strip: MixSceneStripInput): string {
  return JSON.stringify({ strips: [mixSceneStripToNative(strip)] });
}

function sceneBusJson(bus: MixSceneBusInput): string {
  return JSON.stringify({ buses: [mixSceneBusToNative(bus)] });
}

function bypassState(
  requestName: 'setStripInsertBypassed' | 'setBusInsertBypassed',
  state: { bypassed: boolean; resetOnBypass?: boolean },
): { bypassed: boolean; resetOnBypass: boolean } {
  const record = requestObject(requestName, state, 'state', true);
  return {
    bypassed: requireBoolean(record.bypassed, 'bypassed'),
    resetOnBypass: isAbsent(record.resetOnBypass)
      ? false
      : requireBoolean(record.resetOnBypass, 'resetOnBypass'),
  };
}

function insertParam(
  requestName:
    | 'setStripInsertParam'
    | 'setBusInsertParam'
    | 'applyStripInsertParamNow'
    | 'applyBusInsertParamNow'
    | 'restoreStripInsertParam'
    | 'restoreBusInsertParam',
  param: { name: string; value: number },
): { name: string; value: number } {
  const record = requestObject(requestName, param, 'param', true);
  if (typeof record.name !== 'string') {
    throw refuseType(`param.name must be a string`);
  }
  return { name: record.name, value: requireFinite(record.value, 'param.value') };
}

function panLawOf(panLaw: PanLaw): number {
  if (typeof panLaw !== 'string') {
    throw refuseType(`panLaw must be a string`);
  }
  return panLawValue(panLaw, 'panLaw');
}

function panModeOf(panMode: PanMode): number {
  if (typeof panMode !== 'string') {
    throw refuseType(`panMode must be a string`);
  }
  return panModeValue(panMode, 'panMode');
}

function curveName(ordinal: number): AutomationCurve {
  return tokenOf(AUTOMATION_CURVE_VALUES, ordinal, 'automation curve') as AutomationCurve;
}

function automationPointToNative(point: EngineAutomationPoint, i: number): EngineAutomationPoint {
  const curve = isAbsent(point.curveToNext)
    ? AUTOMATION_CURVE_VALUES.linear
    : automationCurveValue(point.curveToNext, `points[${i}].curveToNext`);
  return { ...point, curveToNext: curve as never };
}

function parameterToNative(info: EngineParameterInfo): unknown {
  if (typeof info !== 'object' || info === null || isAbsent(info.defaultCurve)) {
    return info;
  }
  return { ...info, defaultCurve: automationCurveValue(info.defaultCurve, 'info.defaultCurve') };
}

function parameterFromNative(native: Record<string, unknown>): Required<EngineParameterInfo> {
  return {
    ...native,
    defaultCurve: curveName(native.defaultCurve as number),
  } as Required<EngineParameterInfo>;
}

/**
 * A sample count on a MIDI clip schedule whose `0` would read as "not given": absent stays
 * absent, and `0`, a negative or a fraction is refused by name.
 */
function scheduleSampleCount(
  index: number,
  field: string,
  value: unknown,
  meaning: string,
): number | undefined {
  if (value === undefined || value === null) {
    return undefined;
  }
  const where = `clips[${index}].${field}`;
  if (typeof value !== 'number') {
    throw refuseType(`${where} must be an integer`);
  }
  if (value === 0) {
    throw refuse(`${where} must not be 0; omit it for ${meaning}`);
  }
  if (!Number.isSafeInteger(value) || value < 0) {
    throw refuse(`${where} must be an integer in [1, ${Number.MAX_SAFE_INTEGER}]`);
  }
  return value;
}

/** The schedule as the native layer reads it: an absent length is a missing key, never a `0`. */
function midiClipsToNative(clips: ReadonlyArray<EngineMidiClipSchedule>): unknown {
  if (!Array.isArray(clips)) {
    return clips;
  }
  return clips.map((clip, index) => {
    if (typeof clip !== 'object' || clip === null) {
      return clip;
    }
    const { lengthSamples, loopLengthSamples, ...rest } = clip;
    const length = scheduleSampleCount(index, 'lengthSamples', lengthSamples, 'open-ended');
    const loopLength = scheduleSampleCount(
      index,
      'loopLengthSamples',
      loopLengthSamples,
      'no loop length',
    );
    return {
      ...rest,
      ...(length === undefined ? {} : { lengthSamples: length }),
      ...(loopLength === undefined ? {} : { loopLengthSamples: loopLength }),
    };
  });
}

function markerToNative(marker: EngineMarker, i: number): unknown {
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
  return { ...named, kind: requireToken(marker.kind, MARKER_KIND, `markers[${i}].kind`) };
}

/** An unnamed marker reads back without a `name` key, so the record can be passed to `setMarkers` again. */
function markerFromNative(native: Record<string, unknown>): EngineMarker {
  const { name, ...rest } = native;
  return {
    ...rest,
    ...(name === '' ? {} : { name }),
    kind: tokenOf(MARKER_KIND, native.kind as number, 'marker kind'),
  } as EngineMarker;
}

function telemetryFromNative(native: Record<string, unknown>): EngineTelemetry {
  const { graphLatencySamplesQ8, type, error, ...rest } = native as {
    graphLatencySamplesQ8: number;
    type: number;
    error: number;
  } & Record<string, unknown>;
  return {
    ...rest,
    type: tokenOf(TELEMETRY_TYPE, type, 'telemetry type'),
    error: tokenOf(TELEMETRY_ERROR, error, 'telemetry error'),
    graphLatencySamples: graphLatencySamplesQ8 / LATENCY_Q8_SCALE,
  } as unknown as EngineTelemetry;
}

function meterTelemetryFromNative(native: Record<string, unknown>): EngineMeterTelemetry {
  const { truePeakDbL, truePeakDbR, maxTruePeakDb, ...rest } = native;
  return {
    ...rest,
    truePeakDbtpL: truePeakDbL,
    truePeakDbtpR: truePeakDbR,
    maxTruePeakDbtp: maxTruePeakDb,
  } as unknown as EngineMeterTelemetry;
}

function meterTelemetryWideFromNative(native: Record<string, unknown>): EngineMeterTelemetryWide {
  const { channelCount, truePeakDb, maxTruePeakDb, ...rest } = native;
  return {
    ...rest,
    numChannels: channelCount,
    truePeakDbtp: truePeakDb,
    maxTruePeakDbtp: maxTruePeakDb,
  } as unknown as EngineMeterTelemetryWide;
}

function graphToNative(spec: EngineGraphSpec): unknown {
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
          const { type, numPorts, ...rest } = node;
          const ports = optionalCount(`nodes[${i}].numPorts`, numPorts);
          return {
            ...rest,
            ...(isAbsent(type)
              ? {}
              : { type: requireToken(type, GRAPH_NODE_TYPE, `nodes[${i}].type`) }),
            ...(ports === undefined ? {} : { numPorts: ports }),
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
              : { mix: requireToken(mix, GRAPH_MIX, `connections[${i}].mix`) }),
          };
        })
      : connections,
  };
}

function bounceToNative(options: EngineBounceOptions): Record<string, unknown> {
  const fnName = 'bounceOffline';
  const request = requestObject(fnName, options, 'options', true);
  const native: Record<string, unknown> = { ...request };
  for (const key of ['blockSize', 'targetSampleRate', 'sourceSampleRate'] as const) {
    if (optionalNonZeroInteger(request[key], key) === undefined) {
      delete native[key];
    }
  }
  if (isAbsent(request.targetLufs)) {
    delete native.targetLufs;
  } else {
    const lufs = requireFinite(request.targetLufs, 'targetLufs');
    if (lufs === 0) {
      throw refuse(`targetLufs must not be 0; omit it for -14 LUFS`);
    }
  }
  if (!isAbsent(request.dither)) {
    native.dither = requireToken(request.dither, DITHER, 'dither');
  }
  return native;
}

const CAPTURE_SOURCES = ['output', 'input'] as const;

function partRigInsertsJson(inserts: PartRigInsert[] | undefined): string | undefined {
  if (isAbsent(inserts)) {
    return undefined;
  }
  if (!Array.isArray(inserts)) {
    throw refuseType('inserts must be an array');
  }
  return JSON.stringify(
    inserts.map((insert, index) => {
      if (typeof insert !== 'object' || insert === null) {
        throw refuseType(`inserts[${index}] must be an object`);
      }
      if (typeof insert.processor !== 'string') {
        throw refuseType(`inserts[${index}].processor must be a string`);
      }
      const { params } = insert;
      if (params === undefined) {
        return { processor: insert.processor, params: {} };
      }
      if (typeof params !== 'object' || params === null || Array.isArray(params)) {
        throw refuseType(`inserts[${index}].params must be an object`);
      }
      return { processor: insert.processor, params };
    }),
  );
}

/** A config object that must not repeat the destination id the call already names. */
function withoutDestinationId<T extends object>(config: T): T {
  if (
    typeof config === 'object' &&
    config !== null &&
    (config as { destinationId?: unknown }).destinationId !== undefined
  ) {
    throw refuseType(`destinationId is the first argument; remove it from the config`);
  }
  return config;
}

/** An optional bank selection as the native `(bankValid, msb, lsb)` triple; absence is "no bank". */
function bankFields(
  requestName: 'pushMidiProgram' | 'pushMidiInputProgram',
  bank: { msb: number; lsb: number } | null | undefined,
): { bankValid: boolean; msb: number; lsb: number } {
  if (isAbsent(bank)) {
    return { bankValid: false, msb: 0, lsb: 0 };
  }
  const record = requestObject(requestName, bank, 'bank', true);
  return {
    bankValid: true,
    msb: requireFiniteInteger(record.msb, 'bank.msb'),
    lsb: requireFiniteInteger(record.lsb, 'bank.lsb'),
  };
}

/** A name read back from the addon; a number is an ordinal this binding has no name for. */
function nameFromNative(value: unknown): string {
  if (typeof value !== 'string') {
    throw codedError(ErrorCode.Unknown, `the engine returned an unnamed value ${String(value)}`);
  }
  return value;
}

/** Refuses a numeric ordinal where a closed-set name is the only spelling. */
function requireName(value: unknown, argName: string): string {
  if (typeof value !== 'string') {
    throw refuseType(`${argName} must be a name`);
  }
  return value;
}

/**
 * Realtime audio engine: transport, tempo map, clips, MIDI instruments, mixer
 * strips and buses, capture and offline rendering over one native engine.
 *
 * Strips are addressed by a {@link StripRef}: a number is a track id and
 * `'master'` is the master strip; any other string, a digit-only one included,
 * is a `RangeError`. Buses are addressed by `busId`. A refused argument is a
 * `TypeError` / `RangeError`; every other failure is a coded {@link SonareError}.
 * A full command queue throws `SonareError` with `OutOfMemory`.
 */
@entryClass('RealtimeEngine', [
  'process',
  'processWithMonitor',
  'assertTrackSource',
  'resolvePunch',
  'laneIndexOf',
])
export class RealtimeEngine {
  private native: InstanceType<typeof addon.RealtimeEngine>;
  private disposed = false;
  /** Lowest id `setMarkers` may assign; never moves back, so an id is not reused. */
  private nextMarkerId = 1;
  private sampleRate: number;
  /** Track ids in lane order as last declared; `'timeline'` once a timeline installed the lanes. */
  private laneOrder: number[] | 'timeline' = [];
  /** Shape of the capture buffer last allocated by {@link configureCapture}. */
  private captureShape: { numChannels: number; capacityFrames: number } | undefined;

  /**
   * @param config - Sample rate (required) and queue sizing; see {@link RealtimeEngineConfig}
   * @throws TypeError for a missing `sampleRate` or an unknown key; RangeError for an out-of-range value
   */
  constructor(config: RealtimeEngineConfig) {
    const resolved = resolveEngineConfig('RealtimeEngine', config);
    this.sampleRate = resolved.sampleRate;
    this.native = new addon.RealtimeEngine(
      resolved.sampleRate,
      resolved.maxBlockSize,
      resolved.commandCapacity,
      resolved.telemetryCapacity,
      resolved.maxChannels,
    );
  }

  /**
   * Size the engine's queues and scratch for a sample rate and block size. Takes
   * the object the constructor takes, with the same defaults and validation.
   *
   * `commandCapacity` must not exceed 65536 and `telemetryCapacity` must not
   * exceed 16384; a larger value throws and leaves the engine untouched. The
   * telemetry number is not a queue depth paid for one-for-one: the engine
   * reserves that many meter records per metered lane, so its memory cost is
   * far larger than the number given here.
   */
  prepare(config: RealtimeEngineConfig): void {
    const resolved = resolveEngineConfig('prepare', config);
    this.native.prepare(
      resolved.sampleRate,
      resolved.maxBlockSize,
      resolved.commandCapacity,
      resolved.telemetryCapacity,
      resolved.maxChannels,
    );
    this.sampleRate = resolved.sampleRate;
  }

  /** Starts transport playback, at `renderFrame` when given. */
  play(renderFrame?: number): void {
    this.native.play(resolveRenderFrame(renderFrame));
  }

  /**
   * Stops transport playback, at `renderFrame` when given.
   *
   * A loop wrap, seek or stop sends note-offs plus CC64=0, CC121, CC123 and a
   * centred pitch bend on every channel played since the last reset. When
   * playback next rolls, the controller, program, channel pressure and pitch
   * bend values the clips set before the new position are sent again, so each
   * loop pass and each seek starts from the state continuous playback reaches.
   */
  stop(renderFrame?: number): void {
    this.native.stop(resolveRenderFrame(renderFrame));
  }

  /** Moves the transport to `timelineSample`, a position in timeline samples. */
  seekSample(timelineSample: number, renderFrame?: number): void {
    this.native.seekSample(timelineSample, resolveRenderFrame(renderFrame));
  }

  /**
   * Snaps every in-flight parameter ramp (engine-level smoothed params, mixer
   * lane fader/pan/gate, bus gains) to its target value. It is one step of
   * {@link primeOfflineParameters}; called alone it only snaps the smoothers
   * and does not touch automation or processing state. Control-thread only:
   * must not be called concurrently with {@link process}.
   */
  settleParameters(): void {
    this.native.settleParameters();
  }

  /**
   * Runs the offline pre-roll that bounce and freeze run before rendering,
   * without rendering anything: applies every queued command, resets the mixer
   * and effect processors to their prepared state, adopts the published
   * snapshots, resolves automation and lane gates at the transport position
   * and snaps every smoother. A host that drives the engine block by block
   * calls it once before the first chunk so a repeat render starts from the
   * same state. Instruments are not reset. Control-thread only: must not be
   * called concurrently with {@link process}.
   *
   * @param numChannels - Channel count of the blocks that will be rendered
   * @param blockSize - Frames per block that will be rendered (a larger block size is refused);
   *   omit it for the engine's default block, capped at the prepared maximum
   * @throws If a count is not positive, `numChannels` exceeds the prepared
   *   channel count, or the engine was never prepared
   */
  primeOfflineParameters(numChannels: number, blockSize?: number): void {
    this.native.primeOfflineParameters(numChannels, optionalNonZeroInteger(blockSize, 'blockSize'));
  }

  /**
   * Queues a reset of every mixer and effect processor to its prepared state.
   * At `renderFrame` (omitted: the next block head) the lane, bus, master,
   * monitor and graph processors drop their tails, delay lines and envelopes,
   * automation is applied at the transport position and every smoother is
   * snapped to its target, so playback queued after it starts from the state an
   * offline bounce starts from. Strips the host bound to the engine, including
   * host-owned ones, are reset too. Instruments are not reset. Called during
   * playback it cuts running insert tails and delay lines mid-sound, like a
   * seek.
   *
   * @param renderFrame - Block-relative frame to apply at; omit for the next block head (negative is refused)
   * @throws If the command queue is full
   */
  resetProcessorState(renderFrame?: number): void {
    this.native.resetProcessorState(resolveRenderFrame(renderFrame));
  }

  /**
   * Longest audible tail after the last input, in samples: an upper bound made
   * of the longest instrument tail plus the longest route through lane strips
   * (channel delay included), buses, sends, the graph and the master strip.
   * An instrument lane whose patch is not known yet (a drumKit lane or a General
   * MIDI program lane before any note sounds) contributes a conservative
   * worst-case bound that narrows once a patch has sounded.
   * `2147483647` means the tail is unbounded. Control-thread only: must not be
   * read concurrently with {@link process}.
   */
  get tailSamples(): number {
    return this.native.tailSamples();
  }

  /** Processing latency of the engine in samples (fractional). */
  get graphLatencySamples(): number {
    return this.native.graphLatencySamplesQ8() / LATENCY_Q8_SCALE;
  }

  /** Largest block a process call accepts, as the native side resolved it (the library default when `maxBlockSize` was omitted); 0 before the engine is prepared. */
  get maxBlockSize(): number {
    return this.native.maxBlockSize();
  }

  /**
   * Applies commands queued on an offline/control-only engine immediately.
   * A host that never calls {@link process} still queues onto a bounded
   * realtime command ring, so those commands have to be drained explicitly
   * rather than by the next process() call. Not safe to call concurrently with
   * a running process().
   */
  flushControlCommands(): void {
    this.native.flushControlCommands();
  }

  /**
   * Snaps only the in-flight strip insert parameter ramps (track, bus and
   * master inserts) to their targets, leaving fader/pan ramps untouched. For
   * use after a structural replay (strip replacement followed by restored
   * insert values).
   */
  settleInsertParameters(): void {
    this.native.settleInsertParameters();
  }

  /**
   * Applies the queued commands already due at the current render frame,
   * keeping future-dated commands queued in their original order.
   * Control-only mirror counterpart of {@link flushControlCommands}, which
   * drains future commands too.
   */
  applyCommandsDueNowPreservingFuture(): void {
    this.native.applyCommandsDueNowPreservingFuture();
  }

  /** Moves the transport to `ppq`, a position in quarter notes. */
  seekPpq(ppq: number, renderFrame?: number): void {
    this.native.seekPpq(ppq, resolveRenderFrame(renderFrame));
  }

  /** Set a finite tempo in the range (0, 100000] BPM. */
  setTempo(bpm: number): void {
    this.native.setTempo(bpm);
  }

  /**
   * Sets the time signature. `numerator` and `denominator` are positive integers (a `RangeError` otherwise).
   */
  setTimeSignature(numerator: number, denominator: number): void {
    this.native.setTimeSignature(numerator, denominator);
  }

  /**
   * Installs a tempo map from ramp segments. Each segment needs a finite
   * non-negative `startPpq` and a `bpm` in (0, 100000]; a non-zero `endBpm`
   * uses the same range. An empty array clears the map to the single tempo.
   */
  setTempoSegments(segments: readonly EngineTempoSegment[]): void {
    this.native.setTempoSegments(segments);
  }

  /**
   * Installs a time-signature map. Each segment needs a finite non-negative
   * `startPpq` and a positive `numerator` / `denominator`.
   */
  setTimeSignatureSegments(segments: readonly EngineTimeSignatureSegment[]): void {
    this.native.setTimeSignatureSegments(segments);
  }

  /**
   * Converts a quarter-note position to a whole sample position on the engine timeline, through the tempo map.
   *
   * @param ppq Position in quarter notes (PPQ).
   * @returns Sample index.
   */
  sampleAtPpq(ppq: number): number {
    return this.native.sampleAtPpq(ppq);
  }

  /** Sets the loop region between two quarter-note positions and enables or disables it. */
  setLoop(loop: EngineLoop): void {
    const request = requestObject('setLoop', loop, 'loop', true);
    this.native.setLoop(
      request.startPpq,
      request.endPpq,
      isAbsent(request.enabled) ? true : requireBoolean(request.enabled, 'enabled'),
    );
  }

  /**
   * Registers a parameter's metadata (name, unit, range, default curve) for automation. A new `id` adds
   * a parameter; omitted optional fields take the defaults documented on {@link EngineParameterInfo}.
   * Control thread only; do not call concurrently with {@link process}.
   */
  addParameter(info: EngineParameterInfo): void {
    this.native.addParameter(parameterToNative(info));
  }

  /** Every registered parameter, in registration order. */
  parameters(): Array<Required<EngineParameterInfo>> {
    const count: number = this.native.parameterCount();
    const out: Array<Required<EngineParameterInfo>> = [];
    for (let index = 0; index < count; index += 1) {
      out.push(parameterFromNative(this.native.parameterInfoByIndex(index)));
    }
    return out;
  }

  /** @throws RangeError when no parameter has `id` */
  parameterInfo(id: number): Required<EngineParameterInfo> {
    return parameterFromNative(this.native.parameterInfo(id));
  }

  /**
   * Replaces the automation lane driving `paramId`. An empty `points` array
   * leaves the target undriven rather than snapping it to 0 or a default:
   * once adopted, the target reverts to the last value explicitly sent
   * through {@link setParameter} / {@link setParameterSmoothed}, or is left
   * unchanged if no such value was ever sent for `paramId`. This holds
   * regardless of whether the manual value or the lane clear reaches the
   * audio thread first.
   * A reserved mixer id (`0x4d58xxxx`) other than a master id names no strip and
   * throws a `RangeError`; resolve ids with the `resolve*AutomationId`
   * methods (`resolveStripAutomationId` for the master fader, pan and width).
   */
  setAutomationLane(paramId: number, points: EngineAutomationPoint[]): void {
    this.native.setAutomationLane(paramId, points.map(automationPointToNative));
  }

  /** Number of parameters that currently have an automation lane. */
  get automationLaneCount(): number {
    return this.native.automationLaneCount();
  }

  /**
   * Replaces the whole marker set. Entries without an `id` are assigned the
   * lowest id above every id this engine has stored or assigned that the list
   * does not carry; entries carrying an `id` keep it (positive, unique within
   * the list). Returns the stored markers in the order given.
   */
  setMarkers(markers: ReadonlyArray<Omit<EngineMarker, 'id'> & { id?: number }>): EngineMarker[] {
    if (!Array.isArray(markers)) {
      this.native.setMarkers(markers as never);
      return [];
    }
    let floor = this.nextMarkerId;
    for (const stored of this.markers()) {
      floor = Math.max(floor, stored.id + 1);
    }
    const assigned = assignMarkerIds(markers, floor);
    this.native.setMarkers(assigned.markers.map(markerToNative) as never);
    this.nextMarkerId = assigned.nextId;
    return assigned.markers.map((marker) => this.marker(marker.id));
  }

  /** Every stored marker, in position order. */
  markers(): EngineMarker[] {
    const count: number = this.native.markerCount();
    const out: EngineMarker[] = [];
    for (let index = 0; index < count; index += 1) {
      out.push(markerFromNative(this.native.markerByIndex(index)));
    }
    return out;
  }

  /** @throws RangeError when no marker has `id` */
  marker(id: number): EngineMarker {
    return markerFromNative(this.native.marker(id));
  }

  /**
   * Moves the transport to the position of marker `markerId`. An unknown marker is a `RangeError`.
   * `renderFrame` is the render-frame time to apply; omit it for immediate (a negative value is refused).
   */
  seekMarker(markerId: number, renderFrame?: number): void {
    this.native.seekMarker(markerId, resolveRenderFrame(renderFrame));
  }

  /**
   * Sets the loop region from the positions of two markers (start, end) and enables it. An unknown
   * marker id is a `RangeError`.
   */
  setLoopFromMarkers(startMarkerId: number, endMarkerId: number): void {
    this.native.setLoopFromMarkers(startMarkerId, endMarkerId);
  }

  /**
   * Set a metronome config; click lengths are limited to one second. Give
   * `clickSamples` or `clickSec`, not both; an explicit `0` is a `RangeError`.
   */
  setMetronome(config: EngineMetronomeConfig): void {
    const request = requestObject('setMetronome', config, 'config', true);
    const { clickSec, ...rest } = request;
    const native: Record<string, unknown> = { ...rest };
    const samples = optionalNonZeroInteger(request.clickSamples, 'clickSamples');
    const seconds = optionalNonZero(clickSec, 'clickSec');
    if (samples !== undefined && seconds !== undefined) {
      throw refuse('give clickSamples or clickSec, not both');
    }
    if (seconds !== undefined) {
      native.clickSeconds = seconds;
    }
    this.native.setMetronome(native);
  }

  /** The metronome configuration; reports only the click-length spelling in effect. */
  metronome(): EngineMetronomeConfig {
    const { clickSamples, clickSeconds, ...rest } = this.native.metronome() as {
      clickSamples: number;
      clickSeconds: number;
    } & Record<string, unknown>;
    const out = rest as unknown as EngineMetronomeConfig;
    if (clickSamples > 0) {
      out.clickSamples = clickSamples;
    } else if (clickSeconds > 0) {
      out.clickSec = clickSeconds;
    }
    return out;
  }

  /**
   * Sample at which a count-in of `bars` bars from `startSample` ends. Counting starts at the
   * beginning of the bar containing `startSample` (a mid-bar start gets a short first bar), and
   * every counted bar has the length of the time signature in effect at `startSample`.
   * Returns `startSample` when `bars <= 0`.
   */
  countInEndSample(startSample: number, bars: number): number {
    return this.native.countInEndSample(startSample, bars);
  }

  /**
   * Replaces the whole published clip set; every entry is validated and copied, and a refused entry
   * leaves the previous set unchanged. Control thread only.
   */
  setClips(clips: EngineClip[]): void {
    this.native.setClips(clips.map((clip, i) => toNativeClip(clip, `clips[${i}]`)));
  }

  /**
   * Replaces the clip whose `id` matches `clip.id`, or adds it when no clip has
   * that id. Every other clip stays as published. The clip is validated like a
   * `setClips` entry; a refused clip leaves the published set unchanged.
   *
   * @example
   * ```ts
   * declare const engine: RealtimeEngine;
   * engine.upsertClip({ id: 3, channels: [samples], startPpq: 4, gain: 0.5 });
   * ```
   */
  upsertClip(clip: EngineClip): void {
    this.native.upsertClip(toNativeClip(clip, 'clip'));
  }

  /**
   * Removes the clip `clipId`, leaving every other clip as published.
   * Throws when no clip has that id.
   */
  removeClip(clipId: number): void {
    this.native.removeClip(clipId);
  }

  /** Number of published audio clips. */
  get clipCount(): number {
    return this.native.clipCount();
  }

  /**
   * Declares the mixer track lanes in their order. Throws a `RangeError` when
   * the list holds more than {@link MAX_TRACK_LANES} lanes, and a `TypeError`
   * for an entry that is not a lane object.
   */
  setTrackLanes(lanes: readonly EngineTrackLane[]): void {
    if (!Array.isArray(lanes)) {
      throw refuseType('lanes must be an array');
    }
    this.native.setTrackLanes(
      lanes.map((lane, index) => laneToNative('setTrackLanes', lane, index)),
    );
    this.laneOrder = lanes.map((lane) => lane.trackId);
  }

  /**
   * Replaces one lane's sends, leaving its output bus, layout and every other
   * lane as published. An empty array clears the lane's sends.
   *
   * The edited lane list goes through {@link setTrackLanes}, so it is validated
   * the same way (a send naming an undeclared bus, a duplicate send bus or a
   * level outside the allowed range is refused), automation ids keep naming
   * their track, and an unchanged send list keeps the sends' in-flight ramps; a
   * changed list restarts every send ramp on the lane. Lane membership is
   * caller-owned here: `trackId` must already be in the published lane list
   * (set through {@link setTrackLanes}), otherwise this throws. The worklet
   * facade's `setTrackSends` shares the name but creates lanes as needed.
   * Control-thread only; do not call concurrently with `process`.
   *
   * @throws RangeError for an unknown track (the message names the id) or a send
   *   list `setTrackLanes` would refuse as invalid; `SonareError` `InvalidState` when the
   *   list is valid but earlier configuration refuses it; `TypeError`
   *   when `sends` is not an array
   */
  setTrackSends(trackId: number, sends: EngineTrackSend[]): void {
    this.native.setTrackSends(
      trackId,
      Array.isArray(sends) ? normalizeSends(sends, 'sends') : sends,
    );
  }

  /**
   * Sets one lane's output bus, leaving its sends, layout and every other lane
   * as published. `null` returns the lane to the master mix; any other value
   * must name a declared bus, and `0` is a `RangeError`. Same validation,
   * membership and threading contract as {@link setTrackSends}.
   */
  setTrackOutputBus(trackId: number, busId: number | null): void {
    this.native.setTrackOutputBus(trackId, optionalNonZeroInteger(busId, 'busId') ?? 0);
  }

  /**
   * Configure realtime engine buses: layout, fader, output and sends. Replaces
   * the whole bus list. An `outputBusId` or send naming an undeclared bus, the
   * bus itself, or forming a cycle (through outputs, sends, or bus-sourced
   * sidechain keys) is rejected and leaves the previous buses unchanged.
   */
  setTrackBuses(buses: EngineBus[]): void {
    if (!Array.isArray(buses)) {
      throw refuseType('buses must be an array');
    }
    this.native.setTrackBuses(buses.map((bus, index) => busToNative('setTrackBuses', bus, index)));
  }

  /**
   * Install a timeline compiled by {@link Project.compileTimeline}, all or
   * nothing. Stopped transport only: a playing engine throws and is left
   * unchanged, as does any validation failure. The timeline replaces tempo and
   * time-signature segments, markers, track lanes, automation lanes, clips and
   * the project mixer scene's track strips; a value set on one of these through
   * a low-level setter is overwritten by the next apply. Instruments, buses,
   * the master strip, metronome, loop and capture are untouched, so bind
   * instruments with {@link setBuiltinInstrument} (or a sibling) using each
   * MIDI track's destination id.
   *
   * The engine keeps its own reference, so the timeline may be disposed right
   * after this call. A disposed timeline throws.
   *
   * Also throws on an unprepared engine and on a timeline whose sample rate
   * differs from the engine's. Once a timeline is applied, a later prepare at
   * another rate is refused until the timeline's clips are cleared.
   */
  applyProjectTimeline(timeline: ProjectTimeline): void {
    this.native.applyProjectTimeline(timelineHandle(timeline));
    this.laneOrder = 'timeline';
  }

  /**
   * Keys one insert of a bus strip from a track lane or another bus. The key
   * is taken before the source's lane fader or bus `gainDb`. `null` removes the
   * binding. Control-thread only: must not be called concurrently with
   * {@link process}.
   */
  setBusSidechain(busId: number, insertIndex: number, source: SidechainSource | null): void {
    const { kind, id } = sidechainSourcePair('setBusSidechain', source, true);
    this.native.setBusSidechain(busId, insertIndex, kind, id);
  }

  /**
   * Keys one insert of a strip from another signal (ducking/sidechainRouter
   * inserts); `null` removes the binding. A track strip accepts only a source of
   * kind `'track'`; the master strip takes a track lane or a bus. For the master
   * strip `insertIndex` counts the pre-fader inserts first, then the post-fader
   * inserts, as the other master insert setters do.
   *
   * A key naming the track itself, or a binding that would close a cycle across
   * the bindings as a whole, throws and leaves the existing bindings unchanged.
   * The source and destination tracks need not exist yet, and a track's insert
   * index is not range-checked: a binding outlives lane re-publication. The key
   * is taken after the source's lane strip, with the delay to the keyed insert
   * planned so the rendered result does not depend on lane order or block size.
   * Control-thread only: must not be called concurrently with {@link process}.
   */
  setStripSidechain(ref: StripRef, insertIndex: number, source: SidechainSource | null): void {
    const fnName = 'setStripSidechain';
    const target = stripTarget(ref);
    const { kind, id } = sidechainSourcePair(fnName, source, true);
    if (target.master) {
      this.native.setMasterSidechain(insertIndex, kind, id);
      return;
    }
    this.assertTrackSource(kind);
    this.native.setLaneSidechain(target.trackId, insertIndex, id);
  }

  /**
   * Reports whether {@link setStripSidechain} would accept the binding, without
   * changing anything. `reason` names the first check the setter would fail.
   * Control-thread only: must not be called concurrently with {@link process}.
   */
  canSetStripSidechain(
    ref: StripRef,
    insertIndex: number,
    source: SidechainSource,
  ): SidechainCheck {
    const fnName = 'canSetStripSidechain';
    const target = stripTarget(ref);
    const { kind, id } = sidechainSourcePair(fnName, source, false);
    if (target.master) {
      return sidechainCheck(this.native.canSetMasterSidechain(insertIndex, kind, id));
    }
    this.assertTrackSource(kind);
    return sidechainCheck(this.native.canSetLaneSidechain(target.trackId, insertIndex, id));
  }

  private assertTrackSource(kind: number): void {
    if (kind !== SIDECHAIN_SOURCE_KIND.track) {
      throw refuse(`a track strip accepts only a source of kind 'track'`);
    }
  }

  /** Reports whether {@link setBusSidechain} would accept the binding, without changing anything. */
  canSetBusSidechain(busId: number, insertIndex: number, source: SidechainSource): SidechainCheck {
    const { kind, id } = sidechainSourcePair('canSetBusSidechain', source, false);
    return sidechainCheck(this.native.canSetBusSidechain(busId, insertIndex, kind, id));
  }

  /**
   * Replaces a bus's strip with `settings`. The bus must already exist via
   * {@link setTrackBuses}.
   */
  setBusStrip(busId: number, settings: MixSceneBusInput): void {
    this.native.setBusStripJson(busId, sceneBusJson(settings));
  }

  /** Replaces one equalizer band of a bus strip. */
  setBusEqBand(busId: number, bandIndex: number, band: EqBandInput): void {
    this.native.setBusStripEqBandJson(busId, bandIndex, eqBandJsonOf(band));
  }

  /**
   * Replaces a strip with `settings`: a track id names a track strip, `'master'`
   * the master strip. The strip document carries its own id; the engine applies
   * it to the addressed strip.
   */
  setStrip(ref: StripRef, settings: MixSceneStripInput): void {
    const target = stripTarget(ref);
    const json = sceneStripJson(settings);
    if (target.master) {
      this.native.setMasterStripJson(json);
    } else {
      this.native.setTrackStripJson(target.trackId, json);
    }
  }

  /** Replaces one equalizer band of a strip. */
  setStripEqBand(ref: StripRef, bandIndex: number, band: EqBandInput): void {
    const target = stripTarget(ref);
    const json = eqBandJsonOf(band);
    if (target.master) {
      this.native.setMasterStripEqBandJson(bandIndex, json);
    } else {
      this.native.setTrackStripEqBandJson(target.trackId, bandIndex, json);
    }
  }

  /** Bypasses or engages one strip insert; `resetOnBypass` clears its state when it is bypassed. */
  setStripInsertBypassed(
    ref: StripRef,
    insertIndex: number,
    state: { bypassed: boolean; resetOnBypass?: boolean },
  ): void {
    const target = stripTarget(ref);
    const { bypassed, resetOnBypass } = bypassState('setStripInsertBypassed', state);
    if (target.master) {
      this.native.setMasterStripInsertBypassed(insertIndex, bypassed, resetOnBypass);
    } else {
      this.native.setTrackStripInsertBypassed(target.trackId, insertIndex, bypassed, resetOnBypass);
    }
  }

  /**
   * Bus counterpart of {@link setStripInsertBypassed}. The bus must already
   * exist via {@link setTrackBuses} and carry a strip configured with
   * {@link setBusStrip}.
   */
  setBusInsertBypassed(
    busId: number,
    insertIndex: number,
    state: { bypassed: boolean; resetOnBypass?: boolean },
  ): void {
    const { bypassed, resetOnBypass } = bypassState('setBusInsertBypassed', state);
    this.native.setBusStripInsertBypassed(busId, insertIndex, bypassed, resetOnBypass);
  }

  /**
   * Changes one strip insert parameter in realtime, addressed by the
   * processor's JSON-key parameter name: an entry in {@link
   * masteringInsertParamInfo} whose `id` is non-null. Applied at the next
   * block head via the engine command queue; safe during playback. Throws if
   * the track, insert, or name is unknown, the param is not realtime-safe, or
   * the command queue is full.
   */
  setStripInsertParam(
    ref: StripRef,
    insertIndex: number,
    param: { name: string; value: number },
  ): void {
    const target = stripTarget(ref);
    const { name, value } = insertParam('setStripInsertParam', param);
    if (target.master) {
      this.native.setMasterStripInsertParamByName(insertIndex, name, value);
    } else {
      this.native.setTrackStripInsertParamByName(target.trackId, insertIndex, name, value);
    }
  }

  /**
   * Bus counterpart of {@link setStripInsertParam}. The bus must already exist
   * via {@link setTrackBuses} and carry a strip configured with {@link setBusStrip}.
   */
  setBusInsertParam(
    busId: number,
    insertIndex: number,
    param: { name: string; value: number },
  ): void {
    const { name, value } = insertParam('setBusInsertParam', param);
    this.native.setBusStripInsertParamByName(busId, insertIndex, name, value);
  }

  /**
   * Applies a live strip insert edit on this engine's owning thread, bypassing
   * the command queue. Returns `false` rather than throwing when the track,
   * insert, or name is unknown, or the param is not realtime-safe. Not safe to
   * call concurrently with {@link process}.
   */
  applyStripInsertParamNow(
    ref: StripRef,
    insertIndex: number,
    param: { name: string; value: number },
  ): boolean {
    const target = stripTarget(ref);
    const { name, value } = insertParam('applyStripInsertParamNow', param);
    return target.master
      ? this.native.applyMasterStripInsertParamByNameNow(insertIndex, name, value)
      : this.native.applyTrackStripInsertParamByNameNow(target.trackId, insertIndex, name, value);
  }

  /** Bus counterpart of {@link applyStripInsertParamNow}. */
  applyBusInsertParamNow(
    busId: number,
    insertIndex: number,
    param: { name: string; value: number },
  ): boolean {
    const { name, value } = insertParam('applyBusInsertParamNow', param);
    return this.native.applyBusStripInsertParamByNameNow(busId, insertIndex, name, value);
  }

  /**
   * Restores a retained strip insert value exactly, without a ramp. Used to
   * replay retained edits after {@link setStrip} replaced the strip. Throws if
   * the track, insert, or name is unknown.
   */
  restoreStripInsertParam(
    ref: StripRef,
    insertIndex: number,
    param: { name: string; value: number },
  ): void {
    const target = stripTarget(ref);
    const { name, value } = insertParam('restoreStripInsertParam', param);
    if (target.master) {
      this.native.restoreMasterStripInsertParamByName(insertIndex, name, value);
    } else {
      this.native.restoreTrackStripInsertParamByName(target.trackId, insertIndex, name, value);
    }
  }

  /** Bus counterpart of {@link restoreStripInsertParam}. */
  restoreBusInsertParam(
    busId: number,
    insertIndex: number,
    param: { name: string; value: number },
  ): void {
    const { name, value } = insertParam('restoreBusInsertParam', param);
    this.native.restoreBusStripInsertParamByName(busId, insertIndex, name, value);
  }

  /**
   * Forgets the remembered manual insert-parameter values of one strip and
   * discards its queued insert edits. Call before {@link setStrip} replaces the
   * strip when its old values must not carry over; the setter never does this
   * itself, since a queued edit may already target the new chain.
   */
  clearStripInsertParameterBases(ref: StripRef): void {
    const target = stripTarget(ref);
    if (target.master) {
      this.native.clearMasterInsertParameterBases();
    } else {
      this.native.clearTrackInsertParameterBases(target.trackId);
    }
  }

  /**
   * Bus counterpart of {@link clearStripInsertParameterBases}. Also accepts a bus
   * identity that was already removed, covering every selector the identity held
   * across remove/re-add.
   */
  clearBusInsertParameterBases(busId: number): void {
    this.native.clearBusInsertParameterBases(busId);
  }

  /**
   * Resolves a strip insert parameter (by its JSON-key name) to the reserved
   * automation id that can then be driven with {@link setAutomationLane},
   * {@link setParameter}, or {@link setParameterSmoothed}, exactly like a
   * fader/pan id. Throws a `RangeError` when the track, insert, or name is
   * unknown, like every `resolve*AutomationId` on every surface.
   *
   * This is how a mastering processor gets time-varying automation: the
   * `eq.*`, `dynamics.*`, `saturation.*`, `spectral.*`, `stereo.*`,
   * `maximizer.*` and `multiband.*` processors are all available as strip
   * inserts, so placing one on a strip and resolving its parameter here drives
   * it at audio-block precision, live and offline alike. The whole-signal stages of the offline
   * mastering chain that have no insert form (`repair.declick`, `repair.declip`,
   * `repair.trimSilence`, `loudness`, and the match stages) have no automation id: they buffer the
   * entire signal by construction and do not run on the realtime path. The four realtime repair
   * inserts (`repair.decrackle`, `repair.dehum`, `repair.denoiseClassical`,
   * `repair.dereverbClassical`) load into a strip, but every one of their parameters is
   * construction-only, so no automation id resolves (a `RangeError`) and an edit needs a strip
   * replacement.
   *
   * Track, bus and master insert ids share one lifetime rule: an id names its
   * strip by identity and the kind of processor in its slot (for
   * `effects.gsEfx`, its EFX type too), so it survives lane and bus reorders
   * and the removal of other strips, and stays valid until its track or bus is
   * removed or its slot comes to hold another kind of processor. After that it
   * applies nothing, its queued edits are dropped, and it is never reissued;
   * resolve again for the new processor. A change that would take the engine
   * past its 8192 insert-id entries is refused.
   *
   * @throws RangeError when the track, insert or parameter name does not resolve.
   */
  resolveStripInsertAutomationId(ref: StripRef, insertIndex: number, paramName: string): number {
    const target = stripTarget(ref);
    return target.master
      ? this.native.resolveMasterInsertAutomationId(insertIndex, paramName)
      : this.native.resolveTrackInsertAutomationId(target.trackId, insertIndex, paramName);
  }

  /** Bus-strip counterpart of {@link resolveStripInsertAutomationId}. */
  resolveBusInsertAutomationId(busId: number, insertIndex: number, paramName: string): number {
    return this.native.resolveBusInsertAutomationId(busId, insertIndex, paramName);
  }

  /**
   * Resolve a strip's fader (`'faderDb'`), pan (`'pan'`) or, for the master
   * strip only, width (`'width'`) to its reserved automation id. Same lifetime
   * rule as {@link resolveStripInsertAutomationId}: a track id names the track,
   * not its lane position, so it keeps driving that track across
   * {@link setTrackLanes} reorders and applies nothing once the track is
   * removed; the master strip always exists, so its id never goes stale. Throws
   * a `RangeError` when the track has no lane, the name is unknown, or `'width'`
   * is asked of a track.
   */
  resolveStripAutomationId(ref: StripRef, kind: 'faderDb' | 'pan' | 'width'): number {
    const target = stripTarget(ref);
    assertClosedToken(kind, ['faderDb', 'pan', 'width'], 'kind');
    if (target.master) {
      return this.native.resolveMasterAutomationId(kind);
    }
    if (kind === 'width') {
      throw refuse(`a track strip has no 'width' parameter`);
    }
    return this.native.resolveTrackLaneAutomationId(target.trackId, kind);
  }

  /**
   * Resolve a bus's fader (`'faderDb'`) to its reserved automation id. Same
   * lifetime rule as {@link resolveStripInsertAutomationId}: the id names the
   * bus, not its position in the bus list, so it keeps driving that bus across
   * {@link setTrackBuses} reorders and the removal of other buses, and applies
   * nothing once the bus is removed. Throws a `RangeError` when the bus is not
   * configured or the name is not `'faderDb'`.
   */
  resolveBusAutomationId(busId: number, paramName: 'faderDb'): number {
    return this.native.resolveBusAutomationId(busId, paramName);
  }

  /** Return the immutable construction value for a resolved insert parameter. */
  insertParameterConstructedValue(paramId: number): number {
    return this.native.insertParameterConstructedValue(paramId);
  }

  /**
   * Resolves a hosted instrument's continuous parameter (by its JSON-key name)
   * to the reserved automation id, so an instrument parameter is driven at
   * audio-block precision exactly like a strip insert. Throws a `RangeError` when
   * the destination has no bound instrument, the instrument exposes no
   * automatable parameters, or the name is unknown. Arguments are
   * `(destinationId, paramName)`, as in the C ABI.
   *
   * For the NativeSynth ({@link setSynthInstrument}) the names are the
   * continuous `SynthPatch` fields: `gain`, `busDrive`, `cutoffHz`,
   * `resonanceQ`, `drive`, `keyTrack`, `envToCutoffCents`, `velToCutoffCents`,
   * `ampAttackMs`, `ampDecayMs`, `ampSustain`, `ampReleaseMs`,
   * `filterAttackMs`, `filterDecayMs`, `filterSustain`, `filterReleaseMs`,
   * `lfoRateHz`, `lfoToPitchCents`, `lfo2RateHz`, `glideMs`, `bodyMix`,
   * `stereoSpread`, `detuneCents`, `driftCents`, `pitchOffsetCents`,
   * `hpCutoffHz`, `sampleHoldHz`, `bitDepth`, `driftRateHz`, `ampDelayMs`,
   * `ampHoldMs`, `filterDelayMs`, `filterHoldMs`.
   *
   * Structural fields (`preset`, `engineMode`, `waveform`, `filterModel`,
   * `unison`, `polyphony`, `body`, `modRoutings`) are not automatable and
   * throw a `RangeError`; rebind the instrument with a new patch instead.
   *
   * All automatable parameters of a NativeSynth reach already-sounding voices
   * from the next processed block. Voices using GM program patches are
   * unaffected; parameters apply to NativeSynth's own patch only.
   */
  resolveInstrumentAutomationId(destinationId: number, paramName: string): number {
    return this.native.resolveInstrumentAutomationId(destinationId, paramName);
  }

  /**
   * Sets a strip's fader in decibels in realtime, through its reserved
   * automation id. `renderFrame` is the render-frame time to apply; omit for
   * immediate (negative is refused).
   */
  setStripFaderDb(ref: StripRef, faderDb: number, renderFrame?: number): void {
    this.setParameter(this.resolveStripAutomationId(ref, 'faderDb'), faderDb, renderFrame);
  }

  /** Sets a bus's gain in decibels in realtime, through its reserved automation id. */
  setBusGainDb(busId: number, gainDb: number, renderFrame?: number): void {
    this.setParameter(this.resolveBusAutomationId(busId, 'faderDb'), gainDb, renderFrame);
  }

  /**
   * Sets a track strip's pan position (-1..1) in realtime. Applied at the next
   * block head via the engine command queue; safe during playback. The master
   * strip has no pan setter here: drive its pan through
   * {@link resolveStripAutomationId}.
   *
   * @param ref Track the strip belongs to.
   * @param pan Pan position from -1 (hard left) to 1 (hard right).
   */
  setStripPan(ref: StripRef, pan: number): void {
    this.native.setTrackStripPan(trackOnly(ref), pan);
  }

  /**
   * Sets a track strip's pan law in realtime. Applied at the next block head via
   * the engine command queue; safe during playback.
   *
   * @param ref Track the strip belongs to.
   * @param panLaw Pan law name; a number is refused.
   */
  setStripPanLaw(ref: StripRef, panLaw: PanLaw): void {
    this.native.setTrackStripPanLaw(trackOnly(ref), panLawOf(panLaw));
  }

  /**
   * Sets a track strip's pan mode in realtime. Applied at the next block head
   * via the engine command queue; safe during playback.
   *
   * @param ref Track the strip belongs to.
   * @param panMode Pan mode name; a number is refused.
   */
  setStripPanMode(ref: StripRef, panMode: PanMode): void {
    this.native.setTrackStripPanMode(trackOnly(ref), panModeOf(panMode));
  }

  /**
   * Sets a track strip's independent left/right pan positions (dual-pan mode)
   * in realtime. Applied at the next block head via the engine command queue;
   * safe during playback.
   *
   * @param ref Track the strip belongs to.
   * @param leftPan Left-channel pan position from -1 to 1.
   * @param rightPan Right-channel pan position from -1 to 1.
   */
  setStripDualPan(ref: StripRef, leftPan: number, rightPan: number): void {
    this.native.setTrackStripDualPan(trackOnly(ref), leftPan, rightPan);
  }

  /**
   * Sets a track strip's surround pan in realtime. Applied at the next block
   * head with a short 5 ms placement glide, so a live move is glitch-free; it
   * takes effect when the lane feeds a destination wider than stereo (a 5.1/7.1
   * group bus or master). Requires a lane with a bound strip.
   *
   * @param ref Track the strip belongs to.
   * @param pan Surround position; omitted fields default to a centered point source.
   * @throws TypeError for a wrong-typed field or an unknown key; RangeError for a
   *   field outside its range
   */
  setStripSurroundPan(ref: StripRef, pan: SurroundPan): void {
    const trackId = trackOnly(ref);
    this.native.setTrackStripSurroundPan(
      trackId,
      requestObject('setStripSurroundPan', pan, 'pan', true),
    );
  }

  /**
   * Sets a bus strip's pan position (-1..1) in realtime. Applied at the next
   * block head via the engine command queue; safe during playback. Rejected on
   * a surround (>2ch) bus, where pan is not defined.
   *
   * @param busId Bus the strip belongs to.
   * @param pan Pan position from -1 (hard left) to 1 (hard right).
   */
  setBusPan(busId: number, pan: number): void {
    this.native.setBusStripPan(busId, pan);
  }

  /**
   * Sets a bus strip's pan law in realtime. Applied at the next block head via
   * the engine command queue; safe during playback. Rejected on a surround
   * (>2ch) bus.
   *
   * @param busId Bus the strip belongs to.
   * @param panLaw Pan law name; a number is refused.
   */
  setBusPanLaw(busId: number, panLaw: PanLaw): void {
    this.native.setBusStripPanLaw(busId, panLawOf(panLaw));
  }

  /**
   * Sets a bus strip's pan mode in realtime. Applied at the next block head via
   * the engine command queue; safe during playback. Rejected on a surround
   * (>2ch) bus.
   *
   * @param busId Bus the strip belongs to.
   * @param panMode Pan mode name; a number is refused.
   */
  setBusPanMode(busId: number, panMode: PanMode): void {
    this.native.setBusStripPanMode(busId, panModeOf(panMode));
  }

  /**
   * Sets a bus strip's independent left/right pan positions (dual-pan mode) in
   * realtime. Applied at the next block head via the engine command queue;
   * safe during playback. Rejected on a surround (>2ch) bus.
   *
   * @param busId Bus the strip belongs to.
   * @param leftPan Left-channel pan position from -1 to 1.
   * @param rightPan Right-channel pan position from -1 to 1.
   */
  setBusDualPan(busId: number, leftPan: number, rightPan: number): void {
    this.native.setBusStripDualPan(busId, leftPan, rightPan);
  }

  /**
   * Sets a track strip's alignment delay in samples.
   * `delaySamples` is a non-negative whole-sample delay that moves this lane later
   * relative to every other lane. It is not latency: PDC does not compensate it
   * and the reported graph latency is unchanged. It reallocates the strip's delay
   * line, so treat it as a structural change: do NOT call it concurrently with
   * {@link process}. Stop
   * playback (or otherwise quiesce the audio callback) before calling.
   *
   * @param ref Track the strip belongs to.
   * @param delaySamples Non-negative channel delay in samples.
   */
  setStripChannelDelaySamples(ref: StripRef, delaySamples: number): void {
    this.native.setTrackStripChannelDelaySamples(trackOnly(ref), delaySamples);
  }

  /**
   * Pops the oldest pending clip-page sample request raised by the audio thread, or `null` when none
   * is queued. Supply the page through the clip-page provider.
   */
  popClipPageRequest(): ClipPageSampleRequest | null {
    return this.native.popClipPageRequest();
  }

  /**
   * Sets the clip-page look-ahead window in timeline frames.
   *
   * The player reports the pages it is *about to* read that are not resident
   * yet, so a streaming host can service them before the audio thread reaches
   * them. Without look-ahead a page miss is only reported after the read
   * already produced silence, which costs one block of silence at every page
   * boundary the host has not primed.
   *
   * Look-ahead requests drain through the same {@link popClipPageRequest}
   * queue and are queued *after* the block's genuine misses, so a host that
   * keeps only the newest request per clip tracks the look-ahead frontier.
   *
   * `prepare` defaults this to half a second at the engine's sample rate; `0`
   * disables it. A clip whose pages are all resident produces no requests
   * either way. Safe to call during playback.
   */
  setClipPagePrefetchFrames(frames: number): void {
    this.native.setClipPagePrefetchFrames(frames);
  }

  /** Current clip-page look-ahead window in timeline frames. */
  get clipPagePrefetchFrames(): number {
    return this.native.clipPagePrefetchFrames();
  }

  /**
   * Configures capture as one declarative setup: the buffer, the source, the
   * record offset, the input monitor and the punch window. Absent fields take
   * their defaults (`numChannels` 2, `source` `'output'`, `recordOffsetSamples`
   * 0, input monitor enabled at gain 1, no punch), so a call describes the whole setup rather
   * than a change to it.
   *
   * The buffer is reallocated only when `numChannels` or `capacityFrames`
   * differs from the previous call; reallocating while armed is an
   * `InvalidState` {@link SonareError}. Positive `recordOffsetSamples` delay
   * capture relative to the punch window; input arriving within the offset after
   * a loop wrap belongs to the end of the pass played before it, so every pass
   * captures the whole loop range. A `punch` window is half-open `[start, end)`
   * in timeline positions (see {@link CapturePunch}).
   *
   * @throws TypeError for an unknown key; RangeError for a non-positive size, an
   *   unknown `source`, or a punch that does not give exactly one start and one end
   */
  configureCapture(options: CaptureOptions): void {
    const fnName = 'configureCapture';
    const request = requestObject(fnName, options, 'options', true);
    if (isAbsent(request.capacityFrames)) {
      throw refuseType(`capacityFrames must be an integer`);
    }
    const capacityFrames = request.capacityFrames as number;
    const numChannels = isAbsent(request.numChannels) ? 2 : (request.numChannels as number);
    // Two clauses, two owners: that each is a count a JS number still denotes is
    // the shared check's, that neither may be zero is this buffer's.
    assertNonNegativeSafeInteger(numChannels, 'numChannels');
    assertNonNegativeSafeInteger(capacityFrames, 'capacityFrames');
    if (numChannels <= 0) {
      throw refuse(`numChannels must be an integer in [1, ${Number.MAX_SAFE_INTEGER}]`);
    }
    if (capacityFrames <= 0) {
      throw refuse(`capacityFrames must be an integer in [1, ${Number.MAX_SAFE_INTEGER}]`);
    }
    const source = isAbsent(request.source) ? 'output' : request.source;
    assertClosedToken(source, CAPTURE_SOURCES, 'source');
    const offset = isAbsent(request.recordOffsetSamples)
      ? 0
      : requireFiniteInteger(request.recordOffsetSamples, 'recordOffsetSamples');
    let monitor = { enabled: true, gain: 1 };
    if (!isAbsent(request.inputMonitor)) {
      const inputMonitor = request.inputMonitor;
      if (!isObjectValue(inputMonitor)) {
        throw refuseType(`inputMonitor must be an object`);
      }
      monitor = {
        enabled: requireBoolean(inputMonitor.enabled, 'inputMonitor.enabled'),
        gain: isAbsent(inputMonitor.gain)
          ? 1
          : requireFinite(inputMonitor.gain, 'inputMonitor.gain'),
      };
    }
    const punch = isAbsent(request.punch) ? undefined : this.resolvePunch(request.punch);

    const shape = this.captureShape;
    if (
      shape === undefined ||
      shape.numChannels !== numChannels ||
      shape.capacityFrames !== capacityFrames
    ) {
      if (this.native.captureStatus().armed) {
        throw invalidState(`the capture buffer cannot be resized while armed`);
      }
      this.native.setCaptureBuffer(numChannels, capacityFrames);
      this.captureShape = { numChannels, capacityFrames };
    }
    this.native.setCaptureSource(source);
    this.native.setRecordOffsetSamples(offset);
    this.native.setInputMonitor(monitor.enabled, monitor.gain);
    if (punch === undefined) {
      this.native.setCapturePunch(0, 0, false);
    } else {
      this.native.setCapturePunch(punch.start, punch.end, true);
    }
  }

  private resolvePunch(punch: CapturePunch): { start: number; end: number } {
    if (!isObjectValue(punch)) {
      throw refuseType(`punch must be an object`);
    }
    const request = punch as Record<string, unknown>;
    const bound = (edge: 'start' | 'end'): number => {
      const ppqName = `${edge}Ppq`;
      const sampleName = `${edge}Sample`;
      const secName = `${edge}Sec`;
      const names = [ppqName, sampleName, secName];
      const given = requireOneOf(
        names.map((name) => `punch.${name}`),
        names.map((name) => !isAbsent(request[name])),
      );
      if (given === 0) {
        const ppq = requireFinite(request[ppqName], ppqName);
        if (ppq < 0) {
          throw refuse(`${ppqName} must be at least 0`);
        }
        return Math.round(this.native.sampleAtPpq(ppq));
      }
      return resolveSampleBound(
        request[sampleName] as number | undefined,
        request[secName] as number | undefined,
        this.sampleRate,
        sampleName,
        secName,
      ) as number;
    };
    return { start: bound('start'), end: bound('end') };
  }

  /** Arms or disarms capture. */
  armCapture(state: { armed: boolean }): void {
    const request = requestObject('armCapture', state, 'state', true);
    this.native.armCapture(requireBoolean(request.armed, 'armed'));
  }

  /** Clears the capture frame counter. Call while capture is not running. */
  resetCapture(): void {
    this.native.resetCapture();
  }

  /**
   * Reads the capture state: recorded frames, overflow count, armed and punch flags, source and record offset.
   */
  captureStatus(): EngineCaptureStatus {
    return this.native.captureStatus();
  }

  /**
   * Read the recorded samples from the addon-owned capture buffer.
   *
   * Returns one `Float32Array` per capture channel, each sliced to the number
   * of frames recorded so far (see {@link captureStatus}). Call after capture
   * to retrieve the audio written after {@link configureCapture}.
   */
  capturedAudio(): Float32Array[] {
    return this.native.capturedAudio();
  }

  /**
   * Replaces the audio processing graph (nodes and connections). Invalid specs are refused with a
   * `TypeError` / `RangeError`. Control thread only; do not call concurrently with {@link process}.
   */
  setGraph(spec: EngineGraphSpec): void {
    this.native.setGraph(graphToNative(spec));
  }

  /** Number of nodes in the installed graph. */
  get graphNodeCount(): number {
    return this.native.graphNodeCount();
  }

  /** Number of connections in the installed graph. */
  get graphConnectionCount(): number {
    return this.native.graphConnectionCount();
  }

  /**
   * Renders one block and returns processed channel copies. Input channel
   * buffers are never mutated: the addon copies each plane before adding engine
   * output. Pass zero-filled planes when the engine is the only audio source.
   */
  process(channels: Float32Array[]): Float32Array[] {
    return this.native.process(channels);
  }

  /**
   * Renders one block in place like {@link process}, and also returns the monitor (cue/listen) bus output.
   * Audio thread only; each plane must not exceed the prepared block size.
   *
   * @returns `output` is the program mix and `monitor` the monitor bus, one buffer per plane.
   */
  processWithMonitor(channels: Float32Array[]): EngineProcessWithMonitorResult {
    return this.native.processWithMonitor(channels);
  }

  /**
   * Renders `channels` offline from the current transport position.
   *
   * Set `finalize: false` to render one chunk of a longer timeline; see
   * {@link RenderOfflineRequest.finalize} and {@link finishOfflineRender}.
   */
  renderOffline(request: RenderOfflineRequest): Float32Array[] {
    const fnName = 'renderOffline';
    const record = requestObject(fnName, request, 'request', true);
    if (Array.isArray(record.channels)) {
      record.channels.forEach((plane, index) => {
        if (plane instanceof Float32Array) {
          assertFiniteSamples(plane, true, `channels[${index}]`);
        }
      });
    }
    return this.native.renderOffline(
      record.channels,
      optionalNonZeroInteger(record.blockSize, 'blockSize'),
      record.finalize ?? true,
    );
  }

  /**
   * Ends a chunked offline render: releases every note the sequencer still
   * holds and flushes the PDC / alignment delay lines. Required after
   * `renderOffline({ finalize: false })`; the finalizing form does it itself.
   *
   * Skipping it leaves every note still sounding at the last chunk held. On an
   * engine-internal instrument the tail simply never releases; on a destination
   * marked external ({@link RealtimeEngine.setMidiDestinationExternal}) the
   * note-ons already left through the external MIDI queue, so the note-offs
   * emitted here are the only ones the receiving device will get and the notes
   * otherwise hang outside the engine.
   */
  finishOfflineRender(): void {
    this.native.finishOfflineRender();
  }

  /**
   * Renders `options.totalFrames` frames offline from the current transport position and returns the
   * interleaved result split into channels, with the measured integrated loudness.
   * A span over the memory budget is a `RangeError`; see {@link EngineBounceOptions.totalFrames}.
   */
  bounceOffline(options: EngineBounceOptions): EngineBounceResult {
    const { interleaved, numChannels, sampleRate, integratedLufs } = this.native.bounceOffline(
      bounceToNative(options),
    );
    return {
      channels: deinterleave(interleaved, numChannels),
      sampleRate,
      integratedLufs,
    };
  }

  /**
   * Render the source layer over the span and install it as the only clip.
   * Bakes every audio clip and hosted MIDI instrument through the lane strips,
   * lane inserts and group buses; the master strip, monitor bus, engine graph,
   * capture and metronome stay live and process the frozen clip once on
   * playback. The frozen clip replaces the whole clip set and every MIDI clip is
   * withdrawn in the same call, so nothing plays twice; live MIDI input still
   * reaches the instruments. `numChannels` above the prepared channel count
   * throws.
   */
  freezeOffline(options: EngineFreezeOptions): EngineFreezeResult {
    const fnName = 'freezeOffline';
    const native: Record<string, unknown> = { ...requestObject(fnName, options, 'options', true) };
    if (optionalNonZeroInteger(native.blockSize, 'blockSize') === undefined) {
      delete native.blockSize;
    }
    if (optionalId('clipId', native.clipId) === undefined) {
      delete native.clipId;
    }
    return this.native.freezeOffline(native);
  }

  /**
   * Drains pending process and error telemetry records, oldest first.
   *
   * @param maxRecords Most records to return (non-negative integer). Omitted: 1024. 0 returns an empty array.
   */
  drainTelemetry(maxRecords?: number): EngineTelemetry[] {
    return (this.native.drainTelemetry(maxRecords) as Array<Record<string, unknown>>).map(
      telemetryFromNative,
    );
  }

  /**
   * Drain pending stereo meter telemetry records published by the engine's meter tap.
   * Compatibility drain over the queue {@link drainMeterTelemetryWide} reads; use that one alone.
   */
  drainMeterTelemetry(maxRecords?: number): EngineMeterTelemetry[] {
    return (this.native.drainMeterTelemetry(maxRecords) as Array<Record<string, unknown>>).map(
      meterTelemetryFromNative,
    );
  }

  /**
   * Drain pending meter telemetry as per-plane (wide) records. This is the one
   * meter drain: a stereo target's left and right are planes 0 and 1. Use it
   * alone; {@link drainMeterTelemetry} consumes the same queue (every target's
   * records) and stays for compatibility.
   */
  drainMeterTelemetryWide(maxRecords?: number): EngineMeterTelemetryWide[] {
    return (this.native.drainMeterTelemetryWide(maxRecords) as Array<Record<string, unknown>>).map(
      meterTelemetryWideFromNative,
    );
  }

  /**
   * Per-insert gain reduction (dB, <= 0) of a meter target's strip from the last
   * rendered block, in combined insert order (pre-fader inserts, then post-fader).
   * Bypassed and non-dynamics inserts read 0; the meter record's `gainReductionDb`
   * is the minimum of these entries.
   *
   * @param targetId - Meter record target id (0 master, 1..32 lanes, 33..40 buses); the input monitor target yields `[]`
   * @throws If `targetId` is outside the encoded range
   */
  meterTargetInsertGainReduction(targetId: number): number[] {
    return this.native.meterTargetInsertGainReduction(targetId);
  }

  /** Queue a reset of the master integrated-loudness meter. */
  resetMasterLoudnessMeter(renderFrame?: number): void {
    this.native.resetMasterLoudnessMeter(resolveRenderFrame(renderFrame));
  }

  /**
   * Enable or configure per-target spectrum + vectorscope telemetry.
   *
   * @param intervalFrames - Minimum render-frame gap between published snapshots; `0` disables capture
   * @param bandCount - Requested FFT band resolution (1..64); changing it re-prepares the tap
   * @returns The band count actually applied
   */
  configureScopeTelemetry(intervalFrames: number, bandCount: number): number {
    return this.native.configureScopeTelemetry(intervalFrames, bandCount);
  }

  /** Drain pending spectrum + vectorscope telemetry records published by the engine's scope tap. */
  drainScopeTelemetry(maxRecords?: number): EngineScopeTelemetry[] {
    return this.native.drainScopeTelemetry(maxRecords);
  }

  /**
   * Push a live parameter value to the engine (immediate jump).
   *
   * @param paramId - Target parameter id
   * @param value - New value
   * @param renderFrame - Render-frame time to apply; omit for immediate (negative is refused)
   *
   * This value also becomes `paramId`'s base value: if an automation lane
   * later starts (and stops) driving `paramId`, the target reverts to this
   * value once that lane empties — see {@link setAutomationLane}.
   * A reserved mixer id (`0x4d58xxxx`) other than a master id names no strip and
   * throws a `RangeError`; resolve ids with the `resolve*AutomationId`
   * methods (`resolveStripAutomationId` for the master fader, pan and width).
   */
  setParameter(paramId: number, value: number, renderFrame?: number): void {
    this.native.setParameter(paramId, value, resolveRenderFrame(renderFrame));
  }

  /**
   * Push a live parameter value to the engine using a smoothed ramp. The
   * ramp's target (not its in-flight position) becomes `paramId`'s base
   * value, with the same restore-on-lane-release behavior as
   * {@link setParameter}.
   */
  setParameterSmoothed(paramId: number, value: number, renderFrame?: number): void {
    this.native.setParameterSmoothed(paramId, value, resolveRenderFrame(renderFrame));
  }

  /**
   * Set the ramp time (ms) of the engine-level smoother. It governs only
   * `setParameterSmoothed` on parameters bound through the automation engine.
   * Reserved engine targets — track and bus faders, pan and width, and insert and
   * instrument parameters — keep their own fixed smoothing whatever this is set
   * to, and MIDI-CC mappings apply their values directly. The default is 20 ms;
   * `0` makes the governed changes instant. The time is one time constant: about 63% of the
   * way at the set time.
   */
  setParamSmoothingMs(smoothingMs: number): void {
    this.native.setParamSmoothingMs(smoothingMs);
  }

  /**
   * Queue a track's solo/mute state. An armed PFL/AFL cue ({@link setTrackMonitorMode}) is folded
   * into the program output by `process()` / `processPrepared()` and kept separate by the
   * `WithMonitor` variants; offline renders, bounces and freezes never include it.
   *
   * @param trackId - Track id declared through {@link setTrackLanes}; an unknown id is a `RangeError`
   * @throws SonareError `InvalidState` after a timeline installed the lanes (declare them with {@link setTrackLanes})
   */
  setTrackSoloMute(
    trackId: number,
    state: { solo: boolean; mute: boolean },
    renderFrame?: number,
  ): void {
    const fnName = 'setTrackSoloMute';
    const request = requestObject(fnName, state, 'state', true);
    this.native.setSoloMute(
      this.laneIndexOf(trackId),
      requireBoolean(request.solo, 'solo'),
      requireBoolean(request.mute, 'mute'),
      resolveRenderFrame(renderFrame),
    );
  }

  /**
   * Schedule a per-track PFL/AFL monitor tap at a render-frame boundary.
   *
   * `process()` and `processPrepared()` fold the armed cue into the program output, so the
   * tapped lane is counted twice there; the `WithMonitor` variants keep the cue separate.
   * Offline renders, bounces and freezes never include the cue.
   *
   * @param trackId - Track id declared through {@link setTrackLanes}; an unknown id is a `RangeError`
   */
  setTrackMonitorMode(trackId: number, mode: EngineTrackMonitorMode, renderFrame?: number): void {
    this.native.setTrackMonitorMode(
      this.laneIndexOf(trackId),
      requireToken(mode, TRACK_MONITOR_MODE, 'mode'),
      resolveRenderFrame(renderFrame),
    );
  }

  private laneIndexOf(trackId: number): number {
    if (this.laneOrder === 'timeline') {
      throw invalidState(
        `the lanes were installed by a project timeline; declare them with setTrackLanes`,
      );
    }
    const index = this.laneOrder.indexOf(trackId);
    if (index < 0) {
      throw refuse('trackId must be the id of a track in this engine');
    }
    return index;
  }

  /**
   * Remove all registered parameters and release their backing strings. Use
   * before re-registering a parameter id (add() rejects duplicate ids). Not
   * realtime-safe.
   */
  clearParameters(): void {
    this.native.clearParameters();
  }

  /**
   * Replace the realtime MIDI clip snapshot. Events are absolute render-frame
   * UMP events compiled for the engine timeline.
   */
  setMidiClips(clips: ReadonlyArray<EngineMidiClipSchedule>): void {
    this.native.setMidiClips(midiClipsToNative(clips) as never);
  }

  /**
   * Bind a built-in synth to a realtime MIDI destination. Live note/CC commands
   * and scheduled MIDI clips routed to that destination render through it.
   */
  setBuiltinInstrument(
    destinationId?: number | null,
    config: Omit<BuiltinSynthConfig, 'destinationId'> = {},
  ): void {
    refuseBuiltinSynthOrdinals(config);
    refuseBuiltinPolyphonyZero(config);
    this.native.setBuiltinInstrument(destinationId ?? 0, withoutDestinationId(config));
  }

  /**
   * Bind the patch-driven NativeSynth to a realtime MIDI destination. `patch`
   * is a {@link SynthPatch} (a preset is named by its `preset` field, e.g.
   * `{ preset: 'sawLead' }`; see {@link synthPresetNames}), resolving exactly
   * like {@link Project.bounceWithSynthInstruments}. Live note/CC commands and
   * scheduled MIDI clips routed to that destination render through the synth.
   * Unknown preset names throw.
   *
   * An `engineMode: 'sample'` patch also carries the {@link SampleBank} its
   * keymap names. The engine takes a share of the bank, so it may be destroyed
   * right after this call; a sample patch bound without one renders silence.
   *
   * `useGmPrograms` follows incoming GM bank/program changes and routes
   * channel 10 through the GM drum map, exactly like the same-named bounce
   * binding key; the patch stays the fallback for any unsupported mapping.
   */
  setSynthInstrument(
    destinationId?: number | null,
    patch: Omit<SynthPatch, 'destinationId'> = {},
  ): void {
    this.native.setSynthInstrument(
      destinationId ?? 0,
      normalizeSynthInstrument(withoutDestinationId(patch)),
    );
  }

  /**
   * Load (parse) SoundFont 2 bytes into the engine so SF2 instruments can be
   * bound with {@link setSf2Instrument}. Replaces any previously loaded
   * SoundFont (already-bound SF2 instruments keep the SoundFont they were
   * created with); the input buffer is not referenced after the call.
   */
  loadSoundFont(data: Uint8Array): void {
    this.native.loadSoundFont(data);
  }

  /**
   * Bind a GS-compatible SoundFont player to a realtime MIDI destination, fed
   * by the engine's loaded SoundFont ({@link loadSoundFont}). Live note/CC
   * commands and scheduled MIDI clips routed to that destination render
   * through the player (16 MIDI channels, channel 10 drums, GS NRPN part
   * edits, GS/GM SysEx resets). Without a loaded SoundFont — or for programs
   * the SoundFont does not cover — notes play through the built-in
   * synthesizer GM fallback bank (the data-free floor).
   */
  setSf2Instrument(
    destinationId?: number | null,
    config: Omit<Sf2InstrumentConfig, 'destinationId'> = {},
  ): void {
    this.native.setSf2Instrument(destinationId ?? 0, withoutDestinationId(config));
  }

  /**
   * Clears the instrument bound to `destinationId` (default 0). Its sounding notes are released and the
   * output fades out over 10 ms. Do not call concurrently with {@link process}.
   */
  clearMidiInstrument(destinationId?: number | null): void {
    this.native.clearMidiInstrument(destinationId ?? 0);
  }

  /** Number of bound MIDI instruments. */
  get midiInstrumentCount(): number {
    return this.native.midiInstrumentCount();
  }

  /** Bind a 7/14-bit CC, RPN, or NRPN descriptor to a live parameter. */
  bindMidiCc(binding: ProjectMidiCcBinding): void {
    assertMidiCcBindingBytes(binding);
    this.native.bindMidiCcBinding(binding);
  }

  /** Removes every live MIDI CC / RPN / NRPN binding. */
  clearMidiCcBindings(): void {
    this.native.clearMidiCcBindings();
  }

  /** Number of live MIDI CC bindings. */
  get midiCcBindingCount(): number {
    return this.native.midiCcBindingCount();
  }

  /**
   * Replace the destination instrument's controller profile with a named preset
   * (see {@link controllerProfileNames}). Unknown names throw rather than
   * resolving to a default, and installing a profile drops every channel's
   * accumulated axis value. An instrument with nowhere to put a profile throws.
   */
  setControllerProfile(destinationId: number, presetName: string): void {
    this.native.setControllerProfile(destinationId, presetName);
  }

  /**
   * Add one {@link ControllerBinding} on top of the destination instrument's
   * current profile. Throws a `RangeError` when the axis is `'none'`, when a
   * `polyPressure` binding names a channel-level axis, or when the range or curve
   * is not finite, and an `InvalidState` `SonareError` when the table is full.
   */
  bindController(destinationId: number, binding: ControllerBinding): void {
    for (const key of ['input', 'axis'] as const) {
      if (typeof binding?.[key] !== 'string') {
        throw refuseType(`binding.${key} must be a name`);
      }
    }
    this.native.bindController(destinationId, binding);
  }

  /**
   * Drop every binding of the destination instrument's controller profile. The
   * instrument keeps a profile; it resolves nothing until something is bound.
   */
  clearControllerBindings(destinationId: number): void {
    this.native.clearControllerBindings(destinationId);
  }

  /**
   * Number of bindings the controller profile of the instrument at `destinationId` currently holds.
   */
  controllerBindingCount(destinationId: number): number {
    return this.native.controllerBindingCount(destinationId);
  }

  /**
   * Declare whether note-on velocity is expression for this instrument. When
   * off, every note is taken at full scale and the bound axes carry the
   * dynamics alone. There is no fixed default — each preset states it.
   */
  setControllerVelocityMeaningful(
    destinationId: number,
    controllerVelocityMeaningful: boolean,
  ): void {
    this.native.setControllerVelocityMeaningful(destinationId, controllerVelocityMeaningful);
  }

  /**
   * Reads back whether note-on velocity is expression for the instrument at `destinationId`
   * (see {@link setControllerVelocityMeaningful}).
   */
  controllerVelocityMeaningful(destinationId: number): boolean {
    return this.native.controllerVelocityMeaningful(destinationId);
  }

  /**
   * Say which note a value addressed to a whole MIDI channel belongs to when
   * several are sounding on it, for one per-note dimension
   * ({@link MPE_DIMENSIONS}, {@link NOTE_TRACKINGS}).
   *
   * Set per dimension because the useful answers differ: pressure following the
   * newest note while bend reaches every one is a real configuration, not a
   * mistake. MPE poses this question and declines to answer it, so this is a
   * choice rather than a rule — and it is read only inside an MPE zone, and
   * only while more than one note is sounding on the channel, which an MPE
   * sender avoids by giving each note its own member channel.
   *
   * Both arguments are required names; a number or an unknown spelling throws
   * rather than resolving to a default. A destination with no
   * instrument bound throws a `RangeError`; one whose instrument holds no
   * controller profile throws `NotSupported`.
   */
  setControllerNoteTracking(
    destinationId: number,
    dimension: MpeDimension,
    tracking: NoteTracking,
  ): void {
    this.native.setControllerNoteTracking(
      destinationId,
      requireName(dimension, 'dimension'),
      requireName(tracking, 'tracking'),
    );
  }

  /**
   * Read back {@link RealtimeEngine.setControllerNoteTracking} for one
   * dimension.
   */
  controllerNoteTracking(destinationId: number, dimension: MpeDimension): NoteTracking {
    return nameFromNative(
      this.native.controllerNoteTracking(destinationId, requireName(dimension, 'dimension')),
    ) as NoteTracking;
  }

  /**
   * Set how one MIDI channel of the destination's instrument treats a note-on
   * while another note on that channel is still held (see {@link Articulation}).
   * The mode is per channel, so slurring one part leaves the rest of the rack
   * polyphonic.
   *
   * The mode is a name from {@link ARTICULATIONS} (a number is a `TypeError`), and is
   * required: an omitted one would mean `'poly'`, which plays every note and
   * slurs none of them — indistinguishable from a request that took. A channel
   * above 15 and an out-of-range mode throw rather than being clamped. A
   * destination with no instrument bound throws a `RangeError`; one whose
   * instrument has no articulation of its own throws `NotSupported`, and the
   * two are deliberately different answers.
   */
  setArticulation(destinationId: number, channel: number, articulation: Articulation): void {
    this.native.setArticulation(destinationId, channel, requireName(articulation, 'articulation'));
  }

  /**
   * Set one part rig of the destination's instrument (see
   * {@link Project.setPartRig} for the request; an absent `part` selects every
   * part, and `inserts[].params` is an object). A destination with no
   * instrument bound throws a `RangeError`; an instrument without part rigs
   * (builtin, host callback) throws `NotSupported`.
   */
  setPartRig(request: PartRigRequest): void {
    const fnName = 'setPartRig';
    const record = requestObject(fnName, request, 'request', true);
    this.native.setPartRig(
      record.destinationId,
      isAbsent(record.part) ? PART_RIG_EVERY_PART : record.part,
      requireName(record.mode, 'mode') as PartRigMode,
      partRigInsertsJson(record.inserts as PartRigInsert[] | undefined),
    );
  }

  /**
   * Read back {@link RealtimeEngine.setArticulation} for one channel.
   */
  articulation(destinationId: number, channel: number): Articulation {
    return nameFromNative(this.native.articulation(destinationId, channel)) as Articulation;
  }

  /**
   * How many times a `'monoLegato'` continuation was asked for and refused, so
   * the note started a voice of its own instead. Counted rather than inferred:
   * a refusal sounds like an ordinary note, so nothing in the audio separates
   * "this engine declines legato" from "the mode was never set". Saturates at
   * 2^32 - 1 rather than wrapping.
   *
   * Refuses on the same terms as {@link RealtimeEngine.setArticulation} rather
   * than answering zero: a destination with no instrument bound throws
   * a `RangeError`, and one whose instrument has no articulation of its own
   * throws `NotSupported`. An instrument that never had an articulation has
   * refused nothing, and a zero here would read as "every slur took" — the
   * reading this counter exists to prevent.
   */
  legatoFallbackCount(destinationId: number): number {
    return this.native.legatoFallbackCount(destinationId);
  }

  /**
   * Install/replace a live non-destructive MIDI-FX insert for one destination.
   *
   * @param destinationId - Destination to insert on
   * @param config - Typed MIDI-FX chain configuration
   * @throws TypeError when `config` is not an object
   * @throws SonareError InvalidFormat when the core refuses a key or a field's value
   */
  setMidiFx(destinationId: number, config: MidiFxConfig): void {
    this.native.setMidiFx(destinationId, midiFxConfigJson(config));
  }

  /** Removes the live MIDI-FX insert of `destinationId` (default 0). */
  clearMidiFx(destinationId = 0): void {
    this.native.clearMidiFx(destinationId);
  }

  /** Enable the engine-owned live MIDI input source for a destination. */
  setMidiInputSource(destinationId = 0): void {
    this.native.setMidiInputSource(destinationId);
  }

  /** Disables the engine-owned live MIDI input source. */
  clearMidiInputSource(): void {
    this.native.clearMidiInputSource();
  }

  /** Live MIDI input events queued and not yet drained. */
  get midiInputPendingCount(): number {
    return this.native.midiInputPendingCount();
  }

  /**
   * Push a live MIDI note-on to the engine-owned MIDI input source.
   *
   * @param offsetInBlockSamples Samples into the next block drained, clamped to its last frame;
   *   negative reads as 0. Not an absolute clock position. Defaults to 0.
   */
  pushMidiInputNoteOn(
    group: number,
    channel: number,
    note: number,
    velocity: number,
    offsetInBlockSamples = 0,
  ): void {
    this.native.pushMidiInputNoteOn(group, channel, note, velocity, offsetInBlockSamples);
  }

  /**
   * Push a live MIDI note-off to the engine-owned MIDI input source.
   *
   * @param offsetInBlockSamples Samples into the next block drained, clamped to its last frame;
   *   negative reads as 0. Not an absolute clock position. Defaults to 0.
   */
  pushMidiInputNoteOff(
    group: number,
    channel: number,
    note: number,
    velocity = 0,
    offsetInBlockSamples = 0,
  ): void {
    this.native.pushMidiInputNoteOff(group, channel, note, velocity, offsetInBlockSamples);
  }

  /**
   * Push a live MIDI control change to the engine-owned MIDI input source.
   *
   * @param offsetInBlockSamples Samples into the next block drained, clamped to its last frame;
   *   negative reads as 0. Not an absolute clock position. Defaults to 0.
   */
  pushMidiInputCc(
    group: number,
    channel: number,
    controller: number,
    value: number,
    offsetInBlockSamples = 0,
  ): void {
    this.native.pushMidiInputCc(group, channel, controller, value, offsetInBlockSamples);
  }

  /**
   * Push a live MIDI pitch bend into the engine-owned MIDI input source.
   *
   * Requires {@link RealtimeEngine.setMidiInputSource}; without it the call is
   * refused. The engine drains the queue at block start, so the event lands in
   * the next block drained, `offsetInBlockSamples` frames in.
   *
   * @param group UMP group (0..15).
   * @param channel MIDI channel (0..15).
   * @param bend14 Unsigned 14-bit bend, centre 8192 (0..16383). A value outside
   *   that range is refused rather than wrapped into it.
   * @param offsetInBlockSamples Samples into the next block drained, clamped to its last frame;
   *   negative reads as 0. Not an absolute clock position. Defaults to 0.
   * @example
   * ```ts
   * const destinationId = 0;
   * engine.setMidiInputSource(destinationId);
   * engine.pushMidiInputPitchBend(0, 0, 16383); // a full bend up
   * ```
   */
  pushMidiInputPitchBend(
    group: number,
    channel: number,
    bend14: number,
    offsetInBlockSamples = 0,
  ): void {
    this.native.pushMidiInputPitchBend(group, channel, bend14, offsetInBlockSamples);
  }

  /**
   * Push a live MIDI channel pressure (aftertouch) into the engine-owned MIDI
   * input source. Requires {@link RealtimeEngine.setMidiInputSource}.
   *
   * @param group UMP group (0..15).
   * @param channel MIDI channel (0..15).
   * @param pressure 7-bit channel pressure (0..127), applying to every sounding
   *   note on the channel.
   * @param offsetInBlockSamples Samples into the next block drained, clamped to its last frame;
   *   negative reads as 0. Not an absolute clock position. Defaults to 0.
   */
  pushMidiInputChannelPressure(
    group: number,
    channel: number,
    pressure: number,
    offsetInBlockSamples = 0,
  ): void {
    this.native.pushMidiInputChannelPressure(group, channel, pressure, offsetInBlockSamples);
  }

  /**
   * Push a live MIDI polyphonic key pressure into the engine-owned MIDI input
   * source. Requires {@link RealtimeEngine.setMidiInputSource}.
   *
   * @param group UMP group (0..15).
   * @param channel MIDI channel (0..15).
   * @param note Key the pressure belongs to (0..127), so it reaches that voice
   *   alone rather than the whole channel.
   * @param pressure 7-bit key pressure (0..127).
   * @param offsetInBlockSamples Samples into the next block drained, clamped to its last frame;
   *   negative reads as 0. Not an absolute clock position. Defaults to 0.
   */
  pushMidiInputPolyPressure(
    group: number,
    channel: number,
    note: number,
    pressure: number,
    offsetInBlockSamples = 0,
  ): void {
    this.native.pushMidiInputPolyPressure(group, channel, note, pressure, offsetInBlockSamples);
  }

  /**
   * Queues a live MIDI note-on for a MIDI destination.
   *
   * @param destinationId MIDI destination id.
   * @param group UMP group, 0..15.
   * @param channel MIDI channel, 0..15.
   * @param note Note number, 0..127.
   * @param velocity 7-bit velocity, 0..127.
   * @param renderFrame Render-frame time to apply. Omitted: immediate. A negative value is refused.
   */
  pushMidiNoteOn(
    destinationId: number,
    group: number,
    channel: number,
    note: number,
    velocity: number,
    renderFrame?: number,
  ): void {
    this.native.pushMidiNoteOn(
      destinationId,
      group,
      channel,
      note,
      velocity,
      resolveRenderFrame(renderFrame),
    );
  }

  /**
   * Queues a live MIDI note-off for a MIDI destination.
   *
   * @param destinationId MIDI destination id.
   * @param group UMP group, 0..15.
   * @param channel MIDI channel, 0..15.
   * @param note Note number, 0..127.
   * @param velocity 7-bit release velocity, 0..127. Default 0.
   * @param renderFrame Render-frame time to apply. Omitted: immediate. A negative value is refused.
   */
  pushMidiNoteOff(
    destinationId: number,
    group: number,
    channel: number,
    note: number,
    velocity = 0,
    renderFrame?: number,
  ): void {
    this.native.pushMidiNoteOff(
      destinationId,
      group,
      channel,
      note,
      velocity,
      resolveRenderFrame(renderFrame),
    );
  }

  /**
   * Queue an immediate (live) MIDI control change to a MIDI destination. Values
   * are 7-bit; channel 0..15, group 0..15. `renderFrame` is the render-frame
   * time to apply; omit for immediate (negative is refused).
   */
  pushMidiCc(
    destinationId: number,
    group: number,
    channel: number,
    controller: number,
    value: number,
    renderFrame?: number,
  ): void {
    this.native.pushMidiCc(
      destinationId,
      group,
      channel,
      controller,
      value,
      resolveRenderFrame(renderFrame),
    );
  }

  /**
   * Queue an immediate (live) MIDI pitch bend to a MIDI destination.
   *
   * The bend travels as a single-word MIDI 1.0 UMP rather than as a 7-bit
   * scalar command, which is what carries it at its own width: no 7-bit
   * controller value can spell a 14-bit bend.
   *
   * @param destinationId MIDI destination id (clip/instrument destination).
   * @param group UMP group (0..15).
   * @param channel MIDI channel (0..15).
   * @param bend14 Unsigned 14-bit bend, centre 8192 (0..16383). A value outside
   *   that range is refused rather than wrapped into it.
   * @param renderFrame Render-frame time to apply; omit for immediate (negative is refused).
   * @example
   * ```ts
   * const destinationId = 0;
   * engine.pushMidiNoteOn(destinationId, 0, 0, 60, 100);
   * engine.pushMidiPitchBend(destinationId, 0, 0, 16383); // a full bend up
   * ```
   */
  pushMidiPitchBend(
    destinationId: number,
    group: number,
    channel: number,
    bend14: number,
    renderFrame?: number,
  ): void {
    this.native.pushMidiPitchBend(
      destinationId,
      group,
      channel,
      bend14,
      resolveRenderFrame(renderFrame),
    );
  }

  /**
   * Queue an immediate (live) MIDI channel pressure (aftertouch) to a MIDI
   * destination.
   *
   * @param destinationId MIDI destination id (clip/instrument destination).
   * @param group UMP group (0..15).
   * @param channel MIDI channel (0..15).
   * @param pressure 7-bit channel pressure (0..127), applying to every sounding
   *   note on the channel.
   * @param renderFrame Render-frame time to apply; omit for immediate (negative is refused).
   */
  pushMidiChannelPressure(
    destinationId: number,
    group: number,
    channel: number,
    pressure: number,
    renderFrame?: number,
  ): void {
    this.native.pushMidiChannelPressure(
      destinationId,
      group,
      channel,
      pressure,
      resolveRenderFrame(renderFrame),
    );
  }

  /**
   * Queue an immediate (live) MIDI polyphonic key pressure to a MIDI
   * destination.
   *
   * @param destinationId MIDI destination id (clip/instrument destination).
   * @param group UMP group (0..15).
   * @param channel MIDI channel (0..15).
   * @param note Key the pressure belongs to (0..127), so it reaches that voice
   *   alone rather than the whole channel.
   * @param pressure 7-bit key pressure (0..127).
   * @param renderFrame Render-frame time to apply; omit for immediate (negative is refused).
   */
  pushMidiPolyPressure(
    destinationId: number,
    group: number,
    channel: number,
    note: number,
    pressure: number,
    renderFrame?: number,
  ): void {
    this.native.pushMidiPolyPressure(
      destinationId,
      group,
      channel,
      note,
      pressure,
      resolveRenderFrame(renderFrame),
    );
  }

  /**
   * Queue a MIDI panic (all-notes-off) releasing every sounding note.
   * `renderFrame` is the render-frame time to apply; omit for immediate (negative is refused).
   */
  pushMidiPanic(renderFrame?: number): void {
    this.native.pushMidiPanic(resolveRenderFrame(renderFrame));
  }

  /**
   * Queue an immediate (live) MIDI SysEx message to a MIDI destination. `data`
   * is the full SysEx frame including the leading 0xF0 and trailing 0xF7, and
   * must be 1..512 bytes. `renderFrame` is the render-frame time to apply
   * (omit for immediate). Throws a `RangeError` when the destination
   * instrument cannot prepare the SysEx (retrying cannot help), and
   * `OutOfMemory` when the payload slots or the command queue are full
   * (retry after a processed block).
   */
  pushMidiSysex(destinationId: number, data: Uint8Array, renderFrame?: number): void {
    this.native.pushMidiSysex(destinationId, data, resolveRenderFrame(renderFrame));
  }

  /**
   * Queue an immediate (live) raw UMP message to a MIDI destination. `words` is
   * 1 to 4 words, most significant first, and its length must match the message
   * type of `words[0]`. MIDI 2.0 channel-voice messages (MT 0x4) arrive at full
   * width; SysEx7 / data messages (MT 0x3 / 0x5) are refused, use
   * {@link pushMidiSysex}. Throws when the slot ring or command queue is full
   * (retry after a process block). `renderFrame` is the render-frame time to
   * apply (omit for immediate).
   */
  pushMidiUmp(destinationId: number, words: UmpWords, renderFrame?: number): void {
    this.native.pushMidiUmp(destinationId, words, resolveRenderFrame(renderFrame));
  }

  /**
   * Queue a live MIDI 2.0 Program Change to a MIDI destination. The bank
   * travels in the same message and is applied only when `bank` is given
   * (`null` and absent both mean no bank change).
   * Program, bank MSB and LSB are 7-bit; group and channel 0..15. The message
   * takes the receive path a wire Program Change takes. Which receivers select
   * programs: the SoundFont player (`setSf2Instrument`) selects bank and
   * program under its own RX PROGRAM CHANGE / RX BANK SELECT gates; a
   * NativeSynth bound with `useGmPrograms: true` (`setSynthInstrument`)
   * resolves each channel's voice from the bank and program; a NativeSynth
   * whose patch plays the GM kit (the `drumKit` preset, or a percussion patch
   * with `gmKit` on) takes the program as the GS drum-set select on every
   * channel. Every other receiver accepts the message and ignores it: the
   * built-in synth, any other NativeSynth patch, a part with RX PROGRAM CHANGE
   * off, and an MPE member channel. Throws when the slot ring or command queue is full
   * (retry after a process block). `renderFrame` is the render-frame time to
   * apply (omit for immediate); a future-dated op can be evicted by the
   * pending-command bank like any future command.
   */
  pushMidiProgram(
    destinationId: number,
    group: number,
    channel: number,
    program: number,
    bank?: { msb: number; lsb: number } | null,
    renderFrame?: number,
  ): void {
    const { bankValid, msb, lsb } = bankFields('pushMidiProgram', bank);
    this.native.pushMidiProgram(
      destinationId,
      group,
      channel,
      program,
      bankValid,
      msb,
      lsb,
      resolveRenderFrame(renderFrame),
    );
  }

  /**
   * Push a live MIDI 2.0 Program Change to the engine-owned MIDI input source.
   * The rules match {@link pushMidiProgram}. `offsetInBlockSamples` is
   * an offset into the next block drained (clamped to its last frame, negative reads as 0), not
   * an absolute clock position.
   */
  pushMidiInputProgram(
    group: number,
    channel: number,
    program: number,
    bank?: { msb: number; lsb: number } | null,
    offsetInBlockSamples = 0,
  ): void {
    const { bankValid, msb, lsb } = bankFields('pushMidiInputProgram', bank);
    this.native.pushMidiInputProgram(
      group,
      channel,
      program,
      bankValid,
      msb,
      lsb,
      offsetInBlockSamples,
    );
  }

  /**
   * Push one raw UMP message (1 to 4 words) to the engine-owned MIDI input
   * source. The message rules match {@link pushMidiUmp}. `offsetInBlockSamples` is
   * an offset into the next block drained (clamped to its last frame, negative reads as 0), not
   * an absolute clock position.
   */
  pushMidiInputUmp(words: UmpWords, offsetInBlockSamples = 0): void {
    this.native.pushMidiInputUmp(words, offsetInBlockSamples);
  }

  /**
   * Routes a MIDI destination (a track lane) to the external-MIDI output queue
   * instead of the internal instrument rack, so the track drives an external
   * device. Its sequenced events are buffered for {@link drainExternalMidi}.
   * Clearing it restores internal-synth playback. Control-thread only. The
   * change takes effect at the next processed block; switching a destination's
   * route first releases its notes and resets its controllers through the old
   * route, and drops its pending MIDI-FX events. Single writer.
   */
  setMidiDestinationExternal(destinationId: number, midiDestinationExternal: boolean): void {
    this.native.setMidiDestinationExternal(destinationId, midiDestinationExternal);
  }

  /**
   * Enables forwarding MIDI clock (0xF8) and transport (start/continue/stop)
   * bytes to the external output queue, drained without a `destinationId`,
   * so external gear stays tempo-synced. Off by default; control-thread only.
   */
  setExternalMidiClockEnabled(externalMidiClockEnabled: boolean): void {
    this.native.setExternalMidiClockEnabled(externalMidiClockEnabled);
  }

  /** Number of external-MIDI events dropped because the output queue was full. */
  get externalMidiDroppedCount(): number {
    return this.native.externalMidiDroppedCount();
  }

  /** Cumulative page misses dropped because the native bounded request queue was full. */
  get clipPageRequestOverflowCount(): number {
    return this.native.clipPageRequestOverflowCount();
  }

  /** Cumulative warp-stretch requests dropped because the native queue was full. */
  get warpStretchOverflowCount(): number {
    return this.native.warpStretchOverflowCount();
  }

  /**
   * Sets the number of concurrent time-stretch voices. `voices` must be an
   * integer in `[0, 64]`; a non-integer, negative, or larger value throws and
   * leaves the capacity unchanged. Default is 8. Capacity 0 disables
   * time-stretch, so every warped clip plays resampled instead and none of
   * that counts toward {@link warpStretchOverflowCount}. A change applied
   * while the engine is running restarts the splice state of any clip
   * stretching through a voice at that moment. Control-thread only.
   */
  setWarpVoiceCapacity(voices: number): void {
    this.native.setWarpVoiceCapacity(voices);
  }

  /** Reads the current time-stretch voice capacity (default 8). */
  get warpVoiceCapacity(): number {
    return this.native.warpVoiceCapacity();
  }

  /**
   * Drains queued external-MIDI events, already lowered to MIDI 1.0 byte
   * messages so the host can write them straight to an output port. Returns one
   * entry per lowered message; transport/clock bytes carry no
   * `destinationId`. `maxRecords` caps the number of output
   * events returned — the shared unit across every surface. Events past the cap
   * stay queued for the next call (lossless); call again to drain the rest.
   *
   * `maxRecords` must be 0 (drain nothing) or at least 4, the most MIDI 1.0
   * messages a single queued event can lower to (a MIDI 2.0 registered or
   * assignable controller becomes CC 101/100 or 99/98 plus Data Entry 6/38):
   * a smaller budget could never
   * consume a record, so it is rejected with a `RangeError` rather than
   * returning an empty array while the queue keeps growing. A negative or
   * fractional value is likewise a `RangeError`, a non-number a `TypeError`.
   */
  drainExternalMidi(maxRecords?: number): ExternalMidiEvent[] {
    return this.native
      .drainExternalMidi(maxRecords)
      .map(
        ({ destinationId, ...event }: Required<ExternalMidiEvent>): ExternalMidiEvent =>
          destinationId === EVERY_PORT_DESTINATION ? event : { destinationId, ...event },
      );
  }

  /** Read the current engine transport state (playing/position/ppq/tempo). */
  transportState(): EngineTransportState {
    const { isPlaying: _legacy, ...state } = this.native.getTransportState();
    return state as EngineTransportState;
  }

  /** Releases the native engine handle. Idempotent; the engine is unusable afterwards. */
  destroy(): void {
    if (this.disposed) {
      return;
    }
    this.disposed = true;
    this.native.destroy();
  }

  /**

   * Releases the native handle; lets `using` free it automatically (needs TypeScript 5.2+

   * or a runtime with native explicit resource management; Node 22 does not parse `using`).

   */
  [Symbol.dispose](): void {
    this.destroy();
  }
}

/**
 * Engine C-ABI version this package's TypeScript was built against; an internal
 * mirror the ABI tooling checks, not part of the package entry.
 */
export const EXPECTED_ENGINE_ABI_VERSION = 3;

/** Most lanes {@link RealtimeEngine.setTrackLanes} accepts (`SONARE_ENGINE_MAX_TRACK_LANES`). */
export const MAX_TRACK_LANES = 32;

/** ABI version of the realtime engine command/telemetry layout in the loaded addon. */
export function engineAbiVersion(): number {
  return addon.engineAbiVersion();
}

/** ABI version of the realtime voice-changer config layout in the loaded addon. */
export function voiceChangerAbiVersion(): number {
  return addon.voiceChangerAbiVersion();
}
