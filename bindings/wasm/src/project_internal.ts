import { AUTOMATION_CURVE_VALUES, resolveEnumOrdinal } from './codes.js';
import { ErrorCode } from './errors.js';
import type {
  BuiltinSynthConfig,
  ExternalInstrument,
  PartRigEntry,
  PartRigInsert,
  PartRigKey,
  PartRigMode,
  PartRigRequest,
  ProjectInstrumentBinding,
  SampleDesc,
  SampleZoneDesc,
  Sf2InstrumentConfig,
  Sf2ProgramStatus,
  SynthEnumTables,
  SynthPatch,
} from './instrument_types.js';
import {
  assertDeclaredKeys,
  codedError,
  refuse,
  refuseType,
  refuseUnknownKey,
} from './internal/entry.js';
import { releasedHandleError } from './internal/handle_guard.js';
import { requireListBudget, stringifyWithinBudget } from './internal/list_budget.js';
import { optionalId } from './internal/optional_ints.js';
import { deinterleavePlanes, interleavePlanes } from './internal/pod_config.js';
import {
  AUTOMATION_TARGET_KINDS,
  CHORD_QUALITY_CODES,
  FADE_CURVES,
  KEY_MODES,
  LOOP_MODES,
  MARKER_KINDS,
  OVERLAP_POLICIES,
  SOURCE_KINDS,
  STEM_LAYOUTS,
  TRACK_KINDS,
  tokenName,
  WARP_MODES,
} from './internal/project_tokens.js';
import { refuseSynthPatchOrdinals } from './internal/synth_tokens.js';
import { assertEventPpq, umpWord0 } from './internal/ump_events.js';
import { isAbsent, isNonArrayObject, UINT32_MAX } from './internal/value_guards.js';
import { getSonareModule } from './module_state.js';
import type {
  AlignTakeToReferenceRequest,
  AlignTakeToReferenceResult,
  ProjectAssistSidecar,
  ProjectAutomationLaneDesc,
  ProjectAutomationPoint,
  ProjectAutomationTargetKind,
  ProjectBounceResult,
  ProjectChordQuality,
  ProjectChordSymbol,
  ProjectClipCompSegment,
  ProjectClipTake,
  ProjectCompileResult,
  ProjectDiagnostic,
  ProjectDiagnosticCode,
  ProjectFadeCurve,
  ProjectKeyMode,
  ProjectKeySegment,
  ProjectLoopMode,
  ProjectLoopRecordingDesc,
  ProjectLoopRecordingResult,
  ProjectMarkerKind,
  ProjectMidiCcBinding,
  ProjectMidiClipResult,
  ProjectMidiEvent,
  ProjectMidiRouteConfig,
  ProjectMidiRouteResult,
  ProjectNotePairValidation,
  ProjectOverlapPolicy,
  ProjectSource,
  ProjectSourceKind,
  ProjectTempoCandidate,
  ProjectTempoOptions,
  ProjectTempoSegment,
  ProjectTimeSignatureSegment,
  ProjectTrackKind,
  ProjectWarpMapDesc,
  ProjectWarpMode,
  TranscribeOptions,
  TranscribeResult,
} from './project_types.js';
// A value import, and this module's one import cycle: sample_bank.ts reaches
// back here for projectModule(). Both directions are read inside function
// bodies only, never while a module body runs, which is what makes the cycle
// safe. Reading SampleBank at module scope here breaks ONE import order and
// leaves the other working, so the property is pinned by
// tests/sample-bank-module-cycle.test.ts rather than left to review.
import { SampleBank } from './sample_bank.js';
import {
  assertAudioChannels,
  assertBoundedInteger,
  assertNibble,
  assertU7,
  assertU32,
} from './validation.js';
import { SONARE_MAX_PUBLIC_PPQ as MAX_PUBLIC_PPQ } from './worklet/engine-rings.js';

/**
 * A synth binding as the embind layer takes it: the public `sampleBank` handle
 * has already been resolved to the id the native registry looks the bank up by,
 * because an embind class instance cannot travel inside a plain JS object the
 * C++ side reads field by field.
 */
export type NativeSynthBinding = Omit<SynthPatch, 'sampleBank'> & { sampleBankId?: number };

// Embind handle for the C++ `SampleBankWasm` class, narrowed the same way as
// `WasmProject` below: it is registered next to the project, so a build without
// arrangement support has neither.
export interface WasmSampleBank {
  readonly id: number;
  addSample: (data: Float32Array, desc: SampleDesc) => number;
  addZone: (setIndex: number, zone: Omit<SampleZoneDesc, 'setIndex'>) => void;
  sampleCount: () => number;
  setCount: () => number;
  delete: () => void;
}

// Embind handle for the C++ `ProjectTimelineWasm` class. Registered next to the
// project, so a build without arrangement support has neither.
export interface WasmProjectTimeline {
  readonly id: number;
  delete: () => void;
}

// A disposed facade stays keyed with a null handle, so it is refused as released
// rather than as something that never was a timeline.
const projectTimelineHandles = new WeakMap<object, WasmProjectTimeline | null>();

/** Internal: registers the facade object whose identity may cross to the engine. */
export function registerProjectTimeline(owner: object, native: WasmProjectTimeline): void {
  projectTimelineHandles.set(owner, native);
}

/** Internal: marks a facade disposed in the identity registry. */
export function unregisterProjectTimeline(owner: object): void {
  projectTimelineHandles.set(owner, null);
}

/**
 * Internal: resolves only a timeline facade created by this module; undefined
 * for anything else. Throws `InvalidState` for a disposed one.
 */
export function projectTimelineNativeId(owner: unknown): number | undefined {
  if (owner === null || (typeof owner !== 'object' && typeof owner !== 'function')) {
    return undefined;
  }
  const native = projectTimelineHandles.get(owner);
  if (native === null) {
    throw releasedHandleError('ProjectTimeline');
  }
  return native?.id;
}

/** One diagnostic as the embind layer returns it: the library's tokens, with `0` for "no target". */
export interface NativeProjectDiagnostic {
  code: ProjectDiagnosticCode;
  severity: 'error' | 'warning';
  targetId: number;
  message: string;
}

/** `compile` as the embind layer returns it, before the facade types it. */
export interface NativeProjectCompileResult {
  diagnostics: NativeProjectDiagnostic[];
}

/** `compile(true)` as the embind layer returns it, before the facade wraps the handle. */
export type NativeProjectCompileWithTimeline = NativeProjectCompileResult & {
  timeline: WasmProjectTimeline | null;
};

/** A stored track as the embind layer returns it. */
export interface NativeProjectTrack {
  id: number;
  kind: number;
  midiDestinationId: number;
  gain: number;
  pan: number;
  mute: boolean;
  solo: boolean;
  name: string;
}

/** A stored clip as the embind layer returns it. */
export interface NativeProjectClip {
  id: number;
  trackId: number;
  sourceId: number;
  sourceKind: number;
  startPpq: number;
  lengthPpq: number;
  sourceOffsetPpq: number;
  gain: number;
  loopMode: number;
  loopLengthPpq: number;
}

/** A stored source as the embind layer returns it. */
export interface NativeProjectSource {
  id: number;
  kind: number;
  numChannels: number;
  storageHandleId: number;
  sampleRateHint: number;
  nameOrUri: string;
  contentHash: string;
  externalStemRole: string;
}

/** A stored marker as the embind layer returns it. */
export interface NativeProjectMarker {
  id: number;
  ppq: number;
  name: string;
  kind: number;
  keyFifths: number;
  keyMinor: boolean;
}

/** A tempo candidate as the embind layer returns it. */
export interface NativeProjectTempoCandidate {
  bpm: number;
  confidence: number;
  label: 'primary' | 'half' | 'double';
  timeSignatureCount: number;
  timeSignature: ProjectTimeSignatureSegment;
}

/** Options as the embind layer reads them: absent means default, `0` is not passed. */
export interface NativeProjectBounceOptions {
  totalFrames?: number;
  blockSize?: number;
  numChannels?: number;
  instrumentLatencySamples?: number;
}

/** A native bounce: the interleaved render and the width it was mixed at. */
export interface NativeProjectBounce {
  interleaved: Float32Array;
  numChannels: number;
}

/** A native instrument binding: the instrument's fields with its destination beside them. */
export type NativeInstrumentBinding<T> = T & { destinationId: number };

/**
 * Swaps a facade {@link SampleBank} in a synth patch descriptor for the id the
 * embind layer looks the native bank up by, leaving every other field alone.
 *
 * Shared by the offline bounce and the realtime engine so `sampleBank` means
 * the same thing on both. It lives in this internal module rather than beside
 * the class because it hands out an internal identity that no caller has a use
 * for, and the entry point must not re-export it: a facade symbol with no C
 * counterpart is an active parity finding.
 *
 * The cast is the only one of its kind: `released` is private, and TypeScript's
 * private is a compile-time rule, so reading it once here beats widening the
 * class with a method the parity tool would then read as a facade op.
 */
export function normalizeSynthInstrument(patch: unknown): NativeSynthBinding {
  if (!isNonArrayObject(patch)) {
    throw refuseType(`patch must be a SynthPatch object; a preset is named by its preset field`);
  }
  refuseSynthPatchOrdinals(patch);
  const { sampleBank, ...rest } = patch as SynthPatch;
  if (!sampleBank) {
    // Omitted, and at runtime an explicit null: both mean no bank, which a
    // sample patch renders as silence.
    return rest;
  }
  if (!(sampleBank instanceof SampleBank)) {
    throw refuseType('sampleBank must be a SampleBank instance');
  }
  if ((sampleBank as unknown as { released: boolean }).released) {
    // The id outlives the handle, so without this the native registry would be
    // the only thing that notices — and with a different error class than Node.
    throw releasedHandleError('SampleBank');
  }
  return { ...rest, sampleBankId: sampleBank.nativeId };
}

/**
 * Resolve an instrument binding option of `Project.bounce` to the list the native
 * layer reads. The argument is one `{ destinationId?, instrument }` pair or an
 * array of them (`null` and an empty array both mean no bindings); `option` names
 * the `Project.bounce` option it came from in refusals. A pair
 * without an `instrument` key (a bare config) is refused by name, and so is a
 * `destinationId` placed inside the instrument, since it belongs to the pair.
 * `entry` converts each instrument to the shape the native reader takes; the
 * destination is merged in beside it, defaulting to `0`.
 */
/** The keys of a `{ destinationId?, instrument }` pair. */
const INSTRUMENT_BINDING_KEYS = ['destinationId', 'instrument'];

export function normalizeInstrumentBindings<E, R extends object>(
  value: ReadonlyArray<ProjectInstrumentBinding<E>> | null | undefined,
  entry: (instrument: E) => R,
  option = 'instruments',
): NativeInstrumentBinding<R>[] {
  if (isAbsent(value)) {
    return [];
  }
  if (!Array.isArray(value)) {
    throw refuseType(`${option} must be an array of { destinationId?, instrument } pairs`);
  }
  requireListBudget(value, option);
  return value.map((pair: unknown, index: number) => {
    const label = `${option}[${index}]`;
    if (!isNonArrayObject(pair)) {
      throw refuseType(`${label} must be a { destinationId?, instrument } pair`);
    }
    const record = pair as Record<string, unknown>;
    assertDeclaredKeys(record, INSTRUMENT_BINDING_KEYS, label);
    if (isAbsent(record.instrument)) {
      throw refuseType(`${label}.instrument must be given`);
    }
    const instrument = record.instrument;
    if (typeof instrument !== 'object' || Array.isArray(instrument)) {
      throw refuseType(`${label}.instrument must be an object`);
    }
    if ('destinationId' in instrument) {
      throw refuseUnknownKey(`${label}.instrument.destinationId`, [`${label}.destinationId`]);
    }
    // 0 is a real destination here, so an omitted id is 0 rather than refused.
    const destinationId = isAbsent(record.destinationId) ? 0 : record.destinationId;
    assertBoundedInteger(destinationId, `${label}.destinationId`, 0, UINT32_MAX);
    return { ...entry(instrument as E), destinationId };
  });
}

/**
 * Serialize a part-rig request's inserts to the C ABI JSON form, refusing a
 * wrong shape by field name. Returns `undefined` when there are none.
 */
function partRigInsertsJson(inserts: PartRigInsert[] | undefined): string | undefined {
  if (isAbsent(inserts)) {
    return undefined;
  }
  if (!Array.isArray(inserts)) {
    throw refuseType('inserts must be an array');
  }
  requireListBudget(inserts, 'inserts');
  return stringifyWithinBudget(inserts, 'inserts');
}

/** A part-rig call resolved to the native argument list; `mode` is unset for a key. */
export interface NormalizedPartRig {
  destinationId: number;
  part: number;
  mode: PartRigMode | undefined;
  insertsJson: string | undefined;
}

/**
 * Resolve either part-rig call form — a {@link PartRigRequest} / {@link PartRigKey}
 * object, or positional `(destinationId, part, mode?, inserts?)` — to the native
 * arguments, so both forms share one validation path. A request object takes no
 * further arguments.
 */
export function normalizePartRig(
  requestOrDestinationId: PartRigRequest | PartRigKey | number,
  part?: number,
  mode?: PartRigMode,
  inserts?: PartRigInsert[],
): NormalizedPartRig {
  if (typeof requestOrDestinationId === 'object' && requestOrDestinationId !== null) {
    if (part !== undefined || mode !== undefined || inserts !== undefined) {
      throw refuseType(`a request object takes no further arguments`);
    }
    const request = requestOrDestinationId as Partial<PartRigRequest>;
    return {
      destinationId: request.destinationId as number,
      part: request.part as number,
      mode: request.mode,
      insertsJson: partRigInsertsJson(request.inserts),
    };
  }
  return {
    destinationId: requestOrDestinationId,
    part: part as number,
    mode,
    insertsJson: partRigInsertsJson(inserts),
  };
}

/** Reads a native `{ mode, insertsJson }` part rig back into the public shape. */
export function partRigFromNative(
  native: { mode: PartRigEntry['mode']; insertsJson: string | null } | null,
): PartRigEntry | null {
  if (native === null) {
    return null;
  }
  if (native.insertsJson === null) {
    return { mode: native.mode };
  }
  const inserts = JSON.parse(native.insertsJson) as NonNullable<PartRigEntry['inserts']>;
  return { mode: native.mode, inserts };
}

// Embind handle for the C++ `ProjectWasm` class. `SonareModule` describes the
// raw module's free functions and does not carry bound-class handles, so the
// module is narrowed through this shape here — which is also where the
// arrangement surface is probed for, since a build without arrangement support
// registers neither the class nor its free functions.
export interface WasmProject {
  applyVocalEdit: (request: unknown) => unknown;
  getVocalEditDependencies: () => unknown;
  rehydrateVocalEdits: (originals: unknown, cancel?: unknown) => unknown;
  toJson: () => string;
  setSampleRate: (sampleRate: number) => void;
  addTrack: (desc: { kind?: number | string; name?: string }) => number;
  addClip: (desc: {
    trackId: number;
    isMidi?: boolean;
    startPpq?: number;
    lengthPpq: number;
    sourceOffsetPpq?: number;
    gain?: number;
    audio?: Float32Array;
    audioChannels?: number;
    audioSampleRate?: number;
    sourceUri?: string;
  }) => number;
  importExternalStems: (request: unknown) => { trackIds: number[]; clipIds: number[] };
  addLoopRecordingTakes: (desc: ProjectLoopRecordingDesc) => ProjectLoopRecordingResult;
  addMidiClip: (startPpq: number, lengthPpq: number) => ProjectMidiClipResult;
  splitClip: (clipId: number, splitPpq: number) => number;
  trimClip: (clipId: number, newStartPpq: number, newLengthPpq: number) => void;
  moveClip: (clipId: number, newStartPpq: number, newTrackId: number) => void;
  setTrackKind: (trackId: number, kind: number) => void;
  setClipWarpRef: (clipId: number, warpRefId: number) => void;
  setClipWarpMode: (clipId: number, mode: number) => void;
  setWarpMap: (map: ProjectWarpMapDesc) => void;
  removeWarpMap: (warpRefId: number) => void;
  setTrackMidiDestination: (trackId: number, destinationId: number) => void;
  setTrackGain: (trackId: number, gain: number) => void;
  setTrackMute: (trackId: number, mute: boolean) => void;
  setTrackSolo: (trackId: number, solo: boolean) => void;
  setTrackPan: (trackId: number, pan: number) => void;
  setPartRig: (
    destinationId: number,
    part: number,
    mode: PartRigMode,
    insertsJson: string | undefined,
  ) => void;
  getPartRig: (
    destinationId: number,
    part: number,
  ) => { mode: PartRigMode; insertsJson: string | null } | null;
  clearPartRig: (destinationId: number, part: number) => void;
  undo: () => void;
  redo: () => void;
  clearHistory: () => void;
  setMaxUndoDepth: (depth: number) => void;
  setMaxHistoryBytes: (bytes: number) => void;
  setMidiEvents: (clipId: number, events: ReadonlyArray<ProjectMidiEvent>) => void;
  importSmf: (data: Uint8Array) => number;
  exportSmf: () => Uint8Array<ArrayBuffer>;
  importClipFile: (data: Uint8Array) => number;
  exportClipFile: () => Uint8Array<ArrayBuffer>;
  setProgram: (
    clipId: number,
    program: number,
    bank: number,
    group: number | undefined,
    channel: number | undefined,
  ) => void;
  bakeMidiFx: {
    (clipId: number, configJson: string, withSourceIndex?: false | null): void;
    (clipId: number, configJson: string, withSourceIndex: true): Int32Array;
  };
  previewMidiFxCount: (clipId: number, configJson: string) => number;
  validateMidiNotes: (clipId: number) => ProjectNotePairValidation;
  transcribeToClip: (
    clipId: number,
    samples: Float32Array,
    sampleRate: number,
    config: TranscribeOptions,
  ) => number;
  analyzeTempo: (
    audio: Float32Array,
    sampleRate: number,
    options: ProjectTempoOptions | undefined,
  ) => NativeProjectTempoCandidate[];
  autoTempo: (
    audio: Float32Array,
    sampleRate: number,
    candidateIndex: number,
    applyTimeSignatures: boolean,
    options: ProjectTempoOptions | undefined,
  ) => number;
  snapToGrid: (ppq: number, strength: number, division: number) => number;
  compile: {
    (withTimeline: undefined): NativeProjectCompileResult;
    (withTimeline: true): NativeProjectCompileWithTimeline;
  };
  /**
   * Each trailing list binds one instrument kind (undefined binds none); only the callback
   * list can answer `{ instrumentFailure }`.
   */
  bounce: (
    options: NativeProjectBounceOptions,
    builtinBindings: ReadonlyArray<NativeInstrumentBinding<BuiltinSynthConfig>> | undefined,
    instrumentBindings: ReadonlyArray<NativeInstrumentBinding<ExternalInstrument>> | undefined,
    synthBindings: ReadonlyArray<NativeInstrumentBinding<NativeSynthBinding>> | undefined,
    sf2Bindings: ReadonlyArray<NativeInstrumentBinding<Sf2InstrumentConfig>> | undefined,
  ) => NativeProjectBounce | { instrumentFailure: unknown };
  loadSoundFont: (data: Uint8Array) => void;
  clearSoundFont: () => void;
  soundFontPresetCount: () => number;
  soundFontManifest: () => Sf2ProgramStatus[];
  removeClip: (clipId: number) => void;
  setClipGain: (clipId: number, gain: number) => void;
  setClipFade: (
    clipId: number,
    fadeIn: { lengthPpq: number; curve: number },
    fadeOut: { lengthPpq: number; curve: number },
  ) => void;
  unresolvedAudioSourceIds: () => number[];
  unresolvedAudioSources: () => NativeProjectSource[];
  setSourceAudio: (
    sourceId: number,
    audio: Float32Array,
    channels: number,
    sampleRate: number,
  ) => void;
  setAudioSourceMetadata: (sourceId: number, contentHash: string, externalStemRole: string) => void;
  setClipTakes: (
    clipId: number,
    takes: ReadonlyArray<ProjectClipTake>,
    activeTakeId: number,
  ) => void;
  setClipCompSegments: (clipId: number, segments: ReadonlyArray<ProjectClipCompSegment>) => void;
  setClipLoop: (
    clipId: number,
    loopMode: number,
    loopLengthPpq: number,
    loopCrossfadePpq: number,
  ) => void;
  setClipSource: (clipId: number, sourceId: number) => void;
  duplicateClip: (clipId: number, newStartPpq: number) => number;
  removeTrack: (trackId: number) => void;
  renameTrack: (trackId: number, name: string) => void;
  setTrackRoute: (trackId: number, channelStripRef: string, outputTarget: string) => void;
  addAutomationLane: (trackId: number, desc: NativeAutomationLaneDesc) => number;
  editAutomationLane: (
    trackId: number,
    targetParamId: number,
    desc: NativeAutomationLaneDesc,
  ) => void;
  removeAutomationLane: (trackId: number, targetParamId: number) => void;
  annotateKeys: (keys: ReadonlyArray<NativeKeySegment>) => void;
  annotateChords: (chords: ReadonlyArray<NativeChordSymbol>) => void;
  setAssistSidecar: (
    moduleId: string,
    schemaVersion: number,
    targetTrackId: number,
    regionStartPpq: number,
    regionEndPpq: number,
    payload: Uint8Array,
  ) => void;
  assistSidecarCount: () => number;
  getAssistSidecar: (index: number) => ProjectAssistSidecar;
  setOverlapPolicy: (policy: number) => void;
  getOverlapPolicy: () => number;
  getSampleRate: () => number;
  setMixerSceneJson: (sceneJson: string) => void;
  setMarker: (marker: {
    id: number;
    ppq: number;
    name: string;
    kind: number;
    keyFifths: number;
    keyMinor: boolean;
  }) => number;
  markerByIndex: (index: number) => NativeProjectMarker;
  trackByIndex: (index: number) => NativeProjectTrack;
  clipByIndex: (index: number) => NativeProjectClip;
  sourceByIndex: (index: number) => NativeProjectSource;
  markerCount: () => number;
  trackCount: () => number;
  clipCount: () => number;
  sourceCount: () => number;
  tempoSegmentCount: () => number;
  tempoSegmentByIndex: (index: number) => ProjectTempoSegment & { startSample?: number };
  timeSignatureByIndex: (index: number) => ProjectTimeSignatureSegment;
  timeSignatureCount: () => number;
  setTempoSegments: (segments: ReadonlyArray<ProjectTempoSegment>) => void;
  setTimeSignatures: (segments: ReadonlyArray<ProjectTimeSignatureSegment>) => void;
  lastBounceCompileResult: () => NativeProjectCompileResult & { hasTimeline: boolean };
  delete: () => void;
}

export interface ProjectModule {
  Project: {
    new (): WasmProject;
    fromJson: {
      (json: string, withDiagnostics?: false | null): WasmProject;
      (json: string, withDiagnostics: true): { project: WasmProject; diagnostics: string };
    };
  };
  SampleBank: { new (): WasmSampleBank };
  projectAbiVersion: () => number;
  synthPresetNames: () => string[];
  synthPresetPatch: (name: string) => SynthPatch;
  synthGsDrumKitName: (program: number) => string | null;
  synthGsDrumKitIsVoicedApart: (program: number) => number;
  synthGsVariationIsVoicedApart: (bank: number, program: number) => number;
  controllerProfileNames: () => string[];
  _synthEnumTables: () => SynthEnumTables;
  _synthPatchRoundTrip: (patch: SynthPatch) => SynthPatch;
  midiGmInstrumentName: (program: number) => string | null;
  midiGmProgramForName: (name: string) => number;
  midiGmFamilyName: (family: number) => string | null;
  midiGmFamilyFirstProgram: (family: number) => number;
  midiGm2InstrumentName: (bankLsb: number, program: number) => string | null;
  midiGmDrumName: (note: number) => string | null;
  midiGmDrumNoteForName: (name: string) => number;
  midiGm2DrumSetName: (bankLsb: number) => string | null;
  midiGm2DrumName: (bankLsb: number, note: number) => string | null;
  midiCcName: (controller: number) => string | null;
  midiCcIndexForName: (name: string) => number;
  midiPerNoteControllerName: (index: number) => string | null;
  midiBankProgram: (
    ppq: number,
    group: number,
    channel: number,
    bankMsb: number,
    bankLsb: number,
    program: number,
  ) => ProjectMidiEvent[];
  midiRouteEvents: (
    events: ReadonlyArray<ProjectMidiEvent>,
    config: ProjectMidiRouteConfig,
  ) => ProjectMidiRouteResult;
  midiCcLearn: (
    events: ReadonlyArray<ProjectMidiEvent>,
    paramId: number,
    minValue: number,
    maxValue: number,
    minMovement: number,
  ) => ProjectMidiCcBinding | null;
  midiCcToBreakpoint: (
    bindings: ReadonlyArray<ProjectMidiCcBinding>,
    event: ProjectMidiEvent,
  ) => { ppq: number; value: number; curveToNext: number } | null;
  midiParamToCc: (
    bindings: ReadonlyArray<ProjectMidiCcBinding>,
    paramId: number,
    unitValue: number,
    group: number,
    ppq: number,
  ) => ProjectMidiEvent | null;
  transcribe: (
    samples: Float32Array,
    sampleRate: number,
    tempoBpm: number | undefined,
    config: TranscribeOptions,
  ) => TranscribeResult;
  // Positional in the C ABI's own order minus the derived lengths and the out
  // parameters. The request object is passed as `config` as well, the way
  // `transcribe` passes its own: the reader only looks at the resolution keys.
  alignTakeToReference: (
    reference: Float32Array,
    take: Float32Array,
    sampleRate: number,
    takeSampleRate: number,
    config: AlignTakeToReferenceRequest,
  ) => AlignTakeToReferenceResult;
  // Numeric analysis ordinals in; the facade has already resolved names.
  chordSymbolFromAnalysis: (
    root: number,
    quality: number,
    bass: number,
  ) => { rootPc: number; quality: number; extensions: number[]; slashBassPc: number };
  keyModeFromAnalysis: (mode: number) => number;
}

export function projectModule(): ProjectModule {
  const candidate = getSonareModule() as unknown as Partial<ProjectModule>;
  if (typeof candidate.projectAbiVersion !== 'function' || candidate.Project === undefined) {
    throw codedError(
      ErrorCode.NotSupported,
      'libsonare was built without arrangement (headless DAW) support',
    );
  }
  return candidate as ProjectModule;
}

export function projectMidi1Event(
  ppq: number,
  group: number,
  status: number,
  channel: number,
  data1: number,
  data2 = 0,
): ProjectMidiEvent {
  assertEventPpq(ppq);
  const g = assertNibble(group, 'group');
  const ch = assertNibble(channel, 'channel');
  const d1 = assertU7(data1, 'data1');
  const d2 = assertU7(data2, 'data2');
  // UMP MIDI-1.0 channel-voice word (message type 0x2). Canonical layout is
  // sonare::midi::make_midi1_* (C-ABI sonare_midi_*, which Python delegates to);
  // this hand-written copy is locked against those words by the golden vectors
  // in project.test.ts (mirrored in the Node suite) so it cannot silently drift.
  const word = umpWord0(0x2, g, status, ch, d1, d2);
  return { ppq, word0: word, word1: 0 };
}

export function assertProjectMidiEvents(events: ReadonlyArray<ProjectMidiEvent>): void {
  if (!Array.isArray(events)) {
    throw refuseType(`events must be an array`);
  }
  requireListBudget(events, 'events');
  events.forEach((event: unknown, index) => {
    const prefix = `events[${index}]`;
    if (event === null || typeof event !== 'object' || Array.isArray(event)) {
      throw refuseType(`${prefix} must be a { ppq, word0, word1? } object`);
    }
    const record = event as ProjectMidiEvent;
    if (typeof record.ppq !== 'number') {
      throw refuseType(`${prefix}.ppq must be a number`);
    }
    if (typeof record.word0 !== 'number') {
      throw refuseType(`${prefix}.word0 must be an integer`);
    }
    if (record.word1 !== undefined && typeof record.word1 !== 'number') {
      throw refuseType(`${prefix}.word1 must be an integer`);
    }
    if (!(Number.isFinite(record.ppq) && record.ppq >= 0 && record.ppq <= MAX_PUBLIC_PPQ)) {
      throw refuse(`${prefix}.ppq must be finite and in [0, 1e12]`);
    }
    assertU32(record.word0, `${prefix}.word0`);
    if (record.word1 !== undefined) {
      assertU32(record.word1, `${prefix}.word1`);
    }
  });
}

/** A native ordinal back to its token; an ordinal outside the table is a binding defect. */

/**
 * An optional string field whose empty form would read as "not given": absent stays absent and
 * `""` is refused by name. `hint` finishes "omit it ...".
 */
export function optionalName(argName: string, value: unknown, hint: string): string | undefined {
  if (isAbsent(value)) {
    return undefined;
  }
  if (typeof value !== 'string') {
    throw refuseType(`${argName} must be a string`);
  }
  if (value === '') {
    throw refuse(`${argName} must not be empty; omit it ${hint}`);
  }
  return value;
}

/** The fields of a clip take and of a comp segment. */
const TAKE_KEYS = ['id', 'sourceId', 'sourceOffsetPpq', 'name'];
const COMP_SEGMENT_KEYS = ['startPpq', 'endPpq', 'takeId', 'crossfadePpq'];

/** The take list as the native layer reads it, each take checked by name; absent fields are missing keys. */
export function takesToNative(takes: unknown): Array<Record<string, unknown>> {
  if (!Array.isArray(takes)) {
    throw refuseType(`takes must be an array`);
  }
  requireListBudget(takes, 'takes');
  return takes.map((entry: unknown, index) => {
    const where = `takes[${index}]`;
    if (!isNonArrayObject(entry)) {
      throw refuseType(`${where} must be an object`);
    }
    assertDeclaredKeys(entry, TAKE_KEYS, where);
    const { id, sourceId, name, ...rest } = entry as Record<string, unknown>;
    if (typeof id !== 'number') {
      throw refuseType(`${where}.id must be an integer`);
    }
    if (id === 0) {
      throw refuse(`${where}.id must not be 0`);
    }
    const takeId = optionalId(`${where}.id`, id);
    const source = optionalId(`${where}.sourceId`, sourceId, "for the clip's source");
    const label = optionalName(`${where}.name`, name, 'for an unnamed take');
    return {
      ...rest,
      id: takeId,
      ...(source === undefined ? {} : { sourceId: source }),
      ...(label === undefined ? {} : { name: label }),
    };
  });
}

/** The comp segments as the native layer reads them; an absent `takeId` is a missing key. */
export function compSegmentsToNative(segments: unknown): Array<Record<string, unknown>> {
  if (!Array.isArray(segments)) {
    throw refuseType(`segments must be an array`);
  }
  requireListBudget(segments, 'segments');
  return segments.map((entry: unknown, index) => {
    if (!isNonArrayObject(entry)) {
      throw refuseType(`segments[${index}] must be an object`);
    }
    assertDeclaredKeys(entry, COMP_SEGMENT_KEYS, `segments[${index}]`);
    const { takeId, ...rest } = entry as Record<string, unknown>;
    const take = optionalId(`segments[${index}].takeId`, takeId, 'for the active take');
    return { ...rest, ...(take === undefined ? {} : { takeId: take }) };
  });
}

export function projectTrackKindValue(kind: ProjectTrackKind): number {
  return resolveEnumOrdinal(kind, TRACK_KINDS, 'kind');
}

export function projectTrackKindName(ordinal: number): ProjectTrackKind {
  return tokenName(TRACK_KINDS, ordinal, 'track kind');
}

export function projectSourceKindName(ordinal: number): ProjectSourceKind {
  return tokenName(SOURCE_KINDS, ordinal, 'source kind');
}

export function projectOverlapPolicyValue(policy: ProjectOverlapPolicy): number {
  return resolveEnumOrdinal(policy, OVERLAP_POLICIES, 'policy');
}

export function projectOverlapPolicyName(ordinal: number): ProjectOverlapPolicy {
  return tokenName(OVERLAP_POLICIES, ordinal, 'overlap policy');
}

/** A {@link ProjectKeySegment} as the native layer reads it. */
export type NativeKeySegment = Omit<ProjectKeySegment, 'mode'> & { mode?: number };

/** A {@link ProjectChordSymbol} as the native layer reads it. */
export type NativeChordSymbol = Omit<ProjectChordSymbol, 'quality'> & { quality?: number };

export function projectKeySegmentsToNative(
  keys: ReadonlyArray<ProjectKeySegment>,
): NativeKeySegment[] {
  requireListBudget(keys, 'keys');
  return keys.map((key) =>
    key.mode === undefined
      ? (key as NativeKeySegment)
      : { ...key, mode: resolveEnumOrdinal(key.mode, KEY_MODES, 'mode') },
  );
}

export function projectChordSymbolsToNative(
  chords: ReadonlyArray<ProjectChordSymbol>,
): NativeChordSymbol[] {
  requireListBudget(chords, 'chords');
  return chords.map((chord) =>
    chord.quality === undefined
      ? (chord as NativeChordSymbol)
      : {
          ...chord,
          quality: resolveEnumOrdinal(chord.quality, CHORD_QUALITY_CODES, 'quality'),
        },
  );
}

export function projectKeyModeName(ordinal: number): ProjectKeyMode {
  return tokenName(KEY_MODES, ordinal, 'key mode');
}

export function projectChordQualityName(ordinal: number): ProjectChordQuality {
  return tokenName(CHORD_QUALITY_CODES, ordinal, 'chord quality');
}

export function projectFadeCurveValue(
  argName: string,
  curve: ProjectFadeCurve | undefined,
): number {
  return resolveEnumOrdinal(isAbsent(curve) ? 'linear' : curve, FADE_CURVES, argName);
}

export function projectMarkerKindValue(kind: ProjectMarkerKind | undefined): number {
  return resolveEnumOrdinal(isAbsent(kind) ? 'marker' : kind, MARKER_KINDS, 'kind');
}

export function projectMarkerKindName(ordinal: number): ProjectMarkerKind {
  return tokenName(MARKER_KINDS, ordinal, 'marker kind');
}

export function projectStemLayoutValue(layout: unknown): number {
  return resolveEnumOrdinal(layout, STEM_LAYOUTS, 'stems[].layout');
}

/** One native diagnostic as the typed record, with no target when `0`. */
export function projectDiagnosticFromNative(native: NativeProjectDiagnostic): ProjectDiagnostic {
  const diagnostic = {
    code: native.code,
    severity: native.severity,
    message: native.message,
  };
  return native.targetId === 0 ? diagnostic : { ...diagnostic, targetId: native.targetId };
}

/** The typed compile result: diagnostics only. */
export function projectCompileResultFromNative(
  native: NativeProjectCompileResult,
): ProjectCompileResult {
  return { diagnostics: native.diagnostics.map(projectDiagnosticFromNative) };
}

/**
 * Planar audio for a native interleaved call: validates the planes and the
 * sample rate, and returns the interleaved buffer plus the channel count.
 */
export function interleaveChannels(
  channels: unknown,
  sampleRate: unknown,
): { audio: Float32Array; numChannels: number } {
  assertAudioChannels(channels, sampleRate);
  const planes = channels as Float32Array[];
  const frames = planes[0].length;
  if (planes.some((plane) => plane.length !== frames)) {
    throw refuse(`channels must all have the same length`);
  }
  return { audio: interleavePlanes(planes), numChannels: planes.length };
}

/** Native interleaved bounce output as the planar result. */
export function projectBounceResult(
  raw: NativeProjectBounce,
  sampleRate: number,
): ProjectBounceResult {
  const { interleaved, numChannels } = raw;
  return { channels: deinterleavePlanes(interleaved, numChannels), sampleRate };
}

/** A native source with its kind as a token. */
export function projectSourceFromNative(native: NativeProjectSource): ProjectSource {
  const { kind, contentHash, externalStemRole, ...rest } = native;
  return {
    ...rest,
    ...(contentHash === '' ? {} : { contentHash }),
    ...(externalStemRole === '' ? {} : { externalStemRole }),
    kind: projectSourceKindName(kind),
  };
}

/** The native tempo candidate with the meter's first segment under its public name. */
export function projectTempoCandidateFromNative(
  native: NativeProjectTempoCandidate,
): ProjectTempoCandidate {
  const { timeSignature, ...rest } = native;
  return { ...rest, firstTimeSignature: timeSignature };
}

/** One automation breakpoint as embind reads it: the curve as its ordinal. */
export type NativeAutomationPoint = Omit<ProjectAutomationPoint, 'curveToNext'> & {
  curveToNext: number;
};

/** An automation lane as embind reads it: target kind and curves as ordinals. */
export type NativeAutomationLaneDesc = Omit<ProjectAutomationLaneDesc, 'targetKind' | 'points'> & {
  targetKind?: number;
  points: NativeAutomationPoint[];
};

/** A breakpoint the native layer produced, with its curve ordinal named. */
export function projectAutomationPointFromNative(native: {
  ppq: number;
  value: number;
  curveToNext: number;
}): ProjectAutomationPoint {
  return {
    ...native,
    curveToNext: tokenName(AUTOMATION_CURVE_VALUES, native.curveToNext, 'automation curve'),
  };
}

/** The fields an automation breakpoint declares. */
export const AUTOMATION_POINT_KEYS = ['ppq', 'value', 'curveToNext'];

/** Resolve a breakpoint's curve token; a key outside the point's fields is refused by name. */
export function automationPointValue(
  point: ProjectAutomationPoint,
  index: number,
): NativeAutomationPoint {
  assertDeclaredKeys(point, AUTOMATION_POINT_KEYS, `points[${index}]`);
  const curveToNext = isAbsent(point.curveToNext)
    ? AUTOMATION_CURVE_VALUES.linear
    : resolveEnumOrdinal(
        point.curveToNext,
        AUTOMATION_CURVE_VALUES,
        `points[${index}].curveToNext`,
      );
  return { ...point, curveToNext };
}

/** Resolve an automation lane's tokens to the ordinals embind reads. */
export function projectAutomationLaneValue(
  desc: ProjectAutomationLaneDesc,
): NativeAutomationLaneDesc {
  if (desc.targetParamId === 0) {
    throw refuse('project automation lane targetParamId must be non-zero');
  }
  const { targetKind, points, ...rest } = desc;
  requireListBudget(points, 'points');
  return {
    ...rest,
    ...(targetKind === undefined
      ? {}
      : { targetKind: projectAutomationTargetKindValue(targetKind) }),
    points: points.map((point, index) => automationPointValue(point, index)),
  };
}

export function projectAutomationTargetKindValue(kind: ProjectAutomationTargetKind): number {
  return resolveEnumOrdinal(kind, AUTOMATION_TARGET_KINDS, 'targetKind');
}

export function projectWarpModeValue(mode: ProjectWarpMode): number {
  return resolveEnumOrdinal(mode, WARP_MODES, 'mode');
}

export function projectLoopModeValue(mode: ProjectLoopMode): number {
  return resolveEnumOrdinal(mode, LOOP_MODES, 'loopMode');
}

export function projectLoopModeName(ordinal: number): ProjectLoopMode {
  return tokenName(LOOP_MODES, ordinal, 'loop mode');
}
