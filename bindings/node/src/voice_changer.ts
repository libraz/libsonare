import { entryClass, nameNativeError, refuse, refuseType, withEntry } from './internal/entry.js';
import { addon } from './native.js';
import type {
  RealtimeVoiceChangerConfig,
  RealtimeVoiceChangerConfigInput,
  RealtimeVoiceChangerOptions,
  VoicePresetId,
} from './types.js';
import {
  assertAudioChannels,
  assertAudioInput,
  assertKnownRequestKeys,
  assertPositiveInteger,
  assertRetuneGrain,
  assertSampleRate,
  requestOnly,
  sampleOneShot,
} from './validation.js';

/** Public preset ids in the order of the native preset ordinals. */
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
function presetOrdinal(id: unknown, argName = 'preset'): number {
  if (typeof id !== 'string') {
    throw refuseType(`${argName} must be one of ${PRESET_IDS.join(', ')}`);
  }
  const ordinal = PRESET_IDS.indexOf(id as VoicePresetId);
  if (ordinal < 0) {
    throw refuse(`${argName} must be one of ${PRESET_IDS.join(', ')}`);
  }
  return ordinal;
}

function checkedPresetId(id: unknown, argName = 'preset'): VoicePresetId {
  return PRESET_IDS[presetOrdinal(id, argName)] as VoicePresetId;
}

function isRecord(value: unknown): value is Record<string, unknown> {
  return typeof value === 'object' && value !== null && !Array.isArray(value);
}

/** True for the flat config object (as opposed to a nested preset document). */
function isFlatConfig(config: unknown): config is Record<string, unknown> {
  return isRecord(config) && 'retunePitchSemitones' in config;
}

let flatReference: RealtimeVoiceChangerConfig | undefined;

/** Every required flat-config field with a value of its type; they do not vary by preset. */
function flatConfigReference(): RealtimeVoiceChangerConfig {
  flatReference ??= realtimeVoiceChangerPresetConfig('neutralMonitor');
  return flatReference;
}

/**
 * Refuses a flat config missing a field or carrying one of the wrong JS type, naming the public
 * field before native sees it.
 */
function assertFlatConfigFields(config: Record<string, unknown>): void {
  for (const [key, sample] of Object.entries(flatConfigReference())) {
    const value = config[key];
    if (key === 'retuneGrainSizeSamples') {
      continue;
    }
    if (key !== 'harmonySteps' && typeof value !== typeof sample) {
      throw refuseType(`${key} must be a ${typeof sample}`);
    }
  }
}

/**
 * The one optional flat field: an omitted (`undefined` or `null`) retune grain is the
 * automatic grain and leaves the key out; a given one is a multiple of 4 in [4, 8192].
 */
function withRetuneGrain(config: Record<string, unknown>): Record<string, unknown> {
  const grain = config.retuneGrainSizeSamples;
  if (grain === undefined || grain === null) {
    const { retuneGrainSizeSamples: _omitted, ...rest } = config;
    return rest;
  }
  assertRetuneGrain(grain, 'retuneGrainSizeSamples');
  return config;
}

/**
 * The flat config as the addon reads it. Every field but the retune grain is required, as in
 * the native parser.
 *
 * @throws TypeError for a missing or wrong-typed field; RangeError for a `harmonySteps` of
 *   another length or a retune grain that is not a multiple of 4 in [4, 8192].
 */
function nativePodConfig(config: Record<string, unknown>): Record<string, unknown> {
  config = withRetuneGrain(config);
  assertFlatConfigFields(config);
  const steps = config.harmonySteps;
  if (!Array.isArray(steps)) {
    throw refuseType(`harmonySteps must be an array of three numbers`);
  }
  if (steps.length !== 3) {
    throw refuse(`harmonySteps must hold exactly three numbers`);
  }
  return config;
}

/**
 * A voice-changer config in the form the addon reads: a preset id is checked, a flat config is
 * validated, and a nested preset document passes through.
 *
 * @throws TypeError for any other type; RangeError for an unknown preset id.
 */
function nativeVoiceConfig(
  config: RealtimeVoiceChangerConfigInput,
): Record<string, unknown> | string {
  if (typeof config === 'string') {
    return checkedPresetId(config, 'config');
  }
  if (isFlatConfig(config)) {
    return nativePodConfig(config);
  }
  if (!isRecord(config)) {
    throw refuseType(`config must be a preset id or an object`);
  }
  return config;
}

function assertChannelCount(numChannels: unknown): asserts numChannels is 1 | 2 {
  if (typeof numChannels !== 'number') {
    throw refuseType(`numChannels must be 1 or 2`);
  }
  if (numChannels !== 1 && numChannels !== 2) {
    throw refuse(`numChannels must be 1 or 2`);
  }
}

/** Checks a planar channel list (1 or 2 equal-length planes) and the optional carrier. */
function assertPlanes(channels: unknown, carrier?: unknown): Float32Array[] {
  if (!Array.isArray(channels)) {
    throw refuseType(`channels must be an array of Float32Array`);
  }
  assertChannelCount(channels.length);
  channels.forEach((plane, i) => {
    if (!(plane instanceof Float32Array)) {
      throw refuseType(`channels[${i}] must be a Float32Array`);
    }
    if (plane.length !== channels[0].length) {
      throw refuse(`channels must have equal lengths`);
    }
  });
  if (carrier !== undefined && !(carrier instanceof Float32Array)) {
    throw refuseType(`carrier must be a Float32Array`);
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

/** Realtime voice-changer chain: pitch, formant, EQ, dynamics, harmony, vocoder, character and space stages over mono or stereo blocks. Call {@link RealtimeVoiceChanger.destroy} when done. */
@entryClass('RealtimeVoiceChanger', [
  'processInterleavedInto',
  'processInterleavedWithCarrierInto',
  'processMono',
  'processMonoInto',
  'processMonoWithCarrier',
  'processMonoWithCarrierInto',
])
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
    if (typeof options !== 'object' || options === null || Array.isArray(options)) {
      throw refuseType(`options must be an object with a sampleRate`);
    }
    for (const key of Object.keys(options)) {
      if (!CONSTRUCTOR_KEYS.includes(key)) {
        throw refuseType(`unknown option '${key}'`);
      }
    }
    assertSampleRate(options.sampleRate);
    const maxBlockSize = options.maxBlockSize ?? undefined;
    if (maxBlockSize !== undefined) {
      if (typeof maxBlockSize !== 'number') {
        throw refuseType(`maxBlockSize must be a number`);
      }
      assertPositiveInteger(maxBlockSize, 'maxBlockSize');
    }
    const numChannels = options.numChannels;
    assertChannelCount(numChannels);
    this.native = new addon.RealtimeVoiceChanger(
      nativeVoiceConfig(options.config ?? 'neutralMonitor'),
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

  /** Clears the streaming DSP state (filters, envelopes, reverb, limiter) and keeps the configuration. Not for use while a block is being processed. */
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
    this.native.setConfig(nativeVoiceConfig(config));
  }

  /**
   * The applied configuration as a flat record, in the form {@link setConfig} and the
   * constructor accept, so `other.setConfig(changer.config())` reproduces it. An automatic
   * retune grain reads back as an absent `retuneGrainSizeSamples`.
   */
  config(): RealtimeVoiceChangerConfig {
    return this.native.config() as RealtimeVoiceChangerConfig;
  }

  /** Current configuration as a JSON preset document. */
  configJson(): string {
    return this.native.configJson();
  }

  /** Latency of the chain in samples. */
  get latencySamples(): number {
    return this.native.latencySamples();
  }

  /** Largest block a process call accepts, as the native side resolved it (the library default when `maxBlockSize` was omitted). */
  get maxBlockSize(): number {
    return this.native.maxBlockSize();
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

  /** Processes a mono block and returns a new array of the same length. */
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

  /** Realtime hot path: {@link RealtimeVoiceChanger.processMonoWithCarrier} writing into `output`, which has the length of `input`. */
  processMonoWithCarrierInto(
    input: Float32Array,
    carrier: Float32Array,
    output: Float32Array,
  ): void {
    this.native.processMonoWithCarrierInto(input, carrier, output);
  }

  /** Realtime hot path: {@link RealtimeVoiceChanger.processMono} writing into `output`, which has the length of `input`. */
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
    const planes = assertPlanes(channels);
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
    const planes = assertPlanes(channels, carrier);
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
    try {
      assertChannelCount(numChannels);
    } catch (error) {
      throw nameNativeError(error, 'RealtimeVoiceChanger.processInterleavedInto');
    }
    this.native.processInterleavedInto(input, numChannels, output);
  }

  /** {@link processInterleavedInto} with a mono carrier of one sample per frame shared by every channel. */
  processInterleavedWithCarrierInto(
    input: Float32Array,
    carrier: Float32Array,
    numChannels: 1 | 2,
    output: Float32Array,
  ): void {
    try {
      assertChannelCount(numChannels);
    } catch (error) {
      throw nameNativeError(error, 'RealtimeVoiceChanger.processInterleavedWithCarrierInto');
    }
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

  /** Releases the native handle. Idempotent. */
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
export interface VoiceChangeOptions {
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
  /** Mono input audio. */
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
  return withEntry('voiceChange', () => {
    const request = sampleOneShot<VoiceChangeRequest>('voiceChange', args);
    assertAudioInput(request.samples, request.sampleRate, request);
    return addon.voiceChange(
      request.samples,
      request.sampleRate,
      request.pitchSemitones ?? 0.0,
      request.formantFactor ?? 1.0,
      request.formantMode ?? 'relative',
    );
  });
}

/** Inputs for the one-shot {@link voiceChangeRealtime} facade. */
export interface VoiceChangeRealtimeRequest {
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
  return withEntry('voiceChangeRealtime', () => {
    const fnName = 'voiceChangeRealtime';
    const req = requestOnly<VoiceChangeRealtimeRequest>(fnName, [request]);
    if (req.sampleRate === undefined || req.sampleRate === null) {
      throw refuseType(`sampleRate must be a number`);
    }
    assertAudioChannels(req.channels, req.sampleRate, req);
    const planes = assertPlanes(req.channels);
    const native = withEntry('voiceChangeRealtime', () =>
      nativeVoiceConfig(req.config ?? 'neutralMonitor'),
    );
    const out = addon.voiceChangeRealtime(
      interleavePlanes(planes),
      req.sampleRate,
      typeof native === 'string' ? native : JSON.stringify(native),
      planes.length,
    );
    return deinterleavePlanes(out, planes.length);
  });
}

/** The built-in preset ids, in preset-ordinal order. */
export function realtimeVoiceChangerPresetNames(): VoicePresetId[] {
  return addon.realtimeVoiceChangerPresetNames() as VoicePresetId[];
}

/**
 * The preset document of a built-in preset, as JSON.
 *
 * @throws TypeError unless `name` is a string; RangeError for an unknown id.
 */
export function realtimeVoiceChangerPresetJson(name: VoicePresetId): string {
  return withEntry('realtimeVoiceChangerPresetJson', () => {
    return addon.realtimeVoiceChangerPresetJson(checkedPresetId(name, 'name'));
  });
}

/** Result of {@link realtimeVoiceChangerValidatePresetJson}. */
export interface RealtimeVoiceChangerValidatePresetJsonResult {
  /** Whether the document was accepted. */
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
  return withEntry('voiceCharacterPresetId', () => {
    if (typeof ordinal !== 'number') {
      throw refuseType('ordinal must be a number');
    }
    if (!Number.isSafeInteger(ordinal) || ordinal < 0 || ordinal >= PRESET_IDS.length) {
      throw refuse(`ordinal must be an integer in [0, ${PRESET_IDS.length - 1}]`);
    }
    return addon.voiceCharacterPresetId(ordinal) as VoicePresetId;
  });
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
  return withEntry('realtimeVoiceChangerPresetConfig', () => {
    return addon.realtimeVoiceChangerPresetConfig(
      presetOrdinal(preset),
    ) as RealtimeVoiceChangerConfig;
  });
}

/** Live controls for {@link StreamingRetune}. */
export interface StreamingRetuneConfig {
  /** Pitch shift in semitones, finite and in [-24, 24]; outside it is refused. Default 0. */
  pitchSemitones?: number;
  /** Dry/wet blend in [0, 1]; outside it is refused. Default 1 (fully wet). */
  mix?: number;
  /**
   * Grain length in samples, a multiple of 4 in [4, 8192]. Omitted: the automatic grain, roughly
   * 46 ms derived from the sample rate at each {@link StreamingRetune.prepare} and readable from
   * {@link StreamingRetune.grainSizeSamples} after it; 0 is refused. Structural: a new value
   * takes effect at the NEXT {@link StreamingRetune.prepare} call.
   */
  grainSizeSamples?: number;
}

/** The controls {@link StreamingRetune.config} reports; `grainSizeSamples` is absent when automatic. */
export type StreamingRetuneAppliedConfig = Required<
  Omit<StreamingRetuneConfig, 'grainSizeSamples'>
> &
  Pick<StreamingRetuneConfig, 'grainSizeSamples'>;

// The grain is checked here because nothing below refuses a fraction or an out-of-range value;
// native refuses wrong-typed controls.
function assertRetuneConfig(config: StreamingRetuneConfig): StreamingRetuneConfig {
  if (!isRecord(config)) {
    throw refuseType(`config must be an object`);
  }
  assertKnownRequestKeys('StreamingRetune', config as Record<string, unknown>);
  const { grainSizeSamples, ...rest } = config as Record<string, unknown>;
  if (grainSizeSamples === undefined || grainSizeSamples === null) {
    return rest;
  }
  assertRetuneGrain(grainSizeSamples, 'grainSizeSamples');
  return config;
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
@entryClass('StreamingRetune', ['processMono'])
export class StreamingRetune {
  private native: InstanceType<typeof addon.StreamingRetune>;
  private disposed = false;

  constructor(config: StreamingRetuneConfig = {}) {
    this.native = new addon.StreamingRetune(assertRetuneConfig(config));
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
   * `grainSizeSamples` keeps the configured one, automatic included, so re-{@link prepare}ing
   * at another sample rate re-derives an automatic grain rather than freezing the first rate's.
   *
   * @throws TypeError for a wrong-typed or unknown key; RangeError for a `grainSizeSamples`
   *   of 0 or not a multiple of 4 in [4, 8192], or a control outside its domain.
   */
  setConfig(config: StreamingRetuneConfig): void {
    this.native.setConfig(assertRetuneConfig(config));
  }

  /**
   * The configured controls, in the form {@link setConfig} accepts, so
   * `other.setConfig(retune.config())` reproduces them. An automatic grain reads back as an
   * absent `grainSizeSamples`; the grain in use is {@link grainSizeSamples}.
   */
  config(): StreamingRetuneAppliedConfig {
    const native = this.native.config() as {
      pitchSemitones: number;
      mix: number;
      grainSizeSamples: number;
    };
    const controls = { pitchSemitones: native.pitchSemitones, mix: native.mix };
    return native.grainSizeSamples === 0
      ? controls
      : { ...controls, grainSizeSamples: native.grainSizeSamples };
  }

  /** Grain length in use, in samples; 0 before {@link prepare}. */
  get grainSizeSamples(): number {
    return this.native.grainSizeSamples();
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
