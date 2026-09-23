import { flattenChainConfig } from './_chain_config';
import { getSonareModule } from './module_state';
import type {
  MasteringChainConfig,
  MasteringChainResult,
  MasteringChainStereoResult,
  MasteringPreset,
} from './public_types';
import type { ProgressCallback } from './sonare.js';
import type { ValidateOptions } from './validation';
import { assertSamples } from './validation';

function requireModule() {
  return getSonareModule();
}

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

export interface NormalizeRequest extends ValidateOptions {
  samples: Float32Array;
  sampleRate?: number;
  targetDb?: number;
  mode?: NormalizeMode;
}

/**
 * Normalize audio to a target peak or RMS level.
 *
 * @param samples - Audio samples (mono, float32)
 * @param sampleRate - Sample rate in Hz (default: 22050)
 * @param targetDb - Finite target at or below 0 dBFS (default: 0 dB = full scale).
 *   For `mode: 'peak'`, this is the peak target; for `mode: 'rms'`, this is the RMS target.
 * @param mode - Normalization mode: `'peak'` (default) or `'rms'`.
 * @returns Normalized audio
 */
export function normalize(request: NormalizeRequest): Float32Array;
export function normalize(
  samples: Float32Array,
  sampleRate: number,
  targetDb?: number,
  options?: ValidateOptions,
): Float32Array;
export function normalize(
  samples: Float32Array,
  sampleRate: number,
  targetDb?: number,
  mode?: NormalizeMode,
  options?: ValidateOptions,
): Float32Array;
export function normalize(
  samples: Float32Array | NormalizeRequest,
  sampleRate?: number,
  targetDb = 0.0,
  modeOrOptions: NormalizeMode | ValidateOptions = 'peak',
  options: ValidateOptions = {},
): Float32Array {
  if (
    modeOrOptions !== undefined &&
    modeOrOptions !== null &&
    typeof modeOrOptions !== 'string' &&
    typeof modeOrOptions !== 'object'
  ) {
    throw new TypeError("normalize: mode must be the string 'peak' or 'rms'");
  }
  if (modeOrOptions === null) {
    throw new TypeError("normalize: mode must be the string 'peak' or 'rms'");
  }
  const positionalOptions =
    typeof modeOrOptions === 'object' && modeOrOptions !== null ? modeOrOptions : options;
  const positionalMode = typeof modeOrOptions === 'string' ? modeOrOptions : undefined;
  const request: NormalizeRequest =
    samples instanceof Float32Array
      ? { samples, sampleRate, targetDb, mode: positionalMode, ...positionalOptions }
      : samples;
  assertSamples('normalize', request.samples, request.validate !== false);
  const mode = resolveNormalizeMode(request.mode);
  return requireModule().normalizeEx(
    request.samples,
    request.sampleRate ?? 22050,
    request.targetDb ?? 0.0,
    mode,
  );
}

export interface NormalizeStereoRequest extends ValidateOptions {
  left: Float32Array;
  right: Float32Array;
  sampleRate?: number;
  targetDb?: number;
  mode?: NormalizeMode;
}

/** A normalized channel pair and the gain both channels were moved by. */
export interface NormalizeStereoResult {
  left: Float32Array;
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
 * @param request.sampleRate - Sample rate in Hz (default: 22050)
 * @param request.targetDb - Finite target at or below 0 dBFS. Defaults to 0 for
 *   `mode: 'peak'` and -20 for `mode: 'rms'`, matching the library and the other
 *   language surfaces.
 * @param request.mode - `'peak'` (default) or `'rms'`
 * @returns The normalized pair and the shared gain in dB
 * @throws RangeError when the two channels differ in length
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
  assertSamples('normalizeStereo', request.left, request.validate !== false);
  assertSamples('normalizeStereo', request.right, request.validate !== false);
  if (request.left.length !== request.right.length) {
    throw new RangeError('Stereo channel lengths must match.');
  }
  const mode = resolveNormalizeMode(request.mode, 'normalizeStereo');
  // Mode-dependent, unlike the mono `normalize` on this surface, which defaults
  // to 0 dB in both modes. 0 dBFS RMS is not a usable default -- the peaks sit
  // well above the RMS, so every one of them clips -- and the library and the
  // other surfaces all default RMS to -20. A new entry point takes the shared
  // default rather than inheriting a surface-local one.
  const targetDb = request.targetDb ?? (mode === 'rms' ? -20.0 : 0.0);
  return requireModule().normalizeStereo(
    request.left,
    request.right,
    request.sampleRate ?? 22050,
    targetDb,
    mode,
  );
}

/** Internal envelope selecting the core dotted-param parser in the embind layer. */
function canonicalChainConfig(config: MasteringChainConfig): Record<string, unknown> {
  return { __flatParams: flattenChainConfig(config) };
}

export interface MasteringChainRequest {
  samples: Float32Array;
  sampleRate?: number;
  config?: MasteringChainConfig;
  onProgress?: ProgressCallback;
  cancel?: () => boolean;
}

export interface MasteringChainStereoRequest {
  left: Float32Array;
  right: Float32Array;
  sampleRate?: number;
  config?: MasteringChainConfig;
  onProgress?: ProgressCallback;
  cancel?: () => boolean;
}

/** Canonical request form for one-shot preset mastering. */
export interface MasterAudioRequest {
  samples: Float32Array;
  sampleRate?: number;
  preset?: MasteringPreset;
  overrides?: MasteringChainConfig;
  onProgress?: ProgressCallback;
  cancel?: () => boolean;
}

/** Canonical request form for one-shot stereo preset mastering. */
export interface MasterAudioStereoRequest {
  left: Float32Array;
  right: Float32Array;
  sampleRate?: number;
  preset?: MasteringPreset;
  overrides?: MasteringChainConfig;
  onProgress?: ProgressCallback;
  cancel?: () => boolean;
}

function masterAudioRequest(
  requestOrSamples: MasterAudioRequest | Float32Array,
  sampleRate: number,
  preset: MasteringPreset,
  overrides: MasteringChainConfig | null,
  onProgress?: ProgressCallback,
): MasterAudioRequest {
  if (requestOrSamples instanceof Float32Array) {
    return {
      samples: requestOrSamples,
      sampleRate,
      preset,
      overrides: overrides ?? {},
      onProgress,
    };
  }
  return requestOrSamples;
}

function masterAudioStereoRequest(
  requestOrLeft: MasterAudioStereoRequest | Float32Array,
  right: Float32Array | undefined,
  sampleRate: number,
  preset: MasteringPreset,
  overrides: MasteringChainConfig | null,
  onProgress?: ProgressCallback,
): MasterAudioStereoRequest {
  if (requestOrLeft instanceof Float32Array) {
    return {
      left: requestOrLeft,
      right: right as Float32Array,
      sampleRate,
      preset,
      overrides: overrides ?? {},
      onProgress,
    };
  }
  return requestOrLeft;
}

/**
 * Apply a configurable mastering chain in WASM.
 *
 * @param samples - Audio samples (mono, float32)
 * @param sampleRate - Sample rate in Hz (default: 22050)
 * @param config - Chain stage configuration
 * @returns Processed audio, loudness metadata, and applied stage names
 */
export function masteringChain(request: MasteringChainRequest): MasteringChainResult;
export function masteringChain(
  samples: Float32Array,
  sampleRate?: number,
  config?: MasteringChainConfig,
  onProgress?: ProgressCallback,
): MasteringChainResult;
export function masteringChain(
  samples: Float32Array | MasteringChainRequest,
  sampleRate = 22050,
  config: MasteringChainConfig = {},
  onProgress?: ProgressCallback,
): MasteringChainResult {
  const request =
    samples instanceof Float32Array ? { samples, sampleRate, config, onProgress } : samples;
  if (request.onProgress || request.cancel) {
    return requireModule().masteringChainWithProgress(
      request.samples,
      request.sampleRate ?? 22050,
      canonicalChainConfig(request.config ?? {}),
      request.onProgress ?? (() => {}),
      request.cancel ?? (() => false),
    );
  }
  return requireModule().masteringChain(
    request.samples,
    request.sampleRate ?? 22050,
    canonicalChainConfig(request.config ?? {}),
  );
}

/**
 * Apply a configurable stereo mastering chain in WASM.
 *
 * @param left - Left channel samples
 * @param right - Right channel samples
 * @param sampleRate - Sample rate in Hz
 * @param config - Chain stage configuration
 * @returns Processed stereo audio, loudness metadata, and applied stage names
 */
export function masteringChainStereo(
  request: MasteringChainStereoRequest,
): MasteringChainStereoResult;
export function masteringChainStereo(
  left: Float32Array,
  right: Float32Array,
  sampleRate?: number,
  config?: MasteringChainConfig,
  onProgress?: ProgressCallback,
): MasteringChainStereoResult;
export function masteringChainStereo(
  left: Float32Array | MasteringChainStereoRequest,
  right?: Float32Array,
  sampleRate = 22050,
  config: MasteringChainConfig = {},
  onProgress?: ProgressCallback,
): MasteringChainStereoResult {
  const request =
    left instanceof Float32Array
      ? { left, right: right as Float32Array, sampleRate, config, onProgress }
      : left;
  if (request.left.length !== request.right.length) {
    throw new Error('Stereo channel lengths must match.');
  }
  if (request.onProgress || request.cancel) {
    return requireModule().masteringChainStereoWithProgress(
      request.left,
      request.right,
      request.sampleRate ?? 22050,
      canonicalChainConfig(request.config ?? {}),
      request.onProgress ?? (() => {}),
      request.cancel ?? (() => false),
    );
  }
  return requireModule().masteringChainStereo(
    request.left,
    request.right,
    request.sampleRate ?? 22050,
    canonicalChainConfig(request.config ?? {}),
  );
}

/**
 * Apply a configurable mastering chain in WASM with progress reporting.
 *
 * @param samples - Audio samples (mono, float32)
 * @param sampleRate - Sample rate in Hz (default: 22050)
 * @param config - Chain stage configuration
 * @param onProgress - Progress callback (progress: 0-1, stage: string)
 * @returns Processed audio, loudness metadata, and applied stage names
 */
export function masteringChainWithProgress(
  request: MasteringChainRequest & Required<Pick<MasteringChainRequest, 'onProgress'>>,
): MasteringChainResult;
export function masteringChainWithProgress(
  samples: Float32Array,
  sampleRate?: number,
  config?: MasteringChainConfig,
  onProgress?: ProgressCallback,
): MasteringChainResult;
export function masteringChainWithProgress(
  samples: Float32Array | MasteringChainRequest,
  sampleRate = 22050,
  config: MasteringChainConfig = {},
  onProgress?: ProgressCallback,
): MasteringChainResult {
  const request =
    samples instanceof Float32Array ? { samples, sampleRate, config, onProgress } : samples;
  if (!request.onProgress) {
    throw new TypeError('masteringChainWithProgress: onProgress is required');
  }
  return requireModule().masteringChainWithProgress(
    request.samples,
    request.sampleRate ?? 22050,
    canonicalChainConfig(request.config ?? {}),
    request.onProgress,
    request.cancel ?? (() => false),
  );
}

/**
 * Apply a configurable stereo mastering chain in WASM with progress reporting.
 *
 * @param left - Left channel samples
 * @param right - Right channel samples
 * @param sampleRate - Sample rate in Hz
 * @param config - Chain stage configuration
 * @param onProgress - Progress callback (progress: 0-1, stage: string)
 * @returns Processed stereo audio, loudness metadata, and applied stage names
 */
export function masteringChainStereoWithProgress(
  request: MasteringChainStereoRequest & Required<Pick<MasteringChainStereoRequest, 'onProgress'>>,
): MasteringChainStereoResult;
export function masteringChainStereoWithProgress(
  left: Float32Array,
  right: Float32Array,
  sampleRate?: number,
  config?: MasteringChainConfig,
  onProgress?: ProgressCallback,
): MasteringChainStereoResult;
export function masteringChainStereoWithProgress(
  left: Float32Array | MasteringChainStereoRequest,
  right?: Float32Array,
  sampleRate = 22050,
  config: MasteringChainConfig = {},
  onProgress?: ProgressCallback,
): MasteringChainStereoResult {
  const request =
    left instanceof Float32Array
      ? { left, right: right as Float32Array, sampleRate, config, onProgress }
      : left;
  if (!request.onProgress) {
    throw new TypeError('masteringChainStereoWithProgress: onProgress is required');
  }
  if (request.left.length !== request.right.length) {
    throw new Error('Stereo channel lengths must match.');
  }
  return requireModule().masteringChainStereoWithProgress(
    request.left,
    request.right,
    request.sampleRate ?? 22050,
    canonicalChainConfig(request.config ?? {}),
    request.onProgress,
    request.cancel ?? (() => false),
  );
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
 * The flat `{key: number|boolean}` params of preset `preset`'s built-in chain
 * configuration, in the same key space {@link masteringAssistantSuggestChain}
 * returns. Passing this straight through as `overrides` to {@link masterAudio}
 * reproduces the preset unchanged, bit for bit in the C++ core.
 *
 * @param preset - Preset identifier from {@link masteringPresetNames}.
 * @throws For an unknown `preset`.
 */
export function masteringPresetParams(preset: MasteringPreset): Record<string, number | boolean> {
  return requireModule().masteringPresetParams(preset);
}

/**
 * List the delivery targets the mastering assistant accepts as `targetPlatform`.
 *
 * Read from the library rather than from a list kept here, so a target added in
 * the core is discoverable without a binding change.
 *
 * @returns Target names in index order (e.g. "streaming", "broadcast", "club")
 */
export function masteringPlatformNames(): string[] {
  return Array.from(requireModule().masteringPlatformNames());
}

/**
 * Apply a named mastering preset chain to mono audio.
 *
 * @param samples - Audio samples (mono, float32)
 * @param sampleRate - Sample rate in Hz (default: 22050)
 * @param presetName - Preset identifier from {@link masteringPresetNames}
 * @param overrides - Optional nested {@link MasteringChainConfig} applied on top of the preset (e.g. `{ loudness: { targetLufs: -14 } }`). Pass `null` for preset defaults.
 * @param onProgress - Optional per-stage progress callback (progress: 0-1, stage: string).
 * @returns Processed audio, loudness metadata, and applied stage names
 */
export function masterAudio(request: MasterAudioRequest): MasteringChainResult;
export function masterAudio(
  samples: Float32Array,
  sampleRate?: number,
  presetName?: MasteringPreset,
  overrides?: MasteringChainConfig,
  onProgress?: ProgressCallback,
): MasteringChainResult;
export function masterAudio(
  samples: MasterAudioRequest | Float32Array,
  sampleRate = 22050,
  presetName: MasteringPreset = 'pop',
  overrides: MasteringChainConfig = {},
  onProgress?: ProgressCallback,
): MasteringChainResult {
  const request = masterAudioRequest(samples, sampleRate, presetName, overrides, onProgress);
  const flat = flattenChainConfig(request.overrides ?? {});
  if (request.onProgress || request.cancel) {
    return requireModule().masterAudioWithProgress(
      request.preset ?? 'pop',
      request.samples,
      request.sampleRate ?? 22050,
      flat,
      request.onProgress ?? (() => {}),
      request.cancel ?? (() => false),
    );
  }
  return requireModule().masterAudio(
    request.preset ?? 'pop',
    request.samples,
    request.sampleRate ?? 22050,
    flat,
  );
}

/**
 * Apply a named mastering preset chain to stereo audio.
 *
 * @param left - Left channel samples
 * @param right - Right channel samples
 * @param sampleRate - Sample rate in Hz
 * @param presetName - Preset identifier from {@link masteringPresetNames}
 * @param overrides - Optional nested {@link MasteringChainConfig} applied on top of the preset (e.g. `{ loudness: { targetLufs: -14 } }`). Pass `null` for preset defaults.
 * @param onProgress - Optional per-stage progress callback (progress: 0-1, stage: string).
 * @returns Processed stereo audio, loudness metadata, and applied stage names
 */
export function masterAudioStereo(request: MasterAudioStereoRequest): MasteringChainStereoResult;
export function masterAudioStereo(
  left: Float32Array,
  right: Float32Array,
  sampleRate?: number,
  presetName?: MasteringPreset,
  overrides?: MasteringChainConfig,
  onProgress?: ProgressCallback,
): MasteringChainStereoResult;
export function masterAudioStereo(
  left: MasterAudioStereoRequest | Float32Array,
  right: Float32Array | undefined = undefined,
  sampleRate = 22050,
  presetName: MasteringPreset = 'pop',
  overrides: MasteringChainConfig = {},
  onProgress?: ProgressCallback,
): MasteringChainStereoResult {
  const request = masterAudioStereoRequest(
    left,
    right,
    sampleRate,
    presetName,
    overrides,
    onProgress,
  );
  const flat = flattenChainConfig(request.overrides ?? {});
  if (request.left.length !== request.right.length) {
    throw new Error('Stereo channel lengths must match.');
  }
  if (request.onProgress || request.cancel) {
    return requireModule().masterAudioStereoWithProgress(
      request.preset ?? 'pop',
      request.left,
      request.right,
      request.sampleRate ?? 22050,
      flat,
      request.onProgress ?? (() => {}),
      request.cancel ?? (() => false),
    );
  }
  return requireModule().masterAudioStereo(
    request.preset ?? 'pop',
    request.left,
    request.right,
    request.sampleRate ?? 22050,
    flat,
  );
}

/**
 * Mono `masterAudio` with per-stage progress reporting. `onProgress` is invoked
 * with `(progress, stage)` between each chain stage (progress is in [0,1]).
 */
export function masterAudioWithProgress(
  request: MasterAudioRequest & Required<Pick<MasterAudioRequest, 'onProgress'>>,
): MasteringChainResult;
export function masterAudioWithProgress(
  samples: Float32Array,
  sampleRate?: number,
  presetName?: MasteringPreset,
  overrides?: MasteringChainConfig | null,
  onProgress?: ProgressCallback,
): MasteringChainResult;
export function masterAudioWithProgress(
  samples: MasterAudioRequest | Float32Array,
  sampleRate = 22050,
  presetName: MasteringPreset = 'pop',
  overrides: MasteringChainConfig | null = null,
  onProgress?: ProgressCallback,
): MasteringChainResult {
  const request = masterAudioRequest(samples, sampleRate, presetName, overrides, onProgress);
  if (!request.onProgress) {
    throw new TypeError('masterAudioWithProgress: onProgress is required');
  }
  return requireModule().masterAudioWithProgress(
    request.preset ?? 'pop',
    request.samples,
    request.sampleRate ?? 22050,
    flattenChainConfig(request.overrides ?? {}),
    request.onProgress,
    request.cancel ?? (() => false),
  );
}

/**
 * Stereo `masterAudio` with per-stage progress reporting.
 */
export function masterAudioStereoWithProgress(
  request: MasterAudioStereoRequest & Required<Pick<MasterAudioStereoRequest, 'onProgress'>>,
): MasteringChainStereoResult;
export function masterAudioStereoWithProgress(
  left: Float32Array,
  right: Float32Array,
  sampleRate?: number,
  presetName?: MasteringPreset,
  overrides?: MasteringChainConfig | null,
  onProgress?: ProgressCallback,
): MasteringChainStereoResult;
export function masterAudioStereoWithProgress(
  left: MasterAudioStereoRequest | Float32Array,
  right: Float32Array | undefined = undefined,
  sampleRate = 22050,
  presetName: MasteringPreset = 'pop',
  overrides: MasteringChainConfig | null = null,
  onProgress?: ProgressCallback,
): MasteringChainStereoResult {
  const request = masterAudioStereoRequest(
    left,
    right,
    sampleRate,
    presetName,
    overrides,
    onProgress,
  );
  if (!request.onProgress) {
    throw new TypeError('masterAudioStereoWithProgress: onProgress is required');
  }
  if (request.left.length !== request.right.length) {
    throw new Error('Stereo channel lengths must match.');
  }
  return requireModule().masterAudioStereoWithProgress(
    request.preset ?? 'pop',
    request.left,
    request.right,
    request.sampleRate ?? 22050,
    flattenChainConfig(request.overrides ?? {}),
    request.onProgress,
    request.cancel ?? (() => false),
  );
}
