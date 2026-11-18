import type { ClipPageSampleRequest } from './clip_page_streamer.js';
import {
  AUTOMATION_CURVE_VALUES,
  automationCurveCode,
  panLawCode,
  panModeCode,
  sendTimingCode,
  sidechainCheckFromCode,
  sidechainSourceKindCode,
  trackMonitorModeCode,
} from './codes.js';
import { ErrorCode, SonareError } from './errors.js';
import { CHANNEL_LAYOUT_VALUES } from './internal/channel_layout.js';
import {
  bindNativeClipPageProvider,
  nativeClipPageProviderOf,
} from './internal/clip_page_native.js';
import { eqBandJson } from './internal/eq_band_json.js';
import { externalMidiEvent } from './internal/external_midi.js';
import { assertHandleLive, releasedHandleError } from './internal/handle_guard.js';
import { assignMarkerIds } from './internal/marker_ids.js';
import { midiFxConfigJson } from './internal/midi_fx_config.js';
import { mixSceneBusToNative, mixSceneStripToNative } from './internal/mix_scene.js';
import { deinterleavePlanes } from './internal/pod_config.js';
import { refuseBuiltinSynthOrdinals } from './internal/synth_tokens.js';
import { getSonareModule } from './module_state.js';
import type {
  Articulation,
  BuiltinSynthConfig,
  ControllerBinding,
  MpeDimension,
  NoteTracking,
  PartRigRequest,
  ProjectMidiCcBinding,
  Sf2InstrumentConfig,
  SynthPatch,
} from './project.js';
import type { ProjectTimeline } from './project_class.js';
import {
  normalizePartRig,
  normalizeSynthInstrument,
  projectTimelineNativeId,
} from './project_internal.js';
import type {
  EqBand,
  MixSceneBusInput,
  MixSceneStripInput,
  PanLaw,
  PanMode,
  SidechainCheck,
  SurroundPan,
  UmpWords,
} from './public_types.js';
import type {
  CaptureOptions,
  ClipPageProviderOptions,
  EngineAutomationPoint,
  EngineBounceOptions,
  EngineBounceResult,
  EngineBus,
  EngineCapabilities,
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
  EngineTelemetryError,
  EngineTelemetryType,
  EngineTempoSegment,
  EngineTimeSignatureSegment,
  EngineTrackLane,
  EngineTrackMonitorMode,
  EngineTrackSend,
  EngineTransportState,
  ExternalMidiEvent,
  MidiFxConfig,
  RealtimeEngineConfig,
  RenderOfflineRequest,
  SidechainSource,
  StripRef,
} from './public_types_engine.js';
import type {
  WasmClipPageProvider,
  WasmEngineBus,
  WasmEngineClip,
  WasmEngineMeterTelemetry,
  WasmEngineMeterTelemetryWide,
  WasmEngineTrackSend,
  WasmRealtimeEngine,
} from './sonare.js';
import {
  assertFloatValue,
  assertSurroundPan,
  optionalBoolean,
  requestObject,
  requireBoolean,
  resolveSampleBound,
  resolveRenderFrame as resolveWireRenderFrame,
} from './validation.js';

export type {
  CaptureOptions,
  CapturePunch,
  ClipPageProviderOptions,
  EngineAutomationPoint,
  EngineBounceOptions,
  EngineBounceResult,
  EngineBus,
  EngineCapabilities,
  EngineCaptureSource,
  EngineCaptureStatus,
  EngineClip,
  EngineDither,
  EngineFreezeOptions,
  EngineFreezeResult,
  EngineGraphConnection,
  EngineGraphMix,
  EngineGraphNode,
  EngineGraphNodeType,
  EngineGraphParameterBinding,
  EngineGraphSpec,
  EngineLoop,
  EngineMarker,
  EngineMeterTelemetry,
  EngineMeterTelemetryWide,
  EngineMetronomeConfig,
  EngineMidiClipSchedule,
  EngineMidiEvent,
  EngineParameterInfo,
  EngineProcessWithMonitorResult,
  EngineScopeTelemetry,
  EngineTelemetry,
  EngineTelemetryError,
  EngineTelemetryType,
  EngineTempoSegment,
  EngineTimeSignatureSegment,
  EngineTrackLane,
  EngineTrackMonitorMode,
  EngineTrackSend,
  EngineTransportState,
  ExternalMidiEvent,
  MarkerKind,
  MidiFxConfig,
  RealtimeEngineConfig,
  RenderOfflineRequest,
  SidechainSource,
  StripRef,
} from './public_types_engine.js';

/** The native layer takes an absent frame as `undefined`; the shared resolver spells it -1. */
function resolveRenderFrame(
  fnName: string,
  renderFrame: number | null | undefined,
): number | undefined {
  const frame = resolveWireRenderFrame(fnName, renderFrame);
  return frame < 0 ? undefined : frame;
}

/** Engine ABI version this wrapper was written against; an internal mirror, not part of any package entry. */
export const EXPECTED_ENGINE_ABI_VERSION = 3;

/** Most lanes {@link RealtimeEngine.setTrackLanes} accepts (`SONARE_ENGINE_MAX_TRACK_LANES`). */
export const MAX_TRACK_LANES = 32;

const UMP_WORD_MIN = -0x80000000;
const UMP_WORD_MAX = 0xffffffff;

// A word may be spelled `(0x4 << 28) | …`, which is a signed int once bit 31 is
// set, so the signed 32-bit range is accepted alongside the unsigned one.
function assertUmpWords(fnName: string, words: UmpWords): UmpWords {
  if (!(words instanceof Uint32Array) && !Array.isArray(words)) {
    throw new TypeError(`${fnName}: words must be a Uint32Array or a number array`);
  }
  if (words.length < 1 || words.length > 4) {
    throw new RangeError(`${fnName}: words must hold 1 to 4 words`);
  }
  for (let i = 0; i < words.length; i++) {
    const word = words[i];
    if (
      typeof word !== 'number' ||
      !Number.isInteger(word) ||
      word < UMP_WORD_MIN ||
      word > UMP_WORD_MAX
    ) {
      throw new RangeError(`${fnName}: words[${i}] must be an integer 32-bit word`);
    }
  }
  return words;
}

const MARKER_KINDS = { marker: 0, text: 1, lyric: 2, cuePoint: 3, keySignature: 4 } as const;
const WARP_MODES = { off: 0, repitch: 1, tempoSync: 2, timeStretch: 3 } as const;
const DITHERS = { rpdf: 1, tpdf: 2, noiseShaped: 3 } as const;
const GRAPH_NODE_TYPES = { passThrough: 0, gain: 1 } as const;
const GRAPH_MIXES = { replace: 0, add: 1 } as const;
const CAPTURE_SOURCES = { output: 0, input: 1 } as const;
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

/** Resolves a closed-set string token; an ordinal is a `TypeError` and an unknown spelling a `RangeError`. */
function tokenCode(
  fnName: string,
  argName: string,
  value: unknown,
  table: Readonly<Record<string, number>>,
): number {
  const accepted = Object.keys(table)
    .map((key) => `'${key}'`)
    .join(', ');
  if (typeof value !== 'string') {
    throw new TypeError(`${fnName}: ${argName} must be one of ${accepted}`);
  }
  // biome-ignore lint/suspicious/noPrototypeBuiltins: Object.hasOwn is newer than the ES2020 target.
  if (!Object.prototype.hasOwnProperty.call(table, value)) {
    throw new RangeError(`${fnName}: ${argName} must be one of ${accepted} (got '${value}')`);
  }
  return table[value];
}

function tokenName<T extends string>(table: Readonly<Record<T, number>>, ordinal: number): T {
  const name = (Object.keys(table) as T[]).find((key) => table[key] === ordinal);
  if (name === undefined) {
    throw new RangeError(`unknown ordinal ${ordinal}`);
  }
  return name;
}

/** Runs an existing token resolver after refusing an ordinal, which is never a public spelling. */
function noOrdinal(fnName: string, argName: string, value: unknown): void {
  if (typeof value === 'number') {
    throw new TypeError(`${fnName}: ${argName} must be a string token, not an ordinal`);
  }
}

/** An optional positive integer count: absent stays `undefined`, and an explicit `0` is refused. */
function optionalCount(fnName: string, argName: string, value: unknown): number | undefined {
  if (value === undefined || value === null) {
    return undefined;
  }
  if (typeof value !== 'number') {
    throw new TypeError(`${fnName}: ${argName} must be a number`);
  }
  if (!Number.isFinite(value)) {
    throw new RangeError(`${fnName}: ${argName} must be a finite number`);
  }
  if (value === 0) {
    throw new RangeError(`${fnName}: ${argName} must not be 0 (omit it for the default)`);
  }
  if (value < 0 || !Number.isInteger(value)) {
    throw new RangeError(`${fnName}: ${argName} must be a positive integer`);
  }
  return value;
}

/** An optional id that names a bus: absent routes to the master mix, so an explicit `0` is refused. */
function optionalBusId(fnName: string, argName: string, value: unknown): number {
  if (value === undefined || value === null) {
    return 0;
  }
  if (typeof value !== 'number') {
    throw new TypeError(`${fnName}: ${argName} must be a number`);
  }
  if (value === 0) {
    throw new RangeError(
      `${fnName}: ${argName} must not be 0 (omit it to route to the master mix)`,
    );
  }
  return value;
}

function requireObject(fnName: string, argName: string, value: unknown): void {
  if (typeof value !== 'object' || value === null || Array.isArray(value)) {
    throw new TypeError(`${fnName}: ${argName} must be an object`);
  }
}

/** An omitted size is `0`, which the native engine reads as the library default. */
interface ResolvedEngineConfig {
  sampleRate: number;
  maxBlockSize: number;
  commandCapacity: number;
  telemetryCapacity: number;
  maxChannels: number;
}

function normalizeEngineConfig(
  fnName: 'RealtimeEngine' | 'prepare',
  config: unknown,
): ResolvedEngineConfig {
  const request = requestObject(fnName, config as RealtimeEngineConfig, 'config', true);
  const sampleRate = request.sampleRate;
  if (typeof sampleRate !== 'number') {
    throw new TypeError(`${fnName}: sampleRate must be a number`);
  }
  if (!Number.isFinite(sampleRate) || sampleRate <= 0) {
    throw new RangeError(`${fnName}: sampleRate must be a positive finite number`);
  }
  return {
    sampleRate,
    maxBlockSize: optionalCount(fnName, 'maxBlockSize', request.maxBlockSize) ?? 0,
    commandCapacity: optionalCount(fnName, 'commandCapacity', request.commandCapacity) ?? 0,
    telemetryCapacity: optionalCount(fnName, 'telemetryCapacity', request.telemetryCapacity) ?? 0,
    maxChannels: optionalCount(fnName, 'maxChannels', request.maxChannels) ?? 0,
  };
}

function normalizeRenderOfflineRequest(request: RenderOfflineRequest): {
  channels: Float32Array[];
  blockSize: number | undefined;
  finalize: boolean;
} {
  requestObject('renderOffline', request, 'request', true);
  if (optionalBoolean('renderOffline', request.validate, 'validate', true)) {
    for (const [plane, channel] of (request.channels ?? []).entries()) {
      for (let i = 0; i < (channel?.length ?? 0); i++) {
        if (!Number.isFinite(channel[i])) {
          throw new RangeError(
            `renderOffline: channels[${plane}] contains NaN or Inf at index ${i}`,
          );
        }
      }
    }
  }
  return {
    channels: request.channels,
    blockSize: optionalCount('renderOffline', 'blockSize', request.blockSize),
    finalize: optionalBoolean('renderOffline', request.finalize, 'finalize', true),
  };
}

/** Engine ABI agreement and host transport support; internal to the engine and worklet node. */
export function engineCapabilities(): EngineCapabilities {
  const abiVersion = getSonareModule().engineAbiVersion();
  const sharedArrayBuffer = typeof globalThis.SharedArrayBuffer === 'function';
  const atomics = typeof globalThis.Atomics === 'object';
  const audioWorklet =
    typeof AudioWorkletNode !== 'undefined' ||
    typeof (globalThis as typeof globalThis & { AudioWorkletProcessor?: unknown })
      .AudioWorkletProcessor !== 'undefined';
  return {
    engineAbiVersion: abiVersion,
    expectedEngineAbiVersion: EXPECTED_ENGINE_ABI_VERSION,
    abiCompatible: abiVersion === EXPECTED_ENGINE_ABI_VERSION,
    sharedArrayBuffer,
    atomics,
    audioWorklet,
    mode: sharedArrayBuffer && atomics ? 'sab' : 'postMessage',
  };
}

/** The clip shape the embind layer reads: the warp mode travels as its ordinal, a page provider as its embind instance. */
function nativeClip(clip: EngineClip): WasmEngineClip {
  const { warpMode, pageProvider, ...rest } = clip;
  return {
    ...rest,
    ...(warpMode === undefined
      ? {}
      : { warpMode: tokenCode('setClips', 'warpMode', warpMode, WARP_MODES) as 0 | 1 | 2 | 3 }),
    ...(pageProvider === undefined
      ? {}
      : { pageProvider: nativeClipPageProviderOf(pageProvider) as WasmEngineClip['pageProvider'] }),
  };
}

function nativeMarker(marker: EngineMarker): Record<string, unknown> {
  const { kind, ...rest } = marker;
  return kind === undefined
    ? rest
    : { ...rest, kind: tokenCode('setMarkers', 'kind', kind, MARKER_KINDS) };
}

function markerFromNative(marker: {
  id: number;
  ppq: number;
  name?: string;
  kind?: number;
  keyFifths?: number;
  keyMinor?: boolean;
}): EngineMarker {
  return { ...marker, kind: tokenName(MARKER_KINDS, marker.kind ?? 0) };
}

function curveFromNative(ordinal: number): NonNullable<EngineAutomationPoint['curveToNext']> {
  const entry = Object.entries(AUTOMATION_CURVE_VALUES).find(([, value]) => value === ordinal);
  if (entry === undefined) {
    throw new RangeError(`unknown automation curve ordinal ${ordinal}`);
  }
  return entry[0] as NonNullable<EngineAutomationPoint['curveToNext']>;
}

function nativeAutomationPoint(fnName: string, point: EngineAutomationPoint) {
  noOrdinal(fnName, 'curveToNext', point.curveToNext);
  const { curveToNext, ...rest } = point;
  return curveToNext === undefined
    ? rest
    : { ...rest, curveToNext: automationCurveCode(curveToNext) };
}

function parameterInfoFromNative(info: {
  id: number;
  name: string;
  unit: string;
  minValue: number;
  maxValue: number;
  defaultValue: number;
  rtSafe: boolean;
  defaultCurve: number;
}): Required<EngineParameterInfo> {
  return { ...info, defaultCurve: curveFromNative(info.defaultCurve) };
}

function telemetryFromNative(record: {
  type: number;
  error: number;
  renderFrame: number;
  timelineSample: number;
  audibleTimelineSample: number;
  graphLatencySamplesQ8: number;
  value: number;
}): EngineTelemetry {
  const type = TELEMETRY_TYPES[record.type];
  const error = TELEMETRY_ERRORS[record.error];
  if (type === undefined || error === undefined) {
    throw new RangeError(`unknown telemetry ordinal (type ${record.type}, error ${record.error})`);
  }
  return {
    type,
    error,
    renderFrame: record.renderFrame,
    timelineSample: record.timelineSample,
    audibleTimelineSample: record.audibleTimelineSample,
    graphLatencySamples: record.graphLatencySamplesQ8 / 256,
    value: record.value,
  };
}

function meterTelemetryFromNative(record: WasmEngineMeterTelemetry): EngineMeterTelemetry {
  const { truePeakDbL, truePeakDbR, maxTruePeakDb, ...rest } = record;
  return {
    ...rest,
    truePeakDbtpL: truePeakDbL,
    truePeakDbtpR: truePeakDbR,
    maxTruePeakDbtp: maxTruePeakDb,
  };
}

function meterTelemetryWideFromNative(
  record: WasmEngineMeterTelemetryWide,
): EngineMeterTelemetryWide {
  const { channelCount, truePeakDb, maxTruePeakDb, ...rest } = record;
  return {
    ...rest,
    numChannels: channelCount,
    truePeakDbtp: truePeakDb,
    maxTruePeakDbtp: maxTruePeakDb,
  };
}

/** A strip address resolved against the engine: a track id or the master strip. */
type StripTarget = { master: false; trackId: number } | { master: true };

function stripTarget(fnName: string, ref: StripRef): StripTarget {
  if (typeof ref === 'number') {
    return { master: false, trackId: ref };
  }
  if (typeof ref !== 'string') {
    throw new TypeError(`${fnName}: ref must be a track id or 'master'`);
  }
  if (ref === 'master') {
    return { master: true };
  }
  throw new RangeError(`${fnName}: ref must be a track id or 'master' (got '${ref}')`);
}

function trackOnly(fnName: string, ref: StripRef): number {
  const target = stripTarget(fnName, ref);
  if (target.master) {
    throw new RangeError(`${fnName}: the master strip has no such setter`);
  }
  return target.trackId;
}

function sidechainSourceArg(
  fnName: 'setStripSidechain' | 'setBusSidechain' | 'canSetStripSidechain' | 'canSetBusSidechain',
  source: SidechainSource,
): { kind: number; id: number } {
  requestObject(fnName, source, 'source', true);
  noOrdinal(fnName, 'kind', source.kind);
  if (typeof source.id !== 'number') {
    throw new TypeError(`${fnName}: source.id must be a number`);
  }
  if (source.id === 0) {
    throw new RangeError(`${fnName}: source.id must not be 0 (pass null to remove the binding)`);
  }
  return { kind: sidechainSourceKindCode(source.kind), id: source.id };
}

function bypassState(
  fnName: 'setStripInsertBypassed' | 'setBusInsertBypassed',
  state: { bypassed: boolean; resetOnBypass?: boolean },
): { bypassed: boolean; resetOnBypass: boolean } {
  requestObject(fnName, state, 'state', true);
  return {
    bypassed: requireBoolean(fnName, state.bypassed, 'bypassed'),
    resetOnBypass: optionalBoolean(fnName, state.resetOnBypass, 'resetOnBypass', false),
  };
}

function insertParam(
  fnName:
    | 'setStripInsertParam'
    | 'setBusInsertParam'
    | 'applyStripInsertParamNow'
    | 'applyBusInsertParamNow'
    | 'restoreStripInsertParam'
    | 'restoreBusInsertParam',
  param: { name: string; value: number },
): { name: string; value: number } {
  requestObject(fnName, param, 'param', true);
  if (typeof param.name !== 'string') {
    throw new TypeError(`${fnName}: param.name must be a string`);
  }
  if (typeof param.value !== 'number') {
    throw new TypeError(`${fnName}: param.value must be a number`);
  }
  return param;
}

function programBank(
  fnName: 'pushMidiProgram' | 'pushMidiInputProgram',
  bank: { msb: number; lsb: number } | null | undefined,
): { bankValid: boolean; bankMsb: number; bankLsb: number } {
  if (bank === undefined || bank === null) {
    return { bankValid: false, bankMsb: 0, bankLsb: 0 };
  }
  requestObject(fnName, bank, 'bank', true);
  return { bankValid: true, bankMsb: bank.msb, bankLsb: bank.lsb };
}

function sceneStripJson(fnName: string, strip: MixSceneStripInput): string {
  return JSON.stringify({ strips: [mixSceneStripToNative(fnName, strip)] });
}

function sceneBusJson(fnName: string, bus: MixSceneBusInput): string {
  return JSON.stringify({ buses: [mixSceneBusToNative(fnName, bus)] });
}

function bandJson(fnName: string, band: unknown): string {
  requireObject(fnName, 'band', band);
  return eqBandJson(band as object);
}

/**
 * Sample-accurate realtime engine: clips, MIDI instruments, a track mixer and
 * automation, rendered block by block or offline.
 *
 * Strips are addressed by a {@link StripRef}: a number is a track id and
 * `'master'` is the master strip; buses have their own `setBus*` family.
 *
 * Errors follow one rule across the methods:
 * - A call refused for its own argument (an out-of-range value, an unknown or
 *   wrong-kind track, bus, insert, destination or parameter id or name, a
 *   malformed patch or strip, an unknown request key or `engineParams` key)
 *   throws a `RangeError` (`TypeError` for a value of the wrong type or an
 *   unknown request key), never a `SonareError`.
 * - A call refused because of state earlier calls left (a full binding table,
 *   rack or pool, no master strip configured, a queued SysEx the new instrument
 *   cannot prepare) throws a `SonareError` with `ErrorCode.InvalidState`; a
 *   call on a destroyed engine does too.
 * - A full command queue or payload ring, and a failed allocation, throw a
 *   `SonareError` with `ErrorCode.OutOfMemory`; a feature compiled out of the
 *   build throws `ErrorCode.NotSupported`.
 */
export class RealtimeEngine {
  private nativeHandle: WasmRealtimeEngine;
  private released = false;
  /** Lowest id `setMarkers` may assign; never moves back, so an id is not reused. */
  private nextMarkerId = 1;

  private get native(): WasmRealtimeEngine {
    assertHandleLive(this.released, 'RealtimeEngine');
    return this.nativeHandle;
  }
  private sampleRate: number;
  private metronomeSpelling: 'samples' | 'sec' = 'samples';
  private captureBuffer: { numChannels: number; capacityFrames: number } | undefined;
  // Track ids in lane order, as the last `setTrackLanes` declared them; unknown once a
  // project timeline installed its own lanes.
  private laneOrder: number[] | undefined = [];

  /**
   * @param config - Engine settings; `sampleRate` is required and an unknown key
   *   is a `TypeError`.
   */
  constructor(config: RealtimeEngineConfig) {
    const settings = normalizeEngineConfig('RealtimeEngine', config);
    const module = getSonareModule();
    const capabilities = engineCapabilities();
    if (!capabilities.abiCompatible) {
      throw new SonareError(
        ErrorCode.AbiMismatch,
        `Engine ABI mismatch: wasm=${capabilities.engineAbiVersion}, expected=${capabilities.expectedEngineAbiVersion}`,
      );
    }
    this.sampleRate = settings.sampleRate;
    this.nativeHandle = new module.RealtimeEngine(
      settings.sampleRate,
      settings.maxBlockSize,
      settings.commandCapacity,
      settings.telemetryCapacity,
      settings.maxChannels,
    );
  }

  /**
   * Size the engine's queues and scratch for a sample rate and block size. It
   * takes the same object, defaults and refusals as the constructor.
   *
   * `commandCapacity` must not exceed 65536 and `telemetryCapacity` must not
   * exceed 16384; a larger value throws and leaves the engine untouched. The
   * telemetry number is not a queue depth paid for one-for-one: the engine
   * reserves that many meter records per metered lane, so its memory cost is
   * far larger than the number given here.
   */
  prepare(config: RealtimeEngineConfig): void {
    const settings = normalizeEngineConfig('prepare', config);
    this.native.prepareWithChannels(
      settings.sampleRate,
      settings.maxBlockSize,
      settings.commandCapacity,
      settings.telemetryCapacity,
      settings.maxChannels,
    );
    this.sampleRate = settings.sampleRate;
  }

  /**
   * Queue a sample-accurate parameter change (engine kSetParam).
   * A reserved mixer id (`0x4d58xxxx`) other than a master id names no strip and
   * throws a `RangeError`; resolve ids with {@link resolveStripAutomationId},
   * {@link resolveBusAutomationId} and {@link resolveStripInsertAutomationId}.
   */
  setParameter(paramId: number, value: number, renderFrame?: number): void {
    this.native.setParameter(paramId, value, resolveRenderFrame('setParameter', renderFrame));
  }

  /** Queue a smoothed parameter change (engine kSetParamSmoothed). */
  setParameterSmoothed(paramId: number, value: number, renderFrame?: number): void {
    this.native.setParameterSmoothed(
      paramId,
      value,
      resolveRenderFrame('setParameterSmoothed', renderFrame),
    );
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

  /** Lane index of a declared track; the native setters still address lanes by position. */
  private laneIndexOf(fnName: string, trackId: number): number {
    if (typeof trackId !== 'number') {
      throw new TypeError(`${fnName}: trackId must be a number`);
    }
    if (this.laneOrder === undefined) {
      throw new SonareError(
        ErrorCode.InvalidState,
        `${fnName}: the lane order is unknown after a project timeline was applied; declare the lanes with setTrackLanes first`,
      );
    }
    const index = this.laneOrder.indexOf(trackId);
    if (index < 0) {
      throw new RangeError(`${fnName}: trackId ${trackId} has no lane`);
    }
    return index;
  }

  /**
   * Queue a track's solo/mute state. An armed PFL/AFL cue ({@link setTrackMonitorMode}) is folded
   * into the program output by `process()` / `processPrepared()` and kept separate by the
   * `WithMonitor` variants; offline renders, bounces and freezes never include it.
   *
   * The track must be declared through {@link setTrackLanes}: an unknown id is a `RangeError`,
   * and after {@link applyProjectTimeline} the call throws `InvalidState` until the lanes are
   * declared again.
   */
  setTrackSoloMute(
    trackId: number,
    state: { solo: boolean; mute: boolean },
    renderFrame?: number,
  ): void {
    requestObject('setTrackSoloMute', state, 'state', true);
    this.native.setSoloMute(
      this.laneIndexOf('setTrackSoloMute', trackId),
      requireBoolean('setTrackSoloMute', state.solo, 'solo'),
      requireBoolean('setTrackSoloMute', state.mute, 'mute'),
      resolveRenderFrame('setTrackSoloMute', renderFrame),
    );
  }

  /**
   * Queue a per-track PFL/AFL monitor tap mode change. `mode` is a string; an
   * ordinal is a `TypeError`. The track must be declared through
   * {@link setTrackLanes}, as for {@link setTrackSoloMute}.
   *
   * `process()` and `processPrepared()` fold the armed cue into the program output, so the
   * tapped lane is counted twice there; the `WithMonitor` variants keep the cue separate.
   * Offline renders, bounces and freezes never include the cue.
   */
  setTrackMonitorMode(trackId: number, mode: EngineTrackMonitorMode, renderFrame?: number): void {
    noOrdinal('setTrackMonitorMode', 'mode', mode);
    this.native.setTrackMonitorMode(
      this.laneIndexOf('setTrackMonitorMode', trackId),
      trackMonitorModeCode(mode),
      resolveRenderFrame('setTrackMonitorMode', renderFrame),
    );
  }

  /**
   * Replaces the realtime MIDI clip schedule. Failure leaves the previously accepted schedule in place.
   */
  setMidiClips(clips: readonly EngineMidiClipSchedule[]): void {
    this.native.setMidiClips(clips);
  }

  /** Bind the built-in synth to a MIDI destination; `config` defaults to a sine patch. */
  setBuiltinInstrument(
    destinationId: number,
    config: Omit<BuiltinSynthConfig, 'destinationId'> = {},
  ): void {
    if (typeof config === 'object' && config !== null && 'destinationId' in config) {
      throw new TypeError(
        "setBuiltinInstrument: unknown config key 'destinationId' (pass it as the first argument)",
      );
    }
    refuseBuiltinSynthOrdinals('setBuiltinInstrument', config);
    this.native.setBuiltinInstrument(destinationId, config);
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
   * keymap names. The synth takes a share of the bank, so it may be released
   * right after this call; a sample patch bound without one renders silence.
   *
   * Replacing a bound instrument takes effect at the next render block,
   * whether or not the transport is rolling. The outgoing instrument's
   * sounding notes fade out over 10 ms. The incoming instrument starts from its
   * default channel state (program, bank, controllers); a host that needs the
   * current position's state resends it with {@link pushMidiUmp} after this call.
   */
  setSynthInstrument(destinationId: number, patch: SynthPatch = {}): void {
    this.native.setSynthInstrument(
      destinationId,
      normalizeSynthInstrument('setSynthInstrument', patch),
    );
  }

  /**
   * Load (parse) SoundFont 2 bytes into the engine so SF2 instruments can be
   * bound with {@link setSf2Instrument}. The host fetches the `.sf2` and
   * passes the raw bytes; they are copied into linear memory for the call and
   * not referenced afterwards. Replaces any previously loaded SoundFont.
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
   *
   * Replacing a bound instrument takes effect at the next render block,
   * whether or not the transport is rolling. The outgoing instrument's
   * sounding notes fade out over 10 ms. The incoming instrument starts from its
   * default channel state (program, bank, controllers); a host that needs the
   * current position's state resends it with {@link pushMidiUmp} after this call.
   */
  setSf2Instrument(
    destinationId: number,
    config: Omit<Sf2InstrumentConfig, 'destinationId'> = {},
  ): void {
    if (typeof config === 'object' && config !== null && 'destinationId' in config) {
      throw new TypeError(
        "setSf2Instrument: unknown config key 'destinationId' (pass it as the first argument)",
      );
    }
    this.native.setSf2Instrument(destinationId, config);
  }

  /**
   * Clears the instrument bound to `destinationId` (default 0). Its sounding notes are released and the
   * output fades out over 10 ms. Do not call concurrently with {@link process}.
   */
  clearMidiInstrument(destinationId = 0): void {
    this.native.clearMidiInstrument(destinationId);
  }

  /** Number of bound MIDI instruments. */
  get midiInstrumentCount(): number {
    return this.native.midiInstrumentCount();
  }

  /**
   * Bind a live MIDI controller (7/14-bit CC, RPN or NRPN) to an engine
   * automation parameter. The MIDI event still reaches the destination
   * instrument; when bound, its value is also mapped into
   * `[minValue, maxValue]` for `paramId`.
   */
  bindMidiCc(binding: ProjectMidiCcBinding): void {
    this.native.bindMidiCcBinding(binding);
  }

  /** Removes every live MIDI CC / RPN / NRPN binding. */
  clearMidiCcBindings(): void {
    this.native.clearMidiCcBindings();
  }

  /** Number of bound MIDI CC descriptors. */
  get midiCcBindingCount(): number {
    return this.native.midiCcBindingCount();
  }

  /**
   * Replace a destination instrument's controller profile with a named preset
   * (see {@link controllerProfileNames}). Installing a profile drops every
   * channel's accumulated axis values: the new bindings say nothing about what
   * the old ones had reached. An unknown name throws, and so does a destination
   * with no instrument or one whose instrument holds no profile.
   */
  setControllerProfile(destinationId: number, presetName: string): void {
    this.native.setControllerProfile(destinationId, presetName);
  }

  /** Add one {@link ControllerBinding} on top of the destination's current profile; `input` and `axis` are strings. */
  bindController(destinationId: number, binding: ControllerBinding): void {
    noOrdinal('bindController', 'input', binding?.input);
    noOrdinal('bindController', 'axis', binding?.axis);
    this.native.bindController(destinationId, binding);
  }

  /**
   * Drop every binding of the destination's controller profile. The instrument
   * keeps a profile; it resolves nothing until something is bound again.
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
   * Whether note-on velocity is expression for this instrument. No fixed
   * default is possible — a wind controller ships sending breath-derived
   * velocity on one model and a constant on the next — so each preset states it
   * and a host building its own profile sets it. When false the synth takes
   * every note at full scale and the bound axes carry the dynamics alone.
   */
  setControllerVelocityMeaningful(
    destinationId: number,
    controllerVelocityMeaningful: boolean,
  ): void {
    this.native.setControllerVelocityMeaningful(
      destinationId,
      requireBoolean(
        'setControllerVelocityMeaningful',
        controllerVelocityMeaningful,
        'controllerVelocityMeaningful',
      ),
    );
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
   * Both arguments are required strings; an ordinal is a `TypeError` and an
   * unknown spelling is refused rather than resolved to a default, as are a
   * destination with no instrument and one whose instrument holds no controller
   * profile.
   */
  setControllerNoteTracking(
    destinationId: number,
    dimension: MpeDimension,
    tracking: NoteTracking,
  ): void {
    noOrdinal('setControllerNoteTracking', 'dimension', dimension);
    noOrdinal('setControllerNoteTracking', 'tracking', tracking);
    this.native.setControllerNoteTracking(destinationId, dimension, tracking);
  }

  /** Read back {@link setControllerNoteTracking} for one dimension. */
  controllerNoteTracking(destinationId: number, dimension: MpeDimension): NoteTracking {
    noOrdinal('controllerNoteTracking', 'dimension', dimension);
    return this.native.controllerNoteTracking(destinationId, dimension) as NoteTracking;
  }

  /**
   * Set how one MIDI channel (0–15) of a destination's instrument treats a
   * note-on while another note on that channel is still held: `'poly'` takes a
   * new voice each time, `'monoRetrigger'` stops and restarts the note (what
   * GS MONO MODE and CC126 mean), `'monoLegato'` carries the sounding voice
   * and only moves its pitch — a wind player's slur, which no MIDI message can
   * reach by design.
   *
   * `'monoLegato'` is a request, not a guarantee: an engine whose exciter is
   * spent at the onset — anything struck or plucked — and a target pitch below
   * what the engine's delay line can hold both fall back to an ordinary note,
   * which {@link legatoFallbackCount} counts. A channel outside [0,15] and an
   * articulation outside the enum are refused rather than clamped, and so is a
   * destination with no instrument or one whose instrument has no articulation
   * of its own. An ordinal is a `TypeError`.
   */
  setArticulation(destinationId: number, channel: number, articulation: Articulation): void {
    noOrdinal('setArticulation', 'articulation', articulation);
    this.native.setArticulation(destinationId, channel, articulation);
  }

  /**
   * Set the rig of one part (0-15), or of the destination's default when `part`
   * is omitted, on the instrument bound to `destinationId`. A direct call, not a
   * stored edit: a destination with no instrument is an invalid parameter, and
   * an instrument without part rigs (a builtin, a host-callback instrument) is
   * refused as not implemented rather than succeeding quietly. `'chain'` takes
   * 1-8 `inserts`; no other mode does.
   */
  setPartRig(request: PartRigRequest): void {
    requestObject('setPartRig', request, 'request', true);
    noOrdinal('setPartRig', 'mode', request.mode);
    // Absent part addresses the destination's default rig (the C ABI's 0xff).
    const rig = normalizePartRig('setPartRig', { ...request, part: request.part ?? 0xff });
    this.native.setPartRig(rig.destinationId, rig.part, rig.mode as never, rig.insertsJson);
  }

  /** Read back {@link setArticulation}. */
  articulation(destinationId: number, channel: number): Articulation {
    return this.native.articulation(destinationId, channel) as Articulation;
  }

  /**
   * How many times a legato continuation was asked for on this destination and
   * refused, so the note started a voice of its own instead. Counted rather
   * than inferred: a refusal sounds like an ordinary note, so nothing in the
   * audio separates "this engine declines legato" from "the mode was never
   * set". Saturates at 4294967295 rather than wrapping — matching the C ABI, so
   * the same phrase reports the same number on every surface — after which it
   * reads as "at least this many".
   *
   * Throws on a destination with no instrument, and on one whose instrument has
   * no articulation of its own — the same two refusals
   * {@link setArticulation} keeps apart. Reporting 0 for the second would read
   * as "every slur took", which is the reading this counter exists to prevent.
   */
  legatoFallbackCount(destinationId: number): number {
    return this.native.legatoFallbackCount(destinationId);
  }

  /** Install/replace a live non-destructive MIDI-FX insert for one destination. */
  setMidiFx(destinationId: number, config: MidiFxConfig): void {
    this.native.setMidiFx(destinationId, midiFxConfigJson(config, 'setMidiFx'));
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
   * Route a destination's (track lane's) MIDI to the external output queue
   * instead of the internal instrument rack, so the track plays an external
   * device. Clearing it restores internal-synth playback. Control-thread only.
   * The change takes effect at the next processed block; switching a
   * destination's route first releases its notes and resets its controllers
   * through the old route, and drops its pending MIDI-FX events. Single writer.
   */
  setMidiDestinationExternal(destinationId: number, midiDestinationExternal: boolean): void {
    this.native.setMidiDestinationExternal(
      destinationId,
      requireBoolean(
        'setMidiDestinationExternal',
        midiDestinationExternal,
        'midiDestinationExternal',
      ),
    );
  }

  /**
   * Enable/disable forwarding MIDI clock + transport (start/continue/stop) to
   * the external output queue so external gear tracks the transport tempo.
   */
  setExternalMidiClockEnabled(externalMidiClockEnabled: boolean): void {
    this.native.setExternalMidiClockEnabled(
      requireBoolean(
        'setExternalMidiClockEnabled',
        externalMidiClockEnabled,
        'externalMidiClockEnabled',
      ),
    );
  }

  /** Count of external-MIDI events dropped because the output queue was full. */
  get externalMidiDroppedCount(): number {
    return this.native.externalMidiDroppedCount();
  }

  /** External-MIDI events queued and not yet drained. */
  get externalMidiPendingCount(): number {
    return this.native.externalMidiPendingCount();
  }

  /**
   * Drain queued external-MIDI events, already lowered to MIDI 1.0 byte
   * messages ready to write to a Web MIDI output port. Call once per audio
   * block / animation frame. `maxRecords` caps the number of output events
   * returned — the shared unit across every surface. Events past the cap stay
   * queued for the next call (lossless); call again to drain the rest.
   *
   * One queued record lowers to at most 4 MIDI 1.0 messages (a MIDI 2.0
   * registered or assignable controller becomes CC 101/100 or 99/98 plus Data
   * Entry 6/38), so a positive `maxRecords` below 4 could never consume a record
   * and is rejected with a `RangeError` instead of returning nothing forever.
   */
  drainExternalMidi(maxRecords?: number): ExternalMidiEvent[] {
    return this.native
      .drainExternalMidi(maxRecords)
      .map((event) =>
        externalMidiEvent(event.destinationId, event.renderFrame, Uint8Array.from(event.bytes)),
      );
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
   * Push a live MIDI pitch bend to the engine-owned MIDI input source.
   *
   * `bend14` is unsigned 14-bit with centre 8192 (0..16383) — the dimension is
   * not 7-bit, so a value past 16383 is refused rather than narrowed. The input
   * source must be enabled with {@link setMidiInputSource} first.
   *
   * @param offsetInBlockSamples Samples into the next block drained, clamped to its last frame;
   *   negative reads as 0. Not an absolute clock position. Defaults to 0.
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
   * Push a live MIDI channel pressure to the engine-owned MIDI input source.
   * `pressure` is 7-bit (0..127). Under MPE this is the member channel's
   * per-note pressure.
   *
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
   * Push a live MIDI polyphonic key pressure to the engine-owned MIDI input
   * source. `note` and `pressure` are 7-bit (0..127).
   *
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
      resolveRenderFrame('pushMidiNoteOn', renderFrame),
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
      resolveRenderFrame('pushMidiNoteOff', renderFrame),
    );
  }

  /**
   * Queue an immediate (live) MIDI control change to a MIDI destination
   * (engine kMidiCcImmediate). `group`/`channel` are 0..15; `controller`/`value`
   * are 7-bit (0..127). `renderFrame` is the frame to fire at (omit for
   * immediate). Mirrors the Node/Python/C-ABI `pushMidiCc`.
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
      resolveRenderFrame('pushMidiCc', renderFrame),
    );
  }

  /**
   * Queue an immediate (live) MIDI pitch bend to a MIDI destination. `bend14`
   * is unsigned 14-bit with centre 8192 (0..16383); `renderFrame` is the frame
   * to fire at (omit for immediate). Mirrors the Node/Python/C-ABI
   * `pushMidiPitchBend`.
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
      resolveRenderFrame('pushMidiPitchBend', renderFrame),
    );
  }

  /**
   * Queue an immediate (live) MIDI channel pressure to a MIDI destination.
   * `pressure` is 7-bit (0..127); `renderFrame` is the frame to fire at (omit
   * for immediate). Mirrors the Node/Python/C-ABI `pushMidiChannelPressure`.
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
      resolveRenderFrame('pushMidiChannelPressure', renderFrame),
    );
  }

  /**
   * Queue an immediate (live) MIDI polyphonic key pressure to a MIDI
   * destination. `note` and `pressure` are 7-bit (0..127); `renderFrame` is the
   * frame to fire at (omit for immediate). Mirrors the Node/Python/C-ABI
   * `pushMidiPolyPressure`.
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
      resolveRenderFrame('pushMidiPolyPressure', renderFrame),
    );
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
    this.native.pushMidiUmp(
      destinationId,
      assertUmpWords('pushMidiUmp', words),
      resolveRenderFrame('pushMidiUmp', renderFrame),
    );
  }

  /**
   * Queue a live MIDI 2.0 Program Change, with an optional bank, to a MIDI
   * destination. `bank` carries `msb` and `lsb` (0..127); omit it to leave the
   * bank unchanged. `group`/`channel` are 0..15 and `program` 0..127. The
   * message takes the same receive path as {@link pushMidiUmp}. Which receivers
   * select programs: the SoundFont player (`setSf2Instrument`) selects bank and
   * program under its own RX PROGRAM CHANGE / RX BANK SELECT gates; a NativeSynth
   * bound with `useGmPrograms: true` (`setSynthInstrument`) resolves each
   * channel's voice from the bank and program; a NativeSynth whose patch plays
   * the GM kit (the `drumKit` preset, or a percussion patch with `gmKit` on)
   * takes the program as the GS drum-set select on every channel. Every other
   * receiver accepts the message and ignores it: the built-in synth, any other
   * NativeSynth patch, a part with RX PROGRAM CHANGE off, and an MPE member
   * channel. Throws when the slot ring or command queue is full. `renderFrame`
   * is the frame to fire at (omit for immediate).
   */
  pushMidiProgram(
    destinationId: number,
    group: number,
    channel: number,
    program: number,
    bank?: { msb: number; lsb: number },
    renderFrame?: number,
  ): void {
    const { bankValid, bankMsb, bankLsb } = programBank('pushMidiProgram', bank);
    this.native.pushMidiProgram(
      destinationId,
      group,
      channel,
      program,
      bankValid,
      bankMsb,
      bankLsb,
      resolveRenderFrame('pushMidiProgram', renderFrame),
    );
  }

  /**
   * Push a live MIDI 2.0 Program Change to the engine-owned MIDI input source.
   * The arguments and refusals match {@link pushMidiProgram}; `offsetInBlockSamples` is
   * an offset into the next block drained (clamped to its last frame, negative reads as 0), not
   * an absolute clock position.
   */
  pushMidiInputProgram(
    group: number,
    channel: number,
    program: number,
    bank?: { msb: number; lsb: number },
    offsetInBlockSamples = 0,
  ): void {
    const { bankValid, bankMsb, bankLsb } = programBank('pushMidiInputProgram', bank);
    this.native.pushMidiInputProgram(
      group,
      channel,
      program,
      bankValid,
      bankMsb,
      bankLsb,
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
    this.native.pushMidiInputUmp(assertUmpWords('pushMidiInputUmp', words), offsetInBlockSamples);
  }

  /**
   * Queue an immediate (live) MIDI SysEx frame to a MIDI destination. `data` is
   * the full message including the leading 0xF0 and trailing 0xF7 (1..512
   * bytes). `renderFrame` is the frame to fire at (omit for immediate). Throws
   * a `RangeError` when the destination instrument cannot prepare
   * the SysEx (retrying cannot help), and `OutOfMemory` when the payload
   * slots or the command queue are full (retry after a processed block).
   * Mirrors the Node/Python/C-ABI `pushMidiSysex`.
   */
  pushMidiSysex(destinationId: number, data: Uint8Array, renderFrame?: number): void {
    this.native.pushMidiSysex(
      destinationId,
      data,
      resolveRenderFrame('pushMidiSysex', renderFrame),
    );
  }

  /**
   * Queue a MIDI panic (all-notes-off) releasing every sounding note at
   * `renderFrame` (omitted = immediate). Mirrors the C-ABI `pushMidiPanic`.
   */
  pushMidiPanic(renderFrame?: number): void {
    this.native.pushMidiPanic(resolveRenderFrame('pushMidiPanic', renderFrame));
  }

  /**
   * Remove all registered parameters. Automation lanes stay in place, as they do
   * on {@link addParameter}. Control-thread only; not realtime-safe. Mirrors the
   * C-ABI `clearParameters`.
   */
  clearParameters(): void {
    this.native.clearParameters();
  }

  /** Read back the current transport state snapshot. */
  transportState(): EngineTransportState {
    const state = this.native.getTransportState();
    return {
      playing: state.playing,
      looping: state.looping,
      renderFrame: state.renderFrame,
      samplePosition: state.samplePosition,
      ppq: state.ppq,
      bpm: state.bpm,
      loopStartPpq: state.loopStartPpq,
      loopEndPpq: state.loopEndPpq,
      sampleRate: state.sampleRate,
      barStartPpq: state.barStartPpq,
      barCount: state.barCount,
      timeSignature: { ...state.timeSignature },
      beat: state.beat,
      beatFraction: state.beatFraction,
    };
  }

  /** Queues an integrated-loudness reset; short-term and momentary windows are retained. */
  resetMasterLoudnessMeter(renderFrame?: number): void {
    this.native.resetMasterLoudnessMeter(
      resolveRenderFrame('resetMasterLoudnessMeter', renderFrame),
    );
  }

  /** Reads the immutable factory value for a resolved insert parameter id. */
  insertParameterConstructedValue(paramId: number): number {
    return this.native.insertParameterConstructedValue(paramId);
  }

  /** Starts transport playback, at `renderFrame` when given. */
  play(renderFrame?: number): void {
    this.native.play(resolveRenderFrame('play', renderFrame));
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
    this.native.stop(resolveRenderFrame('stop', renderFrame));
  }

  /** Moves the transport to `timelineSample`, a position in timeline samples. */
  seekSample(timelineSample: number, renderFrame?: number): void {
    this.native.seekSample(timelineSample, resolveRenderFrame('seekSample', renderFrame));
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
    this.native.primeOfflineParameters(
      numChannels,
      optionalCount('primeOfflineParameters', 'blockSize', blockSize),
    );
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
    this.native.resetProcessorState(resolveRenderFrame('resetProcessorState', renderFrame));
  }

  /**
   * Longest audible tail after the last input, in samples: an upper bound made
   * of the longest instrument tail plus the longest route through lane strips
   * (channel delay included), buses, sends, the graph and the master strip.
   * An instrument lane whose patch is not known yet (a drumKit lane or a General
   * MIDI program lane before any note sounds) contributes a conservative
   * worst-case bound that narrows once a patch has sounded.
   * `2147483647` means the tail is unbounded. Control-thread only: must not be
   * called concurrently with {@link process}.
   */
  get tailSamples(): number {
    return this.native.tailSamples();
  }

  /** Processing latency of the engine in samples, fractional. */
  get graphLatencySamples(): number {
    return this.native.graphLatencySamplesQ8() / 256;
  }

  /** Largest block a process call accepts, as the native side resolved it (the library default when `maxBlockSize` was omitted); 0 before the engine is prepared. */
  get maxBlockSize(): number {
    return this.native.maxBlockSize();
  }

  /** Snap only insert automation slots after structural replay. */
  settleInsertParameters(): void {
    this.native.settleInsertParameters();
  }

  /** Drains queued commands on an offline/control-only engine immediately. */
  flushControlCommands(): void {
    this.native.flushControlCommands();
  }

  /** Applies commands already due on a control-only mirror, retaining future commands. */
  applyCommandsDueNowPreservingFuture(): void {
    this.native.applyCommandsDueNowPreservingFuture();
  }

  /** Moves the transport to `ppq`, a position in quarter notes. */
  seekPpq(ppq: number, renderFrame?: number): void {
    this.native.seekPpq(ppq, resolveRenderFrame('seekPpq', renderFrame));
  }

  /** Set a finite tempo in the range (0, 100000] BPM. */
  setTempo(bpm: number): void {
    this.native.setTempo(bpm);
  }

  /**
   * Installs a tempo map from ramp segments. Each segment needs a finite non-negative `startPpq` and a
   * `bpm` in (0, 100000]; a non-zero `endBpm` uses the same range. An empty array clears the map to the
   * single tempo.
   */
  setTempoSegments(segments: readonly EngineTempoSegment[]): void {
    this.native.setTempoSegments([...segments]);
  }

  /**
   * Sets the time signature. `numerator` and `denominator` are positive integers (a `RangeError` otherwise).
   */
  setTimeSignature(numerator: number, denominator: number): void {
    this.native.setTimeSignature(numerator, denominator);
  }

  /**
   * Installs a time-signature map. Each segment needs a finite non-negative `startPpq` and a positive
   * `numerator` / `denominator`.
   */
  setTimeSignatureSegments(segments: readonly EngineTimeSignatureSegment[]): void {
    this.native.setTimeSignatureSegments([...segments]);
  }

  /**
   * Converts a quarter-note position to a whole sample position on the engine timeline, through the tempo map.
   *
   * @param ppq Position in quarter notes (PPQ).
   * @returns Sample index.
   */
  sampleAtPpq(ppq: number): number {
    return Number(this.native.sampleAtPpq(ppq));
  }

  /** Sets the loop region between two quarter-note positions and enables or disables it. */
  setLoop(loop: EngineLoop): void {
    requestObject('setLoop', loop, 'loop', true);
    this.native.setLoop(
      loop.startPpq,
      loop.endPpq,
      optionalBoolean('setLoop', loop.enabled, 'enabled', true),
    );
  }

  /**
   * Registers a parameter's metadata (name, unit, range, default curve) for automation. A new `id` adds
   * a parameter; omitted optional fields take the defaults documented on {@link EngineParameterInfo}.
   * Control thread only; do not call concurrently with {@link process}.
   */
  addParameter(info: EngineParameterInfo): void {
    noOrdinal('addParameter', 'defaultCurve', info?.defaultCurve);
    const { defaultCurve, ...rest } = info;
    this.native.addParameter(
      defaultCurve === undefined
        ? rest
        : { ...rest, defaultCurve: automationCurveCode(defaultCurve) },
    );
  }

  /** Every registered parameter, in registration order. */
  parameters(): Required<EngineParameterInfo>[] {
    const native = this.native;
    const count = native.parameterCount();
    const out: Required<EngineParameterInfo>[] = [];
    for (let index = 0; index < count; index++) {
      out.push(parameterInfoFromNative(native.parameterInfoByIndex(index) as never));
    }
    return out;
  }

  /** One registered parameter by id; an unknown id is a `RangeError`. */
  parameterInfo(id: number): Required<EngineParameterInfo> {
    return parameterInfoFromNative(this.native.parameterInfo(id) as never);
  }

  /**
   * Replaces the automation lane driving `paramId`.
   * A reserved mixer id (`0x4d58xxxx`) other than a master id names no strip and
   * throws a `RangeError`; resolve ids with {@link resolveStripAutomationId}.
   * `curveToNext` is an {@link AutomationCurve} string; an ordinal is a `TypeError`.
   */
  setAutomationLane(paramId: number, points: EngineAutomationPoint[]): void {
    this.native.setAutomationLane(
      paramId,
      Array.isArray(points)
        ? points.map((point) => nativeAutomationPoint('setAutomationLane', point) as never)
        : points,
    );
  }

  /** Number of automation lanes. */
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
    this.native.setMarkers(assigned.markers.map(nativeMarker) as never);
    this.nextMarkerId = assigned.nextId;
    return assigned.markers.map((marker) => this.marker(marker.id));
  }

  /** Every stored marker, in timeline order. */
  markers(): EngineMarker[] {
    const native = this.native;
    const count = native.markerCount();
    const out: EngineMarker[] = [];
    for (let index = 0; index < count; index++) {
      out.push(markerFromNative(native.markerByIndex(index)));
    }
    return out;
  }

  /** One stored marker by id; an unknown id is a `RangeError`. */
  marker(id: number): EngineMarker {
    return markerFromNative(this.native.marker(id));
  }

  /**
   * Moves the transport to the position of marker `markerId`. An unknown marker is a `RangeError`.
   * `renderFrame` is the render-frame time to apply; omit it for immediate (a negative value is refused).
   */
  seekMarker(markerId: number, renderFrame?: number): void {
    this.native.seekMarker(markerId, resolveRenderFrame('seekMarker', renderFrame));
  }

  /**
   * Sets the loop region from the positions of two markers (start, end) and enables it. An unknown
   * marker id is a `RangeError`.
   */
  setLoopFromMarkers(startMarkerId: number, endMarkerId: number): void {
    this.native.setLoopFromMarkers(startMarkerId, endMarkerId);
  }

  /**
   * Set a metronome config. The click length is `clickSamples` or `clickSec`
   * (at most one second), never both; an explicit `0` is refused, so omit the
   * field for the 2 ms default.
   */
  setMetronome(config: EngineMetronomeConfig): void {
    requestObject('setMetronome', config, 'config', true);
    const { clickSamples, clickSec, ...rest } = config;
    const hasSamples = clickSamples !== undefined && clickSamples !== null;
    const hasSec = clickSec !== undefined && clickSec !== null;
    if (hasSamples && hasSec) {
      throw new RangeError('setMetronome: give clickSamples or clickSec, not both');
    }
    for (const [name, value] of [
      ['clickSamples', clickSamples],
      ['clickSec', clickSec],
    ] as const) {
      if (value === 0) {
        throw new RangeError(`setMetronome: ${name} must not be 0 (omit it for the default)`);
      }
    }
    this.native.setMetronome({
      ...rest,
      ...(hasSamples ? { clickSamples } : {}),
      ...(hasSec ? { clickSeconds: clickSec } : {}),
    });
    this.metronomeSpelling = hasSec ? 'sec' : 'samples';
  }

  /** The metronome config, carrying only the click-length spelling that was set. */
  metronome(): EngineMetronomeConfig {
    const { clickSamples, clickSeconds, ...rest } = this.native.metronome();
    return this.metronomeSpelling === 'sec'
      ? { ...rest, clickSec: clickSeconds }
      : { ...rest, clickSamples };
  }

  /**
   * Sample at which a count-in of `bars` bars from `startSample` ends. Counting starts at the
   * beginning of the bar containing `startSample` (a mid-bar start gets a short first bar), and
   * every counted bar has the length of the time signature in effect at `startSample`.
   * Returns `startSample` when `bars <= 0`.
   */
  countInEndSample(startSample: number, bars: number): number {
    return Number(this.native.countInEndSample(startSample, bars));
  }

  /**
   * Replaces the audio processing graph (nodes and connections). Invalid specs are refused with a
   * `TypeError` / `RangeError`. Control thread only; do not call concurrently with {@link process}.
   */
  setGraph(spec: EngineGraphSpec): void {
    requireObject('setGraph', 'spec', spec);
    this.native.setGraph({
      ...spec,
      nodes: Array.isArray(spec.nodes)
        ? spec.nodes.map(({ type, ...node }) => ({
            ...node,
            ...(type === undefined
              ? {}
              : { type: tokenCode('setGraph', 'node type', type, GRAPH_NODE_TYPES) }),
          }))
        : spec.nodes,
      connections: Array.isArray(spec.connections)
        ? spec.connections.map(({ mix, ...connection }) => ({
            ...connection,
            ...(mix === undefined ? {} : { mix: tokenCode('setGraph', 'mix', mix, GRAPH_MIXES) }),
          }))
        : spec.connections,
    } as never);
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
   * Replaces the whole published clip set; every entry is validated and copied, and a refused entry
   * leaves the previous set unchanged. Control thread only.
   */
  setClips(clips: EngineClip[]): void {
    this.native.setClips(clips.map(nativeClip));
  }

  /**
   * Replaces every published clip carrying `clip.id` with `clip`, or adds it
   * when none does. Every other clip stays as published, so its audio is not
   * copied again. Validated as {@link setClips} validates each entry; a refused
   * clip leaves the published set unchanged.
   */
  upsertClip(clip: EngineClip): void {
    this.native.upsertClip(nativeClip(clip));
  }

  /**
   * Removes the clip carrying `clipId`.
   *
   * @throws RangeError when no clip has that id.
   */
  removeClip(clipId: number): void {
    this.native.removeClip(clipId);
  }

  /**
   * Returns the engine-owned PCM behind a clip: the audio generated for a
   * tempo-sync clip by the control-thread setter, or the audio a
   * {@link freezeOffline} produced. `null` for any other clip.
   */
  prebakedClipChannels(clipId: number): Float32Array[] | null {
    return this.native.prebakedClipChannels(clipId);
  }

  /** Number of published clips. */
  get clipCount(): number {
    return this.native.clipCount();
  }

  /**
   * Normalizes each send's pre/post tap point to the integer the native layer
   * reads; an omitted one stays absent, and embind fills in the default.
   * Shared by track lanes and buses, which carry the same send shape.
   */
  private static normalizeSends(fnName: string, sends: EngineTrackSend[]): WasmEngineTrackSend[] {
    return sends.map((send) => {
      noOrdinal(fnName, 'sendTiming', send.sendTiming);
      const { sendTiming, ...rest } = send;
      return sendTiming === undefined ? rest : { ...rest, sendTiming: sendTimingCode(sendTiming) };
    });
  }

  /**
   * Declares the mixer track lanes in their order. Throws a `RangeError` when
   * the list holds more than {@link MAX_TRACK_LANES} lanes, and when a lane's
   * `outputBusId` is an explicit `0` (omit it to stay on the master mix).
   */
  setTrackLanes(lanes: readonly EngineTrackLane[]): void {
    const nativeLanes = lanes.map((lane) => {
      const { sourceChannelLayout, outputBusId, ...rest } = lane;
      return {
        ...rest,
        sends: lane.sends ? RealtimeEngine.normalizeSends('setTrackLanes', lane.sends) : undefined,
        outputBusId: optionalBusId('setTrackLanes', 'outputBusId', outputBusId),
        ...(sourceChannelLayout === undefined
          ? {}
          : {
              sourceChannelLayout: tokenCode(
                'setTrackLanes',
                'sourceChannelLayout',
                sourceChannelLayout,
                CHANNEL_LAYOUT_VALUES,
              ),
            }),
      };
    });
    this.native.setTrackLanes(nativeLanes);
    this.laneOrder = lanes.map((lane) => lane.trackId);
  }

  /**
   * Replaces the sends of one lane, leaving its output bus, layout and every
   * other lane unchanged; an empty array clears them. An unchanged send list
   * keeps its in-flight level ramps. Throws for an unknown track id, an
   * undeclared bus, a duplicate send bus or a level outside the send range.
   * Unlike the worklet engine's `setTrackSends`, the lane must already be
   * declared through {@link setTrackLanes}.
   */
  setTrackSends(trackId: number, sends: EngineTrackSend[]): void {
    this.native.setTrackSends(trackId, RealtimeEngine.normalizeSends('setTrackSends', sends));
  }

  /**
   * Sets the output bus of one lane, leaving its sends, layout and every other
   * lane unchanged; `null` routes to the master mix and `0` is a `RangeError`.
   * Throws for an unknown track id or an undeclared bus; the lane must already
   * be declared through {@link setTrackLanes}.
   */
  setTrackOutputBus(trackId: number, busId: number | null): void {
    this.native.setTrackOutputBus(trackId, optionalBusId('setTrackOutputBus', 'busId', busId));
  }

  /**
   * Keys one insert of a strip from another source (ducking/sidechainRouter
   * inserts); `null` removes the binding. A track ref accepts only a `'track'`
   * source, the master strip a track or a bus. Lanes are processed in key order
   * and the key is delay-compensated to the destination strip input, so the
   * result depends on neither the lane order nor the block size. Throws,
   * leaving the bindings unchanged, for a lane keying itself, a binding that
   * would close a cycle over the lane bindings, a full binding table and an
   * alignment past the delay ceiling. Not callable while a block is being
   * processed.
   */
  setStripSidechain(ref: StripRef, insertIndex: number, source: SidechainSource | null): void {
    const target = stripTarget('setStripSidechain', ref);
    const key =
      source === null ? { kind: 0, id: 0 } : sidechainSourceArg('setStripSidechain', source);
    if (target.master) {
      this.native.setMasterSidechain(insertIndex, key.kind, key.id);
      return;
    }
    if (key.kind !== 0) {
      throw new RangeError("setStripSidechain: a track strip accepts only a 'track' source");
    }
    this.native.setLaneSidechain(target.trackId, insertIndex, key.id);
  }

  /**
   * Installs a compiled project timeline into this stopped engine, all or
   * nothing: tempo and time-signature segments, markers, track lanes, track
   * automation, audio and MIDI clips, and the per-track strips of the project's
   * mixer scene are replaced in full (a later low-level setter on those domains
   * is overwritten by the next apply). Instruments, buses, the master strip,
   * metronome, loop and capture are not touched; bind instruments separately.
   *
   * Throws while the transport is playing, for a timeline the engine cannot
   * hold, and for a scene strip routed from several tracks. A validation
   * failure leaves the engine unchanged. The engine keeps its own reference, so
   * {@link ProjectTimeline.destroy} may follow immediately.
   *
   * Also throws on an unprepared engine and on a timeline whose sample rate
   * differs from the engine's. Once a timeline is applied, a later prepare at
   * another rate is refused until the timeline's clips are cleared.
   */
  applyProjectTimeline(timeline: ProjectTimeline): void {
    // Resolve through the identity registry so no raw native handle crosses the
    // facade boundary and the realtime bundle needs no value class import.
    const timelineId = projectTimelineNativeId(timeline);
    if (timelineId === undefined) {
      throw new TypeError('timeline must be a ProjectTimeline instance');
    }
    this.native.applyProjectTimeline(timelineId);
    this.laneOrder = undefined;
  }

  /**
   * Replaces the bus topology. Refuses (state unchanged) an output or send naming an undeclared bus or
   * the bus itself, and any routing cycle. Control thread only; do not call concurrently with {@link process}.
   */
  setTrackBuses(buses: EngineBus[]): void {
    // Array.isArray guards a caller-fabricated array-like (e.g. `{ length }`)
    // meant to probe the native array-length read: passing it through
    // unmodified lets that guard see the real (missing) length rather than
    // failing here on a `.map` that array-likes do not implement.
    this.native.setTrackBuses(
      Array.isArray(buses)
        ? buses.map((bus) => {
            const { channelLayout, outputBusId, ...rest } = bus;
            return {
              ...rest,
              sends: bus.sends
                ? RealtimeEngine.normalizeSends('setTrackBuses', bus.sends)
                : undefined,
              outputBusId: optionalBusId('setTrackBuses', 'outputBusId', outputBusId),
              ...(channelLayout === undefined
                ? {}
                : {
                    channelLayout: tokenCode(
                      'setTrackBuses',
                      'channelLayout',
                      channelLayout,
                      CHANNEL_LAYOUT_VALUES,
                    ),
                  }),
            };
          })
        : (buses as WasmEngineBus[]),
    );
  }

  /**
   * Keys one insert of a bus strip from a track lane or another bus
   * (ducking/sidechainRouter inserts); `null` removes the binding.
   */
  setBusSidechain(busId: number, insertIndex: number, source: SidechainSource | null): void {
    const key =
      source === null ? { kind: 0, id: 0 } : sidechainSourceArg('setBusSidechain', source);
    this.native.setBusSidechain(busId, insertIndex, key.kind, key.id);
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
    const target = stripTarget('canSetStripSidechain', ref);
    const key = sidechainSourceArg('canSetStripSidechain', source);
    if (target.master) {
      return sidechainCheckFromCode(
        this.native.canSetMasterSidechain(insertIndex, key.kind, key.id),
      );
    }
    if (key.kind !== 0) {
      throw new RangeError("canSetStripSidechain: a track strip accepts only a 'track' source");
    }
    return sidechainCheckFromCode(
      this.native.canSetLaneSidechain(target.trackId, insertIndex, key.id),
    );
  }

  /** Reports whether {@link setBusSidechain} would accept the binding, without changing anything. */
  canSetBusSidechain(busId: number, insertIndex: number, source: SidechainSource): SidechainCheck {
    const key = sidechainSourceArg('canSetBusSidechain', source);
    return sidechainCheckFromCode(
      this.native.canSetBusSidechain(busId, insertIndex, key.kind, key.id),
    );
  }

  /** Replaces a bus strip with the given scene bus; the document is sent as `{ buses: [bus] }`. */
  setBusStrip(busId: number, bus: MixSceneBusInput): void {
    this.native.setBusStripJson(busId, sceneBusJson('setBusStrip', bus));
  }

  /**
   * Replaces a strip (a track's, or the master) with the given scene strip; the
   * document is sent as `{ strips: [strip] }`.
   */
  setStrip(ref: StripRef, strip: MixSceneStripInput): void {
    const target = stripTarget('setStrip', ref);
    const json = sceneStripJson('setStrip', strip);
    if (target.master) {
      this.native.setMasterStripJson(json);
    } else {
      this.native.setTrackStripJson(target.trackId, json);
    }
  }

  /** Sets one EQ band of a strip (a track's, or the master). */
  setStripEqBand(ref: StripRef, bandIndex: number, band: EqBand): void {
    const target = stripTarget('setStripEqBand', ref);
    const json = bandJson('setStripEqBand', band);
    if (target.master) {
      this.native.setMasterStripEqBandJson(bandIndex, json);
    } else {
      this.native.setTrackStripEqBandJson(target.trackId, bandIndex, json);
    }
  }

  /** Sets one EQ band of a bus strip. */
  setBusEqBand(busId: number, bandIndex: number, band: EqBand): void {
    this.native.setBusStripEqBandJson(busId, bandIndex, bandJson('setBusEqBand', band));
  }

  /** Bypasses or re-enables one insert of a strip; `resetOnBypass` defaults to `false`. */
  setStripInsertBypassed(
    ref: StripRef,
    insertIndex: number,
    state: { bypassed: boolean; resetOnBypass?: boolean },
  ): void {
    const target = stripTarget('setStripInsertBypassed', ref);
    const { bypassed, resetOnBypass } = bypassState('setStripInsertBypassed', state);
    if (target.master) {
      this.native.setMasterStripInsertBypassed(insertIndex, bypassed, resetOnBypass);
    } else {
      this.native.setTrackStripInsertBypassed(target.trackId, insertIndex, bypassed, resetOnBypass);
    }
  }

  /** Bus-strip counterpart of {@link setStripInsertBypassed}. */
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
   * processor's JSON-key parameter name — one of the entries
   * {@link masteringInsertParamInfo} reports with a non-null `id`; a
   * construction-only entry (`id` null) takes effect only when the insert is
   * built. Applied at the next block head via the engine command queue; safe
   * during playback. Throws if the track, insert, or name is unknown, the
   * param is not realtime-safe, or the command queue is full.
   */
  setStripInsertParam(
    ref: StripRef,
    insertIndex: number,
    param: { name: string; value: number },
  ): void {
    const target = stripTarget('setStripInsertParam', ref);
    const { name, value } = insertParam('setStripInsertParam', param);
    if (target.master) {
      this.native.setMasterStripInsertParamByName(insertIndex, name, value);
    } else {
      this.native.setTrackStripInsertParamByName(target.trackId, insertIndex, name, value);
    }
  }

  /**
   * Apply a live insert edit on this engine's owning thread without draining its
   * command queue. Returns `false` when the smoother slots are exhausted.
   */
  applyStripInsertParamNow(
    ref: StripRef,
    insertIndex: number,
    param: { name: string; value: number },
  ): boolean {
    const target = stripTarget('applyStripInsertParamNow', ref);
    const { name, value } = insertParam('applyStripInsertParamNow', param);
    return target.master
      ? this.native.applyMasterStripInsertParamByNameNow(insertIndex, name, value)
      : this.native.applyTrackStripInsertParamByNameNow(target.trackId, insertIndex, name, value);
  }

  /** Restore a retained insert value exactly after a strip scene is replayed. */
  restoreStripInsertParam(
    ref: StripRef,
    insertIndex: number,
    param: { name: string; value: number },
  ): void {
    const target = stripTarget('restoreStripInsertParam', ref);
    const { name, value } = insertParam('restoreStripInsertParam', param);
    if (target.master) {
      this.native.restoreMasterStripInsertParamByName(insertIndex, name, value);
    } else {
      this.native.restoreTrackStripInsertParamByName(target.trackId, insertIndex, name, value);
    }
  }

  /** Bus-strip counterpart of {@link setStripInsertParam}. */
  setBusInsertParam(
    busId: number,
    insertIndex: number,
    param: { name: string; value: number },
  ): void {
    const { name, value } = insertParam('setBusInsertParam', param);
    this.native.setBusStripInsertParamByName(busId, insertIndex, name, value);
  }

  /** Bus-strip counterpart of {@link applyStripInsertParamNow}. */
  applyBusInsertParamNow(
    busId: number,
    insertIndex: number,
    param: { name: string; value: number },
  ): boolean {
    const { name, value } = insertParam('applyBusInsertParamNow', param);
    return this.native.applyBusStripInsertParamByNameNow(busId, insertIndex, name, value);
  }

  /** Bus-strip counterpart of {@link restoreStripInsertParam}. */
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
    const target = stripTarget('clearStripInsertParameterBases', ref);
    if (target.master) {
      this.native.clearMasterInsertParameterBases();
    } else {
      this.native.clearTrackInsertParameterBases(target.trackId);
    }
  }

  /**
   * Clears the stored base values of the inserts of bus `busId` that automation offsets are applied to.
   * Not safe concurrently with {@link process}.
   */
  clearBusInsertParameterBases(busId: number): void {
    this.native.clearBusInsertParameterBases(busId);
  }

  /**
   * Resolves a strip insert parameter (by its JSON-key name) to the reserved
   * automation id usable with `setAutomationLane` / `setParameter`. Throws
   * `RangeError` when the track, insert, or name is unknown, as Node does and as
   * Python raises `SonareValueError`; no surface returns a `-1` sentinel.
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
   * construction-only, so no automation id resolves (RangeError) and an edit needs a strip
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
   */
  resolveStripInsertAutomationId(ref: StripRef, insertIndex: number, paramName: string): number {
    const target = stripTarget('resolveStripInsertAutomationId', ref);
    return target.master
      ? this.native.resolveMasterInsertAutomationId(insertIndex, paramName)
      : this.native.resolveTrackInsertAutomationId(target.trackId, insertIndex, paramName);
  }

  /** Bus-strip counterpart of {@link resolveStripInsertAutomationId}. */
  resolveBusInsertAutomationId(busId: number, insertIndex: number, paramName: string): number {
    return this.native.resolveBusInsertAutomationId(busId, insertIndex, paramName);
  }

  /**
   * Resolve a strip's fader (`'faderDb'`), pan (`'pan'`) or width (`'width'`) to
   * its reserved automation id, usable with `setAutomationLane` / `setParameter`.
   * A track ref has no width, so `'width'` on one is a `RangeError`. The id
   * names the track, not its lane position, so it keeps driving that track
   * across {@link setTrackLanes} reorders and applies nothing once the track is
   * removed; the master strip always exists, so its id never goes stale. Throws
   * `RangeError` when the track has no lane or the name is none of the three.
   */
  resolveStripAutomationId(ref: StripRef, kind: 'faderDb' | 'pan' | 'width'): number {
    const target = stripTarget('resolveStripAutomationId', ref);
    if (target.master) {
      return this.native.resolveMasterAutomationId(kind);
    }
    if (kind === 'width') {
      throw new RangeError("resolveStripAutomationId: a track strip has no 'width' parameter");
    }
    return this.native.resolveTrackLaneAutomationId(target.trackId, kind);
  }

  /**
   * Resolve a bus's fader (`'faderDb'`) to its reserved automation id. Same
   * lifetime rule as {@link resolveStripInsertAutomationId}: the id names the
   * bus, not its position in the bus list, so it keeps driving that bus across
   * {@link setTrackBuses} reorders and the removal of other buses, and applies
   * nothing once the bus is removed. Throws `RangeError` when the bus is not
   * configured or the name is not `'faderDb'`.
   */
  resolveBusAutomationId(busId: number, paramName: 'faderDb'): number {
    return this.native.resolveBusAutomationId(busId, paramName);
  }

  /**
   * Queue a strip fader change in dB: the strip's `'faderDb'` automation id plus
   * {@link setParameter}.
   */
  setStripFaderDb(ref: StripRef, faderDb: number, renderFrame?: number): void {
    this.setParameter(this.resolveStripAutomationId(ref, 'faderDb'), faderDb, renderFrame);
  }

  /** Queue a bus gain change in dB: the bus's `'faderDb'` automation id plus {@link setParameter}. */
  setBusGainDb(busId: number, gainDb: number, renderFrame?: number): void {
    this.setParameter(this.resolveBusAutomationId(busId, 'faderDb'), gainDb, renderFrame);
  }

  /**
   * Resolves a hosted instrument's continuous parameter (by its JSON-key name)
   * to the reserved automation id usable with `setAutomationLane` /
   * `setParameter`, so an instrument parameter is driven at audio-block
   * precision exactly like a strip insert. Throws `RangeError` when the destination
   * has no bound instrument, the instrument exposes no automatable parameters,
   * or the name is unknown.
   *
   * For the NativeSynth ({@link setSynthInstrument}) the names are the
   * continuous {@link SynthPatch} fields: `gain`, `busDrive`, `cutoffHz`,
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
   * throw `RangeError`: they resize voice pools or swap DSP topology, which is not
   * audio-thread safe. Rebind the instrument with a new patch instead.
   *
   * All automatable parameters of a NativeSynth reach already-sounding voices
   * from the next processed block. Voices using GM program patches are
   * unaffected; parameters apply to NativeSynth's own patch only.
   *
   * The id survives an unbind/rebind of the same destination and applies
   * nothing while that destination is unbound.
   */
  resolveInstrumentAutomationId(destinationId: number, paramName: string): number {
    return this.native.resolveInstrumentAutomationId(destinationId, paramName);
  }

  /** Sets a track strip's pan position in realtime (glitch-free). The master strip has no setter: drive `resolveStripAutomationId('master', 'pan')`. */
  setStripPan(ref: StripRef, pan: number): void {
    this.native.setTrackStripPan(trackOnly('setStripPan', ref), pan);
  }

  /** Sets a track strip's pan law in realtime; an ordinal is a `TypeError`. */
  setStripPanLaw(ref: StripRef, panLaw: PanLaw): void {
    const trackId = trackOnly('setStripPanLaw', ref);
    noOrdinal('setStripPanLaw', 'panLaw', panLaw);
    this.native.setTrackStripPanLaw(trackId, panLawCode(panLaw));
  }

  /** Sets a track strip's pan mode in realtime; an ordinal is a `TypeError`. */
  setStripPanMode(ref: StripRef, panMode: PanMode): void {
    const trackId = trackOnly('setStripPanMode', ref);
    noOrdinal('setStripPanMode', 'panMode', panMode);
    this.native.setTrackStripPanMode(trackId, panModeCode(panMode));
  }

  /** Sets a track strip's dual-pan left/right positions in realtime. */
  setStripDualPan(ref: StripRef, leftPan: number, rightPan: number): void {
    this.native.setTrackStripDualPan(trackOnly('setStripDualPan', ref), leftPan, rightPan);
  }

  /**
   * Sets a track strip's surround placement in realtime (glitch-free; a
   * constant-power 5 ms one-pole glide). Used when the destination has more than
   * two channels. Omitted fields default to a centered point source
   * (`distance` 1); a `distance` of 0 or less is treated as 1. Throws a RangeError
   * for `azimuth` outside [-180, 180] or `divergence`/`lfe` outside [0, 1], and for an
   * unknown track, a track without a lane strip, or a non-finite field.
   */
  setStripSurroundPan(ref: StripRef, pan: SurroundPan): void {
    const trackId = trackOnly('setStripSurroundPan', ref);
    assertSurroundPan('setStripSurroundPan', pan);
    this.native.setTrackStripSurroundPan(trackId, pan);
  }

  /**
   * Sets a track strip's alignment delay (whole samples), moving this lane
   * later relative to every other lane. It is not latency: PDC does not
   * compensate it and the reported graph latency is unchanged.
   */
  setStripChannelDelaySamples(ref: StripRef, delaySamples: number): void {
    this.native.setTrackStripChannelDelaySamples(
      trackOnly('setStripChannelDelaySamples', ref),
      delaySamples,
    );
  }

  /**
   * Sets a bus strip's output pan position in realtime (glitch-free). Throws
   * for an unknown bus or one wider than stereo.
   */
  setBusPan(busId: number, pan: number): void {
    this.native.setBusStripPan(busId, pan);
  }

  /** Sets a bus strip's pan law in realtime; an ordinal is a `TypeError`. */
  setBusPanLaw(busId: number, panLaw: PanLaw): void {
    noOrdinal('setBusPanLaw', 'panLaw', panLaw);
    this.native.setBusStripPanLaw(busId, panLawCode(panLaw));
  }

  /** Sets a bus strip's pan mode in realtime; an ordinal is a `TypeError`. */
  setBusPanMode(busId: number, panMode: PanMode): void {
    noOrdinal('setBusPanMode', 'panMode', panMode);
    this.native.setBusStripPanMode(busId, panModeCode(panMode));
  }

  /** Sets a bus strip's dual-pan left/right positions in realtime. */
  setBusDualPan(busId: number, leftPan: number, rightPan: number): void {
    this.native.setBusStripDualPan(busId, leftPan, rightPan);
  }

  /** Pops the oldest clip-page request, or `undefined` when none is queued. */
  popClipPageRequest(): ClipPageSampleRequest | undefined {
    return this.native.popClipPageRequest() ?? undefined;
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

  /** The current time-stretch voice capacity (default 8). */
  get warpVoiceCapacity(): number {
    return this.native.warpVoiceCapacity();
  }

  /**
   * Sets the clip-page look-ahead window in timeline frames.
   *
   * The player reports the pages it is *about to* read that are not resident
   * yet, so a streaming host can service them before the audio thread reaches
   * them. Without look-ahead a page miss is only reported after the read
   * already produced silence, which costs one block of silence at every page
   * boundary the host has not primed — the reason a sliding-window streamer
   * cannot keep a live playhead fed from miss reports alone.
   *
   * Look-ahead requests drain through the same `popClipPageRequest` queue and
   * are queued *after* the block's genuine misses, so a host that keeps only
   * the newest request per clip (as {@link ClipPageStreamer} does) tracks the
   * look-ahead frontier.
   *
   * `prepare` defaults this to half a second at the engine's sample rate. `0`
   * disables the look-ahead. A clip whose pages are all resident produces no
   * requests at all, with or without look-ahead. Safe to call during playback.
   */
  setClipPagePrefetchFrames(frames: number): void {
    this.native.setClipPagePrefetchFrames(frames);
  }

  /** Current clip-page look-ahead window in timeline frames. */
  get clipPagePrefetchFrames(): number {
    return this.native.clipPagePrefetchFrames();
  }

  /**
   * Resolves one punch bound spelled as quarter notes, samples or seconds; a
   * negative value is refused and giving more than one spelling is a `RangeError`.
   */
  private punchBound(
    ppq: number | undefined,
    sample: number | undefined,
    sec: number | undefined,
    names: readonly [string, string, string],
  ): number {
    const [ppqName, sampleName, secName] = names;
    if (ppq === undefined || ppq === null) {
      const bound = resolveSampleBound(
        'configureCapture',
        sample,
        sec,
        this.sampleRate,
        `punch.${sampleName}`,
        `punch.${secName}`,
      );
      if (bound === undefined) {
        throw new RangeError(
          `configureCapture: punch needs ${ppqName}, ${sampleName} or ${secName}`,
        );
      }
      return bound;
    }
    if ((sample !== undefined && sample !== null) || (sec !== undefined && sec !== null)) {
      throw new RangeError(
        `configureCapture: give one of punch.${ppqName}, punch.${sampleName} or punch.${secName}`,
      );
    }
    if (typeof ppq !== 'number' || !Number.isFinite(ppq) || ppq < 0) {
      throw new RangeError(`configureCapture: punch.${ppqName} must be a non-negative number`);
    }
    return this.sampleAtPpq(ppq);
  }

  /**
   * Sets up capture in one call: buffer, source, record offset, input monitor
   * and punch window. The call states the whole setup, so an omitted field
   * returns to its default and an omitted `punch` clears the punch window. The
   * buffer is reallocated only when `numChannels` or `capacityFrames` changes,
   * and doing that while capture is armed is a `SonareError` with
   * `ErrorCode.InvalidState`.
   */
  configureCapture(options: CaptureOptions): void {
    requestObject('configureCapture', options, 'options', true);
    if (typeof options.capacityFrames !== 'number') {
      throw new TypeError('configureCapture: capacityFrames must be a number');
    }
    const numChannels = optionalCount('configureCapture', 'numChannels', options.numChannels) ?? 2;
    const source = tokenCode(
      'configureCapture',
      'source',
      options.source ?? 'output',
      CAPTURE_SOURCES,
    );
    const offset = options.recordOffsetSamples ?? 0;
    if (typeof offset !== 'number') {
      throw new TypeError('configureCapture: recordOffsetSamples must be a number');
    }
    let monitor = { enabled: true, gain: 1 };
    if (options.inputMonitor !== undefined && options.inputMonitor !== null) {
      requireObject('configureCapture', 'inputMonitor', options.inputMonitor);
      const gain = options.inputMonitor.gain ?? 1;
      assertFloatValue('configureCapture', gain, 'inputMonitor.gain');
      monitor = {
        enabled: requireBoolean(
          'configureCapture',
          options.inputMonitor.enabled,
          'inputMonitor.enabled',
        ),
        gain,
      };
    }
    let punch: { start: number; end: number } | undefined;
    if (options.punch !== undefined && options.punch !== null) {
      requireObject('configureCapture', 'punch', options.punch);
      punch = {
        start: this.punchBound(
          options.punch.startPpq,
          options.punch.startSample,
          options.punch.startSec,
          ['startPpq', 'startSample', 'startSec'],
        ),
        end: this.punchBound(options.punch.endPpq, options.punch.endSample, options.punch.endSec, [
          'endPpq',
          'endSample',
          'endSec',
        ]),
      };
    }
    const native = this.native;
    // Applied first: a window the engine refuses leaves the rest of the setup untouched.
    if (punch === undefined) {
      native.setCapturePunch(0, 1, false);
    } else {
      native.setCapturePunch(punch.start, punch.end, true);
    }
    const current = this.captureBuffer;
    if (
      current === undefined ||
      current.numChannels !== numChannels ||
      current.capacityFrames !== options.capacityFrames
    ) {
      if (current !== undefined && native.captureStatus().armed) {
        throw new SonareError(
          ErrorCode.InvalidState,
          'configureCapture: the capture buffer cannot change while capture is armed',
        );
      }
      native.setCaptureBuffer(numChannels, options.capacityFrames);
      this.captureBuffer = { numChannels, capacityFrames: options.capacityFrames };
    }
    native.setCaptureSource(source === CAPTURE_SOURCES.input ? 'input' : 'output');
    native.setRecordOffsetSamples(offset);
    native.setInputMonitor(monitor.enabled, monitor.gain);
  }

  /** Arms or disarms capture. */
  armCapture(state: { armed: boolean }): void {
    requestObject('armCapture', state, 'state', true);
    this.native.armCapture(requireBoolean('armCapture', state.armed, 'armed'));
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
   * Reads the recorded samples: one `Float32Array` per capture channel, sliced to the frames recorded so far.
   */
  capturedAudio(): Float32Array[] {
    return this.native.capturedAudio();
  }

  /**
   * Renders in place, adding engine output to `channels`. Zero each plane first
   * when it contains no upstream input.
   */
  process(channels: Float32Array[]): Float32Array[] {
    return this.native.process(channels);
  }

  /**
   * Allocates persistent per-channel WASM-heap scratch for the zero-copy
   * `channelBuffer` / `processPrepared` realtime path. Call once (off the
   * audio thread) before driving `processPrepared` from an AudioWorklet so the
   * render callback never allocates on the C++/JS heap.
   */
  prepareChannels(numChannels: number, maxFrames: number): void {
    this.native.prepareChannels(numChannels, maxFrames);
  }

  /**
   * Returns a Float32Array view onto the persistent WASM-heap scratch for one
   * channel (valid for up to `numFrames`). Fill it, call `processPrepared`, then
   * read the same view back. Re-acquire after WASM memory growth.
   */
  channelBuffer(channel: number, numFrames: number): Float32Array {
    return this.native.getChannelBuffer(channel, numFrames);
  }

  /**
   * Runs the engine in place over the prepared per-channel scratch buffers.
   * Zero each active span first when it contains no upstream input.
   * Allocation-free: safe to call on the AudioWorklet render thread after
   * `prepareChannels`.
   */
  processPrepared(numFrames: number): void {
    this.native.processPrepared(numFrames);
  }

  /**
   * Allocates the cue-bus counterpart of {@link prepareChannels}. Needed only
   * when PFL/AFL monitoring must reach a separate output: `processPrepared`
   * folds the cue bus into the program output, while
   * {@link processPreparedWithMonitor} keeps the two apart. Call once, off the
   * audio thread, with at least as many channels as `prepareChannels` got.
   */
  prepareMonitorChannels(numChannels: number, maxFrames: number): void {
    this.native.prepareMonitorChannels(numChannels, maxFrames);
  }

  /**
   * Returns a Float32Array view onto the persistent cue-bus scratch for one
   * channel (valid for up to `numFrames`). Read it after
   * {@link processPreparedWithMonitor}. Re-acquire after WASM memory growth.
   */
  monitorChannelBuffer(channel: number, numFrames: number): Float32Array {
    return this.native.getMonitorChannelBuffer(channel, numFrames);
  }

  /**
   * Runs the engine in place over the prepared scratch, writing the cue bus to
   * the monitor scratch instead of folding it into the program output.
   * Allocation-free: safe on the AudioWorklet render thread after
   * `prepareChannels` and `prepareMonitorChannels`.
   */
  processPreparedWithMonitor(numFrames: number): void {
    this.native.processPreparedWithMonitor(numFrames);
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
   * Render `request.channels` offline from the current transport position.
   * Requesting more planes than `prepare` reserved throws a `RangeError` rather
   * than returning silence that reads as a finished render.
   *
   * Set `finalize: false` to render one chunk of a longer timeline; see
   * {@link RenderOfflineRequest.finalize} and {@link finishOfflineRender}.
   */
  renderOffline(request: RenderOfflineRequest): Float32Array[] {
    const normalized = normalizeRenderOfflineRequest(request);
    return this.native.renderOffline(
      normalized.channels,
      normalized.blockSize,
      normalized.finalize,
    );
  }

  /**
   * End a chunked offline render: release every note the sequencer still holds
   * and flush the PDC / alignment delay lines. Required after
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
   * Bounce the timeline to planar channels. `numChannels` above the
   * prepared channel count throws a `RangeError`. `targetSampleRate`,
   * `sourceSampleRate` and `targetLufs` are omitted for their defaults (the
   * source rate, the engine rate, -14 LUFS); an explicit `0` is a `RangeError`.
   */
  bounceOffline(options: EngineBounceOptions): EngineBounceResult {
    requireObject('bounceOffline', 'options', options);
    noOrdinal('bounceOffline', 'dither', options.dither);
    for (const name of [
      'blockSize',
      'targetSampleRate',
      'sourceSampleRate',
      'targetLufs',
    ] as const) {
      const value = options[name];
      if (value === undefined || value === null) {
        continue;
      }
      if (typeof value === 'number' && value === 0) {
        throw new RangeError(`bounceOffline: ${name} must not be 0 (omit it for the default)`);
      }
      if (typeof value === 'number' && !Number.isFinite(value)) {
        throw new RangeError(`bounceOffline: ${name} must be a finite number`);
      }
    }
    const { dither, ...rest } = options;
    const result = this.native.bounceOffline({
      ...rest,
      ...(dither === undefined
        ? {}
        : { dither: tokenCode('bounceOffline', 'dither', dither, DITHERS) as 1 | 2 | 3 }),
    });
    return {
      channels: deinterleavePlanes(result.interleaved, result.numChannels),
      sampleRate: result.sampleRate,
      numChannels: result.numChannels,
      integratedLufs: result.integratedLufs,
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
   * throws a `RangeError`.
   */
  freezeOffline(options: EngineFreezeOptions): EngineFreezeResult {
    requireObject('freezeOffline', 'options', options);
    const { blockSize, ...rest } = options;
    const block = optionalCount('freezeOffline', 'blockSize', blockSize);
    return this.native.freezeOffline(block === undefined ? rest : { ...rest, blockSize: block });
  }

  /**
   * Drains pending process and error telemetry records, oldest first.
   *
   * @param maxRecords Most records to return (non-negative integer). Omitted: 1024. 0 returns an empty array.
   */
  drainTelemetry(maxRecords?: number): EngineTelemetry[] {
    return this.native.drainTelemetry(maxRecords).map(telemetryFromNative);
  }

  drainMeterTelemetry(maxRecords?: number): EngineMeterTelemetry[] {
    return this.native.drainMeterTelemetry(maxRecords).map(meterTelemetryFromNative);
  }

  /**
   * Drains pending meter telemetry as per-plane (wide) records. This is the one
   * meter drain: a stereo target's left and right are planes 0 and 1, and a
   * surround target carries every plane. Use it alone; {@link drainMeterTelemetry}
   * consumes the same queue (every target's records) and stays for
   * compatibility. The live AudioWorklet engine drains through this one, so a
   * worklet-driven engine delivers per-plane meters on both the postMessage and
   * the SharedArrayBuffer meter paths.
   */
  drainMeterTelemetryWide(maxRecords?: number): EngineMeterTelemetryWide[] {
    return this.native.drainMeterTelemetryWide(maxRecords).map(meterTelemetryWideFromNative);
  }

  /**
   * Per-insert gain reduction in dB (each <= 0) of a meter target's strip from the
   * last rendered block, in combined pre-to-post insert order. `targetId` uses the
   * meter-record encoding (0 master, 1..32 lanes, 33..40 buses; 0xFFFF monitor
   * yields an empty array). Throws for an id outside that range.
   */
  meterTargetInsertGainReduction(targetId: number): number[] {
    return this.native.meterTargetInsertGainReduction(targetId);
  }

  /**
   * Enables per-target spectrum + vectorscope capture. @param intervalFrames is
   * the minimum render-frame gap between snapshots (0 disables). @param bandCount
   * is the FFT band resolution (1..64); changing it re-prepares the tap. Returns
   * the band count actually applied.
   */
  configureScopeTelemetry(intervalFrames: number, bandCount: number): number {
    return this.native.configureScopeTelemetry(intervalFrames, bandCount);
  }

  /** Drains pending spectrum + vectorscope snapshots (per mix target). */
  drainScopeTelemetry(maxRecords?: number): EngineScopeTelemetry[] {
    return this.native.drainScopeTelemetry(maxRecords);
  }

  /** Release the underlying WASM object. Idempotent, as the Node facade is. */
  destroy(): void {
    if (this.released) {
      return;
    }
    this.released = true;
    this.nativeHandle.delete();
  }

  /** Releases the handle through {@link destroy}, so the instance works with `using`. */
  [Symbol.dispose](): void {
    this.destroy();
  }
}

/** A paged clip source, independent of any engine; pages are supplied on demand. */
export class ClipPageProvider {
  private nativeHandle: WasmClipPageProvider;
  private disposed = false;

  /**
   * @param options - Channel count, length and page size, all positive.
   * @throws RangeError when a dimension is not positive.
   */
  constructor(options: ClipPageProviderOptions) {
    requestObject('ClipPageProvider', options, 'options', true);
    const native = new (getSonareModule().ClipPageProvider)(
      options.numChannels,
      options.numSamples,
      options.pageFrames,
    );
    this.nativeHandle = native;
    bindNativeClipPageProvider(this, native);
  }

  private get native(): WasmClipPageProvider {
    if (this.disposed) {
      throw releasedHandleError('ClipPageProvider', 'is destroyed');
    }
    return this.nativeHandle;
  }

  /** Hand the engine one page of planar channels. Throws `InvalidState` after `destroy()`. */
  supply(pageIndex: number, channels: Float32Array[]): void {
    this.native.supply(pageIndex, channels);
  }

  /** Evict one resident page. Throws `InvalidState` after `destroy()`. */
  clear(pageIndex: number): void {
    this.native.clear(pageIndex);
  }

  /** Release the provider; a clip already scheduled keeps playing. Idempotent. */
  destroy(): void {
    if (this.disposed) {
      return;
    }
    this.disposed = true;
    this.nativeHandle.delete();
  }

  /** Releases the provider through {@link destroy}, so the instance works with `using`. */
  [Symbol.dispose](): void {
    this.destroy();
  }
}
