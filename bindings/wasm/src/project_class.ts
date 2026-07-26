import {
  assertProjectMidiEvents,
  projectLoopModeValue,
  projectMidi1Event,
  projectModule,
  projectTrackKindValue,
  projectWarpModeValue,
  type WasmProject,
} from './project_internal';
import type {
  BuiltinSynthBinding,
  MidiCcLearnOptions,
  ProjectAssistSidecar,
  ProjectAutomationLaneDesc,
  ProjectAutomationPoint,
  ProjectBounceOptions,
  ProjectChordSymbol,
  ProjectClipCompSegment,
  ProjectClipDesc,
  ProjectClipFade,
  ProjectClipTake,
  ProjectCompileResult,
  ProjectDeserializeResult,
  ProjectKeySegment,
  ProjectLoopMode,
  ProjectLoopRecordingDesc,
  ProjectLoopRecordingResult,
  ProjectMarker,
  ProjectMidiCcBinding,
  ProjectMidiClipResult,
  ProjectMidiEvent,
  ProjectMidiRouteConfig,
  ProjectMidiRouteResult,
  ProjectNotePairValidation,
  ProjectTempoCandidate,
  ProjectTempoSegment,
  ProjectTimeSignatureSegment,
  ProjectTrackDesc,
  ProjectTrackKind,
  ProjectWarpMapDesc,
  ProjectWarpMode,
  Sf2InstrumentConfig,
  Sf2ProgramStatus,
  SynthPatch,
} from './project_types';

/**
 * Headless DAW project (control-thread-only arrangement model).
 *
 * Wraps the embind `Project` class over the C-ABI keystone
 * `sonare_c_project.{h,cpp}`. Construct an empty project with `new Project()`,
 * or deserialize one with {@link Project.fromJson}; serialize back with
 * {@link toJson}; compile to a renderable timeline with {@link compile}; render
 * offline to interleaved float audio with {@link bounce}. The edit and MIDI
 * methods mirror the Node/Python project bindings.
 *
 * Call {@link delete} (or use a `try/finally`) to release the underlying WASM
 * object — the embind handle is not garbage-collected automatically.
 *
 * @example
 * ```typescript
 * const project = new Project();
 * try {
 *   project.setSampleRate(48000);
 *   const json = project.toJson();
 *   const restored = Project.fromJson(json);
 *   restored.delete();
 * } finally {
 *   project.delete();
 * }
 * ```
 */
export class Project {
  private native: WasmProject;

  constructor() {
    this.native = new (projectModule().Project)();
  }

  /** Pack a MIDI 1.0 note-on event accepted by {@link setMidiEvents}. */
  static midiNoteOn(
    ppq: number,
    group: number,
    channel: number,
    note: number,
    velocity: number,
  ): ProjectMidiEvent {
    return projectMidi1Event('Project.midiNoteOn', ppq, group, 0x9, channel, note, velocity);
  }

  /** Pack a MIDI 1.0 note-off event accepted by {@link setMidiEvents}. */
  static midiNoteOff(
    ppq: number,
    group: number,
    channel: number,
    note: number,
    velocity = 0,
  ): ProjectMidiEvent {
    return projectMidi1Event('Project.midiNoteOff', ppq, group, 0x8, channel, note, velocity);
  }

  /** Pack a MIDI 1.0 control-change event. */
  static midiCc(
    ppq: number,
    group: number,
    channel: number,
    controller: number,
    value: number,
  ): ProjectMidiEvent {
    return projectMidi1Event('Project.midiCc', ppq, group, 0xb, channel, controller, value);
  }

  /** Pack a MIDI 1.0 poly-pressure event. */
  static midiPolyPressure(
    ppq: number,
    group: number,
    channel: number,
    note: number,
    pressure: number,
  ): ProjectMidiEvent {
    return projectMidi1Event('Project.midiPolyPressure', ppq, group, 0xa, channel, note, pressure);
  }

  /** Pack a MIDI 1.0 program-change event. */
  static midiProgram(
    ppq: number,
    group: number,
    channel: number,
    program: number,
  ): ProjectMidiEvent {
    return projectMidi1Event('Project.midiProgram', ppq, group, 0xc, channel, program, 0);
  }

  /** Return the General MIDI instrument name for `program`, or `null` when out of range. */
  static gmInstrumentName(program: number): string | null {
    return projectModule().midiGmInstrumentName(program);
  }

  /** Return the General MIDI program number for a canonical instrument name, or `-1`. */
  static gmProgramForName(name: string): number {
    return projectModule().midiGmProgramForName(name);
  }

  /** Return the General MIDI family name for `family`, or `null` when out of range. */
  static gmFamilyName(family: number): string | null {
    return projectModule().midiGmFamilyName(family);
  }

  /** Return the first General MIDI program number in `family`, or `-1`. */
  static gmFamilyFirstProgram(family: number): number {
    return projectModule().midiGmFamilyFirstProgram(family);
  }

  /** Return the GM2 bank/program instrument variation name, or `null` when unavailable. */
  static gm2InstrumentName(bankLsb: number, program: number): string | null {
    return projectModule().midiGm2InstrumentName(bankLsb, program);
  }

  /** Return the General MIDI drum name for `note`, or `null` when out of range. */
  static gmDrumName(note: number): string | null {
    return projectModule().midiGmDrumName(note);
  }

  /** Return the General MIDI drum note for a canonical drum name, or `-1`. */
  static gmDrumNoteForName(name: string): number {
    return projectModule().midiGmDrumNoteForName(name);
  }

  /** Return the GM2 drum-set name for `bankLsb`, or `null` when unavailable. */
  static gm2DrumSetName(bankLsb: number): string | null {
    return projectModule().midiGm2DrumSetName(bankLsb);
  }

  /** Return the GM2 drum name for `bankLsb`/`note`, or `null` when unavailable. */
  static gm2DrumName(bankLsb: number, note: number): string | null {
    return projectModule().midiGm2DrumName(bankLsb, note);
  }

  /** Return the MIDI CC name for `controller`, or `null` when out of range. */
  static midiCcName(controller: number): string | null {
    return projectModule().midiCcName(controller);
  }

  /** Return the MIDI CC number for a canonical controller name, or `-1`. */
  static midiCcIndexForName(name: string): number {
    return projectModule().midiCcIndexForName(name);
  }

  /** Return the MIDI 2.0 per-note controller name for `index`, or `null`. */
  static perNoteControllerName(index: number): string | null {
    return projectModule().midiPerNoteControllerName(index);
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
    return projectModule().midiBankProgram(ppq, group, channel, bankMsb, bankLsb, program);
  }

  /** Route MIDI events through the native MidiRouter filter/remap/thru logic. */
  static midiRouteEvents(
    events: ReadonlyArray<ProjectMidiEvent>,
    config: ProjectMidiRouteConfig = {},
  ): ProjectMidiRouteResult {
    return projectModule().midiRouteEvents(events, config);
  }

  /** Run native MIDI learn over an event stream; returns `null` when nothing is learned. */
  static midiCcLearn(
    events: ReadonlyArray<ProjectMidiEvent>,
    paramId: number,
    options: MidiCcLearnOptions = {},
  ): ProjectMidiCcBinding | null {
    return projectModule().midiCcLearn(
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
    return projectModule().midiCcToBreakpoint(bindings, event);
  }

  /** Convert one automation value back to a CC UMP event using native CcMap. */
  static midiParamToCc(
    bindings: ReadonlyArray<ProjectMidiCcBinding>,
    paramId: number,
    unitValue: number,
    group: number,
    ppq = 0,
  ): ProjectMidiEvent | null {
    return projectModule().midiParamToCc(bindings, paramId, unitValue, group, ppq);
  }

  /** Pack a MIDI 1.0 channel-pressure event. */
  static midiChannelPressure(
    ppq: number,
    group: number,
    channel: number,
    pressure: number,
  ): ProjectMidiEvent {
    return projectMidi1Event('Project.midiChannelPressure', ppq, group, 0xd, channel, pressure, 0);
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
    return projectMidi1Event(
      'Project.midiPitchBend',
      ppq,
      group,
      0xe,
      channel,
      bend & 0x7f,
      bend >> 7,
    );
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

  /** Set the project sample rate in Hz. Must be > 0. */
  setSampleRate(sampleRate: number): void {
    this.native.setSampleRate(sampleRate);
  }

  /** Add a track and return its allocated stable id. */
  addTrack(desc: ProjectTrackDesc = {}): number {
    return this.native.addTrack({ ...desc, kind: projectTrackKindValue(desc.kind) });
  }

  /** Add an audio or MIDI clip and return its allocated clip id. */
  addClip(desc: ProjectClipDesc): number {
    return this.native.addClip(desc);
  }

  /** Split captured loop-recording audio into takes and add one clip. */
  addLoopRecordingTakes(desc: ProjectLoopRecordingDesc): ProjectLoopRecordingResult {
    return this.native.addLoopRecordingTakes(desc);
  }

  /** Create a MIDI track + clip; returns `{ trackId, clipId }`. */
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

  /** Move a clip to `newStartPpq` and optionally another track. */
  moveClip(clipId: number, newStartPpq: number, newTrackId = 0): void {
    this.native.moveClip(clipId, newStartPpq, newTrackId);
  }

  /** Change a track kind via an undoable edit. */
  setTrackKind(trackId: number, kind: ProjectTrackKind): void {
    this.native.setTrackKind(trackId, projectTrackKindValue(kind));
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
   * Routes through an undoable edit command.
   */
  setTrackMidiDestination(trackId: number, destinationId: number): void {
    this.native.setTrackMidiDestination(trackId, destinationId);
  }

  /** Set a track's linear playback gain (1.0 = unity; >= 0) via an undoable edit. */
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

  /** Set a track's stereo balance in [-1, +1] (0 = center) via an undoable edit. */
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

  /** Replace a MIDI clip's entire event list. */
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
  importSmf(data: Uint8Array): number {
    return this.native.importSmf(data);
  }

  /** Export the project's tempo map + MIDI clips to an SMF byte buffer. */
  exportSmf(): Uint8Array {
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
   */
  exportClipFile(): Uint8Array {
    return this.native.exportClipFile();
  }

  /**
   * Set a MIDI clip's channel-0 program / bank at source PPQ 0. `bank` defaults
   * to `-1` (no Bank Select emitted), matching `setProgramOnChannel` and the
   * Node/Python surfaces; pass `>= 0` to emit a Bank Select.
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
  bakeMidiFx(clipId: number, configJson: string): void {
    this.native.bakeMidiFx(clipId, configJson);
  }

  /** Backward alias for {@link bakeMidiFx}. */
  setMidiFx(clipId: number, configJson: string): void {
    this.bakeMidiFx(clipId, configJson);
  }

  /**
   * Pre-flight check for hanging / unmatched notes in a MIDI clip: reports
   * whether every note-on in the exported half-open playback window has a
   * matching note-off (FIFO per group+channel+note). Useful before bouncing to
   * catch a stuck note. Throws if `clipId` is unknown or not a MIDI clip.
   */
  validateMidiNotes(clipId: number): ProjectNotePairValidation {
    return this.native.validateMidiNotes(clipId);
  }

  /** Return ranked tempo-octave and detected-meter candidates without editing. */
  analyzeTempo(audio: Float32Array, sampleRate: number): ProjectTempoCandidate[] {
    return this.native.analyzeTempo(audio, sampleRate);
  }

  /** Detect and install a ranked tempo candidate; optionally apply detected meter. */
  autoTempo(
    audio: Float32Array,
    sampleRate: number,
    candidateIndex = 0,
    applyTimeSignatures = false,
  ): number {
    return this.native.autoTempo(audio, sampleRate, candidateIndex, applyTimeSignatures);
  }

  /** Snap to a bar (`division=0`), beat (`1`), or beat subdivision (`2+`). */
  snapToGrid(ppq: number, strength = 1.0, division = 1): number {
    return this.native.snapToGrid(ppq, strength, division);
  }

  /** Compile the project into a renderable timeline, surfacing diagnostics. */
  compile(): ProjectCompileResult {
    return this.native.compile();
  }

  /**
   * Compile + render the project offline to interleaved float audio. MIDI
   * tracks render silently here (no instrument is bound) — use
   * {@link bounceWithBuiltinInstrument} to make MIDI audible.
   *
   * When `totalFrames` is omitted (or `<= 0`) the render length is auto-derived
   * from the arrangement, so a project with content renders without computing a
   * frame count; an empty project yields an empty buffer.
   *
   * @example
   * ```typescript
   * const audio = project.bounce({ numChannels: 2 });
   * ```
   */
  bounce(options: ProjectBounceOptions = {}): Float32Array {
    return this.native.bounce(options);
  }

  /**
   * Compile + render the project offline, routing MIDI tracks through the
   * built-in oscillator synth so a MIDI-only arrangement bounces to audible
   * audio. Pass a {@link BuiltinSynthBinding} (or an array of them) to choose
   * the patch and MIDI destination; omit it (or pass `{}`) for one
   * default-destination sine patch. Because the parameter defaults to `{}`,
   * omission and explicit `undefined` both create that one default binding.
   * Use an explicitly empty array `[]` (or runtime `null`) for zero bindings,
   * so MIDI tracks render silently.
   *
   * Like {@link bounce}, omitting `totalFrames` auto-derives the render length
   * from the arrangement plus the synth's release tail.
   *
   * @example
   * ```typescript
   * // MIDI-only project -> non-silent stereo audio.
   * const audio = project.bounceWithBuiltinInstrument(
   *   { waveform: 'saw' },
   *   { numChannels: 2 },
   * );
   * ```
   */
  bounceWithBuiltinInstrument(
    instrument: BuiltinSynthBinding | ReadonlyArray<BuiltinSynthBinding> = {},
    options: ProjectBounceOptions = {},
  ): Float32Array {
    return this.native.bounceWithBuiltinInstrument(instrument, options);
  }

  /**
   * Compile + render the project offline, routing MIDI tracks through the
   * patch-driven NativeSynth — the full synthesizer (subtractive / FM /
   * Karplus-Strong / modal / additive / percussion / extended-waveguide-piano
   * engines plus the realism layer). Pass a {@link SynthPatch}, a preset-name
   * string (`'saw-lead'` / `'va:saw-lead'`; see {@link synthPresetNames}), or
   * an array of either; each object entry may carry a `destinationId` binding
   * convenience (default 0), which is not part of the NativeSynth patch itself.
   * Because the parameter defaults to `{}`, omission and explicit `undefined`
   * both create one default binding. Use an explicitly empty array `[]` (or
   * runtime `null`) for zero bindings. Unknown preset names throw.
   * Deterministic for a fixed project + options + patch.
   */
  bounceWithSynthInstrument(
    instrument: SynthPatch | string | ReadonlyArray<SynthPatch | string> = {},
    options: ProjectBounceOptions = {},
  ): Float32Array {
    return this.native.bounceWithSynthInstrument(instrument, options);
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
   * Like {@link bounceWithBuiltinInstrument}, but each bound destination
   * renders through a GS-compatible SoundFont player fed by the project's
   * loaded SoundFont ({@link loadSoundFont}): 16 MIDI channels per player,
   * channel 10 drums via bank 128, GS NRPN part edits and GS/GM SysEx resets
   * honored. Programs the SoundFont does not cover — including bouncing with
   * no SoundFont loaded at all — play through the built-in synthesizer GM
   * fallback bank (the data-free floor; see {@link soundFontManifest} for the
   * per-program backend). Because the parameter defaults to `{}`, omission and
   * explicit `undefined` both create one default binding. Use an explicitly
   * empty array `[]` (or runtime `null`) for zero bindings, so MIDI tracks
   * render silently.
   */
  bounceWithSf2Instrument(
    instrument: Sf2InstrumentConfig | ReadonlyArray<Sf2InstrumentConfig> = {},
    options: ProjectBounceOptions = {},
  ): Float32Array {
    return this.native.bounceWithSf2Instrument(instrument, options);
  }

  /** Remove a clip (undoable). */
  removeClip(clipId: number): void {
    this.native.removeClip(clipId);
  }

  /** Set a clip's linear playback gain (>= 0; undoable). */
  setClipGain(clipId: number, gain: number): void {
    this.native.setClipGain(clipId, gain);
  }

  /** Set a clip's fade-in / fade-out regions (undoable). */
  setClipFade(clipId: number, fadeIn: ProjectClipFade = {}, fadeOut: ProjectClipFade = {}): void {
    this.native.setClipFade(clipId, fadeIn, fadeOut);
  }

  /** Replace a clip's take list and active take id (undoable). */
  setClipTakes(clipId: number, takes: ReadonlyArray<ProjectClipTake>, activeTakeId = 0): void {
    this.native.setClipTakes(clipId, takes, activeTakeId);
  }

  /** Replace a clip's comp segments (undoable). */
  setClipCompSegments(clipId: number, segments: ReadonlyArray<ProjectClipCompSegment>): void {
    this.native.setClipCompSegments(clipId, segments);
  }

  /**
   * Set a clip's loop mode + loop length in PPQ (undoable). `loopCrossfadePpq`
   * is an optional equal-power crossfade at the loop seam (PPQ, finite and >= 0;
   * 0 = hard loop); the engine clamps it to the clip's pre-roll and half the loop.
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

  /** Append an automation lane to a track; returns the lane index (undoable). */
  addAutomationLane(trackId: number, desc: ProjectAutomationLaneDesc): number {
    return this.native.addAutomationLane(trackId, {
      targetParamId: desc.targetParamId,
      points: desc.points,
    });
  }

  /** Replace an existing automation lane in place (undoable). */
  editAutomationLane(trackId: number, laneIndex: number, desc: ProjectAutomationLaneDesc): void {
    this.native.editAutomationLane(trackId, laneIndex, {
      targetParamId: desc.targetParamId,
      points: desc.points,
    });
  }

  /** Remove an automation lane from a track (undoable). */
  removeAutomationLane(trackId: number, laneIndex: number): void {
    this.native.removeAutomationLane(trackId, laneIndex);
  }

  /** Replace the project's key annotation stream (undoable). */
  annotateKeys(keys: ReadonlyArray<ProjectKeySegment>): void {
    this.native.annotateKeys(keys);
  }

  /** Replace the project's chord-symbol annotation stream (undoable). */
  annotateChords(chords: ReadonlyArray<ProjectChordSymbol>): void {
    this.native.annotateChords(chords);
  }

  /** Add or update an opaque assist sidecar by module id + target scope (undoable). */
  setAssistSidecar(
    moduleId: string,
    schemaVersion: number,
    targetTrackId: number,
    regionStartPpq: number,
    regionEndPpq: number,
    payload: Uint8Array,
  ): void {
    this.native.setAssistSidecar(
      moduleId,
      schemaVersion,
      targetTrackId,
      regionStartPpq,
      regionEndPpq,
      payload,
    );
  }

  /** Number of assist sidecars currently stored on the project. */
  assistSidecarCount(): number {
    return this.native.assistSidecarCount();
  }

  /** Read one assist sidecar by stable project order. */
  getAssistSidecar(index: number): ProjectAssistSidecar {
    return this.native.getAssistSidecar(index);
  }

  /** Set the project's clip-overlap policy (SonareProjectOverlapPolicy ordinal). */
  setOverlapPolicy(policy: number): void {
    this.native.setOverlapPolicy(policy);
  }

  /** Read the project's clip-overlap policy (SonareProjectOverlapPolicy ordinal). */
  getOverlapPolicy(): number {
    return this.native.getOverlapPolicy();
  }

  /** Read the project sample rate in Hz. */
  getSampleRate(): number {
    return this.native.getSampleRate();
  }

  /** Replace the project's mixer scene from a scene JSON string. */
  setMixerSceneJson(sceneJson: string): void {
    this.native.setMixerSceneJson(sceneJson);
  }

  /**
   * Add or replace a marker. Pass `markerId` 0 to allocate a new id; returns the
   * stable marker id (the allocated id when 0 was passed).
   */
  setMarker(markerId: number, ppq: number, name: string): number {
    return this.native.setMarker(markerId, ppq, name);
  }

  /**
   * Add or replace a marker from a full {@link ProjectMarker}, including its
   * {@link MarkerKind} and (for key signatures) the key. Pass `id` 0 to allocate
   * a new id; returns the stable marker id.
   */
  setMarkerEx(marker: ProjectMarker): number {
    return this.native.setMarkerEx(marker);
  }

  /** Read a project marker by index (0-based, in stored order). */
  markerByIndex(index: number): ProjectMarker {
    return this.native.markerByIndex(index);
  }

  /** Number of markers in the project. */
  markerCount(): number {
    return this.native.markerCount();
  }

  /** Number of tracks in the project. */
  trackCount(): number {
    return this.native.trackCount();
  }

  /** Number of clips in the project. */
  clipCount(): number {
    return this.native.clipCount();
  }

  /** Number of audio sources registered on the project. */
  sourceCount(): number {
    return this.native.sourceCount();
  }

  /** Number of tempo-map segments on the project. */
  tempoSegmentCount(): number {
    return this.native.tempoSegmentCount();
  }

  /** Number of time-signature segments on the project. */
  timeSignatureCount(): number {
    return this.native.timeSignatureCount();
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
   * (e.g. MIDI clips rendering silently without a bound instrument). When no
   * bounce has run, the result is empty with `hasTimeline` set.
   */
  lastBounceCompileResult(): ProjectCompileResult {
    return this.native.lastBounceCompileResult();
  }

  /** Release the underlying WASM object. Safe to call only once. */
  delete(): void {
    this.native.delete();
  }

  /** Alias for {@link delete}, provided for cross-binding (Node) compatibility. */
  destroy(): void {
    this.delete();
  }
}
