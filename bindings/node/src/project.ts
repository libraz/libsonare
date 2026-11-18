import { ErrorCode, SonareError } from './errors.js';
import { midiFxConfigJson } from './internal/midi_fx_config.js';
import { mixSceneJsonToNative } from './internal/mix_scene.js';
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
import { nativeTranscribeRequest } from './internal/transcribe_request.js';
import { addon } from './native.js';
import type {
  BuiltinSynthConfig,
  ExternalInstrument,
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
  ProjectCompileResult,
  ProjectCompileTimelineResult,
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
  ProjectProgramRequest,
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
  Sf2InstrumentConfig,
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
  assertInt32,
  assertInt64,
  assertProjectMidiEvents,
  assertSampleRate,
  assertSamples,
  assertU7,
  assertU32,
  requestObject,
  resolveOptionalNonNegative,
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

const isAbsent = (value: unknown): value is undefined | null =>
  value === undefined || value === null;

function requireNumber(fnName: string, value: unknown, argName: string): number {
  if (typeof value !== 'number') {
    throw new TypeError(`${fnName}: ${argName} must be a number`);
  }
  return value;
}

/** An optional field where `0` would mean "the default": absent stays absent, `0` is refused. */
function optionalNonZero(
  fnName: string,
  value: unknown,
  argName: string,
  check: (fn: string, v: number, arg: string) => void,
): number | undefined {
  if (isAbsent(value)) {
    return undefined;
  }
  const number = requireNumber(fnName, value, argName);
  check(fnName, number, argName);
  if (number === 0) {
    throw new RangeError(`${fnName}: ${argName} must not be 0 (omit it for the default)`);
  }
  if (number < 0) {
    throw new RangeError(`${fnName}: ${argName} must be positive (omit it for the default)`);
  }
  return number;
}

/** Render settings the native bounce reads. */
function bounceSettings(
  fnName: BounceFn,
  options: ProjectBounceOptions | undefined,
): Record<string, unknown> {
  const request = requestObject(fnName, options ?? {}, 'options', true);
  const native: Record<string, unknown> = {};
  const totalFrames = optionalNonZero(fnName, request.totalFrames, 'totalFrames', assertInt64);
  const blockSize = optionalNonZero(fnName, request.blockSize, 'blockSize', assertInt32);
  const numChannels = optionalNonZero(fnName, request.numChannels, 'numChannels', assertInt32);
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

type BounceFn =
  | 'bounce'
  | 'bounceWithBuiltinInstruments'
  | 'bounceWithSynthInstruments'
  | 'bounceWithSf2Instruments'
  | 'bounceWithInstruments';

/**
 * Resolve the bindings of a `bounceWith*Instruments` call to the list the native
 * layer reads: each pair's `destinationId` is folded into a copy of its instrument.
 * Anything but an array of pairs is a `TypeError`.
 */
function bindInstruments<T extends object>(
  fnName: BounceFn,
  value: readonly ProjectInstrumentBinding<T>[],
  entry: (instrument: T) => T,
  withDestination: (instrument: T, destinationId: number) => T,
): T[] {
  if (!Array.isArray(value)) {
    throw new TypeError(
      `${fnName}: instruments must be an array of { destinationId?, instrument } pairs`,
    );
  }
  return value.map((pair) => {
    const binding = requestObject('ProjectInstrumentBinding', pair, 'binding', true);
    const instrument = binding.instrument as unknown;
    if (typeof instrument !== 'object' || instrument === null || Array.isArray(instrument)) {
      throw new TypeError(`${fnName}: binding.instrument must be an object`);
    }
    if ('destinationId' in instrument) {
      throw new TypeError(
        `${fnName}: unknown request key 'destinationId' in 'instrument' (give it on the binding)`,
      );
    }
    const resolved = entry(instrument as T);
    if (isAbsent(binding.destinationId)) {
      return resolved;
    }
    assertU32(
      fnName,
      requireNumber(fnName, binding.destinationId, 'destinationId'),
      'destinationId',
    );
    return withDestination(resolved, binding.destinationId);
  });
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

function clipFromNative(raw: Record<string, unknown>): ProjectClip {
  return {
    ...raw,
    sourceKind: tokenName(SOURCE_KIND, raw.sourceKind as number, 'source kind'),
    loopMode: tokenName(LOOP_MODE, raw.loopMode as number, 'loop mode'),
  } as ProjectClip;
}

function sourceFromNative(raw: Record<string, unknown>): ProjectSource {
  return {
    ...raw,
    kind: tokenName(SOURCE_KIND, raw.kind as number, 'source kind'),
  } as ProjectSource;
}

function markerFromNative(raw: Record<string, unknown>): ProjectMarker {
  return {
    id: raw.id,
    ppq: raw.ppq,
    name: raw.name,
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

function automationLaneToNative(
  fnName: string,
  desc: ProjectAutomationLaneDesc,
): NativeAutomationLaneDesc {
  const { targetKind, ...rest } = desc;
  const resolved =
    targetKind === undefined
      ? rest
      : {
          ...rest,
          targetKind: tokenOrdinal(fnName, targetKind, AUTOMATION_TARGET_KIND, 'targetKind'),
        };
  return projectAutomationLaneValue(
    resolved as Omit<ProjectAutomationLaneDesc, 'targetKind'> & { targetKind?: 0 | 1 | 2 },
    fnName,
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
    configJson: midiFxConfigJson(record.config, 'bakeMidiFx'),
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
    throw new TypeError(
      'addLoopRecordingTakes: audioChannels must be omitted for planar audio; the channel count is the number of arrays',
    );
  }
  const { data, numChannels } = interleavePlanar('addLoopRecordingTakes', audio, 'audio');
  return { ...desc, audio: data, audioChannels: numChannels };
}

let createTimeline: (native: InstanceType<typeof addon.ProjectTimeline>) => ProjectTimeline;

/**
 * Immutable compiled playback snapshot of a {@link Project}, produced by
 * {@link Project.compileTimeline} and installed with
 * {@link RealtimeEngine.applyProjectTimeline}. The native handle is not
 * garbage-collected, so release it with {@link destroy} (or `using`). It cannot
 * be constructed directly.
 */
export class ProjectTimeline {
  private disposed = false;

  private constructor(token: symbol, native: InstanceType<typeof addon.ProjectTimeline>) {
    if (token !== TIMELINE_TOKEN) {
      throw new TypeError('ProjectTimeline cannot be constructed; use Project.compileTimeline()');
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
 * ```typescript
 * const project = new Project();
 * const track = project.addTrack({ kind: 'audio', name: 'lead' });
 * const clip = project.addClip({ trackId: track, startPpq: 0, lengthPpq: 4 });
 * const json = project.toJson();
 * project.destroy();
 * ```
 */
export class Project {
  private readonly handle: NativeProject;

  /** The native project; refused while an instrument bounce is calling back into JavaScript. */
  private get native(): NativeProject {
    if (this.instrumentBounceActive) {
      throw new SonareError(
        ErrorCode.InvalidState,
        'Project is busy: it cannot be used from inside a bounceWithInstruments callback',
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
    if (this.disposed) {
      throw new SonareError(ErrorCode.InvalidState, 'Project has been disposed');
    }
  }

  private disposed = false;

  /** Create a new empty project (throws on a project ABI mismatch). */
  constructor();
  constructor(adopted?: unknown) {
    if (adopted !== undefined && !(adopted instanceof AdoptedProject)) {
      throw new TypeError('Project takes no arguments');
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
    const result = addon.Project.fromJsonWithDiagnostics(json);
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
    this.native.setOverlapPolicy(
      tokenOrdinal('setOverlapPolicy', policy, OVERLAP_POLICY, 'policy'),
    );
  }

  /** The project's clip-overlap policy. */
  get overlapPolicy(): ProjectOverlapPolicy {
    return tokenName(OVERLAP_POLICY, this.native.getOverlapPolicy(), 'overlap policy');
  }

  /**
   * Replace the project's mixer scene from scene JSON (see {@link Mixer.fromJson}).
   *
   * @throws SonareError `InvalidFormat` naming the first key the scene reader does not
   *   recognise (e.g. `unknown scene key 'strips[2].faderDB'`; `$`- and `x-`-prefixed keys are
   *   exempt); the project's scene is left unchanged.
   */
  setMixerSceneJson(sceneJson: string): void {
    this.native.setMixerSceneJson(mixSceneJsonToNative('Project.setMixerSceneJson', sceneJson));
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
      ppq: requireNumber(fnName, request.ppq, 'ppq'),
    };
    if (!isAbsent(request.id)) {
      assertU32(fnName, requireNumber(fnName, request.id, 'id'), 'id');
      if (request.id === 0) {
        throw new RangeError(`${fnName}: id must not be 0 (omit it for the default)`);
      }
      marker.id = request.id;
    }
    if (!isAbsent(request.name)) {
      marker.name = request.name;
    }
    if (!isAbsent(request.kind)) {
      marker.kind = tokenOrdinal(fnName, request.kind, MARKER_KIND, 'kind');
    }
    if (!isAbsent(request.keyFifths)) {
      marker.keyFifths = request.keyFifths;
    }
    if (!isAbsent(request.keyMinor)) {
      marker.keyMinor = request.keyMinor;
    }
    return this.native.setMarkerEx(marker);
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
    assertSampleRate(fnName, record.sampleRate);
    const { data, numChannels } = interleavePlanar(fnName, record.channels);
    this.native.setSourceAudio(
      requireNumber(fnName, record.sourceId, 'sourceId'),
      data,
      numChannels,
      record.sampleRate,
    );
  }

  /** Replace an audio source's metadata strings as one undoable edit. */
  setAudioSourceMetadata(request: ProjectAudioSourceMetadataRequest): void {
    const fnName = 'setAudioSourceMetadata';
    const record = requestObject(fnName, request, 'request', true);
    for (const field of ['contentHash', 'externalStemRole'] as const) {
      if (!isAbsent(record[field]) && typeof record[field] !== 'string') {
        throw new TypeError(`${fnName}: ${field} must be a string`);
      }
    }
    this.native.setAudioSourceMetadata(
      requireNumber(fnName, record.sourceId, 'sourceId'),
      record.contentHash ?? '',
      record.externalStemRole ?? '',
    );
  }

  // -- edit --

  /** Add a track and return its allocated stable id. */
  addTrack(desc: ProjectTrackDesc = {}): number {
    return this.native.addTrack({
      ...desc,
      kind: tokenOrdinal('addTrack', desc.kind ?? 'audio', TRACK_KIND, 'kind'),
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
    if (channels === undefined) {
      return this.native.addClip(rest);
    }
    assertSampleRate(fnName, sampleRate);
    const { data, numChannels } = interleavePlanar(fnName, channels);
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
      throw new TypeError(`${fnName}: stems must be an array`);
    }
    return this.native.importExternalStems({
      ...record,
      stems: record.stems.map((entry) => {
        const stem = requestObject('ExternalSeparatedStem', entry, 'stem', true);
        const { channels, layout, ...rest } = stem;
        return {
          ...rest,
          layout: tokenOrdinal(fnName, layout, STEM_LAYOUT, 'layout'),
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
    let trackId = 0;
    if (!isAbsent(record.trackId)) {
      assertU32(fnName, requireNumber(fnName, record.trackId, 'trackId'), 'trackId');
      if (record.trackId === 0) {
        throw new RangeError(`${fnName}: trackId must not be 0 (omit it to keep the track)`);
      }
      trackId = record.trackId;
    }
    this.native.moveClip(clipId, newStartPpq, trackId);
  }

  /** Change a track kind via an undoable edit. */
  setTrackKind(trackId: number, kind: ProjectTrackKind): void {
    this.native.setTrackKind(trackId, tokenOrdinal('setTrackKind', kind, TRACK_KIND, 'kind'));
  }

  /** Set a clip's warp reference id (0 clears it). */
  setClipWarpRef(clipId: number, warpRefId: number): void {
    this.native.setClipWarpRef(clipId, warpRefId);
  }

  /** Set a clip's warp playback mode. */
  setClipWarpMode(clipId: number, mode: ProjectWarpMode): void {
    this.native.setClipWarpMode(clipId, tokenOrdinal('setClipWarpMode', mode, WARP_MODE, 'mode'));
  }

  /** Add or replace a first-class warp map referenced by clip warp ids. */
  setWarpMap(map: import('./types.js').ProjectWarpMapDesc): void {
    this.native.setWarpMap(map);
  }

  /** Remove a first-class warp map by id. */
  removeWarpMap(warpRefId: number): void {
    this.native.removeWarpMap(warpRefId);
  }

  /**
   * Route a track's MIDI clips to a host/instrument destination id.
   *
   * Builtin, NativeSynth, and SF2 instruments retain source-track provenance
   * inside a shared destination voice pool. With only zero-latency instruments
   * bound, live lanes and channel-strip bounces remain aligned. Configure a
   * live lane per source track that needs strip processing. A shared opaque or
   * latency-bearing instrument cannot be separated back into strip inputs; its
   * channel-strip bounce throws `NOT_SUPPORTED`.
   */
  setTrackMidiDestination(trackId: number, destinationId: number): void {
    this.native.setTrackMidiDestination(trackId, destinationId);
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
    const rig = normalizePartRig(fnName, request);
    assertClosedToken(fnName, rig.mode, PART_RIG_MODES, 'mode');
    this.native.setPartRig(rig.destinationId, rig.part, rig.mode as PartRigMode, rig.insertsJson);
  }

  /** Read one part rig entry, or `null` when none is set. */
  partRig(key: PartRigKey): PartRigEntry | null {
    const record = requestObject('partRig', key, 'key', true);
    const raw = this.native.getPartRig(
      requireNumber('partRig', record.destinationId, 'destinationId'),
      requireNumber('partRig', record.part, 'part'),
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
      requireNumber('clearPartRig', record.destinationId, 'destinationId'),
      requireNumber('clearPartRig', record.part, 'part'),
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
        lengthPpq: isAbsent(ppq) ? 0 : requireNumber(fnName, ppq, lengthName),
        curve: isAbsent(curve)
          ? FADE_CURVE.linear
          : tokenOrdinal(fnName, curve, FADE_CURVE, curveName),
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
    takes.forEach((take, index) => {
      if (take.sourceId !== undefined) {
        // 0 reuses the clip's current source, and the addon truncates onto it.
        assertU32(fnName, take.sourceId, `takes[${index}].sourceId`);
      }
    });
    let activeTakeId = 0;
    if (!isAbsent(record.activeTakeId)) {
      assertU32(fnName, requireNumber(fnName, record.activeTakeId, 'activeTakeId'), 'activeTakeId');
      if (record.activeTakeId === 0) {
        throw new RangeError(`${fnName}: activeTakeId must not be 0 (omit it for the default)`);
      }
      activeTakeId = record.activeTakeId;
    }
    this.native.setClipTakes(clipId, takes, activeTakeId);
  }

  /** Replace a clip's comp segments via an undoable edit. */
  setClipCompSegments(clipId: number, segments: ReadonlyArray<ProjectClipCompSegment>): void {
    segments.forEach((segment, index) => {
      if (segment.takeId !== undefined) {
        // 0 falls back to the active/default take, and the addon truncates onto it.
        assertU32('setClipCompSegments', segment.takeId, `segments[${index}].takeId`);
      }
    });
    this.native.setClipCompSegments(clipId, segments);
  }

  /** Set a clip's loop mode and loop length via an undoable edit; see {@link ProjectClipLoopRequest}. */
  setClipLoop(clipId: number, request: ProjectClipLoopRequest): void {
    const fnName = 'setClipLoop';
    const record = requestObject(fnName, request, 'request', true);
    this.native.setClipLoop(
      clipId,
      tokenOrdinal(fnName, record.loopMode, LOOP_MODE, 'loopMode'),
      isAbsent(record.loopLengthPpq)
        ? 0
        : requireNumber(fnName, record.loopLengthPpq, 'loopLengthPpq'),
      isAbsent(record.loopCrossfadePpq)
        ? 0
        : requireNumber(fnName, record.loopCrossfadePpq, 'loopCrossfadePpq'),
    );
  }

  /** Rebind a clip to a different already-registered source via an undoable edit. */
  setClipSource(clipId: number, sourceId: number): void {
    this.native.setClipSource(clipId, sourceId);
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
      throw new TypeError('renameTrack: name must be a string');
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
    return this.native.addAutomationLane(
      trackId,
      automationLaneToNative('addAutomationLane', desc),
    );
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
    this.native.editAutomationLane(
      trackId,
      targetParamId,
      automationLaneToNative('editAutomationLane', desc),
    );
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
    assertProjectMidiEvents('Project.setMidiEvents', events);
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
   * Set a MIDI clip's channel-0 program / bank at source PPQ 0. Omit `bank` for
   * no Bank Select, matching {@link setProgramOnChannel} and the Python/WASM surfaces;
   * pass `>= 0` to emit a Bank Select. A negative `bank` is refused.
   */
  setProgram(clipId: number, program: number, bank?: number): void {
    this.native.setProgram(clipId, program, resolveOptionalNonNegative('setProgram', bank, 'bank'));
  }

  /** Set a MIDI clip's program, and optionally its Bank Select, for one UMP group and channel. */
  setProgramOnChannel(clipId: number, request: ProjectProgramRequest): void {
    const fnName = 'setProgramOnChannel';
    const record = requestObject(fnName, request, 'request', true);
    let bank = -1;
    if (!isAbsent(record.bank)) {
      const msb = assertU7(fnName, requireNumber(fnName, record.bank.msb, 'bank.msb'), 'bank.msb');
      const lsb = assertU7(fnName, requireNumber(fnName, record.bank.lsb, 'bank.lsb'), 'bank.lsb');
      bank = (msb << 7) | lsb;
    }
    this.native.setProgramOnChannel(
      clipId,
      requireNumber(fnName, record.group, 'group'),
      requireNumber(fnName, record.channel, 'channel'),
      requireNumber(fnName, record.program, 'program'),
      bank,
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
    return { sourceIndex: this.native.bakeMidiFxWithSourceIndex(clipId, configJson) };
  }

  /**
   * Count the events {@link bakeMidiFx} would produce for this clip and
   * configuration, without mutating the project. The transform is
   * deterministic, so the count matches what the bake goes on to produce.
   */
  previewMidiFxCount(request: ProjectMidiFxPreviewRequest): number {
    const record = requestObject('previewMidiFx', request, 'request', true);
    return this.native.previewMidiFxCount(
      record.clipId,
      midiFxConfigJson(record.config, 'previewMidiFx'),
    );
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
    assertSamples('Project.transcribeToClip', request.samples, true);
    assertSampleRate('Project.transcribeToClip', request.sampleRate);
    return this.native.transcribeToClip(
      nativeTranscribeRequest('Project.transcribeToClip', request),
    );
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
    assertAudioInput(fnName, request.samples, request.sampleRate);
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
    assertAudioInput(fnName, request.samples, request.sampleRate);
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
        ...(key.mode === undefined
          ? {}
          : { mode: tokenOrdinal('annotateKeys', key.mode, KEY_MODE, 'mode') }),
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
          : { quality: tokenOrdinal('annotateChords', chord.quality, CHORD_QUALITY, 'quality') }),
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

  /** Compile the project into an RT-readable timeline, surfacing diagnostics. */
  compile(): ProjectCompileResult {
    return { diagnostics: diagnosticsFromNative(this.native.compile().diagnostics) };
  }

  /**
   * Compile the project and keep the resulting timeline. Returns the same
   * diagnostics as {@link compile} plus `timeline`, which is absent when
   * compilation produced none. Install it with {@link RealtimeEngine.applyProjectTimeline};
   * later project edits do not change an already compiled timeline.
   */
  compileTimeline(): ProjectCompileTimelineResult {
    const raw = this.native.compileTimeline();
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
   * MIDI tracks routed to a destination render as silence here, because no
   * instrument is bound. To audition MIDI through the built-in synth, use
   * {@link bounceWithBuiltinInstruments}.
   */
  bounce(options?: ProjectBounceOptions): ProjectBounceResult {
    const native = bounceSettings('bounce', options);
    return planarBounce(this.native.bounce(native), this.sampleRate);
  }

  /**
   * Like {@link bounce}, but drives MIDI tracks routed to a destination through
   * instruments you implement in JavaScript. Each pair binds an
   * {@link ExternalInstrument} to its `destinationId` (default `0`).
   *
   * Every callback runs synchronously on the calling thread while this method
   * is blocked, so there is no async instrument: a callback that returns a
   * Promise is refused with a `TypeError`. A callback that throws stops all
   * further callbacks, and the bounce rethrows that first error once it
   * returns (a native failure is reported ahead of it). The `outputs` passed to
   * `render` are zero-filled scratch arrays, added into the engine's buffers
   * when `render` returns normally.
   *
   * While the bounce runs, using this project from inside a callback (a
   * mutation, a nested bounce, or `destroy`) throws `InvalidState`.
   *
   * One instrument shared by tracks that feed different channel strips throws
   * a `SonareError` with `NOT_SUPPORTED`, because callback audio carries no
   * source-track attribution; bind one destination per strip instead.
   */
  bounceWithInstruments(
    instruments: readonly ProjectInstrumentBinding<ExternalInstrument>[] = [],
    options?: ProjectBounceOptions,
  ): ProjectBounceResult {
    const fnName = 'bounceWithInstruments';
    const settings = bounceSettings(fnName, options);
    const bindings = bindInstruments(fnName, instruments, (i) => i, withDestinationLayer);
    const native = this.native;
    this.instrumentBounceActive = true;
    try {
      return planarBounce(native.bounceWithInstruments(bindings, settings), native.getSampleRate());
    } finally {
      this.instrumentBounceActive = false;
    }
  }

  /**
   * Like {@link bounce}, but renders MIDI tracks routed to a destination
   * through the built-in oscillator synth so a MIDI-only arrangement bounces
   * to audible audio. Each pair binds a {@link BuiltinSynthConfig} patch to a
   * `destinationId` (default `0`). An empty
   * array renders silence, identical to {@link bounce}.
   */
  bounceWithBuiltinInstruments(
    instruments: readonly ProjectInstrumentBinding<BuiltinSynthConfig>[] = [],
    options?: ProjectBounceOptions,
  ): ProjectBounceResult {
    const fnName = 'bounceWithBuiltinInstruments';
    const settings = bounceSettings(fnName, options);
    const bindings = bindInstruments(
      fnName,
      instruments,
      (i) => {
        refuseBuiltinSynthOrdinals(fnName, i);
        return i;
      },
      withDestinationCopy,
    );
    const native = this.native;
    return planarBounce(
      native.bounceWithBuiltinInstruments(bindings, settings),
      native.getSampleRate(),
    );
  }

  /**
   * Like {@link bounce}, but renders MIDI tracks routed to a destination through
   * the patch-driven NativeSynth — the full synthesizer (every
   * {@link SynthEngineMode} engine plus the realism layer; the modes are
   * enumerated by {@link SYNTH_ENGINE_MODES}). Each pair binds a {@link SynthPatch}
   * to a `destinationId` (default `0`). Name a
   * catalog preset with `preset` (see {@link synthPresetNames}). A patch may also
   * set `useGmPrograms: true` to follow incoming GM bank/program changes (and use
   * the GM drum map on channel 10); it defaults to `false`, preserving the
   * fixed-patch behavior. An `engineMode: 'sample'` patch also takes a
   * `sampleBank` ({@link SampleBank}) holding the PCM its keymap names.
   * `useGmPrograms` and `sampleBank` are JS binding conveniences, not part of the
   * NativeSynth patch itself. An empty array renders silence. Unknown preset names
   * throw. Deterministic for a fixed project + options + patch.
   */
  bounceWithSynthInstruments(
    instruments: readonly ProjectInstrumentBinding<SynthPatch>[] = [],
    options?: ProjectBounceOptions,
  ): ProjectBounceResult {
    const fnName = 'bounceWithSynthInstruments';
    const settings = bounceSettings(fnName, options);
    const bindings = bindInstruments(
      fnName,
      instruments,
      (i) => normalizeSynthInstrument(fnName, i) as SynthPatch,
      withDestinationCopy,
    );
    const native = this.native;
    return planarBounce(
      native.bounceWithSynthInstruments(bindings, settings),
      native.getSampleRate(),
    );
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

  /**
   * Like {@link bounceWithBuiltinInstruments}, but each bound destination
   * renders through a GS-compatible SoundFont player fed by the project's
   * loaded SoundFont ({@link loadSoundFont}): 16 MIDI channels per player,
   * channel 10 drums via bank 128, GS NRPN part edits and GS/GM SysEx resets
   * honored. Programs the SoundFont does not cover — including bouncing with
   * no SoundFont loaded at all — play through the built-in synthesizer GM
   * fallback bank (the data-free floor; see {@link soundFontManifest} for the
   * per-program backend). An empty array renders silence.
   */
  bounceWithSf2Instruments(
    instruments: readonly ProjectInstrumentBinding<Sf2InstrumentConfig>[] = [],
    options?: ProjectBounceOptions,
  ): ProjectBounceResult {
    const fnName = 'bounceWithSf2Instruments';
    const settings = bounceSettings(fnName, options);
    const bindings = bindInstruments(fnName, instruments, (i) => i, withDestinationCopy);
    const native = this.native;
    return planarBounce(
      native.bounceWithSf2Instruments(bindings, settings),
      native.getSampleRate(),
    );
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
