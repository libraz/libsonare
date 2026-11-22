import {
  entryClass,
  invalidState,
  refuse,
  refuseType,
  refuseUnknownKey,
} from './internal/entry.js';
import { assertHandleLive } from './internal/handle_guard.js';
import { midiFxConfigJson } from './internal/midi_fx_config.js';
import { deinterleave, interleavePlanar } from './internal/planar.js';
import { setTimelineHandle } from './internal/project_handles.js';
import {
  AUTOMATION_TARGET_KIND,
  CHORD_QUALITY,
  FADE_CURVE,
  KEY_MODE,
  LOOP_MODE,
  MARKER_KIND,
  OVERLAP_POLICY,
  SOURCE_KIND,
  STEM_LAYOUT,
  TRACK_KIND,
  tokenName,
  tokenOrdinal,
  WARP_MODE,
} from './internal/project_tokens.js';
import { refuseBuiltinSynthOrdinals } from './internal/synth_tokens.js';
import { isAbsent, isNonArrayObject } from './internal/value_guards.js';
import { addon } from './native.js';
import type {
  ExternalSeparatedStemImportRequest,
  ExternalSeparatedStemImportResult,
  PartRigEntry,
  PartRigKey,
  PartRigMode,
  PartRigRequest,
  ProjectAnalyzeTempoRequest,
  ProjectAssistSidecar,
  ProjectAssistSidecarInput,
  ProjectAudioSourceMetadataRequest,
  ProjectAutomationLaneDesc,
  ProjectAutoTempoOptions,
  ProjectAutoTempoRequest,
  ProjectBounceOptions,
  ProjectBounceResult,
  ProjectChordSymbol,
  ProjectClip,
  ProjectClipCompSegment,
  ProjectClipDesc,
  ProjectClipFadeRequest,
  ProjectClipLoopRequest,
  ProjectClipMoveOptions,
  ProjectClipTake,
  ProjectClipTakesOptions,
  ProjectCompileOptions,
  ProjectCompileResult,
  ProjectDiagnostic,
  ProjectInstrumentBinding,
  ProjectKeySegment,
  ProjectLoopRecordingDesc,
  ProjectLoopRecordingResult,
  ProjectMarker,
  ProjectMarkerInput,
  ProjectMidiClipResult,
  ProjectMidiEvent,
  ProjectMidiFxBakeRequest,
  ProjectMidiFxBakeResult,
  ProjectMidiFxPreviewRequest,
  ProjectNotePairValidation,
  ProjectOverlapPolicy,
  ProjectProgramOptions,
  ProjectSource,
  ProjectSourceAudioRequest,
  ProjectTempoCandidate,
  ProjectTempoOptions,
  ProjectTempoSegment,
  ProjectTimeSignatureSegment,
  ProjectTrack,
  ProjectTrackDesc,
  ProjectTrackKind,
  ProjectTranscribeRequest,
  ProjectWarpMode,
  Sf2ProgramStatus,
  SynthPatch,
} from './types.js';
import { PART_RIG_MODES } from './types_instrument.js';
import type {
  ProjectVocalEditApplyRequest,
  ProjectVocalEditApplyResult,
  ProjectVocalEditDependency,
  ProjectVocalOriginalSource,
  ProjectVocalRehydrateItem,
  ProjectVocalRehydrateOptions,
} from './types_vocal_project.js';
import {
  assertAudioInput,
  assertClosedToken,
  assertProjectMidiEvents,
  assertSampleRate,
  assertSamples,
  assertU7,
  assertU32,
  C_INT_MAX,
  requestObject,
  requireBoolean,
  sampleOneShot,
} from './validation.js';
import {
  type NativeAutomationLaneDesc,
  normalizePartRig,
  normalizeSynthInstrument,
  projectAutomationLaneValue,
} from './value_coercion.js';
import {
  projectApplyVocalEdit,
  projectGetVocalEditDependencies,
  projectRehydrateVocalEdits,
} from './vocal_project.js';

export * from './synth_catalog.js';
export * from './take_alignment.js';

/**
 * ABI version of the flat project layout in the loaded native binding. `undefined` when the
 * build has no project subsystem; check `capabilities().features` for presence.
 */
export function projectAbiVersion(): number | undefined {
  const version = (addon.projectAbiVersion as (() => number) | undefined)?.() ?? 0;
  return version > 0 ? version : undefined;
}

type NativeProject = InstanceType<typeof addon.Project>;

/** Carries a native handle the module adopts instead of allocating a fresh project. */
class AdoptedProject {
  constructor(readonly native: NativeProject) {}
}

/** Proves a {@link ProjectTimeline} is built by this module. */
const TIMELINE_TOKEN = Symbol('ProjectTimeline');

function requireNumber(value: unknown, argName: string): number {
  if (typeof value !== 'number') {
    throw refuseType(`${argName} must be a number`);
  }
  return value;
}

function requireInteger(value: unknown, argName: string): number {
  if (typeof value !== 'number') {
    throw refuseType(`${argName} must be an integer`);
  }
  return value;
}

/** An optional field where `0` would mean "the default": absent stays absent, `0` is refused. */
function optionalNonZero(value: unknown, argName: string, max: number): number | undefined {
  if (isAbsent(value)) {
    return undefined;
  }
  const number = requireInteger(value, argName);
  if (number === 0) {
    throw refuse(`${argName} must not be 0; omit it for the default`);
  }
  if (!Number.isInteger(number) || number < 1 || number > max) {
    throw refuse(`${argName} must be an integer in [1, ${max}]`);
  }
  return number;
}

/**
 * An optional id whose `0` would read as "not given": absent (`undefined` or `null`) stays
 * absent, and an explicit `0` is refused by name.
 */
function optionalId(argName: string, value: unknown, hint = 'for the default'): number | undefined {
  if (isAbsent(value)) {
    return undefined;
  }
  const number = requireInteger(value, argName);
  assertU32(number, argName);
  if (number === 0) {
    throw refuse(`${argName} must not be 0; omit it ${hint}`);
  }
  return number;
}

/** An optional string field whose empty form would read as "not given": absent stays absent, `""` is refused. */
function optionalName(argName: string, value: unknown, hint: string): string | undefined {
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

/** Refuse the first key of `record` outside `keys`, by its path. */
function refuseUnknownFields(record: object, where: string, keys: readonly string[]): void {
  for (const key of Object.keys(record)) {
    if (!keys.includes(key)) {
      throw refuseUnknownKey(`${where}.${key}`, keys);
    }
  }
}

/** The take list as the native layer reads it, each take checked by name; absent fields are missing keys. */
function takesToNative(takes: unknown): Array<Record<string, unknown>> {
  if (!Array.isArray(takes)) {
    throw refuseType(`takes must be an array`);
  }
  return takes.map((entry: unknown, index) => {
    const where = `takes[${index}]`;
    if (!isNonArrayObject(entry)) {
      throw refuseType(`${where} must be an object`);
    }
    refuseUnknownFields(entry, where, TAKE_KEYS);
    const { id, sourceId, name, ...rest } = entry as Record<string, unknown>;
    const takeId = requireInteger(id, `${where}.id`);
    assertU32(takeId, `${where}.id`);
    if (takeId === 0) {
      throw refuse(`${where}.id must not be 0`);
    }
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
function compSegmentsToNative(segments: unknown): Array<Record<string, unknown>> {
  if (!Array.isArray(segments)) {
    throw refuseType(`segments must be an array`);
  }
  return segments.map((entry: unknown, index) => {
    if (!isNonArrayObject(entry)) {
      throw refuseType(`segments[${index}] must be an object`);
    }
    refuseUnknownFields(entry, `segments[${index}]`, COMP_SEGMENT_KEYS);
    const { takeId, ...rest } = entry as Record<string, unknown>;
    const take = optionalId(`segments[${index}].takeId`, takeId, 'for the active take');
    return { ...rest, ...(take === undefined ? {} : { takeId: take }) };
  });
}

/** Render settings the native bounce reads. */
function bounceSettings(request: ProjectBounceOptions): Record<string, unknown> {
  const native: Record<string, unknown> = {};
  const totalFrames = optionalNonZero(request.totalFrames, 'totalFrames', Number.MAX_SAFE_INTEGER);
  const blockSize = optionalNonZero(request.blockSize, 'blockSize', C_INT_MAX);
  const numChannels = optionalNonZero(request.numChannels, 'numChannels', C_INT_MAX);
  if (totalFrames !== undefined) {
    native.totalFrames = totalFrames;
  }
  if (blockSize !== undefined) {
    native.blockSize = blockSize;
  }
  if (numChannels !== undefined) {
    native.numChannels = numChannels;
  }
  if (!isAbsent(request.instrumentLatencySamples)) {
    native.instrumentLatencySamples = request.instrumentLatencySamples;
  }
  return native;
}

/**
 * Resolve one instrument list of a bounce to the list the native
 * layer reads: each pair's `destinationId` is folded into a copy of its instrument.
 * Anything but an array of pairs is a `TypeError`.
 */
function bindInstruments<T extends object>(
  list: string,
  value: readonly ProjectInstrumentBinding<T>[],
  entry: (instrument: T) => T,
  withDestination: (instrument: T, destinationId: number) => T,
): T[] {
  if (!Array.isArray(value)) {
    throw refuseType(`${list} must be an array of { destinationId?, instrument } pairs`);
  }
  return value.map((pair: unknown, index: number) => {
    const label = `${list}[${index}]`;
    const binding = instrumentBinding(pair, label);
    const instrument = binding.instrument as T;
    const resolved = entry(instrument);
    if (isAbsent(binding.destinationId)) {
      return resolved;
    }
    const where = `${label}.destinationId`;
    assertU32(requireInteger(binding.destinationId, where), where);
    return withDestination(resolved, binding.destinationId as number);
  });
}

/** The keys of a `{ destinationId?, instrument }` pair. */
const INSTRUMENT_BINDING_KEYS = ['destinationId', 'instrument'];

/**
 * One `{ destinationId?, instrument }` pair, refused by its path: an undeclared key, a
 * missing or non-object instrument, and a `destinationId` given inside the instrument
 * instead of beside it.
 */
function instrumentBinding(pair: unknown, label: string): Record<string, unknown> {
  if (!isNonArrayObject(pair)) {
    throw refuseType(`${label} must be a { destinationId?, instrument } pair`);
  }
  const record = pair as Record<string, unknown>;
  for (const key of Object.keys(record)) {
    if (!INSTRUMENT_BINDING_KEYS.includes(key)) {
      throw refuseUnknownKey(`${label}.${key}`, INSTRUMENT_BINDING_KEYS);
    }
  }
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
  return record;
}

const withDestinationCopy = <T extends object>(instrument: T, destinationId: number): T => ({
  ...instrument,
  destinationId,
});

/** Same as {@link withDestinationCopy} but keeps the instrument's prototype callbacks reachable. */
const withDestinationLayer = <T extends object>(instrument: T, destinationId: number): T =>
  Object.assign(Object.create(instrument) as T, { destinationId });

/** A native bounce: interleaved samples and the width the render was mixed at. */
interface NativeBounce {
  interleaved: Float32Array;
  numChannels: number;
}

function planarBounce(raw: NativeBounce, sampleRate: number): ProjectBounceResult {
  const { interleaved, numChannels } = raw;
  return { channels: deinterleave(interleaved, numChannels), sampleRate };
}

function diagnosticsFromNative(raw: unknown): ProjectDiagnostic[] {
  const list = raw as Array<{
    code: ProjectDiagnostic['code'];
    severity: ProjectDiagnostic['severity'];
    targetId: number;
    message: string;
  }>;
  return list.map((d) => ({
    code: d.code,
    severity: d.severity,
    ...(d.targetId === 0 ? {} : { targetId: d.targetId }),
    message: d.message,
  }));
}

/** Copy of an addon record under the key set the public type declares. */
function trackFromNative(raw: Record<string, unknown>): ProjectTrack {
  return { ...raw, kind: tokenName(TRACK_KIND, raw.kind as number, 'track kind') } as ProjectTrack;
}

/**
 * A clip loop length as the native layer reads it: an omitted length is its whole-clip `0`, and an
 * explicit `0` is refused so that omission is the one spelling of a whole-clip loop.
 */
function wholeClipLoopLength(value: unknown): number {
  if (isAbsent(value)) {
    return 0;
  }
  const length = requireNumber(value, 'loopLengthPpq');
  if (length === 0) {
    throw refuse('loopLengthPpq must not be 0; omit it to loop the whole clip');
  }
  return length;
}

/** A clip record; a whole-clip loop, or no loop, reads back without `loopLengthPpq`. */
function clipFromNative(raw: Record<string, unknown>): ProjectClip {
  const { loopLengthPpq, ...rest } = raw;
  return {
    ...rest,
    ...(loopLengthPpq === 0 ? {} : { loopLengthPpq }),
    sourceKind: tokenName(SOURCE_KIND, raw.sourceKind as number, 'source kind'),
    loopMode: tokenName(LOOP_MODE, raw.loopMode as number, 'loop mode'),
  } as ProjectClip;
}

/** An unset metadata string reads back as an absent key, so a source record never carries `""`. */
function sourceFromNative(raw: Record<string, unknown>): ProjectSource {
  const { contentHash, externalStemRole, ...rest } = raw;
  return {
    ...rest,
    ...(contentHash === '' ? {} : { contentHash }),
    ...(externalStemRole === '' ? {} : { externalStemRole }),
    kind: tokenName(SOURCE_KIND, raw.kind as number, 'source kind'),
  } as ProjectSource;
}

function markerFromNative(raw: Record<string, unknown>): ProjectMarker {
  return {
    id: raw.id,
    ppq: raw.ppq,
    ...(raw.name === '' ? {} : { name: raw.name }),
    kind: tokenName(MARKER_KIND, raw.kind as number, 'marker kind'),
    keyFifths: raw.keyFifths,
    keyMinor: raw.keyMinor,
  } as ProjectMarker;
}

function tempoOptionsToNative(request: ProjectTempoOptions): ProjectTempoOptions | undefined {
  const native: Record<string, unknown> = {};
  for (const key of [
    'adaptiveTempo',
    'tempoUpdateIntervalBeats',
    'rampThreshold',
    'includeOctaveCandidates',
  ] as const) {
    if (!isAbsent(request[key])) {
      native[key] = request[key];
    }
  }
  return Object.keys(native).length === 0 ? undefined : (native as ProjectTempoOptions);
}

function tempoCandidateFromNative(raw: Record<string, unknown>): ProjectTempoCandidate {
  const { timeSignature, ...rest } = raw;
  return { ...rest, firstTimeSignature: timeSignature } as unknown as ProjectTempoCandidate;
}

function automationLaneToNative(desc: ProjectAutomationLaneDesc): NativeAutomationLaneDesc {
  const { targetKind, ...rest } = desc;
  const resolved =
    targetKind === undefined
      ? rest
      : {
          ...rest,
          targetKind: tokenOrdinal(targetKind, AUTOMATION_TARGET_KIND, 'targetKind'),
        };
  return projectAutomationLaneValue(
    resolved as Omit<ProjectAutomationLaneDesc, 'targetKind'> & { targetKind?: 0 | 1 | 2 },
  );
}

/**
 * Folds the request forms of `bakeMidiFx` into the native argument shape,
 * refusing an unknown key by name.
 */
function midiFxBakeRequest(request: ProjectMidiFxBakeRequest): {
  clipId: number;
  configJson: string;
  withSourceIndex: boolean;
} {
  const record = requestObject('bakeMidiFx', request, 'request', true);
  return {
    clipId: record.clipId,
    configJson: midiFxConfigJson(record.config),
    withSourceIndex: record.withSourceIndex ?? false,
  };
}

/** Interleave planar loop-recording audio; an interleaved array passes through. */
function interleaveLoopRecording(desc: ProjectLoopRecordingDesc): ProjectLoopRecordingDesc {
  const { audio } = desc;
  if (!Array.isArray(audio)) {
    return desc;
  }
  if ((desc as { audioChannels?: unknown }).audioChannels !== undefined) {
    throw refuseType(
      'audioChannels must be omitted for planar audio; the channel count is the number of arrays',
    );
  }
  const { data, numChannels } = interleavePlanar(audio, 'audio');
  return { ...desc, audio: data, audioChannels: numChannels };
}

let createTimeline: (native: InstanceType<typeof addon.ProjectTimeline>) => ProjectTimeline;

/**
 * Immutable compiled playback snapshot of a {@link Project}, produced by
 * {@link Project.compile} with `withTimeline: true` and installed with
 * {@link RealtimeEngine.applyProjectTimeline}. The native handle is not
 * garbage-collected, so release it with {@link destroy} (or `using`). It cannot
 * be constructed directly.
 */
@entryClass('ProjectTimeline')
export class ProjectTimeline {
  private disposed = false;

  private constructor(token: symbol, native: InstanceType<typeof addon.ProjectTimeline>) {
    if (token !== TIMELINE_TOKEN) {
      throw refuseType(
        'ProjectTimeline cannot be constructed; use Project.compile({ withTimeline: true })',
      );
    }
    this.nativeTimeline = native;
    setTimelineHandle(this, native);
  }

  private readonly nativeTimeline: InstanceType<typeof addon.ProjectTimeline>;

  /**
   * Release the native timeline. Idempotent. An engine the timeline was
   * applied to keeps its own reference, so this is safe right after
   * {@link RealtimeEngine.applyProjectTimeline}.
   */
  destroy(): void {
    if (this.disposed) {
      return;
    }
    this.disposed = true;
    this.nativeTimeline.destroy();
  }

  /**
   * Releases the native timeline; lets `using` free it automatically (needs TypeScript 5.2+
   * or a runtime with native explicit resource management; Node 22 does not parse `using`).
   */
  [Symbol.dispose](): void {
    this.destroy();
  }

  static {
    createTimeline = (native) => new ProjectTimeline(TIMELINE_TOKEN, native);
  }
}

/**
 * Headless arrangement / DAW project (the curated `sonare_project_*` C ABI).
 *
 * Wraps an opaque native project handle over the arrangement control plane
 * (EditHistory + the offline compiler/bounce, serializer, and MIR tempo/grid
 * bridges). Every method is control-thread-only and performs no file or device
 * I/O: project JSON and SMF bytes are exchanged in memory. All mutation routes
 * through the native EditHistory, so {@link undo} / {@link redo} work, and
 * serialization is deterministic (`toJson` is byte-stable for a given project
 * state within one build). Musical positions are PPQ (quarter notes).
 *
 * @example
 * ```ts
 * const project = new Project();
 * const track = project.addTrack({ kind: 'audio', name: 'lead' });
 * const clip = project.addClip({ trackId: track, startPpq: 0, lengthPpq: 4 });
 * const json = project.toJson();
 * project.destroy();
 * ```
 */
@entryClass('Project', ['native', 'assertAlive'])
export class Project {
  private readonly handle: NativeProject;

  /** The native project; refused while an instrument bounce is calling back into JavaScript. */
  private get native(): NativeProject {
    if (this.instrumentBounceActive) {
      throw invalidState(
        'Project is busy: it cannot be used from inside a bounce instrument callback',
      );
    }
    return this.handle;
  }

  private instrumentBounceActive = false;

  /**
   * Applies a vocal edit to a clip's audio.
   *
   * @param request - The edit and the original audio it applies to
   * @returns The rendered result and the stored edit state
   */
  applyVocalEdit(request: ProjectVocalEditApplyRequest): ProjectVocalEditApplyResult {
    this.assertAlive();
    return projectApplyVocalEdit(this.native, request);
  }

  /** Vocal-edit dependencies of every clip that carries a stored edit. */
  vocalEditDependencies(): ProjectVocalEditDependency[] {
    this.assertAlive();
    return projectGetVocalEditDependencies(this.native);
  }

  /**
   * Re-renders the stored vocal edits against the original audio.
   *
   * @param originals - The original audio of each edited source
   * @param options - `cancel` is polled between items; return `true` to stop
   * @returns One entry per stored edit
   */
  rehydrateVocalEdits(
    originals: readonly ProjectVocalOriginalSource[],
    options?: ProjectVocalRehydrateOptions,
  ): ProjectVocalRehydrateItem[] {
    this.assertAlive();
    return projectRehydrateVocalEdits(this.native, originals, options);
  }

  private assertAlive(): void {
    assertHandleLive(this.disposed, 'Project');
  }

  private disposed = false;

  /** Create a new empty project (throws on a project ABI mismatch). */
  constructor();
  constructor(adopted?: unknown) {
    if (adopted !== undefined && !(adopted instanceof AdoptedProject)) {
      throw refuseType('Project takes no arguments');
    }
    this.handle = adopted === undefined ? new addon.Project() : adopted.native;
  }

  /**
   * Deserialize project JSON into a new project. Throws cleanly (with the joined
   * native diagnostic messages) on malformed input.
   */
  static fromJson(json: string): Project {
    return adoptProject(addon.Project.fromJson(json));
  }

  /**
   * Deserialize project JSON and return native warning diagnostics emitted on
   * successful loads, such as dangling source references preserved for repair.
   */
  static fromJsonWithDiagnostics(json: string): { project: Project; diagnostics: string } {
    const result = addon.Project.fromJson(json, true);
    return {
      project: adoptProject(result.project),
      diagnostics: result.diagnostics,
    };
  }

  // -- serialization --

  /** Serialize the project to deterministic JSON. */
  toJson(): string {
    return this.native.toJson();
  }

  /**
   * Set the project sample rate in Hz. Must be a whole number of hertz within
   * `[8000, 384000]`; anything else throws. Applied through the edit history, so
   * it is undoable.
   */
  setSampleRate(sampleRate: number): void {
    this.native.setSampleRate(sampleRate);
  }

  /** The project sample rate in Hz. */
  get sampleRate(): number {
    return this.native.getSampleRate();
  }

  /** Set the project's clip-overlap policy. */
  setOverlapPolicy(policy: ProjectOverlapPolicy): void {
    this.native.setOverlapPolicy(tokenOrdinal(policy, OVERLAP_POLICY, 'policy'));
  }

  /** The project's clip-overlap policy. */
  get overlapPolicy(): ProjectOverlapPolicy {
    return tokenName(OVERLAP_POLICY, this.native.getOverlapPolicy(), 'overlap policy');
  }

  /**
   * Replace the project's mixer scene from scene JSON (see {@link Mixer.fromJson}).
   *
   * @throws SonareError `InvalidFormat` naming the first key the scene reader does not
   *   recognise (e.g. `unknown key strips[2].faderDB`; `$`- and `x-`-prefixed keys are
   *   exempt); the project's scene is left unchanged.
   */
  setMixerSceneJson(sceneJson: string): void {
    this.native.setMixerSceneJson(sceneJson);
  }

  /**
   * Add or replace a marker. An omitted `id` allocates a new one; an explicit
   * `0` is a `RangeError`.
   *
   * @returns The marker id
   */
  setMarker(input: ProjectMarkerInput): number {
    const fnName = 'setMarker';
    const request = requestObject(fnName, input, 'input', true);
    const marker: Record<string, unknown> = {
      ppq: requireNumber(request.ppq, 'ppq'),
    };
    const id = optionalId('id', request.id);
    if (id !== undefined) {
      marker.id = id;
    }
    if (!isAbsent(request.name)) {
      if (typeof request.name !== 'string') {
        throw refuseType(`name must be a string`);
      }
      if (request.name === '') {
        throw refuse('marker name must not be empty; omit it for an unnamed marker');
      }
      marker.name = request.name;
    }
    if (!isAbsent(request.kind)) {
      marker.kind = tokenOrdinal(request.kind, MARKER_KIND, 'kind');
    }
    if (!isAbsent(request.keyFifths)) {
      marker.keyFifths = request.keyFifths;
    }
    if (!isAbsent(request.keyMinor)) {
      marker.keyMinor = request.keyMinor;
    }
    return this.native.setMarker(marker);
  }

  /** Every marker, in stored order. */
  markers(): ProjectMarker[] {
    const native = this.native;
    return Array.from({ length: native.markerCount() }, (_, i) =>
      markerFromNative(native.markerByIndex(i) as unknown as Record<string, unknown>),
    );
  }

  /** Every track, in stored order. */
  tracks(): ProjectTrack[] {
    const native = this.native;
    return Array.from({ length: native.trackCount() }, (_, i) =>
      trackFromNative(native.trackByIndex(i) as unknown as Record<string, unknown>),
    );
  }

  /** Every clip, in stored order. */
  clips(): ProjectClip[] {
    const native = this.native;
    return Array.from({ length: native.clipCount() }, (_, i) =>
      clipFromNative(native.clipByIndex(i) as unknown as Record<string, unknown>),
    );
  }

  /** Every source, in stored order. */
  sources(): ProjectSource[] {
    const native = this.native;
    return Array.from({ length: native.sourceCount() }, (_, i) =>
      sourceFromNative(native.sourceByIndex(i) as unknown as Record<string, unknown>),
    );
  }

  /** Replace the project's tempo segment list. */
  setTempoSegments(segments: ReadonlyArray<ProjectTempoSegment>): void {
    this.native.setTempoSegments(segments);
  }

  /** Replace the project's time-signature segment list. */
  setTimeSignatures(segments: ReadonlyArray<ProjectTimeSignatureSegment>): void {
    this.native.setTimeSignatures(segments);
  }

  /** Every tempo segment, in stored order, in the shape {@link setTempoSegments} accepts. */
  tempoSegments(): ProjectTempoSegment[] {
    const native = this.native;
    return Array.from({ length: native.tempoSegmentCount() }, (_, i) =>
      native.tempoSegmentByIndex(i),
    );
  }

  /** Every time-signature segment, in stored order, in the shape {@link setTimeSignatures} accepts. */
  timeSignatures(): ProjectTimeSignatureSegment[] {
    const native = this.native;
    return Array.from({ length: native.timeSignatureCount() }, (_, i) =>
      native.timeSignatureByIndex(i),
    );
  }

  /** Audio source ids that need decoded PCM after deserialization. */
  unresolvedAudioSourceIds(): number[] {
    return this.native.unresolvedAudioSourceIds();
  }

  /** Unresolved audio source descriptors, in the order of {@link unresolvedAudioSourceIds}; `nameOrUri` is the full URI. */
  unresolvedAudioSources(): ProjectSource[] {
    return (this.native.unresolvedAudioSources() as unknown as Record<string, unknown>[]).map(
      sourceFromNative,
    );
  }

  /** Register decoded PCM for an existing audio source (undoable). */
  setSourceAudio(request: ProjectSourceAudioRequest): void {
    const fnName = 'setSourceAudio';
    const record = requestObject(fnName, request, 'request', true);
    assertSampleRate(record.sampleRate);
    const { data, numChannels } = interleavePlanar(record.channels);
    this.native.setSourceAudio(
      requireInteger(record.sourceId, 'sourceId'),
      data,
      numChannels,
      record.sampleRate,
    );
  }

  /** Replace an audio source's metadata strings as one undoable edit. */
  setAudioSourceMetadata(request: ProjectAudioSourceMetadataRequest): void {
    const fnName = 'setAudioSourceMetadata';
    const record = requestObject(fnName, request, 'request', true);
    const [contentHash, externalStemRole] = (['contentHash', 'externalStemRole'] as const).map(
      (field) => optionalName(field, record[field], 'to clear it'),
    );
    this.native.setAudioSourceMetadata(
      requireInteger(record.sourceId, 'sourceId'),
      contentHash ?? '',
      externalStemRole ?? '',
    );
  }

  // -- edit --

  /** Add a track and return its allocated stable id. */
  addTrack(desc: ProjectTrackDesc = {}): number {
    const { name, ...rest } = desc;
    const label = optionalName('name', name, 'for no name');
    return this.native.addTrack({
      ...rest,
      kind: tokenOrdinal(desc.kind ?? 'audio', TRACK_KIND, 'kind'),
      ...(label === undefined ? {} : { name: label }),
    });
  }

  /**
   * Add an audio or MIDI clip and return its allocated clip id. An audio clip
   * with `channels` is bound to a fresh renderable source; without it the source
   * is metadata-only.
   */
  addClip(desc: ProjectClipDesc): number {
    const fnName = 'addClip';
    const { channels, sampleRate, ...rest } = requestObject(fnName, desc, 'desc', true);
    if (requireInteger(rest.trackId, 'trackId') === 0) {
      throw refuse(`trackId must not be 0; a clip needs an existing track`);
    }
    if (channels === undefined) {
      return this.native.addClip(rest);
    }
    assertSampleRate(sampleRate);
    const { data, numChannels } = interleavePlanar(channels);
    return this.native.addClip({
      ...rest,
      audio: data,
      audioChannels: numChannels,
      audioSampleRate: sampleRate,
    });
  }

  /**
   * Import host-separated PCM as ordinary audio tracks and clips. Input PCM is
   * copied; the native layer never resamples, retimes, phase-aligns, or adds
   * hidden gain compensation.
   */
  importExternalStems(
    request: ExternalSeparatedStemImportRequest,
  ): ExternalSeparatedStemImportResult {
    const fnName = 'importExternalStems';
    const record = requestObject(fnName, request, 'request', true);
    if (!Array.isArray(record.stems)) {
      throw refuseType(`stems must be an array`);
    }
    return this.native.importExternalStems({
      ...record,
      stems: record.stems.map((entry) => {
        const stem = requestObject('ExternalSeparatedStem', entry, 'stem', true);
        const { channels, layout, ...rest } = stem;
        return {
          ...rest,
          layout: tokenOrdinal(layout, STEM_LAYOUT, 'layout'),
          planarSamples: channels,
        };
      }),
    });
  }

  /**
   * Split captured loop-recording audio into takes and add one clip. `audio` may
   * be interleaved or planar (`Float32Array[]`). The last take is made active
   * unless it is partial and an earlier complete take exists; `partialTail:
   * 'drop'` does not create the partial take. A partial take is marked
   * `"partial": true` in its `takes[]` entry of `toJson()`.
   */
  addLoopRecordingTakes(desc: ProjectLoopRecordingDesc): ProjectLoopRecordingResult {
    return this.native.addLoopRecordingTakes(interleaveLoopRecording(desc));
  }

  /**
   * Create a MIDI track + clip; returns `{ trackId, clipId }`.
   *
   * @remarks
   * The clip window is half-open, `[0, lengthPpq)`: an event at exactly
   * `lengthPpq` is outside it. Export and validation close a note still
   * sounding at the clip end with a note-off there, so a note running the full
   * length of the clip is a matched pair.
   */
  addMidiClip(startPpq: number, lengthPpq: number): ProjectMidiClipResult {
    return this.native.addMidiClip(startPpq, lengthPpq);
  }

  /** Split a clip at `splitPpq` (absolute PPQ); returns the new (right-hand) clip id. */
  splitClip(clipId: number, splitPpq: number): number {
    return this.native.splitClip(clipId, splitPpq);
  }

  /** Trim a clip's start / length (PPQ). */
  trimClip(clipId: number, newStartPpq: number, newLengthPpq: number): void {
    this.native.trimClip(clipId, newStartPpq, newLengthPpq);
  }

  /** Move a clip to `newStartPpq`; with `options.trackId` it also moves to that track. */
  moveClip(clipId: number, newStartPpq: number, options?: ProjectClipMoveOptions): void {
    const fnName = 'moveClip';
    const record = options === undefined ? {} : requestObject(fnName, options, 'options', true);
    const trackId = optionalId('trackId', record.trackId, 'to keep the track');
    this.native.moveClip(clipId, newStartPpq, trackId ?? 0);
  }

  /** Change a track kind via an undoable edit. */
  setTrackKind(trackId: number, kind: ProjectTrackKind): void {
    this.native.setTrackKind(trackId, tokenOrdinal(kind, TRACK_KIND, 'kind'));
  }

  /** Set a clip's warp reference id (0 clears it). */
  setClipWarpRef(clipId: number, warpRefId: number): void {
    this.native.setClipWarpRef(clipId, warpRefId);
  }

  /** Set a clip's warp playback mode. */
  setClipWarpMode(clipId: number, mode: ProjectWarpMode): void {
    this.native.setClipWarpMode(clipId, tokenOrdinal(mode, WARP_MODE, 'mode'));
  }

  /** Add or replace a first-class warp map referenced by clip warp ids. */
  setWarpMap(map: import('./types.js').ProjectWarpMapDesc): void {
    if (!isNonArrayObject(map)) {
      throw refuseType('map must be an object');
    }
    const { name, ...rest } = map;
    const label = optionalName('name', name, 'for no name');
    this.native.setWarpMap(label === undefined ? rest : { ...rest, name: label });
  }

  /** Remove a first-class warp map by id. */
  removeWarpMap(warpRefId: number): void {
    this.native.removeWarpMap(warpRefId);
  }

  /**
   * Route a track's MIDI clips to a host/instrument destination id. Destination
   * `0`, which every track starts on, is a real id like any other.
   *
   * Builtin, NativeSynth, and SF2 instruments retain source-track provenance
   * inside a shared destination voice pool. With only zero-latency instruments
   * bound, live lanes and channel-strip bounces remain aligned. Configure a
   * live lane per source track that needs strip processing. A shared opaque or
   * latency-bearing instrument cannot be separated back into strip inputs; its
   * channel-strip bounce throws `NOT_SUPPORTED`.
   */
  setTrackMidiDestination(trackId: number, destinationId?: number | null): void {
    this.native.setTrackMidiDestination(trackId, destinationId ?? 0);
  }

  /**
   * Set a destination's part rig through an undoable edit. `part` is 0-15 or
   * {@link PART_RIG_ALL_PARTS} for the destination default. `mode` is `'bank'`
   * (the instrument's own rig), `'none'`, or `'chain'` with 1-8 `inserts`.
   * Entries are stored whether or not a track routes to the destination and take
   * effect when a bounce binds an instrument that carries part rigs.
   */
  setPartRig(request: PartRigRequest): void {
    const fnName = 'setPartRig';
    requestObject(fnName, request, 'request', true);
    const rig = normalizePartRig(request);
    assertClosedToken(rig.mode, PART_RIG_MODES, 'mode');
    this.native.setPartRig(rig.destinationId, rig.part, rig.mode as PartRigMode, rig.insertsJson);
  }

  /** Read one part rig entry, or `null` when none is set. */
  partRig(key: PartRigKey): PartRigEntry | null {
    const record = requestObject('partRig', key, 'key', true);
    const raw = this.native.getPartRig(
      requireInteger(record.destinationId, 'destinationId'),
      requireInteger(record.part, 'part'),
    );
    if (raw === null) {
      return null;
    }
    if (raw.inserts === undefined) {
      return { mode: raw.mode };
    }
    const inserts = JSON.parse(raw.inserts) as NonNullable<PartRigEntry['inserts']>;
    return { mode: raw.mode, inserts };
  }

  /** Remove one part rig entry through an undoable edit. */
  clearPartRig(key: PartRigKey): void {
    const record = requestObject('clearPartRig', key, 'key', true);
    this.native.clearPartRig(
      requireInteger(record.destinationId, 'destinationId'),
      requireInteger(record.part, 'part'),
    );
  }

  /**
   * Set a track's linear playback gain (1.0 = unity; >= 0) via an undoable edit.
   *
   * The value reaches the track's audio and MIDI alike, but the stage it lands
   * on follows the track's channel strip. A strip bound by this track alone
   * (including one synthesized for an unbound track) carries the controls on its
   * own fader and panner. A strip several tracks share processes their sum and
   * carries none of them; each track applies its controls upstream instead — on
   * its own clip schedules for audio, on its track lane for MIDI.
   *
   * A MIDI track's gain/pan on a shared strip ride the track lane, which is fed
   * per source track only by an instrument that preserves source-track identity
   * (see {@link setTrackMidiDestination}). An opaque host-callback instrument, or
   * one reporting non-zero latency, renders one buffer per destination and has no
   * per-track stage on a shared strip, so its gain/pan do not reach the bounce
   * there; bind such an instrument to a track with an exclusive strip. Mute and
   * solo are unaffected: a silenced MIDI track schedules no events at all.
   */
  setTrackGain(trackId: number, gain: number): void {
    this.native.setTrackGain(trackId, gain);
  }

  /** Set a track's mute flag via an undoable edit (a muted track is silent). */
  setTrackMute(trackId: number, mute: boolean): void {
    this.native.setTrackMute(trackId, mute);
  }

  /** Set a track's solo flag via an undoable edit (when any track is soloed, only soloed tracks sound). */
  setTrackSolo(trackId: number, solo: boolean): void {
    this.native.setTrackSolo(trackId, solo);
  }

  /**
   * Set a track's stereo balance in [-1, +1] (0 = center) via an undoable edit.
   *
   * See {@link setTrackGain} for which stage a track's controls land on. The pan
   * law that shapes the balance belongs to that stage: the strip's configured law
   * on a channel strip and on the clips of an audio track sharing a strip, and
   * the track lane's law for a MIDI track sharing a strip (the law of whatever
   * strip the host bound to that lane, or a linear balance when none is bound).
   * Every law is normalized so a centered track stays at unity and only the away
   * channel is attenuated, so the difference is a taper, not a level offset.
   *
   * @throws {RangeError} When `pan` is outside `[-1, 1]` or
   *   non-finite; the project and its undo history are left unchanged.
   */
  setTrackPan(trackId: number, pan: number): void {
    this.native.setTrackPan(trackId, pan);
  }

  /** Remove a clip via an undoable edit (undo restores it + its MIDI content). */
  removeClip(clipId: number): void {
    this.native.removeClip(clipId);
  }

  /** Set a clip's linear playback gain (>= 0; 0 = muted) via an undoable edit. */
  setClipGain(clipId: number, gain: number): void {
    this.native.setClipGain(clipId, gain);
  }

  /**
   * Set a clip's fade-in / fade-out regions via an undoable edit. An omitted side,
   * or an omitted length, is a zero-length linear fade.
   */
  setClipFade(clipId: number, request: ProjectClipFadeRequest): void {
    const fnName = 'setClipFade';
    const record = requestObject(fnName, request, 'request', true);
    const side = (ppq: unknown, curve: unknown, lengthName: string, curveName: string) => {
      if (isAbsent(ppq) && isAbsent(curve)) {
        return undefined;
      }
      return {
        lengthPpq: isAbsent(ppq) ? 0 : requireNumber(ppq, lengthName),
        curve: isAbsent(curve) ? FADE_CURVE.linear : tokenOrdinal(curve, FADE_CURVE, curveName),
      };
    };
    this.native.setClipFade(
      clipId,
      side(record.fadeInPpq, record.fadeInCurve, 'fadeInPpq', 'fadeInCurve'),
      side(record.fadeOutPpq, record.fadeOutCurve, 'fadeOutPpq', 'fadeOutCurve'),
    );
  }

  /**
   * Replace a clip's take list and active take id via an undoable edit. An
   * omitted `options.activeTakeId` plays the clip's base source; an explicit `0`
   * is a `RangeError`.
   */
  setClipTakes(
    clipId: number,
    takes: ReadonlyArray<ProjectClipTake>,
    options?: ProjectClipTakesOptions,
  ): void {
    const fnName = 'setClipTakes';
    const record = options === undefined ? {} : requestObject(fnName, options, 'options', true);
    const nativeTakes = takesToNative(takes);
    const activeTakeId = optionalId('activeTakeId', record.activeTakeId);
    this.native.setClipTakes(clipId, nativeTakes as never, activeTakeId ?? 0);
  }

  /** Replace a clip's comp segments via an undoable edit. */
  setClipCompSegments(clipId: number, segments: ReadonlyArray<ProjectClipCompSegment>): void {
    this.native.setClipCompSegments(clipId, compSegmentsToNative(segments) as never);
  }

  /** Set a clip's loop mode and loop length via an undoable edit; see {@link ProjectClipLoopRequest}. */
  setClipLoop(clipId: number, request: ProjectClipLoopRequest): void {
    const fnName = 'setClipLoop';
    const record = requestObject(fnName, request, 'request', true);
    this.native.setClipLoop(
      clipId,
      tokenOrdinal(record.loopMode, LOOP_MODE, 'loopMode'),
      wholeClipLoopLength(record.loopLengthPpq),
      isAbsent(record.loopCrossfadePpq)
        ? 0
        : requireNumber(record.loopCrossfadePpq, 'loopCrossfadePpq'),
    );
  }

  /** Rebind a clip to a different already-registered source via an undoable edit. */
  setClipSource(clipId: number, sourceId: number): void {
    this.native.setClipSource(clipId, requireInteger(sourceId, 'sourceId'));
  }

  /**
   * Duplicate a clip at `newStartPpq` (same track), copying any MIDI content,
   * via an undoable edit; returns the new clip id.
   */
  duplicateClip(clipId: number, newStartPpq: number): number {
    return this.native.duplicateClip(clipId, newStartPpq);
  }

  /** Remove a track (and its clips) via an undoable edit. */
  removeTrack(trackId: number): void {
    this.native.removeTrack(trackId);
  }

  /** Rename a track via an undoable edit; an empty `name` clears it. */
  renameTrack(trackId: number, name: string): void {
    if (typeof name !== 'string') {
      throw refuseType('name must be a string');
    }
    this.native.renameTrack(trackId, name);
  }

  /**
   * Set a track's mixer-strip binding + output target via an undoable edit.
   * Pass `undefined`/empty to clear the respective field.
   */
  setTrackRoute(trackId: number, channelStripRef?: string, outputTarget?: string): void {
    this.native.setTrackRoute(trackId, channelStripRef ?? null, outputTarget ?? null);
  }

  /**
   * Append an automation lane to a track via an undoable edit; returns the
   * lane's stable target parameter id. An omitted `desc.targetKind` adds an
   * `'opaque'` lane; supply it to persist a typed fader or pan target.
   */
  addAutomationLane(trackId: number, desc: ProjectAutomationLaneDesc): number {
    return this.native.addAutomationLane(trackId, automationLaneToNative(desc));
  }

  /**
   * Replace the lane identified by its stable target parameter id. An omitted
   * `desc.targetKind` keeps the lane's current kind; a supplied one replaces it.
   */
  editAutomationLane(
    trackId: number,
    targetParamId: number,
    desc: ProjectAutomationLaneDesc,
  ): void {
    this.native.editAutomationLane(trackId, targetParamId, automationLaneToNative(desc));
  }

  /** Remove the lane identified by its stable target parameter id. */
  removeAutomationLane(trackId: number, targetParamId: number): void {
    this.native.removeAutomationLane(trackId, targetParamId);
  }

  /** Undo the most recent edit (throws when the undo stack is empty). */
  undo(): void {
    this.native.undo();
  }

  /** Redo the most recently undone edit (throws when the redo stack is empty). */
  redo(): void {
    this.native.redo();
  }

  /** Clear the undo + redo stacks without changing the current project state. */
  clearHistory(): void {
    this.native.clearHistory();
  }

  /**
   * Cap the undo history to `depth` most-recent edits (clamped to >= 1); older
   * entries beyond the cap are discarded immediately.
   */
  setMaxUndoDepth(depth: number): void {
    this.native.setMaxUndoDepth(depth);
  }

  /**
   * Set the combined retained-byte cap for undo and redo history. Zero is
   * valid and makes subsequent successful edits non-undoable.
   */
  setMaxHistoryBytes(bytes: number): void {
    this.native.setMaxHistoryBytes(bytes);
  }

  // -- MIDI --

  /**
   * Replace a MIDI clip's entire event list. Each event is
   * `{ ppq, word0, word1? }`; pass an empty array to clear. `word0`/`word1` are
   * the first two UMP words of a channel-voice message (stored opaquely).
   *
   * Drops the clip's SysEx, which {@link importSmf} and {@link exportSmf} both
   * keep. The payloads sit beside the event list and are reached by a handle
   * `ProjectMidiEvent` does not carry, so replacing the list leaves nothing
   * referring to them: a GS setup block that survives an import and an export
   * byte for byte is gone after one call here. Nothing reads the handles back
   * either, so a caller that must keep the setup edits the exported file
   * rather than the event list.
   *
   * Only events inside the clip's half-open window `[0, lengthPpq)` (after the
   * source offset) are kept for export; a note still sounding at the clip end,
   * including one whose note-off sits at or past `lengthPpq`, is closed with a
   * note-off at the clip end.
   */
  setMidiEvents(clipId: number, events: ReadonlyArray<ProjectMidiEvent>): void {
    assertProjectMidiEvents(events);
    this.native.setMidiEvents(clipId, events);
  }

  /**
   * Import an in-memory SMF buffer; returns the first added clip id.
   * Malformed or partially truncated tracks are rejected instead of installing
   * a silently shortened clip.
   */
  importSmf(data: Buffer | Uint8Array): number {
    return this.native.importSmf(data);
  }

  /** Export the project's tempo map + MIDI clips to SMF bytes. */
  exportSmf(): Uint8Array {
    return new Uint8Array(this.native.exportSmf());
  }

  /**
   * Import a MIDI 2.0 Clip File (`SMF2CLIP`); returns the first added clip id.
   * Unlike {@link importSmf}, MIDI 2.0 channel-voice messages (16-bit velocity,
   * 32-bit CC, per-note / registered controllers, bank-valid Program Change)
   * survive without loss.
   */
  importClipFile(data: Buffer | Uint8Array): number {
    return this.native.importClipFile(data);
  }

  /**
   * Export the project's tempo map + MIDI clips to MIDI 2.0 Clip File
   * (`SMF2CLIP`) bytes. MIDI 2.0-only events are written without loss —
   * prefer this over {@link exportSmf} when MIDI 2.0 fidelity matters.
   */
  exportClipFile(): Uint8Array {
    return new Uint8Array(this.native.exportClipFile());
  }

  /**
   * Set a MIDI clip's program at source PPQ 0. `options.bank` emits a Bank Select
   * before the program (omitted, none is emitted); `options.group` and
   * `options.channel` pick the UMP group and MIDI channel and default to 0.
   */
  setProgram(clipId: number, program: number, options?: ProjectProgramOptions): void {
    const fnName = 'setProgram';
    const record = options === undefined ? {} : requestObject(fnName, options, 'options', true);
    let bank = -1;
    if (!isAbsent(record.bank)) {
      const msb = assertU7(requireInteger(record.bank.msb, 'bank.msb'), 'bank.msb');
      const lsb = assertU7(requireInteger(record.bank.lsb, 'bank.lsb'), 'bank.lsb');
      bank = (msb << 7) | lsb;
    }
    this.native.setProgram(
      clipId,
      requireInteger(program, 'program'),
      bank,
      isAbsent(record.group) ? 0 : requireInteger(record.group, 'group'),
      isAbsent(record.channel) ? 0 : requireInteger(record.channel, 'channel'),
    );
  }

  /**
   * Destructively bake a MIDI-FX chain into all stored events. Large clips are
   * drained without truncation; failure leaves the original clip unchanged.
   * Setting `withSourceIndex` also returns per-event provenance, so a selection
   * or an editorial annotation can be carried across the bake.
   */
  bakeMidiFx(request: ProjectMidiFxBakeRequest): ProjectMidiFxBakeResult {
    const { clipId, configJson, withSourceIndex } = midiFxBakeRequest(request);
    if (!withSourceIndex) {
      this.native.bakeMidiFx(clipId, configJson);
      return {};
    }
    return { sourceIndex: this.native.bakeMidiFx(clipId, configJson, true) };
  }

  /**
   * Count the events {@link bakeMidiFx} would produce for this clip and
   * configuration, without mutating the project. The transform is
   * deterministic, so the count matches what the bake goes on to produce.
   */
  previewMidiFxCount(request: ProjectMidiFxPreviewRequest): number {
    const record = requestObject('previewMidiFx', request, 'request', true);
    return this.native.previewMidiFxCount(record.clipId, midiFxConfigJson(record.config));
  }

  /**
   * Validate that every note-on in the clip's exported half-open playback
   * window `[0, lengthPpq)` has a matching note-off. A note still sounding at
   * the clip end (its note-off at or past `lengthPpq`) is closed there by
   * export, so it validates as paired.
   *
   * @param clipId Target MIDI clip id.
   * @returns `ok` is `true` when fully paired; `unmatchedNoteOns` /
   *          `unmatchedNoteOffs` count the dangling events of each kind.
   * @throws If `clipId` is not a MIDI clip.
   */
  validateMidiNotes(clipId: number): ProjectNotePairValidation {
    return this.native.validateMidiNotes(clipId);
  }

  /**
   * Transcribe mono audio straight into a MIDI clip's event list, returning the
   * number of notes written.
   *
   * The PPQ grid is this project's own tempo map, which is why the request
   * carries no tempo: a project whose tempo was installed by
   * {@link autoTempo} transcribes onto that map rather than onto a second,
   * separately detected one. The clip's entire event list is REPLACED, exactly
   * as {@link setMidiEvents} replaces it.
   *
   * @throws If `clipId` is not a MIDI clip, if `samples` is empty, or if
   *         `sampleRate` is out of range.
   */
  transcribeToClip(request: ProjectTranscribeRequest): number {
    assertSamples(request.samples, true);
    assertSampleRate(request.sampleRate);
    return this.native.transcribeToClip(request);
  }

  // -- MIR --

  /**
   * Return ranked tempo-octave and detected-meter candidates without editing the
   * project. Takes `(samples, sampleRate, options?)` or one
   * {@link ProjectAnalyzeTempoRequest}.
   */
  analyzeTempo(request: ProjectAnalyzeTempoRequest): ProjectTempoCandidate[];
  analyzeTempo(
    samples: Float32Array,
    sampleRate: number,
    options?: ProjectTempoOptions,
  ): ProjectTempoCandidate[];
  analyzeTempo(...args: unknown[]): ProjectTempoCandidate[] {
    const fnName = 'analyzeTempo';
    const request = sampleOneShot<ProjectAnalyzeTempoRequest>(fnName, args);
    assertAudioInput(request.samples, request.sampleRate);
    return (
      this.native.analyzeTempo(
        request.samples,
        request.sampleRate,
        tempoOptionsToNative(request),
      ) as unknown as Record<string, unknown>[]
    ).map(tempoCandidateFromNative);
  }

  /**
   * Detect and install a ranked tempo candidate; optionally apply its detected meter.
   * Takes `(samples, sampleRate, options?)` or one {@link ProjectAutoTempoRequest}.
   *
   * @remarks
   * `candidateIndex` indexes the ranking {@link analyzeTempo} produced, so pair
   * the two on the same options. Read the installed map back with
   * {@link tempoSegments}.
   */
  autoTempo(request: ProjectAutoTempoRequest): number;
  autoTempo(samples: Float32Array, sampleRate: number, options?: ProjectAutoTempoOptions): number;
  autoTempo(...args: unknown[]): number {
    const fnName = 'autoTempo';
    const request = sampleOneShot<ProjectAutoTempoRequest>(fnName, args);
    assertAudioInput(request.samples, request.sampleRate);
    return this.native.autoTempo(
      request.samples,
      request.sampleRate,
      request.candidateIndex ?? 0,
      request.applyTimeSignatures ?? false,
      tempoOptionsToNative(request),
    );
  }

  /**
   * Snap a PPQ coordinate to the project grid. `division` is `0` for bars,
   * `1` for beats, or the number of subdivisions per beat. `strength` is in
   * `[0, 1]` (0 = no snap, 1 = exact grid line).
   */
  snapToGrid(ppq: number, strength = 1.0, division = 1): number {
    return this.native.snapToGrid(ppq, strength, division);
  }

  /**
   * Replace the project's key annotation stream via an undoable edit (existing
   * chord / section / onset annotations are preserved).
   */
  annotateKeys(keys: ProjectKeySegment[]): void {
    this.native.annotateKeys(
      keys.map((key) => ({
        ...key,
        ...(key.mode === undefined ? {} : { mode: tokenOrdinal(key.mode, KEY_MODE, 'mode') }),
      })),
    );
  }

  /** Replace the project's chord-symbol annotation stream via an undoable edit. */
  annotateChords(chords: ProjectChordSymbol[]): void {
    this.native.annotateChords(
      chords.map((chord) => ({
        ...chord,
        ...(chord.quality === undefined
          ? {}
          : { quality: tokenOrdinal(chord.quality, CHORD_QUALITY, 'quality') }),
      })),
    );
  }

  // -- assist sidecars --

  /**
   * Add or update an opaque assist sidecar (keyed by module id + target scope)
   * via an undoable edit. The payload bytes are copied.
   */
  setAssistSidecar(sidecar: ProjectAssistSidecarInput): void {
    this.native.setAssistSidecar(requestObject('setAssistSidecar', sidecar, 'sidecar', true));
  }

  /** Every stored assist sidecar, in stable project order. */
  assistSidecars(): ProjectAssistSidecar[] {
    return this.native.assistSidecars();
  }

  // -- compile / render --

  /**
   * Compile the project into an RT-readable timeline, surfacing diagnostics.
   *
   * With `withTimeline: true` the result also carries `timeline`, absent when compilation
   * produced none. Install it with {@link RealtimeEngine.applyProjectTimeline}; later project
   * edits do not change an already compiled timeline.
   */
  compile(options?: ProjectCompileOptions): ProjectCompileResult {
    if (options === undefined) {
      return { diagnostics: diagnosticsFromNative(this.native.compile().diagnostics) };
    }
    const request = requestObject('compile', options, 'options', true);
    const withTimeline = isAbsent(request.withTimeline)
      ? false
      : requireBoolean(request.withTimeline, 'withTimeline');
    if (!withTimeline) {
      return { diagnostics: diagnosticsFromNative(this.native.compile().diagnostics) };
    }
    const raw = this.native.compile(true);
    const diagnostics = diagnosticsFromNative(raw.diagnostics);
    return raw.timeline === null
      ? { diagnostics }
      : { diagnostics, timeline: createTimeline(raw.timeline) };
  }

  /**
   * Retrieve the compile result captured by the most recent {@link bounce}.
   *
   * `undefined` when no bounce has run on this project. A clean bounce returns a
   * result with empty diagnostics; a failed one carries at least one error.
   */
  lastBounceCompileResult(): ProjectCompileResult | undefined {
    const raw = this.native.lastBounceCompileResult();
    if (!raw.hasTimeline && raw.diagnostics.length === 0) {
      return undefined;
    }
    return { diagnostics: diagnosticsFromNative(raw.diagnostics) };
  }

  /**
   * Compile + render the project offline to planar float audio at the project
   * sample rate. Deterministic: the same project + options yields bit-identical
   * output within one build.
   *
   * Omitting `options.totalFrames` derives the render length from the arrangement
   * rather than producing an empty render.
   *
   * MIDI tracks routed to a destination no instrument list binds render as
   * silence. Bind instruments with `builtinInstruments` (built-in oscillator
   * synth), `synthInstruments` (patch-driven NativeSynth), `sf2Instruments`
   * (GS-compatible SoundFont players fed by {@link loadSoundFont}) or
   * `instruments` (JavaScript instruments); an empty list
   * renders silence. A destination bound twice, within one list or across
   * lists, is refused.
   *
   * Every `instruments` callback runs synchronously on the calling thread while
   * this method is blocked, so a callback that returns a Promise is refused with
   * a `TypeError`. A callback that throws stops all further callbacks, and the
   * bounce rethrows that first error once it returns (a native failure is
   * reported ahead of it). While the bounce runs, using this project from inside
   * a callback (a mutation, a nested bounce, or `destroy`) throws `InvalidState`.
   * One instrument shared by tracks that feed different channel strips throws a
   * `SonareError` with `NOT_SUPPORTED`; bind one destination per strip instead.
   */
  bounce(options?: ProjectBounceOptions): ProjectBounceResult {
    const request = requestObject('bounce', options ?? {}, 'options', true);
    const settings = bounceSettings(request);
    const builtin =
      request.builtinInstruments === undefined
        ? undefined
        : bindInstruments(
            'builtinInstruments',
            request.builtinInstruments,
            (i) => {
              refuseBuiltinSynthOrdinals(i);
              return i;
            },
            withDestinationCopy,
          );
    const synth =
      request.synthInstruments === undefined
        ? undefined
        : bindInstruments(
            'synthInstruments',
            request.synthInstruments,
            (i) => normalizeSynthInstrument(i) as SynthPatch,
            withDestinationCopy,
          );
    const sf2 =
      request.sf2Instruments === undefined
        ? undefined
        : bindInstruments('sf2Instruments', request.sf2Instruments, (i) => i, withDestinationCopy);
    const callbacks =
      request.instruments === undefined
        ? undefined
        : bindInstruments('instruments', request.instruments, (i) => i, withDestinationLayer);
    const native = this.native;
    if (callbacks === undefined) {
      return planarBounce(native.bounce(settings, builtin, synth, sf2), native.getSampleRate());
    }
    this.instrumentBounceActive = true;
    try {
      return planarBounce(
        native.bounce(settings, builtin, synth, sf2, callbacks),
        native.getSampleRate(),
      );
    } finally {
      this.instrumentBounceActive = false;
    }
  }

  /**
   * Load (parse) SoundFont 2 bytes into the project: presets / instruments /
   * sample headers plus the sample PCM decoded to a float pool. Replaces any
   * previously loaded SoundFont; the input buffer is not referenced after the
   * call. Throws on malformed input (the previous SoundFont is kept).
   */
  loadSoundFont(data: Uint8Array): void {
    this.native.loadSoundFont(data);
  }

  /** Release the project's loaded SoundFont (no-op when none is loaded). */
  clearSoundFont(): void {
    this.native.clearSoundFont();
  }

  /** Number of presets in the loaded SoundFont (0 when none is loaded). */
  get soundFontPresetCount(): number {
    return this.native.soundFontPresetCount();
  }

  /**
   * Enumerate every (channel, bank, program) combination the arrangement plays
   * a note through, in first-use order, reporting whether each resolves in the
   * loaded SoundFont (`'sf2'`, GS variation/drum fallbacks included) or would
   * fall back to the built-in synth (`'synth'`). Without a loaded SoundFont
   * every entry is a synth fallback.
   */
  soundFontManifest(): Sf2ProgramStatus[] {
    return this.native.soundFontManifest();
  }

  /** Release the underlying native project. Idempotent. */
  destroy(): void {
    if (this.disposed) {
      return;
    }
    this.disposed = true;
    this.native.destroy();
  }

  /**
   * Releases the native project; lets `using` free it automatically (needs TypeScript 5.2+
   * or a runtime with native explicit resource management; Node 22 does not parse `using`).
   */
  [Symbol.dispose](): void {
    this.destroy();
  }
}

function adoptProject(native: NativeProject): Project {
  return new (Project as unknown as new (adopted: AdoptedProject) => Project)(
    new AdoptedProject(native),
  );
}
