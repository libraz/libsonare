/**
 * Instrument definitions a project source plays through: the built-in synth
 * patch, the SoundFont player, and the sample bank's descriptors.
 */

import type { SampleBank } from './sample_bank.js';
import type { MasteringInsertParamUnit } from './types_capabilities.js';

/** Names accepted by the minimal built-in oscillator synth. */
export const BUILTIN_SYNTH_WAVEFORMS = ['sine', 'saw', 'sawtooth', 'square', 'triangle'] as const;

/** Oscillator waveform for the {@link BuiltinSynthConfig built-in synth}. */
export type SynthWaveform = (typeof BUILTIN_SYNTH_WAVEFORMS)[number];

/**
 * Patch for the built-in minimal polyphonic oscillator synth used by
 * {@link ProjectBounceOptions.builtinInstruments}. An omitted field takes the library
 * default, so an empty object is the default sine patch; a given value is read
 * literally and refused outside its range, never clamped or replaced.
 */
export interface BuiltinSynthConfig {
  /**
   * Oscillator waveform, a {@link SynthWaveform} name (default `'sine'`). The
   * one config field with no nearest sensible value — an unknown name throws
   * rather than falling back to sine.
   *
   * @throws {RangeError} The name is not in the set.
   * @throws {TypeError} The value is not a string.
   */
  waveform?: SynthWaveform;
  /** Master output gain (linear), in (0, 4]; default 0.2. */
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
 * One dispatched MIDI event delivered to {@link ExternalInstrument.onEvent}.
 */
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
 * A JavaScript instrument driven during a {@link Project.bounce} with `instruments`.
 * Only `render` is required. Every callback runs synchronously on the calling
 * thread and must not return a Promise.
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
 * Patch for the GS-compatible SoundFont player used by
 * {@link ProjectBounceOptions.sf2Instruments} and
 * {@link RealtimeEngine.setSf2Instrument}. An omitted field takes the library
 * default; a given value is read literally and refused outside its range.
 */
export interface Sf2InstrumentConfig {
  /** Master output gain (linear), in [0, 4]; default 0.5. 0 is silence. */
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
   * How the GS insertion effects (EFX) are realised: 'modern' (the default) or
   * 'classic'. Any other value is refused.
   */
  gsEfxRealization?: 'modern' | 'classic';
}

/**
 * Every NativeSynth engine selector, by name, in the library's enum order.
 *
 * Selecting a mode blanks every engine section but its own. Every mode but
 * `'sample'` voices from an otherwise default patch: the engine section is the
 * named preset's when the preset uses the selected engine, else the engine's
 * base preset's, so a bare `'fm'` sounds the ePiano's operators and the bare
 * `'percussion'` is the GM drum kit. `'sample'` renders silence until a bank is
 * supplied through {@link SynthPatch.sampleBank}. Set the selected engine's own
 * fields with {@link SynthPatch.engineParams}; {@link synthEngineParamInfo}
 * lists them.
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

/** Device gestures a {@link ControllerBinding} can listen to. */
export const CONTROLLER_INPUTS = [
  'controlChange',
  'channelPressure',
  'polyPressure',
  'pitchBend',
  'velocity',
] as const;

/** Expression axes a {@link ControllerBinding} can drive; `'none'` is refused. */
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

/** Articulation names {@link RealtimeEngine.setArticulation} accepts: polyphonic, retriggering mono, legato mono. */
export const ARTICULATIONS = ['poly', 'monoRetrigger', 'monoLegato'] as const;

/** How a part's rig is chosen: the instrument's own bank rig, no rig, or an explicit insert chain. */
export const PART_RIG_MODES = ['bank', 'none', 'chain'] as const;

/** The `part` value that addresses a destination's default rather than one part. */
export const PART_RIG_ALL_PARTS = 0xff;

/** The three dimensions MPE carries per note, for {@link RealtimeEngine.setControllerNoteTracking}. */
export const MPE_DIMENSIONS = ['bend', 'pressure', 'timbre'] as const;

/**
 * Which note a value addressed to a whole MIDI channel belongs to when several
 * are sounding on it, for {@link RealtimeEngine.setControllerNoteTracking}.
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
 * `'default'` keeps whatever the bank recorded for that sample, which
 * {@link SampleDescLoopMode} spells without a default.
 */
export type SampleLoopMode = (typeof SAMPLE_LOOP_MODES)[number];

/** Whether a {@link SynthPatch} sample follows the played key (`'default'` keeps the base). */
export type SampleKeyTrack = (typeof SAMPLE_KEY_TRACKS)[number];

/**
 * What a note-on restarts on a {@link SynthPatch} (`'default'` keeps the base
 * patch's). `'free'` seeds each voice's start phases and random streams from
 * the voice slot and a running note count, so two plays of one note differ;
 * `'note'` seeds them from the note number alone.
 */
export type SynthRetrigger = (typeof SYNTH_RETRIGGERS)[number];

/**
 * The patch's LFO waveform (vibrato and tremolo LFOs) on a {@link SynthPatch},
 * at every destination either reaches (`'default'` keeps the base patch's).
 * `'triangle'` is every catalog preset's shape; `'sine'` carries no harmonics,
 * so a vibrato's or a tremolo's sidebands sit where a player's do. `'random'`
 * glides to a new seeded value in [-1, 1] each cycle, so it wanders with no
 * line at the rate; it is seeded per voice, so a bounce repeats it exactly.
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
  /** Fine tuning applied on top of {@link rootKey}. */
  fineTuneCents?: number;
  /** Rate the sample was recorded at; omit to play it at the render's rate. */
  sourceRate?: number;
  /** Loop start, as a frame offset inside this sample. */
  loopStart?: number;
  /** Loop end, as a frame offset inside this sample. */
  loopEnd?: number;
  /**
   * The sample's own loop behaviour, a SoundFont `sampleModes` state. A
   * loop that survives clamping empty is dropped, so a malformed loop plays
   * unlooped rather than wrapping over nothing.
   * {@link SynthPatch.sampleLoop} overrides this per patch.
   */
  loopMode?: SampleDescLoopMode;
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
 * for `pitchCents` and `vibratoDepth`. `lo > hi` inverts the gesture. `curve`
 * is the exponent applied to the normalized input before that range maps it;
 * keep the default `1` unless the device sends an unshaped gesture, since a
 * wind controller has already applied the curve its player chose.
 *
 * `axis: 'none'` is refused rather than treated as an empty slot, and a
 * `polyPressure` binding may only name one of the four excitation axes —
 * `loudness`, `pitchCents` and `vibratoDepth` are channel-level state, so a
 * per-note value applied channel-wide would look to the caller like a binding
 * that took.
 */
export interface ControllerBinding {
  /** Gesture the device sends, by name. */
  input: ControllerInput;
  /** CC number 0..127 for `'controlChange'`; every other input ignores it. */
  index?: number;
  /** Expression axis the gesture means, by name. */
  axis: ControllerAxis;
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
 * - `'monoRetrigger'` — one note at a time; a new note-on stops the previous
 *   note and starts over. This is what GS MONO MODE and CC126 mean, and it is
 *   all they can reach.
 * - `'monoLegato'` — one note at a time, carried: a new note-on re-tunes the
 *   sounding voice instead of starting one, so the exciter and the amplitude
 *   envelope never restart. This is a wind player's slur, and no MIDI message
 *   reaches it by design — reading CC126 as this one would change what a
 *   spec-compliant GS file sounds like.
 *
 * `'monoLegato'` is a request, not a guarantee: an engine whose exciter is
 * spent at the onset — anything struck or plucked — and a target pitch below
 * what the engine's delay line holds both fall back to an ordinary note, which
 * {@link RealtimeEngine.legatoFallbackCount} counts.
 */
export type Articulation = (typeof ARTICULATIONS)[number];

/** One per-note MPE dimension ({@link MPE_DIMENSIONS}). */
export type MpeDimension = (typeof MPE_DIMENSIONS)[number];

/** One note-attribution rule ({@link NOTE_TRACKINGS}). */
export type NoteTracking = (typeof NOTE_TRACKINGS)[number];

/**
 * Versioned NativeSynth patch for {@link ProjectBounceOptions.synthInstruments} and
 * {@link RealtimeEngine.setSynthInstrument}.
 *
 * The patch starts from a BASE — the named `preset` (see
 * {@link synthPresetNames}; a `"va:"` routing prefix is accepted) or, when
 * `preset` is omitted, the default subtractive patch. Omitting a numeric field
 * keeps the base value; supplying one overrides it, including an explicit `0` such as
 * `stereoSpread: 0`. A value that is non-finite or outside the range
 * {@link synthPatchParamInfo} publishes for its field, and more than 8
 * `modRoutings`, are refused with a coded `SonareError` naming the field. The enum fields
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
  /**
   * Follow incoming GM bank/program changes and route channel 10 through the
   * GM drum map, for offline project bounces ({@link ProjectBounceOptions.synthInstruments})
   * and for {@link RealtimeEngine.setSynthInstrument} alike. Defaults to
   * `false`, preserving the fixed-patch behavior. This is a binding option,
   * not a NativeSynth patch field.
   */
  useGmPrograms?: boolean;
  /**
   * Sample bank an `engineMode: 'sample'` patch reads, for offline project
   * bounces and for {@link RealtimeEngine.setSynthInstrument} alike. Borrowed
   * for the call: it must outlive the bounce, and adding to it while the
   * bounce runs is not allowed. A sample patch bound without a bank renders
   * silence rather than failing, the same way a patch naming a keymap set the
   * bank lacks does. This is a binding option, not a NativeSynth patch field.
   */
  sampleBank?: SampleBank;
  /** Base preset name (see {@link synthPresetNames}); omit for the init patch. */
  preset?: string;
  /** Engine selector (see {@link SYNTH_ENGINE_MODES}). Omitted: keeps the base patch's engine. */
  engineMode?: SynthEngineMode;
  /**
   * Fields of the selected engine's own model section by key (`bowForce`,
   * `ops1.level`, ...), as listed by {@link synthEngineParamInfo}. Applied over
   * the engine section the patch resolves to (see {@link SYNTH_ENGINE_MODES}).
   * Unlike mastering insert params, an unknown key, a key of another engine, a
   * non-finite value, a fractional value for an integer field and a value outside
   * a field's range are refused with a coded `SonareError` naming the key;
   * nothing is clamped. Write-only: a patch read back (for example from
   * {@link synthPresetPatch}) carries no `engineParams`.
   */
  engineParams?: Record<string, number>;
  // --- oscillator section (subtractive mode) ---
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
  // --- filter section ---
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
  // --- envelopes (ms / sustain in [0, 1]) ---
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
  // --- LFOs / glide ---
  /** Vibrato LFO rate (Hz), [0, 40]. Omitted: keeps the base value. */
  lfoRateHz?: number;
  /** Hardwired vibrato depth (cents), [0, 1200]. Omitted: keeps the base value. */
  lfoToPitchCents?: number;
  /** The patch's LFO waveform (vibrato and tremolo LFOs), as a {@link SynthLfoShape} name. */
  lfoShape?: SynthLfoShape;
  /** Mod-matrix LFO2 rate (Hz), [0, 40]. Omitted: keeps the base value. */
  lfo2RateHz?: number;
  /** Portamento time (ms), [0, 5000]. Omitted: keeps the base value. */
  glideMs?: number;
  // --- realism polish ---
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
  sampleLoop?: SampleLoopMode;
  /** Attack skip, as a fraction of the sampled region [0, 1]. */
  sampleStartOffset?: number;
  /** Whether the sample follows the played key, or sounds at its recorded pitch. */
  sampleKeyTrack?: SampleKeyTrack;
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

/** A part-rig mode ({@link PART_RIG_MODES}). */
export type PartRigMode = (typeof PART_RIG_MODES)[number];

/** One insert of a `'chain'` part rig. Omitted `params` uses an empty JSON object. */
export interface PartRigInsert {
  /** Processor name, e.g. `'saturation.overdrive'`. */
  processor: string;
  /** Processor parameters. */
  params?: Record<string, unknown>;
}

/** Selects one part rig entry of a destination; `part` is 0-15 or {@link PART_RIG_ALL_PARTS}. */
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

/** A part rig entry read back by {@link Project.getPartRig}; `inserts` is set only for `'chain'`. */
export interface PartRigEntry {
  /** Rig mode this entry holds. */
  mode: PartRigMode;
  /** Inserts of a `'chain'` entry, in processing order; absent for the other modes. */
  inserts?: {
    /** Processor name, e.g. `'saturation.overdrive'`. */
    processor: string;
    /** Processor parameters; an empty object when none were given. */
    params: Record<string, unknown>;
  }[];
}

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
 * One field descriptor from {@link synthEngineParamInfo} or
 * {@link synthPatchParamInfo}, in the field names of
 * {@link MasteringInsertParamInfo} (`min` / `max` are absent, not null, for an
 * unbounded field).
 */
export interface SynthParamInfo {
  /** The key {@link SynthPatch.engineParams} takes, or the {@link SynthPatch} field name. */
  name: string;
  /** `'boolean'` is a switch given as 0 or 1. */
  type: 'number' | 'boolean';
  /** True only for a field taking whole numbers. */
  integer?: boolean;
  /** Smallest accepted value; absent for a field with no closed range. */
  min?: number;
  /** Largest accepted value; absent for a field with no closed range. */
  max?: number;
  /**
   * The engine's base preset's value (engine fields) or the "keep base"
   * default (wrapper fields); a boolean for a switch.
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

/** One enum field descriptor from {@link synthPatchParamInfo}. */
export interface SynthPatchEnumParamInfo {
  /** The {@link SynthPatch} field name. */
  name: string;
  /** Enum vocabulary and its default token. */
  type: 'enum';
  /** Accepted tokens in C ordinal order. */
  values: string[];
  /** The ordinal-0 token. */
  default: string;
}

/** A numeric, boolean, or enum descriptor from {@link synthPatchParamInfo}. */
export type SynthPatchParamInfo = SynthParamInfo | SynthPatchEnumParamInfo;
