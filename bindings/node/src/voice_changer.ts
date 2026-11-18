import { addon } from './native.js';
import type {
  RealtimeVoiceChangerConfig,
  RealtimeVoiceChangerConfigInput,
  RealtimeVoiceChangerOptions,
  VoicePresetId,
} from './types.js';
import type { ValidateOptions } from './validation.js';
import {
  assertAudioChannels,
  assertAudioInput,
  assertPositiveInteger,
  assertSampleRate,
  requestOnly,
  sampleOneShot,
} from './validation.js';

// Native still spells the preset ids in kebab case; same order as the native ordinals.
const PRESET_IDS: readonly VoicePresetId[] = [
  'neutralMonitor',
  'brightIdol',
  'softWhisper',
  'deepNarrator',
  'robotMascot',
  'darkVillain',
  'hardTune',
  'robotMonotone',
  'choirHarmony',
  'vocoderStrings',
  'megaphone',
  'radioVoice',
];
const NATIVE_PRESET_IDS: readonly string[] = [
  'neutral-monitor',
  'bright-idol',
  'soft-whisper',
  'deep-narrator',
  'robot-mascot',
  'dark-villain',
  'hard-tune',
  'robot-monotone',
  'choir-harmony',
  'vocoder-strings',
  'megaphone',
  'radio-voice',
];

function presetOrdinal(fnName: string, id: unknown, argName = 'preset'): number {
  if (typeof id !== 'string') {
    throw new TypeError(`${fnName}: ${argName} must be a string`);
  }
  const ordinal = PRESET_IDS.indexOf(id as VoicePresetId);
  if (ordinal < 0) {
    throw new RangeError(`${fnName}: ${argName} must be one of ${PRESET_IDS.join(', ')}`);
  }
  return ordinal;
}

function presetIdToNative(fnName: string, id: unknown, argName = 'preset'): string {
  return NATIVE_PRESET_IDS[presetOrdinal(fnName, id, argName)] as string;
}

// Native still spells these fields the old way; the map runs in both directions.
const NATIVE_KEY: Readonly<Record<string, string>> = {
  voiceEngine: 'voiceEngineEngine',
  reverbVariation: 'reverbVariationVariation',
  eqHighPassHz: 'eqHighpassHz',
  retunePitchSemitones: 'retuneSemitones',
  retuneGrainSizeSamples: 'retuneGrainSize',
};
const PUBLIC_KEY: Readonly<Record<string, string>> = Object.fromEntries(
  Object.entries(NATIVE_KEY).map(([publicKey, nativeKey]) => [nativeKey, publicKey]),
);

// Native-only spellings: a config carrying one is refused, not forwarded.
const RETIRED_KEYS: ReadonlySet<string> = new Set([
  ...Object.values(NATIVE_KEY),
  'harmonyStep1',
  'harmonyStep2',
  'harmonyStep3',
]);

// Native still spells these tokens in snake case; keyed by the public field name.
const NATIVE_TOKENS: Readonly<Record<string, Readonly<Record<string, string>>>> = {
  pitchCorrectionScale: { major: 'major', naturalMinor: 'natural_minor', chromatic: 'chromatic' },
  characterType: {
    none: 'none',
    megaphone: 'megaphone',
    radio: 'radio',
    bbdChorus: 'bbd_chorus',
    strobo: 'strobo',
  },
  reverbVariation: {
    reverb: 'reverb',
    echo: 'echo',
    tempoDelay: 'tempo_delay',
    dubEcho: 'dub_echo',
  },
  reverbVariationDivision: {
    quarter: 'quarter',
    eighth: 'eighth',
    dottedEighth: 'dotted_eighth',
    sixteenth: 'sixteenth',
  },
};

function isRecord(value: unknown): value is Record<string, unknown> {
  return typeof value === 'object' && value !== null && !Array.isArray(value);
}

/** True for the flat config object (as opposed to a nested preset document). */
function isFlatConfig(config: unknown): config is Record<string, unknown> {
  return isRecord(config) && ('retunePitchSemitones' in config || 'retuneSemitones' in config);
}

function mapToken(fnName: string, field: string, value: unknown, toNative: boolean): string {
  const publicTable = NATIVE_TOKENS[field] as Readonly<Record<string, string>>;
  const table: Record<string, string> = toNative
    ? { ...publicTable }
    : Object.fromEntries(Object.entries(publicTable).map(([k, v]) => [v, k]));
  if (typeof value !== 'string') {
    throw new TypeError(`${fnName}: ${field} must be a string`);
  }
  if (!Object.hasOwn(table, value)) {
    throw new RangeError(
      `${fnName}: ${field} must be one of ${Object.keys(publicTable).join(', ')}`,
    );
  }
  return table[value] as string;
}

let flatReference: RealtimeVoiceChangerConfig | undefined;

/** Every flat-config field with a value of its type; the fields do not vary by preset. */
function flatConfigReference(): RealtimeVoiceChangerConfig {
  flatReference ??= realtimeVoiceChangerPresetConfig('neutralMonitor');
  return flatReference;
}

/**
 * Refuses a flat config missing a field or carrying one of the wrong JS type, naming the public
 * field before native sees it: native reports its own spelling of the key.
 */
function assertFlatConfigFields(fnName: string, config: Record<string, unknown>): void {
  for (const [key, sample] of Object.entries(flatConfigReference())) {
    const value = config[key];
    if (value === undefined) {
      throw new TypeError(`${fnName}: ${key} is required`);
    }
    if (key !== 'harmonySteps' && typeof value !== typeof sample) {
      throw new TypeError(`${fnName}: ${key} must be a ${typeof sample}`);
    }
  }
}

/**
 * The flat config spelt the way the addon reads it. Every field is required, as in the native
 * parser.
 *
 * @throws TypeError for a missing or wrong-typed field or a
 *   native-only key; RangeError for an unknown token or a `harmonySteps` of another length.
 */
function nativePodConfig(fnName: string, config: Record<string, unknown>): Record<string, unknown> {
  assertFlatConfigFields(fnName, config);
  const out: Record<string, unknown> = {};
  for (const [key, value] of Object.entries(config)) {
    if (RETIRED_KEYS.has(key)) {
      throw new TypeError(`${fnName}: unknown config key '${key}'`);
    }
    if (key === 'harmonySteps' || value === undefined) {
      continue;
    }
    out[NATIVE_KEY[key] ?? key] = Object.hasOwn(NATIVE_TOKENS, key)
      ? mapToken(fnName, key, value, true)
      : value;
  }
  const steps = config.harmonySteps;
  if (steps === undefined) {
    throw new TypeError(`${fnName}: harmonySteps is required`);
  }
  if (!Array.isArray(steps)) {
    throw new TypeError(`${fnName}: harmonySteps must be an array of three numbers`);
  }
  if (steps.length !== 3) {
    throw new RangeError(`${fnName}: harmonySteps must hold exactly three numbers`);
  }
  [out.harmonyStep1, out.harmonyStep2, out.harmonyStep3] = steps;
  return out;
}

/** Inverse of {@link nativePodConfig}. */
function publicPodConfig(
  fnName: string,
  config: Record<string, unknown>,
): RealtimeVoiceChangerConfig {
  const out: Record<string, unknown> = {};
  for (const [key, value] of Object.entries(config)) {
    if (/^harmonyStep[123]$/.test(key)) {
      continue;
    }
    const field = PUBLIC_KEY[key] ?? key;
    out[field] = Object.hasOwn(NATIVE_TOKENS, field)
      ? mapToken(fnName, field, value, false)
      : value;
  }
  out.harmonySteps = [config.harmonyStep1, config.harmonyStep2, config.harmonyStep3];
  return out as unknown as RealtimeVoiceChangerConfig;
}

/**
 * A voice-changer config in the form the addon reads: a preset id becomes its native id, the
 * flat config its native spelling, and a nested preset document passes through.
 *
 * @throws TypeError for any other type; RangeError for an unknown preset id.
 */
function nativeVoiceConfig(
  fnName: string,
  config: RealtimeVoiceChangerConfigInput,
): Record<string, unknown> | string {
  if (typeof config === 'string') {
    return presetIdToNative(fnName, config, 'config');
  }
  if (isFlatConfig(config)) {
    return nativePodConfig(fnName, config);
  }
  if (!isRecord(config)) {
    throw new TypeError(`${fnName}: config must be a preset id or an object`);
  }
  return config;
}

function assertChannelCount(fnName: string, numChannels: unknown): asserts numChannels is 1 | 2 {
  if (typeof numChannels !== 'number') {
    throw new TypeError(`${fnName}: numChannels must be 1 or 2`);
  }
  if (numChannels !== 1 && numChannels !== 2) {
    throw new RangeError(`${fnName}: numChannels must be 1 or 2`);
  }
}

/** Checks a planar channel list (1 or 2 equal-length planes) and the optional carrier. */
function assertPlanes(fnName: string, channels: unknown, carrier?: unknown): Float32Array[] {
  if (!Array.isArray(channels)) {
    throw new TypeError(`${fnName}: channels must be an array of Float32Array`);
  }
  assertChannelCount(fnName, channels.length);
  channels.forEach((plane, i) => {
    if (!(plane instanceof Float32Array)) {
      throw new TypeError(`${fnName}: channels[${i}] must be a Float32Array`);
    }
    if (plane.length !== channels[0].length) {
      throw new RangeError(`${fnName}: channels must have equal lengths`);
    }
  });
  if (carrier !== undefined && !(carrier instanceof Float32Array)) {
    throw new TypeError(`${fnName}: carrier must be a Float32Array`);
  }
  return channels as Float32Array[];
}

function interleavePlanes(planes: readonly Float32Array[]): Float32Array {
  const frames = planes[0]?.length ?? 0;
  const out = new Float32Array(frames * planes.length);
  planes.forEach((plane, ch) => {
    for (let i = 0; i < frames; i++) {
      out[i * planes.length + ch] = plane[i] as number;
    }
  });
  return out;
}

function deinterleavePlanes(data: Float32Array, numChannels: number): Float32Array[] {
  const frames = data.length / numChannels;
  const planes = Array.from({ length: numChannels }, () => new Float32Array(frames));
  for (let ch = 0; ch < numChannels; ch++) {
    const plane = planes[ch] as Float32Array;
    for (let i = 0; i < frames; i++) {
      plane[i] = data[i * numChannels + ch] as number;
    }
  }
  return planes;
}

const CONSTRUCTOR_KEYS: readonly string[] = ['sampleRate', 'maxBlockSize', 'numChannels', 'config'];

export class RealtimeVoiceChanger {
  private native: InstanceType<typeof addon.RealtimeVoiceChanger>;
  private disposed = false;

  /**
   * Creates a voice changer prepared for `options.sampleRate`.
   *
   * @throws TypeError for a wrong-typed or unknown option, or a missing `sampleRate`;
   *   RangeError for an out-of-domain `sampleRate`, `maxBlockSize` (explicit 0 included),
   *   `numChannels` or an unknown preset id or token.
   */
  constructor(options: RealtimeVoiceChangerOptions) {
    const fnName = 'RealtimeVoiceChanger';
    if (typeof options !== 'object' || options === null || Array.isArray(options)) {
      throw new TypeError(`${fnName}: options must be an object with a sampleRate`);
    }
    for (const key of Object.keys(options)) {
      if (!CONSTRUCTOR_KEYS.includes(key)) {
        throw new TypeError(`${fnName}: unknown option '${key}'`);
      }
    }
    assertSampleRate(fnName, options.sampleRate);
    const maxBlockSize = options.maxBlockSize ?? 128;
    if (typeof maxBlockSize !== 'number') {
      throw new TypeError(`${fnName}: maxBlockSize must be a number`);
    }
    assertPositiveInteger(fnName, maxBlockSize, 'maxBlockSize');
    const numChannels = options.numChannels ?? 1;
    assertChannelCount(fnName, numChannels);
    this.native = new addon.RealtimeVoiceChanger(
      nativeVoiceConfig(fnName, options.config ?? 'neutralMonitor'),
    );
    try {
      this.native.prepare(options.sampleRate, maxBlockSize, numChannels);
    } catch (error) {
      // The handle is already allocated; a throwing constructor leaves `this`
      // unreachable, so release it here rather than waiting for GC.
      this.native.destroy();
      this.disposed = true;
      throw error;
    }
  }

  reset(): void {
    this.native.reset();
  }

  /**
   * Replaces the configuration.
   *
   * @throws TypeError for a wrong-typed config; RangeError for an unknown preset id or token or
   *   a field outside its range (nothing is clamped); the previous configuration stays in effect.
   */
  setConfig(config: RealtimeVoiceChangerConfigInput): void {
    this.native.setConfig(nativeVoiceConfig('RealtimeVoiceChanger.setConfig', config));
  }

  configJson(): string {
    return this.native.configJson();
  }

  /** Latency of the chain in samples. */
  get latencySamples(): number {
    return this.native.latencySamples();
  }

  /**
   * Blocks in which the chain discarded its own state because a
   * non-finite value had reached it.
   *
   * Advisory telemetry, and the only thing that separates a degraded stream
   * from a clean one. Every stage of this chain leaves an in-domain finite
   * value where a non-finite one was, so the output stays finite, in range
   * and free of any error while carrying samples unrelated to the input. A
   * non-zero count is what says the samples in between were not computed from
   * what you supplied.
   *
   * Monotonic for the lifetime of the instance. The unit is one processed
   * block, never a channel.
   *
   * @example
   * ```ts
   * changer.processPlanar([left, right]);
   * if (changer.nonFiniteDiscardCount > 0) {
   *   // the audio just produced is not a function of the input
   * }
   * ```
   */
  get nonFiniteDiscardCount(): number {
    return this.native.nonFiniteDiscardCount();
  }

  processMono(input: Float32Array): Float32Array {
    return this.native.processMono(input);
  }

  /**
   * Like {@link processMono}, with the external vocoder carrier (`vocoderCarrierSource:
   * 'external'`). `carrier` must be as long as `input`; it is read only while the vocoder is on
   * with an external carrier and ignored otherwise. The carrier-less process methods throw a
   * `SonareError` (`InvalidState`) while an external carrier is configured.
   */
  processMonoWithCarrier(input: Float32Array, carrier: Float32Array): Float32Array {
    return this.native.processMonoWithCarrier(input, carrier);
  }

  processMonoWithCarrierInto(
    input: Float32Array,
    carrier: Float32Array,
    output: Float32Array,
  ): void {
    this.native.processMonoWithCarrierInto(input, carrier, output);
  }

  processMonoInto(input: Float32Array, output: Float32Array): void {
    this.native.processMonoInto(input, output);
  }

  /**
   * Processes planar blocks (one `Float32Array` per channel, 1 or 2, equal lengths) and returns
   * the processed planes without touching the inputs.
   *
   * @throws TypeError for a wrong-typed argument; RangeError for another channel count or
   *   unequal lengths; SonareError `InvalidState` when two channels are given to a changer
   *   prepared for one.
   */
  processPlanar(channels: Float32Array[]): Float32Array[] {
    const planes = assertPlanes('RealtimeVoiceChanger.processPlanar', channels);
    if (planes.length === 1) {
      return [this.native.processMono(planes[0] as Float32Array)];
    }
    const left = Float32Array.from(planes[0] as Float32Array);
    const right = Float32Array.from(planes[1] as Float32Array);
    this.native.processPlanarStereo(left, right);
    return [left, right];
  }

  /**
   * {@link processPlanar} with one mono external vocoder `carrier` of one sample per frame,
   * shared by every channel.
   */
  processPlanarWithCarrier(channels: Float32Array[], carrier: Float32Array): Float32Array[] {
    const planes = assertPlanes('RealtimeVoiceChanger.processPlanarWithCarrier', channels, carrier);
    if (planes.length === 1) {
      return [this.native.processMonoWithCarrier(planes[0] as Float32Array, carrier)];
    }
    const left = Float32Array.from(planes[0] as Float32Array);
    const right = Float32Array.from(planes[1] as Float32Array);
    this.native.processPlanarStereoWithCarrier(left, right, carrier);
    return [left, right];
  }

  /**
   * Realtime hot path: processes an interleaved block (L0,R0,L1,R1,...) into `output`, which has
   * the length of `input`.
   *
   * @throws RangeError unless `numChannels` is 1 or 2.
   */
  processInterleavedInto(input: Float32Array, numChannels: 1 | 2, output: Float32Array): void {
    assertChannelCount('RealtimeVoiceChanger.processInterleavedInto', numChannels);
    this.native.processInterleavedInto(input, numChannels, output);
  }

  /** {@link processInterleavedInto} with a mono carrier of one sample per frame shared by every channel. */
  processInterleavedWithCarrierInto(
    input: Float32Array,
    carrier: Float32Array,
    numChannels: 1 | 2,
    output: Float32Array,
  ): void {
    assertChannelCount('RealtimeVoiceChanger.processInterleavedWithCarrierInto', numChannels);
    this.native.processInterleavedWithCarrierInto(input, carrier, numChannels, output);
  }

  /**
   * Holds MIDI note `note` from the start of the next processed block. Held notes become the
   * voices' pitch targets whatever the pitch correction mode, the newest leading, up to
   * `1 + harmonyVoices` of them. Holding a held note again refreshes it.
   *
   * @param note MIDI note, an integer in [0, 127] (`RangeError` otherwise).
   * @param velocity Validated only, in (0, 1], default 1 (`RangeError` for 0, NaN or above 1; release
   *   a note with {@link noteOff}). It does not change the sound.
   * @throws SonareError `InvalidState` on a grain-engine changer, a destroyed one, or when the
   *   note queue is full (the event is dropped).
   */
  noteOn(note: number, velocity = 1.0): void {
    this.native.noteOn(note, velocity);
  }

  /** Releases MIDI note `note` from the start of the next block; an unheld note is a no-op. */
  noteOff(note: number): void {
    this.native.noteOff(note);
  }

  /** Releases every held note, including one kept by `midiRelease: 'hold'`. Always succeeds. */
  allNotesOff(): void {
    this.native.allNotesOff();
  }

  destroy(): void {
    if (this.disposed) {
      return;
    }
    this.disposed = true;
    this.native.destroy();
  }

  /**

   * Releases native resources; lets `using` free them automatically (needs TypeScript 5.2+

   * or a runtime with native explicit resource management; Node 22 does not parse `using`).

   */
  [Symbol.dispose](): void {
    this.destroy();
  }
}

/** How `formantFactor` relates to the pitch shift in {@link voiceChange}. */
export type FormantMode = 'relative' | 'absolute';

/** Options for {@link voiceChange}. All fields are optional. */
export interface VoiceChangeOptions extends ValidateOptions {
  /** Pitch shift in semitones (negative = down), a finite number. Default 0. */
  pitchSemitones?: number;
  /** Formant scale factor (>1 brightens, <1 darkens), a finite number. Default 1. */
  formantFactor?: number;
  /**
   * How `formantFactor` relates to the pitch shift. Default `'relative'`.
   *
   * - `'relative'`: `formantFactor` is the warp applied after the pitch shift, so the
   *   formants end up at `formantFactor * 2^(pitchSemitones / 12)` of the input's.
   * - `'absolute'`: `formantFactor` is the formant shift relative to the input (1 keeps
   *   the formants where they were); the warp applied is
   *   `formantFactor / 2^(pitchSemitones / 12)`. A request whose warp falls outside
   *   [0.55, 1.65] throws a `RangeError` naming the reachable `formantFactor` range for
   *   the given `pitchSemitones`; it is never clamped.
   */
  formantMode?: FormantMode;
}

/** Inputs for the one-shot {@link voiceChange} facade. */
export interface VoiceChangeRequest extends VoiceChangeOptions {
  samples: Float32Array;
  /** Sample rate in Hz, an integer in [8000, 384000]. Required. */
  sampleRate: number;
}

/**
 * Applies a voice change by shifting pitch and formants independently.
 *
 * @throws TypeError for a wrong-typed argument, a missing `sampleRate` or an unknown key;
 *   RangeError for out-of-domain audio, rate or setting.
 */
export function voiceChange(request: VoiceChangeRequest): Float32Array;
export function voiceChange(
  samples: Float32Array,
  sampleRate: number,
  options?: VoiceChangeOptions,
): Float32Array;
export function voiceChange(...args: unknown[]): Float32Array {
  const request = sampleOneShot<VoiceChangeRequest>('voiceChange', args);
  assertAudioInput('voiceChange', request.samples, request.sampleRate, request);
  return addon.voiceChange(
    request.samples,
    request.sampleRate,
    request.pitchSemitones ?? 0.0,
    request.formantFactor ?? 1.0,
    request.formantMode ?? 'relative',
  );
}

/** Inputs for the one-shot {@link voiceChangeRealtime} facade. */
export interface VoiceChangeRealtimeRequest extends ValidateOptions {
  /** One (mono) or two (stereo) planar channels of equal length. */
  channels: Float32Array[];
  /** Sample rate in Hz, an integer in [8000, 384000]. Required. */
  sampleRate: number;
  /** Preset id, preset document or flat config. Default `'neutralMonitor'`. */
  config?: RealtimeVoiceChangerConfigInput;
}

/**
 * Applies the realtime voice-changer chain to whole planar buffers in one call and returns the
 * processed channels, with the layout and length of the input.
 *
 * @throws TypeError for a wrong-typed field, a missing `sampleRate` or an unknown key;
 *   RangeError for a channel count other than 1 or 2, unequal channel lengths, out-of-domain
 *   audio or rate, or an unknown preset id or token.
 */
export function voiceChangeRealtime(request: VoiceChangeRealtimeRequest): Float32Array[] {
  const fnName = 'voiceChangeRealtime';
  const req = requestOnly<VoiceChangeRealtimeRequest>(fnName, [request]);
  if (req.sampleRate === undefined || req.sampleRate === null) {
    throw new TypeError(`${fnName}: sampleRate is required`);
  }
  assertAudioChannels(fnName, req.channels, req.sampleRate, req);
  const planes = assertPlanes(fnName, req.channels);
  const native = nativeVoiceConfig(fnName, req.config ?? 'neutralMonitor');
  const out = addon.voiceChangeRealtime(
    interleavePlanes(planes),
    req.sampleRate,
    typeof native === 'string' ? native : JSON.stringify(native),
    planes.length,
  );
  return deinterleavePlanes(out, planes.length);
}

/** The built-in preset ids, in preset-ordinal order. */
export function realtimeVoiceChangerPresetNames(): VoicePresetId[] {
  return (addon.realtimeVoiceChangerPresetNames() as string[]).map((native) => {
    const ordinal = NATIVE_PRESET_IDS.indexOf(native);
    if (ordinal < 0) {
      throw new RangeError(`realtimeVoiceChangerPresetNames: unknown native preset '${native}'`);
    }
    return PRESET_IDS[ordinal] as VoicePresetId;
  });
}

/**
 * The preset document of a built-in preset, as JSON.
 *
 * @throws TypeError unless `name` is a string; RangeError for an unknown id.
 */
export function realtimeVoiceChangerPresetJson(name: VoicePresetId): string {
  return addon.realtimeVoiceChangerPresetJson(
    presetIdToNative('realtimeVoiceChangerPresetJson', name, 'name'),
  );
}

/** Result of {@link realtimeVoiceChangerValidatePresetJson}. */
export interface RealtimeVoiceChangerValidatePresetJsonResult {
  ok: boolean;
  /** The document with defaults filled in; present when `ok`. */
  normalizedJson?: string;
  /** Why the document was refused; present when not `ok`. */
  error?: string;
}

/** Checks a preset document and returns it normalised. */
export function realtimeVoiceChangerValidatePresetJson(
  json: string,
): RealtimeVoiceChangerValidatePresetJsonResult {
  return addon.validateRealtimeVoiceChangerPresetJson(json);
}

/**
 * The preset id of a voice-character preset ordinal.
 *
 * @throws TypeError unless `ordinal` is a number; RangeError for a non-integer or unknown ordinal.
 */
export function voiceCharacterPresetId(ordinal: number): VoicePresetId {
  if (typeof ordinal !== 'number') {
    throw new TypeError('voiceCharacterPresetId: ordinal must be a number');
  }
  if (!Number.isSafeInteger(ordinal) || ordinal < 0 || ordinal >= PRESET_IDS.length) {
    throw new RangeError(
      `voiceCharacterPresetId: ordinal must be an integer in [0, ${PRESET_IDS.length - 1}]`,
    );
  }
  const native = addon.voiceCharacterPresetId(ordinal) as string;
  return PRESET_IDS[NATIVE_PRESET_IDS.indexOf(native)] as VoicePresetId;
}

/**
 * The flat config of a built-in preset, skipping the JSON round trip.
 *
 * @throws TypeError unless `preset` is a string (an ordinal is refused); RangeError for an
 *   unknown id.
 */
export function realtimeVoiceChangerPresetConfig(
  preset: VoicePresetId,
): RealtimeVoiceChangerConfig {
  const native = addon.realtimeVoiceChangerPresetConfig(
    presetOrdinal('realtimeVoiceChangerPresetConfig', preset),
  ) as Record<string, unknown>;
  return publicPodConfig('realtimeVoiceChangerPresetConfig', native);
}

/** Live controls for {@link StreamingRetune}. */
export interface StreamingRetuneConfig {
  /** Pitch shift in semitones. Applied value is clamped to +/- 24. Default 0. */
  pitchSemitones?: number;
  /** Dry/wet blend, clamped to [0, 1]. Default 1 (fully wet). */
  mix?: number;
  /**
   * Grain length in samples. Omit to derive roughly a 46 ms grain from the sample rate; an
   * explicit 0 throws `RangeError`. Structural: a new value takes effect at the NEXT
   * {@link StreamingRetune.prepare} call, and {@link StreamingRetune.config} keeps reporting
   * the effective grain until then.
   */
  grainSizeSamples?: number;
}

/**
 * Block-by-block mono pitch shifter (grain overlap-add), the streaming
 * counterpart of the offline `pitchShift`.
 *
 * State persists across {@link processMono} calls. Call {@link prepare} before
 * processing and {@link destroy} when finished. A non-finite `pitchSemitones` or
 * `mix` is rejected rather than substituted, and a block containing a
 * non-finite sample is rejected rather than zeroed: either would otherwise
 * enter the grain history and persist into every later block.
 */
// Native still spells these controls `semitones` and `grainSize`.
function retuneConfigToNative(
  fnName: string,
  config: StreamingRetuneConfig,
): Record<string, unknown> {
  if (!isRecord(config)) {
    throw new TypeError(`${fnName}: config must be an object`);
  }
  const out: Record<string, unknown> = {};
  for (const [key, value] of Object.entries(config)) {
    if (value === undefined || value === null) {
      continue;
    }
    if (key === 'pitchSemitones') {
      out.semitones = value;
    } else if (key === 'grainSizeSamples') {
      if (typeof value !== 'number') {
        throw new TypeError(`${fnName}: grainSizeSamples must be a number`);
      }
      if (value === 0) {
        throw new RangeError(`${fnName}: grainSizeSamples must not be 0 (omit it for the default)`);
      }
      out.grainSize = value;
    } else if (key === 'mix') {
      out.mix = value;
    } else {
      throw new TypeError(`${fnName}: unknown config key '${key}'`);
    }
  }
  return out;
}

export class StreamingRetune {
  private native: InstanceType<typeof addon.StreamingRetune>;
  private disposed = false;

  constructor(config: StreamingRetuneConfig = {}) {
    this.native = new addon.StreamingRetune(retuneConfigToNative('StreamingRetune', config));
  }

  /** Allocate native state for `sampleRate` and resolve the grain size. */
  prepare(sampleRate: number, maxBlockSize: number): void {
    this.native.prepare(sampleRate, maxBlockSize);
  }

  /** Clear grain / overlap-add state without changing the config. */
  reset(): void {
    this.native.reset();
  }

  /**
   * Update the live controls; omitted keys keep their current value. An omitted
   * `grainSizeSamples` keeps the last requested one, so re-{@link prepare}ing at another
   * sample rate re-derives a derived grain rather than freezing the first rate's answer.
   *
   * @throws TypeError for a wrong-typed or unknown key; RangeError for an explicit 0
   *   `grainSizeSamples` or a non-finite control.
   */
  setConfig(config: StreamingRetuneConfig): void {
    this.native.setConfig(retuneConfigToNative('StreamingRetune.setConfig', config));
  }

  /** The currently applied controls, with `grainSizeSamples` as the effective one. */
  config(): Required<StreamingRetuneConfig> {
    const native = this.native.config() as { semitones: number; mix: number; grainSize: number };
    return {
      pitchSemitones: native.semitones,
      mix: native.mix,
      grainSizeSamples: native.grainSize,
    };
  }

  /** Resolved grain length in samples; 0 before {@link prepare}. */
  get grainSizeSamples(): number {
    return this.native.grainSize();
  }

  /** Fixed overlap-add latency in samples (one grain); 0 before prepare. */
  get latencySamples(): number {
    return this.native.latencySamples();
  }

  /** Process one mono block, returning the shifted samples (same length). */
  processMono(samples: Float32Array): Float32Array {
    return this.native.processMono(samples);
  }

  /** Release the native handle now instead of waiting for GC. Idempotent. */
  destroy(): void {
    if (this.disposed) {
      return;
    }
    this.disposed = true;
    this.native.destroy();
  }

  /**

   * Releases native resources; lets `using` free them automatically (needs TypeScript 5.2+

   * or a runtime with native explicit resource management; Node 22 does not parse `using`).

   */
  [Symbol.dispose](): void {
    this.destroy();
  }
}
