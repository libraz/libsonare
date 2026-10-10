import { resolveInsertParams, resolveProcessorParams } from './_processor_params.js';
import { getSonareModule } from './module_state.js';
import type {
  LoudnessMatchResult,
  LoudnessMatchStereoResult,
  MasteringAssistantParams,
  MasteringAssistantResult,
  MasteringInsertParamChoice,
  MasteringInsertParamDependency,
  MasteringInsertParamScale,
  MasteringInsertParams,
  MasteringInsertParamUnit,
  MasteringInsertSlot,
  MasteringOptions,
  MasteringProcessorParams,
  MasteringResult,
  MasteringSoloProcessorParams,
  MasteringStereoResult,
  MasteringStreamingPreviewResult,
  MatchEqCurveResult,
  MatchEstimateReferenceDelaySamplesResult,
  MatchReferenceLoudnessResult,
  MatchTonalBalanceLogBandsResult,
  MatchTonalBalanceResult,
  PairAnalysis,
  PairAnalysisResultMap,
  PairProcessor,
  SoloProcessor,
  StereoAnalysis,
  StereoAnalysisResultMap,
  StereoMonoCompatCheckLogBandsResult,
  StereoMonoCompatCheckResult,
  StereoPairProcessor,
  StreamingPlatform,
  TypedJson,
} from './public_types.js';
import { assertAudioInput, assertString, requestObject } from './validation.js';

export type { MasteringInsertParamChoice, MasteringInsertSlot };

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
  params?: MasteringSoloProcessorParams;
}

export interface MasteringProcessStereoRequest {
  processorName: SoloProcessor;
  left: Float32Array;
  right: Float32Array;
  sampleRate?: number;
  params?: MasteringSoloProcessorParams;
}

/** Canonical request form for a two-input match processor. */
export interface MasteringPairProcessRequest {
  processorName: PairProcessor;
  source: Float32Array;
  reference: Float32Array;
  sampleRate?: number;
  params?: MasteringProcessorParams;
}

/** Canonical request form for a stereo two-input match processor. */
export interface MasteringPairProcessStereoRequest {
  processorName: StereoPairProcessor;
  sourceLeft: Float32Array;
  sourceRight: Float32Array;
  referenceLeft: Float32Array;
  referenceRight: Float32Array;
  sampleRate?: number;
  params?: MasteringProcessorParams;
}

/** Canonical request form for {@link masteringAbMatchLoudness}. */
export interface MasteringAbMatchLoudnessRequest {
  /** The take to gain-match. */
  source: Float32Array;
  /** The take whose loudness `source` is matched to; returned untouched. */
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

/** Canonical request form for a two-input match analysis. */
export interface MasteringPairAnalyzeRequest<N extends PairAnalysis = PairAnalysis> {
  analysisName: N;
  source: Float32Array;
  reference: Float32Array;
  sampleRate?: number;
  params?: MasteringProcessorParams;
}

/** Canonical request form for a stereo analysis. */
export interface MasteringStereoAnalyzeRequest<N extends StereoAnalysis = StereoAnalysis> {
  analysisName: N;
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

/** Canonical request form for the assistant, whose params carry a target platform. */
export interface MasteringAssistantParamsRequest {
  samples: Float32Array;
  sampleRate?: number;
  params?: MasteringAssistantParams;
}

/** Canonical request form for streaming-platform preview. */
export interface MasteringStreamingPreviewRequest {
  samples: Float32Array;
  sampleRate?: number;
  platforms?: StreamingPlatform[];
}

/** Canonical request form for the stereo analysis entry points. */
export interface MasteringStereoParamsRequest {
  left: Float32Array;
  right: Float32Array;
  sampleRate?: number;
  params?: MasteringProcessorParams;
}

/** Stereo counterpart of {@link MasteringAssistantParamsRequest}. */
export interface MasteringAssistantStereoParamsRequest {
  left: Float32Array;
  right: Float32Array;
  sampleRate?: number;
  params?: MasteringAssistantParams;
}

/** Canonical request form for the stereo streaming-platform preview. */
export interface MasteringStreamingPreviewStereoRequest {
  left: Float32Array;
  right: Float32Array;
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
  const request =
    samples instanceof Float32Array
      ? { samples, sampleRate, ...options }
      : requestObject('mastering', samples);
  assertAudioInput('mastering', request.samples, request.sampleRate ?? 22050);
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
  return requireModule().masteringInsertNames();
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
  assertString('masteringInsertParamNames', name, 'name');
  return Array.from(requireModule().masteringInsertParamNames(name));
}

/**
 * One parameter an insert processor's construction reads, whether or not it
 * is realtime-automatable.
 */
export interface MasteringInsertParamInfo {
  /** JSON-key parameter name, as used in scene insert params. */
  name: string;
  /**
   * Integer param id for realtime automation lanes / MIDI-CC binding, or null
   * for a construction-only key with no automation target.
   */
  id: number | null;
  /** Whether the param can be changed live from the audio thread; false when `id` is null. */
  rtSafe: boolean;
  /**
   * The C++ type the processor's config builder reads the key as. `"enum"` is
   * sent as the number in its `choices` entry; `"string"` / `"array"` (an
   * embedded impulse response, a per-band list) is construction-only and
   * reports null for `min`, `max`, `default` and `choices`.
   */
  type: 'boolean' | 'number' | 'enum' | 'string' | 'array';
  /**
   * Smallest value construction accepts, or null when the catalog states no
   * limit or `choices` is non-null. Measured, so it is a hard constraint
   * rather than a UI range; see {@link CapabilityCatalogParameter} for what a
   * measured bound does and does not promise.
   */
  min: number | null;
  /** Largest value construction accepts, or null when the catalog states no limit or `choices` is non-null. */
  max: number | null;
  /** Whether {@link min} itself is rejected (a `> min` constraint); false when `min` is null. */
  minExclusive: boolean;
  /** Whether {@link max} itself is rejected (a `< max` constraint); false when `max` is null. */
  maxExclusive: boolean;
  /**
   * `"nyquist"` when the ceiling follows the processing rate: the effective
   * ceiling is then the lower of `max` (measured at the catalog's probe rate)
   * and the host's Nyquist frequency, both exclusive. Null otherwise.
   */
  maxRelativeTo: 'nyquist' | null;
  /**
   * Value the processor uses when the key is absent — the config struct's own
   * field initializer, an enum as its number. Null for a param id with no
   * construction key, a `"string"` / `"array"` key, or a construction key with
   * no fallback.
   */
  default: boolean | number | null;
  /** Declared unit of a number; null for any other type. */
  unit: MasteringInsertParamUnit | null;
  /** Lowest value a control should draw, inside `min` / `max`; null when the accepted range is also the display range. */
  uiMin: number | null;
  /** Highest value a control should draw, inside `min` / `max`; null when the accepted range is also the display range. */
  uiMax: number | null;
  /** Axis a control draws the value on. */
  scale: MasteringInsertParamScale;
  /**
   * The closed set of values construction accepts, in value order, or null
   * when the accepted values are not a closed set. Non-null only for `"enum"`
   * (every declared enumerator construction accepts) or a `"number"` key
   * whose accepted integers have holes; `min` / `max` are then both null.
   */
  choices: MasteringInsertParamChoice[] | null;
  /**
   * The {@link MasteringInsertSlot} this key belongs to, or null for a key that
   * always exists.
   */
  slot: string | null;
  /**
   * Siblings whose live value bounds this key, each read as `this <relation> factor * sibling`;
   * empty for an independent key. A bound that only restates this dependency at the sibling's default is left null, so `min` and `max` are limits of the key's own.
   */
  dependsOn: MasteringInsertParamDependency[];
}

/**
 * Returns every parameter an insert / FX processor's construction reads,
 * including construction-time-only keys with no realtime automation target.
 * Entries come in two runs: first the processor's realtime automation
 * targets in id order (the keys accepted by
 * {@link RealtimeEngine.setTrackStripInsertParamByName}, `id` non-null); then,
 * sorted by name, every other construction key with `id` null and `rtSafe`
 * false. The name set matches {@link masteringInsertParamNames} plus any
 * automation target construction does not read. Any id of the processor
 * catalog is served, an offline repair stage (`repair.declick`,
 * `repair.declip`, `repair.trimSilence`) included, with the rows its catalog
 * entry's `params` carries; those rows have a null `id`. Returns an empty array
 * for an unknown name.
 *
 * @param name - Insert processor name (see {@link masteringInsertNames}).
 * @param sampleRate - Optional host rate in Hz. A key whose `maxRelativeTo` is
 *   `"nyquist"` then reports, as `max` and `maxExclusive`, the bound accepted
 *   when the insert is built and prepared at that rate (an EQ band frequency
 *   reaches 48000 for a 96000 Hz host). Omitted, the rate-less answer is
 *   returned, with the 24000 cap of the 48 kHz probe. Throws for a rate outside
 *   the supported range.
 */
export function masteringInsertParamInfo(
  name: string,
  sampleRate?: number,
): MasteringInsertParamInfo[] {
  assertString('masteringInsertParamInfo', name, 'name');
  const module = requireModule();
  const json =
    sampleRate === undefined
      ? module.masteringInsertParamInfo(name)
      : module.masteringInsertParamInfoAtRate(name, sampleRate);
  return JSON.parse(json) as MasteringInsertParamInfo[];
}

/** Latency and tail of one insert instance, in samples. */
export interface MasteringInsertTiming {
  /** Latency in samples at the queried sample rate. */
  latencySamples: number;
  /** Audible decay tail in samples at the queried sample rate. */
  tailSamples: number;
}

/** One built-in amp-sim rig with its resolved starting configuration. */
export interface MasteringAmpPresetCatalogEntry {
  /** Stable index used by the amp-sim `presetIndex` parameter. */
  index: number;
  /** Canonical preset identifier accepted by the amp-sim insert. */
  name: string;
  /** Effective values from the core preset, before sparse user overrides. */
  params: Record<string, number | boolean>;
}

/**
 * Returns the built-in amp-sim rigs and their resolved control values.
 *
 * The catalog is read-only metadata for hosts such as Studio. Persist only the
 * preset index and explicit overrides in a project so future core updates can
 * continue to define the canonical DSP configuration.
 */
export function masteringAmpPresetCatalog(): MasteringAmpPresetCatalogEntry[] {
  const json = requireModule().masteringAmpPresetCatalog();
  return JSON.parse(json) as MasteringAmpPresetCatalogEntry[];
}

/**
 * Latency and tail of insert `name` built from `params` and prepared at
 * `sampleRate` (`mastering::api::insert_timing`). Answers for the exact
 * instance a scene or strip would build — an oversampled saturation path, a
 * linear-phase crossover, a lookahead all change the reported latency. The
 * capability catalog's `latencySamples` / `tailSamples` are this query at
 * default parameters and 48 kHz. `effects.reverb.convolution` answers for its
 * configuration without an impulse response: its latency is its fixed
 * partition size and does not depend on one.
 *
 * A key `name`'s construction does not read is refused rather than ignored,
 * because an ignored key would answer for a configuration the caller did not
 * ask for.
 *
 * @param name - Insert processor name (see {@link masteringInsertNames}).
 * @param params - Parameter values, keyed as in {@link masteringInsertParamInfo}
 *   and shaped as the document a scene or strip insert is built from: each
 *   value matches its key's declared `type` (a finite number or boolean, the
 *   `choices` name of an `enum` key, a string for a `string` key, a list of
 *   finite numbers for an `array` key).
 * @param sampleRate - Rate the insert is prepared at.
 * @throws For an unknown `name`, a key the insert does not read, or a value its
 *   construction or `prepare` refuses; a `TypeError` for a value of the wrong
 *   type, a `RangeError` for a non-finite number.
 */
export function masteringInsertTiming(
  name: string,
  params: MasteringInsertParams,
  sampleRate: number,
): MasteringInsertTiming {
  assertString('masteringInsertTiming', name, 'name');
  const json = JSON.stringify(resolveInsertParams('masteringInsertTiming', name, params));
  return requireModule().masteringInsertTiming(name, json, sampleRate);
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

/** Coarse algorithmic work estimate for a realtime insert; not a benchmark. */
export type MasteringRealtimeCost = 'low' | 'moderate' | 'high';

/**
 * Catalog grouping for a processor picker, derived from the id's prefix
 * ("eq.*" -> "eq", "match.*" -> "reference"); anything unprefixed is "other".
 */
export type MasteringProcessorCategory =
  | 'dynamics'
  | 'effects'
  | 'eq'
  | 'final'
  | 'maximizer'
  | 'multiband'
  | 'other'
  | 'reference'
  | 'repair'
  | 'saturation'
  | 'spectral'
  | 'stereo'
  | 'utility'
  | 'voice';

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
  /** Coarse realtime work estimate, or null when the processor is not an insert. */
  realtimeCost: MasteringRealtimeCost | null;
  /**
   * How the mixer wraps the processor on a >2-channel (surround) bus insert:
   * "multichannel" (one full-buffer call) or "stereoPairOnly" (front L/R pair,
   * surround planes passed through dry).
   */
  channelPolicy: MasteringChannelPolicy;
  /** Grouping for a processor picker; see {@link MasteringProcessorCategory}. */
  category: MasteringProcessorCategory;
  /**
   * Whether the processor has a configuration whose output at a sample depends only on the
   * input up to it, plus its reported latency. False for `repair.declick`, `repair.declip` and
   * `repair.trimSilence`; true for the other repair stages and every insert.
   */
  causal: boolean;
  /**
   * The processor's construction parameters: for an insert the list
   * {@link masteringInsertParamInfo} returns, for a `repair.*` stage the bounds its own
   * configuration validation enforces (no `id`, never `rtSafe`). Empty for any other offline entry.
   */
  params: MasteringInsertParamInfo[];
  /**
   * The insert's conditional key groups in declaration order, named by each
   * parameter's `slot`. Empty for entries that are not realtime-insertable.
   */
  slots: MasteringInsertSlot[];
}

/**
 * Returns the machine-readable classification catalog for every named processor
 * id, merging the offline registry, the realtime insert factory, and the pair
 * registry. Lets a host filter a processor picker by realtime insertability
 * instead of offering ids the realtime strip would reject.
 */
export function masteringProcessorCatalog(): MasteringProcessorCatalogEntry[] {
  const json = requireModule().masteringProcessorCatalog();
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
      : requestObject('masteringProcess', processorName, 'request', true);
  assertString('masteringProcess', request.processorName, 'processorName');
  assertAudioInput('masteringProcess', request.samples, request.sampleRate ?? 22050);
  return requireModule().masteringProcess(
    request.processorName,
    request.samples,
    request.sampleRate ?? 22050,
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
      : requestObject('masteringProcessStereo', processorName, 'request', true);
  assertString('masteringProcessStereo', request.processorName, 'processorName');
  assertAudioInput('masteringProcessStereo', request.left, request.sampleRate ?? 22050, {}, 'left');
  assertAudioInput(
    'masteringProcessStereo',
    request.right,
    request.sampleRate ?? 22050,
    {},
    'right',
  );
  if (request.left.length !== request.right.length) {
    throw new RangeError('Stereo channel lengths must match.');
  }
  return requireModule().masteringProcessStereo(
    request.processorName,
    request.left,
    request.right,
    request.sampleRate ?? 22050,
    resolveProcessorParams(request.processorName, request.params ?? {}),
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
      : requestObject('masteringPairProcess', processorName, 'request', true);
  assertString('masteringPairProcess', request.processorName, 'processorName');
  assertAudioInput(
    'masteringPairProcess',
    request.source,
    request.sampleRate ?? 22050,
    {},
    'source',
  );
  assertAudioInput(
    'masteringPairProcess',
    request.reference,
    request.sampleRate ?? 22050,
    {},
    'reference',
  );
  return requireModule().masteringPairProcess(
    request.processorName,
    request.source,
    request.reference,
    request.sampleRate ?? 22050,
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
  params?: MasteringProcessorParams,
): MasteringStereoResult;
export function masteringPairProcessStereo(
  processorName: StereoPairProcessor | MasteringPairProcessStereoRequest,
  sourceLeft?: Float32Array,
  sourceRight?: Float32Array,
  referenceLeft?: Float32Array,
  referenceRight?: Float32Array,
  sampleRate = 22050,
  params: MasteringProcessorParams = {},
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
      : requestObject('masteringPairProcessStereo', processorName, 'request', true);
  assertString('masteringPairProcessStereo', request.processorName, 'processorName');
  assertAudioInput(
    'masteringPairProcessStereo',
    request.sourceLeft,
    request.sampleRate ?? 22050,
    {},
    'sourceLeft',
  );
  assertAudioInput(
    'masteringPairProcessStereo',
    request.sourceRight,
    request.sampleRate ?? 22050,
    {},
    'sourceRight',
  );
  assertAudioInput(
    'masteringPairProcessStereo',
    request.referenceLeft,
    request.sampleRate ?? 22050,
    {},
    'referenceLeft',
  );
  assertAudioInput(
    'masteringPairProcessStereo',
    request.referenceRight,
    request.sampleRate ?? 22050,
    {},
    'referenceRight',
  );
  if (request.sourceLeft.length !== request.sourceRight.length) {
    throw new RangeError('Source left and right channel lengths must match.');
  }
  if (request.referenceLeft.length !== request.referenceRight.length) {
    throw new RangeError('Reference left and right channel lengths must match.');
  }
  return requireModule().masteringPairProcessStereo(
    request.processorName,
    request.sourceLeft,
    request.sourceRight,
    request.referenceLeft,
    request.referenceRight,
    request.sampleRate ?? 22050,
    request.params ?? {},
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
  params?: MasteringProcessorParams,
): TypedJson<MatchReferenceLoudnessResult>;
export function masteringPairAnalyze(
  analysisName: 'match.tonalBalance',
  source: Float32Array,
  reference: Float32Array,
  sampleRate?: number,
  params?: MasteringProcessorParams,
): TypedJson<MatchTonalBalanceResult>;
export function masteringPairAnalyze(
  analysisName: 'match.tonalBalanceLogBands',
  source: Float32Array,
  reference: Float32Array,
  sampleRate?: number,
  params?: MasteringProcessorParams,
): TypedJson<MatchTonalBalanceLogBandsResult>;
export function masteringPairAnalyze(
  analysisName: 'match.matchEqCurve',
  source: Float32Array,
  reference: Float32Array,
  sampleRate?: number,
  params?: MasteringProcessorParams,
): TypedJson<MatchEqCurveResult>;
export function masteringPairAnalyze(
  analysisName: 'match.estimateReferenceDelaySamples',
  source: Float32Array,
  reference: Float32Array,
  sampleRate?: number,
  params?: MasteringProcessorParams,
): TypedJson<MatchEstimateReferenceDelaySamplesResult>;
export function masteringPairAnalyze(
  analysisName: PairAnalysis,
  source: Float32Array,
  reference: Float32Array,
  sampleRate?: number,
  params?: MasteringProcessorParams,
): TypedJson<PairAnalysisResultMap[PairAnalysis]>;
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
      : requestObject('masteringPairAnalyze', analysisName, 'request', true);
  assertString('masteringPairAnalyze', request.analysisName, 'analysisName');
  assertAudioInput(
    'masteringPairAnalyze',
    request.source,
    request.sampleRate ?? 22050,
    {},
    'source',
  );
  assertAudioInput(
    'masteringPairAnalyze',
    request.reference,
    request.sampleRate ?? 22050,
    {},
    'reference',
  );
  return requireModule().masteringPairAnalyze(
    request.analysisName,
    request.source,
    request.reference,
    request.sampleRate ?? 22050,
    request.params ?? {},
  );
}

/**
 * Gain-match `source` to `reference`'s integrated loudness, so an A/B between
 * the two is not decided by level. `source` and `reference` may have
 * independent lengths.
 *
 * The gain is applied with no upper bound and `matchedTruePeakDbtp` reports
 * where that left the peak, rather than the call capping it: a headroom clamp
 * would return `source` at its own loudness whenever it started near full
 * scale. Both loudness values are non-finite for a silent or below-gate take,
 * and `appliedGainDb` is then 0.
 *
 * @example
 * ```ts
 * const { samples, appliedGainDb, matchedTruePeakDbtp } = masteringAbMatchLoudness({
 *   source: take,
 *   reference: master,
 *   sampleRate: 48000,
 * });
 * ```
 */
export function masteringAbMatchLoudness(
  request: MasteringAbMatchLoudnessRequest,
): LoudnessMatchResult {
  requestObject('masteringAbMatchLoudness', request, 'request', true);
  assertAudioInput(
    'masteringAbMatchLoudness',
    request.source,
    request.sampleRate ?? 22050,
    {},
    'source',
  );
  assertAudioInput(
    'masteringAbMatchLoudness',
    request.reference,
    request.sampleRate ?? 22050,
    {},
    'reference',
  );
  return requireModule().masteringAbMatchLoudness(
    request.source,
    request.reference,
    request.sampleRate ?? 22050,
  );
}

/** Gain-match a stereo source to a stereo reference with one shared gain. */
export function masteringAbMatchLoudnessStereo(
  request: MasteringAbMatchLoudnessStereoRequest,
): LoudnessMatchStereoResult {
  requestObject('masteringAbMatchLoudnessStereo', request, 'request', true);
  assertAudioInput(
    'masteringAbMatchLoudnessStereo',
    request.sourceLeft,
    request.sampleRate ?? 22050,
    {},
    'sourceLeft',
  );
  assertAudioInput(
    'masteringAbMatchLoudnessStereo',
    request.sourceRight,
    request.sampleRate ?? 22050,
    {},
    'sourceRight',
  );
  assertAudioInput(
    'masteringAbMatchLoudnessStereo',
    request.referenceLeft,
    request.sampleRate ?? 22050,
    {},
    'referenceLeft',
  );
  assertAudioInput(
    'masteringAbMatchLoudnessStereo',
    request.referenceRight,
    request.sampleRate ?? 22050,
    {},
    'referenceRight',
  );
  if (request.sourceLeft.length !== request.sourceRight.length) {
    throw new RangeError('Source left and right channel lengths must match.');
  }
  if (request.referenceLeft.length !== request.referenceRight.length) {
    throw new RangeError('Reference left and right channel lengths must match.');
  }
  return requireModule().masteringAbMatchLoudnessStereo(
    request.sourceLeft,
    request.sourceRight,
    request.referenceLeft,
    request.referenceRight,
    request.sampleRate ?? 22050,
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
  params?: MasteringProcessorParams,
): TypedJson<StereoMonoCompatCheckResult>;
export function masteringStereoAnalyze(
  analysisName: 'stereo.monoCompatCheckLogBands',
  left: Float32Array,
  right: Float32Array,
  sampleRate?: number,
  params?: MasteringProcessorParams,
): TypedJson<StereoMonoCompatCheckLogBandsResult>;
export function masteringStereoAnalyze(
  analysisName: StereoAnalysis,
  left: Float32Array,
  right: Float32Array,
  sampleRate?: number,
  params?: MasteringProcessorParams,
): TypedJson<StereoAnalysisResultMap[StereoAnalysis]>;
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
      : requestObject('masteringStereoAnalyze', analysisName, 'request', true);
  assertString('masteringStereoAnalyze', request.analysisName, 'analysisName');
  assertAudioInput('masteringStereoAnalyze', request.left, request.sampleRate ?? 22050, {}, 'left');
  assertAudioInput(
    'masteringStereoAnalyze',
    request.right,
    request.sampleRate ?? 22050,
    {},
    'right',
  );
  return requireModule().masteringStereoAnalyze(
    request.analysisName,
    request.left,
    request.right,
    request.sampleRate ?? 22050,
    request.params ?? {},
  );
}

export function masteringAssistantSuggest(
  request: MasteringAssistantParamsRequest,
): TypedJson<MasteringAssistantResult>;
export function masteringAssistantSuggest(
  samples: Float32Array,
  sampleRate?: number,
  params?: MasteringAssistantParams,
): TypedJson<MasteringAssistantResult>;
export function masteringAssistantSuggest(
  samples: Float32Array | MasteringAssistantParamsRequest,
  sampleRate = 22050,
  params: MasteringAssistantParams = {},
): string {
  const request =
    samples instanceof Float32Array
      ? { samples, sampleRate, params }
      : requestObject('masteringAssistantSuggest', samples);
  assertAudioInput('masteringAssistantSuggest', request.samples, request.sampleRate ?? 22050);
  return requireModule().masteringAssistantSuggest(
    request.samples,
    request.sampleRate ?? 22050,
    request.params ?? {},
  );
}

/**
 * Suggest a mastering chain, as the flat `{key: number|boolean}` params map
 * {@link masteringAssistantSuggest}'s `chainConfig` carries, without needing to
 * pull it out of the full assistant document. The returned map can be passed
 * straight through as `overrides` to {@link mastering} / {@link masterAudio}.
 */
export function masteringAssistantSuggestChain(
  request: MasteringAssistantParamsRequest,
): Record<string, number | boolean> {
  requestObject('masteringAssistantSuggestChain', request, 'request', true);
  assertAudioInput('masteringAssistantSuggestChain', request.samples, request.sampleRate ?? 22050);
  return requireModule().masteringAssistantSuggestChain(
    request.samples,
    request.sampleRate ?? 22050,
    request.params ?? {},
  );
}

/** The repair-defect block of {@link MasteringAudioProfile}, and of each channel of a repair analysis. */
export interface MasteringAudioProfileDefects {
  measured: boolean;
  clickCount: number;
  clickRejected: number;
  clickLongestRunSamples: number;
  clickPerSecond: number;
  crackleSampleCount: number;
  crackleSampleFraction: number;
  cracklePerSecond: number;
  clipSampleCount: number;
  clipRunCount: number;
  clipLongestRunSamples: number;
  clipSampleFraction: number;
  clipFlatRunCount: number;
  clipFlatSampleCount: number;
  clipLongestFlatRunSamples: number;
  clipFlatLevel: number;
  noiseFloorDbfs: number;
  noiseBandPeakDbfs: number;
  noiseBandPeakIndex: number;
  humFundamentalHz: number;
  humFundamentalProminence: number;
  humHarmonics: number;
  humFundamentalDbfs: number;
  humPeakHarmonicDbfs: number;
  lateDecayRatioDb: number;
}

/**
 * The shape {@link masteringAudioProfile}'s JSON parses to.
 *
 * The profile crosses as a string, so nothing type-checks it on arrival; this
 * declaration is what a conformance check compares against the paths the C++
 * writer publishes, so a field added on one side and not the other fails there
 * rather than reaching a caller as `undefined`.
 */
export interface MasteringAudioProfile {
  durationSec: number;
  bpm: number;
  bpmConfidence: number;
  loudness: {
    integratedLufs: number;
    lraLu: number;
    truePeakDb: number;
    crestFactorDb: number;
  };
  /** Band levels (`*RmsDb`) are dBFS, mean square: a full-scale sine reads -3.01 dBFS in its band. */
  spectral: {
    subRmsDb: number;
    lowRmsDb: number;
    lowMidRmsDb: number;
    midRmsDb: number;
    highMidRmsDb: number;
    highRmsDb: number;
    airRmsDb: number;
    centroidHz: number;
    flatness: number;
    rolloffHz: number;
  };
  dynamics: {
    shortTermLufsStd: number;
    attackDensity: number;
    sustainRatio: number;
  };
  /**
   * What the repair detectors measured. `measured` is false when nothing ran —
   * either `detectDefects` was not asked for or the input was too short — and
   * every other field is then at its default rather than a reading.
   */
  defects: MasteringAudioProfileDefects;
}

export function masteringAudioProfile(
  request: MasteringSamplesParamsRequest,
): TypedJson<MasteringAudioProfile>;
export function masteringAudioProfile(
  samples: Float32Array,
  sampleRate?: number,
  params?: MasteringProcessorParams,
): TypedJson<MasteringAudioProfile>;
export function masteringAudioProfile(
  samples: Float32Array | MasteringSamplesParamsRequest,
  sampleRate = 22050,
  params: MasteringProcessorParams = {},
): string {
  const request =
    samples instanceof Float32Array
      ? { samples, sampleRate, params }
      : requestObject('masteringAudioProfile', samples);
  assertAudioInput('masteringAudioProfile', request.samples, request.sampleRate ?? 22050);
  return requireModule().masteringAudioProfile(
    request.samples,
    request.sampleRate ?? 22050,
    request.params ?? {},
  );
}

export function masteringStreamingPreview(
  request: MasteringStreamingPreviewRequest,
): TypedJson<MasteringStreamingPreviewResult>;
export function masteringStreamingPreview(
  samples: Float32Array,
  sampleRate?: number,
  platforms?: StreamingPlatform[],
): TypedJson<MasteringStreamingPreviewResult>;
export function masteringStreamingPreview(
  samples: Float32Array | MasteringStreamingPreviewRequest,
  sampleRate = 22050,
  platforms: StreamingPlatform[] = [],
): string {
  const request =
    samples instanceof Float32Array
      ? { samples, sampleRate, platforms }
      : requestObject('masteringStreamingPreview', samples);
  assertAudioInput('masteringStreamingPreview', request.samples, request.sampleRate ?? 22050);
  return requireModule().masteringStreamingPreview(
    request.samples,
    request.sampleRate ?? 22050,
    request.platforms ?? [],
  );
}

/**
 * The stereo helpers below take a request object only. A `Float32Array` in the
 * first position is the positional spelling of the mono helpers, and would
 * otherwise surface as an unrelated complaint about `left`.
 */
function assertStereoRequest(fnName: string, request: unknown): void {
  if (request === null || typeof request !== 'object' || ArrayBuffer.isView(request)) {
    throw new TypeError(`${fnName} takes a request object { left, right, sampleRate }`);
  }
}

/**
 * Suggest a mastering chain for a stereo pair, as shared JSON.
 *
 * Profiles through {@link masteringAudioProfileStereo}, so the loudness stage
 * of the suggestion is built on the channel-summed program rather than a
 * downmix that reads roughly 6 dB low.
 */
export function masteringAssistantSuggestStereo(
  request: MasteringAssistantStereoParamsRequest,
): TypedJson<MasteringAssistantResult> {
  assertStereoRequest('masteringAssistantSuggestStereo', request);
  assertAudioInput(
    'masteringAssistantSuggestStereo',
    request.left,
    request.sampleRate ?? 22050,
    {},
    'left',
  );
  assertAudioInput(
    'masteringAssistantSuggestStereo',
    request.right,
    request.sampleRate ?? 22050,
    {},
    'right',
  );
  return requireModule().masteringAssistantSuggestStereo(
    request.left,
    request.right,
    request.sampleRate ?? 22050,
    request.params ?? {},
  );
}

/**
 * Stereo counterpart of {@link masteringAssistantSuggestChain}: the flat
 * `{key: number|boolean}` params map without the surrounding assistant
 * document, ready to pass through as `overrides` to {@link masterAudioStereo}.
 */
export function masteringAssistantSuggestChainStereo(
  request: MasteringAssistantStereoParamsRequest,
): Record<string, number | boolean> {
  assertStereoRequest('masteringAssistantSuggestChainStereo', request);
  assertAudioInput(
    'masteringAssistantSuggestChainStereo',
    request.left,
    request.sampleRate ?? 22050,
    {},
    'left',
  );
  assertAudioInput(
    'masteringAssistantSuggestChainStereo',
    request.right,
    request.sampleRate ?? 22050,
    {},
    'right',
  );
  return requireModule().masteringAssistantSuggestChainStereo(
    request.left,
    request.right,
    request.sampleRate ?? 22050,
    request.params ?? {},
  );
}

/**
 * Mastering assistant profile of a stereo pair, as shared JSON.
 *
 * The `loudness` block is measured from the two channels: integrated LUFS
 * and LRA come from the channel-summed program and the true peak is the larger
 * of the two. The spectral, dynamics and tempo fields describe shape and timing
 * rather than absolute level and are measured on the downmix, which keeps them
 * comparable with {@link masteringAudioProfile}. Defect detectors run on each
 * channel and their results are aggregated.
 */
export function masteringAudioProfileStereo(
  request: MasteringStereoParamsRequest,
): TypedJson<MasteringAudioProfile> {
  assertStereoRequest('masteringAudioProfileStereo', request);
  assertAudioInput(
    'masteringAudioProfileStereo',
    request.left,
    request.sampleRate ?? 22050,
    {},
    'left',
  );
  assertAudioInput(
    'masteringAudioProfileStereo',
    request.right,
    request.sampleRate ?? 22050,
    {},
    'right',
  );
  return requireModule().masteringAudioProfileStereo(
    request.left,
    request.right,
    request.sampleRate ?? 22050,
    request.params ?? {},
  );
}

/**
 * Preview streaming-platform normalization for a stereo pair, as shared JSON.
 *
 * Measures the integrated loudness with BS.1770 channel summing and reports the
 * larger of the two channel true peaks. Passing a `0.5 * (left + right)` downmix
 * to {@link masteringStreamingPreview} instead reads roughly 6 dB low on
 * decorrelated material, and both the normalization gain and the ceiling-risk
 * flag follow from that measurement.
 */
export function masteringStreamingPreviewStereo(
  request: MasteringStreamingPreviewStereoRequest,
): TypedJson<MasteringStreamingPreviewResult> {
  assertStereoRequest('masteringStreamingPreviewStereo', request);
  assertAudioInput(
    'masteringStreamingPreviewStereo',
    request.left,
    request.sampleRate ?? 22050,
    {},
    'left',
  );
  assertAudioInput(
    'masteringStreamingPreviewStereo',
    request.right,
    request.sampleRate ?? 22050,
    {},
    'right',
  );
  return requireModule().masteringStreamingPreviewStereo(
    request.left,
    request.right,
    request.sampleRate ?? 22050,
    request.platforms ?? [],
  );
}
