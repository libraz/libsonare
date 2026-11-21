/**
 * Instrument definitions a project source plays through: the built-in synth
 * patch, the SoundFont player, and the sample bank's descriptors.
 */

import type { MasteringInsertParamUnit } from './public_types.js';
import type { SampleBank } from './sample_bank.js';

/** Names accepted by the minimal built-in oscillator synth. */
export const BUILTIN_SYNTH_WAVEFORMS = ['sine', 'saw', 'sawtooth', 'square', 'triangle'] as const;

/** Oscillator waveform for the built-in synth. */
export type BuiltinSynthWaveform = (typeof BUILTIN_SYNTH_WAVEFORMS)[number] | 0 | 1 | 2 | 3;

/**
 * Built-in synth patch for {@link ProjectBounceOptions.builtinInstruments} and
 * {@link RealtimeEngine.setBuiltinInstrument}. Every field is optional; an
 * omitted field takes the library default (gain 0.2, attack 5ms, decay 60ms,
 * sustain 0.7, release 120ms, 16 voices), so `{}` is a usable default sine
 * patch. A given value is read literally and refused outside its range.
 */
export interface BuiltinSynthConfig {
  /**
   * Oscillator waveform (default `'sine'`). The one field with no nearest
   * sensible value — an unknown name throws rather than falling back to sine,
   * and a number is a `TypeError`.
   */
  waveform?: BuiltinSynthWaveform;
  /** Master output gain, linear, in (0, 4]; default 0.2. */
  gain?: number;
  /** ADSR attack in ms, in (0, 20000]; default 5. A linear ramp that reaches full level at the set time. */
  attackMs?: number;
  /** ADSR decay in ms, in (0, 20000]; default 60. A linear ramp, the set time is a full 0 to 1 sweep. */
  decayMs?: number;
  /** ADSR sustain level, in (0, 1]; default 0.7. */
  sustain?: number;
  /** ADSR release in ms, in (0, 20000]; default 120. A linear ramp, the set time is a full 0 to 1 sweep. */
  releaseMs?: number;
  /** Max simultaneous voices, in [1, 64]; default 16. */
  polyphony?: number;
}

/**
 * One instrument bound to a MIDI destination for an instrument-bound bounce:
 * the `builtinInstruments`, `synthInstruments`, `sf2Instruments` or `instruments`
 * option of {@link Project.bounce}. The destination is part of the pair, never of
 * the instrument.
 */
export interface ProjectInstrumentBinding<T> {
  /** MIDI destination id the instrument answers to; omit for destination `0` (see {@link Project.setTrackMidiDestination}). */
  destinationId?: number;
  /** The instrument to bind. */
  instrument: T;
}

/** One dispatched MIDI event delivered to {@link ExternalInstrument.onEvent}. */
export interface ExternalInstrumentEvent {
  /** MIDI destination id the event is routed to. */
  destinationId: number;
  /** The event's 32-bit UMP words (one or two), as unsigned integers. */
  words: number[];
  /**
   * Sample-accurate position in frames this instrument has been asked to
   * render so far; the events that close the bounce sit one past the last frame.
   */
  renderFrame: number;
}

/**
 * A JavaScript instrument driven during {@link Project.bounce} with `instruments`.
 * Only `render` is required. Every callback runs synchronously while the
 * bounce is blocked and must not return a Promise.
 */
export interface ExternalInstrument {
  /**
   * Called once per render pass, before the first block, with the sample rate,
   * the largest block `render` will receive, and the number of arrays `render`
   * gets (always 2: the engine hosts an instrument as a stereo source whatever
   * the bounce's `numChannels`).
   */
  prepare?(sampleRate: number, maxBlockFrames: number, channels: number): void;
  /** Called for each dispatched MIDI event, just before the block it falls in is rendered. */
  onEvent?(event: ExternalInstrumentEvent): void;
  /**
   * Write `frames` of audio into `outputs`, one `Float32Array` per channel. The
   * arrays are scratch space that arrive zero-filled; whatever they hold when
   * `render` returns normally is added into the engine's buffers (nothing is
   * added if it throws). They are reused for the next block, so do not keep them.
   */
  render(outputs: Float32Array[], frames: number): void;
  /** Reported instrument latency in samples (plugin delay compensation); defaults to 0. */
  latencySamples?: number;
  /** Release / effect tail in samples, used to extend an auto-length bounce; defaults to 0. */
  tailSamples?: number;
}

/**
 * SoundFont (SF2) player patch for {@link ProjectBounceOptions.sf2Instruments}. Every field is optional; an
 * omitted field takes the library default (gain 0.5, 48 voices), so `{}` is a usable default
 * patch. A given value is read literally and refused outside its range.
 */
export interface Sf2InstrumentConfig {
  /** Master output gain, linear, in [0, 4]; default 0.5. 0 is silence. */
  gain?: number;
  /** Max simultaneous voices, in [1, 64]; default 48. */
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
  /**
   * How the GS insertion effects (EFX) are realised: 'modern' (default) or
   * 'classic'. Any other value is refused by name.
   */
  gsEfxRealization?: 'modern' | 'classic';
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

/**
 * Every NativeSynth engine selector, by name, in the library's enum order.
 *
 * @remarks
 * Selecting a mode blanks every engine section but its own. From a patch that
 * sets only `engineMode`, every mode sounds except `'sample'`, which has
 * nothing to voice until a {@link SampleBank} is supplied: the engine section
 * is the named preset's when that preset uses the selected engine, otherwise
 * the engine's base preset's (`'fm'` the ePiano's, `'percussion'` the GM drum
 * kit, ...). A bare mode therefore follows that preset's bank version. The
 * fields of the section are set through {@link SynthPatch.engineParams} and
 * described by {@link synthEngineParamInfo}.
 */
export const SYNTH_ENGINE_MODES = [
  'default',
  'subtractive',
  'fm',
  'karplusStrong',
  'modal',
  'additive',
  'percussion',
  'piano',
  'pipeOrgan',
  'bowedString',
  'reed',
  'brass',
  'flute',
  'pluckedString',
  'vocal',
  'freeReed',
  'harpsichord',
  'sample',
  'singing',
] as const;

/** Loop modes {@link SynthPatch.sampleLoop} can force; `'default'` keeps the mode the bank recorded. */
export const SAMPLE_LOOP_MODES = ['default', 'none', 'continuous', 'keyDown'] as const;

/** Key-tracking choices for {@link SynthPatch.sampleKeyTrack}; `'default'` keeps the base patch's. */
export const SAMPLE_KEY_TRACKS = ['default', 'on', 'off'] as const;

/** Voice start-state seeding modes for {@link SynthPatch.retrigger}; `'default'` keeps the base patch's. */
export const SYNTH_RETRIGGERS = ['default', 'free', 'note'] as const;

/** LFO waveforms for {@link SynthPatch.lfoShape}; `'default'` keeps the base patch's. */
export const SYNTH_LFO_SHAPES = ['default', 'triangle', 'sine', 'random'] as const;

/** Oscillator waveforms for {@link SynthPatch.waveform} (subtractive engine only); `'default'` keeps the base patch's. */
export const SYNTH_OSC_WAVEFORMS = [
  'default',
  'sine',
  'saw',
  'square',
  'triangle',
  'noise',
] as const;

/** Filter character cores for {@link SynthPatch.filterModel}; `'default'` keeps the base patch's. */
export const SYNTH_FILTER_MODELS = [
  'default',
  'svf',
  'transistorLadder',
  'diodeLadder',
  'sallenKey',
  'svf4Pole',
] as const;

/** Filter outputs for {@link SynthPatch.filterOutput} (SVF only); `'default'` keeps the base patch's. */
export const SYNTH_FILTER_OUTPUTS = ['default', 'lowPass', 'bandPass', 'highPass'] as const;

/** Body/formant resonance voicings for {@link SynthPatch.body}; `'default'` keeps the base patch's. */
export const SYNTH_BODY_TYPES = [
  'default',
  'none',
  'guitar',
  'violin',
  'woodTube',
  'brassBell',
  'vocal',
] as const;

/** Sources a {@link SynthModRouting} can read; `'none'` is refused. */
export const SYNTH_MOD_SOURCES = [
  'none',
  'ampEnv',
  'filterEnv',
  'lfo1',
  'lfo2',
  'velocity',
  'keyTrack',
  'modWheel',
  'random',
  'breath',
  'aftertouch',
  'expressionCc',
  'pitchBend',
] as const;

/** Destinations a {@link SynthModRouting} can drive; `'none'` is refused. */
export const SYNTH_MOD_DESTINATIONS = [
  'none',
  'pitchCents',
  'cutoffCents',
  'ampGain',
  'panUnits',
  'resonanceQ',
  'vibratoDepthCents',
  'filterEnvDepth',
  'lfo1RateScale',
  'excitationForce',
  'excitationPosition',
  'excitationBrightness',
  'spectrumMorph',
] as const;

/** How a device spells a gesture (see {@link ControllerBinding}). */
export const CONTROLLER_INPUTS = [
  'controlChange',
  'channelPressure',
  'polyPressure',
  'pitchBend',
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
  'pitchCents',
  'vibratoDepth',
] as const;

/**
 * What a channel does with a note-on while another note on that channel is
 * still held. `'monoLegato'` carries the sounding voice and only moves its
 * pitch — a wind player's slur — and is deliberately out of reach of any MIDI
 * message: CC126 names a monophonic mode but not this one.
 */
export const ARTICULATIONS = ['poly', 'monoRetrigger', 'monoLegato'] as const;

/**
 * How a part's signal path is built. `'bank'` keeps whatever the instrument's
 * own bank/program selection resolves to, `'none'` bypasses the rig stage, and
 * `'chain'` replaces it with an explicit insert chain.
 */
export const PART_RIG_MODES = ['bank', 'none', 'chain'] as const;

/** The `part` value that addresses a destination's default instead of one part (0-15). */
export const PART_RIG_ALL_PARTS = 0xff;

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

/** Canonical NativeSynth enum vocabularies read from the native module, as arrays of names. */
export interface SynthEnumTables {
  /** Names of {@link SYNTH_ENGINE_MODES}. */
  engineModes: string[];
  /** Names of {@link SYNTH_OSC_WAVEFORMS}. */
  waveforms: string[];
  /** Names of {@link BUILTIN_SYNTH_WAVEFORMS}. */
  builtinWaveforms: string[];
  /** Names of {@link SYNTH_FILTER_MODELS}. */
  filterModels: string[];
  /** Names of {@link SYNTH_FILTER_OUTPUTS}. */
  filterOutputs: string[];
  /** Names of {@link SYNTH_BODY_TYPES}. */
  bodyTypes: string[];
  /** Names of {@link SYNTH_MOD_SOURCES}. */
  modSources: string[];
  /** Names of {@link SYNTH_MOD_DESTINATIONS}. */
  modDestinations: string[];
  /** Names of {@link CONTROLLER_INPUTS}. */
  controllerInputs: string[];
  /** Names of {@link CONTROLLER_AXES}. */
  controllerAxes: string[];
  /** Names of {@link ARTICULATIONS}. */
  articulations: string[];
  /** Names of {@link MPE_DIMENSIONS}. */
  mpeDimensions: string[];
  /** Names of {@link NOTE_TRACKINGS}. */
  noteTrackings: string[];
  /** Names of {@link SAMPLE_LOOP_MODES}. */
  sampleLoopModes: string[];
  /** Names of {@link SAMPLE_KEY_TRACKS}. */
  sampleKeyTracks: string[];
  /** Names of {@link SYNTH_RETRIGGERS}. */
  retriggers: string[];
  /** Names of {@link SYNTH_LFO_SHAPES}. */
  lfoShapes: string[];
  /** Names of {@link PART_RIG_MODES}. */
  partRigModes: string[];
  /** Names of the loop modes {@link SampleDesc.loopMode} takes, which has no `'default'`. */
  sampleDescLoopModes: string[];
}

/** NativeSynth engine selector ({@link SynthPatch}; `'default'` keeps the base patch's). */
export type SynthEngineMode = (typeof SYNTH_ENGINE_MODES)[number];

/**
 * One condition another patch field must meet, as one of the conditions of an
 * alternative in {@link SynthParamInfo.activeWhen}.
 */
export interface SynthParamCondition {
  /** The field the condition reads. */
  key: string;
  /**
   * `'engine'`: a key of the same {@link synthEngineParamInfo} table.
   * `'patch'`: a {@link SynthPatch} field.
   */
  scope: 'engine' | 'patch';
  /**
   * Comparison applied as `<field> <relation> value`: equal, not equal, greater,
   * greater-or-equal, less, less-or-equal.
   */
  relation: 'eq' | 'ne' | 'gt' | 'ge' | 'lt' | 'le';
  /**
   * A number, a boolean against a switch, or a vocabulary name against a
   * wrapper enum (`body: 'violin'`).
   */
  value: number | boolean | string;
}

/**
 * One field a synth descriptor call reports: an engine-section key from
 * {@link synthEngineParamInfo} or a {@link SynthPatch} wrapper field from
 * {@link synthPatchParamInfo}. The insert descriptor's shape, without the
 * automation columns.
 */
export interface SynthParamInfo {
  /** The key {@link SynthPatch.engineParams} (or the patch field) is spelled with. */
  name: string;
  /** `'boolean'` is a switch given as 0 or 1. */
  type: 'number' | 'boolean';
  /** True for a field taking whole numbers only. */
  integer?: boolean;
  /** Smallest accepted value; absent for a field with no closed range. */
  min?: number;
  /** Largest accepted value; absent for a field with no closed range. */
  max?: number;
  /**
   * The engine's base preset's value (engine fields) or the "keep base"
   * default (patch fields); a boolean for a switch.
   */
  default: number | boolean;
  /** Unit of the value; `'none'` for a dimensionless quantity such as a fraction or selector index. */
  unit: MasteringInsertParamUnit;
  /**
   * Present only for a field that reaches the voice under conditions on other
   * patch fields: a list of alternatives, each a list of conditions. The field
   * reaches the voice when every condition of at least one alternative holds,
   * so a field reached through several layers lists one alternative per
   * layer. What a field further needs from a performance (velocity, a
   * controller, overlapping notes), or a value past which it saturates, is not
   * stated.
   *
   * @example
   * ```ts
   * const live = (field: SynthParamInfo, holds: (c: SynthParamCondition) => boolean) =>
   *   field.activeWhen === undefined || field.activeWhen.some((all) => all.every(holds));
   * ```
   */
  activeWhen?: SynthParamCondition[][];
}

/**
 * Per-patch loop override for the sample engine (`'default'` keeps what the
 * bank recorded for the sample the zone names).
 *
 * A different set of values from {@link SampleDesc.loopMode}, which is the
 * SoundFont `sampleModes` name describing the recording itself.
 */
export type SampleLoopMode = (typeof SAMPLE_LOOP_MODES)[number];

/** Whether a sample follows the played key (`'default'` keeps the base patch's). */
export type SampleKeyTrack = (typeof SAMPLE_KEY_TRACKS)[number];

/**
 * What a note-on restarts on a {@link SynthPatch} (`'default'` keeps the base
 * patch's). `'free'` seeds each voice's start phases and random streams from
 * the voice slot and a running note count, so two plays of one note differ;
 * `'note'` seeds them from the note number alone.
 */
export type SynthRetrigger = (typeof SYNTH_RETRIGGERS)[number];

/**
 * The patch's LFO waveform (vibrato and tremolo LFOs) at every destination
 * (`'default'` keeps the base patch's). `'triangle'` is every catalog preset's
 * shape; `'sine'` carries no harmonics, so a vibrato's or a tremolo's sidebands
 * sit where a player's do. `'random'` glides to a new seeded value in [-1, 1]
 * each cycle, so it wanders with no line at the rate; it is seeded per voice, so a
 * bounce repeats it exactly.
 */
export type SynthLfoShape = (typeof SYNTH_LFO_SHAPES)[number];

/**
 * Loop behaviour recorded for one sample in a {@link SampleBank}: the three
 * SoundFont `sampleModes` states by name. There is no `'default'`: a sample's
 * own loop mode is where the default comes from.
 */
export type SampleDescLoopMode = 'none' | 'continuous' | 'keyDown';

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
  loopMode?: SampleDescLoopMode;
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

/** How a part's rig is built ({@link PART_RIG_MODES}). */
export type PartRigMode = (typeof PART_RIG_MODES)[number];

/** One stage of a `'chain'` part rig: a processor name and its parameters. */
export interface PartRigInsert {
  /** Processor name, e.g. `'saturation.overdrive'`. */
  processor: string;
  /** Processor parameters. */
  params?: Record<string, unknown>;
}

/** Selects one part rig entry of a destination; `part` is 0-15 or `PART_RIG_ALL_PARTS`. */
export interface PartRigKey {
  /** MIDI destination id whose rig is addressed. */
  destinationId: number;
  /** Part index 0-15, or {@link PART_RIG_ALL_PARTS} for the destination's default rig. */
  part: number;
}

/** Request for {@link Project.setPartRig} and {@link RealtimeEngine.setPartRig}. */
export interface PartRigRequest extends PartRigKey {
  /** How the part's rig is chosen; `'chain'` requires `inserts`. */
  mode: PartRigMode;
  /** 1-8 inserts; required for `'chain'` and refused for the other modes. */
  inserts?: PartRigInsert[];
}

/** A part rig as stored on a project ({@link Project.partRig}). */
export interface PartRigEntry {
  /** Rig mode this entry holds. */
  mode: PartRigMode;
  /** Present for `'chain'` only; `params` is the parsed parameter object. */
  inserts?: {
    /** Processor name, e.g. `'saturation.overdrive'`. */
    processor: string;
    /** Processor parameters; an empty object when none were given. */
    params: Record<string, unknown>;
  }[];
}

/** @deprecated Use {@link PartRigEntry}, the name the Node binding shares. */
export type PartRig = PartRigEntry;

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
 * canonical names; a number is a `TypeError`, and an unknown name throws rather than
 * resolving to the first member.
 *
 * @example
 * ```ts
 * engine.bindController(0, { input: 'controlChange', index: 2, axis: 'excitation' });
 * ```
 */
export interface ControllerBinding {
  /** How the device spells the gesture. */
  input: ControllerInput;
  /**
   * CC number 0-127 for `'controlChange'`. Every other input is identified by
   * its message status alone and ignores this. Default `0`.
   */
  index?: number;
  /**
   * Which expression axis the gesture means. `'none'` is refused: a binding
   * that means nothing is a caller mistake, not an empty slot.
   */
  axis: ControllerAxis;
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

/** One {@link SynthPatch} mod-matrix routing; each field is a name, and a number is a `TypeError`. */
export interface SynthModRouting {
  /**
   * Mod-matrix source. `'none'` is refused: a routing driven by nothing is a
   * caller mistake, not an empty slot.
   */
  source: SynthModSource;
  /**
   * Mod-matrix destination. `'none'` is refused, for the same reason as
   * `source`.
   */
  destination: SynthModDestination;
  /**
   * Destination units at full source deflection, summed onto whatever the
   * patch or a CC already set and then handled per destination:
   *
   * - `panUnits`: SF2 pan units, clamped to `[-500, 500]` — `1` moves the
   *   image by about 0.2% of a side.
   * - `ampGain` / `filterEnvDepth`: multiplicative. Each routing's amount
   *   composes onto a running multiplier that clamps to `[0, 4]`.
   *   `filterEnvDepth` has nothing to scale while the patch's
   *   `envToCutoffCents` is `0` — the filter envelope contributes no cents for
   *   it to multiply.
   * - `resonanceQ`: added to the patch's own Q, then floored at a fixed
   *   `0.5` (not at that patch's own Q).
   * - `lfo1RateScale`: multiplicative, clamped to `[1/16, 16]`. Audible only
   *   once something else already reads LFO1's output — vibrato depth, the
   *   filter's LFO amount, tremolo — since it retunes LFO1 rather than
   *   sounding on its own; the bare patch's built-in modWheel vibrato is the
   *   usual path.
   * - The three `excitation*` destinations and `spectrumMorph`: an offset in
   *   the engine's own normalized `[0, 1]` axis units, the same scale the
   *   live-control CCs drive, clamped to `[-1, 1]`. Reach is per engine and not
   *   uniform across the three: `excitationForce` reaches pipe organ, bowed
   *   string, reed, brass, flute and free reed; `excitationPosition` reaches
   *   bowed string only; `excitationBrightness` reaches pipe organ, reed,
   *   brass, flute, free reed and vocal. An engine outside a destination's list
   *   ignores it — its exciter is finished before the second sample renders, or
   *   it has none. `spectrumMorph` travels between the two spectral tables an
   *   additive patch carries; a patch needs an explicit `drawbars_b` distinct
   *   from `drawbars` to have a second table to travel to, and no catalog
   *   preset ships one, so the destination is currently unreachable from a
   *   preset name alone.
   *
   * Percussion reaches none of the above: a drum channel's voice runs the
   * per-note kit patch instead of this one, so the whole mod matrix is
   * discarded before the voice starts.
   */
  depth: number;
}

/**
 * Versioned NativeSynth patch for {@link ProjectBounceOptions.synthInstruments}
 * and {@link RealtimeEngine.setSynthInstrument}.
 *
 * The patch starts from a BASE — the named `preset` (see
 * {@link synthPresetNames}; a `"va:"` routing prefix is accepted) or, when
 * `preset` is omitted, the default subtractive patch. Omitting a numeric field
 * keeps the base value; supplying one overrides it, including an explicit `0` such as
 * `stereoSpread: 0`. A value that is non-finite or outside the range
 * {@link synthPatchParamInfo} publishes for its field, and more than 8
 * `modRoutings`, are refused with a `RangeError` naming the field. The enum fields
 * reserve `'default'` as keep. A `modRoutings` array REPLACES the base mod
 * matrix, and an empty array clears it, while omitting the key keeps it.
 *
 * The selected engine's own model fields (bow force, breath pressure, FM
 * operator levels, mode ratios, ...) are set through {@link engineParams}; the
 * fields below are the wrapper sections. Which engines honour each one:
 *
 * - subtractive only: `waveform`, `unison`, `detuneCents`.
 * - sample only: the `sample*` fields.
 * - every engine: `pitchOffsetCents`, the LFOs, `driftCents` / `driftRateHz`,
 *   `glideMs`, the filter and its envelope, the amplitude envelope (an
 *   undamped harpsichord tail outlasts it), `oneShot`, `body` / `bodyMix`,
 *   `drive`, `hpCutoffHz`, `sampleHoldHz` / `bitDepth`, `stereoSpread`.
 * - every engine and a percussion channel: `gain`, `polyphony`, `busDrive`. On
 *   a percussion channel (a percussion patch voicing a GM kit, as the
 *   `'drumKit'` preset does) each note runs its per-note kit patch and every
 *   other field is discarded.
 */
export interface SynthPatch {
  /** Resolve MIDI channels from incoming GM bank/program changes; defaults to false. */
  useGmPrograms?: boolean;
  /** Base preset name (see {@link synthPresetNames}); omit for the init patch. */
  preset?: string;
  /** Engine selector (see {@link SYNTH_ENGINE_MODES}). Omitted: keeps the base patch's engine. */
  engineMode?: SynthEngineMode;
  /**
   * Fields of the selected engine's own model section, by key (for example
   * `{ bowForce: 0.6 }` on `'bowedString'`). {@link synthEngineParamInfo} lists
   * the keys an engine accepts with their ranges, units and defaults. Unlike
   * the wrapper fields, nothing is clamped or ignored: an unknown key, a key of
   * another engine's section, a non-finite value, a fractional value for a
   * whole-number field or a value outside a bounded field's range throws a
   * `RangeError` naming the key. Absent keys keep the
   * engine section's base (the named preset's, or the engine's base preset's).
   * Not returned by {@link synthPresetPatch}.
   */
  engineParams?: Record<string, number>;
  /** Oscillator waveform. Read by the subtractive engine only; every other engine ignores it. */
  waveform?: SynthOscWaveform;
  /** Detuned-stack width [1, 7]. */
  unison?: number;
  /** Unison detune spread (cents), [0, 200]. Subtractive engine only. Omitted: keeps the base value. */
  detuneCents?: number;
  /** Per-voice slow pitch drift depth (cents). */
  driftCents?: number;
  /** Per-voice drift rate (Hz), [0.01, 20]. */
  driftRateHz?: number;
  /** Pre-filter drive [0, 1]. */
  drive?: number;
  /** Filter character core. Omitted: keeps the base patch's. */
  filterModel?: SynthFilterModel;
  /** Filter output (SVF only). Omitted: keeps the base patch's. */
  filterOutput?: SynthFilterOutput;
  /** Filter cutoff (Hz), [10, 22000]. Omitted: keeps the base value. */
  cutoffHz?: number;
  /**
   * Series 12 dB/oct highpass after the main filter, in Hz; 0 disables the
   * stage. The other end of a band the lowpass alone cannot make. It runs at
   * Butterworth Q -- `resonanceQ` belongs to the main filter.
   */
  hpCutoffHz?: number;
  /**
   * Rate the voice's output is held at, in Hz; 0 disables the stage. The
   * voice's own converter, ahead of its amplitude envelope: the aliased images
   * it folds down are as much of a sound as its samples are. Per voice, so a
   * patch layering several voices can convert some and leave others alone.
   *
   * Percussion never reads this: a drum channel's voice runs the per-note kit
   * patch instead of this one, so the field is discarded before the voice
   * starts.
   */
  sampleHoldHz?: number;
  /**
   * Word length the held value is quantized to, in bits; 0 disables the
   * quantizer. Fractional values are meaningful — a converter's effective
   * resolution is rarely a whole number of bits.
   */
  bitDepth?: number;
  /** Filter resonance (Q), [0.5, 30]. Omitted: keeps the base value. */
  resonanceQ?: number;
  /** Cutoff keyboard tracking [0, 1]. */
  keyTrack?: number;
  /**
   * Filter envelope's cutoff depth, in cents at full envelope; 0 leaves the
   * envelope's cutoff contribution off. The cutoff is multiplied by
   * 2^(envelope × envToCutoffCents / 1200), so the response moves by exactly
   * that interval wherever the corner sits well below Nyquist; near the top the
   * `svf` moves slightly less and the ladders slightly more. `cutoffHz` is the
   * −3 dB corner only for `svf` and `svf4Pole` at `resonanceQ` 0.707: with
   * resonance their corner sits above it, and the ladder and Sallen-Key models put their
   * resonant peak on it with the −3 dB point one to three octaves lower, so
   * measure a depth as a ratio of corners rather than against `cutoffHz`.
   */
  envToCutoffCents?: number;
  /**
   * Velocity's cutoff depth, in cents. The rendered term is
   * `velToCutoffCents * (velocity / 127 - 1)`, so velocity 127 is the anchor
   * where it is exactly zero: a positive value darkens softer notes, a
   * negative value brightens them, and both effects grow the further the
   * velocity sits below 127.
   */
  velToCutoffCents?: number;
  // --- pitch offset ---
  /**
   * Constant transposition of the voice's own pitch, in cents, within
   * [-4800, 4800] (outside it is refused); 0 leaves the pitch alone. Applied on top of the note, so it
   * shifts a whole patch without rewriting the part — a detuned layer, a sample
   * set mapped a semitone off, an instrument pitched to a reference other than
   * A440. Carried in the per-sample pitch factor every engine's render already
   * takes, so it applies the same amount on all of them — except percussion,
   * whose per-note kit patch replaces this whole section and never reads it.
   *
   * Also automatable under this same name through
   * {@link RealtimeEngine.resolveInstrumentAutomationId}; it reaches
   * already-sounding voices from the next processed block.
   */
  pitchOffsetCents?: number;
  /** Amplitude envelope delay before the attack (ms), [0, 5000]. */
  ampDelayMs?: number;
  /** Attack in ms; the level reaches full at the set time. */
  ampAttackMs?: number;
  /** Amplitude envelope hold at the peak (ms), [0, 5000]. */
  ampHoldMs?: number;
  /** Decay in ms: three time constants, about -26 dB of the remaining distance at the set time. */
  ampDecayMs?: number;
  /** Amplitude envelope sustain level, [0, 1]. Omitted: keeps the base value. */
  ampSustain?: number;
  /** Release in ms: three time constants, about -26 dB of the remaining distance at the set time. */
  ampReleaseMs?: number;
  /** Filter envelope delay before the attack (ms), [0, 5000]. */
  filterDelayMs?: number;
  /** Attack in ms; the level reaches full at the set time. */
  filterAttackMs?: number;
  /** Filter envelope hold at the peak (ms), [0, 5000]. */
  filterHoldMs?: number;
  /** Decay in ms: three time constants, about -26 dB of the remaining distance at the set time. */
  filterDecayMs?: number;
  /** Filter envelope sustain level, [0, 1]. Omitted: keeps the base value. */
  filterSustain?: number;
  /** Release in ms: three time constants, about -26 dB of the remaining distance at the set time. */
  filterReleaseMs?: number;
  /**
   * Ignore note-off: the voice ends with its envelope's decay or its engine's
   * own end, as a drum hit does.
   */
  oneShot?: boolean;
  /** Vibrato LFO rate (Hz), [0, 40]. Omitted: keeps the base value. */
  lfoRateHz?: number;
  /** The patch's LFO waveform (vibrato and tremolo LFOs) at every destination; see {@link SynthLfoShape}. */
  lfoShape?: SynthLfoShape;
  /** Hardwired vibrato depth (cents), [0, 1200]. Omitted: keeps the base value. */
  lfoToPitchCents?: number;
  /** Mod-matrix LFO2 rate (Hz), [0, 40]. Omitted: keeps the base value. */
  lfo2RateHz?: number;
  /** Portamento time (ms), [0, 5000]. Omitted: keeps the base value. */
  glideMs?: number;
  /** Body/formant resonance voicing. Omitted: keeps the base patch's. */
  body?: SynthBodyType;
  /** Body resonance mix [0, 1]. */
  bodyMix?: number;
  /** Seeded per-voice pan scatter [0, 1]. */
  stereoSpread?: number;
  /**
   * Seed source for each voice's start state. `'note'` derives oscillator start
   * phases, unison jitter, drift and every engine's noise stream from the note
   * number, so the same note played again after its tail has ended renders the
   * same samples; `'free'` (the built-in patches' choice) varies them per note.
   * State outside the voice is reset by neither: controllers, the bus DC
   * blocker, a piano's shared soundboard, a plucked string's sympathetic halo,
   * an organ's wind chest and effect tails. The `'drumKit'` preset's per-note
   * pieces keep their own mode.
   */
  retrigger?: SynthRetrigger;
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
   * offline helpers rather than part of the patch itself: it is resolved to a native handle before the patch crosses into WASM.
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
  /**
   * Overrides the loop mode the bank recorded for the sample ({@link SampleDesc.loopMode}).
   * Same three states, a different numbering: pass the names, not the numbers.
   */
  sampleLoop?: SampleLoopMode;
  /** Attack skip, as a fraction of the mapped region. */
  sampleStartOffset?: number;
  /** Whether the sample follows the played key, or sounds at its recorded pitch. */
  sampleKeyTrack?: SampleKeyTrack;
}
