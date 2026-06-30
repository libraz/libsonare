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

/**
 * Apply mastering loudness normalization with a true-peak ceiling.
 *
 * @param samples - Audio samples (mono, float32)
 * @param sampleRate - Sample rate in Hz (default: 22050)
 * @param options - Loudness/ceiling settings ({@link MasteringOptions})
 * @returns Processed audio and loudness metadata
 */
export function mastering(
  samples: Float32Array,
  sampleRate = 22050,
  options: MasteringOptions = {},
): MasteringResult {
  return requireModule().mastering(
    samples,
    sampleRate,
    options.targetLufs ?? -14.0,
    options.ceilingDb ?? -1.0,
    options.truePeakOversample ?? 4,
    options.releaseMs ?? 0, // 0 => library default (50 ms)
    options.applyGainAtInputRate ?? false,
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

export function masteringProcess(
  processorName: SoloProcessor,
  samples: Float32Array,
  sampleRate = 22050,
  params: MasteringProcessorParams = {},
): MasteringResult {
  return requireModule().masteringProcess(processorName, samples, sampleRate, params);
}

export function masteringProcessStereo(
  processorName: SoloProcessor,
  left: Float32Array,
  right: Float32Array,
  sampleRate = 22050,
  params: MasteringProcessorParams = {},
): MasteringStereoResult {
  if (left.length !== right.length) {
    throw new Error('Stereo channel lengths must match.');
  }
  return requireModule().masteringProcessStereo(processorName, left, right, sampleRate, params);
}

/**
 * Apply a two-input `match.*` processor. `source` and `reference` may have
 * independent lengths — the match primitives consume each buffer at its own
 * length.
 */
export function masteringPairProcess(
  processorName: PairProcessor,
  source: Float32Array,
  reference: Float32Array,
  sampleRate = 22050,
  params: MasteringProcessorParams = {},
): MasteringResult {
  return requireModule().masteringPairProcess(processorName, source, reference, sampleRate, params);
}

/**
 * Analyze a `source` against a `reference` with a two-input analysis. The two
 * buffers may have independent lengths.
 */
export function masteringPairAnalyze(
  analysisName: PairAnalysis,
  source: Float32Array,
  reference: Float32Array,
  sampleRate = 22050,
  params: MasteringProcessorParams = {},
): string {
  return requireModule().masteringPairAnalyze(analysisName, source, reference, sampleRate, params);
}

export function masteringStereoAnalyze(
  analysisName: StereoAnalysis,
  left: Float32Array,
  right: Float32Array,
  sampleRate = 22050,
  params: MasteringProcessorParams = {},
): string {
  return requireModule().masteringStereoAnalyze(analysisName, left, right, sampleRate, params);
}

export function masteringAssistantSuggest(
  samples: Float32Array,
  sampleRate = 22050,
  params: MasteringProcessorParams = {},
): string {
  return requireModule().masteringAssistantSuggest(samples, sampleRate, params);
}

export function masteringAudioProfile(
  samples: Float32Array,
  sampleRate = 22050,
  params: MasteringProcessorParams = {},
): string {
  return requireModule().masteringAudioProfile(samples, sampleRate, params);
}

export function masteringStreamingPreview(
  samples: Float32Array,
  sampleRate = 22050,
  platforms: StreamingPlatform[] = [],
): string {
  return requireModule().masteringStreamingPreview(samples, sampleRate, platforms);
}
