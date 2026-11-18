import { chainConfigFromJson, chainConfigJson } from './internal/chain_config.js';
import { getSonareModule } from './module_state.js';
import type {
  MasteringChainConfig,
  MasteringChainResult,
  MasteringChainStereoResult,
  MasteringPlatform,
  MasteringPreset,
  StreamingLoudnessGainResult,
} from './public_types.js';
import type { ProgressCallback } from './sonare.js';
import type { OptionsOf, ValidateOptions } from './validation.js';
import {
  assertAudioInput,
  assertSampleRate,
  assertString,
  requestObject,
  requestOnly,
  sampleOneShot,
} from './validation.js';

function requireModule() {
  return getSonareModule();
}

/** Level statistic `normalize` targets: `'peak'` or `'rms'`. */
export type NormalizeMode = 'peak' | 'rms';

// `context` is the calling entry point's name: the refusal is about that
// caller's argument, so naming a fixed one would send a normalizeStereo user
// looking at normalize.
function resolveNormalizeMode(value: unknown, context = 'normalize'): NormalizeMode {
  if (value === undefined) {
    return 'peak';
  }
  if (typeof value !== 'string') {
    throw new TypeError(`${context}: mode must be the string 'peak' or 'rms'`);
  }
  if (value !== 'peak' && value !== 'rms') {
    throw new RangeError(`${context}: mode must be the string 'peak' or 'rms'`);
  }
  return value;
}

/** Request for {@link normalize}. */
export interface NormalizeRequest extends ValidateOptions {
  /** Mono input samples. */
  samples: Float32Array;
  /** Sample rate in Hz, an integer in `[8000, 384000]`; there is no default. */
  sampleRate: number;
  /**
   * Finite target level at or below 0 dBFS. Defaults to 0 for `mode: 'peak'` and
   * -20 for `mode: 'rms'`.
   */
  targetDb?: number;
  /** Normalization statistic. Default `'peak'`. */
  mode?: NormalizeMode;
}

/**
 * Normalize audio to a target peak or RMS level.
 *
 * @returns Normalized audio
 * @throws {TypeError} when `sampleRate` is missing or `mode` is not a string
 * @throws {RangeError} on audio out of domain, an out-of-range `sampleRate`, an unknown `mode`
 *   or a `targetDb` that is not finite or sits above 0 dBFS
 */
export function normalize(request: NormalizeRequest): Float32Array;
export function normalize(
  samples: Float32Array,
  sampleRate: number,
  options?: OptionsOf<NormalizeRequest>,
): Float32Array;
export function normalize(...args: unknown[]): Float32Array {
  const r = sampleOneShot<NormalizeRequest>('normalize', args);
  assertAudioInput('normalize', r.samples, r.sampleRate, r);
  const mode = resolveNormalizeMode(r.mode);
  return requireModule().normalizeEx(
    r.samples,
    r.sampleRate,
    r.targetDb ?? (mode === 'rms' ? -20.0 : 0.0),
    mode,
  );
}

/** Request for {@link normalizeStereo}. */
export interface NormalizeStereoRequest extends ValidateOptions {
  /** Left channel samples. */
  left: Float32Array;
  /** Right channel samples. */
  right: Float32Array;
  /** Sample rate in Hz, an integer in `[8000, 384000]`; there is no default. */
  sampleRate: number;
  /**
   * Finite target level at or below 0 dBFS. Defaults to 0 for `mode: 'peak'` and
   * -20 for `mode: 'rms'`.
   */
  targetDb?: number;
  /** Normalization statistic. Default `'peak'`. */
  mode?: NormalizeMode;
}

/** A normalized channel pair and the gain both channels were moved by. */
export interface NormalizeStereoResult {
  /** Normalized left channel. */
  left: Float32Array;
  /** Normalized right channel. */
  right: Float32Array;
  /**
   * One figure rather than a pair: the gain is one decision shared by both
   * channels. Silence leaves the pair untouched and reports 0.
   */
  appliedGainDb: number;
}

/**
 * Normalize a stereo pair on a gain measured across both channels.
 *
 * Normalizing the two channels separately lifts the quieter one until the peaks
 * match, which changes the balance rather than the level. The level here is read
 * from the pair and the resulting gain goes to both channels, so the image is
 * preserved: for `mode: 'peak'` the louder channel reaches `targetDb` and the
 * other keeps its distance from it; for `mode: 'rms'` the quantity driven to
 * `targetDb` is the root mean square over both channels' samples together — the
 * quadratic mean of the per-channel figures, not their average — and the output
 * is hard-clipped to [-1, 1].
 *
 * @param request.left - Left channel samples (float32)
 * @param request.right - Right channel samples, same length as `left`
 * @param request.sampleRate - Sample rate in Hz, required
 * @param request.targetDb - Finite target at or below 0 dBFS. Defaults to 0 for
 *   `mode: 'peak'` and -20 for `mode: 'rms'`, matching the library and the other
 *   language surfaces.
 * @param request.mode - `'peak'` (default) or `'rms'`
 * @returns The normalized pair and the shared gain in dB
 * @throws {TypeError} when `sampleRate` is missing
 * @throws {RangeError} when the two channels differ in length
 *
 * @example
 * ```ts
 * const { left, right, appliedGainDb } = normalizeStereo({
 *   left: leftSamples,
 *   right: rightSamples,
 *   sampleRate: 44100,
 *   targetDb: -1,
 * });
 * ```
 */
export function normalizeStereo(request: NormalizeStereoRequest): NormalizeStereoResult {
  requestObject('normalizeStereo', request, 'request', true);
  assertAudioInput('normalizeStereo', request.left, request.sampleRate, request, 'left');
  assertAudioInput('normalizeStereo', request.right, request.sampleRate, request, 'right');
  if (request.left.length !== request.right.length) {
    throw new RangeError('Stereo channel lengths must match.');
  }
  const mode = resolveNormalizeMode(request.mode, 'normalizeStereo');
  // Mode-dependent, as in the mono `normalize`: 0 dBFS RMS is not a usable
  // default -- the peaks sit well above the RMS, so every one of them clips.
  const targetDb = request.targetDb ?? (mode === 'rms' ? -20.0 : 0.0);
  return requireModule().normalizeStereo(
    request.left,
    request.right,
    request.sampleRate,
    targetDb,
    mode,
  );
}

type NativeResult = Record<string, unknown>;

/** Throws `TypeError` unless `onProgress` and `cancel` are each absent or a function. */
function assertChainCallbacks(fnName: string, request: object): void {
  for (const name of ['onProgress', 'cancel'] as const) {
    const callback = (request as Record<string, unknown>)[name];
    if (callback !== undefined && callback !== null && typeof callback !== 'function') {
      throw new TypeError(`${fnName}: ${name} must be a function`);
    }
  }
}

/** Request for {@link streamingLoudnessGain}. */
export interface StreamingLoudnessGainRequest extends ValidateOptions {
  /** Mono input samples. */
  samples: Float32Array;
  /** Sample rate in Hz, an integer in `[8000, 384000]`; there is no default. */
  sampleRate: number;
  /** The chain config the streaming chain will run; read like {@link masterAudio}'s `overrides`. */
  config?: MasteringChainConfig;
}

/** Request for {@link streamingLoudnessGainStereo}. */
export interface StreamingLoudnessGainStereoRequest extends ValidateOptions {
  /** Left channel samples. */
  left: Float32Array;
  /** Right channel samples. */
  right: Float32Array;
  /** Sample rate in Hz, an integer in `[8000, 384000]`; there is no default. */
  sampleRate: number;
  /** The chain config the streaming chain will run; read like `masterAudio`'s `overrides`. */
  config?: MasteringChainConfig;
}

// Native still names the input true peak `truePeakDb`.
function streamingGainFromNative(raw: object): StreamingLoudnessGainResult {
  const { truePeakDb, ...rest } = raw as NativeResult & { truePeakDb: number };
  return { ...rest, truePeakDbtp: truePeakDb } as unknown as StreamingLoudnessGainResult;
}

/**
 * Measures the loudness numbers a {@link StreamingMasteringChain} needs.
 *
 * Runs the offline chain described by `config` up to its loudness stage and
 * measures there, so `loudnessStaticGainDb` equals the gain {@link masterAudio}
 * applies (ceiling clamp included) and `truePeakDbtp` is the peak that clamp
 * used. The gain is computed from `config.loudness` whether or not the stage is
 * enabled; a silent or below-gate stage input yields 0 dB.
 *
 * @throws {TypeError} when `sampleRate` is missing
 * @throws {RangeError} on audio out of domain or an out-of-range `sampleRate`
 *
 * @example
 * const { loudnessStaticGainDb, truePeakDbtp } = streamingLoudnessGain(samples, 48000, { config });
 * const chain = new StreamingMasteringChain({
 *   ...config,
 *   sampleRate: 48000,
 *   numChannels: 1,
 *   loudnessStaticGainDb,
 *   loudnessStaticGainPeakDb: truePeakDbtp,
 * });
 */
export function streamingLoudnessGain(
  request: StreamingLoudnessGainRequest,
): StreamingLoudnessGainResult;
export function streamingLoudnessGain(
  samples: Float32Array,
  sampleRate: number,
  options?: OptionsOf<StreamingLoudnessGainRequest>,
): StreamingLoudnessGainResult;
export function streamingLoudnessGain(...args: unknown[]): StreamingLoudnessGainResult {
  const r = sampleOneShot<StreamingLoudnessGainRequest>('streamingLoudnessGain', args);
  assertAudioInput('streamingLoudnessGain', r.samples, r.sampleRate, r);
  return streamingGainFromNative(
    requireModule().masteringStreamingLoudnessGain(
      r.samples,
      r.sampleRate,
      chainConfigJson('streamingLoudnessGain', 'config', r.config),
    ),
  );
}

/** Stereo counterpart of {@link streamingLoudnessGain}, with BS.1770 channel summing. */
export function streamingLoudnessGainStereo(
  request: StreamingLoudnessGainStereoRequest,
): StreamingLoudnessGainResult {
  const r = requestObject('streamingLoudnessGainStereo', request, 'request', true);
  assertAudioInput('streamingLoudnessGainStereo', r.left, r.sampleRate, r, 'left');
  assertAudioInput('streamingLoudnessGainStereo', r.right, r.sampleRate, r, 'right');
  if (r.left.length !== r.right.length) {
    throw new RangeError('Stereo channel lengths must match.');
  }
  return streamingGainFromNative(
    requireModule().masteringStreamingLoudnessGainStereo(
      r.left,
      r.right,
      r.sampleRate,
      chainConfigJson('streamingLoudnessGainStereo', 'config', r.config),
    ),
  );
}

/** Request form of {@link masterAudio}. */
export interface MasterAudioRequest extends ValidateOptions {
  /** Mono input samples. */
  samples: Float32Array;
  /** Sample rate in Hz, an integer in `[8000, 384000]`; there is no default. */
  sampleRate: number;
  /**
   * Built-in preset to start from. Absent: the chain runs `overrides` alone. Giving
   * neither `preset` nor `overrides` is refused with a `TypeError`.
   */
  preset?: MasteringPreset;
  /**
   * Chain config applied over the preset, or run alone when `preset` is absent. An
   * explicit `{}` without a preset runs an empty chain and returns the input unchanged.
   */
  overrides?: MasteringChainConfig;
  /** Called between chain stages with `progress` in `[0, 1]` and the stage name. */
  onProgress?: ProgressCallback;
  /** Polled between chain stages; return `true` to stop with a `Cancelled` error. */
  cancel?: () => boolean;
}

/** Request form of {@link masterAudioStereo}. */
export interface MasterAudioStereoRequest extends ValidateOptions {
  /** Left channel samples. */
  left: Float32Array;
  /** Right channel samples. */
  right: Float32Array;
  /** Sample rate in Hz, an integer in `[8000, 384000]`; there is no default. */
  sampleRate: number;
  /**
   * Built-in preset to start from. Absent: the chain runs `overrides` alone. Giving
   * neither `preset` nor `overrides` is refused with a `TypeError`.
   */
  preset?: MasteringPreset;
  /**
   * Chain config applied over the preset, or run alone when `preset` is absent. An
   * explicit `{}` without a preset runs an empty chain and returns the input unchanged.
   */
  overrides?: MasteringChainConfig;
  /** Called between chain stages with `progress` in `[0, 1]` and the stage name. */
  onProgress?: ProgressCallback;
  /** Polled between chain stages; return `true` to stop with a `Cancelled` error. */
  cancel?: () => boolean;
}

/**
 * List built-in mastering preset identifiers.
 *
 * @returns Preset names in display order (e.g. "pop", "edm", "aiMusic")
 */
export function masteringPresetNames(): MasteringPreset[] {
  return Array.from(requireModule().masteringPresetNames()) as MasteringPreset[];
}

/**
 * The nested {@link MasteringChainConfig} of preset `preset`'s built-in chain.
 * Passing it as `overrides` to {@link masterAudio} reproduces the preset.
 *
 * @param preset - Preset identifier from {@link masteringPresetNames}.
 * @throws {TypeError} when `preset` is not a string
 * @throws {RangeError} for an unknown `preset`
 */
export function masteringPresetParams(preset: MasteringPreset): MasteringChainConfig {
  assertString('masteringPresetParams', preset, 'preset');
  return chainConfigFromJson(requireModule().masteringPresetParams(preset));
}

/**
 * Read a serialized chain configuration document into the nested
 * {@link MasteringChainConfig} that {@link masterAudio} takes as `overrides`.
 *
 * The library's own parser validates the document, so a key or token outside
 * the current spelling is refused rather than read. Retired keys are refused
 * with a message naming their current spelling. This is the one entry that
 * takes a chain configuration as JSON; every other call takes the object.
 *
 * @param document - Chain configuration document as JSON:
 *   `{ "version": 2, "params": { "<stage>.<param>": value } }`, the form the
 *   native CLI's `--chain-config` reads. `version` may be left out; dotted keys
 *   and nested objects name the same path, and giving one path twice is refused.
 * @returns The validated configuration in its canonical nested form.
 * @throws {TypeError} when `document` is not a string
 * @throws {SonareError} with code `InvalidFormat` when the document is not
 *   valid JSON, carries another version, or names a key or value the chain
 *   does not accept
 *
 * @example
 * ```ts
 * import { masteringChainConfigFromJson, masterAudio } from '@libraz/libsonare';
 *
 * declare const saved: string; // e.g. read from a project file
 * const overrides = masteringChainConfigFromJson(saved);
 * const mastered = masterAudio({ samples, sampleRate, preset: 'pop', overrides });
 * ```
 */
export function masteringChainConfigFromJson(document: string): MasteringChainConfig {
  assertString('masteringChainConfigFromJson', document, 'document');
  return chainConfigFromJson(requireModule().masteringChainConfigParams(document));
}

/**
 * List the delivery targets the mastering assistant accepts as `targetPlatform`.
 *
 * Read from the library rather than from a list kept here, so a target added in
 * the core is discoverable without a binding change.
 *
 * @returns Target names in index order (e.g. "streaming", "broadcast", "club")
 */
export function masteringPlatformNames(): MasteringPlatform[] {
  return Array.from(requireModule().masteringPlatformNames()) as MasteringPlatform[];
}

/**
 * Master mono audio. With a `preset` the chain starts from that preset and
 * `overrides` is applied on top; without one the chain runs `overrides` alone.
 * Giving neither is refused, since that call would return the input unchanged.
 *
 * @param samples - Audio samples (mono, float32)
 * @param sampleRate - Sample rate in Hz, an integer in `[8000, 384000]`
 * @param options - `preset`, `overrides`, `onProgress`, `cancel`, `validate`
 * @returns Processed audio, loudness metadata, and applied stage names
 * @throws {TypeError} when `sampleRate` is missing, neither `preset` nor `overrides` is given,
 *   or a request field has the wrong type
 * @throws {RangeError} on audio out of domain, an unknown `preset` or an invalid override
 */
export function masterAudio(request: MasterAudioRequest): MasteringChainResult;
export function masterAudio(
  samples: Float32Array,
  sampleRate: number,
  options?: OptionsOf<MasterAudioRequest>,
): MasteringChainResult;
export function masterAudio(...args: unknown[]): MasteringChainResult {
  const r = sampleOneShot<MasterAudioRequest>('masterAudio', args);
  assertChainCallbacks('masterAudio', r);
  if (r.preset !== undefined && r.preset !== null) {
    assertString('masterAudio', r.preset, 'preset');
  }
  assertAudioInput('masterAudio', r.samples, r.sampleRate, r);
  if (
    (r.preset === undefined || r.preset === null) &&
    (r.overrides === undefined || r.overrides === null)
  ) {
    throw new TypeError('masterAudio: give preset or overrides');
  }
  const overrides = chainConfigJson('masterAudio', 'overrides', r.overrides);
  const preset = r.preset ?? null;
  const progress = r.onProgress ?? undefined;
  const cancel = r.cancel ?? undefined;
  const module = requireModule();
  return (
    progress || cancel
      ? module.masterAudioWithProgress(
          preset,
          r.samples,
          r.sampleRate,
          overrides,
          progress ?? (() => {}),
          cancel ?? (() => false),
        )
      : module.masterAudio(preset, r.samples, r.sampleRate, overrides)
  ) as MasteringChainResult;
}

/**
 * Stereo counterpart of {@link masterAudio}; takes a request object only.
 *
 * @returns Processed stereo audio, loudness metadata, and applied stage names
 * @throws {TypeError} when `sampleRate` is missing, neither `preset` nor `overrides` is given,
 *   or a request field has the wrong type
 * @throws {RangeError} on audio out of domain, unequal channel lengths, an unknown `preset`
 *   or an invalid override
 */
export function masterAudioStereo(request: MasterAudioStereoRequest): MasteringChainStereoResult {
  const r = requestOnly<MasterAudioStereoRequest>('masterAudioStereo', [request]);
  assertChainCallbacks('masterAudioStereo', r);
  if (r.preset !== undefined && r.preset !== null) {
    assertString('masterAudioStereo', r.preset, 'preset');
  }
  assertSampleRate('masterAudioStereo', r.sampleRate);
  assertAudioInput('masterAudioStereo', r.left, r.sampleRate, r, 'left');
  assertAudioInput('masterAudioStereo', r.right, r.sampleRate, r, 'right');
  if (r.left.length !== r.right.length) {
    throw new RangeError('Stereo channel lengths must match.');
  }
  if (
    (r.preset === undefined || r.preset === null) &&
    (r.overrides === undefined || r.overrides === null)
  ) {
    throw new TypeError('masterAudioStereo: give preset or overrides');
  }
  const overrides = chainConfigJson('masterAudioStereo', 'overrides', r.overrides);
  const preset = r.preset ?? null;
  const progress = r.onProgress ?? undefined;
  const cancel = r.cancel ?? undefined;
  const module = requireModule();
  return (
    progress || cancel
      ? module.masterAudioStereoWithProgress(
          preset,
          r.left,
          r.right,
          r.sampleRate,
          overrides,
          progress ?? (() => {}),
          cancel ?? (() => false),
        )
      : module.masterAudioStereo(preset, r.left, r.right, r.sampleRate, overrides)
  ) as MasteringChainStereoResult;
}
