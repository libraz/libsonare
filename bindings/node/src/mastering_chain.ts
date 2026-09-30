import { flattenChainConfig } from './_chain_config.js';
import { addon } from './native.js';
import type {
  LoudnessMatchResult,
  MasteringChainConfig,
  MasteringChainResult,
  MasteringChainStereoResult,
  MasteringOptions,
  MasteringPreset,
  MasteringResult,
  MasteringStereoResult,
  PairAnalysis,
  PairProcessor,
  ProgressCallback,
  SoloProcessor,
  StereoAnalysis,
} from './types.js';
import { assertSampleRate } from './validation.js';

export * from './mastering_assistant.js';
export * from './mastering_catalog.js';
export * from './mastering_normalize.js';

/** Canonical request form for loudness/true-peak mastering. */
export interface MasteringRequest extends MasteringOptions {
  samples: Float32Array;
  sampleRate?: number;
}

export interface MasteringProcessRequest {
  processorName: SoloProcessor;
  samples: Float32Array;
  sampleRate?: number;
  params?: Record<string, number | boolean>;
}

export interface MasteringProcessStereoRequest {
  processorName: SoloProcessor;
  left: Float32Array;
  right: Float32Array;
  sampleRate?: number;
  params?: Record<string, number | boolean>;
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

export interface MasteringPairProcessRequest {
  processorName: PairProcessor;
  source: Float32Array;
  reference: Float32Array;
  sampleRate?: number;
  params?: Record<string, number | boolean>;
}

/** Canonical request form for {@link masteringAbMatchLoudness}. */
export interface MasteringAbMatchLoudnessRequest {
  /** The take to gain-match. Returned in `samples` with the gain applied. */
  source: Float32Array;
  /** The take whose integrated loudness `source` is matched to. */
  reference: Float32Array;
  sampleRate?: number;
}

export interface MasteringPairAnalyzeRequest {
  analysisName: PairAnalysis;
  source: Float32Array;
  reference: Float32Array;
  sampleRate?: number;
  params?: Record<string, number | boolean>;
}

export interface MasteringStereoAnalyzeRequest {
  analysisName: StereoAnalysis;
  left: Float32Array;
  right: Float32Array;
  sampleRate?: number;
  params?: Record<string, number | boolean>;
}

export function mastering(request: MasteringRequest): MasteringResult;
export function mastering(
  samples: Float32Array,
  sampleRate?: number,
  options?: MasteringOptions,
): MasteringResult;
export function mastering(
  samples: MasteringRequest | Float32Array,
  sampleRate = 22050,
  options: MasteringOptions = {},
): MasteringResult {
  const request = samples instanceof Float32Array ? { samples, sampleRate, ...options } : samples;
  const resolvedSampleRate = request.sampleRate ?? 22050;
  assertSampleRate('mastering', resolvedSampleRate);
  return addon.mastering(
    request.samples,
    resolvedSampleRate,
    request.targetLufs ?? -14.0,
    request.ceilingDb ?? -1.0,
    request.truePeakOversample ?? 4,
    request.releaseMs ?? 0, // 0 => library default (50 ms)
    request.applyGainAtInputRate ?? false,
  );
}

export function masteringProcess(request: MasteringProcessRequest): MasteringResult;
export function masteringProcess(
  processorName: SoloProcessor,
  samples: Float32Array,
  sampleRate?: number,
  params?: Record<string, number | boolean>,
): MasteringResult;
export function masteringProcess(
  processorName: SoloProcessor | MasteringProcessRequest,
  samples?: Float32Array,
  sampleRate = 22050,
  params: Record<string, number | boolean> = {},
): MasteringResult {
  const request =
    typeof processorName === 'string'
      ? { processorName, samples: samples as Float32Array, sampleRate, params }
      : processorName;
  const resolvedSampleRate = request.sampleRate ?? 22050;
  assertSampleRate('masteringProcess', resolvedSampleRate);
  return addon.masteringProcess(
    request.processorName,
    request.samples,
    resolvedSampleRate,
    request.params ?? {},
  );
}

export function masteringProcessStereo(
  request: MasteringProcessStereoRequest,
): MasteringStereoResult;
export function masteringProcessStereo(
  processorName: SoloProcessor,
  left: Float32Array,
  right: Float32Array,
  sampleRate?: number,
  params?: Record<string, number | boolean>,
): MasteringStereoResult;
export function masteringProcessStereo(
  processorName: SoloProcessor | MasteringProcessStereoRequest,
  left?: Float32Array,
  right?: Float32Array,
  sampleRate = 22050,
  params: Record<string, number | boolean> = {},
): MasteringStereoResult {
  const request =
    typeof processorName === 'string'
      ? {
          processorName,
          left: left as Float32Array,
          right: right as Float32Array,
          sampleRate,
          params,
        }
      : processorName;
  const resolvedSampleRate = request.sampleRate ?? 22050;
  assertSampleRate('masteringProcessStereo', resolvedSampleRate);
  return addon.masteringProcessStereo(
    request.processorName,
    request.left,
    request.right,
    resolvedSampleRate,
    request.params ?? {},
  );
}

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
  const flat = flattenChainConfig(request.config ?? {});
  const resolvedSampleRate = request.sampleRate ?? 22050;
  assertSampleRate('masteringChain', resolvedSampleRate);
  if (request.onProgress || request.cancel) {
    return addon.masteringChainWithProgress(
      request.samples,
      resolvedSampleRate,
      flat,
      request.onProgress ?? (() => {}),
      request.cancel ?? (() => false),
    );
  }
  return addon.masteringChain(request.samples, resolvedSampleRate, flat);
}

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
  const flat = flattenChainConfig(request.config ?? {});
  const resolvedSampleRate = request.sampleRate ?? 22050;
  assertSampleRate('masteringChainStereo', resolvedSampleRate);
  if (request.onProgress || request.cancel) {
    return addon.masteringChainStereoWithProgress(
      request.left,
      request.right,
      resolvedSampleRate,
      flat,
      request.onProgress ?? (() => {}),
      request.cancel ?? (() => false),
    );
  }
  return addon.masteringChainStereo(request.left, request.right, resolvedSampleRate, flat);
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
  overrides: MasteringChainConfig,
  onProgress?: ProgressCallback,
): Required<Pick<MasterAudioRequest, 'samples'>> & Omit<MasterAudioRequest, 'samples'> {
  if (requestOrSamples instanceof Float32Array) {
    return { samples: requestOrSamples, sampleRate, preset, overrides, onProgress };
  }
  return requestOrSamples;
}

function masterAudioStereoRequest(
  requestOrLeft: MasterAudioStereoRequest | Float32Array,
  right: Float32Array | undefined,
  sampleRate: number,
  preset: MasteringPreset,
  overrides: MasteringChainConfig,
  onProgress?: ProgressCallback,
): MasterAudioStereoRequest {
  if (requestOrLeft instanceof Float32Array) {
    return {
      left: requestOrLeft,
      right: right as Float32Array,
      sampleRate,
      preset,
      overrides,
      onProgress,
    };
  }
  return requestOrLeft;
}

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
  const resolvedSampleRate = request.sampleRate ?? 22050;
  assertSampleRate('masterAudio', resolvedSampleRate);
  if (request.onProgress || request.cancel) {
    return addon.masterAudioWithProgress(
      request.preset ?? 'pop',
      request.samples,
      resolvedSampleRate,
      flat,
      request.onProgress ?? (() => {}),
      request.cancel ?? (() => false),
    );
  }
  return addon.masterAudio(request.preset ?? 'pop', request.samples, resolvedSampleRate, flat);
}

/**
 * Asynchronous variant of {@link masterAudio}. Runs the full chain on a libuv
 * worker thread; the returned promise resolves with the same shape as the
 * synchronous version. Progress reporting and cancellation callbacks are not
 * available on the async path (use the synchronous `masterAudio` with
 * `onProgress`/`cancel` if you need them, or spin up multiple async calls in
 * parallel).
 */
export function masterAudioAsync(
  request: Omit<MasterAudioRequest, 'onProgress' | 'cancel'>,
): Promise<MasteringChainResult>;
export function masterAudioAsync(
  samples: Float32Array,
  sampleRate?: number,
  presetName?: MasteringPreset,
  overrides?: MasteringChainConfig,
): Promise<MasteringChainResult>;
export function masterAudioAsync(
  samples: Omit<MasterAudioRequest, 'onProgress' | 'cancel'> | Float32Array,
  sampleRate = 22050,
  presetName: MasteringPreset = 'pop',
  overrides: MasteringChainConfig = {},
): Promise<MasteringChainResult> {
  // Preserve the async validation contract: invalid input is handed to the addon
  // so it becomes a rejected Promise, not a synchronous property-access error
  // while normalizing the new request form.
  if (!(samples instanceof Float32Array) && (!samples || typeof samples !== 'object')) {
    return addon.masterAudioAsync(presetName, samples as unknown as Float32Array, sampleRate, {});
  }
  // Normalizing and flattening can throw on a malformed request (e.g. a
  // non-numeric override leaf); route that through the same rejected-Promise
  // contract so `fn(...).catch(h)` sees every validation failure.
  try {
    const request = masterAudioRequest(samples, sampleRate, presetName, overrides);
    const resolvedSampleRate = request.sampleRate ?? 22050;
    assertSampleRate('masterAudioAsync', resolvedSampleRate);
    return addon.masterAudioAsync(
      request.preset ?? 'pop',
      request.samples,
      resolvedSampleRate,
      flattenChainConfig(request.overrides ?? {}),
    );
  } catch (error) {
    return Promise.reject(error);
  }
}

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
  const resolvedSampleRate = request.sampleRate ?? 22050;
  assertSampleRate('masterAudioStereo', resolvedSampleRate);
  if (request.onProgress || request.cancel) {
    return addon.masterAudioStereoWithProgress(
      request.preset ?? 'pop',
      request.left,
      request.right,
      resolvedSampleRate,
      flat,
      request.onProgress ?? (() => {}),
      request.cancel ?? (() => false),
    );
  }
  return addon.masterAudioStereo(
    request.preset ?? 'pop',
    request.left,
    request.right,
    resolvedSampleRate,
    flat,
  );
}

/**
 * Asynchronous variant of {@link masterAudioStereo}.
 */
export function masterAudioStereoAsync(
  request: Omit<MasterAudioStereoRequest, 'onProgress' | 'cancel'>,
): Promise<MasteringChainStereoResult>;
export function masterAudioStereoAsync(
  left: Float32Array,
  right: Float32Array,
  sampleRate?: number,
  presetName?: MasteringPreset,
  overrides?: MasteringChainConfig,
): Promise<MasteringChainStereoResult>;
export function masterAudioStereoAsync(
  left: Omit<MasterAudioStereoRequest, 'onProgress' | 'cancel'> | Float32Array,
  right: Float32Array | undefined = undefined,
  sampleRate = 22050,
  presetName: MasteringPreset = 'pop',
  overrides: MasteringChainConfig = {},
): Promise<MasteringChainStereoResult> {
  // Preserve the async validation contract: invalid input is handed to the addon
  // so it becomes a rejected Promise, not a synchronous property-access error
  // while normalizing the new request form.
  if (!(left instanceof Float32Array) && (!left || typeof left !== 'object')) {
    return addon.masterAudioStereoAsync(
      presetName,
      left as unknown as Float32Array,
      right as Float32Array,
      sampleRate,
      {},
    );
  }
  // Normalizing and flattening can throw on a malformed request (e.g. a
  // non-numeric override leaf); route that through the same rejected-Promise
  // contract so `fn(...).catch(h)` sees every validation failure.
  try {
    const request = masterAudioStereoRequest(left, right, sampleRate, presetName, overrides);
    const resolvedSampleRate = request.sampleRate ?? 22050;
    assertSampleRate('masterAudioStereoAsync', resolvedSampleRate);
    return addon.masterAudioStereoAsync(
      request.preset ?? 'pop',
      request.left,
      request.right,
      resolvedSampleRate,
      flattenChainConfig(request.overrides ?? {}),
    );
  } catch (error) {
    return Promise.reject(error);
  }
}

/**
 * Apply a two-input `match.*` processor. `source` and `reference` may have
 * independent lengths — the match primitives consume each buffer at its own
 * length.
 */
export function masteringPairProcess(request: MasteringPairProcessRequest): MasteringResult;
export function masteringPairProcess(
  processorName: PairProcessor,
  source: Float32Array,
  reference: Float32Array,
  sampleRate?: number,
  params?: Record<string, number | boolean>,
): MasteringResult;
export function masteringPairProcess(
  processorName: PairProcessor | MasteringPairProcessRequest,
  source?: Float32Array,
  reference?: Float32Array,
  sampleRate = 22050,
  params: Record<string, number | boolean> = {},
): MasteringResult {
  const request =
    typeof processorName === 'string'
      ? {
          processorName,
          source: source as Float32Array,
          reference: reference as Float32Array,
          sampleRate,
          params,
        }
      : processorName;
  const resolvedSampleRate = request.sampleRate ?? 22050;
  assertSampleRate('masteringPairProcess', resolvedSampleRate);
  return addon.masteringPairProcess(
    request.processorName,
    request.source,
    request.reference,
    resolvedSampleRate,
    request.params ?? {},
  );
}

/**
 * Gain-match `source` to `reference`'s BS.1770 integrated loudness, so an A/B
 * between the two is not decided by level. The buffers may have independent
 * lengths — each is measured at its own.
 *
 * The gain is not clamped; {@link LoudnessMatchResult} documents what each
 * returned field then reports.
 *
 * @example
 * ```ts
 * const matched = masteringAbMatchLoudness({ source: take, reference: mix, sampleRate: 44100 });
 * if (matched.matchedTruePeakDbtp > -1) {
 *   // the match pushed the take above the delivery ceiling
 * }
 * ```
 */
export function masteringAbMatchLoudness(
  request: MasteringAbMatchLoudnessRequest,
): LoudnessMatchResult {
  const resolvedSampleRate = request.sampleRate ?? 22050;
  assertSampleRate('masteringAbMatchLoudness', resolvedSampleRate);
  return addon.masteringAbMatchLoudness(request.source, request.reference, resolvedSampleRate);
}

/**
 * Analyze a `source` against a `reference` with a two-input analysis. The two
 * buffers may have independent lengths.
 */
export function masteringPairAnalyze(request: MasteringPairAnalyzeRequest): string;
export function masteringPairAnalyze(
  analysisName: PairAnalysis,
  source: Float32Array,
  reference: Float32Array,
  sampleRate?: number,
  params?: Record<string, number | boolean>,
): string;
export function masteringPairAnalyze(
  analysisName: PairAnalysis | MasteringPairAnalyzeRequest,
  source?: Float32Array,
  reference?: Float32Array,
  sampleRate = 22050,
  params: Record<string, number | boolean> = {},
): string {
  const request =
    typeof analysisName === 'string'
      ? {
          analysisName,
          source: source as Float32Array,
          reference: reference as Float32Array,
          sampleRate,
          params,
        }
      : analysisName;
  const resolvedSampleRate = request.sampleRate ?? 22050;
  assertSampleRate('masteringPairAnalyze', resolvedSampleRate);
  return addon.masteringPairAnalyze(
    request.analysisName,
    request.source,
    request.reference,
    resolvedSampleRate,
    request.params ?? {},
  );
}

export function masteringStereoAnalyze(request: MasteringStereoAnalyzeRequest): string;
export function masteringStereoAnalyze(
  analysisName: StereoAnalysis,
  left: Float32Array,
  right: Float32Array,
  sampleRate?: number,
  params?: Record<string, number | boolean>,
): string;
export function masteringStereoAnalyze(
  analysisName: StereoAnalysis | MasteringStereoAnalyzeRequest,
  left?: Float32Array,
  right?: Float32Array,
  sampleRate = 22050,
  params: Record<string, number | boolean> = {},
): string {
  const request =
    typeof analysisName === 'string'
      ? {
          analysisName,
          left: left as Float32Array,
          right: right as Float32Array,
          sampleRate,
          params,
        }
      : analysisName;
  const resolvedSampleRate = request.sampleRate ?? 22050;
  assertSampleRate('masteringStereoAnalyze', resolvedSampleRate);
  return addon.masteringStereoAnalyze(
    request.analysisName,
    request.left,
    request.right,
    resolvedSampleRate,
    request.params ?? {},
  );
}
