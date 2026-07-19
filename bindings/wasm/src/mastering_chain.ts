import { flattenChainConfig } from './_chain_config';
import { getSonareModule } from './module_state';
import type {
  MasteringChainConfig,
  MasteringChainResult,
  MasteringChainStereoResult,
  MasteringPreset,
} from './public_types';
import type { ProgressCallback } from './sonare.js';

function requireModule() {
  return getSonareModule();
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
}

export interface MasteringChainStereoRequest {
  left: Float32Array;
  right: Float32Array;
  sampleRate?: number;
  config?: MasteringChainConfig;
  onProgress?: ProgressCallback;
}

/** Canonical request form for one-shot preset mastering. */
export interface MasterAudioRequest {
  samples: Float32Array;
  sampleRate?: number;
  preset?: MasteringPreset;
  overrides?: MasteringChainConfig;
  onProgress?: ProgressCallback;
}

/** Canonical request form for one-shot stereo preset mastering. */
export interface MasterAudioStereoRequest {
  left: Float32Array;
  right: Float32Array;
  sampleRate?: number;
  preset?: MasteringPreset;
  overrides?: MasteringChainConfig;
  onProgress?: ProgressCallback;
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
  if (request.onProgress) {
    return requireModule().masteringChainWithProgress(
      request.samples,
      request.sampleRate ?? 22050,
      canonicalChainConfig(request.config ?? {}),
      request.onProgress,
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
  if (request.onProgress) {
    return requireModule().masteringChainStereoWithProgress(
      request.left,
      request.right,
      request.sampleRate ?? 22050,
      canonicalChainConfig(request.config ?? {}),
      request.onProgress,
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
  if (request.onProgress) {
    return requireModule().masterAudioWithProgress(
      request.preset ?? 'pop',
      request.samples,
      request.sampleRate ?? 22050,
      flat,
      request.onProgress,
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
  if (request.onProgress) {
    return requireModule().masterAudioStereoWithProgress(
      request.preset ?? 'pop',
      request.left,
      request.right,
      request.sampleRate ?? 22050,
      flat,
      request.onProgress,
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
  );
}
