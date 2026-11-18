import { AUTOMATION_CURVE_VALUES } from './codes.js';
import { ErrorCode, SonareError } from './errors.js';
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
import { releasedHandleError } from './internal/handle_guard.js';
import { deinterleavePlanes, interleavePlanes } from './internal/pod_config.js';
import { refuseSynthPatchOrdinals } from './internal/synth_tokens.js';
import { assertEventPpq, umpWord0 } from './internal/ump_events.js';
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
import { assertAudioChannels, assertNibble, assertU7, assertU32 } from './validation.js';

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

/** One diagnostic as the embind layer returns it: ordinals, with `0` for "no target". */
export interface NativeProjectDiagnostic {
  code: number;
  severity: number;
  targetId: number;
  message: string;
}

/** `compile` as the embind layer returns it, before the facade types it. */
export interface NativeProjectCompileResult {
  diagnostics: NativeProjectDiagnostic[];
}

/** `compileTimeline` as the embind layer returns it, before the facade wraps the handle. */
export type NativeProjectCompileTimelineResult = NativeProjectCompileResult & {
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
export function normalizeSynthInstrument(fnName: string, patch: unknown): NativeSynthBinding {
  if (patch === null || typeof patch !== 'object' || Array.isArray(patch)) {
    throw new TypeError(
      `${fnName}: patch must be a SynthPatch object; a preset is named by its preset field`,
    );
  }
  refuseSynthPatchOrdinals(fnName, patch);
  const { sampleBank, ...rest } = patch as SynthPatch;
  if (!sampleBank) {
    // Omitted, and at runtime an explicit null: both mean no bank, which a
    // sample patch renders as silence.
    return rest;
  }
  if (!(sampleBank instanceof SampleBank)) {
    throw new TypeError('sampleBank must be a SampleBank instance');
  }
  if ((sampleBank as unknown as { released: boolean }).released) {
    // The id outlives the handle, so without this the native registry would be
    // the only thing that notices — and with a different error class than Node.
    throw releasedHandleError('sampleBank', 'is destroyed');
  }
  return { ...rest, sampleBankId: sampleBank.nativeId };
}

/**
 * Resolve the `bounceWith*Instruments` binding argument to the list the native
 * layer reads. The argument is one `{ destinationId?, instrument }` pair or an
 * array of them (`null` and an empty array both mean no bindings). A pair
 * without an `instrument` key (a bare config) is refused by name, and so is a
 * `destinationId` placed inside the instrument, since it belongs to the pair.
 * `entry` converts each instrument to the shape the native reader takes; the
 * destination is merged in beside it, defaulting to `0`.
 */
export function normalizeInstrumentBindings<E, R extends object>(
  method: string,
  value: ReadonlyArray<ProjectInstrumentBinding<E>> | null | undefined,
  entry: (instrument: E) => R,
): NativeInstrumentBinding<R>[] {
  if (value === null || value === undefined) {
    return [];
  }
  if (!Array.isArray(value)) {
    throw new TypeError(
      `${method}: instruments must be an array of { destinationId?, instrument } pairs`,
    );
  }
  return value.map((pair: unknown, index: number) => {
    const label = `instruments[${index}]`;
    if (typeof pair !== 'object' || pair === null || Array.isArray(pair)) {
      throw new TypeError(`${method}: ${label} must be a { destinationId?, instrument } pair`);
    }
    const record = pair as Record<string, unknown>;
    for (const key of Object.keys(record)) {
      if (key !== 'destinationId' && key !== 'instrument') {
        const hint =
          'instrument' in record ? '' : ` (a bare config must be wrapped as { instrument })`;
        throw new TypeError(`${method}: ${label}: unknown request key '${key}'${hint}`);
      }
    }
    if (!('instrument' in record)) {
      throw new TypeError(`${method}: ${label} needs an 'instrument' key`);
    }
    const instrument = record.instrument;
    if (typeof instrument !== 'object' || instrument === null || Array.isArray(instrument)) {
      throw new TypeError(`${method}: ${label}.instrument must be an object`);
    }
    if ('destinationId' in instrument) {
      throw new TypeError(
        `${method}: ${label}.instrument: unknown request key 'destinationId' ('destinationId' belongs on the pair)`,
      );
    }
    const destinationId = optionalId(method, `${label}.destinationId`, record.destinationId, true);
    return { ...entry(instrument as E), destinationId: destinationId ?? 0 };
  });
}

/**
 * Serialize a part-rig request's inserts to the C ABI JSON form, refusing a
 * wrong shape by field name. Returns `undefined` when there are none.
 */
function partRigInsertsJson(inserts: PartRigInsert[] | undefined): string | undefined {
  if (inserts === undefined || inserts === null) {
    return undefined;
  }
  if (!Array.isArray(inserts)) {
    throw new TypeError('inserts must be an array');
  }
  return JSON.stringify(
    inserts.map((insert, index) => {
      if (typeof insert !== 'object' || insert === null) {
        throw new TypeError(`inserts[${index}] must be an object`);
      }
      if (typeof insert.processor !== 'string') {
        throw new TypeError(`inserts[${index}].processor must be a string`);
      }
      const { params } = insert;
      if (params === undefined) {
        return { processor: insert.processor, params: {} };
      }
      if (typeof params !== 'object' || params === null || Array.isArray(params)) {
        throw new TypeError(`inserts[${index}].params must be an object`);
      }
      return { processor: insert.processor, params };
    }),
  );
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
  method: string,
  requestOrDestinationId: PartRigRequest | PartRigKey | number,
  part?: number,
  mode?: PartRigMode,
  inserts?: PartRigInsert[],
): NormalizedPartRig {
  if (typeof requestOrDestinationId === 'object' && requestOrDestinationId !== null) {
    if (part !== undefined || mode !== undefined || inserts !== undefined) {
      throw new TypeError(`${method}: a request object takes no further arguments`);
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
  setProgram: (clipId: number, program: number, bank: number) => void;
  setProgramOnChannel: (
    clipId: number,
    group: number,
    channel: number,
    program: number,
    bank: number,
  ) => void;
  bakeMidiFx: (clipId: number, configJson: string) => void;
  bakeMidiFxWithSourceIndex: (clipId: number, configJson: string) => Int32Array;
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
  compile: () => NativeProjectCompileResult;
  compileTimeline: () => NativeProjectCompileTimelineResult;
  bounce: (options: NativeProjectBounceOptions) => NativeProjectBounce;
  bounceWithBuiltinInstrument: (
    bindings: ReadonlyArray<NativeInstrumentBinding<BuiltinSynthConfig>>,
    options: NativeProjectBounceOptions,
  ) => NativeProjectBounce;
  bounceWithInstruments: (
    bindings: ReadonlyArray<NativeInstrumentBinding<ExternalInstrument>>,
    options: NativeProjectBounceOptions,
  ) => NativeProjectBounce | { instrumentFailure: unknown };
  bounceWithSynthInstrument: (
    bindings: ReadonlyArray<NativeInstrumentBinding<NativeSynthBinding>>,
    options: NativeProjectBounceOptions,
  ) => NativeProjectBounce;
  loadSoundFont: (data: Uint8Array) => void;
  clearSoundFont: () => void;
  soundFontPresetCount: () => number;
  soundFontManifest: () => Sf2ProgramStatus[];
  bounceWithSf2Instrument: (
    bindings: ReadonlyArray<NativeInstrumentBinding<Sf2InstrumentConfig>>,
    options: NativeProjectBounceOptions,
  ) => NativeProjectBounce;
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
  setMarkerEx: (marker: {
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
    fromJson: (json: string) => WasmProject;
    fromJsonWithDiagnostics: (json: string) => { project: WasmProject; diagnostics: string };
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
    throw new SonareError(
      ErrorCode.NotSupported,
      'libsonare was built without arrangement (headless DAW) support',
    );
  }
  return candidate as ProjectModule;
}

export function projectMidi1Event(
  fnName: string,
  ppq: number,
  group: number,
  status: number,
  channel: number,
  data1: number,
  data2 = 0,
): ProjectMidiEvent {
  assertEventPpq(fnName, ppq);
  const g = assertNibble(fnName, group, 'group');
  const ch = assertNibble(fnName, channel, 'channel');
  const d1 = assertU7(fnName, data1, 'data1');
  const d2 = assertU7(fnName, data2, 'data2');
  // UMP MIDI-1.0 channel-voice word (message type 0x2). Canonical layout is
  // sonare::midi::make_midi1_* (C-ABI sonare_midi_*, which Python delegates to);
  // this hand-written copy is locked against those words by the golden vectors
  // in project.test.ts (mirrored in the Node suite) so it cannot silently drift.
  const word = umpWord0(0x2, g, status, ch, d1, d2);
  return { ppq, word0: word, word1: 0 };
}

export function assertProjectMidiEvents(
  fnName: string,
  events: ReadonlyArray<ProjectMidiEvent>,
): void {
  if (!Array.isArray(events)) {
    throw new TypeError(`${fnName}: events must be an array`);
  }
  events.forEach((event: unknown, index) => {
    const prefix = `events[${index}]`;
    if (event === null || typeof event !== 'object' || Array.isArray(event)) {
      throw new TypeError(`${fnName}: ${prefix} must be a { ppq, word0, word1? } object`);
    }
    const record = event as ProjectMidiEvent;
    for (const key of ['ppq', 'word0'] as const) {
      if (typeof record[key] !== 'number') {
        throw new TypeError(`${fnName}: ${prefix}.${key} must be a number`);
      }
    }
    if (record.word1 !== undefined && typeof record.word1 !== 'number') {
      throw new TypeError(`${fnName}: ${prefix}.word1 must be a number`);
    }
    if (!Number.isFinite(record.ppq) || record.ppq < 0) {
      throw new RangeError(`${fnName}: ${prefix}.ppq must be a non-negative finite number`);
    }
    assertU32(fnName, record.word0, `${prefix}.word0`);
    if (record.word1 !== undefined) {
      assertU32(fnName, record.word1, `${prefix}.word1`);
    }
  });
}

const TRACK_KINDS = { audio: 0, midi: 1, aux: 2 } as const;
const SOURCE_KINDS = { audio: 0, midi: 1 } as const;
const OVERLAP_POLICIES = { disallow: 0, allow: 1 } as const;
const FADE_CURVES = { linear: 0, equalPower: 1, exponential: 2, logarithmic: 3 } as const;
const LOOP_MODES = { off: 0, loop: 1 } as const;
const WARP_MODES = { off: 0, repitch: 1, tempoSync: 2, timeStretch: 3 } as const;
const AUTOMATION_TARGET_KINDS = { opaque: 0, trackFaderDb: 1, trackPan: 2 } as const;
const MARKER_KINDS = { marker: 0, text: 1, lyric: 2, cuePoint: 3, keySignature: 4 } as const;
const STEM_LAYOUTS = { mono: 1, stereo: 2 } as const;
const KEY_MODES = {
  unknown: 0,
  major: 1,
  minor: 2,
  dorian: 3,
  phrygian: 4,
  lydian: 5,
  mixolydian: 6,
  locrian: 7,
} as const satisfies Record<ProjectKeyMode, number>;
const CHORD_QUALITY_CODES = {
  unknown: 0,
  major: 1,
  minor: 2,
  diminished: 3,
  augmented: 4,
  dominant: 5,
  halfDiminished: 6,
  suspended: 7,
} as const satisfies Record<ProjectChordQuality, number>;
const DIAGNOSTIC_CODES = {
  ok: 0,
  danglingSourceRef: 1,
  clipOverlap: 2,
  invalidTempo: 3,
  invalidPpq: 4,
  unsupportedGraph: 5,
  unsupportedMixing: 6,
  emptyAudioSource: 7,
  invalidSampleRate: 8,
  sourceKindMismatch: 9,
  midiClipNoInstrument: 10,
  raggedAudioSource: 11,
  loopCrossfadeUnavailable: 12,
  sharedChannelStrip: 13,
  automationLaneConflict: 14,
  invalidAutomationTargetKind: 15,
  automationLaneCapacity: 16,
  reservedAutomationRouteConflict: 17,
  automationTargetUnrouted: 18,
} as const satisfies Record<ProjectDiagnosticCode, number>;

/**
 * A closed-set token to its native ordinal. A non-string is a `TypeError` (an
 * ordinal included) and a string outside the set is a `RangeError`; an absent
 * value takes `fallback` when one is given.
 */
function tokenCode(
  fnName: string,
  argName: string,
  value: unknown,
  table: Readonly<Record<string, number>>,
  fallback?: string,
): number {
  const token = value === undefined || value === null ? fallback : value;
  const accepted = Object.keys(table)
    .map((key) => `'${key}'`)
    .join(', ');
  if (typeof token !== 'string') {
    throw new TypeError(`${fnName}: ${argName} must be one of ${accepted}`);
  }
  // biome-ignore lint/suspicious/noPrototypeBuiltins: Object.hasOwn is newer than the ES2020 target.
  if (!Object.prototype.hasOwnProperty.call(table, token)) {
    throw new RangeError(`${fnName}: ${argName} must be one of ${accepted} (got '${token}')`);
  }
  return table[token];
}

/** A native ordinal back to its token; an ordinal outside the table is a binding defect. */
function tokenName<T extends string>(
  table: Readonly<Record<T, number>>,
  ordinal: number,
  what: string,
): T {
  const name = (Object.keys(table) as T[]).find((key) => table[key] === ordinal);
  if (name === undefined) {
    throw new RangeError(`unknown native ${what} ordinal ${String(ordinal)}`);
  }
  return name;
}

/**
 * An optional id: absent (`undefined` / `null`) is `undefined`, an explicit `0`
 * is refused because native reads it as "not given", and anything else must be
 * a positive 32-bit integer. `allowZero` is for a destination id, where `0` is a
 * real destination.
 */
export function optionalId(
  fnName: string,
  argName: string,
  value: unknown,
  allowZero = false,
): number | undefined {
  if (value === undefined || value === null) {
    return undefined;
  }
  if (typeof value !== 'number') {
    throw new TypeError(`${fnName}: ${argName} must be a number`);
  }
  if (value === 0 && !allowZero) {
    throw new RangeError(`${fnName}: ${argName} must not be 0 (omit it for the default)`);
  }
  if (!Number.isInteger(value) || value < 0 || value > 0xffff_ffff) {
    throw new RangeError(`${fnName}: ${argName} must be an unsigned 32-bit integer`);
  }
  return value;
}

/** An optional strictly positive integer count: absent is `undefined`, an explicit `0` is refused. */
export function optionalPositiveInt(
  fnName: string,
  argName: string,
  value: unknown,
): number | undefined {
  if (value === undefined || value === null) {
    return undefined;
  }
  if (typeof value !== 'number') {
    throw new TypeError(`${fnName}: ${argName} must be a number`);
  }
  if (value === 0) {
    throw new RangeError(`${fnName}: ${argName} must not be 0 (omit it for the default)`);
  }
  if (!Number.isInteger(value) || value < 0) {
    throw new RangeError(`${fnName}: ${argName} must be a positive integer`);
  }
  return value;
}

export function projectTrackKindValue(kind: ProjectTrackKind, fnName: string): number {
  return tokenCode(fnName, 'kind', kind, TRACK_KINDS);
}

export function projectTrackKindName(ordinal: number): ProjectTrackKind {
  return tokenName(TRACK_KINDS, ordinal, 'track kind');
}

export function projectSourceKindName(ordinal: number): ProjectSourceKind {
  return tokenName(SOURCE_KINDS, ordinal, 'source kind');
}

export function projectOverlapPolicyValue(policy: ProjectOverlapPolicy): number {
  return tokenCode('setOverlapPolicy', 'policy', policy, OVERLAP_POLICIES);
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
  return keys.map((key) =>
    key.mode === undefined
      ? (key as NativeKeySegment)
      : { ...key, mode: tokenCode('annotateKeys', 'mode', key.mode, KEY_MODES) },
  );
}

export function projectChordSymbolsToNative(
  chords: ReadonlyArray<ProjectChordSymbol>,
): NativeChordSymbol[] {
  return chords.map((chord) =>
    chord.quality === undefined
      ? (chord as NativeChordSymbol)
      : {
          ...chord,
          quality: tokenCode('annotateChords', 'quality', chord.quality, CHORD_QUALITY_CODES),
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
  fnName: string,
  argName: string,
  curve: ProjectFadeCurve | undefined,
): number {
  return tokenCode(fnName, argName, curve, FADE_CURVES, 'linear');
}

export function projectMarkerKindValue(
  fnName: string,
  kind: ProjectMarkerKind | undefined,
): number {
  return tokenCode(fnName, 'kind', kind, MARKER_KINDS, 'marker');
}

export function projectMarkerKindName(ordinal: number): ProjectMarkerKind {
  return tokenName(MARKER_KINDS, ordinal, 'marker kind');
}

export function projectStemLayoutValue(fnName: string, layout: unknown): number {
  return tokenCode(fnName, 'stems[].layout', layout, STEM_LAYOUTS);
}

/** One native diagnostic as the typed record: tokens for code and severity, no target when `0`. */
export function projectDiagnosticFromNative(native: NativeProjectDiagnostic): ProjectDiagnostic {
  const diagnostic = {
    code: tokenName(DIAGNOSTIC_CODES, native.code, 'diagnostic code'),
    severity: native.severity === 0 ? ('error' as const) : ('warning' as const),
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
  fnName: string,
  channels: unknown,
  sampleRate: unknown,
): { audio: Float32Array; numChannels: number } {
  assertAudioChannels(fnName, channels, sampleRate);
  const planes = channels as Float32Array[];
  const frames = planes[0].length;
  if (planes.some((plane) => plane.length !== frames)) {
    throw new RangeError(`${fnName}: channels must all have the same length`);
  }
  return { audio: interleavePlanes(planes), numChannels: planes.length };
}

/** Native interleaved bounce output as the planar result. */
export function projectBounceResult(
  raw: NativeProjectBounce,
  sampleRate: number,
): ProjectBounceResult {
  const { interleaved, numChannels } = raw;
  return { channels: deinterleavePlanes(interleaved, numChannels), sampleRate, numChannels };
}

/** A native source with its kind as a token. */
export function projectSourceFromNative(native: NativeProjectSource): ProjectSource {
  const { kind, ...rest } = native;
  return { ...rest, kind: projectSourceKindName(kind) };
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

const AUTOMATION_POINT_KEYS: ReadonlySet<string> = new Set(['ppq', 'value', 'curveToNext']);

/** Resolve a breakpoint's curve token; a key outside the point's fields is refused by name. */
export function projectAutomationPointValue(
  point: ProjectAutomationPoint,
  fnName: string,
): NativeAutomationPoint {
  for (const key of Object.keys(point)) {
    if (!AUTOMATION_POINT_KEYS.has(key)) {
      throw new TypeError(`${fnName}: unknown option '${key}' in 'points'`);
    }
  }
  const curveToNext = tokenCode(
    fnName,
    'curveToNext',
    point.curveToNext,
    AUTOMATION_CURVE_VALUES,
    'linear',
  );
  return { ...point, curveToNext };
}

/** Resolve an automation lane's tokens to the ordinals embind reads. */
export function projectAutomationLaneValue(
  desc: ProjectAutomationLaneDesc,
  fnName: string,
): NativeAutomationLaneDesc {
  if (desc.targetParamId === 0) {
    throw new RangeError('project automation lane targetParamId must be non-zero');
  }
  const { targetKind, points, ...rest } = desc;
  return {
    ...rest,
    ...(targetKind === undefined
      ? {}
      : { targetKind: projectAutomationTargetKindValue(targetKind, fnName) }),
    points: points.map((point) => projectAutomationPointValue(point, fnName)),
  };
}

export function projectAutomationTargetKindValue(
  kind: ProjectAutomationTargetKind,
  fnName: string,
): number {
  return tokenCode(fnName, 'targetKind', kind, AUTOMATION_TARGET_KINDS);
}

export function projectWarpModeValue(mode: ProjectWarpMode): number {
  return tokenCode('setClipWarpMode', 'mode', mode, WARP_MODES);
}

export function projectLoopModeValue(mode: ProjectLoopMode): number {
  return tokenCode('setClipLoop', 'loopMode', mode, LOOP_MODES);
}

export function projectLoopModeName(ordinal: number): ProjectLoopMode {
  return tokenName(LOOP_MODES, ordinal, 'loop mode');
}
