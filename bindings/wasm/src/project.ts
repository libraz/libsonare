import { getSonareModule } from './module_state';

// ============================================================================
// Headless DAW Project
// ============================================================================

/**
 * Expected project ABI version. Mirrors `SONARE_PROJECT_ABI_VERSION` in
 * `src/sonare_c_project.h`; checked against {@link projectAbiVersion} to detect
 * a WASM build whose flat project POD layout has drifted from this wrapper.
 */
export const EXPECTED_PROJECT_ABI_VERSION = 1;

/** Render options for {@link Project.bounce}. All fields are optional. */
export interface ProjectBounceOptions {
  /** Render length in frames at the output sample rate. */
  totalFrames?: number;
  /** Render block size; <= 0 uses the engine default (128). */
  blockSize?: number;
  /** Output channel count; <= 0 uses the default (2). */
  numChannels?: number;
  /** Output sample rate; <= 0 uses the project sample rate. */
  sampleRate?: number;
  /** Host-instrument PDC (latency) fed to the compiler. */
  instrumentLatencySamples?: number;
}

/** Oscillator waveform for the built-in synth. */
export type BuiltinSynthWaveform =
  | 'sine'
  | 'saw'
  | 'sawtooth'
  | 'square'
  | 'triangle'
  | 0
  | 1
  | 2
  | 3;

/**
 * Built-in synth patch + MIDI routing for
 * {@link Project.bounceWithBuiltinInstrument}. Every field is optional; a
 * non-positive (or omitted) numeric field falls back to the C-ABI default
 * (gain 0.2, attack 5ms, decay 60ms, sustain 0.7, release 120ms, 16 voices),
 * so `{}` is a usable default sine patch.
 */
export interface BuiltinSynthBinding {
  /** MIDI destination id this patch answers to (default 0; see {@link Project.setTrackMidiDestination}). */
  destinationId?: number;
  /** Oscillator waveform (default `'sine'`). */
  waveform?: BuiltinSynthWaveform;
  /** Master output gain, linear (0 => 0.2). */
  gain?: number;
  /** ADSR attack in ms (0 => 5). */
  attackMs?: number;
  /** ADSR decay in ms (0 => 60). */
  decayMs?: number;
  /** ADSR sustain level [0,1] (0 => 0.7). */
  sustain?: number;
  /** ADSR release in ms (0 => 120). */
  releaseMs?: number;
  /** Max simultaneous voices (0 => 16, clamped to [1, 64]). */
  polyphony?: number;
}

/**
 * Cross-binding alias of {@link BuiltinSynthBinding}. The same built-in-synth
 * patch concept is named `BuiltinSynthConfig` in the Python binding; this alias
 * lets portable code use that shared name on the WASM surface too.
 */
export type BuiltinSynthConfig = BuiltinSynthBinding;

/**
 * SoundFont (SF2) player patch + MIDI routing for
 * {@link Project.bounceWithSf2Instrument}. Every field is optional; a
 * non-positive (or omitted) numeric field falls back to the C-ABI default
 * (gain 0.5, 48 voices), so `{}` is a usable default patch.
 */
export interface Sf2InstrumentConfig {
  /** MIDI destination id this player answers to (default 0; see {@link Project.setTrackMidiDestination}). */
  destinationId?: number;
  /** Master output gain, linear (0 => 0.5). */
  gain?: number;
  /** Max simultaneous voices (0 => 48, clamped to [1, 64]). */
  polyphony?: number;
}

/** Source backend a resolved MIDI program renders through. */
export type SourceBackend = 'sf2' | 'synth';

/**
 * One {@link Project.soundFontManifest} entry: a (channel, bank, program)
 * combination the arrangement plays, with the backend it resolves to.
 */
export interface Sf2ProgramStatus {
  /** MIDI channel (0-15). */
  channel: number;
  /** Effective SF2 bank (drum channels report 128). */
  bank: number;
  /** Program number (0-127). */
  program: number;
  /** `'sf2'` when the loaded SoundFont covers the program, else `'synth'`. */
  backend: SourceBackend;
  /** Resolved SF2 preset name (GS fallback included); empty for `'synth'`. */
  presetName: string;
}

export const SYNTH_ENGINE_MODES = [
  'default',
  'subtractive',
  'fm',
  'karplus-strong',
  'modal',
  'additive',
  'percussion',
  'piano',
] as const;
export const SYNTH_OSC_WAVEFORMS = [
  'default',
  'sine',
  'saw',
  'square',
  'triangle',
  'noise',
] as const;
export const SYNTH_FILTER_MODELS = [
  'default',
  'svf',
  'moog-ladder',
  'diode-ladder',
  'sallen-key',
] as const;
export const SYNTH_FILTER_OUTPUTS = ['default', 'lowpass', 'bandpass', 'highpass'] as const;
export const SYNTH_BODY_TYPES = ['default', 'none', 'guitar', 'violin', 'wood-tube'] as const;
export const SYNTH_MOD_SOURCES = [
  'none',
  'amp-env',
  'filter-env',
  'lfo1',
  'lfo2',
  'velocity',
  'key-track',
  'mod-wheel',
  'random',
] as const;
export const SYNTH_MOD_DESTINATIONS = [
  'none',
  'pitch-cents',
  'cutoff-cents',
  'amp-gain',
  'pan-units',
] as const;

export interface SynthEnumTables {
  engineModes: string[];
  waveforms: string[];
  filterModels: string[];
  filterOutputs: string[];
  bodyTypes: string[];
  modSources: string[];
  modDestinations: string[];
}

/** NativeSynth engine selector ({@link SynthPatch}; `'default'` keeps the base patch's). */
export type SynthEngineMode = (typeof SYNTH_ENGINE_MODES)[number];

/** NativeSynth oscillator waveform (`'default'` keeps the base patch's). */
export type SynthOscWaveform = (typeof SYNTH_OSC_WAVEFORMS)[number];

/** NativeSynth filter model — the character core (`'default'` keeps the base patch's). */
export type SynthFilterModel = (typeof SYNTH_FILTER_MODELS)[number];

/** NativeSynth filter output (SVF only; `'default'` keeps the base patch's). */
export type SynthFilterOutput = (typeof SYNTH_FILTER_OUTPUTS)[number];

/** NativeSynth body/formant resonance voicing (`'default'` keeps the base patch's). */
export type SynthBodyType = (typeof SYNTH_BODY_TYPES)[number];

/** {@link SynthPatch} mod-matrix source. */
export type SynthModSource = (typeof SYNTH_MOD_SOURCES)[number];

/** {@link SynthPatch} mod-matrix destination. */
export type SynthModDestination = (typeof SYNTH_MOD_DESTINATIONS)[number];

/** One {@link SynthPatch} mod-matrix routing (name or C ordinal per field). */
export interface SynthModRouting {
  source: SynthModSource | number;
  destination: SynthModDestination | number;
  /** Destination units at full source deflection. */
  depth: number;
}

/**
 * Versioned NativeSynth patch for {@link Project.bounceWithSynthInstrument}
 * and {@link RealtimeEngine.setSynthInstrument}.
 *
 * The patch starts from a BASE — the named `preset` (see
 * {@link synthPresetNames}; a `"va:"` routing prefix is accepted) or, when
 * `preset` is omitted, the default subtractive patch. Every numeric field then
 * uses "0 / omit => keep the base value" (non-zero values override, clamped to
 * their audible ranges) and the enum fields reserve `'default'` as keep. The
 * frozen C ABI has no per-field presence bits, so explicit zero numeric
 * overrides (for example `ampSustain: 0`) cannot be represented; they keep the
 * base value. A non-empty `modRoutings` REPLACES the base mod matrix.
 *
 * Mode-specific deep parameters (FM operator stacks, modal mode tables,
 * drawbar registrations, kit pieces, piano strings) travel inside the named
 * presets; the patch exposes the wrapper sections every engine shares.
 */
export interface SynthPatch {
  /**
   * Optional binding convenience for JS realtime/offline helpers. It is not
   * part of the NativeSynth patch itself; Python uses explicit
   * `(destination_id, patch)` bindings instead. Defaults to `0`.
   */
  destinationId?: number;
  /** Base preset name (see {@link synthPresetNames}); omit for the init patch. */
  preset?: string;
  engineMode?: SynthEngineMode | number;
  waveform?: SynthOscWaveform | number;
  /** Detuned-stack width [1, 7]. */
  unison?: number;
  detuneCents?: number;
  /** Per-voice slow pitch drift depth (cents). */
  driftCents?: number;
  /** Pre-filter drive [0, 1]. */
  drive?: number;
  filterModel?: SynthFilterModel | number;
  filterOutput?: SynthFilterOutput | number;
  cutoffHz?: number;
  resonanceQ?: number;
  /** Cutoff keyboard tracking [0, 1]. */
  keyTrack?: number;
  envToCutoffCents?: number;
  velToCutoffCents?: number;
  ampAttackMs?: number;
  ampDecayMs?: number;
  /** 0 / omit keeps the base value; explicit zero sustain is not representable. */
  ampSustain?: number;
  ampReleaseMs?: number;
  filterAttackMs?: number;
  filterDecayMs?: number;
  /** 0 / omit keeps the base value; explicit zero sustain is not representable. */
  filterSustain?: number;
  filterReleaseMs?: number;
  lfoRateHz?: number;
  lfoToPitchCents?: number;
  lfo2RateHz?: number;
  glideMs?: number;
  body?: SynthBodyType | number;
  /** Body resonance mix [0, 1]. */
  bodyMix?: number;
  /** Seeded per-voice pan scatter [0, 1]. */
  stereoSpread?: number;
  /** Mod matrix (at most 8 routings; REPLACES the base matrix when non-empty). */
  modRoutings?: SynthModRouting[];
  /** Master output gain (linear). */
  gain?: number;
  /** Max simultaneous voices [1, 64]. */
  polyphony?: number;
  /** Gain-neutral bus saturation [0, 1]. */
  busDrive?: number;
}

/** Clip fade-curve for {@link Project.setClipFade}. */
export type ProjectFadeCurve =
  | 'linear'
  | 'equalPower'
  | 'exponential'
  | 'logarithmic'
  | 0
  | 1
  | 2
  | 3;

/** One clip fade region for {@link Project.setClipFade}. */
export interface ProjectClipFade {
  /** Fade length in PPQ (>= 0; 0 = no fade). */
  lengthPpq?: number;
  /** Fade curve (default `'linear'`). */
  curve?: ProjectFadeCurve;
}

/** Clip loop mode for {@link Project.setClipLoop}. */
export type ProjectLoopMode = 'off' | 'loop' | 0 | 1;

/** Automation breakpoint interpolation for {@link ProjectAutomationPoint}. */
export type ProjectAutomationCurve = 'linear' | 'exponential' | 'hold' | 'scurve' | 0 | 1 | 2 | 3;

/** One automation breakpoint accepted by the automation-lane edit ops. */
export interface ProjectAutomationPoint {
  /** Breakpoint position in PPQ. */
  ppq: number;
  /** Breakpoint value. */
  value: number;
  /** Curve to the next breakpoint (default `'linear'`). */
  curve?: ProjectAutomationCurve;
}

/** Automation-lane descriptor for {@link Project.addAutomationLane}. */
export interface ProjectAutomationLaneDesc {
  /** Host-defined id of the parameter the lane drives. */
  targetParamId: number;
  /** Breakpoints (stored verbatim). */
  points: ReadonlyArray<ProjectAutomationPoint>;
}

/** One tempo segment for {@link Project.setTempoSegments}. */
export interface ProjectTempoSegment {
  /** Segment start in PPQ. */
  startPpq: number;
  /** Tempo in beats per minute at the segment start. */
  bpm: number;
  /** Optional segment start in samples (default 0). */
  startSample?: number;
  /** Optional ramp end tempo in BPM (0 = constant tempo over the segment). */
  endBpm?: number;
}

/** One time-signature segment for {@link Project.setTimeSignatures}. */
export interface ProjectTimeSignatureSegment {
  /** Segment start in PPQ. */
  startPpq: number;
  /** Beats per bar (time-signature numerator). */
  numerator: number;
  /** Beat unit (time-signature denominator, e.g. 4 or 8). */
  denominator: number;
}

/** Key segment for {@link Project.annotateKeys}. */
export interface ProjectKeySegment {
  startPpq: number;
  endPpq: number;
  /** Tonic pitch class 0..11 (C=0) or 255 for unknown. */
  tonicPc?: number;
  /** KeyMode ordinal (0 unknown, 1 major, 2 minor, 3 dorian, ...). */
  mode?: number;
}

/** Chord symbol for {@link Project.annotateChords}. */
export interface ProjectChordSymbol {
  startPpq: number;
  endPpq: number;
  /** Root pitch class 0..11 (C=0) or 255 for unknown. */
  rootPc?: number;
  /** ChordQuality ordinal (0 unknown, 1 major, 2 minor, ...). */
  quality?: number;
  /** Extension semitone offsets (up to 8). */
  extensions?: ReadonlyArray<number>;
  /** Slash-bass pitch class 0..11 or 255 for none. */
  slashBassPc?: number;
  /** Optional roman-numeral label. */
  romanNumeral?: string;
  /** True at a modulation boundary. */
  modulationBoundary?: boolean;
}

/** Assist sidecar snapshot returned by {@link Project.getAssistSidecar}. */
export interface ProjectAssistSidecar {
  moduleId: string;
  schemaVersion: number;
  targetTrackId: number;
  regionStartPpq: number;
  regionEndPpq: number;
  payload: Uint8Array;
}

/** Track kind for {@link Project.addTrack}. */
export type ProjectTrackKind = 'audio' | 'midi' | 'aux' | 0 | 1 | 2;

/** Descriptor for {@link Project.addTrack}. */
export interface ProjectTrackDesc {
  kind?: ProjectTrackKind;
  name?: string;
}

export interface ProjectWarpAnchor {
  warpSample: number;
  sourceSample: number;
}

export interface ProjectWarpMapDesc {
  id: number;
  name?: string;
  anchors: ProjectWarpAnchor[];
}

/** Descriptor for {@link Project.addClip}. */
export interface ProjectClipDesc {
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
}

/** Result returned by {@link Project.addMidiClip}. */
export interface ProjectMidiClipResult {
  trackId: number;
  clipId: number;
}

/** Flat MIDI event accepted by {@link Project.setMidiEvents}. */
export interface ProjectMidiEvent {
  ppq: number;
  data0: number;
  data1?: number;
}

/** Options for {@link Project.midiRouteEvents}. `null`/omitted filter fields mean any/no remap. */
export interface ProjectMidiRouteConfig {
  filterGroup?: number | null;
  filterChannel?: number | null;
  remapChannel?: number | null;
  thru?: boolean;
}

/** Result of {@link Project.midiRouteEvents}. */
export interface ProjectMidiRouteResult {
  events: ProjectMidiEvent[];
  overflowed: boolean;
  overflowCount: number;
}

export type ProjectMidiCcBindingKind = 0 | 1 | 2 | 3;

/** Options for {@link Project.midiCcLearn}. All fields are optional. */
export interface MidiCcLearnOptions {
  /** Lower end of the mapped parameter range. Default `0`. */
  minValue?: number;
  /** Upper end of the mapped parameter range. Default `1`. */
  maxValue?: number;
  /** Minimum normalized CC movement required to learn a binding. Default `0`. */
  minMovement?: number;
}

/** MIDI CC <-> automation binding descriptor used by CC learn/conversion helpers. */
export interface ProjectMidiCcBinding {
  ccNumber: number;
  /** MIDI channel 0..15, or 255 for any channel. */
  channel: number;
  /** 0 = 7-bit CC, 1 = 14-bit CC, 2 = RPN, 3 = NRPN. */
  kind: ProjectMidiCcBindingKind;
  ccLsbNumber?: number;
  selectorMsb?: number;
  selectorLsb?: number;
  paramId: number;
  minValue: number;
  maxValue: number;
}

/** Result of {@link Project.validateMidiNotes}. */
export interface ProjectNotePairValidation {
  /** True when every note-on has a matching note-off (and vice versa). */
  ok: boolean;
  /** Count of note-ons that never received a matching note-off. */
  unmatchedNoteOns: number;
  /** Count of note-offs with no preceding matching note-on. */
  unmatchedNoteOffs: number;
}

/** One compile diagnostic (mirrors SonareProjectDiagnostic). */
export interface ProjectDiagnostic {
  code: number;
  /** 0 = error, 1 = warning. */
  severity: number;
  /** Affected clip / track / source id (0 = n/a). */
  targetId: number;
  /** Human-readable message for this diagnostic. */
  message: string;
}

/** Diagnostics summary returned by {@link Project.compile}. */
export interface ProjectCompileResult {
  /** Number of diagnostics surfaced by the compile. Kept for backward compatibility. */
  diagnosticCount: number;
  /** True when compilation produced a renderable timeline (no error diagnostics). */
  hasTimeline: boolean;
  /** Newline-joined human-readable detail of every diagnostic. */
  messages: string;
  diagnostics: ProjectDiagnostic[];
}

export interface ProjectDeserializeResult {
  project: Project;
  diagnostics: string;
}

// Embind handle for the C++ `ProjectWasm` class. The generated `SonareModule`
// type only gains `Project` / `projectAbiVersion` after a WASM rebuild, so the
// module is cast through this shape here.
interface WasmProject {
  toJson: () => string;
  setSampleRate: (sampleRate: number) => void;
  addTrack: (desc: { kind?: number | string; name?: string }) => number;
  addClip: (desc: ProjectClipDesc) => number;
  addMidiClip: (startPpq: number, lengthPpq: number) => ProjectMidiClipResult;
  splitClip: (clipId: number, splitPpq: number) => number;
  trimClip: (clipId: number, newStartPpq: number, newLengthPpq: number) => void;
  moveClip: (clipId: number, newStartPpq: number, newTrackId: number) => void;
  setTrackKind: (trackId: number, kind: number) => void;
  setClipWarpRef: (clipId: number, warpRefId: number) => void;
  setWarpMap: (map: ProjectWarpMapDesc) => void;
  removeWarpMap: (warpRefId: number) => void;
  setTrackMidiDestination: (trackId: number, destinationId: number) => void;
  undo: () => void;
  redo: () => void;
  setMidiEvents: (
    clipId: number,
    events: ReadonlyArray<ProjectMidiEvent | readonly [number, number, number]>,
  ) => void;
  importSmf: (data: Uint8Array) => number;
  exportSmf: () => Uint8Array;
  importClipFile: (data: Uint8Array) => number;
  exportClipFile: () => Uint8Array;
  setProgram: (clipId: number, program: number, bank: number) => void;
  setProgramOnChannel: (
    clipId: number,
    group: number,
    channel: number,
    program: number,
    bank: number,
  ) => void;
  bakeMidiFx: (clipId: number, configJson: string) => void;
  setMidiFx: (clipId: number, configJson: string) => void;
  validateMidiNotes: (clipId: number) => ProjectNotePairValidation;
  autoTempo: (audio: Float32Array, sampleRate: number) => number;
  snapToGrid: (ppq: number, strength: number) => number;
  compile: () => ProjectCompileResult;
  bounce: (options: ProjectBounceOptions) => Float32Array;
  bounceWithBuiltinInstrument: (
    bindings: BuiltinSynthBinding | ReadonlyArray<BuiltinSynthBinding> | undefined,
    options: ProjectBounceOptions,
  ) => Float32Array;
  bounceWithSynthInstrument: (
    bindings: SynthPatch | string | ReadonlyArray<SynthPatch | string> | undefined,
    options: ProjectBounceOptions,
  ) => Float32Array;
  loadSoundFont: (data: Uint8Array) => void;
  clearSoundFont: () => void;
  soundFontPresetCount: () => number;
  soundFontManifest: () => Sf2ProgramStatus[];
  bounceWithSf2Instrument: (
    bindings: Sf2InstrumentConfig | ReadonlyArray<Sf2InstrumentConfig> | undefined,
    options: ProjectBounceOptions,
  ) => Float32Array;
  removeClip: (clipId: number) => void;
  setClipGain: (clipId: number, gain: number) => void;
  setClipFade: (clipId: number, fadeIn: ProjectClipFade, fadeOut: ProjectClipFade) => void;
  setClipLoop: (clipId: number, loopMode: number, loopLengthPpq: number) => void;
  setClipSource: (clipId: number, sourceId: number) => void;
  duplicateClip: (clipId: number, newStartPpq: number) => number;
  removeTrack: (trackId: number) => void;
  renameTrack: (trackId: number, name: string) => void;
  setTrackRoute: (trackId: number, channelStripRef: string, outputTarget: string) => void;
  addAutomationLane: (
    trackId: number,
    desc: { targetParamId: number; points: ReadonlyArray<ProjectAutomationPoint> },
  ) => number;
  editAutomationLane: (
    trackId: number,
    laneIndex: number,
    desc: { targetParamId: number; points: ReadonlyArray<ProjectAutomationPoint> },
  ) => void;
  removeAutomationLane: (trackId: number, laneIndex: number) => void;
  annotateKeys: (keys: ReadonlyArray<ProjectKeySegment>) => void;
  annotateChords: (chords: ReadonlyArray<ProjectChordSymbol>) => void;
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
  setMarker: (markerId: number, ppq: number, name: string) => number;
  trackCount: () => number;
  sourceCount: () => number;
  tempoSegmentCount: () => number;
  timeSignatureCount: () => number;
  setTempoSegments: (segments: ReadonlyArray<ProjectTempoSegment>) => void;
  setTimeSignatures: (segments: ReadonlyArray<ProjectTimeSignatureSegment>) => void;
  lastBounceCompileResult: () => ProjectCompileResult;
  delete: () => void;
}

interface ProjectModule {
  Project: {
    new (): WasmProject;
    fromJson: (json: string) => WasmProject;
    fromJsonWithDiagnostics: (json: string) => { project: WasmProject; diagnostics: string };
  };
  projectAbiVersion: () => number;
  synthPresetNames: () => string[];
  synthPresetPatch: (name: string) => SynthPatch;
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
  ) => ProjectAutomationPoint | null;
  midiParamToCc: (
    bindings: ReadonlyArray<ProjectMidiCcBinding>,
    paramId: number,
    unitValue: number,
    group: number,
    ppq: number,
  ) => ProjectMidiEvent | null;
}

function projectModule(): ProjectModule {
  const candidate = getSonareModule() as unknown as Partial<ProjectModule>;
  if (typeof candidate.projectAbiVersion !== 'function' || candidate.Project === undefined) {
    throw new Error('libsonare was built without arrangement (headless DAW) support');
  }
  return candidate as ProjectModule;
}

function assertProjectU7(fnName: string, value: number, argName: string): number {
  if (!Number.isInteger(value) || value < 0 || value > 127) {
    throw new RangeError(`${fnName}: ${argName} must be an integer in [0, 127]`);
  }
  return value;
}

function assertProjectNibble(fnName: string, value: number, argName: string): number {
  if (!Number.isInteger(value) || value < 0 || value > 15) {
    throw new RangeError(`${fnName}: ${argName} must be an integer in [0, 15]`);
  }
  return value;
}

function projectMidi1Event(
  fnName: string,
  ppq: number,
  group: number,
  status: number,
  channel: number,
  data1: number,
  data2 = 0,
): ProjectMidiEvent {
  if (!Number.isFinite(ppq) || ppq < 0) {
    throw new RangeError(`${fnName}: ppq must be a non-negative finite number`);
  }
  const g = assertProjectNibble(fnName, group, 'group');
  const ch = assertProjectNibble(fnName, channel, 'channel');
  const d1 = assertProjectU7(fnName, data1, 'data1');
  const d2 = assertProjectU7(fnName, data2, 'data2');
  // UMP MIDI-1.0 channel-voice word (message type 0x2). Canonical layout is
  // sonare::midi::make_midi1_* (C-ABI sonare_midi_*, which Python delegates to);
  // this hand-written copy is locked against those words by the golden vectors
  // in project.test.ts (mirrored in the Node suite) so it cannot silently drift.
  const word = ((0x2 << 28) | (g << 24) | (status << 20) | (ch << 16) | (d1 << 8) | d2) >>> 0;
  return { ppq, data0: word, data1: 0 };
}

function assertProjectU32(fnName: string, value: number, argName: string): void {
  if (!Number.isInteger(value) || value < 0 || value > 0xffffffff) {
    throw new RangeError(`${fnName}: ${argName} must be an integer in [0, 4294967295]`);
  }
}

function assertProjectMidiEvents(
  fnName: string,
  events: ReadonlyArray<ProjectMidiEvent | readonly [number, number, number]>,
): void {
  if (!Array.isArray(events)) {
    throw new TypeError(`${fnName}: events must be an array`);
  }
  events.forEach((event, index) => {
    const prefix = `events[${index}]`;
    if (Array.isArray(event)) {
      if (event.length < 3) {
        throw new TypeError(`${fnName}: ${prefix} must contain [ppq, data0, data1]`);
      }
      if (!Number.isFinite(event[0]) || event[0] < 0) {
        throw new RangeError(`${fnName}: ${prefix}.ppq must be a non-negative finite number`);
      }
      assertProjectU32(fnName, event[1], `${prefix}.data0`);
      assertProjectU32(fnName, event[2], `${prefix}.data1`);
      return;
    }
    if (event === null || typeof event !== 'object') {
      throw new TypeError(`${fnName}: ${prefix} must be a MIDI event object or tuple`);
    }
    if (!Number.isFinite(event.ppq) || event.ppq < 0) {
      throw new RangeError(`${fnName}: ${prefix}.ppq must be a non-negative finite number`);
    }
    assertProjectU32(fnName, event.data0, `${prefix}.data0`);
    if (event.data1 !== undefined) {
      assertProjectU32(fnName, event.data1, `${prefix}.data1`);
    }
  });
}

/**
 * Runtime ABI version of the flat project POD layout exposed by this WASM
 * build. Equals {@link EXPECTED_PROJECT_ABI_VERSION} when the arrangement
 * subsystem is compiled in. Mirrors the C-ABI `sonare_project_abi_version`.
 */
export function projectAbiVersion(): number {
  return projectModule().projectAbiVersion();
}

/**
 * NativeSynth preset catalog names (`'sine'`, `'saw-lead'`, `'e-piano'`,
 * `'drum-kit'`, ...). Use these to discover valid {@link SynthPatch} preset
 * names instead of hardcoding magic strings.
 */
export function synthPresetNames(): string[] {
  return projectModule().synthPresetNames();
}

/**
 * Fetch a named catalog preset as a {@link SynthPatch} (the preset name plus
 * the wrapper-section values), so hosts can inspect a preset and tweak fields
 * before binding it. A `"va:"` routing prefix is accepted; unknown names
 * throw.
 */
export function synthPresetPatch(name: string): SynthPatch {
  return projectModule().synthPresetPatch(name);
}

export function synthEnumTables(): SynthEnumTables {
  return projectModule()._synthEnumTables();
}

export function synthPatchRoundTripForTest(patch: SynthPatch): SynthPatch {
  return projectModule()._synthPatchRoundTrip(patch);
}

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

  /** Undo the most recent edit. */
  undo(): void {
    this.native.undo();
  }

  /** Redo the most recently undone edit. */
  redo(): void {
    this.native.redo();
  }

  /** Replace a MIDI clip's entire event list. */
  setMidiEvents(
    clipId: number,
    events: ReadonlyArray<ProjectMidiEvent | readonly [number, number, number]>,
  ): void {
    assertProjectMidiEvents('Project.setMidiEvents', events);
    this.native.setMidiEvents(clipId, events);
  }

  /** Import an in-memory SMF buffer; returns the first added clip id. */
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

  /** Destructively bake a MIDI-FX chain into a clip's stored MIDI events. */
  bakeMidiFx(clipId: number, configJson: string): void {
    this.native.bakeMidiFx(clipId, configJson);
  }

  /** Backward alias for {@link bakeMidiFx}. */
  setMidiFx(clipId: number, configJson: string): void {
    this.bakeMidiFx(clipId, configJson);
  }

  /**
   * Pre-flight check for hanging / unmatched notes in a MIDI clip: reports
   * whether every note-on has a matching note-off (FIFO per channel+note).
   * Useful before bouncing to catch a stuck note. Throws if `clipId` is unknown
   * or not a MIDI clip.
   */
  validateMidiNotes(clipId: number): ProjectNotePairValidation {
    return this.native.validateMidiNotes(clipId);
  }

  /** Detect tempo from a mono buffer and install it; returns the primary BPM. */
  autoTempo(audio: Float32Array, sampleRate: number): number {
    return this.native.autoTempo(audio, sampleRate);
  }

  /** Snap a PPQ coordinate to the nearest beat of the project grid. */
  snapToGrid(ppq: number, strength = 1.0): number {
    return this.native.snapToGrid(ppq, strength);
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
   * default-destination sine patch. An explicitly empty array `[]` (or
   * `undefined` / `null`) produces zero bindings, so MIDI tracks render silently.
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
   * An explicitly empty array (or `undefined` / `null`) produces zero bindings.
   * Unknown preset names throw. Deterministic for a fixed project + options +
   * patch.
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
   * per-program backend). An explicitly empty array `[]` (or `undefined` /
   * `null`) produces zero bindings, so MIDI tracks render silently.
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

  /** Set a clip's loop mode + loop length in PPQ (undoable). */
  setClipLoop(clipId: number, loopMode: ProjectLoopMode, loopLengthPpq = 0): void {
    this.native.setClipLoop(clipId, projectLoopModeValue(loopMode), loopLengthPpq);
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

  /** Number of tracks in the project. */
  trackCount(): number {
    return this.native.trackCount();
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

function projectTrackKindValue(kind: ProjectTrackKind | undefined): number {
  if (kind === undefined || kind === 'audio') {
    return 0;
  }
  if (kind === 'midi') {
    return 1;
  }
  if (kind === 'aux') {
    return 2;
  }
  return kind;
}

function projectLoopModeValue(mode: ProjectLoopMode | undefined): number {
  if (mode === undefined || mode === 'off') {
    return 0;
  }
  if (mode === 'loop') {
    return 1;
  }
  return mode;
}
