import type { Project } from './project_class';

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

/**
 * Marker kind ordinals. Mirrors `SonareMarkerKind` in `src/sonare_c_types.h`;
 * the values are part of the ABI and must not be renumbered.
 */
export const MarkerKind = {
  marker: 0,
  text: 1,
  lyric: 2,
  cuePoint: 3,
  keySignature: 4,
} as const;

/** A project timeline marker with its kind and (for key signatures) the key. */
export interface ProjectMarker {
  /** Stable marker id (0 when allocating a new id via {@link Project.setMarkerEx}). */
  id: number;
  /** Marker position in PPQ (quarter notes). */
  ppq: number;
  /** Marker label. */
  name?: string;
  /** {@link MarkerKind} ordinal (default 0 = marker). */
  kind?: number;
  /** Key signature only: -7..7 (sharps positive). */
  keyFifths?: number;
  /** Key signature only: false = major, true = minor. */
  keyMinor?: boolean;
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
  'pipe-organ',
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
  | 'equal-power'
  | 'equal_power'
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

/** One alternate take for {@link Project.setClipTakes}. */
export interface ProjectClipTake {
  id: number;
  sourceId?: number;
  sourceOffsetPpq?: number;
  name?: string;
}

/** One comp segment for {@link Project.setClipCompSegments}. */
export interface ProjectClipCompSegment {
  startPpq: number;
  endPpq: number;
  takeId?: number;
}

/** Descriptor for {@link Project.addLoopRecordingTakes}. */
export interface ProjectLoopRecordingDesc {
  trackId: number;
  startPpq?: number;
  loopLengthPpq: number;
  audio: Float32Array;
  audioChannels?: number;
  audioSampleRate?: number;
}

/** Result returned by {@link Project.addLoopRecordingTakes}. */
export interface ProjectLoopRecordingResult {
  clipId: number;
  takeCount: number;
}

/** Clip loop mode for {@link Project.setClipLoop}. */
export type ProjectLoopMode = 'off' | 'loop' | 0 | 1;
export type ProjectWarpMode = 'off' | 'repitch' | 'tempo-sync' | 0 | 1 | 2;

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
  /** Derived segment start in samples. Accepted for compatibility, ignored on input. */
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
