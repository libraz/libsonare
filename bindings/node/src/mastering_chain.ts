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
  StreamingPlatform,
} from './types.js';
import { assertSampleRate } from './validation.js';

export type NormalizeMode = 'peak' | 'rms';

export interface NormalizeRequest {
  samples: Float32Array;
  sampleRate?: number;
  /** Finite target level at or below 0 dBFS. Default 0. */
  targetDb?: number;
  /** Normalization statistic. Defaults to peak normalization. */
  mode?: NormalizeMode;
}

export function normalize(request: NormalizeRequest): Float32Array;
export function normalize(
  samples: Float32Array,
  sampleRate?: number,
  targetDb?: number,
  mode?: NormalizeMode,
): Float32Array;
export function normalize(
  samples: Float32Array | NormalizeRequest,
  sampleRate = 22050,
  targetDb = 0.0,
  mode?: NormalizeMode,
): Float32Array {
  const request =
    samples instanceof Float32Array ? { samples, sampleRate, targetDb, mode } : samples;
  const resolvedMode = resolveNormalizeMode(request.mode);
  const resolvedSampleRate = request.sampleRate ?? 22050;
  assertSampleRate('normalize', resolvedSampleRate);
  return addon.normalize(
    request.samples,
    resolvedSampleRate,
    request.targetDb ?? 0.0,
    resolvedMode,
  );
}

function resolveNormalizeMode(value: unknown): NormalizeMode {
  if (value === undefined) {
    return 'peak';
  }
  if (typeof value !== 'string') {
    throw new TypeError("normalize: mode must be 'peak' or 'rms'");
  }
  if (value !== 'peak' && value !== 'rms') {
    throw new RangeError("normalize: mode must be 'peak' or 'rms'");
  }
  return value;
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

/**
 * Params accepted by the assistant entry points. Every key is numeric except
 * `targetPlatform`, which is a delivery-target NAME (`'broadcast'`, `'podcast'`,
 * `'club'`, ...). A number is rejected for it: the numeric index the C ABI
 * carries is a transport detail for callers that cannot pass a string, not part
 * of the JavaScript vocabulary.
 */
export type MasteringAssistantParams = Record<string, number | boolean | string>;

export interface MasteringAssistantSuggestRequest {
  samples: Float32Array;
  sampleRate?: number;
  params?: MasteringAssistantParams;
}

/** One entry of {@link MasteringAudioProfile.genreCandidates}. */
export interface MasteringGenreCandidate {
  name: string;
  score: number;
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
  defects: {
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
  };
  genreCandidates: MasteringGenreCandidate[];
}

/** The profile entry points take numeric params only; they have no target platform. */
export interface MasteringAudioProfileRequest {
  samples: Float32Array;
  sampleRate?: number;
  params?: Record<string, number | boolean>;
}

export interface MasteringStreamingPreviewRequest {
  samples: Float32Array;
  sampleRate?: number;
  platforms?: StreamingPlatform[];
}

/** Request for {@link masteringAssistantSuggestStereo}. */
export interface MasteringAssistantSuggestStereoRequest {
  left: Float32Array;
  right: Float32Array;
  sampleRate?: number;
  params?: MasteringAssistantParams;
}

/** Request for {@link masteringAudioProfileStereo}. */
export interface MasteringAudioProfileStereoRequest {
  left: Float32Array;
  right: Float32Array;
  sampleRate?: number;
  params?: Record<string, number | boolean>;
}

/** Request for {@link masteringStreamingPreviewStereo}. */
export interface MasteringStreamingPreviewStereoRequest {
  left: Float32Array;
  right: Float32Array;
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

export function masteringPresetNames(): MasteringPreset[] {
  return addon.masteringPresetNames();
}

/**
 * Delivery targets the mastering assistant accepts as `targetPlatform`.
 *
 * Read from the library rather than from a list kept here, so a target added in
 * the core is discoverable without a binding change.
 */
export function masteringPlatformNames(): string[] {
  return addon.masteringPlatformNames();
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
  /** The C++ type the processor's config builder reads the key as. */
  type: 'boolean' | 'number';
  /**
   * Smallest value construction accepts, or null when the catalog states no
   * limit. Measured, so it is a hard constraint rather than a UI range; see
   * {@link CapabilityCatalogParameter} for what a measured bound does and does
   * not promise.
   */
  min: number | null;
  /** Largest value construction accepts, or null when the catalog states no limit. */
  max: number | null;
  /**
   * Value the processor uses when the key is absent — the config struct's own
   * field initializer. Null only for a param id with no construction key.
   */
  default: boolean | number | null;
  /** Physical unit, or null when the parameter is unitless. */
  unit: string | null;
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

/**
 * Catalog grouping for a processor picker, derived from the id's prefix
 * (`eq.*` -> `eq`, `match.*` -> `reference`); anything unprefixed is `other`.
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
  | 'stereo';

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
  /** Grouping for a processor picker; see {@link MasteringProcessorCategory}. */
  category: MasteringProcessorCategory;
  /**
   * The processor's automatable parameters, the same list
   * {@link masteringInsertParamInfo} returns. Empty for entries that are not
   * realtime-insertable.
   */
  params: MasteringInsertParamInfo[];
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

export function masteringAssistantSuggest(request: MasteringAssistantSuggestRequest): string;
export function masteringAssistantSuggest(
  samples: Float32Array,
  sampleRate?: number,
  params?: MasteringAssistantParams,
): string;
export function masteringAssistantSuggest(
  samples: Float32Array | MasteringAssistantSuggestRequest,
  sampleRate = 22050,
  params: MasteringAssistantParams = {},
): string {
  const request = samples instanceof Float32Array ? { samples, sampleRate, params } : samples;
  const resolvedSampleRate = request.sampleRate ?? 22050;
  assertSampleRate('masteringAssistantSuggest', resolvedSampleRate);
  return addon.masteringAssistantSuggest(request.samples, resolvedSampleRate, request.params ?? {});
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
  const resolvedSampleRate = request.sampleRate ?? 22050;
  assertSampleRate('masteringAudioProfile', resolvedSampleRate);
  return addon.masteringAudioProfile(request.samples, resolvedSampleRate, request.params ?? {});
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
  const resolvedSampleRate = request.sampleRate ?? 22050;
  assertSampleRate('masteringStreamingPreview', resolvedSampleRate);
  return addon.masteringStreamingPreview(
    request.samples,
    resolvedSampleRate,
    request.platforms ?? [],
  );
}

/**
 * Suggest a mastering chain for a stereo pair, as shared JSON.
 *
 * Profiles through {@link masteringAudioProfileStereo}, so the loudness stage
 * of the suggestion is built on the channel-summed program rather than a
 * downmix that reads roughly 6 dB low.
 */
export function masteringAssistantSuggestStereo(
  request: MasteringAssistantSuggestStereoRequest,
): string {
  const resolvedSampleRate = request.sampleRate ?? 22050;
  assertSampleRate('masteringAssistantSuggestStereo', resolvedSampleRate);
  return addon.masteringAssistantSuggestStereo(
    request.left,
    request.right,
    resolvedSampleRate,
    request.params ?? {},
  );
}

/**
 * Mastering assistant profile of a stereo pair, as shared JSON.
 *
 * Only the `loudness` block is measured from the two channels: integrated LUFS
 * and LRA come from the channel-summed program and the true peak is the larger
 * of the two. The spectral, dynamics and tempo fields describe shape and timing
 * rather than absolute level and are measured on the downmix, which keeps them
 * comparable with {@link masteringAudioProfile}.
 */
export function masteringAudioProfileStereo(request: MasteringAudioProfileStereoRequest): string {
  const resolvedSampleRate = request.sampleRate ?? 22050;
  assertSampleRate('masteringAudioProfileStereo', resolvedSampleRate);
  return addon.masteringAudioProfileStereo(
    request.left,
    request.right,
    resolvedSampleRate,
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
): string {
  const resolvedSampleRate = request.sampleRate ?? 22050;
  assertSampleRate('masteringStreamingPreviewStereo', resolvedSampleRate);
  return addon.masteringStreamingPreviewStereo(
    request.left,
    request.right,
    resolvedSampleRate,
    request.platforms ?? [],
  );
}
