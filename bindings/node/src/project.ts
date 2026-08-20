import { addon } from './native.js';
import type {
  BuiltinInstrumentConfig,
  ExternalSeparatedStemImportRequest,
  ExternalSeparatedStemImportResult,
  MidiCcLearnOptions,
  ProjectAssistSidecar,
  ProjectAssistSidecarInput,
  ProjectAutomationLaneDesc,
  ProjectAutomationPoint,
  ProjectBounceOptions,
  ProjectChordSymbol,
  ProjectClip,
  ProjectClipCompSegment,
  ProjectClipDesc,
  ProjectClipFade,
  ProjectClipTake,
  ProjectCompileResult,
  ProjectKeySegment,
  ProjectLoopMode,
  ProjectLoopRecordingDesc,
  ProjectLoopRecordingResult,
  ProjectMarker,
  ProjectMidiCcBinding,
  ProjectMidiClipResult,
  ProjectMidiEvent,
  ProjectMidiFxBakeRequest,
  ProjectMidiFxBakeResult,
  ProjectMidiFxPreviewRequest,
  ProjectMidiRouteConfig,
  ProjectMidiRouteResult,
  ProjectSource,
  ProjectTempoCandidate,
  ProjectTempoOptions,
  ProjectTempoSegment,
  ProjectTimeSignatureSegment,
  ProjectTrack,
  ProjectTrackDesc,
  Sf2InstrumentConfig,
  Sf2ProgramStatus,
  SynthEnumTables,
  SynthPatch,
  SynthWaveform,
  WarpMode,
} from './types.js';
import { assertProjectMidiEvents, midi1Event } from './validation.js';
import {
  projectAutomationLaneValue,
  projectClipFadeValue,
  projectLoopModeValue,
  trackKindValue,
  warpModeValue,
} from './value_coercion.js';

export type { ProjectAutomationTargetKind } from './types.js';
export {
  PROJECT_AUTOMATION_TARGET_OPAQUE,
  PROJECT_AUTOMATION_TARGET_TRACK_FADER_DB,
  PROJECT_AUTOMATION_TARGET_TRACK_PAN,
} from './types.js';

/**
 * Returns the runtime project ABI version of the loaded native binding.
 *
 * Equals {@link EXPECTED_PROJECT_ABI_VERSION} when the arrangement subsystem is
 * compiled in, `0` when the native library was built without it.
 */
export function projectAbiVersion(): number {
  return addon.projectAbiVersion();
}

/**
 * NativeSynth preset catalog names (`'sine'`, `'saw-lead'`, `'e-piano'`,
 * `'drum-kit'`, ...). Use these to discover valid {@link SynthPatch} preset
 * names instead of hardcoding magic strings.
 */
export function synthPresetNames(): string[] {
  return addon.synthPresetNames();
}

/**
 * Fetch a named catalog preset as a {@link SynthPatch} (the preset name plus
 * the wrapper-section values), so hosts can inspect a preset and tweak fields
 * before binding it. A `"va:"` routing prefix is accepted; unknown names
 * throw.
 */
export function synthPresetPatch(name: string): SynthPatch {
  return addon.synthPresetPatch(name);
}

/** Return the canonical NativeSynth enum tables from the native C oracle. */
export function synthEnumTables(): SynthEnumTables {
  return addon._synthEnumTables();
}

/**
 * Folds the positional and request call forms of `bakeMidiFx` into one shape,
 * so defaults, validation and errors cannot diverge between them.
 */
function normalizeMidiFxBakeRequest(
  clipIdOrRequest: number | ProjectMidiFxBakeRequest,
  configJson?: string,
): Required<ProjectMidiFxBakeRequest> {
  if (typeof clipIdOrRequest === 'number') {
    return {
      clipId: clipIdOrRequest,
      configJson: configJson ?? '',
      withSourceIndex: false,
    };
  }
  return {
    clipId: clipIdOrRequest.clipId,
    configJson: clipIdOrRequest.configJson,
    withSourceIndex: clipIdOrRequest.withSourceIndex ?? false,
  };
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
  private native: InstanceType<typeof addon.Project>;
  private disposed = false;

  private constructor(native: InstanceType<typeof addon.Project>) {
    this.native = native;
  }

  /** Create a new empty project (throws on a project ABI mismatch). */
  static create(): Project {
    return new Project(new addon.Project());
  }

  /** Pack a MIDI 1.0 note-on event accepted by {@link setMidiEvents}. */
  static midiNoteOn(
    ppq: number,
    group: number,
    channel: number,
    note: number,
    velocity: number,
  ): ProjectMidiEvent {
    return midi1Event('Project.midiNoteOn', ppq, group, 0x9, channel, note, velocity);
  }

  /** Pack a MIDI 1.0 note-off event accepted by {@link setMidiEvents}. */
  static midiNoteOff(
    ppq: number,
    group: number,
    channel: number,
    note: number,
    velocity = 0,
  ): ProjectMidiEvent {
    return midi1Event('Project.midiNoteOff', ppq, group, 0x8, channel, note, velocity);
  }

  /** Pack a MIDI 1.0 control-change event. */
  static midiCc(
    ppq: number,
    group: number,
    channel: number,
    controller: number,
    value: number,
  ): ProjectMidiEvent {
    return midi1Event('Project.midiCc', ppq, group, 0xb, channel, controller, value);
  }

  /** Pack a MIDI 1.0 poly-pressure event. */
  static midiPolyPressure(
    ppq: number,
    group: number,
    channel: number,
    note: number,
    pressure: number,
  ): ProjectMidiEvent {
    return midi1Event('Project.midiPolyPressure', ppq, group, 0xa, channel, note, pressure);
  }

  /** Pack a MIDI 1.0 program-change event. */
  static midiProgram(
    ppq: number,
    group: number,
    channel: number,
    program: number,
  ): ProjectMidiEvent {
    return midi1Event('Project.midiProgram', ppq, group, 0xc, channel, program, 0);
  }

  /** Return the General MIDI instrument name for `program`, or `null` when out of range. */
  static gmInstrumentName(program: number): string | null {
    return addon.midiGmInstrumentName(program);
  }

  /** Return the General MIDI program number for a canonical instrument name, or `-1`. */
  static gmProgramForName(name: string): number {
    return addon.midiGmProgramForName(name);
  }

  /** Return the General MIDI family name for `family`, or `null` when out of range. */
  static gmFamilyName(family: number): string | null {
    return addon.midiGmFamilyName(family);
  }

  /** Return the first General MIDI program number in `family`, or `-1`. */
  static gmFamilyFirstProgram(family: number): number {
    return addon.midiGmFamilyFirstProgram(family);
  }

  /** Return the GM2 bank/program instrument variation name, or `null` when unavailable. */
  static gm2InstrumentName(bankLsb: number, program: number): string | null {
    return addon.midiGm2InstrumentName(bankLsb, program);
  }

  /** Return the General MIDI drum name for `note`, or `null` when out of range. */
  static gmDrumName(note: number): string | null {
    return addon.midiGmDrumName(note);
  }

  /** Return the General MIDI drum note for a canonical drum name, or `-1`. */
  static gmDrumNoteForName(name: string): number {
    return addon.midiGmDrumNoteForName(name);
  }

  /** Return the GM2 drum-set name for `bankLsb`, or `null` when unavailable. */
  static gm2DrumSetName(bankLsb: number): string | null {
    return addon.midiGm2DrumSetName(bankLsb);
  }

  /** Return the GM2 drum name for `bankLsb`/`note`, or `null` when unavailable. */
  static gm2DrumName(bankLsb: number, note: number): string | null {
    return addon.midiGm2DrumName(bankLsb, note);
  }

  /** Return the MIDI CC name for `controller`, or `null` when out of range. */
  static midiCcName(controller: number): string | null {
    return addon.midiCcName(controller);
  }

  /** Return the MIDI CC number for a canonical controller name, or `-1`. */
  static midiCcIndexForName(name: string): number {
    return addon.midiCcIndexForName(name);
  }

  /** Return the MIDI 2.0 per-note controller name for `index`, or `null`. */
  static perNoteControllerName(index: number): string | null {
    return addon.midiPerNoteControllerName(index);
  }

  /** Expand bank-select + program-change into MIDI events accepted by {@link setMidiEvents}. */
  static midiBankProgram(
    ppq: number,
    group: number,
    channel: number,
    bankMsb: number,
    bankLsb: number,
    program: number,
  ): ProjectMidiEvent[] {
    return addon.midiBankProgram(ppq, group, channel, bankMsb, bankLsb, program);
  }

  /** Route MIDI events through the native MidiRouter filter/remap/thru logic. */
  static midiRouteEvents(
    events: ReadonlyArray<ProjectMidiEvent>,
    config: ProjectMidiRouteConfig = {},
  ): ProjectMidiRouteResult {
    return addon.midiRouteEvents(events, config);
  }

  /** Run native MIDI learn over an event stream; returns `null` when nothing is learned. */
  static midiCcLearn(
    events: ReadonlyArray<ProjectMidiEvent>,
    paramId: number,
    options: MidiCcLearnOptions = {},
  ): ProjectMidiCcBinding | null {
    return addon.midiCcLearn(
      events,
      paramId,
      options.minValue ?? 0,
      options.maxValue ?? 1,
      options.minMovement ?? 0,
    );
  }

  /** Convert one CC event to an automation breakpoint using native CcMap. */
  static midiCcToBreakpoint(
    bindings: ReadonlyArray<ProjectMidiCcBinding>,
    event: ProjectMidiEvent,
  ): ProjectAutomationPoint | null {
    return addon.midiCcToBreakpoint(bindings, event);
  }

  /** Convert one automation value back to a CC UMP event using native CcMap. */
  static midiParamToCc(
    bindings: ReadonlyArray<ProjectMidiCcBinding>,
    paramId: number,
    unitValue: number,
    group: number,
    ppq = 0,
  ): ProjectMidiEvent | null {
    return addon.midiParamToCc(bindings, paramId, unitValue, group, ppq);
  }

  /** Pack a MIDI 1.0 channel-pressure event. */
  static midiChannelPressure(
    ppq: number,
    group: number,
    channel: number,
    pressure: number,
  ): ProjectMidiEvent {
    return midi1Event('Project.midiChannelPressure', ppq, group, 0xd, channel, pressure, 0);
  }

  /** Pack a MIDI 1.0 pitch-bend event (`bend` is unsigned 14-bit, center = 8192). */
  static midiPitchBend(
    ppq: number,
    group: number,
    channel: number,
    bend: number,
  ): ProjectMidiEvent {
    if (!Number.isInteger(bend) || bend < 0 || bend > 0x3fff) {
      throw new RangeError('Project.midiPitchBend: bend must be an integer in [0, 16383]');
    }
    return midi1Event('Project.midiPitchBend', ppq, group, 0xe, channel, bend & 0x7f, bend >> 7);
  }

  /**
   * Deserialize project JSON into a new project. Throws cleanly (with the joined
   * native diagnostic messages) on malformed input.
   */
  static fromJson(json: string): Project {
    return new Project(addon.Project.fromJson(json));
  }

  /**
   * Deserialize project JSON and return native warning diagnostics emitted on
   * successful loads, such as dangling source references preserved for repair.
   */
  static fromJsonWithDiagnostics(json: string): { project: Project; diagnostics: string } {
    const result = addon.Project.fromJsonWithDiagnostics(json);
    return {
      project: new Project(result.project),
      diagnostics: result.diagnostics,
    };
  }

  // -- serialization --

  /** Serialize the project to deterministic JSON. */
  toJson(): string {
    return this.native.toJson();
  }

  /** Set the project sample rate in Hz (must be > 0). */
  setSampleRate(sampleRate: number): void {
    this.native.setSampleRate(sampleRate);
  }

  /** Read the project sample rate in Hz. */
  getSampleRate(): number {
    return this.native.getSampleRate();
  }

  /** Set the project's clip-overlap policy (ordinal). */
  setOverlapPolicy(policy: number): void {
    this.native.setOverlapPolicy(policy);
  }

  /** Read the project's clip-overlap policy (ordinal). */
  getOverlapPolicy(): number {
    return this.native.getOverlapPolicy();
  }

  /** Replace the project's mixer scene from scene JSON (see {@link Mixer.fromSceneJson}). */
  setMixerSceneJson(sceneJson: string): void {
    this.native.setMixerSceneJson(sceneJson);
  }

  /** Add or replace a marker; `markerId` 0 allocates a new id. Returns the marker id. */
  setMarker(markerId: number, ppq: number, name: string): number {
    return this.native.setMarker(markerId, ppq, name);
  }

  /**
   * Add or replace a marker from a full descriptor, including its kind and key
   * signature. `marker.id` 0 (or omitted) allocates a new id. Returns the id.
   */
  setMarkerEx(marker: {
    id?: number;
    ppq: number;
    name?: string;
    kind?: number;
    keyFifths?: number;
    keyMinor?: boolean;
  }): number {
    return this.native.setMarkerEx(marker);
  }

  /** Read a project marker by index (0-based, in stored order). */
  markerByIndex(index: number): ProjectMarker {
    return this.native.markerByIndex(index);
  }

  trackByIndex(index: number): ProjectTrack {
    return this.native.trackByIndex(index);
  }
  clipByIndex(index: number): ProjectClip {
    return this.native.clipByIndex(index);
  }
  sourceByIndex(index: number): ProjectSource {
    return this.native.sourceByIndex(index);
  }

  /** Number of markers in the project value model. */
  markerCount(): number {
    return this.native.markerCount();
  }

  /** Replace the project's tempo segment list. */
  setTempoSegments(segments: ReadonlyArray<ProjectTempoSegment>): void {
    this.native.setTempoSegments(segments);
  }

  /** Replace the project's time-signature segment list. */
  setTimeSignatures(segments: ReadonlyArray<ProjectTimeSignatureSegment>): void {
    this.native.setTimeSignatures(segments);
  }

  /** Number of tracks in the project value model. */
  trackCount(): number {
    return this.native.trackCount();
  }

  /** Number of clips in the project value model. */
  clipCount(): number {
    return this.native.clipCount();
  }

  /** Number of sources in the project value model. */
  sourceCount(): number {
    return this.native.sourceCount();
  }

  /** Audio source ids that need decoded PCM after deserialization. */
  unresolvedAudioSourceIds(): number[] {
    return this.native.unresolvedAudioSourceIds();
  }

  /** Register decoded interleaved PCM for an existing audio source (undoable). */
  setSourceAudio(
    sourceId: number,
    audio: Float32Array,
    channels: number,
    sampleRate: number,
  ): void {
    this.native.setSourceAudio(sourceId, audio, channels, sampleRate);
  }

  /** Replace an audio source's metadata strings as one undoable edit. */
  setAudioSourceMetadata(
    sourceId: number,
    metadata: { contentHash: string; externalStemRole: string },
  ): void;
  setAudioSourceMetadata(sourceId: number, contentHash: string, externalStemRole: string): void;
  setAudioSourceMetadata(
    sourceId: number,
    contentHashOrMetadata: string | { contentHash: string; externalStemRole: string },
    externalStemRole?: string,
  ): void {
    if (typeof contentHashOrMetadata === 'string') {
      if (typeof externalStemRole !== 'string') {
        throw new TypeError(
          'setAudioSourceMetadata expects (sourceId, contentHash, externalStemRole)',
        );
      }
      this.native.setAudioSourceMetadata(sourceId, contentHashOrMetadata, externalStemRole);
      return;
    }
    this.native.setAudioSourceMetadata(
      sourceId,
      contentHashOrMetadata.contentHash,
      contentHashOrMetadata.externalStemRole,
    );
  }

  /** Number of tempo segments in the project value model. */
  tempoSegmentCount(): number {
    return this.native.tempoSegmentCount();
  }

  /** Number of time-signature segments in the project value model. */
  timeSignatureCount(): number {
    return this.native.timeSignatureCount();
  }

  /**
   * Reads a tempo segment by index, in stored order.
   *
   * @param index - Zero-based index below {@link tempoSegmentCount}
   * @returns The segment, in the shape {@link setTempoSegments} accepts
   * @throws A `SonareError` when the index is at or past the count
   */
  tempoSegmentByIndex(index: number): ProjectTempoSegment {
    return this.native.tempoSegmentByIndex(index);
  }

  /**
   * Reads a time-signature segment by index, in stored order.
   *
   * @param index - Zero-based index below {@link timeSignatureCount}
   * @returns The segment, in the shape {@link setTimeSignatures} accepts
   * @throws A `SonareError` when the index is at or past the count
   */
  timeSignatureByIndex(index: number): ProjectTimeSignatureSegment {
    return this.native.timeSignatureByIndex(index);
  }

  // -- edit --

  /** Add a track and return its allocated stable id. */
  addTrack(desc: ProjectTrackDesc = {}): number {
    return this.native.addTrack({ ...desc, kind: trackKindValue(desc.kind) });
  }

  /** Add an audio or MIDI clip and return its allocated clip id. */
  addClip(desc: ProjectClipDesc): number {
    return this.native.addClip(desc);
  }

  /**
   * Import host-separated PCM as ordinary audio tracks and clips. Input PCM is
   * copied; the native layer never resamples, retimes, phase-aligns, or adds
   * hidden gain compensation.
   */
  importExternalStems(
    request: ExternalSeparatedStemImportRequest,
  ): ExternalSeparatedStemImportResult {
    return this.native.importExternalStems({
      ...request,
      stems: request.stems.map((stem) => ({
        ...stem,
        layout: stem.layout === 'mono' ? 1 : stem.layout === 'stereo' ? 2 : stem.layout,
      })),
    });
  }

  /** Split captured loop-recording audio into takes and add one clip. */
  addLoopRecordingTakes(desc: ProjectLoopRecordingDesc): ProjectLoopRecordingResult {
    return this.native.addLoopRecordingTakes(desc);
  }

  /** Create a MIDI track + clip; returns `{ trackId, clipId }`. */
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

  /** Move a clip to `newStartPpq` (and optionally `newTrackId`; 0 = keep track). */
  moveClip(clipId: number, newStartPpq: number, newTrackId = 0): void {
    this.native.moveClip(clipId, newStartPpq, newTrackId);
  }

  /** Change a track kind via an undoable edit. */
  setTrackKind(trackId: number, kind: ProjectTrackDesc['kind']): void {
    this.native.setTrackKind(trackId, trackKindValue(kind));
  }

  /** Set a clip's warp reference id (0 clears it). */
  setClipWarpRef(clipId: number, warpRefId: number): void {
    this.native.setClipWarpRef(clipId, warpRefId);
  }

  /** Set a clip's warp playback mode. */
  setClipWarpMode(clipId: number, mode: WarpMode | number): void {
    this.native.setClipWarpMode(clipId, warpModeValue(mode));
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
   * Set a clip's fade-in / fade-out regions via an undoable edit. Each fade is
   * an optional `{ lengthPpq, curve? }` ({@link ProjectClipFade}); omitted
   * fields and omitted sides become a zero-length linear fade.
   */
  setClipFade(clipId: number, fadeIn?: ProjectClipFade, fadeOut?: ProjectClipFade): void {
    this.native.setClipFade(clipId, projectClipFadeValue(fadeIn), projectClipFadeValue(fadeOut));
  }

  /** Replace a clip's take list and active take id via an undoable edit. */
  setClipTakes(clipId: number, takes: ReadonlyArray<ProjectClipTake>, activeTakeId = 0): void {
    this.native.setClipTakes(clipId, takes, activeTakeId);
  }

  /** Replace a clip's comp segments via an undoable edit. */
  setClipCompSegments(clipId: number, segments: ReadonlyArray<ProjectClipCompSegment>): void {
    this.native.setClipCompSegments(clipId, segments);
  }

  /**
   * Set a clip's loop mode + loop length (PPQ) via an undoable edit.
   * `loopMode` is a {@link ProjectLoopMode} ordinal/name (0/off, 1/loop). When
   * looping, `loopLengthPpq` must be finite and >= 0, where `0` means "loop the
   * entire clip" (the effective length is resolved from the clip's own
   * duration). `loopCrossfadePpq` is an optional equal-power crossfade at the
   * loop seam (PPQ, finite and >= 0; 0 = hard loop); the engine clamps it to the
   * clip's pre-roll and half the loop.
   */
  setClipLoop(
    clipId: number,
    loopMode: ProjectLoopMode,
    loopLengthPpq = 0,
    loopCrossfadePpq = 0,
  ): void {
    this.native.setClipLoop(
      clipId,
      projectLoopModeValue(loopMode),
      loopLengthPpq,
      loopCrossfadePpq,
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

  /** Rename a track via an undoable edit (omit / null `name` = empty). */
  renameTrack(trackId: number, name?: string): void {
    this.native.renameTrack(trackId, name ?? null);
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
   * lane's stable target parameter id. Omit `desc.targetKind` for the legacy
   * opaque target; supply it to persist a typed fader or pan target.
   */
  addAutomationLane(trackId: number, desc: ProjectAutomationLaneDesc): number {
    return this.native.addAutomationLane(trackId, projectAutomationLaneValue(desc));
  }

  /**
   * Replace the lane identified by its stable target parameter id. Omitting
   * `desc.targetKind` preserves the existing lane kind through the legacy C
   * edit path; supplying it applies the typed target through the extended path.
   */
  editAutomationLane(
    trackId: number,
    targetParamId: number,
    desc: ProjectAutomationLaneDesc,
  ): void {
    this.native.editAutomationLane(trackId, targetParamId, projectAutomationLaneValue(desc));
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
   * `{ ppq, data0, data1? }` (or a `[ppq, data0, data1]` tuple); pass an empty
   * array to clear. `data0`/`data1` are the first two UMP-1.0 words of a
   * channel-voice message (stored opaquely).
   */
  setMidiEvents(
    clipId: number,
    events: ReadonlyArray<ProjectMidiEvent | readonly [number, number, number]>,
  ): void {
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

  /** Export the project's tempo map + MIDI clips to an SMF byte buffer. */
  exportSmf(): Buffer {
    return this.native.exportSmf();
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
   * Export the project's tempo map + MIDI clips to a MIDI 2.0 Clip File
   * (`SMF2CLIP`) byte buffer. MIDI 2.0-only events are written without loss —
   * prefer this over {@link exportSmf} when MIDI 2.0 fidelity matters.
   */
  exportClipFile(): Buffer {
    return this.native.exportClipFile();
  }

  /**
   * Set a MIDI clip's channel-0 program / bank at source PPQ 0. `bank` defaults
   * to `-1` (no Bank Select emitted), matching `setProgramOnChannel` and the
   * Python/WASM surfaces; pass `>= 0` to emit a Bank Select.
   */
  setProgram(clipId: number, program: number, bank = -1): void {
    this.native.setProgram(clipId, program, bank);
  }

  /** Set a MIDI clip's program / bank for one UMP group and channel. */
  setProgramOnChannel(
    clipId: number,
    group: number,
    channel: number,
    program: number,
    bank = -1,
  ): void {
    this.native.setProgramOnChannel(clipId, group, channel, program, bank);
  }

  /**
   * Destructively bake a MIDI-FX chain into all stored events. Large clips are
   * drained without truncation; failure leaves the original clip unchanged.
   */
  bakeMidiFx(clipId: number, configJson: string): void;
  /**
   * Request form. Setting `withSourceIndex` also returns per-event provenance,
   * so a selection or an editorial annotation can be carried across the bake.
   */
  bakeMidiFx(request: ProjectMidiFxBakeRequest): ProjectMidiFxBakeResult;
  bakeMidiFx(
    clipIdOrRequest: number | ProjectMidiFxBakeRequest,
    configJson?: string,
  ): ProjectMidiFxBakeResult {
    const request = normalizeMidiFxBakeRequest(clipIdOrRequest, configJson);
    if (!request.withSourceIndex) {
      this.native.bakeMidiFx(request.clipId, request.configJson);
      return {};
    }
    return {
      sourceIndex: this.native.bakeMidiFxWithSourceIndex(request.clipId, request.configJson),
    };
  }

  /**
   * Count the events {@link bakeMidiFx} would produce for this clip and
   * configuration, without mutating the project. The transform is
   * deterministic, so the count matches what the bake goes on to produce.
   */
  previewMidiFxCount(request: ProjectMidiFxPreviewRequest): number {
    return this.native.previewMidiFxCount(request.clipId, request.configJson);
  }

  /** Backward alias for {@link bakeMidiFx}. */
  setMidiFx(clipId: number, configJson: string): void {
    this.bakeMidiFx(clipId, configJson);
  }

  /**
   * Validate that every note-on in the clip's exported half-open playback
   * window has a matching note-off.
   *
   * @param clipId Target MIDI clip id.
   * @returns `ok` is `true` when fully paired; `unmatchedNoteOns` /
   *          `unmatchedNoteOffs` count the dangling events of each kind.
   * @throws If `clipId` is not a MIDI clip.
   */
  validateMidiNotes(clipId: number): {
    ok: boolean;
    unmatchedNoteOns: number;
    unmatchedNoteOffs: number;
  } {
    return this.native.validateMidiNotes(clipId);
  }

  // -- MIR --

  /** Return ranked tempo-octave and detected-meter candidates without editing the project. */
  analyzeTempo(
    audio: Float32Array,
    sampleRate: number,
    options?: ProjectTempoOptions,
  ): ProjectTempoCandidate[] {
    return this.native.analyzeTempo(audio, sampleRate, options);
  }

  /**
   * Detect and install a ranked tempo candidate; optionally apply its detected meter.
   *
   * @remarks
   * `candidateIndex` indexes the ranking {@link analyzeTempo} produced, so pair
   * the two on the same `options`. Read the installed map back with
   * {@link tempoSegmentCount} and {@link tempoSegmentByIndex}.
   */
  autoTempo(
    audio: Float32Array,
    sampleRate: number,
    candidateIndex = 0,
    applyTimeSignatures = false,
    options?: ProjectTempoOptions,
  ): number {
    return this.native.autoTempo(audio, sampleRate, candidateIndex, applyTimeSignatures, options);
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
    this.native.annotateKeys(keys);
  }

  /** Replace the project's chord-symbol annotation stream via an undoable edit. */
  annotateChords(chords: ProjectChordSymbol[]): void {
    this.native.annotateChords(chords);
  }

  // -- assist sidecars --

  /**
   * Add or update an opaque assist sidecar (keyed by module id + target scope)
   * via an undoable edit. The payload bytes are copied.
   */
  setAssistSidecar(sidecar: ProjectAssistSidecarInput): void {
    this.native.setAssistSidecar(sidecar);
  }

  /** Number of assist sidecars currently stored on the project. */
  assistSidecarCount(): number {
    return this.native.assistSidecarCount();
  }

  /** Read one assist sidecar by stable project order. */
  getAssistSidecar(index: number): ProjectAssistSidecar {
    return this.native.getAssistSidecar(index);
  }

  /** Read every stored assist sidecar as an array (stable project order). */
  assistSidecars(): ProjectAssistSidecar[] {
    return this.native.assistSidecars();
  }

  // -- compile / render --

  /** Compile the project into an RT-readable timeline, surfacing diagnostics. */
  compile(): ProjectCompileResult {
    return this.native.compile();
  }

  /**
   * Retrieve the compile result captured by the most recent {@link bounce}.
   *
   * On a project no bounce has ever run on, the result is empty in full:
   * `hasTimeline` is `false` and `diagnostics` is empty. A failed bounce is
   * distinguishable from that state, because a bounce only loses its timeline
   * through an error diagnostic and so always reports at least one.
   */
  lastBounceCompileResult(): ProjectCompileResult {
    return this.native.lastBounceCompileResult();
  }

  /**
   * Compile + render the project offline to an interleaved float buffer
   * (`totalFrames * channels` samples). Deterministic: the same project +
   * options yields a bit-identical array within one build.
   *
   * Omitting `options.totalFrames` (or passing `<= 0`) auto-derives the render
   * length from the arrangement rather than producing an empty render.
   *
   * MIDI tracks routed to a destination render as silence here, because no
   * instrument is bound. To audition MIDI through the built-in synth, use
   * {@link bounceWithBuiltinInstrument} / {@link bounceWithBuiltinInstruments}.
   */
  bounce(options: ProjectBounceOptions = {}): Float32Array {
    return this.native.bounce(options);
  }

  /**
   * Like {@link bounce}, but renders MIDI tracks routed to a destination
   * through the built-in oscillator synth so a MIDI-only arrangement bounces
   * to audible audio. Each entry of `instruments` binds a
   * {@link BuiltinInstrumentConfig} patch to a `destinationId` (default `0`).
   * An empty array renders silence, identical to {@link bounce}.
   *
   * Argument order is instrument-first to match the WASM and Python bindings.
   */
  bounceWithBuiltinInstruments(
    instruments: BuiltinInstrumentConfig[] = [],
    options: ProjectBounceOptions = {},
  ): Float32Array {
    return this.native.bounceWithBuiltinInstruments(instruments, options);
  }

  /**
   * Convenience wrapper over {@link bounceWithBuiltinInstruments} for the
   * common single-instrument case. Pass a {@link BuiltinInstrumentConfig}
   * (e.g. `{ waveform: 'saw', destinationId: 0 }`) or a bare
   * {@link SynthWaveform} name to bind one built-in synth patch. The
   * `destinationId` field is a JS binding convenience, not part of the
   * oscillator patch itself.
   *
   * Argument order is instrument-first to match the WASM and Python bindings.
   */
  bounceWithBuiltinInstrument(
    instrument: BuiltinInstrumentConfig | SynthWaveform = {},
    options: ProjectBounceOptions = {},
  ): Float32Array {
    const config: BuiltinInstrumentConfig =
      typeof instrument === 'string' ? { waveform: instrument } : instrument;
    return this.native.bounceWithBuiltinInstruments([config], options);
  }

  /**
   * Like {@link bounce}, but renders MIDI tracks routed to a destination
   * through the patch-driven NativeSynth — the full synthesizer (subtractive /
   * FM / Karplus-Strong / modal / additive / percussion /
   * extended-waveguide-piano engines plus the realism layer). Each entry of
   * `instruments` binds a {@link SynthPatch} (or a preset-name string such as
   * `'saw-lead'` / `'va:saw-lead'`; see {@link synthPresetNames}) to a
   * `destinationId` (default `0`). An object descriptor may also set
   * `useGmPrograms: true` to follow incoming GM bank/program changes (and use
   * the GM drum map on channel 10); it defaults to `false`, preserving the
   * fixed-patch behavior. `destinationId` and `useGmPrograms` are JS binding
   * conveniences, not part of the NativeSynth patch itself. An empty array
   * renders silence. Unknown preset names throw. Deterministic for a fixed
   * project + options + patch.
   *
   * Argument order is instrument-first to match the WASM and Python bindings.
   */
  bounceWithSynthInstruments(
    instruments: (SynthPatch | string)[] = [],
    options: ProjectBounceOptions = {},
  ): Float32Array {
    return this.native.bounceWithSynthInstruments(instruments, options);
  }

  /**
   * Convenience wrapper over {@link bounceWithSynthInstruments} for the common
   * single-instrument case. Pass a {@link SynthPatch} or a bare preset name
   * (`'saw-lead'` / `'va:saw-lead'`). Set `useGmPrograms: true` on an object
   * descriptor to follow incoming GM bank/program changes.
   */
  bounceWithSynthInstrument(
    instrument: SynthPatch | string = {},
    options: ProjectBounceOptions = {},
  ): Float32Array {
    return this.native.bounceWithSynthInstruments([instrument], options);
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
   * per-program backend). An empty array renders silence.
   *
   * Argument order is instrument-first to match the WASM and Python bindings.
   */
  bounceWithSf2Instruments(
    instruments: Sf2InstrumentConfig[] = [],
    options: ProjectBounceOptions = {},
  ): Float32Array {
    return this.native.bounceWithSf2Instruments(instruments, options);
  }

  /**
   * Convenience wrapper over {@link bounceWithSf2Instruments} for the common
   * single-instrument case.
   */
  bounceWithSf2Instrument(
    instrument: Sf2InstrumentConfig = {},
    options: ProjectBounceOptions = {},
  ): Float32Array {
    return this.native.bounceWithSf2Instruments([instrument], options);
  }

  /** Release the underlying native project. Idempotent. */
  destroy(): void {
    if (this.disposed) {
      return;
    }
    this.disposed = true;
    this.native.destroy();
  }

  /** Alias for {@link destroy}, provided for cross-binding (WASM) compatibility. */
  delete(): void {
    this.destroy();
  }

  /** Releases the native project; lets `using` (Node 22+) free it automatically. */
  [Symbol.dispose](): void {
    this.destroy();
  }
}
