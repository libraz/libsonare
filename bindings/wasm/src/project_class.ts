import { ErrorCode, SonareError } from './errors.js';
import type {
  BuiltinSynthConfig,
  ExternalInstrument,
  PartRigEntry,
  PartRigKey,
  PartRigRequest,
  ProjectInstrumentBinding,
  Sf2InstrumentConfig,
  Sf2ProgramStatus,
  SynthPatch,
} from './instrument_types.js';
import { assertHandleLive } from './internal/handle_guard.js';
import { midiFxConfigJson } from './internal/midi_fx_config.js';
import { mixSceneJsonToNative } from './internal/mix_scene.js';
import { refuseBuiltinSynthOrdinals } from './internal/synth_tokens.js';
import { nativeTranscribeOptions } from './internal/transcribe_request.js';
import {
  assertProjectMidiEvents,
  interleaveChannels,
  type NativeProjectBounce,
  type NativeProjectBounceOptions,
  type NativeSynthBinding,
  normalizeInstrumentBindings,
  normalizePartRig,
  normalizeSynthInstrument,
  optionalId,
  optionalPositiveInt,
  partRigFromNative,
  projectAutomationLaneValue,
  projectBounceResult,
  projectChordSymbolsToNative,
  projectCompileResultFromNative,
  projectFadeCurveValue,
  projectKeySegmentsToNative,
  projectLoopModeName,
  projectLoopModeValue,
  projectMarkerKindName,
  projectMarkerKindValue,
  projectModule,
  projectOverlapPolicyName,
  projectOverlapPolicyValue,
  projectSourceFromNative,
  projectSourceKindName,
  projectStemLayoutValue,
  projectTempoCandidateFromNative,
  projectTrackKindName,
  projectTrackKindValue,
  projectWarpModeValue,
  registerProjectTimeline,
  unregisterProjectTimeline,
  type WasmProject,
  type WasmProjectTimeline,
} from './project_internal.js';
import type {
  ExternalSeparatedStemImportRequest,
  ExternalSeparatedStemImportResult,
  ProjectAnalyzeTempoRequest,
  ProjectAssistSidecar,
  ProjectAssistSidecarInput,
  ProjectAudioSourceMetadataInput,
  ProjectAutomationLaneDesc,
  ProjectAutoTempoOptions,
  ProjectAutoTempoRequest,
  ProjectBounceOptions,
  ProjectBounceResult,
  ProjectChordSymbol,
  ProjectClip,
  ProjectClipCompSegment,
  ProjectClipDesc,
  ProjectClipFadeInput,
  ProjectClipLoopInput,
  ProjectClipTake,
  ProjectClipTakesOptions,
  ProjectCompileResult,
  ProjectCompileTimelineResult,
  ProjectDeserializeResult,
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
  ProjectMoveClipOptions,
  ProjectNotePairValidation,
  ProjectOverlapPolicy,
  ProjectProgramInput,
  ProjectSource,
  ProjectSourceAudioInput,
  ProjectTempoCandidate,
  ProjectTempoOptions,
  ProjectTempoSegment,
  ProjectTimeSignatureSegment,
  ProjectTrack,
  ProjectTrackDesc,
  ProjectTrackKind,
  ProjectTranscribeRequest,
  ProjectWarpMapDesc,
  ProjectWarpMode,
} from './project_types.js';
import type {
  ProjectVocalEditApplyRequest,
  ProjectVocalEditApplyResult,
  ProjectVocalEditDependency,
  ProjectVocalOriginalSource,
  ProjectVocalRehydrateItem,
  ProjectVocalRehydrateOptions,
} from './public_types_vocal_project.js';
import {
  assertSampleRate,
  assertSamples,
  assertU7,
  optionalBoolean,
  requestObject,
  requireBoolean,
  resolveOptionalNonNegative,
  sampleOneShot,
} from './validation.js';
import {
  projectApplyVocalEdit,
  projectGetVocalEditDependencies,
  projectRehydrateVocalEdits,
} from './vocal_project.js';

function validateAssistSidecarUint32(value: unknown, field: string): number {
  if (
    typeof value !== 'number' ||
    !Number.isSafeInteger(value) ||
    value < 0 ||
    value > 0xffff_ffff
  ) {
    throw new RangeError(`Project.setAssistSidecar: ${field} must be a uint32`);
  }
  return value;
}

function validateAssistSidecarPpq(value: unknown, field: string): number {
  if (typeof value !== 'number' || !Number.isFinite(value) || value < 0) {
    throw new RangeError(
      `Project.setAssistSidecar: ${field} must be a finite, non-negative number`,
    );
  }
  return value;
}

function validateAssistSidecarPayload(value: unknown): Uint8Array {
  if (!(value instanceof Uint8Array)) {
    throw new TypeError('Project.setAssistSidecar: payload must be a Uint8Array');
  }
  return value;
}

function validateAssistSidecarModuleId(value: unknown): string {
  if (typeof value !== 'string' || value.length === 0) {
    throw new TypeError('Project.setAssistSidecar: moduleId must be a non-empty string');
  }
  return value;
}

/** The options bag of an entry point that takes an optional one: absent is `{}`, anything else must be an object. */
function optionsObject<T extends object>(
  fnName: Parameters<typeof requestObject>[0],
  options: T | undefined,
): T {
  return options === undefined ? ({} as T) : requestObject(fnName, options, 'options', true);
}

/** A part-rig key, which is exactly a destination and a part. */
function partRigKey(fnName: string, key: unknown): PartRigKey {
  if (typeof key !== 'object' || key === null || Array.isArray(key)) {
    throw new TypeError(`${fnName}: key must be an object`);
  }
  for (const name of Object.keys(key)) {
    if (name !== 'destinationId' && name !== 'part') {
      throw new TypeError(`${fnName}: unknown request key '${name}'`);
    }
  }
  return key as PartRigKey;
}

/** The instrument handed to the native reader, forwarding each callback with the instrument as `this`. */
function externalInstrumentBinding(
  instrument: ExternalInstrument,
): ExternalInstrument & { render: ExternalInstrument['render'] } {
  const wrapped: ExternalInstrument = {
    render: (outputs, frames) => instrument.render(outputs, frames),
  };
  if (typeof instrument.prepare === 'function') {
    wrapped.prepare = (sampleRate, maxBlockFrames, channels) =>
      instrument.prepare?.(sampleRate, maxBlockFrames, channels);
  }
  if (typeof instrument.onEvent === 'function') {
    wrapped.onEvent = (event) => instrument.onEvent?.(event);
  }
  if (instrument.latencySamples !== undefined) {
    wrapped.latencySamples = instrument.latencySamples;
  }
  if (instrument.tailSamples !== undefined) {
    wrapped.tailSamples = instrument.tailSamples;
  }
  return wrapped as ExternalInstrument & { render: ExternalInstrument['render'] };
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
  const channels = audio.length;
  const frames = channels > 0 ? audio[0].length : 0;
  if (audio.some((plane) => plane.length !== frames)) {
    throw new RangeError('addLoopRecordingTakes: planar audio channels must have equal lengths');
  }
  const interleaved = new Float32Array(frames * channels);
  for (let c = 0; c < channels; c++) {
    const plane = audio[c];
    for (let i = 0; i < frames; i++) {
      interleaved[i * channels + c] = plane[i];
    }
  }
  return { ...desc, audio: interleaved, audioChannels: channels };
}

/** Marks a {@link ProjectTimeline} construction as coming from this module. */
const timelineToken = Symbol('ProjectTimeline');

/**
 * An immutable compiled snapshot of a {@link Project}, produced by
 * {@link Project.compileTimeline} and installed into a stopped realtime engine
 * with `RealtimeEngine.applyProjectTimeline`.
 *
 * An engine keeps its own reference after an apply, so {@link destroy} may be
 * called right afterwards. The embind handle is not garbage-collected; release
 * it when no further apply needs it.
 */
export class ProjectTimeline {
  private native: WasmProjectTimeline | null;

  /**
   * Timelines are produced by {@link Project.compileTimeline}; the constructor is
   * not callable from outside this module and throws `TypeError`.
   *
   * @internal
   */
  private constructor(token: symbol, native: WasmProjectTimeline) {
    if (token !== timelineToken) {
      throw new TypeError('ProjectTimeline cannot be constructed; use Project.compileTimeline()');
    }
    this.native = native;
    registerProjectTimeline(this, native);
  }

  /** Release the underlying WASM object. Idempotent. */
  destroy(): void {
    if (this.native === null) {
      return;
    }
    unregisterProjectTimeline(this);
    this.native.delete();
    this.native = null;
  }

  /** Releases the timeline through {@link destroy}, so the instance works with `using`. */
  [Symbol.dispose](): void {
    this.destroy();
  }
}

/**
 * Headless DAW project (control-thread-only arrangement model).
 *
 * Wraps the embind `Project` class over the C-ABI keystone
 * `sonare_c_project.{h,cpp}`. Construct an empty project with `new Project()`,
 * or deserialize one with {@link Project.fromJson}; serialize back with
 * {@link toJson}; compile to a renderable timeline with {@link compile}; render
 * offline to planar float audio with {@link bounce}. The edit and MIDI
 * methods mirror the Node/Python project bindings.
 *
 * Call {@link destroy} (or use a `try/finally`) to release the underlying WASM
 * object — the embind handle is not garbage-collected automatically.
 *
 * @example
 * ```typescript
 * const project = new Project();
 * try {
 *   project.setSampleRate(48000);
 *   const json = project.toJson();
 *   const restored = Project.fromJson(json);
 *   restored.destroy();
 * } finally {
 *   project.destroy();
 * }
 * ```
 */
export class Project {
  private nativeHandle: WasmProject;

  /** The native project; refused while an instrument bounce is calling back into JavaScript. */
  private get native(): WasmProject {
    assertHandleLive(this.released, 'Project');
    this.assertNotBusy();
    return this.nativeHandle;
  }

  private assertNotBusy(): void {
    if (this.instrumentBounceActive) {
      throw new SonareError(
        ErrorCode.InvalidState,
        'Project is busy: it cannot be used from inside a bounceWithInstruments callback',
      );
    }
  }

  private set native(handle: WasmProject) {
    this.nativeHandle = handle;
  }

  private instrumentBounceActive = false;

  /**
   * Applies a vocal edit to a clip's audio.
   *
   * @param request - The edit and the original audio it applies to
   * @returns The rendered result and the stored edit state
   */
  applyVocalEdit(request: ProjectVocalEditApplyRequest): ProjectVocalEditApplyResult {
    return projectApplyVocalEdit(this.native, request);
  }

  /** The vocal-edit dependencies of the project's clips, in clip order. */
  vocalEditDependencies(): ProjectVocalEditDependency[] {
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
    this.activeVocalRehydrateCalls += 1;
    try {
      return projectRehydrateVocalEdits(this.native, originals, options);
    } finally {
      this.activeVocalRehydrateCalls -= 1;
      if (this.activeVocalRehydrateCalls === 0 && this.deleteRequested) {
        this.nativeHandle.delete();
      }
    }
  }

  private released = false;
  private deleteRequested = false;
  private activeVocalRehydrateCalls = 0;

  constructor() {
    this.nativeHandle = new (projectModule().Project)();
  }

  /**
   * Deserialize project JSON into a new {@link Project}. Throws if the JSON is
   * malformed, surfacing the joined diagnostic messages.
   */
  static fromJson(json: string): Project {
    const project = new Project();
    // Replace the freshly-created empty handle with the deserialized one. If
    // fromJson throws (malformed JSON) the empty handle is released first so no
    // WASM object leaks.
    const restored = (() => {
      try {
        return projectModule().Project.fromJson(json);
      } catch (error) {
        project.native.delete();
        throw error;
      }
    })();
    project.native.delete();
    project.native = restored;
    return project;
  }

  /**
   * Deserialize project JSON and return native warning diagnostics emitted on
   * successful loads, such as dangling source references preserved for repair.
   */
  static fromJsonWithDiagnostics(json: string): ProjectDeserializeResult {
    const project = new Project();
    const restored = (() => {
      try {
        return projectModule().Project.fromJsonWithDiagnostics(json);
      } catch (error) {
        project.native.delete();
        throw error;
      }
    })();
    project.native.delete();
    project.native = restored.project;
    return { project, diagnostics: restored.diagnostics };
  }

  /** Serialize the project (+ MIDI content) to deterministic JSON. */
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

  /** Add a track and return its allocated stable id. */
  addTrack(desc: ProjectTrackDesc = {}): number {
    return this.native.addTrack({
      ...desc,
      kind: projectTrackKindValue(desc.kind ?? 'audio', 'addTrack'),
    });
  }

  /**
   * Add an audio or MIDI clip and return its allocated clip id. Audio is given
   * as planar `channels` with its `sampleRate`; a clip with neither is a MIDI
   * clip or a metadata-only audio clip named by `sourceUri`.
   */
  addClip(desc: ProjectClipDesc): number {
    const { channels, sampleRate, ...rest } = requestObject('Project.addClip', desc, 'desc', true);
    if (channels === undefined) {
      return this.native.addClip(rest);
    }
    const planar = interleaveChannels('Project.addClip', channels, sampleRate);
    return this.native.addClip({
      ...rest,
      audio: planar.audio,
      audioChannels: planar.numChannels,
      audioSampleRate: sampleRate,
    });
  }

  /** Import host-separated PCM through the normal audio-track/clip path. */
  importExternalStems(
    request: ExternalSeparatedStemImportRequest,
  ): ExternalSeparatedStemImportResult {
    const { sampleRate, stems } = requestObject(
      'importExternalSeparatedStem',
      request,
      'request',
      true,
    );
    if (!Array.isArray(stems)) {
      throw new TypeError('importExternalStems: stems must be an array');
    }
    return this.native.importExternalStems({
      sampleRate,
      stems: stems.map((stem) => {
        const { channels, layout, ...rest } = stem;
        return {
          ...rest,
          layout: projectStemLayoutValue('importExternalStems', layout),
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

  /** Split a clip at `splitPpq` and return the new clip id. */
  splitClip(clipId: number, splitPpq: number): number {
    return this.native.splitClip(clipId, splitPpq);
  }

  /** Trim a clip's start / length in PPQ. */
  trimClip(clipId: number, newStartPpq: number, newLengthPpq: number): void {
    this.native.trimClip(clipId, newStartPpq, newLengthPpq);
  }

  /** Move a clip to `newStartPpq`, and to another track when `options.trackId` is given. */
  moveClip(clipId: number, newStartPpq: number, options?: ProjectMoveClipOptions): void {
    const trackId = optionalId(
      'moveClip',
      'trackId',
      optionsObject('Project.moveClip', options).trackId,
    );
    this.native.moveClip(clipId, newStartPpq, trackId ?? 0);
  }

  /** Change a track kind via an undoable edit. */
  setTrackKind(trackId: number, kind: ProjectTrackKind): void {
    this.native.setTrackKind(trackId, projectTrackKindValue(kind, 'setTrackKind'));
  }

  /** Set a clip's warp reference id (0 clears it). */
  setClipWarpRef(clipId: number, warpRefId: number): void {
    this.native.setClipWarpRef(clipId, warpRefId);
  }

  /** Set a clip's warp playback mode. */
  setClipWarpMode(clipId: number, mode: ProjectWarpMode): void {
    this.native.setClipWarpMode(clipId, projectWarpModeValue(mode));
  }

  /** Add or replace a first-class warp map referenced by clip warp ids. */
  setWarpMap(map: ProjectWarpMapDesc): void {
    this.native.setWarpMap(map);
  }

  /** Remove a first-class warp map by id. */
  removeWarpMap(warpRefId: number): void {
    this.native.removeWarpMap(warpRefId);
  }

  /**
   * Route a track's MIDI to host-instrument `destinationId` (0 = default). The
   * compiler stamps every MIDI clip on the track with this id so the engine
   * dispatches its events to the instrument registered for that destination.
   * Routes through an undoable edit command. Builtin, NativeSynth, and SF2
   * instruments retain source-track provenance inside a shared destination
   * voice pool. With only zero-latency instruments bound, live lanes and
   * channel-strip bounces remain aligned. Configure one live lane per source
   * track that needs strip processing.
   */
  setTrackMidiDestination(trackId: number, destinationId: number): void {
    this.native.setTrackMidiDestination(trackId, destinationId);
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

  /**
   * Set the rig of one part (0-15), or of the destination's default when `part`
   * is {@link PART_RIG_ALL_PARTS}, on MIDI destination `destinationId`, via an
   * undoable edit. `'chain'` takes 1-8 `inserts` and is the only mode that does;
   * a destination no track references is stored and read back all the same.
   * Refused: a malformed insert document (invalid format), an out-of-range part,
   * an unknown mode or processor, or inserts on a non-chain mode (invalid
   * parameter), and an insert chain in a build without mastering.
   */
  setPartRig(request: PartRigRequest): void {
    requestObject('setPartRig', request, 'request', true);
    if (typeof request.mode === 'number') {
      throw new TypeError('setPartRig: mode must be a string token, not an ordinal');
    }
    const rig = normalizePartRig('setPartRig', request);
    this.native.setPartRig(rig.destinationId, rig.part, rig.mode as never, rig.insertsJson);
  }

  /** The stored rig for `key`, or null when none is stored. */
  partRig(key: PartRigKey): PartRigEntry | null {
    const rig = normalizePartRig('partRig', partRigKey('partRig', key));
    return partRigFromNative(this.native.getPartRig(rig.destinationId, rig.part));
  }

  /** Remove the stored rig for `key` via an undoable edit (no-op when absent). */
  clearPartRig(key: PartRigKey): void {
    const rig = normalizePartRig('clearPartRig', partRigKey('clearPartRig', key));
    this.native.clearPartRig(rig.destinationId, rig.part);
  }

  /** Set a track's mute flag via an undoable edit (a muted track is silent). */
  setTrackMute(trackId: number, mute: boolean): void {
    this.native.setTrackMute(trackId, requireBoolean('setTrackMute', mute, 'mute'));
  }

  /** Set a track's solo flag via an undoable edit (when any track is soloed, only soloed tracks sound). */
  setTrackSolo(trackId: number, solo: boolean): void {
    this.native.setTrackSolo(trackId, requireBoolean('setTrackSolo', solo, 'solo'));
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
   * @throws {RangeError} when `pan` is outside `[-1, 1]` or non-finite, or the
   *   track is unknown; the project and its undo history are left unchanged.
   */
  setTrackPan(trackId: number, pan: number): void {
    this.native.setTrackPan(trackId, pan);
  }

  /** Undo the most recent edit. */
  undo(): void {
    this.native.undo();
  }

  /** Redo the most recently undone edit. */
  redo(): void {
    this.native.redo();
  }

  /** Clear the undo/redo history without changing the current project state. */
  clearHistory(): void {
    this.native.clearHistory();
  }

  /** Cap the undo history depth (clamped to >= 1); evicts oldest entries beyond the cap. */
  setMaxUndoDepth(depth: number): void {
    if (!Number.isInteger(depth) || depth < 1) {
      throw new RangeError('Project.setMaxUndoDepth: depth must be an integer >= 1');
    }
    this.native.setMaxUndoDepth(depth);
  }

  /** Set the combined undo/redo history byte cap. Zero disables retention. */
  setMaxHistoryBytes(bytes: number): void {
    if (typeof bytes !== 'number') {
      throw new TypeError('Project.setMaxHistoryBytes: bytes must be a number');
    }
    if (!Number.isFinite(bytes) || !Number.isInteger(bytes) || bytes < 0 || bytes > 0xffff_ffff) {
      throw new RangeError(
        'Project.setMaxHistoryBytes: bytes must be a finite integer in the uint32 range',
      );
    }
    this.native.setMaxHistoryBytes(bytes);
  }

  /**
   * Replace a MIDI clip's entire event list.
   *
   * @remarks
   * Drops the clip's SysEx, which {@link importSmf} and {@link exportSmf} both
   * keep. A clip's SysEx payloads sit beside the event list and are reached by
   * a handle {@link ProjectMidiEvent} does not carry, so replacing the list
   * leaves nothing referring to them: a GS setup block that survives an import
   * and an export byte for byte is gone after one call here. Nothing reads the
   * handles back either, so a caller that must keep the setup edits the
   * exported file rather than the event list.
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
  importSmf(data: Uint8Array): number {
    return this.native.importSmf(data);
  }

  /**
   * Export the project's tempo map + MIDI clips to an SMF byte buffer.
   *
   * @remarks
   * The buffer owns a plain `ArrayBuffer`, which is what the `Blob` / `File`
   * constructors accept — so `new Blob([project.exportSmf()])` compiles without
   * a copy through `new Uint8Array(...)` first.
   */
  exportSmf(): Uint8Array<ArrayBuffer> {
    return this.native.exportSmf();
  }

  /**
   * Import a MIDI 2.0 Clip File (`SMF2CLIP`); returns the first added clip id.
   * Unlike {@link importSmf}, MIDI 2.0 channel-voice messages (16-bit velocity,
   * 32-bit CC, per-note / registered controllers, bank-valid Program Change)
   * survive without loss.
   */
  importClipFile(data: Uint8Array): number {
    return this.native.importClipFile(data);
  }

  /**
   * Export the project's tempo map + MIDI clips to a MIDI 2.0 Clip File
   * (`SMF2CLIP`) byte buffer. MIDI 2.0-only events are written without loss —
   * prefer this over {@link exportSmf} when MIDI 2.0 fidelity matters.
   *
   * @remarks
   * As with {@link exportSmf}, the buffer owns a plain `ArrayBuffer` and goes
   * straight into a `Blob`.
   */
  exportClipFile(): Uint8Array<ArrayBuffer> {
    return this.native.exportClipFile();
  }

  /**
   * Set a MIDI clip's channel-0 program / bank at source PPQ 0. Omit `bank` for
   * no Bank Select, matching `setProgramOnChannel` and the Node/Python surfaces;
   * pass `>= 0` to emit a Bank Select. A negative `bank` is refused.
   */
  setProgram(clipId: number, program: number, bank?: number): void {
    this.native.setProgram(clipId, program, resolveOptionalNonNegative('setProgram', bank, 'bank'));
  }

  /** Set a MIDI clip's program / bank for one UMP group and channel. */
  setProgramOnChannel(clipId: number, input: ProjectProgramInput): void {
    const { group, channel, program, bank } = requestObject(
      'Project.setProgramOnChannel',
      input,
      'input',
      true,
    );
    if (bank !== undefined && (typeof bank !== 'object' || bank === null)) {
      throw new TypeError('setProgramOnChannel: bank must be an object { msb, lsb }');
    }
    // The native call takes one 14-bit bank (MSB << 7 | LSB); a negative omits Bank Select.
    const packed =
      bank === undefined
        ? undefined
        : (assertU7('setProgramOnChannel', bank.msb, 'bank.msb') << 7) |
          assertU7('setProgramOnChannel', bank.lsb, 'bank.lsb');
    this.native.setProgramOnChannel(
      clipId,
      group,
      channel,
      program,
      resolveOptionalNonNegative('setProgramOnChannel', packed, 'bank'),
    );
  }

  /**
   * Destructively bake a MIDI-FX chain into all stored events. Large clips are
   * drained without truncation; failure leaves the original clip unchanged.
   * Setting `withSourceIndex` also returns per-event provenance, so a selection
   * or an editorial annotation can be carried across the bake.
   */
  bakeMidiFx(request: ProjectMidiFxBakeRequest): ProjectMidiFxBakeResult {
    const { clipId, config, withSourceIndex } = requestObject(
      'bakeMidiFx',
      request,
      'request',
      true,
    );
    const configJson = midiFxConfigJson(config, 'bakeMidiFx');
    if (!optionalBoolean('bakeMidiFx', withSourceIndex, 'withSourceIndex', false)) {
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
    const { clipId, config } = requestObject('previewMidiFx', request, 'request', true);
    const configJson = midiFxConfigJson(config, 'previewMidiFx');
    return this.native.previewMidiFxCount(clipId, configJson);
  }

  /**
   * Pre-flight check for hanging / unmatched notes in a MIDI clip: reports
   * whether every note-on in the exported half-open playback window
   * `[0, lengthPpq)` has a matching note-off (FIFO per group+channel+note). A
   * note still sounding at the clip end is closed there by export, so it
   * validates as paired. Useful before bouncing to catch a stuck note. Throws if `clipId` is unknown or not a MIDI clip.
   */
  validateMidiNotes(clipId: number): ProjectNotePairValidation {
    return this.native.validateMidiNotes(clipId);
  }

  /**
   * Transcribe mono audio straight into a MIDI clip's event list, **replacing**
   * whatever it held — exactly as {@link setMidiEvents} does.
   *
   * The PPQ grid is this project's own tempo map, which is why there is no
   * `tempoBpm` field: a project whose tempo was installed by {@link autoTempo}
   * transcribes onto that map rather than onto a second, separately detected
   * tempo. Use the standalone `transcribe` when you want events without a
   * project.
   *
   * Quantizing, tempo detection and key/chord annotation are not done here —
   * see `transcribe` for what each belongs to.
   *
   * @returns the number of notes written (half the events)
   * @throws {RangeError} on empty `samples`, a non-finite sample, or a
   *   `sampleRate` outside `[8000, 384000]`
   * @throws {RangeError} on an option outside its domain
   * @throws {RangeError} when `clipId` is unknown or not a MIDI clip
   * @throws {SonareError} `NotSupported` when the library was built without the pitch
   *   editor
   */
  transcribeToClip(request: ProjectTranscribeRequest): number {
    assertSamples('Project.transcribeToClip', request.samples, true);
    assertSampleRate('Project.transcribeToClip', request.sampleRate);
    const { clipId, samples, sampleRate, ...options } = request;
    return this.native.transcribeToClip(
      clipId,
      samples,
      sampleRate,
      nativeTranscribeOptions('Project.transcribeToClip', options),
    );
  }

  /**
   * Return ranked tempo-octave and detected-meter candidates without editing.
   * Takes `(samples, sampleRate, options?)` or one request object.
   */
  analyzeTempo(
    samples: Float32Array,
    sampleRate: number,
    options?: ProjectTempoOptions,
  ): ProjectTempoCandidate[];
  analyzeTempo(request: ProjectAnalyzeTempoRequest): ProjectTempoCandidate[];
  analyzeTempo(...args: unknown[]): ProjectTempoCandidate[] {
    const { samples, sampleRate, ...options } = sampleOneShot<ProjectAnalyzeTempoRequest>(
      'Project.analyzeTempo',
      args,
    );
    return this.native
      .analyzeTempo(samples, sampleRate, options)
      .map(projectTempoCandidateFromNative);
  }

  /**
   * Detect and install a ranked tempo candidate; optionally apply the detected
   * meter. Takes `(samples, sampleRate, options?)` or one request object.
   *
   * @remarks
   * `candidateIndex` indexes the ranking {@link analyzeTempo} produced, so pair
   * the two on the same scoring options. Read the installed map back with
   * {@link tempoSegments}.
   */
  autoTempo(samples: Float32Array, sampleRate: number, options?: ProjectAutoTempoOptions): number;
  autoTempo(request: ProjectAutoTempoRequest): number;
  autoTempo(...args: unknown[]): number {
    const { samples, sampleRate, candidateIndex, applyTimeSignatures, ...options } =
      sampleOneShot<ProjectAutoTempoRequest>('Project.autoTempo', args);
    return this.native.autoTempo(
      samples,
      sampleRate,
      candidateIndex ?? 0,
      optionalBoolean('autoTempo', applyTimeSignatures, 'applyTimeSignatures', false),
      options,
    );
  }

  /** Snap to a bar (`division=0`), beat (`1`), or beat subdivision (`2+`). */
  snapToGrid(ppq: number, strength = 1.0, division = 1): number {
    return this.native.snapToGrid(ppq, strength, division);
  }

  /** Compile the project into a renderable timeline, surfacing diagnostics. */
  compile(): ProjectCompileResult {
    return projectCompileResultFromNative(this.native.compile());
  }

  /**
   * Compile the project and keep the resulting timeline. The result is
   * {@link compile}'s plus `timeline`: a {@link ProjectTimeline} to install with
   * `RealtimeEngine.applyProjectTimeline`, absent when an error diagnostic
   * means nothing compiled.
   */
  compileTimeline(): ProjectCompileTimelineResult {
    const { timeline, ...result } = this.native.compileTimeline();
    const compiled = projectCompileResultFromNative(result);
    if (timeline === null) {
      return compiled;
    }
    return {
      ...compiled,
      timeline: new (
        ProjectTimeline as unknown as new (
          token: symbol,
          native: WasmProjectTimeline,
        ) => ProjectTimeline
      )(timelineToken, timeline),
    };
  }

  /** The native options of one bounce, after the sentinel refusals. */
  private bounceRequest(options: ProjectBounceOptions | undefined): NativeProjectBounceOptions {
    const request = optionsObject('Project.bounce', options);
    const native: NativeProjectBounceOptions = {};
    const totalFrames = optionalPositiveInt('bounce', 'totalFrames', request.totalFrames);
    const blockSize = optionalPositiveInt('bounce', 'blockSize', request.blockSize);
    const numChannels = optionalPositiveInt('bounce', 'numChannels', request.numChannels);
    if (totalFrames !== undefined) {
      native.totalFrames = totalFrames;
    }
    if (blockSize !== undefined) {
      native.blockSize = blockSize;
    }
    if (numChannels !== undefined) {
      native.numChannels = numChannels;
    }
    if (request.instrumentLatencySamples !== undefined) {
      const latency = request.instrumentLatencySamples;
      if (typeof latency !== 'number') {
        throw new TypeError('bounce: instrumentLatencySamples must be a number');
      }
      native.instrumentLatencySamples = latency;
    }
    return native;
  }

  private bounceResult(raw: NativeProjectBounce): ProjectBounceResult {
    return projectBounceResult(raw, this.native.getSampleRate());
  }

  /**
   * Compile + render the project offline to planar float audio. MIDI tracks
   * render silently here (no instrument is bound) — use
   * {@link bounceWithBuiltinInstruments} to make MIDI audible.
   *
   * When `totalFrames` is omitted the render length is derived from the
   * arrangement, so a project with content renders without computing a frame
   * count; an empty project yields no frames. The result is at the project
   * sample rate; a bounce is never resampled.
   *
   * @example
   * ```typescript
   * const { channels, sampleRate } = project.bounce({ numChannels: 2 });
   * ```
   */
  bounce(options?: ProjectBounceOptions): ProjectBounceResult {
    const native = this.bounceRequest(options);
    return this.bounceResult(this.native.bounce(native));
  }

  /**
   * Compile + render the project offline, routing MIDI tracks through the
   * built-in oscillator synth so a MIDI-only arrangement bounces to audible
   * audio. Each pair binds a {@link BuiltinSynthConfig} to a MIDI destination
   * (`destinationId` omitted is destination `0`). An empty array renders
   * silence for MIDI tracks, identical to {@link bounce}. A bare config without
   * an `instrument` key is refused by name.
   *
   * Like {@link bounce}, omitting `totalFrames` derives the render length from
   * the arrangement plus the synth's release tail.
   *
   * @example
   * ```typescript
   * // MIDI-only project -> non-silent stereo audio.
   * const { channels } = project.bounceWithBuiltinInstruments(
   *   { instrument: { waveform: 'saw' } },
   *   { numChannels: 2 },
   * );
   * ```
   */
  bounceWithBuiltinInstruments(
    instruments:
      | ProjectInstrumentBinding<BuiltinSynthConfig>
      | ReadonlyArray<ProjectInstrumentBinding<BuiltinSynthConfig>> = [],
    options?: ProjectBounceOptions,
  ): ProjectBounceResult {
    const bindings = normalizeInstrumentBindings(
      'bounceWithBuiltinInstruments',
      instruments,
      (instrument) => {
        refuseBuiltinSynthOrdinals('bounceWithBuiltinInstruments', instrument);
        return instrument;
      },
    );
    const native = this.bounceRequest(options);
    return this.bounceResult(this.native.bounceWithBuiltinInstrument(bindings, native));
  }

  /**
   * Like {@link bounce}, but drives MIDI tracks routed to a destination through
   * instruments you implement in JavaScript. Each pair binds an
   * {@link ExternalInstrument} to a destination (`destinationId` omitted is
   * destination `0`).
   *
   * Every callback runs synchronously while this method is blocked, so there is
   * no async instrument: a callback that returns a Promise is refused with a
   * `TypeError`. A callback that throws stops all further callbacks, and the
   * bounce rethrows that first error once it returns (a native failure is
   * reported ahead of it). The `outputs` passed to `render` are zero-filled
   * scratch arrays, added into the engine's buffers when `render` returns
   * normally.
   *
   * While the bounce runs, using this project from inside a callback (a
   * mutation, a nested bounce, or `delete`) throws `InvalidState`.
   *
   * One instrument shared by tracks that feed different channel strips throws
   * a `SonareError` with `NOT_SUPPORTED`, because callback audio carries no
   * source-track attribution; bind one destination per strip instead.
   */
  bounceWithInstruments(
    instruments:
      | ProjectInstrumentBinding<ExternalInstrument>
      | ReadonlyArray<ProjectInstrumentBinding<ExternalInstrument>> = [],
    options?: ProjectBounceOptions,
  ): ProjectBounceResult {
    const method = 'bounceWithInstruments';
    const pairs = Array.isArray(instruments) ? instruments : [instruments];
    pairs.forEach((pair, index) => {
      const instrument = (pair as { instrument?: ExternalInstrument } | null)?.instrument;
      for (const key of ['prepare', 'onEvent', 'render'] as const) {
        const callback = instrument?.[key];
        if (
          instrument !== undefined &&
          typeof callback !== 'function' &&
          (key === 'render' || (callback ?? null) !== null)
        ) {
          throw new TypeError(
            `${method}: instruments[${index}].instrument.${key} must be a function`,
          );
        }
      }
    });
    const bindings = normalizeInstrumentBindings(method, instruments, externalInstrumentBinding);
    const nativeOptions = this.bounceRequest(options);
    const native = this.native;
    this.instrumentBounceActive = true;
    let result: ReturnType<WasmProject['bounceWithInstruments']>;
    try {
      result = native.bounceWithInstruments(bindings, nativeOptions);
    } finally {
      this.instrumentBounceActive = false;
    }
    // The adapter reports a throwing callback as data instead of throwing
    // through C++ frames, whose destructors a JS exception would skip.
    if ('instrumentFailure' in result) {
      throw result.instrumentFailure;
    }
    return this.bounceResult(result);
  }

  /**
   * Compile + render the project offline, routing MIDI tracks through the
   * patch-driven NativeSynth — the full synthesizer (every
   * {@link SynthEngineMode} engine plus the realism layer; the modes are
   * enumerated by {@link SYNTH_ENGINE_MODES}). Each pair binds a
   * {@link SynthPatch} (name a catalog preset through its `preset` field; see
   * {@link synthPresetNames}) to a destination (`destinationId` omitted is
   * destination `0`). An empty array renders silence for MIDI tracks. Unknown
   * preset names throw. With `useGmPrograms` enabled, MIDI program changes
   * select the corresponding General MIDI voice while the patch remains the
   * fallback. Deterministic for a fixed project + options + patch.
   *
   * An `engineMode: 'sample'` patch reads its PCM from the {@link SampleBank}
   * passed as `sampleBank`; the bank must still be alive when the bounce runs,
   * and one bound without a bank renders silence.
   */
  bounceWithSynthInstruments(
    instruments:
      | ProjectInstrumentBinding<SynthPatch>
      | ReadonlyArray<ProjectInstrumentBinding<SynthPatch>> = [],
    options?: ProjectBounceOptions,
  ): ProjectBounceResult {
    const bindings = normalizeInstrumentBindings(
      'bounceWithSynthInstruments',
      instruments,
      (instrument) =>
        normalizeSynthInstrument('bounceWithSynthInstruments', instrument) as NativeSynthBinding,
    );
    const native = this.bounceRequest(options);
    return this.bounceResult(this.native.bounceWithSynthInstrument(bindings, native));
  }

  /**
   * Load (parse) SoundFont 2 bytes into the project: presets / instruments /
   * sample headers plus the sample PCM decoded to a float pool. The host
   * fetches the `.sf2` and passes the raw bytes; they are copied into linear
   * memory for the call and not referenced afterwards. Replaces any previously
   * loaded SoundFont; throws on malformed input (the previous SoundFont is
   * kept).
   */
  loadSoundFont(data: Uint8Array): void {
    this.native.loadSoundFont(data);
  }

  /** Release the project's loaded SoundFont (no-op when none is loaded). */
  clearSoundFont(): void {
    this.native.clearSoundFont();
  }

  /** Number of presets in the loaded SoundFont (0 when none is loaded). */
  soundFontPresetCount(): number {
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
   * per-program backend). Each pair binds a {@link Sf2InstrumentConfig} to a
   * destination (`destinationId` omitted is destination `0`); an empty array
   * renders silence for MIDI tracks.
   */
  bounceWithSf2Instruments(
    instruments:
      | ProjectInstrumentBinding<Sf2InstrumentConfig>
      | ReadonlyArray<ProjectInstrumentBinding<Sf2InstrumentConfig>> = [],
    options?: ProjectBounceOptions,
  ): ProjectBounceResult {
    const bindings = normalizeInstrumentBindings(
      'bounceWithSf2Instruments',
      instruments,
      (instrument) => instrument,
    );
    const native = this.bounceRequest(options);
    return this.bounceResult(this.native.bounceWithSf2Instrument(bindings, native));
  }

  /** Remove a clip (undoable). */
  removeClip(clipId: number): void {
    this.native.removeClip(clipId);
  }

  /** Set a clip's linear playback gain (>= 0; undoable). */
  setClipGain(clipId: number, gain: number): void {
    this.native.setClipGain(clipId, gain);
  }

  /** Set a clip's fade-in / fade-out regions from one object whose lengths carry their unit (undoable). */
  setClipFade(clipId: number, input: ProjectClipFadeInput): void {
    const { fadeInPpq, fadeInCurve, fadeOutPpq, fadeOutCurve } = requestObject(
      'Project.setClipFade',
      input,
      'input',
      true,
    );
    this.native.setClipFade(
      clipId,
      {
        lengthPpq: fadeInPpq ?? 0,
        curve: projectFadeCurveValue('setClipFade', 'fadeInCurve', fadeInCurve),
      },
      {
        lengthPpq: fadeOutPpq ?? 0,
        curve: projectFadeCurveValue('setClipFade', 'fadeOutCurve', fadeOutCurve),
      },
    );
  }

  /** Audio source ids that need decoded PCM after deserialization. */
  unresolvedAudioSourceIds(): number[] {
    return this.native.unresolvedAudioSourceIds();
  }

  /** Unresolved audio source descriptors, in the order of {@link unresolvedAudioSourceIds}; `nameOrUri` is the full URI. */
  unresolvedAudioSources(): ProjectSource[] {
    return this.native.unresolvedAudioSources().map(projectSourceFromNative);
  }

  /** Register decoded planar PCM for an existing audio source (undoable). */
  setSourceAudio(input: ProjectSourceAudioInput): void {
    const { sourceId, channels, sampleRate } = requestObject(
      'Project.setSourceAudio',
      input,
      'input',
      true,
    );
    const planar = interleaveChannels('setSourceAudio', channels, sampleRate);
    this.native.setSourceAudio(sourceId, planar.audio, planar.numChannels, sampleRate);
  }

  /** Replace an audio source's metadata strings as one undoable edit; an omitted string is cleared. */
  setAudioSourceMetadata(input: ProjectAudioSourceMetadataInput): void {
    const { sourceId, contentHash, externalStemRole } = requestObject(
      'Project.setAudioSourceMetadata',
      input,
      'input',
      true,
    );
    this.native.setAudioSourceMetadata(sourceId, contentHash ?? '', externalStemRole ?? '');
  }

  /**
   * Replace a clip's take list (undoable). `options.activeTakeId` names the take
   * that plays; omit it for the clip's base source.
   */
  setClipTakes(
    clipId: number,
    takes: ReadonlyArray<ProjectClipTake>,
    options?: ProjectClipTakesOptions,
  ): void {
    const activeTakeId = optionalId(
      'setClipTakes',
      'activeTakeId',
      optionsObject('Project.setClipTakes', options).activeTakeId,
    );
    this.native.setClipTakes(clipId, takes, activeTakeId ?? 0);
  }

  /** Replace a clip's comp segments (undoable). */
  setClipCompSegments(clipId: number, segments: ReadonlyArray<ProjectClipCompSegment>): void {
    this.native.setClipCompSegments(clipId, segments);
  }

  /**
   * Set a clip's loop mode and loop length in PPQ (undoable). `loopCrossfadePpq`
   * is an equal-power crossfade at the loop seam (PPQ, finite and >= 0); omit it
   * for a hard loop. The engine clamps it to the clip's pre-roll and half the loop.
   */
  setClipLoop(clipId: number, input: ProjectClipLoopInput): void {
    const { loopMode, loopLengthPpq, loopCrossfadePpq } = requestObject(
      'Project.setClipLoop',
      input,
      'input',
      true,
    );
    this.native.setClipLoop(
      clipId,
      projectLoopModeValue(loopMode),
      loopLengthPpq ?? 0,
      loopCrossfadePpq ?? 0,
    );
  }

  /** Rebind a clip to a different (already-registered) source (undoable). */
  setClipSource(clipId: number, sourceId: number): void {
    this.native.setClipSource(clipId, sourceId);
  }

  /** Duplicate a clip at `newStartPpq` (same track); returns the new clip id. */
  duplicateClip(clipId: number, newStartPpq: number): number {
    return this.native.duplicateClip(clipId, newStartPpq);
  }

  /** Remove a track and its clips (undoable). */
  removeTrack(trackId: number): void {
    this.native.removeTrack(trackId);
  }

  /** Rename a track (undoable). */
  renameTrack(trackId: number, name: string): void {
    this.native.renameTrack(trackId, name);
  }

  /** Set a track's mixer-strip binding + output target (undoable; omit / '' clears). */
  setTrackRoute(trackId: number, channelStripRef?: string, outputTarget?: string): void {
    this.native.setTrackRoute(trackId, channelStripRef ?? '', outputTarget ?? '');
  }

  /** Append an automation lane; returns its stable target parameter id (undoable). */
  addAutomationLane(trackId: number, desc: ProjectAutomationLaneDesc): number {
    return this.native.addAutomationLane(
      trackId,
      projectAutomationLaneValue(desc, 'addAutomationLane'),
    );
  }

  /** Replace the lane identified by its stable target parameter id (undoable). */
  editAutomationLane(
    trackId: number,
    targetParamId: number,
    desc: ProjectAutomationLaneDesc,
  ): void {
    this.native.editAutomationLane(
      trackId,
      targetParamId,
      projectAutomationLaneValue(desc, 'editAutomationLane'),
    );
  }

  /** Remove the lane identified by its stable target parameter id (undoable). */
  removeAutomationLane(trackId: number, targetParamId: number): void {
    this.native.removeAutomationLane(trackId, targetParamId);
  }

  /** Replace the project's key annotation stream (undoable). */
  annotateKeys(keys: ReadonlyArray<ProjectKeySegment>): void {
    this.native.annotateKeys(projectKeySegmentsToNative(keys));
  }

  /** Replace the project's chord-symbol annotation stream (undoable). */
  annotateChords(chords: ReadonlyArray<ProjectChordSymbol>): void {
    this.native.annotateChords(projectChordSymbolsToNative(chords));
  }

  /** Add or update an opaque assist sidecar via an undoable edit. */
  setAssistSidecar(sidecar: ProjectAssistSidecarInput): void {
    requestObject('Project.setAssistSidecar', sidecar, 'sidecar', true);
    const moduleId = validateAssistSidecarModuleId(sidecar.moduleId);
    this.native.setAssistSidecar(
      moduleId,
      validateAssistSidecarUint32(sidecar.schemaVersion ?? 0, 'schemaVersion'),
      validateAssistSidecarUint32(sidecar.targetTrackId ?? 0, 'targetTrackId'),
      validateAssistSidecarPpq(sidecar.regionStartPpq ?? 0, 'regionStartPpq'),
      validateAssistSidecarPpq(sidecar.regionEndPpq ?? 0, 'regionEndPpq'),
      validateAssistSidecarPayload(sidecar.payload ?? new Uint8Array()),
    );
  }

  /** Every stored assist sidecar, in stable project order. */
  assistSidecars(): ProjectAssistSidecar[] {
    const count = this.native.assistSidecarCount();
    return Array.from({ length: count }, (_, index) => this.native.getAssistSidecar(index));
  }

  /** Set the project's clip-overlap policy. */
  setOverlapPolicy(policy: ProjectOverlapPolicy): void {
    this.native.setOverlapPolicy(projectOverlapPolicyValue(policy));
  }

  /** The project's clip-overlap policy. */
  get overlapPolicy(): ProjectOverlapPolicy {
    return projectOverlapPolicyName(this.native.getOverlapPolicy());
  }

  /** The project sample rate in Hz. */
  get sampleRate(): number {
    return this.native.getSampleRate();
  }

  /**
   * Replace the project's mixer scene from a scene JSON string.
   *
   * @returns One warning per key the scene reader does not recognise (e.g.
   *   `unknown scene key 'strips[2].faderDB'`; `$`- and `x-`-prefixed keys are exempt).
   *   The scene is still applied; empty when every key was consumed.
   */
  setMixerSceneJson(sceneJson: string): string[] {
    return this.native.setMixerSceneJson(
      mixSceneJsonToNative('Project.setMixerSceneJson', sceneJson),
    );
  }

  /**
   * Add or replace a marker. Omit `id` to allocate a new marker; an explicit `0`
   * is refused. Returns the stable marker id.
   */
  setMarker(input: ProjectMarkerInput): number {
    const { id, ppq, name, kind, keyFifths, keyMinor } = requestObject(
      'Project.setMarker',
      input,
      'input',
      true,
    );
    if (name !== undefined && typeof name !== 'string') {
      throw new TypeError('setMarker: name must be a string');
    }
    return this.native.setMarkerEx({
      id: optionalId('setMarker', 'id', id) ?? 0,
      ppq,
      name: name ?? '',
      kind: projectMarkerKindValue('setMarker', kind),
      keyFifths: keyFifths ?? 0,
      keyMinor: optionalBoolean('setMarker', keyMinor, 'keyMinor', false),
    });
  }

  /** Every marker, in stored order. */
  markers(): ProjectMarker[] {
    return this.readAll(this.native.markerCount(), (index) => {
      const marker = this.native.markerByIndex(index);
      return { ...marker, kind: projectMarkerKindName(marker.kind) };
    });
  }

  /** Every track, in stored order. */
  tracks(): ProjectTrack[] {
    return this.readAll(this.native.trackCount(), (index) => {
      const track = this.native.trackByIndex(index);
      return { ...track, kind: projectTrackKindName(track.kind) };
    });
  }

  /** Every clip, in stored order. */
  clips(): ProjectClip[] {
    return this.readAll(this.native.clipCount(), (index) => {
      const clip = this.native.clipByIndex(index);
      return {
        ...clip,
        sourceKind: projectSourceKindName(clip.sourceKind),
        loopMode: projectLoopModeName(clip.loopMode),
      };
    });
  }

  /** Every audio and MIDI source, in stored order. */
  sources(): ProjectSource[] {
    return this.readAll(this.native.sourceCount(), (index) =>
      projectSourceFromNative(this.native.sourceByIndex(index)),
    );
  }

  /** The tempo map, in the shape {@link setTempoSegments} accepts. */
  tempoSegments(): ProjectTempoSegment[] {
    return this.readAll(this.native.tempoSegmentCount(), (index) => {
      const { startSample: _derived, ...segment } = this.native.tempoSegmentByIndex(index);
      return segment;
    });
  }

  /** The time-signature map, in the shape {@link setTimeSignatures} accepts. */
  timeSignatures(): ProjectTimeSignatureSegment[] {
    return this.readAll(this.native.timeSignatureCount(), (index) =>
      this.native.timeSignatureByIndex(index),
    );
  }

  private readAll<T>(count: number, read: (index: number) => T): T[] {
    return Array.from({ length: count }, (_, index) => read(index));
  }

  /** Replace the project's tempo map with the given segments. */
  setTempoSegments(segments: ReadonlyArray<ProjectTempoSegment>): void {
    this.native.setTempoSegments(segments);
  }

  /** Replace the project's time-signature map with the given segments. */
  setTimeSignatures(segments: ReadonlyArray<ProjectTimeSignatureSegment>): void {
    this.native.setTimeSignatures(segments);
  }

  /**
   * Compile diagnostics produced by the most recent bounce on this project
   * (e.g. MIDI clips rendering silently without a bound instrument).
   *
   * `undefined` when no bounce has run on this project. A clean bounce returns a
   * result with empty diagnostics; a failed one carries at least one error.
   */
  lastBounceCompileResult(): ProjectCompileResult | undefined {
    const raw = this.native.lastBounceCompileResult();
    if (!raw.hasTimeline && raw.diagnostics.length === 0) {
      return undefined;
    }
    return projectCompileResultFromNative(raw);
  }

  /**
   * Release the underlying WASM object. Idempotent, as the Node facade is.
   * Called from a rehydrate cancel callback, the release happens when the
   * outermost {@link rehydrateVocalEdits} call returns.
   */
  destroy(): void {
    if (this.released) {
      return;
    }
    this.assertNotBusy();
    this.released = true;
    if (this.activeVocalRehydrateCalls !== 0) {
      this.deleteRequested = true;
      return;
    }
    this.nativeHandle.delete();
  }

  /** Releases the handle through {@link destroy}, so the instance works with `using`. */
  [Symbol.dispose](): void {
    this.destroy();
  }
}
