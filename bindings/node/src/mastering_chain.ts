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
  SoloProcessor,
  StereoAnalysis,
  StreamingPlatform,
} from './types.js';

export function normalize(samples: Float32Array, sampleRate = 22050, targetDb = 0.0): Float32Array {
  return addon.normalize(samples, sampleRate, targetDb);
}

export function mastering(
  samples: Float32Array,
  sampleRate = 22050,
  options: MasteringOptions = {},
): MasteringResult {
  return addon.mastering(
    samples,
    sampleRate,
    options.targetLufs ?? -14.0,
    options.ceilingDb ?? -1.0,
    options.truePeakOversample ?? 4,
    options.releaseMs ?? 0, // 0 => library default (50 ms)
    options.applyGainAtInputRate ?? false,
  );
}

export function masteringProcess(
  processorName: SoloProcessor,
  samples: Float32Array,
  sampleRate = 22050,
  params: Record<string, number | boolean> = {},
): MasteringResult {
  return addon.masteringProcess(processorName, samples, sampleRate, params);
}

export function masteringProcessStereo(
  processorName: SoloProcessor,
  left: Float32Array,
  right: Float32Array,
  sampleRate = 22050,
  params: Record<string, number | boolean> = {},
): MasteringStereoResult {
  return addon.masteringProcessStereo(processorName, left, right, sampleRate, params);
}

export function masteringChain(
  samples: Float32Array,
  sampleRate = 22050,
  config: MasteringChainConfig = {},
  onProgress?: (progress: number, stage: string) => void,
): MasteringChainResult {
  const flat = flattenChainConfig(config);
  if (onProgress) {
    return addon.masteringChainWithProgress(samples, sampleRate, flat, onProgress);
  }
  return addon.masteringChain(samples, sampleRate, flat);
}

export function masteringChainStereo(
  left: Float32Array,
  right: Float32Array,
  sampleRate = 22050,
  config: MasteringChainConfig = {},
  onProgress?: (progress: number, stage: string) => void,
): MasteringChainStereoResult {
  const flat = flattenChainConfig(config);
  if (onProgress) {
    return addon.masteringChainStereoWithProgress(left, right, sampleRate, flat, onProgress);
  }
  return addon.masteringChainStereo(left, right, sampleRate, flat);
}

export function masteringPresetNames(): MasteringPreset[] {
  return addon.masteringPresetNames();
}

export function masterAudio(
  samples: Float32Array,
  sampleRate = 22050,
  presetName: MasteringPreset = 'pop',
  overrides: MasteringChainConfig = {},
  onProgress?: (progress: number, stage: string) => void,
): MasteringChainResult {
  const flat = flattenChainConfig(overrides);
  if (onProgress) {
    return addon.masterAudioWithProgress(presetName, samples, sampleRate, flat, onProgress);
  }
  return addon.masterAudio(presetName, samples, sampleRate, flat);
}

/**
 * Asynchronous variant of {@link masterAudio}. Runs the full chain on a libuv
 * worker thread; the returned promise resolves with the same shape as the
 * synchronous version. Progress reporting is not available on the async path
 * (use the synchronous `masterAudio` with `onProgress` if you need it, or
 * spin up multiple async calls in parallel).
 */
export function masterAudioAsync(
  samples: Float32Array,
  sampleRate = 22050,
  presetName: MasteringPreset = 'pop',
  overrides: MasteringChainConfig = {},
): Promise<MasteringChainResult> {
  return addon.masterAudioAsync(presetName, samples, sampleRate, flattenChainConfig(overrides));
}

export function masterAudioStereo(
  left: Float32Array,
  right: Float32Array,
  sampleRate = 22050,
  presetName: MasteringPreset = 'pop',
  overrides: MasteringChainConfig = {},
  onProgress?: (progress: number, stage: string) => void,
): MasteringChainStereoResult {
  const flat = flattenChainConfig(overrides);
  if (onProgress) {
    return addon.masterAudioStereoWithProgress(
      presetName,
      left,
      right,
      sampleRate,
      flat,
      onProgress,
    );
  }
  return addon.masterAudioStereo(presetName, left, right, sampleRate, flat);
}

/**
 * Asynchronous variant of {@link masterAudioStereo}.
 */
export function masterAudioStereoAsync(
  left: Float32Array,
  right: Float32Array,
  sampleRate = 22050,
  presetName: MasteringPreset = 'pop',
  overrides: MasteringChainConfig = {},
): Promise<MasteringChainStereoResult> {
  return addon.masterAudioStereoAsync(
    presetName,
    left,
    right,
    sampleRate,
    flattenChainConfig(overrides),
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
export function masteringPairProcess(
  processorName: PairProcessor,
  source: Float32Array,
  reference: Float32Array,
  sampleRate = 22050,
  params: Record<string, number | boolean> = {},
): MasteringResult {
  return addon.masteringPairProcess(processorName, source, reference, sampleRate, params);
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
  params: Record<string, number | boolean> = {},
): string {
  return addon.masteringPairAnalyze(analysisName, source, reference, sampleRate, params);
}

export function masteringStereoAnalyze(
  analysisName: StereoAnalysis,
  left: Float32Array,
  right: Float32Array,
  sampleRate = 22050,
  params: Record<string, number | boolean> = {},
): string {
  return addon.masteringStereoAnalyze(analysisName, left, right, sampleRate, params);
}

export function masteringAssistantSuggest(
  samples: Float32Array,
  sampleRate = 22050,
  params: Record<string, number | boolean> = {},
): string {
  return addon.masteringAssistantSuggest(samples, sampleRate, params);
}

export function masteringAudioProfile(
  samples: Float32Array,
  sampleRate = 22050,
  params: Record<string, number | boolean> = {},
): string {
  return addon.masteringAudioProfile(samples, sampleRate, params);
}

export function masteringStreamingPreview(
  samples: Float32Array,
  sampleRate = 22050,
  platforms: StreamingPlatform[] = [],
): string {
  return addon.masteringStreamingPreview(samples, sampleRate, platforms);
}
