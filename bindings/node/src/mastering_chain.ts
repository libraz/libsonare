import { flattenChainConfig } from './_chain_config.js';
import { type MasteringSoloProcessorParams, resolveProcessorParams } from './_processor_params.js';
import { addon } from './native.js';
import type {
  LoudnessMatchResult,
  LoudnessMatchStereoResult,
  MasteringChainConfig,
  MasteringChainResult,
  MasteringChainStereoResult,
  MasteringOptions,
  MasteringPreset,
  MasteringResult,
  MasteringStereoResult,
  MatchEqCurveResult,
  MatchEstimateReferenceDelaySamplesResult,
  MatchReferenceLoudnessResult,
  MatchTonalBalanceLogBandsResult,
  MatchTonalBalanceResult,
  PairAnalysis,
  PairAnalysisResultMap,
  PairProcessor,
  ProgressCallback,
  SoloProcessor,
  StereoAnalysis,
  StereoAnalysisResultMap,
  StereoMonoCompatCheckLogBandsResult,
  StereoMonoCompatCheckResult,
  StereoPairProcessor,
  StreamingLoudnessGainResult,
  TypedJson,
} from './types.js';
import { assertAudioInput } from './validation.js';

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
  params?: MasteringSoloProcessorParams;
}

export interface MasteringProcessStereoRequest {
  processorName: SoloProcessor;
  left: Float32Array;
  right: Float32Array;
  sampleRate?: number;
  params?: MasteringSoloProcessorParams;
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

export interface MasteringPairProcessStereoRequest {
  processorName: StereoPairProcessor;
  sourceLeft: Float32Array;
  sourceRight: Float32Array;
  referenceLeft: Float32Array;
  referenceRight: Float32Array;
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

/** Canonical request form for stereo AB loudness matching. */
export interface MasteringAbMatchLoudnessStereoRequest {
  sourceLeft: Float32Array;
  sourceRight: Float32Array;
  referenceLeft: Float32Array;
  referenceRight: Float32Array;
  sampleRate?: number;
}

export interface MasteringPairAnalyzeRequest<N extends PairAnalysis = PairAnalysis> {
  analysisName: N;
  source: Float32Array;
  reference: Float32Array;
  sampleRate?: number;
  params?: Record<string, number | boolean>;
}

export interface MasteringStereoAnalyzeRequest<N extends StereoAnalysis = StereoAnalysis> {
  analysisName: N;
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
  assertAudioInput('mastering', request.samples, resolvedSampleRate, request);
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
  params?: MasteringSoloProcessorParams,
): MasteringResult;
export function masteringProcess(
  processorName: SoloProcessor | MasteringProcessRequest,
  samples?: Float32Array,
  sampleRate = 22050,
  params: MasteringSoloProcessorParams = {},
): MasteringResult {
  const request =
    typeof processorName === 'string'
      ? { processorName, samples: samples as Float32Array, sampleRate, params }
      : processorName;
  const resolvedSampleRate = request.sampleRate ?? 22050;
  assertAudioInput('masteringProcess', request.samples, resolvedSampleRate, request);
  return addon.masteringProcess(
    request.processorName,
    request.samples,
    resolvedSampleRate,
    resolveProcessorParams(request.processorName, request.params ?? {}),
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
  params?: MasteringSoloProcessorParams,
): MasteringStereoResult;
export function masteringProcessStereo(
  processorName: SoloProcessor | MasteringProcessStereoRequest,
  left?: Float32Array,
  right?: Float32Array,
  sampleRate = 22050,
  params: MasteringSoloProcessorParams = {},
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
  assertAudioInput('masteringProcessStereo', request.left, resolvedSampleRate, request, 'left');
  assertAudioInput('masteringProcessStereo', request.right, resolvedSampleRate, request, 'right');
  return addon.masteringProcessStereo(
    request.processorName,
    request.left,
    request.right,
    resolvedSampleRate,
    resolveProcessorParams(request.processorName, request.params ?? {}),
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
  assertAudioInput('masteringChain', request.samples, resolvedSampleRate, request);
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
  assertAudioInput('masteringChainStereo', request.left, resolvedSampleRate, request, 'left');
  assertAudioInput('masteringChainStereo', request.right, resolvedSampleRate, request, 'right');
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

/** Request for {@link streamingLoudnessGain}. */
export interface StreamingLoudnessGainRequest {
  samples: Float32Array;
  sampleRate?: number;
  /** The chain config the streaming chain will run; read like {@link masteringChain}'s. */
  config?: MasteringChainConfig;
}

/** Request for {@link streamingLoudnessGainStereo}. */
export interface StreamingLoudnessGainStereoRequest {
  left: Float32Array;
  right: Float32Array;
  sampleRate?: number;
  config?: MasteringChainConfig;
}

/**
 * Measures the loudness numbers a {@link StreamingMasteringChain} needs.
 *
 * Runs the offline chain described by `config` up to its loudness stage and
 * measures there, so `loudnessStaticGainDb` equals the gain
 * {@link masteringChain} applies (ceiling clamp included) and `truePeakDb` is
 * the peak that clamp used. The gain is computed from `config.loudness` whether
 * or not the stage is enabled; a silent or below-gate stage input yields 0 dB.
 *
 * @example
 * const { loudnessStaticGainDb, truePeakDb } = streamingLoudnessGain({ samples, sampleRate, config });
 * const chain = new StreamingMasteringChain({
 *   ...config,
 *   loudnessStaticGainDb,
 *   loudnessStaticGainPeakDb: truePeakDb,
 * });
 */
export function streamingLoudnessGain(
  request: StreamingLoudnessGainRequest,
): StreamingLoudnessGainResult {
  const resolvedSampleRate = request.sampleRate ?? 22050;
  assertAudioInput('streamingLoudnessGain', request.samples, resolvedSampleRate, request);
  return addon.masteringStreamingLoudnessGain(
    request.samples,
    resolvedSampleRate,
    flattenChainConfig(request.config ?? {}),
  );
}

/** Stereo counterpart of {@link streamingLoudnessGain}, with BS.1770 channel summing. */
export function streamingLoudnessGainStereo(
  request: StreamingLoudnessGainStereoRequest,
): StreamingLoudnessGainResult {
  const resolvedSampleRate = request.sampleRate ?? 22050;
  assertAudioInput(
    'streamingLoudnessGainStereo',
    request.left,
    resolvedSampleRate,
    request,
    'left',
  );
  assertAudioInput(
    'streamingLoudnessGainStereo',
    request.right,
    resolvedSampleRate,
    request,
    'right',
  );
  return addon.masteringStreamingLoudnessGainStereo(
    request.left,
    request.right,
    resolvedSampleRate,
    flattenChainConfig(request.config ?? {}),
  );
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
  assertAudioInput('masterAudio', request.samples, resolvedSampleRate, request);
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
    assertAudioInput('masterAudioAsync', request.samples, resolvedSampleRate, request);
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
  assertAudioInput('masterAudioStereo', request.left, resolvedSampleRate, request, 'left');
  assertAudioInput('masterAudioStereo', request.right, resolvedSampleRate, request, 'right');
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
    assertAudioInput('masterAudioStereoAsync', request.left, resolvedSampleRate, request, 'left');
    assertAudioInput('masterAudioStereoAsync', request.right, resolvedSampleRate, request, 'right');
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
  assertAudioInput('masteringPairProcess', request.source, resolvedSampleRate, request, 'source');
  assertAudioInput(
    'masteringPairProcess',
    request.reference,
    resolvedSampleRate,
    request,
    'reference',
  );
  return addon.masteringPairProcess(
    request.processorName,
    request.source,
    request.reference,
    resolvedSampleRate,
    request.params ?? {},
  );
}

/**
 * Apply the stereo `match.abCrossfade` processor. Source and reference stereo
 * pairs may have independent lengths, but each pair must have equal channels.
 */
export function masteringPairProcessStereo(
  request: MasteringPairProcessStereoRequest,
): MasteringStereoResult;
export function masteringPairProcessStereo(
  processorName: StereoPairProcessor,
  sourceLeft: Float32Array,
  sourceRight: Float32Array,
  referenceLeft: Float32Array,
  referenceRight: Float32Array,
  sampleRate?: number,
  params?: Record<string, number | boolean>,
): MasteringStereoResult;
export function masteringPairProcessStereo(
  processorName: StereoPairProcessor | MasteringPairProcessStereoRequest,
  sourceLeft?: Float32Array,
  sourceRight?: Float32Array,
  referenceLeft?: Float32Array,
  referenceRight?: Float32Array,
  sampleRate = 22050,
  params: Record<string, number | boolean> = {},
): MasteringStereoResult {
  const request =
    typeof processorName === 'string'
      ? {
          processorName,
          sourceLeft: sourceLeft as Float32Array,
          sourceRight: sourceRight as Float32Array,
          referenceLeft: referenceLeft as Float32Array,
          referenceRight: referenceRight as Float32Array,
          sampleRate,
          params,
        }
      : processorName;
  const resolvedSampleRate = request.sampleRate ?? 22050;
  assertAudioInput(
    'masteringPairProcessStereo',
    request.sourceLeft,
    resolvedSampleRate,
    request,
    'sourceLeft',
  );
  assertAudioInput(
    'masteringPairProcessStereo',
    request.sourceRight,
    resolvedSampleRate,
    request,
    'sourceRight',
  );
  assertAudioInput(
    'masteringPairProcessStereo',
    request.referenceLeft,
    resolvedSampleRate,
    request,
    'referenceLeft',
  );
  assertAudioInput(
    'masteringPairProcessStereo',
    request.referenceRight,
    resolvedSampleRate,
    request,
    'referenceRight',
  );
  return addon.masteringPairProcessStereo(
    request.processorName,
    request.sourceLeft,
    request.sourceRight,
    request.referenceLeft,
    request.referenceRight,
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
  assertAudioInput(
    'masteringAbMatchLoudness',
    request.source,
    resolvedSampleRate,
    request,
    'source',
  );
  assertAudioInput(
    'masteringAbMatchLoudness',
    request.reference,
    resolvedSampleRate,
    request,
    'reference',
  );
  return addon.masteringAbMatchLoudness(request.source, request.reference, resolvedSampleRate);
}

/**
 * Gain-match a stereo source to a stereo reference with one shared gain.
 * Source and reference pairs may have independent lengths, but each pair must
 * have equal channels.
 */
export function masteringAbMatchLoudnessStereo(
  request: MasteringAbMatchLoudnessStereoRequest,
): LoudnessMatchStereoResult {
  const resolvedSampleRate = request.sampleRate ?? 22050;
  assertAudioInput(
    'masteringAbMatchLoudnessStereo',
    request.sourceLeft,
    resolvedSampleRate,
    request,
    'sourceLeft',
  );
  assertAudioInput(
    'masteringAbMatchLoudnessStereo',
    request.sourceRight,
    resolvedSampleRate,
    request,
    'sourceRight',
  );
  assertAudioInput(
    'masteringAbMatchLoudnessStereo',
    request.referenceLeft,
    resolvedSampleRate,
    request,
    'referenceLeft',
  );
  assertAudioInput(
    'masteringAbMatchLoudnessStereo',
    request.referenceRight,
    resolvedSampleRate,
    request,
    'referenceRight',
  );
  return addon.masteringAbMatchLoudnessStereo(
    request.sourceLeft,
    request.sourceRight,
    request.referenceLeft,
    request.referenceRight,
    resolvedSampleRate,
  );
}

/**
 * Analyze a `source` against a `reference` with a two-input analysis. The two
 * buffers may have independent lengths.
 *
 * Returns JSON whose shape depends on the analysis name; the overload for
 * each analysis name carries its entry of {@link PairAnalysisResultMap}, which
 * `JSON.parse(json) as JsonResult<typeof json>` reads back.
 */
export function masteringPairAnalyze(
  request: MasteringPairAnalyzeRequest<'match.referenceLoudness'>,
): TypedJson<MatchReferenceLoudnessResult>;
export function masteringPairAnalyze(
  request: MasteringPairAnalyzeRequest<'match.tonalBalance'>,
): TypedJson<MatchTonalBalanceResult>;
export function masteringPairAnalyze(
  request: MasteringPairAnalyzeRequest<'match.tonalBalanceLogBands'>,
): TypedJson<MatchTonalBalanceLogBandsResult>;
export function masteringPairAnalyze(
  request: MasteringPairAnalyzeRequest<'match.matchEqCurve'>,
): TypedJson<MatchEqCurveResult>;
export function masteringPairAnalyze(
  request: MasteringPairAnalyzeRequest<'match.estimateReferenceDelaySamples'>,
): TypedJson<MatchEstimateReferenceDelaySamplesResult>;
export function masteringPairAnalyze(
  request: MasteringPairAnalyzeRequest,
): TypedJson<PairAnalysisResultMap[PairAnalysis]>;
export function masteringPairAnalyze(
  analysisName: 'match.referenceLoudness',
  source: Float32Array,
  reference: Float32Array,
  sampleRate?: number,
  params?: Record<string, number | boolean>,
): TypedJson<MatchReferenceLoudnessResult>;
export function masteringPairAnalyze(
  analysisName: 'match.tonalBalance',
  source: Float32Array,
  reference: Float32Array,
  sampleRate?: number,
  params?: Record<string, number | boolean>,
): TypedJson<MatchTonalBalanceResult>;
export function masteringPairAnalyze(
  analysisName: 'match.tonalBalanceLogBands',
  source: Float32Array,
  reference: Float32Array,
  sampleRate?: number,
  params?: Record<string, number | boolean>,
): TypedJson<MatchTonalBalanceLogBandsResult>;
export function masteringPairAnalyze(
  analysisName: 'match.matchEqCurve',
  source: Float32Array,
  reference: Float32Array,
  sampleRate?: number,
  params?: Record<string, number | boolean>,
): TypedJson<MatchEqCurveResult>;
export function masteringPairAnalyze(
  analysisName: 'match.estimateReferenceDelaySamples',
  source: Float32Array,
  reference: Float32Array,
  sampleRate?: number,
  params?: Record<string, number | boolean>,
): TypedJson<MatchEstimateReferenceDelaySamplesResult>;
export function masteringPairAnalyze(
  analysisName: PairAnalysis,
  source: Float32Array,
  reference: Float32Array,
  sampleRate?: number,
  params?: Record<string, number | boolean>,
): TypedJson<PairAnalysisResultMap[PairAnalysis]>;
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
  assertAudioInput('masteringPairAnalyze', request.source, resolvedSampleRate, request, 'source');
  assertAudioInput(
    'masteringPairAnalyze',
    request.reference,
    resolvedSampleRate,
    request,
    'reference',
  );
  return addon.masteringPairAnalyze(
    request.analysisName,
    request.source,
    request.reference,
    resolvedSampleRate,
    request.params ?? {},
  );
}

/**
 * Analyze a stereo pair. Returns JSON whose shape depends on the analysis name;
 * the overload for
 * each analysis name carries its entry of {@link StereoAnalysisResultMap}.
 */
export function masteringStereoAnalyze(
  request: MasteringStereoAnalyzeRequest<'stereo.monoCompatCheck'>,
): TypedJson<StereoMonoCompatCheckResult>;
export function masteringStereoAnalyze(
  request: MasteringStereoAnalyzeRequest<'stereo.monoCompatCheckLogBands'>,
): TypedJson<StereoMonoCompatCheckLogBandsResult>;
export function masteringStereoAnalyze(
  request: MasteringStereoAnalyzeRequest,
): TypedJson<StereoAnalysisResultMap[StereoAnalysis]>;
export function masteringStereoAnalyze(
  analysisName: 'stereo.monoCompatCheck',
  left: Float32Array,
  right: Float32Array,
  sampleRate?: number,
  params?: Record<string, number | boolean>,
): TypedJson<StereoMonoCompatCheckResult>;
export function masteringStereoAnalyze(
  analysisName: 'stereo.monoCompatCheckLogBands',
  left: Float32Array,
  right: Float32Array,
  sampleRate?: number,
  params?: Record<string, number | boolean>,
): TypedJson<StereoMonoCompatCheckLogBandsResult>;
export function masteringStereoAnalyze(
  analysisName: StereoAnalysis,
  left: Float32Array,
  right: Float32Array,
  sampleRate?: number,
  params?: Record<string, number | boolean>,
): TypedJson<StereoAnalysisResultMap[StereoAnalysis]>;
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
  assertAudioInput('masteringStereoAnalyze', request.left, resolvedSampleRate, request, 'left');
  assertAudioInput('masteringStereoAnalyze', request.right, resolvedSampleRate, request, 'right');
  return addon.masteringStereoAnalyze(
    request.analysisName,
    request.left,
    request.right,
    resolvedSampleRate,
    request.params ?? {},
  );
}
