import { flattenChainConfig } from './_chain_config.js';
import { addon } from './native.js';
import type {
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
  StreamingPlatform,
} from './types.js';

export interface NormalizeRequest {
  samples: Float32Array;
  sampleRate?: number;
  /** Finite peak target at or below 0 dBFS. Default 0. */
  targetDb?: number;
}

export function normalize(request: NormalizeRequest): Float32Array;
export function normalize(
  samples: Float32Array,
  sampleRate?: number,
  targetDb?: number,
): Float32Array;
export function normalize(
  samples: Float32Array | NormalizeRequest,
  sampleRate = 22050,
  targetDb = 0.0,
): Float32Array {
  const request = samples instanceof Float32Array ? { samples, sampleRate, targetDb } : samples;
  return addon.normalize(request.samples, request.sampleRate ?? 22050, request.targetDb ?? 0.0);
}

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
}

export interface MasteringChainStereoRequest {
  left: Float32Array;
  right: Float32Array;
  sampleRate?: number;
  config?: MasteringChainConfig;
  onProgress?: ProgressCallback;
}

export interface MasteringPairProcessRequest {
  processorName: PairProcessor;
  source: Float32Array;
  reference: Float32Array;
  sampleRate?: number;
  params?: Record<string, number | boolean>;
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

export interface MasteringAssistantSuggestRequest {
  samples: Float32Array;
  sampleRate?: number;
  params?: Record<string, number | boolean>;
}

export interface MasteringAudioProfileRequest extends MasteringAssistantSuggestRequest {}

export interface MasteringStreamingPreviewRequest {
  samples: Float32Array;
  sampleRate?: number;
  platforms?: StreamingPlatform[];
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
  return addon.mastering(
    request.samples,
    request.sampleRate ?? 22050,
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
  return addon.masteringProcess(
    request.processorName,
    request.samples,
    request.sampleRate ?? 22050,
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
  return addon.masteringProcessStereo(
    request.processorName,
    request.left,
    request.right,
    request.sampleRate ?? 22050,
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
  if (request.onProgress) {
    return addon.masteringChainWithProgress(
      request.samples,
      request.sampleRate ?? 22050,
      flat,
      request.onProgress,
    );
  }
  return addon.masteringChain(request.samples, request.sampleRate ?? 22050, flat);
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
  if (request.onProgress) {
    return addon.masteringChainStereoWithProgress(
      request.left,
      request.right,
      request.sampleRate ?? 22050,
      flat,
      request.onProgress,
    );
  }
  return addon.masteringChainStereo(request.left, request.right, request.sampleRate ?? 22050, flat);
}

export function masteringPresetNames(): MasteringPreset[] {
  return addon.masteringPresetNames();
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
  if (request.onProgress) {
    return addon.masterAudioWithProgress(
      request.preset ?? 'pop',
      request.samples,
      request.sampleRate ?? 22050,
      flat,
      request.onProgress,
    );
  }
  return addon.masterAudio(
    request.preset ?? 'pop',
    request.samples,
    request.sampleRate ?? 22050,
    flat,
  );
}

/**
 * Asynchronous variant of {@link masterAudio}. Runs the full chain on a libuv
 * worker thread; the returned promise resolves with the same shape as the
 * synchronous version. Progress reporting is not available on the async path
 * (use the synchronous `masterAudio` with `onProgress` if you need it, or
 * spin up multiple async calls in parallel).
 */
export function masterAudioAsync(
  request: Omit<MasterAudioRequest, 'onProgress'>,
): Promise<MasteringChainResult>;
export function masterAudioAsync(
  samples: Float32Array,
  sampleRate?: number,
  presetName?: MasteringPreset,
  overrides?: MasteringChainConfig,
): Promise<MasteringChainResult>;
export function masterAudioAsync(
  samples: Omit<MasterAudioRequest, 'onProgress'> | Float32Array,
  sampleRate = 22050,
  presetName: MasteringPreset = 'pop',
  overrides: MasteringChainConfig = {},
): Promise<MasteringChainResult> {
  const request = masterAudioRequest(samples, sampleRate, presetName, overrides);
  return addon.masterAudioAsync(
    request.preset ?? 'pop',
    request.samples,
    request.sampleRate ?? 22050,
    flattenChainConfig(request.overrides ?? {}),
  );
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
  if (request.onProgress) {
    return addon.masterAudioStereoWithProgress(
      request.preset ?? 'pop',
      request.left,
      request.right,
      request.sampleRate ?? 22050,
      flat,
      request.onProgress,
    );
  }
  return addon.masterAudioStereo(
    request.preset ?? 'pop',
    request.left,
    request.right,
    request.sampleRate ?? 22050,
    flat,
  );
}

/**
 * Asynchronous variant of {@link masterAudioStereo}.
 */
export function masterAudioStereoAsync(
  request: Omit<MasterAudioStereoRequest, 'onProgress'>,
): Promise<MasteringChainStereoResult>;
export function masterAudioStereoAsync(
  left: Float32Array,
  right: Float32Array,
  sampleRate?: number,
  presetName?: MasteringPreset,
  overrides?: MasteringChainConfig,
): Promise<MasteringChainStereoResult>;
export function masterAudioStereoAsync(
  left: Omit<MasterAudioStereoRequest, 'onProgress'> | Float32Array,
  right: Float32Array | undefined = undefined,
  sampleRate = 22050,
  presetName: MasteringPreset = 'pop',
  overrides: MasteringChainConfig = {},
): Promise<MasteringChainStereoResult> {
  const request = masterAudioStereoRequest(left, right, sampleRate, presetName, overrides);
  return addon.masterAudioStereoAsync(
    request.preset ?? 'pop',
    request.left,
    request.right,
    request.sampleRate ?? 22050,
    flattenChainConfig(request.overrides ?? {}),
  );
}

export function masteringProcessorNames(): SoloProcessor[] {
  return addon.masteringProcessorNames();
}

export function masteringPairProcessorNames(): PairProcessor[] {
  return addon.masteringPairProcessorNames();
}

export function masteringPairAnalysisNames(): PairAnalysis[] {
  return addon.masteringPairAnalysisNames();
}

export function masteringStereoAnalysisNames(): StereoAnalysis[] {
  return addon.masteringStereoAnalysisNames();
}

/**
 * Returns the channel-strip insert / FX processor names that mixing scene
 * inserts can build (includes the creative effects.* reverbs / modulation /
 * delay when FX support is compiled in). Use these to discover valid insert
 * names instead of hardcoding magic strings.
 */
export function masteringInsertNames(): string[] {
  return addon.masteringInsertNames();
}

/**
 * Returns the camelCase parameter names a given insert / FX processor reads, for
 * tooling/validation. Any key NOT in this list is silently ignored by the
 * processor (and would be reported via {@link Mixer.sceneWarnings} when a scene
 * carrying it is loaded). Band/sub-band processors enumerate their indexed
 * `band{i}.<field>` keys. Returns an empty array for an unknown name (or one
 * whose insert needs an unavailable build feature, e.g. FX).
 *
 * @param name - Insert processor name (see {@link masteringInsertNames}).
 */
export function masteringInsertParamNames(name: string): string[] {
  return addon.masteringInsertParamNames(name);
}

/** One realtime-automatable parameter of an insert processor. */
export interface MasteringInsertParamInfo {
  /** JSON-key parameter name, as used in scene insert params. */
  name: string;
  /** Integer param id for realtime automation lanes / MIDI-CC binding. */
  id: number;
  /** Whether the param can be changed live from the audio thread. */
  rtSafe: boolean;
  /** Physical unit when the parameter is not unitless. */
  unit?: string;
}

/**
 * Returns the realtime-automatable parameter descriptors for an insert / FX
 * processor: each entry maps a JSON-key parameter name to the integer id used by
 * realtime automation and reports whether it is realtime-safe. Unlike
 * {@link masteringInsertParamNames} (every construction key), this lists only the
 * realtime-controllable subset — the keys accepted by
 * {@link RealtimeEngine.setTrackStripInsertParamByName}. Returns an empty array
 * for an unknown name or a processor with no automatable parameters.
 *
 * @param name - Insert processor name (see {@link masteringInsertNames}).
 */
export function masteringInsertParamInfo(name: string): MasteringInsertParamInfo[] {
  const json = addon.masteringInsertParamInfo(name);
  return JSON.parse(json) as MasteringInsertParamInfo[];
}

/**
 * How a processor handles a buffer with more than two channels (a surround
 * bed). `multichannel` processes every plane in one call; `stereoPairOnly`
 * operates on the front L/R pair and passes any surround planes through dry.
 * `perChannel`/`passthrough` are reserved and unused by the current catalog.
 */
export type MasteringChannelPolicy =
  | 'multichannel'
  | 'stereoPairOnly'
  | 'perChannel'
  | 'passthrough';

/** Coarse algorithmic work estimate for a realtime insert; not a benchmark. */
export type MasteringRealtimeCost = 'low' | 'moderate' | 'high';

/** One mastering processor's role in the catalog. */
export interface MasteringProcessorCatalogEntry {
  /** Stable processor id (e.g. `dynamics.compressor`, `match.abCrossfade`). */
  id: string;
  /**
   * Coarse role: `pair` (two-input source/reference), `realtime` (usable as a
   * live insert), or `offline` (whole-buffer only). Precedence is
   * `pair > realtime > offline`: a processor that fits more than one role is
   * reported under the highest one.
   */
  kind: 'realtime' | 'offline' | 'pair';
  /**
   * Whether the processor belongs to the realtime-insert set (the processors
   * accepted as live track-strip inserts). Mirrors {@link masteringInsertNames}.
   */
  realtimeInsertable: boolean;
  /** Whether the processor requires a stereo signal (e.g. mid/side EQ). */
  stereoOnly: boolean;
  /**
   * Reported latency for the default 48 kHz / 512-sample probe configuration.
   * Zero for offline processors; configuration-dependent values are estimates.
   */
  latencySamples: number;
  /**
   * Audible decay length for the same default prepared probe. Zero for
   * offline, dry-only, and no-tail processors.
   */
  tailSamples: number;
  /** Coarse realtime work estimate, or null when the processor is not an insert. */
  realtimeCost: MasteringRealtimeCost | null;
  /**
   * How the mixer wraps the processor on a >2-channel (surround) bus insert:
   * `multichannel` (one full-buffer call) or `stereoPairOnly` (front L/R pair,
   * surround planes passed through dry).
   */
  channelPolicy: MasteringChannelPolicy;
}

/**
 * Returns the full mastering processor catalog: every processor id paired with
 * its coarse role and capability flags. `kind` follows the precedence
 * `pair > realtime > offline`, so a processor usable in more than one role is
 * reported under the highest. `realtimeInsertable` matches the realtime-insert
 * set ({@link masteringInsertNames}). Hosts use it to filter a processor picker
 * — e.g. to show only realtime-insertable entries for a live track strip, or to
 * gate stereo-only entries on mono material.
 */
export function masteringProcessorCatalog(): MasteringProcessorCatalogEntry[] {
  const json = addon.masteringProcessorCatalog();
  return JSON.parse(json) as MasteringProcessorCatalogEntry[];
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
  return addon.masteringPairProcess(
    request.processorName,
    request.source,
    request.reference,
    request.sampleRate ?? 22050,
    request.params ?? {},
  );
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
  return addon.masteringPairAnalyze(
    request.analysisName,
    request.source,
    request.reference,
    request.sampleRate ?? 22050,
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
  return addon.masteringStereoAnalyze(
    request.analysisName,
    request.left,
    request.right,
    request.sampleRate ?? 22050,
    request.params ?? {},
  );
}

export function masteringAssistantSuggest(request: MasteringAssistantSuggestRequest): string;
export function masteringAssistantSuggest(
  samples: Float32Array,
  sampleRate?: number,
  params?: Record<string, number | boolean>,
): string;
export function masteringAssistantSuggest(
  samples: Float32Array | MasteringAssistantSuggestRequest,
  sampleRate = 22050,
  params: Record<string, number | boolean> = {},
): string {
  const request = samples instanceof Float32Array ? { samples, sampleRate, params } : samples;
  return addon.masteringAssistantSuggest(
    request.samples,
    request.sampleRate ?? 22050,
    request.params ?? {},
  );
}

export function masteringAudioProfile(request: MasteringAudioProfileRequest): string;
export function masteringAudioProfile(
  samples: Float32Array,
  sampleRate?: number,
  params?: Record<string, number | boolean>,
): string;
export function masteringAudioProfile(
  samples: Float32Array | MasteringAudioProfileRequest,
  sampleRate = 22050,
  params: Record<string, number | boolean> = {},
): string {
  const request = samples instanceof Float32Array ? { samples, sampleRate, params } : samples;
  return addon.masteringAudioProfile(
    request.samples,
    request.sampleRate ?? 22050,
    request.params ?? {},
  );
}

export function masteringStreamingPreview(request: MasteringStreamingPreviewRequest): string;
export function masteringStreamingPreview(
  samples: Float32Array,
  sampleRate?: number,
  platforms?: StreamingPlatform[],
): string;
export function masteringStreamingPreview(
  samples: Float32Array | MasteringStreamingPreviewRequest,
  sampleRate = 22050,
  platforms: StreamingPlatform[] = [],
): string {
  const request = samples instanceof Float32Array ? { samples, sampleRate, platforms } : samples;
  return addon.masteringStreamingPreview(
    request.samples,
    request.sampleRate ?? 22050,
    request.platforms ?? [],
  );
}
