/**
 * Instrument definitions a project source plays through: the built-in synth
 * patch, the SoundFont player, and the sample bank's descriptors.
 */

import type { SampleBank } from './sample_bank.js';

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
  /**
   * Oscillator waveform: a {@link SynthWaveform} name or numeric enum (0=sine).
   * The one config field with no nearest sensible value — a name or an ordinal
   * outside the set throws rather than falling back to sine.
   *
   * @throws {RangeError} The value is a string or a number the resolver cannot
   * resolve: an unknown name, an ordinal outside the enum, or a number outside
   * the signed 32-bit range (which would otherwise wrap onto a valid ordinal).
   * @throws {TypeError} The value is neither a string nor a number.
   */
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
  /**
   * Render the instrument alone, without the stage the bank binds after its
   * voice — the amplifier and cabinet an electric guitar is never heard
   * without. Defaults to false, so a MIDI file that selects a distorted guitar
   * and asks for nothing else still comes out amplified. Set it to get the
   * direct signal, which is what a voice is calibrated against.
   */
  clearBankRig?: boolean;
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

export const CONTROLLER_INPUTS = [
  'control-change',
  'channel-pressure',
  'poly-pressure',
  'pitch-bend',
  'velocity',
] as const;

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

export const ARTICULATIONS = ['poly', 'mono-retrigger', 'mono-legato'] as const;

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

/**
 * Loop behaviour a {@link SynthPatch} forces on the sample it plays.
 * `'default'` keeps whatever the bank recorded for that sample.
 *
 * The NUMBERS here are not the ones {@link SampleDescLoopMode} uses. This is
 * the patch's own scale, `0` default / `1` none / `2` continuous / `3` key-down;
 * the sample's own loop mode is SoundFont `sampleModes`, where continuous is
 * `1` and there is no default. Use the names on both and the two cannot be
 * confused; a number read for the wrong one turns "continuous" into "no loop".
 */
export type SampleLoopMode = (typeof SAMPLE_LOOP_MODES)[number];

/** Whether a {@link SynthPatch} sample follows the played key (`'default'` keeps the base). */
export type SampleKeyTrack = (typeof SAMPLE_KEY_TRACKS)[number];

/**
 * Loop behaviour recorded for one sample in a {@link SampleBank}.
 *
 * A number is the raw SoundFont `sampleModes` value the C struct carries
 * (`0` no loop, `1` continuous, `3` while the key is held), so SF2-derived data
 * passes through untranslated; the names above are the readable spellings of
 * the same three states. There is no `'default'`: a sample's own loop mode is
 * where the default comes from.
 *
 * {@link SampleLoopMode}, the patch-side override, spells the same three states
 * with the same names but numbers them differently (`2` is continuous there).
 * Prefer the names — a number read for the wrong scale turns "continuous" into
 * "no loop" silently.
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
  /** Fine tuning applied on top of {@link rootKey}. */
  fineTuneCents?: number;
  /** Rate the sample was recorded at; omit to play it at the render's rate. */
  sourceRate?: number;
  /** Loop start, as a frame offset inside this sample. */
  loopStart?: number;
  /** Loop end, as a frame offset inside this sample. */
  loopEnd?: number;
  /**
   * The sample's own loop behaviour, on the SoundFont `sampleModes` scale. A
   * loop that survives clamping empty is dropped, so a malformed loop plays
   * unlooped rather than wrapping over nothing.
   *
   * {@link SynthPatch.sampleLoop} overrides this per patch and numbers the same
   * states differently, so pass the names rather than the numbers between them.
   */
  loopMode?: SampleDescLoopMode | number;
}

/**
 * One key/velocity rectangle mapped onto a sample ({@link SampleBank.addZone}).
 *
 * Every bound defaults ON ITS OWN, so narrowing one edge never collapses
 * another into an empty range: `{ keyLo: 48 }` is 48..127 at every velocity,
 * `{ velLo: 64 }` is the whole keyboard at 64..127, and an empty descriptor is
 * the whole keyboard. The one rectangle this cannot express is a zone covering
 * nothing but key 0.
 */
export interface SampleZoneDesc {
  /**
   * Keymap set the zone joins; a {@link SynthPatch} names a set through
   * {@link SynthPatch.sampleSet}. Sets below it are created. Defaults to `0`.
   */
  setIndex?: number;
  /** Sample the zone plays, as returned by {@link SampleBank.addSample}. */
  sampleIndex?: number;
  /** Lowest key of the rectangle. Defaults to `0`, the lowest key. */
  keyLo?: number;
  /** Highest key of the rectangle. Defaults to `127`. */
  keyHi?: number;
  /** Lowest velocity of the rectangle. Defaults to `1`, since velocity 0 is a note-off. */
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

/** {@link SynthPatch} mod-matrix source. */
export type SynthModSource = (typeof SYNTH_MOD_SOURCES)[number];

/** {@link SynthPatch} mod-matrix destination. */
export type SynthModDestination = (typeof SYNTH_MOD_DESTINATIONS)[number];

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

/** Device gesture a {@link ControllerBinding} listens to. */
export type ControllerInput = (typeof CONTROLLER_INPUTS)[number];

/** Expression axis a {@link ControllerBinding} drives. */
export type ControllerAxis = (typeof CONTROLLER_AXES)[number];

/**
 * One device gesture mapped onto one expression axis, for
 * {@link RealtimeEngine.bindController}.
 *
 * Binding the same `input` twice with different axes is how a single gesture
 * reaches both — a breath controller driving excitation and loudness together.
 *
 * `lo` / `hi` are the axis value at zero and at full deflection, in the axis's
 * own unit: normalized `[0, 1]` for the excitation axes and `loudness`, cents
 * for `pitch-cents` and `vibrato-depth`. `lo > hi` inverts the gesture. `curve`
 * is the exponent applied to the normalized input before that range maps it;
 * keep the default `1` unless the device sends an unshaped gesture, since a
 * wind controller has already applied the curve its player chose.
 *
 * `axis: 'none'` is refused rather than treated as an empty slot, and a
 * `poly-pressure` binding may only name one of the four excitation axes —
 * `loudness`, `pitch-cents` and `vibrato-depth` are channel-level state, so a
 * per-note value applied channel-wide would look to the caller like a binding
 * that took.
 */
export interface ControllerBinding {
  /** Gesture the device sends (name or C ordinal). */
  input: ControllerInput | number;
  /** CC number 0..127 for `'control-change'`; every other input ignores it. */
  index?: number;
  /** Expression axis the gesture means (name or C ordinal). */
  axis: ControllerAxis | number;
  /** Axis value at zero deflection. Default 0. */
  lo?: number;
  /** Axis value at full deflection. Default 1. */
  hi?: number;
  /** Positive exponent shaping the normalized input. Default 1 (linear). */
  curve?: number;
}

/**
 * What one MIDI channel of an instrument does with a note-on while another note
 * on that channel is still held, for {@link RealtimeEngine.setArticulation}.
 *
 * - `'poly'` — every note-on takes its own voice.
 * - `'mono-retrigger'` — one note at a time; a new note-on stops the previous
 *   note and starts over. This is what GS MONO MODE and CC126 mean, and it is
 *   all they can reach.
 * - `'mono-legato'` — one note at a time, carried: a new note-on re-tunes the
 *   sounding voice instead of starting one, so the exciter and the amplitude
 *   envelope never restart. This is a wind player's slur, and no MIDI message
 *   reaches it by design — reading CC126 as this one would change what a
 *   spec-compliant GS file sounds like.
 *
 * `'mono-legato'` is a request, not a guarantee: an engine whose exciter is
 * spent at the onset — anything struck or plucked — and a target pitch below
 * what the engine's delay line holds both fall back to an ordinary note, which
 * {@link RealtimeEngine.legatoFallbackCount} counts.
 */
export type Articulation = (typeof ARTICULATIONS)[number];

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
  /**
   * Sample bank an `engineMode: 'sample'` patch reads, for offline project
   * bounces. Borrowed for the call: it must outlive the bounce, and adding to
   * it while the bounce runs is not allowed. A sample patch bound without a
   * bank renders silence rather than failing, the same way a patch naming a
   * keymap set the bank lacks does. This is a project-bounce binding option,
   * not a NativeSynth patch field.
   */
  sampleBank?: SampleBank;
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
  // --- sample engine (read only when the resolved engine is 'sample') ---
  /**
   * Keymap set in the bound {@link sampleBank}; negative selects none. Unlike
   * every other numeric field this one has no "keep the base" sentinel, because
   * only a sample patch reads the block — which is what keeps set `0`
   * addressable.
   */
  sampleSet?: number;
  /** Linear gain on the sample. */
  sampleLevel?: number;
  /**
   * Overrides the loop mode the bank recorded for the sample
   * ({@link SampleDesc.loopMode}). Same three states, a different numbering:
   * pass the names, not the numbers.
   */
  sampleLoop?: SampleLoopMode | number;
  /** Attack skip, as a fraction of the sampled region [0, 1]. */
  sampleStartOffset?: number;
  /** Whether the sample follows the played key, or sounds at its recorded pitch. */
  sampleKeyTrack?: SampleKeyTrack | number;
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
