/**
 * Instrument definitions a project source plays through: the built-in synth
 * patch, the SoundFont player, and the sample bank's descriptors.
 */

import type { SampleBank } from './sample_bank';

/** Names accepted by the minimal built-in oscillator synth. */
export const BUILTIN_SYNTH_WAVEFORMS = ['sine', 'saw', 'sawtooth', 'square', 'triangle'] as const;

/** Oscillator waveform for the built-in synth. */
export type BuiltinSynthWaveform = (typeof BUILTIN_SYNTH_WAVEFORMS)[number] | 0 | 1 | 2 | 3;

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
  /**
   * Oscillator waveform (default `'sine'`). The one field with no nearest
   * sensible value — a name or an ordinal outside the set throws rather than
   * falling back to sine.
   */
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
  /** Prefer dedicated physical models for covered melodic GM programs. Defaults to false; drums stay SF2-first. */
  preferModelForModeledFamilies?: boolean;
  /**
   * Render the instrument alone, without the stage the bank binds after its
   * voice — the amplifier and cabinet an electric guitar is never heard
   * without. Defaults to false, so a MIDI file that selects a distorted guitar
   * and asks for nothing else still comes out amplified. Set it to get the
   * direct signal, which is what a voice is calibrated against.
   */
  clearBankRig?: boolean;
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
  'bowed-string',
  'reed',
  'brass',
  'flute',
  'plucked-string',
  'vocal',
  'free-reed',
  'harpsichord',
  'sample',
] as const;

export const SAMPLE_LOOP_MODES = ['default', 'none', 'continuous', 'key-down'] as const;

export const SAMPLE_KEY_TRACKS = ['default', 'on', 'off'] as const;

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
  'breath',
  'aftertouch',
  'expression-cc',
  'pitch-bend',
] as const;

export const SYNTH_MOD_DESTINATIONS = [
  'none',
  'pitch-cents',
  'cutoff-cents',
  'amp-gain',
  'pan-units',
  'resonance-q',
  'vibrato-depth-cents',
  'filter-env-depth',
  'lfo1-rate-scale',
  'excitation-force',
  'excitation-position',
  'excitation-brightness',
  'spectrum-morph',
] as const;

/** How a device spells a gesture (see {@link ControllerBinding}). */
export const CONTROLLER_INPUTS = [
  'control-change',
  'channel-pressure',
  'poly-pressure',
  'pitch-bend',
  'velocity',
] as const;

/** What a gesture means — the expression axis a binding drives. */
export const CONTROLLER_AXES = [
  'none',
  'excitation',
  'position',
  'brightness',
  'morph',
  'loudness',
  'pitch-cents',
  'vibrato-depth',
] as const;

/**
 * What a channel does with a note-on while another note on that channel is
 * still held. `'mono-legato'` carries the sounding voice and only moves its
 * pitch — a wind player's slur — and is deliberately out of reach of any MIDI
 * message: CC126 names a monophonic mode but not this one.
 */
export const ARTICULATIONS = ['poly', 'mono-retrigger', 'mono-legato'] as const;

/** The three dimensions MPE carries per note ({@link RealtimeEngine.setControllerNoteTracking}). */
export const MPE_DIMENSIONS = ['bend', 'pressure', 'timbre'] as const;

/**
 * Which note a value addressed to a whole MIDI channel belongs to when several
 * are sounding on it ({@link RealtimeEngine.setControllerNoteTracking}).
 *
 * MPE poses this question and declines to answer it — how a controller affects
 * the notes when more than one is active on a member channel is left to the
 * device — so this is a choice rather than a rule. A released note is never
 * selected, whatever the rule and however long a pedal keeps it sounding.
 */
export const NOTE_TRACKINGS = ['last', 'lowest', 'highest', 'all'] as const;

export interface SynthEnumTables {
  engineModes: string[];
  waveforms: string[];
  builtinWaveforms: string[];
  filterModels: string[];
  filterOutputs: string[];
  bodyTypes: string[];
  modSources: string[];
  modDestinations: string[];
  controllerInputs: string[];
  controllerAxes: string[];
  articulations: string[];
  mpeDimensions: string[];
  noteTrackings: string[];
}

/** NativeSynth engine selector ({@link SynthPatch}; `'default'` keeps the base patch's). */
export type SynthEngineMode = (typeof SYNTH_ENGINE_MODES)[number];

/**
 * Per-patch loop override for the sample engine (`'default'` keeps what the
 * bank recorded for the sample the zone names).
 *
 * A different set of values from {@link SampleDesc.loopMode}, which is the
 * SoundFont `sampleModes` number describing the recording itself.
 */
export type SampleLoopMode = (typeof SAMPLE_LOOP_MODES)[number];

/** Whether a sample follows the played key (`'default'` keeps the base patch's). */
export type SampleKeyTrack = (typeof SAMPLE_KEY_TRACKS)[number];

/**
 * Loop behaviour recorded for one sample in a {@link SampleBank}.
 *
 * A number is the raw SoundFont `sampleModes` value the C struct carries
 * (`0` no loop, `1` continuous, `3` while the key is held), so SF2-derived data
 * passes through untranslated; the names are the readable spellings of the same
 * three states. There is no `'default'`: a sample's own loop mode is where the
 * default comes from.
 */
export type SampleDescLoopMode = 'none' | 'continuous' | 'key-down';

/**
 * Tuning and looping of one sample, in units relative to that sample
 * ({@link SampleBank.addSample}).
 *
 * Every field is optional and the omitted state is meaningful: the empty
 * descriptor is an unlooped sample rooted at middle C and played at the
 * render's own rate.
 */
export interface SampleDesc {
  /** MIDI key at which the sample sounds at its recorded pitch. Defaults to 60. */
  rootKey?: number;
  /** Fine tuning applied on top of {@link SampleDesc.rootKey}. */
  fineTuneCents?: number;
  /** Rate the sample was recorded at; omit to play it at the render's rate. */
  sourceRate?: number;
  /** Loop start, as a frame offset inside this sample. */
  loopStart?: number;
  /** Loop end, as a frame offset inside this sample. */
  loopEnd?: number;
  /**
   * The sample's own loop behaviour. A loop that survives clamping empty is
   * dropped, so a malformed loop plays unlooped rather than wrapping over
   * nothing. {@link SynthPatch.sampleLoop} overrides this per patch.
   */
  loopMode?: SampleDescLoopMode | number;
}

/**
 * One key/velocity rectangle mapped onto a sample ({@link SampleBank.addZone}).
 *
 * Every bound defaults on its own, so narrowing one edge never collapses
 * another: an omitted upper bound is `127`, an omitted `velLo` is `1` (velocity
 * zero is a note-off, not a dynamic), and an omitted `keyLo` is simply the
 * lowest key. `{ keyLo: 48 }` is therefore keys 48-127 at every velocity and
 * `{ velLo: 64 }` its exact mirror, while an empty rectangle is the whole
 * keyboard. The one rectangle this cannot express is the single key `0`.
 */
export interface SampleZoneDesc {
  /**
   * Keymap set the zone joins; a {@link SynthPatch} names a set through
   * {@link SynthPatch.sampleSet}. Sets below it are created. Defaults to `0`.
   */
  setIndex?: number;
  /** Sample the zone plays, as returned by {@link SampleBank.addSample}. */
  sampleIndex?: number;
  /** Lowest key of the rectangle. Defaults to `0`. */
  keyLo?: number;
  /** Highest key of the rectangle. Defaults to `127`. */
  keyHi?: number;
  /** Lowest velocity of the rectangle. Defaults to `1`. */
  velLo?: number;
  /** Highest velocity of the rectangle. Defaults to `127`. */
  velHi?: number;
  /** Added to the sample's own fine tuning. */
  tuneCents?: number;
  /** Linear gain; omit for unity. */
  gain?: number;
  /** Pan in the voice mixer's units, `-500` to `500`. */
  panUnits?: number;
}

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

/** Input side of a {@link ControllerBinding}: how the device spells the gesture. */
export type ControllerInput = (typeof CONTROLLER_INPUTS)[number];

/** Output side of a {@link ControllerBinding}: which expression axis it means. */
export type ControllerAxis = (typeof CONTROLLER_AXES)[number];

/** Per-channel note-overlap rule ({@link RealtimeEngine.setArticulation}). */
export type Articulation = (typeof ARTICULATIONS)[number];

/** One per-note MPE dimension ({@link MPE_DIMENSIONS}). */
export type MpeDimension = (typeof MPE_DIMENSIONS)[number];

/** One note-attribution rule ({@link NOTE_TRACKINGS}). */
export type NoteTracking = (typeof NOTE_TRACKINGS)[number];

/**
 * One device gesture bound to one expression axis
 * ({@link RealtimeEngine.bindController}).
 *
 * Binding the same input twice with different axes is how a single gesture
 * reaches two of them, which is what a breath controller driving both
 * excitation and loudness needs. `input` and `axis` are required and are the
 * canonical names (or their C ordinals); an unknown name throws rather than
 * resolving to the first member.
 *
 * @example
 * ```ts
 * engine.bindController(0, { input: 'control-change', index: 2, axis: 'excitation' });
 * ```
 */
export interface ControllerBinding {
  /** How the device spells the gesture. */
  input: ControllerInput | number;
  /**
   * CC number 0-127 for `'control-change'`. Every other input is identified by
   * its message status alone and ignores this. Default `0`.
   */
  index?: number;
  /**
   * Which expression axis the gesture means. `'none'` is refused: a binding
   * that means nothing is a caller mistake, not an empty slot.
   */
  axis: ControllerAxis | number;
  /**
   * Axis value at zero deflection, in the axis's own unit — normalized `[0,1]`
   * for the excitation axes and loudness, cents for pitch and vibrato depth.
   * Default `0`.
   */
  lo?: number;
  /** Axis value at full deflection; `lo > hi` inverts the gesture. Default `1`. */
  hi?: number;
  /**
   * Exponent applied to the normalized input before the range maps it. Default
   * `1` (linear) and deliberately so: a wind controller has already applied the
   * curve its player chose, and a second one on this side bends a gesture that
   * was already shaped. Must be finite and positive.
   */
  curve?: number;
}

/** One {@link SynthPatch} mod-matrix routing (name or C ordinal per field). */
export interface SynthModRouting {
  source: SynthModSource | number;
  destination: SynthModDestination | number;
  /**
   * Destination units at full source deflection.
   *
   * For the three `excitation-*` destinations and `spectrum-morph` this is an
   * offset in the engine's own normalized `[0, 1]` axis units — the same scale
   * the live-control CCs drive — summed onto whatever the patch or a CC set and
   * clamped by the engine. The `excitation-*` ones reach the physical model's
   * exciter (bow force and contact point, breath pressure, bore brightness), so
   * only the continuously-excited engines act on them: `bowed-string`, `brass`,
   * `reed` and `flute`. An engine whose exciter is finished at note-on has
   * nothing per sample to reach and ignores them. `spectrum-morph` travels
   * between the two spectral tables a patch carries — today the drawbar organ's
   * second registration — and a patch carrying one table declines it.
   */
  depth: number;
}

/**
 * Versioned NativeSynth patch for {@link Project.bounceWithSynthInstrument}
 * and {@link RealtimeEngine.setSynthInstrument}.
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
  /** Resolve MIDI channels from incoming GM bank/program changes; defaults to false. */
  useGmPrograms?: boolean;
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
  /**
   * Series 12 dB/oct highpass after the main filter, in Hz; 0 disables the
   * stage. The other end of a band the lowpass alone cannot make. It runs at
   * Butterworth Q -- `resonanceQ` belongs to the main filter.
   */
  hpCutoffHz?: number;
  /**
   * Rate the voice's output is held at, in Hz; 0 disables the stage. The
   * voice's own converter, ahead of its amplitude envelope: a drum machine runs
   * one far below the mix rate, and the aliased images that folds down are as
   * much of its sound as its samples are. Per voice, so a kit can convert the
   * voices a machine stores and leave its analogue ones alone.
   */
  sampleHoldHz?: number;
  /**
   * Word length the held value is quantized to, in bits; 0 disables the
   * quantizer. Fractional values are meaningful — a converter's effective
   * resolution is rarely a whole number of bits.
   */
  bitDepth?: number;
  resonanceQ?: number;
  /** Cutoff keyboard tracking [0, 1]. */
  keyTrack?: number;
  envToCutoffCents?: number;
  velToCutoffCents?: number;
  // --- pitch offset ---
  /**
   * Constant transposition of the voice's own pitch, in cents, clamped to
   * [-4800, 4800]; 0 leaves the pitch alone. Applied on top of the note, so it
   * shifts a whole patch without rewriting the part — a detuned layer, a sample
   * set mapped a semitone off, an instrument pitched to a reference other than
   * A440. Carried in the per-sample pitch factor every engine's render already
   * takes, so it applies the same amount on all of them.
   *
   * Also automatable under this same name through
   * {@link RealtimeEngine.resolveInstrumentAutomationId}; it is one of the names
   * that reaches a voice that is already sounding, rather than waiting for the
   * next note.
   */
  pitchOffsetCents?: number;
  ampAttackMs?: number;
  ampDecayMs?: number;
  ampSustain?: number;
  ampReleaseMs?: number;
  filterAttackMs?: number;
  filterDecayMs?: number;
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
  /**
   * Bank the sample engine reads its PCM from. Binding convenience for the JS
   * offline helpers rather than part of the patch itself, like `destinationId`:
   * it is resolved to a native handle before the patch crosses into WASM.
   * A `'sample'` patch bound without a bank renders silence.
   */
  sampleBank?: SampleBank;
  /**
   * Keymap set in the bound bank (negative selects none). Read only by a
   * `'sample'` patch, which is what lets set `0` stay addressable without a
   * "keep the base value" sentinel of its own.
   */
  sampleSet?: number;
  /** Linear gain on the sample. */
  sampleLevel?: number;
  sampleLoop?: SampleLoopMode | number;
  /** Attack skip, as a fraction of the mapped region. */
  sampleStartOffset?: number;
  sampleKeyTrack?: SampleKeyTrack | number;
}
