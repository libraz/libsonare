import { chainConfigFromDocument } from './internal/chain_config.js';
import { refuse, refuseType, refuseUnknownKey, withEntry } from './internal/entry.js';
import { refuseZeroSentinel } from './internal/metering_native.js';
import { resolveInsertParams, resolveProcessorParams } from './internal/processor_params.js';
import { parseRecordJson } from './internal/record_json.js';
import { audioDefectsFromNative } from './internal/repair_common.js';
import { isNonArrayObject } from './internal/value_guards.js';
import { masteringPlatformNames } from './mastering_chain.js';
import { getSonareModule } from './module_state.js';
import type {
  CapabilityCatalog,
  LoudnessMatchResult,
  LoudnessMatchStereoResult,
  MasteringAssistantResult,
  MasteringAudioProfile,
  MasteringAudioProfileDefects,
  MasteringChannelPolicy,
  MasteringInsertName,
  MasteringInsertParamChoice,
  MasteringInsertParamInfo,
  MasteringInsertSlot,
  MasteringInsertTiming,
  MasteringNormalizeLoudnessOptions,
  MasteringNormalizeLoudnessResult,
  MasteringPairParams,
  MasteringPlatform,
  MasteringPreset,
  MasteringProcessorCatalogEntry,
  MasteringProcessorCategory,
  MasteringProcessorParams,
  MasteringRealtimeCost,
  MasteringResult,
  MasteringStereoResult,
  MasteringStreamingPreviewResult,
  PairAnalysis,
  PairAnalysisResultMap,
  PairProcessor,
  SoloProcessor,
  StereoAnalysis,
  StereoAnalysisResultMap,
  StereoPairProcessor,
  StreamingPlatform,
} from './public_types.js';
import type { OptionsOf } from './validation.js';
import {
  assertAudioInput,
  assertClosedToken,
  assertFloatValue,
  assertIntegerValue,
  assertPositiveFloatValue,
  assertSampleRate,
  assertString,
  optionalBoolean,
  requestOnly,
  sampleOneShot,
} from './validation.js';

export type {
  MasteringAudioProfile,
  MasteringAudioProfileDefects,
  MasteringChannelPolicy,
  MasteringInsertParamChoice,
  MasteringInsertParamInfo,
  MasteringInsertSlot,
  MasteringInsertTiming,
  MasteringProcessorCatalogEntry,
  MasteringProcessorCategory,
  MasteringRealtimeCost,
};

function requireModule() {
  return getSonareModule();
}

/** Request form of {@link masteringNormalizeLoudness}. */
export interface MasteringNormalizeLoudnessRequest extends MasteringNormalizeLoudnessOptions {
  /** Mono input samples. */
  samples: Float32Array;
  /** Sample rate in Hz, an integer in `[8000, 384000]`; there is no default. */
  sampleRate: number;
}

/** Request form of {@link masteringApplyProcessor}. */
export interface MasteringApplyProcessorRequest<P extends SoloProcessor = SoloProcessor> {
  /** Name of the processor to apply (see `masteringProcessorNames`). */
  processor: P;
  /** Mono input samples. */
  samples: Float32Array;
  /** Sample rate in Hz, an integer in `[8000, 384000]`; there is no default. */
  sampleRate: number;
  /** Parameters of the processor. */
  params?: MasteringProcessorParams<P>;
}

/** Request form of {@link masteringApplyProcessorStereo}. */
export interface MasteringApplyProcessorStereoRequest<P extends SoloProcessor = SoloProcessor> {
  /** Name of the processor to apply to the pair. */
  processor: P;
  /** Left channel samples. */
  left: Float32Array;
  /** Right channel samples. */
  right: Float32Array;
  /** Sample rate in Hz, an integer in `[8000, 384000]`; there is no default. */
  sampleRate: number;
  /** Parameters of the processor. */
  params?: MasteringProcessorParams<P>;
}

/** Request form of {@link masteringApplyPairProcessor}. */
export interface MasteringApplyPairProcessorRequest<P extends PairProcessor = PairProcessor> {
  /** Name of the pair processor to apply (see `masteringPairProcessorNames`). */
  processor: P;
  /** The take the processor is applied to. */
  source: Float32Array;
  /** The take the processor reads as its reference. */
  reference: Float32Array;
  /** Sample rate in Hz, an integer in `[8000, 384000]`; there is no default. */
  sampleRate: number;
  /**
   * Rate of the reference, in Hz. Omitted: `sampleRate`. A reference at another
   * rate is resampled onto `sampleRate` first.
   */
  referenceSampleRate?: number;
  /** Parameters of the processor. */
  params?: MasteringPairParams;
}

/** Request form of {@link masteringApplyPairProcessorStereo}. */
export interface MasteringApplyPairProcessorStereoRequest {
  /** Name of the pair processor to apply (see `masteringPairProcessorNames`). */
  processor: StereoPairProcessor;
  /** Left channel of the source take. */
  sourceLeft: Float32Array;
  /** Right channel of the source take. */
  sourceRight: Float32Array;
  /** Left channel of the reference take. */
  referenceLeft: Float32Array;
  /** Right channel of the reference take. */
  referenceRight: Float32Array;
  /** Sample rate in Hz, an integer in `[8000, 384000]`; there is no default. */
  sampleRate: number;
  /**
   * Rate of the reference, in Hz. Omitted: `sampleRate`. A reference at another
   * rate is resampled onto `sampleRate` first.
   */
  referenceSampleRate?: number;
  /** Parameters of the processor. */
  params?: MasteringPairParams;
}

/** Request form of {@link masteringAbMatchLoudness}. */
export interface MasteringAbMatchLoudnessRequest {
  /** The take to gain-match. */
  source: Float32Array;
  /** The take whose loudness `source` is matched to; returned untouched. */
  reference: Float32Array;
  /** Sample rate in Hz, an integer in `[8000, 384000]`; there is no default. */
  sampleRate: number;
  /**
   * Rate of the reference, in Hz. Omitted: `sampleRate`. A reference at another
   * rate is resampled onto `sampleRate` and measured there.
   */
  referenceSampleRate?: number;
}

/** Request form of {@link masteringAbMatchLoudnessStereo}. */
export interface MasteringAbMatchLoudnessStereoRequest {
  /** Left channel of the take to gain-match. */
  sourceLeft: Float32Array;
  /** Right channel of the take to gain-match. */
  sourceRight: Float32Array;
  /** Left channel of the take whose loudness `source` is matched to; returned untouched. */
  referenceLeft: Float32Array;
  /** Right channel of the take whose loudness `source` is matched to; returned untouched. */
  referenceRight: Float32Array;
  /** Sample rate in Hz, an integer in `[8000, 384000]`; there is no default. */
  sampleRate: number;
  /**
   * Rate of the reference, in Hz. Omitted: `sampleRate`. A reference at another
   * rate is resampled onto `sampleRate` and measured there.
   */
  referenceSampleRate?: number;
}

/** Request form of {@link masteringAnalyzePair}. */
export interface MasteringAnalyzePairRequest<A extends PairAnalysis = PairAnalysis> {
  /** Name of the pair analysis to run (see `masteringPairAnalysisNames`). */
  analysis: A;
  /** The take being analyzed. */
  source: Float32Array;
  /** The take it is compared against. */
  reference: Float32Array;
  /** Sample rate in Hz, an integer in `[8000, 384000]`; there is no default. */
  sampleRate: number;
  /**
   * Rate of the reference, in Hz. Omitted: `sampleRate`. A reference at another
   * rate is resampled onto `sampleRate` first, so a delay estimate is in source-rate samples.
   */
  referenceSampleRate?: number;
  /** Parameters of the analysis. */
  params?: MasteringPairParams;
}

/** Request form of {@link masteringAnalyzeStereo}. */
export interface MasteringAnalyzeStereoRequest<A extends StereoAnalysis = StereoAnalysis> {
  /** Name of the stereo analysis to run (see `masteringStereoAnalysisNames`). */
  analysis: A;
  /** Left channel samples. */
  left: Float32Array;
  /** Right channel samples. */
  right: Float32Array;
  /** Sample rate in Hz, an integer in `[8000, 384000]`; there is no default. */
  sampleRate: number;
  /** Parameters of the analysis. */
  params?: MasteringPairParams;
}

/** Request form of {@link masteringAudioProfile}. */
export interface MasteringAudioProfileRequest {
  /** Mono input samples. */
  samples: Float32Array;
  /** Sample rate in Hz, an integer in `[8000, 384000]`; there is no default. */
  sampleRate: number;
  /** Analysis window in samples at 48000 Hz, rescaled to the input rate; a positive integer. */
  nFft?: number;
  /** Analysis hop in samples at 48000 Hz, rescaled to the input rate; a positive integer. */
  hopLength?: number;
  /** True-peak oversampling factor; a positive integer. */
  truePeakOversample?: number;
  /** Measure the repair defects. Default true; false leaves `defects` absent. */
  detectDefects?: boolean;
}

/** Stereo counterpart of {@link MasteringAudioProfileRequest}. */
export interface MasteringAudioProfileStereoRequest {
  /** Left channel samples. */
  left: Float32Array;
  /** Right channel samples. */
  right: Float32Array;
  /** Sample rate in Hz, an integer in `[8000, 384000]`; there is no default. */
  sampleRate: number;
  /** Analysis window in samples at 48000 Hz, rescaled to the input rate; a positive integer. */
  nFft?: number;
  /** Analysis hop in samples at 48000 Hz, rescaled to the input rate; a positive integer. */
  hopLength?: number;
  /** True-peak oversampling factor; a positive integer. */
  truePeakOversample?: number;
  /** Measure the repair defects. Default `true`; `false` leaves `defects` absent. */
  detectDefects?: boolean;
}

/** Fields the assistant reads beside the audio. */
export interface MasteringAssistantSettings {
  /** Integrated loudness target in LUFS. */
  targetLufs?: number;
  /** True-peak ceiling in dBFS. */
  ceilingDbtp?: number;
  /** Let the suggestion enable repair stages. */
  enableRepair?: boolean;
  /** Prefer settings that survive streaming-service normalization. */
  preferStreamingSafe?: boolean;
  /** Blend factor in `[0, 1]` for the speech mono fold. */
  speechMonoAmount?: number;
  /** Delivery target the loudness and ceiling follow. */
  targetPlatform?: MasteringPlatform;
  /** Mastering preset the suggestion starts from; restoration presets are refused. */
  preset?: MasteringPreset;
}

/** Request form of {@link masteringAssistantSuggest}. */
export interface MasteringAssistantSuggestRequest extends MasteringAssistantSettings {
  /** Mono input samples. */
  samples: Float32Array;
  /** Sample rate in Hz, an integer in `[8000, 384000]`; there is no default. */
  sampleRate: number;
}

/** Request form of {@link masteringAssistantSuggestStereo}. */
export interface MasteringAssistantSuggestStereoRequest extends MasteringAssistantSettings {
  /** Left channel samples. */
  left: Float32Array;
  /** Right channel samples. */
  right: Float32Array;
  /** Sample rate in Hz, an integer in `[8000, 384000]`; there is no default. */
  sampleRate: number;
}

/** Request form of {@link masteringStreamingPreview}. */
export interface MasteringStreamingPreviewRequest {
  /** Mono input samples. */
  samples: Float32Array;
  /** Sample rate in Hz, an integer in `[8000, 384000]`; there is no default. */
  sampleRate: number;
  /** Delivery targets to preview; absent: the built-in set. */
  platforms?: StreamingPlatform[];
}

/** The fields of a streaming platform record. */
const PLATFORM_KEYS = ['name', 'targetLufs', 'ceilingDbtp'];

/** The preview targets in the native spelling: absent is the built-in set, an empty list is refused. */
function previewPlatforms(platforms: unknown): StreamingPlatform[] {
  if (platforms === undefined || platforms === null) {
    return [];
  }
  if (!Array.isArray(platforms)) {
    throw refuseType(`platforms must be an array`);
  }
  if (platforms.length === 0) {
    throw refuse(`platforms must not be empty; omit it for the built-in set`);
  }
  platforms.forEach((platform, i) => {
    if (!isNonArrayObject(platform)) {
      throw refuseType(`platforms[${i}] must be an object`);
    }
    for (const key of Object.keys(platform)) {
      if (!PLATFORM_KEYS.includes(key)) {
        throw refuseUnknownKey(`platforms[${i}].${key}`, PLATFORM_KEYS);
      }
    }
    const { name, targetLufs, ceilingDbtp } = platform as Record<string, unknown>;
    if (typeof name !== 'string') {
      throw refuseType(`platforms[${i}].name must be a string`);
    }
    if (typeof targetLufs !== 'number') {
      throw refuseType(`platforms[${i}].targetLufs must be a number`);
    }
    if (typeof ceilingDbtp !== 'number') {
      throw refuseType(`platforms[${i}].ceilingDbtp must be a number`);
    }
  });
  return platforms as StreamingPlatform[];
}

/** Request form of {@link masteringStreamingPreviewStereo}. */
export interface MasteringStreamingPreviewStereoRequest {
  /** Left channel samples. */
  left: Float32Array;
  /** Right channel samples. */
  right: Float32Array;
  /** Sample rate in Hz, an integer in `[8000, 384000]`; there is no default. */
  sampleRate: number;
  /** Delivery targets to preview; absent: the built-in set. */
  platforms?: StreamingPlatform[];
}

/** Request form of {@link masteringInsertParamInfo}. */
export interface MasteringInsertParamInfoRequest {
  /** Insert processor name (see {@link masteringInsertNames}). */
  insert: MasteringInsertName;
  /**
   * Host rate in Hz. Absent: the rate-less catalog view, where a `maxRelativeTo`
   * of `"nyquist"` keeps the 24000 cap of the 48 kHz probe.
   */
  sampleRate?: number;
}

/** Request form of {@link masteringInsertTiming}. */
export interface MasteringInsertTimingRequest<N extends MasteringInsertName = MasteringInsertName> {
  /** Insert processor name (see {@link masteringInsertNames}). */
  insert: N;
  /** Parameters of the insert. */
  params?: MasteringProcessorParams<N>;
  /** Rate the insert is prepared at, in Hz; there is no default. */
  sampleRate: number;
}

// `releaseMs` and `truePeakOversample` read 0 as "library default" natively, so an omitted one is passed as that 0.
function nativeLoudnessArgs(r: MasteringNormalizeLoudnessRequest) {
  if (r.targetLufs !== undefined && r.targetLufs !== null) {
    assertFloatValue(r.targetLufs, 'targetLufs');
  }
  if (r.ceilingDbtp !== undefined && r.ceilingDbtp !== null) {
    assertFloatValue(r.ceilingDbtp, 'ceilingDbtp');
  }
  refuseZeroSentinel(r, ['truePeakOversample']);
  if (r.truePeakOversample !== undefined && r.truePeakOversample !== null) {
    assertIntegerValue(r.truePeakOversample, 'truePeakOversample');
  }
  if (r.releaseMs !== undefined && r.releaseMs !== null) {
    assertPositiveFloatValue(r.releaseMs, 'releaseMs');
  }
  return [
    r.targetLufs ?? -14.0,
    r.ceilingDbtp ?? -1.0,
    r.truePeakOversample ?? 0,
    r.releaseMs ?? 0,
    optionalBoolean(r.applyGainAtInputRate, 'applyGainAtInputRate', false),
  ] as const;
}

/**
 * Normalize loudness to a target and hold a true-peak ceiling.
 *
 * @param request - Mono `samples`, `sampleRate` in Hz, an integer in `[8000, 384000]`, and the
 *   loudness/ceiling settings ({@link MasteringNormalizeLoudnessOptions}); or pass
 *   `(samples, sampleRate, options?)`.
 * @returns Processed audio and loudness metadata
 * @throws {TypeError} when `sampleRate` is missing or a field has the wrong type
 * @throws {RangeError} on audio out of domain, or an explicit 0 `truePeakOversample` or `releaseMs`
 *   (omit it for the default)
 */
export function masteringNormalizeLoudness(
  request: MasteringNormalizeLoudnessRequest,
): MasteringNormalizeLoudnessResult;
export function masteringNormalizeLoudness(
  samples: Float32Array,
  sampleRate: number,
  options?: OptionsOf<MasteringNormalizeLoudnessRequest>,
): MasteringNormalizeLoudnessResult;
export function masteringNormalizeLoudness(...args: unknown[]): MasteringNormalizeLoudnessResult {
  return withEntry('masteringNormalizeLoudness', () => {
    const r = sampleOneShot<MasteringNormalizeLoudnessRequest>('masteringNormalizeLoudness', args);
    assertAudioInput(r.samples, r.sampleRate, r);
    const result = requireModule().mastering(r.samples, r.sampleRate, ...nativeLoudnessArgs(r));
    return result as MasteringNormalizeLoudnessResult;
  });
}

/** Names of the processors `masteringApplyProcessor` accepts. */
export function masteringProcessorNames(): SoloProcessor[] {
  return withEntry('masteringProcessorNames', () => {
    // embind hands back a vector whose constructor is not this realm's Array, so the
    // result is not structured-cloneable (breaks postMessage to a Worker).
    // Array.from() re-roots it as a plain Array. Same for the sibling *Names() below.
    return Array.from(requireModule().masteringProcessorNames()) as SoloProcessor[];
  });
}

/**
 * Names of the insert processors the mastering chain can instantiate by name
 * (`mastering::api::insert_factory_names`). Mirrors the C-ABI
 * `sonare_mastering_insert_names` (which joins this list) as an array.
 */
export function masteringInsertNames(): MasteringInsertName[] {
  return withEntry('masteringInsertNames', () => {
    return Array.from(requireModule().masteringInsertNames()) as MasteringInsertName[];
  });
}

/**
 * Returns the camelCase parameter names a given insert / FX processor reads, for
 * tooling/validation. Any key NOT in this list is silently ignored by the
 * processor (and would be reported via {@link Mixer.sceneWarnings} when a scene
 * carrying it is loaded). Band/sub-band processors enumerate their indexed
 * `band{i}.<field>` keys.
 *
 * @param insert - Insert processor name (see {@link masteringInsertNames}).
 * @throws {TypeError} when `insert` is not a string
 * @throws {RangeError} for an unknown `insert`
 */
export function masteringInsertParamNames<N extends MasteringInsertName>(
  insert: N,
): (keyof MasteringProcessorParams<N> & string)[] {
  return withEntry('masteringInsertParamNames', () => {
    assertClosedToken(
      insert,
      withEntry('masteringInsertParamNames', () => masteringInsertNames()),
      'insert',
    );
    return Array.from(
      requireModule().masteringInsertParamNames(insert),
    ) as (keyof MasteringProcessorParams<N> & string)[];
  });
}

/**
 * Returns every parameter an insert / FX processor's construction reads,
 * including construction-time-only keys with no realtime automation target.
 * Entries come in two runs: first the processor's realtime automation
 * targets in id order (the keys accepted by
 * {@link RealtimeEngine.setTrackStripInsertParamByName}, `id` non-null); then,
 * sorted by name, every other construction key with `id` null and `rtSafe`
 * false. The name set matches {@link masteringInsertParamNames} plus any
 * automation target construction does not read. Returns an empty array for an
 * unknown name.
 *
 * With `sampleRate`, a key whose `maxRelativeTo` is `"nyquist"` reports, as
 * `max` and `maxExclusive`, the bound accepted when the insert is built and
 * prepared at that rate (an EQ band frequency reaches 48000 for a 96000 Hz
 * host). Without it the rate-less answer is returned, with the 24000 cap of the
 * 48 kHz probe.
 *
 * @throws {TypeError} when the request or `insert` is the wrong type
 * @throws {RangeError} for a `sampleRate` outside the supported range
 */
export function masteringInsertParamInfo(
  request: MasteringInsertParamInfoRequest,
): MasteringInsertParamInfo[] {
  return withEntry('masteringInsertParamInfo', () => {
    const r = requestOnly<MasteringInsertParamInfoRequest>('masteringInsertParamInfo', [request]);
    assertString(r.insert, 'insert');
    const module = requireModule();
    if (r.sampleRate !== undefined && r.sampleRate !== null) {
      assertSampleRate(r.sampleRate);
    }
    const json = module.masteringInsertParamInfo(r.insert, r.sampleRate ?? undefined);
    return parseRecordJson(json) as MasteringInsertParamInfo[];
  });
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
  return withEntry('masteringAmpPresetCatalog', () => {
    const json = requireModule().masteringAmpPresetCatalog();
    return parseRecordJson(json) as MasteringAmpPresetCatalogEntry[];
  });
}

/**
 * Latency and tail of insert `insert` built from `params` and prepared at
 * `sampleRate` (`mastering::api::insert_timing`). Answers for the exact
 * instance a scene or strip would build — an oversampled saturation path, a
 * linear-phase crossover, a lookahead all change the reported latency. The
 * capability catalog's `latencySamples` / `tailSamples` are this query at
 * default parameters and 48 kHz. `effects.reverb.convolution` answers for its
 * configuration without an impulse response: its latency is its fixed
 * partition size and does not depend on one.
 *
 * A key the insert's construction does not read is refused rather than ignored,
 * because an ignored key would answer for a configuration the caller did not
 * ask for.
 *
 * @throws {TypeError} for a missing `sampleRate`, or a value of the wrong type or an unknown key
 * @throws {RangeError} for an unknown insert, a non-finite number or a value the insert refuses
 */
export function masteringInsertTiming<N extends MasteringInsertName>(
  request: MasteringInsertTimingRequest<N>,
): MasteringInsertTiming {
  return withEntry('masteringInsertTiming', () => {
    const r = requestOnly<MasteringInsertTimingRequest<N>>('masteringInsertTiming', [request]);
    assertString(r.insert, 'insert');
    assertSampleRate(r.sampleRate);
    const json = JSON.stringify(
      withEntry('masteringInsertTiming', () => resolveInsertParams(r.insert, r.params ?? {})),
    );
    return requireModule().masteringInsertTiming(r.insert, json, r.sampleRate);
  });
}

/**
 * Returns the machine-readable classification catalog for every named processor
 * id, merging the offline registry, the realtime insert factory, and the pair
 * registry. Lets a host filter a processor picker by realtime insertability
 * instead of offering ids the realtime strip would reject.
 */
export function masteringProcessorCatalog(): MasteringProcessorCatalogEntry[] {
  return withEntry('masteringProcessorCatalog', () => {
    const json = requireModule().masteringProcessorCatalog();
    return parseRecordJson(json) as MasteringProcessorCatalogEntry[];
  });
}

/** Names of the pair processors `masteringApplyPairProcessor` accepts. */
export function masteringPairProcessorNames(): PairProcessor[] {
  return withEntry('masteringPairProcessorNames', () => {
    return Array.from(requireModule().masteringPairProcessorNames()) as PairProcessor[];
  });
}

/** Names of the pair analyses `masteringAnalyzePair` accepts. */
export function masteringPairAnalysisNames(): PairAnalysis[] {
  return withEntry('masteringPairAnalysisNames', () => {
    return Array.from(requireModule().masteringPairAnalysisNames()) as PairAnalysis[];
  });
}

/** Names of the stereo analyses `masteringAnalyzeStereo` accepts. */
export function masteringStereoAnalysisNames(): StereoAnalysis[] {
  return withEntry('masteringStereoAnalysisNames', () => {
    return Array.from(requireModule().masteringStereoAnalysisNames()) as StereoAnalysis[];
  });
}

/**
 * Apply one named processor to mono audio.
 *
 * @param request - Mono `samples`, `sampleRate` in Hz, an integer in `[8000, 384000]`, `processor`
 *   (required) and `params`; or pass `(samples, sampleRate, options)`.
 * @throws {TypeError} when `sampleRate` or `processor` is missing, a field has the wrong type or a
 *   parameter key is one the processor does not read
 * @throws {RangeError} on audio out of domain, an unknown processor or a parameter value it refuses
 */
export function masteringApplyProcessor<P extends SoloProcessor>(
  request: MasteringApplyProcessorRequest<P>,
): MasteringResult;
export function masteringApplyProcessor<P extends SoloProcessor>(
  samples: Float32Array,
  sampleRate: number,
  options: OptionsOf<MasteringApplyProcessorRequest<P>>,
): MasteringResult;
export function masteringApplyProcessor(...args: unknown[]): MasteringResult {
  return withEntry('masteringApplyProcessor', () => {
    const r = sampleOneShot<MasteringApplyProcessorRequest>('masteringApplyProcessor', args);
    assertString(r.processor, 'processor');
    assertAudioInput(r.samples, r.sampleRate, r);
    const result = requireModule().masteringProcess(
      r.processor,
      r.samples,
      r.sampleRate,
      withEntry('masteringApplyProcessor', () =>
        resolveProcessorParams(r.processor, r.params ?? {}),
      ),
    );
    return result as MasteringResult;
  });
}

/**
 * Stereo counterpart of {@link masteringApplyProcessor}; takes a request object only.
 *
 * @throws {TypeError} when `sampleRate` or `processor` is missing, a field has the wrong type or a
 *   parameter key is one the processor does not read
 * @throws {RangeError} on audio out of domain, unequal channel lengths, an unknown processor
 *   or a parameter it refuses
 */
export function masteringApplyProcessorStereo<P extends SoloProcessor>(
  request: MasteringApplyProcessorStereoRequest<P>,
): MasteringStereoResult {
  return withEntry('masteringApplyProcessorStereo', () => {
    const r = requestOnly<MasteringApplyProcessorStereoRequest>('masteringApplyProcessorStereo', [
      request,
    ]);
    assertString(r.processor, 'processor');
    assertAudioInput(r.left, r.sampleRate, r, 'left');
    assertAudioInput(r.right, r.sampleRate, r, 'right');
    if (r.left.length !== r.right.length) {
      throw refuse('left and right must have the same length');
    }
    return requireModule().masteringProcessStereo(
      r.processor,
      r.left,
      r.right,
      r.sampleRate,
      withEntry('masteringApplyProcessorStereo', () =>
        resolveProcessorParams(r.processor, r.params ?? {}),
      ),
    );
  });
}

/**
 * Validates the two-input audio of a pair request and returns the reference rate.
 * `source` and `reference` may have independent lengths and rates.
 */
function assertPairAudio(
  r: {
    sampleRate: number;
    referenceSampleRate?: number;
  },
  source: [string, unknown],
  reference: [string, unknown],
): number {
  assertAudioInput(source[1], r.sampleRate, r, source[0]);
  const referenceRate = r.referenceSampleRate ?? r.sampleRate;
  assertSampleRate(referenceRate, 'referenceSampleRate');
  assertAudioInput(reference[1], referenceRate, r, reference[0]);
  return referenceRate;
}

/**
 * Apply a two-input `match.*` processor. `source` and `reference` may have
 * independent lengths and rates: a reference at `referenceSampleRate` is
 * resampled onto `sampleRate` first. Takes a request object only.
 *
 * @throws {TypeError} when `sampleRate` or `processor` is missing, a field has the wrong type or a
 *   parameter key is one the processor does not read
 * @throws {RangeError} on audio out of domain, an unknown processor or a parameter value it refuses
 */
export function masteringApplyPairProcessor<P extends PairProcessor>(
  request: MasteringApplyPairProcessorRequest<P>,
): MasteringResult {
  return withEntry('masteringApplyPairProcessor', () => {
    const r = requestOnly<MasteringApplyPairProcessorRequest>('masteringApplyPairProcessor', [
      request,
    ]);
    assertString(r.processor, 'processor');
    const referenceRate = assertPairAudio(r, ['source', r.source], ['reference', r.reference]);
    const result = requireModule().masteringPairProcess(
      r.processor,
      r.source,
      r.reference,
      r.sampleRate,
      referenceRate,
      r.params ?? {},
    );
    return result as MasteringResult;
  });
}

/**
 * Apply the stereo `match.abCrossfade` processor. Source and reference stereo
 * pairs may have independent lengths, but each pair must have equal channels.
 *
 * @throws {TypeError} when `sampleRate` or `processor` is missing, a field has the wrong type or a
 *   parameter key is one the processor does not read
 * @throws {RangeError} on audio out of domain or a pair whose channels differ in length
 */
export function masteringApplyPairProcessorStereo(
  request: MasteringApplyPairProcessorStereoRequest,
): MasteringStereoResult {
  return withEntry('masteringApplyPairProcessorStereo', () => {
    const r = requestOnly<MasteringApplyPairProcessorStereoRequest>(
      'masteringApplyPairProcessorStereo',
      [request],
    );
    assertString(r.processor, 'processor');
    assertAudioInput(r.sourceLeft, r.sampleRate, r, 'sourceLeft');
    assertAudioInput(r.sourceRight, r.sampleRate, r, 'sourceRight');
    const referenceRate = r.referenceSampleRate ?? r.sampleRate;
    assertSampleRate(referenceRate, 'referenceSampleRate');
    assertAudioInput(r.referenceLeft, referenceRate, r, 'referenceLeft');
    assertAudioInput(r.referenceRight, referenceRate, r, 'referenceRight');
    if (r.sourceLeft.length !== r.sourceRight.length) {
      throw refuse('sourceLeft and sourceRight must have the same length');
    }
    if (r.referenceLeft.length !== r.referenceRight.length) {
      throw refuse('referenceLeft and referenceRight must have the same length');
    }
    return requireModule().masteringPairProcessStereo(
      r.processor,
      r.sourceLeft,
      r.sourceRight,
      r.referenceLeft,
      r.referenceRight,
      r.sampleRate,
      referenceRate,
      r.params ?? {},
    );
  });
}

/**
 * Analyze a `source` against a `reference` with a two-input analysis. The two
 * buffers may have independent lengths and rates; a delay estimate is in
 * samples at `sampleRate`. Takes a request object only; the result type follows
 * `analysis` through {@link PairAnalysisResultMap}.
 *
 * @throws {TypeError} when `sampleRate` or `analysis` is missing or a field has the wrong type
 * @throws {RangeError} on audio out of domain or an unknown analysis
 */
export function masteringAnalyzePair<A extends PairAnalysis>(
  request: MasteringAnalyzePairRequest<A>,
): PairAnalysisResultMap[A] {
  return withEntry('masteringAnalyzePair', () => {
    const r = requestOnly<MasteringAnalyzePairRequest>('masteringAnalyzePair', [request]);
    assertString(r.analysis, 'analysis');
    const referenceRate = assertPairAudio(r, ['source', r.source], ['reference', r.reference]);
    return parseRecordJson(
      requireModule().masteringPairAnalyze(
        r.analysis,
        r.source,
        r.reference,
        r.sampleRate,
        referenceRate,
        r.params ?? {},
      ),
    ) as PairAnalysisResultMap[A];
  });
}

/**
 * Analyze a stereo pair. Takes a request object only; the result type follows
 * `analysis` through {@link StereoAnalysisResultMap}.
 *
 * @throws {TypeError} when `sampleRate` or `analysis` is missing or a field has the wrong type
 * @throws {RangeError} on audio out of domain or an unknown analysis
 */
export function masteringAnalyzeStereo<A extends StereoAnalysis>(
  request: MasteringAnalyzeStereoRequest<A>,
): StereoAnalysisResultMap[A] {
  return withEntry('masteringAnalyzeStereo', () => {
    const r = requestOnly<MasteringAnalyzeStereoRequest>('masteringAnalyzeStereo', [request]);
    assertString(r.analysis, 'analysis');
    assertAudioInput(r.left, r.sampleRate, r, 'left');
    assertAudioInput(r.right, r.sampleRate, r, 'right');
    return parseRecordJson(
      requireModule().masteringStereoAnalyze(
        r.analysis,
        r.left,
        r.right,
        r.sampleRate,
        r.params ?? {},
      ),
    ) as StereoAnalysisResultMap[A];
  });
}

/**
 * Gain-match `source` to `reference`'s integrated loudness, so an A/B between
 * the two is not decided by level. `source` and `reference` may have
 * independent lengths and rates; a reference at `referenceSampleRate` is
 * resampled onto `sampleRate` before it is measured.
 *
 * The gain is applied with no upper bound and `matchedTruePeakDbtp` reports
 * where that left the peak, rather than the call capping it: a headroom clamp
 * would return `source` at its own loudness whenever it started near full
 * scale. Both loudness values are non-finite for a silent or below-gate take,
 * and `appliedGainDb` is then 0.
 *
 * @throws {TypeError} when `sampleRate` is missing or a field has the wrong type
 * @throws {RangeError} on audio out of domain
 *
 * @example
 * ```ts
 * declare const take: Float32Array, master: Float32Array; // mono, 48 kHz
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
  return withEntry('masteringAbMatchLoudness', () => {
    const r = requestOnly<MasteringAbMatchLoudnessRequest>('masteringAbMatchLoudness', [request]);
    const referenceRate = assertPairAudio(r, ['source', r.source], ['reference', r.reference]);
    return requireModule().masteringAbMatchLoudness(
      r.source,
      r.reference,
      r.sampleRate,
      referenceRate,
    );
  });
}

/** Gain-match a stereo source to a stereo reference with one shared gain. */
export function masteringAbMatchLoudnessStereo(
  request: MasteringAbMatchLoudnessStereoRequest,
): LoudnessMatchStereoResult {
  return withEntry('masteringAbMatchLoudnessStereo', () => {
    const r = requestOnly<MasteringAbMatchLoudnessStereoRequest>('masteringAbMatchLoudnessStereo', [
      request,
    ]);
    assertAudioInput(r.sourceLeft, r.sampleRate, r, 'sourceLeft');
    assertAudioInput(r.sourceRight, r.sampleRate, r, 'sourceRight');
    const referenceRate = r.referenceSampleRate ?? r.sampleRate;
    assertSampleRate(referenceRate, 'referenceSampleRate');
    assertAudioInput(r.referenceLeft, referenceRate, r, 'referenceLeft');
    assertAudioInput(r.referenceRight, referenceRate, r, 'referenceRight');
    if (r.sourceLeft.length !== r.sourceRight.length) {
      throw refuse('sourceLeft and sourceRight must have the same length');
    }
    if (r.referenceLeft.length !== r.referenceRight.length) {
      throw refuse('referenceLeft and referenceRight must have the same length');
    }
    return requireModule().masteringAbMatchLoudnessStereo(
      r.sourceLeft,
      r.sourceRight,
      r.referenceLeft,
      r.referenceRight,
      r.sampleRate,
      referenceRate,
    );
  });
}

function assistantResultFromNative(json: string): MasteringAssistantResult {
  const native = parseRecordJson(json) as {
    chainConfig: { params: Record<string, unknown> };
    explanation: string[];
    profile: MasteringAssistantResult['profile'];
  };
  return {
    chainConfig: chainConfigFromDocument(native.chainConfig),
    explanation: native.explanation,
    profile: native.profile,
  };
}

function previewFromNative(json: string): MasteringStreamingPreviewResult {
  return parseRecordJson(json) as MasteringStreamingPreviewResult;
}

// `defects` is absent when the detectors did not run.
function profileFromNative(json: string): MasteringAudioProfile {
  const profile = parseRecordJson(json) as MasteringAudioProfile;
  if (profile.defects !== undefined) {
    profile.defects = audioDefectsFromNative(profile.defects);
  }
  return profile;
}

/** Native params of an audio-profile request; the profile always runs the defect detectors unless told not to. */
function profileParams(
  r: Pick<
    MasteringAudioProfileRequest,
    'nFft' | 'hopLength' | 'truePeakOversample' | 'detectDefects'
  >,
): Record<string, number | boolean> {
  const params: Record<string, number | boolean> = {};
  for (const key of ['nFft', 'hopLength', 'truePeakOversample'] as const) {
    const value = r[key];
    if (value !== undefined && value !== null) {
      params[key] = value;
    }
  }
  params.detectDefects = optionalBoolean(r.detectDefects, 'detectDefects', true);
  return params;
}

/** The presets the assistant starts from: every mastering preset, no restoration one. */
function assistantPresetNames(): MasteringPreset[] {
  const catalog = parseRecordJson(getSonareModule().capabilityCatalog()) as CapabilityCatalog;
  return catalog.masteringPresets
    .filter((entry) => entry.kind === 'mastering')
    .map((entry) => entry.name as MasteringPreset);
}

/** Native params of an assistant request, with the two name-valued fields checked against the library's tables. */
function assistantParams(r: MasteringAssistantSettings): Record<string, number | boolean | string> {
  const params: Record<string, number | boolean | string> = {};
  for (const key of ['targetLufs', 'ceilingDbtp'] as const) {
    if (r[key] !== undefined && r[key] !== null) {
      assertFloatValue(r[key], key);
      params[key] = r[key];
    }
  }
  if (r.speechMonoAmount !== undefined && r.speechMonoAmount !== null) {
    assertFloatValue(r.speechMonoAmount, 'speechMonoAmount');
    params.speechMonoAmount = r.speechMonoAmount;
  }
  for (const key of ['enableRepair', 'preferStreamingSafe'] as const) {
    if (r[key] !== undefined && r[key] !== null) {
      params[key] = optionalBoolean(r[key], key, false);
    }
  }
  if (r.targetPlatform !== undefined && r.targetPlatform !== null) {
    assertClosedToken(r.targetPlatform, masteringPlatformNames(), 'targetPlatform');
    params.targetPlatform = r.targetPlatform;
  }
  if (r.preset !== undefined && r.preset !== null) {
    assertClosedToken(r.preset, assistantPresetNames(), 'preset');
    params.preset = r.preset;
  }
  return params;
}

/**
 * Suggest a mastering chain for mono audio.
 *
 * @param request - Mono `samples`, `sampleRate` in Hz, an integer in `[8000, 384000]`, and the
 *   assistant settings; or pass `(samples, sampleRate, options?)`.
 * @returns The suggested chain, one explanation line per decision and the measured profile
 * @throws {TypeError} when `sampleRate` is missing or a field has the wrong type
 * @throws {RangeError} on audio out of domain, or an unknown `targetPlatform` or `preset`
 */
export function masteringAssistantSuggest(
  request: MasteringAssistantSuggestRequest,
): MasteringAssistantResult;
export function masteringAssistantSuggest(
  samples: Float32Array,
  sampleRate: number,
  options?: OptionsOf<MasteringAssistantSuggestRequest>,
): MasteringAssistantResult;
export function masteringAssistantSuggest(...args: unknown[]): MasteringAssistantResult {
  return withEntry('masteringAssistantSuggest', () => {
    const r = sampleOneShot<MasteringAssistantSuggestRequest>('masteringAssistantSuggest', args);
    assertAudioInput(r.samples, r.sampleRate, r);
    return assistantResultFromNative(
      requireModule().masteringAssistantSuggest(
        r.samples,
        r.sampleRate,
        withEntry('masteringAssistantSuggest', () => assistantParams(r)),
      ),
    );
  });
}

/**
 * Measure the mastering profile of mono audio: loudness, spectral and dynamics
 * groups, and — unless `detectDefects` is false — the repair detectors' readings
 * under `defects`.
 *
 * @throws {TypeError} when `sampleRate` is missing or a field has the wrong type
 * @throws {RangeError} on audio out of domain or a non-positive `nFft`, `hopLength` or
 *   `truePeakOversample`
 */
export function masteringAudioProfile(request: MasteringAudioProfileRequest): MasteringAudioProfile;
export function masteringAudioProfile(
  samples: Float32Array,
  sampleRate: number,
  options?: OptionsOf<MasteringAudioProfileRequest>,
): MasteringAudioProfile;
export function masteringAudioProfile(...args: unknown[]): MasteringAudioProfile {
  return withEntry('masteringAudioProfile', () => {
    const r = sampleOneShot<MasteringAudioProfileRequest>('masteringAudioProfile', args);
    assertAudioInput(r.samples, r.sampleRate, r);
    return profileFromNative(
      requireModule().masteringAudioProfile(r.samples, r.sampleRate, profileParams(r)),
    );
  });
}

/**
 * Preview streaming-platform normalization of mono audio.
 *
 * @throws {TypeError} when `sampleRate` is missing or a field has the wrong type
 * @throws {RangeError} on audio out of domain
 */
export function masteringStreamingPreview(
  request: MasteringStreamingPreviewRequest,
): MasteringStreamingPreviewResult;
export function masteringStreamingPreview(
  samples: Float32Array,
  sampleRate: number,
  options?: OptionsOf<MasteringStreamingPreviewRequest>,
): MasteringStreamingPreviewResult;
export function masteringStreamingPreview(...args: unknown[]): MasteringStreamingPreviewResult {
  return withEntry('masteringStreamingPreview', () => {
    const r = sampleOneShot<MasteringStreamingPreviewRequest>('masteringStreamingPreview', args);
    assertAudioInput(r.samples, r.sampleRate, r);
    return previewFromNative(
      requireModule().masteringStreamingPreview(
        r.samples,
        r.sampleRate,
        previewPlatforms(r.platforms),
      ),
    );
  });
}

/**
 * Suggest a mastering chain for a stereo pair. Takes a request object only.
 *
 * Profiles through {@link masteringAudioProfileStereo}, so the loudness stage
 * of the suggestion is built on the channel-summed program rather than a
 * downmix that reads roughly 6 dB low.
 *
 * @throws {TypeError} when `sampleRate` is missing or a field has the wrong type
 * @throws {RangeError} on audio out of domain, or an unknown `targetPlatform` or `preset`
 */
export function masteringAssistantSuggestStereo(
  request: MasteringAssistantSuggestStereoRequest,
): MasteringAssistantResult {
  return withEntry('masteringAssistantSuggestStereo', () => {
    const r = requestOnly<MasteringAssistantSuggestStereoRequest>(
      'masteringAssistantSuggestStereo',
      [request],
    );
    assertAudioInput(r.left, r.sampleRate, r, 'left');
    assertAudioInput(r.right, r.sampleRate, r, 'right');
    return assistantResultFromNative(
      requireModule().masteringAssistantSuggestStereo(
        r.left,
        r.right,
        r.sampleRate,
        withEntry('masteringAssistantSuggestStereo', () => assistantParams(r)),
      ),
    );
  });
}

/**
 * Mastering profile of a stereo pair.
 *
 * The `loudness` group is measured from the two channels: integrated LUFS and
 * loudness range come from the channel-summed program and the true peak is the
 * larger of the two. The spectral, dynamics and tempo fields describe shape and
 * timing rather than absolute level and are measured on the downmix, which keeps
 * them comparable with {@link masteringAudioProfile}. Defect detectors run on
 * each channel and their results are aggregated.
 */
export function masteringAudioProfileStereo(
  request: MasteringAudioProfileStereoRequest,
): MasteringAudioProfile {
  return withEntry('masteringAudioProfileStereo', () => {
    const r = requestOnly<MasteringAudioProfileStereoRequest>('masteringAudioProfileStereo', [
      request,
    ]);
    assertAudioInput(r.left, r.sampleRate, r, 'left');
    assertAudioInput(r.right, r.sampleRate, r, 'right');
    return profileFromNative(
      requireModule().masteringAudioProfileStereo(r.left, r.right, r.sampleRate, profileParams(r)),
    );
  });
}

/**
 * Preview streaming-platform normalization for a stereo pair.
 *
 * Measures the integrated loudness with BS.1770 channel summing and reports the
 * larger of the two channel true peaks. Passing a `0.5 * (left + right)` downmix
 * to {@link masteringStreamingPreview} instead reads roughly 6 dB low on
 * decorrelated material, and both the normalization gain and the ceiling-risk
 * flag follow from that measurement.
 */
export function masteringStreamingPreviewStereo(
  request: MasteringStreamingPreviewStereoRequest,
): MasteringStreamingPreviewResult {
  return withEntry('masteringStreamingPreviewStereo', () => {
    const r = requestOnly<MasteringStreamingPreviewStereoRequest>(
      'masteringStreamingPreviewStereo',
      [request],
    );
    assertAudioInput(r.left, r.sampleRate, r, 'left');
    assertAudioInput(r.right, r.sampleRate, r, 'right');
    return previewFromNative(
      requireModule().masteringStreamingPreviewStereo(
        r.left,
        r.right,
        r.sampleRate,
        previewPlatforms(r.platforms),
      ),
    );
  });
}
