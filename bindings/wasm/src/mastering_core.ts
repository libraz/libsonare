import { getSonareModule } from './module_state';
import type {
  MasteringOptions,
  MasteringProcessorParams,
  MasteringResult,
  MasteringStereoResult,
  PairAnalysis,
  PairProcessor,
  SoloProcessor,
  StereoAnalysis,
  StreamingPlatform,
} from './public_types';

function requireModule() {
  return getSonareModule();
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
  params?: MasteringProcessorParams;
}

export interface MasteringProcessStereoRequest {
  processorName: SoloProcessor;
  left: Float32Array;
  right: Float32Array;
  sampleRate?: number;
  params?: MasteringProcessorParams;
}

/** Canonical request form for a two-input match processor. */
export interface MasteringPairProcessRequest {
  processorName: PairProcessor;
  source: Float32Array;
  reference: Float32Array;
  sampleRate?: number;
  params?: MasteringProcessorParams;
}

/** Canonical request form for a two-input match analysis. */
export interface MasteringPairAnalyzeRequest {
  analysisName: PairAnalysis;
  source: Float32Array;
  reference: Float32Array;
  sampleRate?: number;
  params?: MasteringProcessorParams;
}

/** Canonical request form for a stereo analysis. */
export interface MasteringStereoAnalyzeRequest {
  analysisName: StereoAnalysis;
  left: Float32Array;
  right: Float32Array;
  sampleRate?: number;
  params?: MasteringProcessorParams;
}

/** Canonical request form for assistant/profile calls. */
export interface MasteringSamplesParamsRequest {
  samples: Float32Array;
  sampleRate?: number;
  params?: MasteringProcessorParams;
}

/** Canonical request form for streaming-platform preview. */
export interface MasteringStreamingPreviewRequest {
  samples: Float32Array;
  sampleRate?: number;
  platforms?: StreamingPlatform[];
}

/**
 * Apply mastering loudness normalization with a true-peak ceiling.
 *
 * @param samples - Audio samples (mono, float32)
 * @param sampleRate - Sample rate in Hz (default: 22050)
 * @param options - Loudness/ceiling settings ({@link MasteringOptions})
 * @returns Processed audio and loudness metadata
 */
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
  return requireModule().mastering(
    request.samples,
    request.sampleRate ?? 22050,
    request.targetLufs ?? -14.0,
    request.ceilingDb ?? -1.0,
    request.truePeakOversample ?? 4,
    request.releaseMs ?? 0, // 0 => library default (50 ms)
    request.applyGainAtInputRate ?? false,
  );
}

export function masteringProcessorNames(): SoloProcessor[] {
  // embind hands back a vector whose constructor is not this realm's Array, so the
  // result is not structured-cloneable (breaks postMessage to a Worker).
  // Array.from() re-roots it as a plain Array. Same for the sibling *Names() below.
  return Array.from(requireModule().masteringProcessorNames()) as SoloProcessor[];
}

/**
 * Names of the insert processors the mastering chain can instantiate by name
 * (`mastering::api::insert_factory_names`). Mirrors the C-ABI
 * `sonare_mastering_insert_names` (which joins this list) as a `string[]`.
 */
export function masteringInsertNames(): string[] {
  return (
    requireModule() as unknown as { masteringInsertNames: () => string[] }
  ).masteringInsertNames();
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
  return Array.from(
    (
      requireModule() as unknown as { masteringInsertParamNames: (name: string) => string[] }
    ).masteringInsertParamNames(name),
  );
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
  const json = (
    requireModule() as unknown as { masteringInsertParamInfo: (name: string) => string }
  ).masteringInsertParamInfo(name);
  return JSON.parse(json) as MasteringInsertParamInfo[];
}

/**
 * How a processor handles a buffer with more than two channels (a surround
 * bed). "multichannel" processes every plane in one call; "stereoPairOnly"
 * operates on the front L/R pair and passes any surround planes through dry.
 * "perChannel"/"passthrough" are reserved and unused by the current catalog.
 */
export type MasteringChannelPolicy =
  | 'multichannel'
  | 'stereoPairOnly'
  | 'perChannel'
  | 'passthrough';

/** One processor's realtime/offline/pair classification in the catalog. */
export interface MasteringProcessorCatalogEntry {
  /** Processor id (the name used for scene inserts / named processors). */
  id: string;
  /**
   * Primary classification, by precedence pair > realtime > offline: "pair" for
   * two-input match.* processors, "realtime" for ids that build as a realtime
   * scene insert, "offline" for whole-file-only processors.
   */
  kind: 'realtime' | 'offline' | 'pair';
  /** True exactly for ids that always succeed as a realtime scene insert. */
  realtimeInsertable: boolean;
  /** True for processors with no mono implementation (stereo-only). */
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
  /**
   * How the mixer wraps the processor on a >2-channel (surround) bus insert:
   * "multichannel" (one full-buffer call) or "stereoPairOnly" (front L/R pair,
   * surround planes passed through dry).
   */
  channelPolicy: MasteringChannelPolicy;
}

/**
 * Returns the machine-readable classification catalog for every named processor
 * id, merging the offline registry, the realtime insert factory, and the pair
 * registry. Lets a host filter a processor picker by realtime insertability
 * instead of offering ids the realtime strip would reject.
 */
export function masteringProcessorCatalog(): MasteringProcessorCatalogEntry[] {
  const json = (
    requireModule() as unknown as { masteringProcessorCatalog: () => string }
  ).masteringProcessorCatalog();
  return JSON.parse(json) as MasteringProcessorCatalogEntry[];
}

export function masteringPairProcessorNames(): PairProcessor[] {
  return Array.from(requireModule().masteringPairProcessorNames()) as PairProcessor[];
}

export function masteringPairAnalysisNames(): PairAnalysis[] {
  return Array.from(requireModule().masteringPairAnalysisNames()) as PairAnalysis[];
}

export function masteringStereoAnalysisNames(): StereoAnalysis[] {
  return Array.from(requireModule().masteringStereoAnalysisNames()) as StereoAnalysis[];
}

export function masteringProcess(request: MasteringProcessRequest): MasteringResult;
export function masteringProcess(
  processorName: SoloProcessor,
  samples: Float32Array,
  sampleRate?: number,
  params?: MasteringProcessorParams,
): MasteringResult;
export function masteringProcess(
  processorName: SoloProcessor | MasteringProcessRequest,
  samples?: Float32Array,
  sampleRate = 22050,
  params: MasteringProcessorParams = {},
): MasteringResult {
  const request =
    typeof processorName === 'string'
      ? { processorName, samples: samples as Float32Array, sampleRate, params }
      : processorName;
  return requireModule().masteringProcess(
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
  params?: MasteringProcessorParams,
): MasteringStereoResult;
export function masteringProcessStereo(
  processorName: SoloProcessor | MasteringProcessStereoRequest,
  left?: Float32Array,
  right?: Float32Array,
  sampleRate = 22050,
  params: MasteringProcessorParams = {},
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
  if (request.left.length !== request.right.length) {
    throw new Error('Stereo channel lengths must match.');
  }
  return requireModule().masteringProcessStereo(
    request.processorName,
    request.left,
    request.right,
    request.sampleRate ?? 22050,
    request.params ?? {},
  );
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
  params?: MasteringProcessorParams,
): MasteringResult;
export function masteringPairProcess(
  processorName: PairProcessor | MasteringPairProcessRequest,
  source?: Float32Array,
  reference?: Float32Array,
  sampleRate = 22050,
  params: MasteringProcessorParams = {},
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
  return requireModule().masteringPairProcess(
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
  params?: MasteringProcessorParams,
): string;
export function masteringPairAnalyze(
  analysisName: PairAnalysis | MasteringPairAnalyzeRequest,
  source?: Float32Array,
  reference?: Float32Array,
  sampleRate = 22050,
  params: MasteringProcessorParams = {},
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
  return requireModule().masteringPairAnalyze(
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
  params?: MasteringProcessorParams,
): string;
export function masteringStereoAnalyze(
  analysisName: StereoAnalysis | MasteringStereoAnalyzeRequest,
  left?: Float32Array,
  right?: Float32Array,
  sampleRate = 22050,
  params: MasteringProcessorParams = {},
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
  return requireModule().masteringStereoAnalyze(
    request.analysisName,
    request.left,
    request.right,
    request.sampleRate ?? 22050,
    request.params ?? {},
  );
}

export function masteringAssistantSuggest(request: MasteringSamplesParamsRequest): string;
export function masteringAssistantSuggest(
  samples: Float32Array,
  sampleRate?: number,
  params?: MasteringProcessorParams,
): string;
export function masteringAssistantSuggest(
  samples: Float32Array | MasteringSamplesParamsRequest,
  sampleRate = 22050,
  params: MasteringProcessorParams = {},
): string {
  const request = samples instanceof Float32Array ? { samples, sampleRate, params } : samples;
  return requireModule().masteringAssistantSuggest(
    request.samples,
    request.sampleRate ?? 22050,
    request.params ?? {},
  );
}

export function masteringAudioProfile(request: MasteringSamplesParamsRequest): string;
export function masteringAudioProfile(
  samples: Float32Array,
  sampleRate?: number,
  params?: MasteringProcessorParams,
): string;
export function masteringAudioProfile(
  samples: Float32Array | MasteringSamplesParamsRequest,
  sampleRate = 22050,
  params: MasteringProcessorParams = {},
): string {
  const request = samples instanceof Float32Array ? { samples, sampleRate, params } : samples;
  return requireModule().masteringAudioProfile(
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
  return requireModule().masteringStreamingPreview(
    request.samples,
    request.sampleRate ?? 22050,
    request.platforms ?? [],
  );
}
