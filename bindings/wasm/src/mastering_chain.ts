import { chainConfigFromJson, chainConfigJson } from './internal/chain_config.js';
import { refuse, refuseType, withEntry } from './internal/entry.js';
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
import type { OptionsOf } from './validation.js';
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

function resolveNormalizeMode(value: unknown): NormalizeMode {
  if (value === undefined) {
    return 'peak';
  }
  if (typeof value !== 'string') {
    throw refuseType(`mode must be one of peak, rms`);
  }
  if (value !== 'peak' && value !== 'rms') {
    throw refuse(`mode must be one of peak, rms`);
  }
  return value;
}

/** Request for {@link normalize}. */
export interface NormalizeRequest {
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
  return withEntry('normalize', () => {
    const r = sampleOneShot<NormalizeRequest>('normalize', args);
    assertAudioInput(r.samples, r.sampleRate, r);
    const mode = resolveNormalizeMode(r.mode);
    return requireModule().normalize(
      r.samples,
      r.sampleRate,
      r.targetDb ?? (mode === 'rms' ? -20.0 : 0.0),
      mode,
    );
  });
}

/** Request for {@link normalizeStereo}. */
export interface NormalizeStereoRequest {
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
  return withEntry('normalizeStereo', () => {
    requestObject('normalizeStereo', request, 'request', true);
    assertAudioInput(request.left, request.sampleRate, request, 'left');
    assertAudioInput(request.right, request.sampleRate, request, 'right');
    if (request.left.length !== request.right.length) {
      throw refuse('left and right must have the same length');
    }
    const mode = resolveNormalizeMode(request.mode);
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
  });
}

type NativeResult = Record<string, unknown>;

/** Throws `TypeError` unless `onProgress` and `cancel` are each absent or a function. */
function assertChainCallbacks(request: object): void {
  for (const name of ['onProgress', 'cancel'] as const) {
    const callback = (request as Record<string, unknown>)[name];
    if (callback !== undefined && callback !== null && typeof callback !== 'function') {
      throw refuseType(`${name} must be a function`);
    }
  }
}

/** Request for {@link streamingLoudnessGain}. */
export interface StreamingLoudnessGainRequest {
  /** Mono input samples. */
  samples: Float32Array;
  /** Sample rate in Hz, an integer in `[8000, 384000]`; there is no default. */
  sampleRate: number;
  /** The chain config the streaming chain will run; read like {@link masterAudio}'s `overrides`. */
  config?: MasteringChainConfig;
}

/** Request for {@link streamingLoudnessGainStereo}. */
export interface StreamingLoudnessGainStereoRequest {
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
 * ```ts
 * declare const config: MasteringChainConfig; // the chain the stream will run
 * const { loudnessStaticGainDb, truePeakDbtp } = streamingLoudnessGain(samples, 48000, { config });
 * const chain = new StreamingMasteringChain({
 *   ...config,
 *   sampleRate: 48000,
 *   numChannels: 1,
 *   loudnessStaticGainDb,
 *   loudnessStaticGainPeakDbtp: truePeakDbtp,
 * });
 * ```
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
  return withEntry('streamingLoudnessGain', () => {
    const r = sampleOneShot<StreamingLoudnessGainRequest>('streamingLoudnessGain', args);
    assertAudioInput(r.samples, r.sampleRate, r);
    return streamingGainFromNative(
      requireModule().masteringStreamingLoudnessGain(
        r.samples,
        r.sampleRate,
        chainConfigJson('config', r.config),
      ),
    );
  });
}

/** Stereo counterpart of {@link streamingLoudnessGain}, with BS.1770 channel summing. */
export function streamingLoudnessGainStereo(
  request: StreamingLoudnessGainStereoRequest,
): StreamingLoudnessGainResult {
  return withEntry('streamingLoudnessGainStereo', () => {
    const r = requestObject('streamingLoudnessGainStereo', request, 'request', true);
    assertAudioInput(r.left, r.sampleRate, r, 'left');
    assertAudioInput(r.right, r.sampleRate, r, 'right');
    if (r.left.length !== r.right.length) {
      throw refuse('left and right must have the same length');
    }
    return streamingGainFromNative(
      requireModule().masteringStreamingLoudnessGainStereo(
        r.left,
        r.right,
        r.sampleRate,
        chainConfigJson('config', r.config),
      ),
    );
  });
}

/** Request form of {@link masterAudio}. */
export interface MasterAudioRequest {
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
   * A stage that carries any key takes the `enabled` it states, and is turned on when
   * it states none; so a full config, such as {@link masteringChainConfigFromJson}
   * returns, turns off every preset stage it marks `enabled: false`.
   */
  overrides?: MasteringChainConfig;
  /** Called between chain stages with `progress` in `[0, 1]` and the stage name. */
  onProgress?: ProgressCallback;
  /** Polled between chain stages; return `true` to stop with a `Cancelled` error. */
  cancel?: () => boolean;
}

/** Request form of {@link masterAudioStereo}. */
export interface MasterAudioStereoRequest {
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
   * A stage that carries any key takes the `enabled` it states, and is turned on when
   * it states none; so a full config, such as {@link masteringChainConfigFromJson}
   * returns, turns off every preset stage it marks `enabled: false`.
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
  return withEntry('masteringPresetNames', () => {
    return Array.from(requireModule().masteringPresetNames()) as MasteringPreset[];
  });
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
  return withEntry('masteringPresetParams', () => {
    assertString(preset, 'preset');
    return chainConfigFromJson(requireModule().masteringPresetParams(preset));
  });
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
 * // The document is the whole chain, so it runs without a preset.
 * const overrides = masteringChainConfigFromJson(saved);
 * const mastered = masterAudio({ samples, sampleRate, overrides });
 * ```
 */
export function masteringChainConfigFromJson(document: string): MasteringChainConfig {
  return withEntry('masteringChainConfigFromJson', () => {
    assertString(document, 'document');
    return chainConfigFromJson(requireModule().masteringChainConfigParams(document));
  });
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
  return withEntry('masteringPlatformNames', () => {
    return Array.from(requireModule().masteringPlatformNames()) as MasteringPlatform[];
  });
}

/**
 * Master mono audio. With a `preset` the chain starts from that preset and
 * `overrides` is applied on top; without one the chain runs `overrides` alone.
 * Giving neither is refused, since that call would return the input unchanged.
 *
 * @param request - Mono `samples`, `sampleRate` in Hz, an integer in `[8000, 384000]`, `preset`,
 *   `overrides`, `onProgress` and `cancel`; or pass `(samples, sampleRate, options?)`.
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
  return withEntry('masterAudio', () => {
    const r = sampleOneShot<MasterAudioRequest>('masterAudio', args);
    assertChainCallbacks(r);
    if (r.preset !== undefined && r.preset !== null) {
      assertString(r.preset, 'preset');
    }
    assertAudioInput(r.samples, r.sampleRate, r);
    if (
      (r.preset === undefined || r.preset === null) &&
      (r.overrides === undefined || r.overrides === null)
    ) {
      throw refuseType('give preset or overrides');
    }
    const overrides = chainConfigJson('overrides', r.overrides);
    const preset = r.preset ?? null;
    const progress = r.onProgress ?? undefined;
    const cancel = r.cancel ?? undefined;
    const module = requireModule();
    return module.masterAudio(
      preset,
      r.samples,
      r.sampleRate,
      overrides,
      progress,
      cancel,
    ) as MasteringChainResult;
  });
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
  return withEntry('masterAudioStereo', () => {
    const r = requestOnly<MasterAudioStereoRequest>('masterAudioStereo', [request]);
    assertChainCallbacks(r);
    if (r.preset !== undefined && r.preset !== null) {
      assertString(r.preset, 'preset');
    }
    assertSampleRate(r.sampleRate);
    assertAudioInput(r.left, r.sampleRate, r, 'left');
    assertAudioInput(r.right, r.sampleRate, r, 'right');
    if (r.left.length !== r.right.length) {
      throw refuse('left and right must have the same length');
    }
    if (
      (r.preset === undefined || r.preset === null) &&
      (r.overrides === undefined || r.overrides === null)
    ) {
      throw refuseType('give preset or overrides');
    }
    const overrides = chainConfigJson('overrides', r.overrides);
    const preset = r.preset ?? null;
    const progress = r.onProgress ?? undefined;
    const cancel = r.cancel ?? undefined;
    const module = requireModule();
    return module.masterAudioStereo(
      preset,
      r.left,
      r.right,
      r.sampleRate,
      overrides,
      progress,
      cancel,
    ) as MasteringChainStereoResult;
  });
}
