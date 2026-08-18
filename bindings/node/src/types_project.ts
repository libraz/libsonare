import type { EngineAutomationPointCurve } from './types_engine.js';

/**
 * Expected project ABI version, mirroring `SONARE_PROJECT_ABI_VERSION`
 * (src/sonare_c_project.h) and the other bindings' constant. A `projectAbiVersion()`
 * that differs means the native binary lays out the flat project PODs
 * differently than this binding expects (0 = arrangement support compiled out).
 */
export const EXPECTED_PROJECT_ABI_VERSION = 1;

/** Track kind for {@link ProjectTrackDesc} (mirrors SonareProjectTrackKind). */
export type ProjectTrackKind = 'audio' | 'midi' | 'aux' | 0 | 1 | 2;

/** Track-kind ordinals (mirror SonareProjectTrackKind). */
export const PROJECT_TRACK_AUDIO = 0;

export const PROJECT_TRACK_MIDI = 1;

export const PROJECT_TRACK_AUX = 2;

/** Descriptor for {@link Project.addTrack}. */
export interface ProjectTrackDesc {
  /** Track kind: `'audio'` | `'midi'` | `'aux'` or the ordinal 0/1/2. */
  kind?: ProjectTrackKind;
  /** Optional track name. */
  name?: string;
}

/** Read-only stored track descriptor. */
export interface ProjectTrack {
  id: number;
  kind: number;
  midiDestinationId: number;
  gain: number;
  pan: number;
  mute: boolean;
  solo: boolean;
  name: string;
}

/** Read-only stored clip descriptor. */
export interface ProjectClip {
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

/** Read-only stored audio or MIDI source descriptor. */
export interface ProjectSource {
  id: number;
  kind: number;
  channelCount: number;
  storageHandleId: number;
  sampleRateHint: number;
  nameOrUri: string;
  /** Owning content hash for audio sources; empty for MIDI sources. */
  contentHash: string;
  /** External source-separation role for audio sources; empty for MIDI sources. */
  externalStemRole: string;
}

/** One first-class warp-map anchor. Sample positions must be finite and monotonic. */
export interface ProjectWarpAnchor {
  warpSample: number;
  sourceSample: number;
}

/** First-class project warp map referenced by clip `warpRefId`. */
export interface ProjectWarpMapDesc {
  id: number;
  name?: string;
  anchors: ProjectWarpAnchor[];
}

export type WarpMode = 'off' | 'repitch' | 'tempo-sync';

/**
 * Descriptor for {@link Project.addClip}. All musical positions are PPQ
 * (quarter notes); `lengthPpq` must be > 0.
 */
export interface ProjectClipDesc {
  /** Owning track id (from {@link Project.addTrack}). */
  trackId: number;
  /** `true` for a MIDI clip, `false`/omitted for an audio clip. */
  isMidi?: boolean;
  /** Clip start position in PPQ (default 0). */
  startPpq?: number;
  /** Clip length in PPQ (must be > 0). */
  lengthPpq: number;
  /** Offset into the source content in PPQ (default 0). */
  sourceOffsetPpq?: number;
  /** Linear clip gain (default 1). */
  gain?: number;
  /**
   * Decoded interleaved audio for an audio clip. When provided, the clip is
   * bound to a fresh renderable audio source; omit for a metadata-only source.
   */
  audio?: Float32Array;
  /** Channel count of `audio` (default 1). */
  audioChannels?: number;
  /** Sample rate of `audio` in Hz (default 0 = the project's). */
  audioSampleRate?: number;
  /** Optional host-local source reference for a metadata-only audio source. */
  sourceUri?: string;
}

/** One decoded output of an external source-separation model. */
export interface ExternalSeparatedStem {
  /** Non-empty, unique host label. It is used as the new track name. */
  name: string;
  /** Optional semantic metadata such as `'vocals'`; it has no DSP effect. */
  role?: string;
  /** Mono or stereo layout. */
  layout: 'mono' | 'stereo' | 1 | 2;
  /** One Float32Array per layout channel, all with the same frame count. */
  planarSamples: Float32Array[];
  /** Absolute project-rate frame where the stem begins (default 0). */
  startFrame?: number;
}

/** Request for {@link Project.importExternalStems}. */
export interface ExternalSeparatedStemImportRequest {
  /** Must exactly equal the target project's sample rate; no resampling occurs. */
  sampleRate: number;
  stems: ExternalSeparatedStem[];
}

/** Stable ids created by {@link Project.importExternalStems}. */
export interface ExternalSeparatedStemImportResult {
  trackIds: number[];
  clipIds: number[];
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

/** `(trackId, clipId)` returned by {@link Project.addMidiClip}. */
export interface ProjectMidiClipResult {
  trackId: number;
  clipId: number;
}

/**
 * A flat MIDI event accepted by {@link Project.setMidiEvents}. `data0` / `data1`
 * are the first two UMP words of a channel-voice message (stored opaquely).
 * `data1` defaults to 0. SysEx imported from SMF is preserved by the native
 * project side store; it is not constructible through this flat event object.
 * The tuple form `[ppq, data0, data1]` is also accepted.
 */
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

/** Result of {@link Project.compile}. */
export interface ProjectCompileResult {
  /** `true` when compilation produced a renderable timeline (no error diagnostics). */
  hasTimeline: boolean;
  /** Newline-joined human-readable diagnostic detail. */
  messages: string;
  diagnostics: ProjectDiagnostic[];
}

/** One tempo segment for {@link Project.setTempoSegments}. */
export interface ProjectTempoSegment {
  /** Segment start position in PPQ. */
  startPpq: number;
  /** Tempo in BPM at the segment start. */
  bpm: number;
  /** Derived segment start in samples. Accepted for compatibility, ignored on input. */
  startSample?: number;
  /** Tempo in BPM at the segment end for a ramp; `0` / omitted = constant tempo. */
  endBpm?: number;
}

/** One time-signature segment for {@link Project.setTimeSignatures}. */
export interface ProjectTimeSignatureSegment {
  /** Segment start position in PPQ. */
  startPpq: number;
  /** Beats per bar. */
  numerator: number;
  /** Beat unit (e.g. `4` for quarter note). */
  denominator: number;
}

/** A ranked primary/half/double tempo hypothesis from {@link Project.analyzeTempo}. */
export interface ProjectTempoCandidate {
  bpm: number;
  confidence: number;
  label: 'primary' | 'half' | 'double';
  timeSignatureCount: number;
  timeSignature: ProjectTimeSignatureSegment;
}

/** Options for {@link Project.bounce}. Zero / omitted fields take native defaults. */
export interface ProjectBounceOptions {
  /**
   * Render length in frames at the output sample rate. Omit / `<= 0` lets the
   * native side auto-derive the length from the arrangement (musical end plus
   * any instrument release tail). It does NOT produce an empty render.
   */
  totalFrames?: number;
  /** Render block size; <= 0 / omit => 128. */
  blockSize?: number;
  /** Output channel count; <= 0 / omit => 2. */
  numChannels?: number;
  /** Output sample rate; <= 0 / omit => the project's. */
  sampleRate?: number;
  /** Host-instrument PDC (samples) fed to the compiler. */
  instrumentLatencySamples?: number;
}

/** Names accepted by the minimal built-in oscillator synth. */
export const BUILTIN_SYNTH_WAVEFORMS = ['sine', 'saw', 'sawtooth', 'square', 'triangle'] as const;

/** Oscillator waveform for the {@link BuiltinInstrumentConfig built-in synth}. */
export type SynthWaveform = (typeof BUILTIN_SYNTH_WAVEFORMS)[number];

/**
 * Patch for the built-in minimal polyphonic oscillator synth used by
 * {@link Project.bounceWithBuiltinInstrument} /
 * {@link Project.bounceWithBuiltinInstruments}. Every numeric field uses
 * "0 / omit => sensible default", so an empty object is the default sine patch
 * and callers override only what they need.
 */
export interface BuiltinInstrumentConfig {
  /**
   * MIDI destination id this patch renders (the value set by
   * {@link Project.setTrackMidiDestination}). Defaults to `0`.
   */
  destinationId?: number;
  /** Oscillator waveform: a {@link SynthWaveform} name or numeric enum (0=sine). */
  waveform?: SynthWaveform | number;
  /** Master output gain (linear); 0 / omit => 0.2. */
  gain?: number;
  /** ADSR attack in ms; 0 / omit => 5. */
  attackMs?: number;
  /** ADSR decay in ms; 0 / omit => 60. */
  decayMs?: number;
  /** ADSR sustain level [0, 1]; 0 / omit => 0.7. */
  sustain?: number;
  /** ADSR release in ms; 0 / omit => 120. */
  releaseMs?: number;
  /** Max simultaneous voices; 0 / omit => 16, clamped to [1, 64]. */
  polyphony?: number;
}

/**
 * Cross-binding alias of {@link BuiltinInstrumentConfig}. The same built-in-synth
 * patch concept is named `BuiltinSynthConfig` in the Python binding; this alias
 * lets portable code use that shared name on the Node surface too.
 */
export type BuiltinSynthConfig = BuiltinInstrumentConfig;

/**
 * Patch for the GS-compatible SoundFont player used by
 * {@link Project.bounceWithSf2Instrument} /
 * {@link Project.bounceWithSf2Instruments} and
 * {@link RealtimeEngine.setSf2Instrument}. Every field uses
 * "0 / omit => sensible default".
 */
export interface Sf2InstrumentConfig {
  /**
   * MIDI destination id this player renders (the value set by
   * {@link Project.setTrackMidiDestination}). Defaults to `0`.
   */
  destinationId?: number;
  /** Master output gain (linear); 0 / omit => 0.5. */
  gain?: number;
  /** Max simultaneous voices; 0 / omit => 48, clamped to [1, 64]. */
  polyphony?: number;
  /** Prefer dedicated physical models for covered melodic GM programs. Defaults to false; drums stay SF2-first. */
  preferModelForModeledFamilies?: boolean;
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
  'bowed-string',
  'reed',
  'brass',
  'flute',
  'plucked-string',
  'vocal',
  'free-reed',
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

export const SYNTH_BODY_TYPES = [
  'default',
  'none',
  'guitar',
  'violin',
  'wood-tube',
  'brass-bell',
  'vocal',
] as const;

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
  builtinWaveforms: string[];
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
 * Versioned NativeSynth patch for {@link Project.bounceWithSynthInstrument} /
 * {@link Project.bounceWithSynthInstruments} and
 * {@link RealtimeEngine.setSynthInstrument}.
 *
 * The patch starts from a BASE — the named `preset` (see
 * {@link synthPresetNames}; a `"va:"` routing prefix is accepted) or, when
 * `preset` is omitted, the default subtractive patch. Omitting a numeric field
 * keeps the base value; supplying one overrides it (clamped to its audible
 * range), including an explicit `0` such as `stereoSpread: 0`. The enum fields
 * reserve `'default'` as keep. A `modRoutings` array REPLACES the base mod
 * matrix, and an empty array clears it, while omitting the key keeps it.
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
  /**
   * Follow incoming GM bank/program changes for offline project bounces and
   * route channel 10 through the GM drum map. Defaults to `false`, preserving
   * the fixed-patch behavior. This is a project-bounce binding option, not a
   * NativeSynth patch field.
   */
  useGmPrograms?: boolean;
  /** Base preset name (see {@link synthPresetNames}); omit for the init patch. */
  preset?: string;
  engineMode?: SynthEngineMode | number;
  // --- oscillator section (subtractive mode) ---
  waveform?: SynthOscWaveform | number;
  /** Detuned-stack width [1, 7]. */
  unison?: number;
  detuneCents?: number;
  /** Per-voice slow pitch drift depth (cents). */
  driftCents?: number;
  /** Pre-filter drive [0, 1]. */
  drive?: number;
  // --- filter section ---
  filterModel?: SynthFilterModel | number;
  filterOutput?: SynthFilterOutput | number;
  cutoffHz?: number;
  resonanceQ?: number;
  /** Cutoff keyboard tracking [0, 1]. */
  keyTrack?: number;
  envToCutoffCents?: number;
  velToCutoffCents?: number;
  // --- envelopes (ms / sustain in [0, 1]) ---
  ampAttackMs?: number;
  ampDecayMs?: number;
  ampSustain?: number;
  ampReleaseMs?: number;
  filterAttackMs?: number;
  filterDecayMs?: number;
  filterSustain?: number;
  filterReleaseMs?: number;
  // --- LFOs / glide ---
  lfoRateHz?: number;
  lfoToPitchCents?: number;
  lfo2RateHz?: number;
  glideMs?: number;
  // --- realism polish ---
  body?: SynthBodyType | number;
  /** Body resonance mix [0, 1]. */
  bodyMix?: number;
  /** Seeded per-voice pan scatter [0, 1]. */
  stereoSpread?: number;
  /** Mod matrix (at most 8 routings; REPLACES the base matrix when non-empty). */
  modRoutings?: SynthModRouting[];
  // --- voice pool / bus ---
  /** Master output gain (linear). */
  gain?: number;
  /** Max simultaneous voices [1, 64]. */
  polyphony?: number;
  /** Gain-neutral bus saturation [0, 1]. */
  busDrive?: number;
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

/** Clip fade-curve ordinals/names (mirror SonareProjectFadeCurve). */
export type ProjectFadeCurve =
  | 0
  | 1
  | 2
  | 3
  | 'linear'
  | 'equal-power'
  | 'equal_power'
  | 'equalPower'
  | 'equalpower'
  | 'exponential'
  | 'exp'
  | 'logarithmic'
  | 'log';

export const PROJECT_FADE_CURVE_LINEAR = 0;

export const PROJECT_FADE_CURVE_EQUAL_POWER = 1;

export const PROJECT_FADE_CURVE_EXPONENTIAL = 2;

export const PROJECT_FADE_CURVE_LOGARITHMIC = 3;

/** Clip loop-mode ordinals/names (mirror SonareProjectLoopMode). */
export type ProjectLoopMode = 0 | 1 | 'off' | 'loop';

export const PROJECT_LOOP_MODE_OFF = 0;

export const PROJECT_LOOP_MODE_LOOP = 1;

/** Persistent target classification for a project automation lane. */
export type ProjectAutomationTargetKind = 'opaque' | 'track-fader-db' | 'track-pan' | 0 | 1 | 2;

/** Automation target-kind ordinals (mirror SonareAutomationTargetKind). */
export const PROJECT_AUTOMATION_TARGET_OPAQUE = 0;

export const PROJECT_AUTOMATION_TARGET_TRACK_FADER_DB = 1;

export const PROJECT_AUTOMATION_TARGET_TRACK_PAN = 2;

/** One clip fade region for {@link Project.setClipFade}. */
export interface ProjectClipFade {
  /** Fade length in PPQ; finite and >= 0 (0 = no fade). */
  lengthPpq: number;
  /** Interpolation curve ({@link ProjectFadeCurve}); default linear (0). */
  curve?: ProjectFadeCurve;
}

/** One alternate take for {@link Project.setClipTakes}. */
export interface ProjectClipTake {
  /** Non-zero take id unique within the clip. */
  id: number;
  /** Source id for this take; 0 reuses the clip's current source. */
  sourceId?: number;
  /** Offset into the take source in PPQ. */
  sourceOffsetPpq?: number;
  /** Optional UI/display name. */
  name?: string;
}

/** One comp segment for {@link Project.setClipCompSegments}. */
export interface ProjectClipCompSegment {
  startPpq: number;
  endPpq: number;
  /** Take id to play in this range; 0 falls back to the active/default take. */
  takeId?: number;
}

/**
 * One automation breakpoint for {@link Project.addAutomationLane} /
 * {@link Project.editAutomationLane}. `curve` (alias `curveToNext`) is the
 * PPQ-domain curve to the next breakpoint (0 = Linear default, 1 = Exponential,
 * 2 = Hold, 3 = SCurve).
 */
export interface ProjectAutomationPoint {
  ppq: number;
  value: number;
  curve?: EngineAutomationPointCurve;
  curveToNext?: EngineAutomationPointCurve;
}

/**
 * Descriptor for {@link Project.addAutomationLane} /
 * {@link Project.editAutomationLane}.
 */
export interface ProjectAutomationLaneDesc {
  /** Host-defined, non-zero target parameter id the lane drives (zero is reserved). */
  targetParamId: number;
  /**
   * Optional persistent target classification. Omit for the legacy opaque lane
   * form; typed values are serialized in project schema version 2.
   */
  targetKind?: ProjectAutomationTargetKind;
  /** Breakpoints (stored verbatim; need not be pre-sorted). */
  points: ProjectAutomationPoint[];
}

/** One key segment for {@link Project.annotateKeys}. */
export interface ProjectKeySegment {
  startPpq: number;
  endPpq: number;
  /** Tonic pitch class 0..11 (C=0) or 255 for unknown. Default 255. */
  tonicPc?: number;
  /**
   * KeyMode ordinal: 0 unknown, 1 major, 2 minor, 3 dorian, 4 phrygian,
   * 5 lydian, 6 mixolydian, 7 locrian. Default 0.
   */
  mode?: number;
}

/** One chord symbol for {@link Project.annotateChords}. */
export interface ProjectChordSymbol {
  startPpq: number;
  endPpq: number;
  /** Root pitch class 0..11 (C=0) or 255 for unknown. Default 255. */
  rootPc?: number;
  /**
   * ChordQuality ordinal: 0 unknown, 1 major, 2 minor, 3 diminished,
   * 4 augmented, 5 dominant, 6 half-diminished, 7 suspended. Default 0.
   */
  quality?: number;
  /** Extension semitone offsets (up to 8). */
  extensions?: number[];
  /** Slash-bass pitch class 0..11 or 255 for none. Default 255. */
  slashBassPc?: number;
  /** Optional roman-numeral label. */
  romanNumeral?: string;
  /** Marks a modulation boundary. Default false. */
  modulationBoundary?: boolean;
}

/** Descriptor for {@link Project.setAssistSidecar}. */
export interface ProjectAssistSidecarInput {
  /** Non-empty module id key. */
  moduleId: string;
  /** Module-defined schema version. Default 0. */
  schemaVersion?: number;
  /** Target track id (0 = project scope). Default 0. */
  targetTrackId?: number;
  /** Region start in PPQ. Default 0. */
  regionStartPpq?: number;
  /** Region end in PPQ. Default 0. */
  regionEndPpq?: number;
  /** Opaque module-owned payload bytes. */
  payload?: Uint8Array;
}

/** A stored assist sidecar returned by {@link Project.getAssistSidecar}. */
export interface ProjectAssistSidecar {
  moduleId: string;
  schemaVersion: number;
  targetTrackId: number;
  regionStartPpq: number;
  regionEndPpq: number;
  payload: Uint8Array;
}

/**
 * Request form of {@link Project.bakeMidiFx}. The positional
 * `(clipId, configJson)` call stays supported and normalizes to this shape.
 */
export interface ProjectMidiFxBakeRequest {
  /** Target MIDI clip id. */
  clipId: number;
  /** MIDI-FX chain configuration as JSON. */
  configJson: string;
  /** Return per-event provenance in the result. Default false. */
  withSourceIndex?: boolean;
}

/** Result of the request form of {@link Project.bakeMidiFx}. */
export interface ProjectMidiFxBakeResult {
  /**
   * One entry per transformed event in canonical order: the index of the input
   * event it derives from, or -1 for an event with no originating input. Chord
   * and arpeggiator fan-out makes several outputs share one source index, so a
   * caller that treats the first output per index as the same event and the
   * rest as newly generated can carry a selection across the bake. Present only
   * when the request set `withSourceIndex`.
   */
  sourceIndex?: Int32Array;
}

/** Request form of {@link Project.previewMidiFxCount}. */
export interface ProjectMidiFxPreviewRequest {
  /** Target MIDI clip id. */
  clipId: number;
  /** MIDI-FX chain configuration as JSON. */
  configJson: string;
}
