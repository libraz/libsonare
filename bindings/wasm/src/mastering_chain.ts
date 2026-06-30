import { getSonareModule } from './module_state';
import type {
  MasteringChainConfig,
  MasteringChainResult,
  MasteringPreset,
  MasteringStereoChainResult,
} from './public_types';
import type { ProgressCallback } from './sonare.js';

function requireModule() {
  return getSonareModule();
}

/**
 * Apply a configurable mastering chain in WASM.
 *
 * @param samples - Audio samples (mono, float32)
 * @param sampleRate - Sample rate in Hz (default: 22050)
 * @param config - Chain stage configuration
 * @returns Processed audio, loudness metadata, and applied stage names
 */
export function masteringChain(
  samples: Float32Array,
  sampleRate = 22050,
  config: MasteringChainConfig,
): MasteringChainResult {
  return requireModule().masteringChain(samples, sampleRate, config as Record<string, unknown>);
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
  left: Float32Array,
  right: Float32Array,
  sampleRate = 22050,
  config: MasteringChainConfig,
): MasteringStereoChainResult {
  if (left.length !== right.length) {
    throw new Error('Stereo channel lengths must match.');
  }
  return requireModule().masteringChainStereo(
    left,
    right,
    sampleRate,
    config as Record<string, unknown>,
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
  samples: Float32Array,
  sampleRate = 22050,
  config: MasteringChainConfig,
  onProgress: ProgressCallback,
): MasteringChainResult {
  return requireModule().masteringChainWithProgress(
    samples,
    sampleRate,
    config as Record<string, unknown>,
    onProgress,
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
  left: Float32Array,
  right: Float32Array,
  sampleRate = 22050,
  config: MasteringChainConfig,
  onProgress: ProgressCallback,
): MasteringStereoChainResult {
  if (left.length !== right.length) {
    throw new Error('Stereo channel lengths must match.');
  }
  return requireModule().masteringChainStereoWithProgress(
    left,
    right,
    sampleRate,
    config as Record<string, unknown>,
    onProgress,
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
 * @param overrides - Optional flat overrides (dot-notation, e.g. `'loudness.targetLufs'`) applied on top of the preset. Pass `null` for preset defaults.
 * @returns Processed audio, loudness metadata, and applied stage names
 */
export function masterAudio(
  samples: Float32Array,
  sampleRate = 22050,
  presetName: MasteringPreset = 'pop',
  overrides: Record<string, number | boolean> = {},
): MasteringChainResult {
  return requireModule().masterAudio(presetName, samples, sampleRate, overrides);
}

/**
 * Apply a named mastering preset chain to stereo audio.
 *
 * @param left - Left channel samples
 * @param right - Right channel samples
 * @param sampleRate - Sample rate in Hz
 * @param presetName - Preset identifier from {@link masteringPresetNames}
 * @param overrides - Optional flat overrides (dot-notation, e.g. `'loudness.targetLufs'`) applied on top of the preset. Pass `null` for preset defaults.
 * @returns Processed stereo audio, loudness metadata, and applied stage names
 */
export function masterAudioStereo(
  left: Float32Array,
  right: Float32Array,
  sampleRate = 22050,
  presetName: MasteringPreset = 'pop',
  overrides: Record<string, number | boolean> = {},
): MasteringStereoChainResult {
  if (left.length !== right.length) {
    throw new Error('Stereo channel lengths must match.');
  }
  return requireModule().masterAudioStereo(presetName, left, right, sampleRate, overrides);
}

/**
 * Mono `masterAudio` with per-stage progress reporting. `onProgress` is invoked
 * with `(progress, stage)` between each chain stage (progress is in [0,1]).
 */
export function masterAudioWithProgress(
  samples: Float32Array,
  sampleRate = 22050,
  presetName: MasteringPreset,
  onProgress: ProgressCallback,
  overrides: Record<string, number | boolean> | null = null,
): MasteringChainResult {
  return requireModule().masterAudioWithProgress(
    presetName,
    samples,
    sampleRate,
    overrides,
    onProgress,
  );
}

/**
 * Stereo `masterAudio` with per-stage progress reporting.
 */
export function masterAudioStereoWithProgress(
  left: Float32Array,
  right: Float32Array,
  sampleRate = 22050,
  presetName: MasteringPreset,
  onProgress: ProgressCallback,
  overrides: Record<string, number | boolean> | null = null,
): MasteringStereoChainResult {
  if (left.length !== right.length) {
    throw new Error('Stereo channel lengths must match.');
  }
  return requireModule().masterAudioStereoWithProgress(
    presetName,
    left,
    right,
    sampleRate,
    overrides,
    onProgress,
  );
}
