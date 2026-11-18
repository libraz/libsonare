import { flattenChainConfig } from './internal/chain_config.js';
import { resolveProcessorParams } from './internal/processor_params.js';
import { addon } from './native.js';
import type {
  LoudnessMatchResult,
  LoudnessMatchStereoResult,
  MasteringChainConfig,
  MasteringChainResult,
  MasteringChainStereoResult,
  MasteringNormalizeLoudnessOptions,
  MasteringNormalizeLoudnessResult,
  MasteringPairParams,
  MasteringPreset,
  MasteringProcessorParams,
  MasteringResult,
  MasteringStereoResult,
  PairAnalysis,
  PairAnalysisResultMap,
  PairProcessor,
  ProgressCallback,
  SoloProcessor,
  StereoAnalysis,
  StereoAnalysisResultMap,
  StereoPairProcessor,
  StreamingLoudnessGainResult,
} from './types.js';
import type { OptionsOf, ValidateOptions } from './validation.js';
import {
  assertAudioInput,
  assertFloatValue,
  assertPositiveFloatValue,
  assertPositiveInteger,
  assertSampleRate,
  requestOnly,
  sampleOneShot,
} from './validation.js';

export * from './mastering_assistant.js';
export * from './mastering_catalog.js';
export * from './mastering_normalize.js';

/** Request form of {@link masteringNormalizeLoudness}. */
export interface MasteringNormalizeLoudnessRequest extends MasteringNormalizeLoudnessOptions {
  /** Mono input samples. */
  samples: Float32Array;
  /** Sample rate in Hz, an integer in `[8000, 384000]`; there is no default. */
  sampleRate: number;
}

/** Request form of {@link masteringApplyProcessor}. */
export interface MasteringApplyProcessorRequest<P extends SoloProcessor = SoloProcessor>
  extends ValidateOptions {
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
export interface MasteringApplyProcessorStereoRequest<P extends SoloProcessor = SoloProcessor>
  extends ValidateOptions {
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
export interface MasteringApplyPairProcessorRequest<P extends PairProcessor = PairProcessor>
  extends ValidateOptions {
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
export interface MasteringApplyPairProcessorStereoRequest extends ValidateOptions {
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
export interface MasteringAbMatchLoudnessRequest extends ValidateOptions {
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
export interface MasteringAbMatchLoudnessStereoRequest extends ValidateOptions {
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
export interface MasteringAnalyzePairRequest<A extends PairAnalysis = PairAnalysis>
  extends ValidateOptions {
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
export interface MasteringAnalyzeStereoRequest<A extends StereoAnalysis = StereoAnalysis>
  extends ValidateOptions {
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

// `releaseMs` and `truePeakOversample` read 0 as "library default" natively; the facade omits instead.
function nativeLoudnessArgs(fnName: string, r: MasteringNormalizeLoudnessRequest) {
  if (r.targetLufs !== undefined && r.targetLufs !== null) {
    assertFloatValue(fnName, r.targetLufs, 'targetLufs');
  }
  if (r.ceilingDb !== undefined && r.ceilingDb !== null) {
    assertFloatValue(fnName, r.ceilingDb, 'ceilingDb');
  }
  if (r.truePeakOversample !== undefined && r.truePeakOversample !== null) {
    assertPositiveInteger(fnName, r.truePeakOversample, 'truePeakOversample');
  }
  if (r.releaseMs !== undefined && r.releaseMs !== null) {
    assertPositiveFloatValue(fnName, r.releaseMs, 'releaseMs');
  }
  return [
    r.targetLufs ?? -14.0,
    r.ceilingDb ?? -1.0,
    r.truePeakOversample ?? 4,
    r.releaseMs ?? 0,
    r.applyGainAtInputRate ?? false,
  ] as const;
}

/**
 * Normalize loudness to a target and hold a true-peak ceiling.
 *
 * @param samples - Audio samples (mono, float32)
 * @param sampleRate - Sample rate in Hz, an integer in `[8000, 384000]`
 * @param options - Loudness/ceiling settings ({@link MasteringNormalizeLoudnessOptions})
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
  const r = sampleOneShot<MasteringNormalizeLoudnessRequest>('masteringNormalizeLoudness', args);
  assertAudioInput('masteringNormalizeLoudness', r.samples, r.sampleRate, r);
  const result = addon.mastering(
    r.samples,
    r.sampleRate,
    ...nativeLoudnessArgs('masteringNormalizeLoudness', r),
  );
  return {
    ...result,
    latencySamples: result.latencySamples ?? 0,
  } as MasteringNormalizeLoudnessResult;
}

/**
 * Apply one named processor to mono audio.
 *
 * @param samples - Audio samples (mono, float32)
 * @param sampleRate - Sample rate in Hz, an integer in `[8000, 384000]`
 * @param options - `processor` (required), `params`, `validate`
 * @throws {TypeError} when `sampleRate` or `processor` is missing or a field has the wrong type
 * @throws {RangeError} on audio out of domain, an unknown processor or a parameter it refuses
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
  const r = sampleOneShot<MasteringApplyProcessorRequest>('masteringApplyProcessor', args);
  assertAudioInput('masteringApplyProcessor', r.samples, r.sampleRate, r);
  const result = addon.masteringProcess(
    r.processor,
    r.samples,
    r.sampleRate,
    resolveProcessorParams(r.processor, r.params ?? {}),
  );
  return { ...result, latencySamples: result.latencySamples ?? 0 } as MasteringResult;
}

/**
 * Stereo counterpart of {@link masteringApplyProcessor}; takes a request object only.
 *
 * @throws {TypeError} when `sampleRate` or `processor` is missing or a field has the wrong type
 * @throws {RangeError} on audio out of domain, unequal channel lengths, an unknown processor
 *   or a parameter it refuses
 */
export function masteringApplyProcessorStereo<P extends SoloProcessor>(
  request: MasteringApplyProcessorStereoRequest<P>,
): MasteringStereoResult {
  const r = requestOnly<MasteringApplyProcessorStereoRequest>('masteringApplyProcessorStereo', [
    request,
  ]);
  assertAudioInput('masteringApplyProcessorStereo', r.left, r.sampleRate, r, 'left');
  assertAudioInput('masteringApplyProcessorStereo', r.right, r.sampleRate, r, 'right');
  if (r.left.length !== r.right.length) {
    throw new RangeError('Stereo channel lengths must match.');
  }
  return addon.masteringProcessStereo(
    r.processor,
    r.left,
    r.right,
    r.sampleRate,
    resolveProcessorParams(r.processor, r.params ?? {}),
  );
}

/**
 * Validates the two-input audio of a pair request and returns the reference rate.
 * `source` and `reference` may have independent lengths and rates.
 */
function assertPairAudio(
  fnName: string,
  r: {
    sampleRate: number;
    referenceSampleRate?: number;
    validate?: boolean;
  },
  source: [string, unknown],
  reference: [string, unknown],
): number {
  assertAudioInput(fnName, source[1], r.sampleRate, r, source[0]);
  const referenceRate = r.referenceSampleRate ?? r.sampleRate;
  assertSampleRate(fnName, referenceRate, 'referenceSampleRate');
  assertAudioInput(fnName, reference[1], referenceRate, r, reference[0]);
  return referenceRate;
}

/**
 * Apply a two-input `match.*` processor. `source` and `reference` may have
 * independent lengths and rates: a reference at `referenceSampleRate` is
 * resampled onto `sampleRate` first. Takes a request object only.
 *
 * @throws {TypeError} when `sampleRate` or `processor` is missing or a field has the wrong type
 * @throws {RangeError} on audio out of domain, an unknown processor or a parameter it refuses
 */
export function masteringApplyPairProcessor<P extends PairProcessor>(
  request: MasteringApplyPairProcessorRequest<P>,
): MasteringResult {
  const r = requestOnly<MasteringApplyPairProcessorRequest>('masteringApplyPairProcessor', [
    request,
  ]);
  const fn = 'masteringApplyPairProcessor';
  const referenceRate = assertPairAudio(fn, r, ['source', r.source], ['reference', r.reference]);
  const result = addon.masteringPairProcess(
    r.processor,
    r.source,
    r.reference,
    r.sampleRate,
    referenceRate,
    r.params ?? {},
  );
  return { ...result, latencySamples: result.latencySamples ?? 0 } as MasteringResult;
}

/**
 * Apply the stereo `match.abCrossfade` processor. Source and reference stereo
 * pairs may have independent lengths, but each pair must have equal channels.
 *
 * @throws {TypeError} when `sampleRate` or `processor` is missing or a field has the wrong type
 * @throws {RangeError} on audio out of domain or a pair whose channels differ in length
 */
export function masteringApplyPairProcessorStereo(
  request: MasteringApplyPairProcessorStereoRequest,
): MasteringStereoResult {
  const r = requestOnly<MasteringApplyPairProcessorStereoRequest>(
    'masteringApplyPairProcessorStereo',
    [request],
  );
  const fn = 'masteringApplyPairProcessorStereo';
  assertAudioInput(fn, r.sourceLeft, r.sampleRate, r, 'sourceLeft');
  assertAudioInput(fn, r.sourceRight, r.sampleRate, r, 'sourceRight');
  const referenceRate = r.referenceSampleRate ?? r.sampleRate;
  assertSampleRate(fn, referenceRate, 'referenceSampleRate');
  assertAudioInput(fn, r.referenceLeft, referenceRate, r, 'referenceLeft');
  assertAudioInput(fn, r.referenceRight, referenceRate, r, 'referenceRight');
  if (r.sourceLeft.length !== r.sourceRight.length) {
    throw new RangeError('Source left and right channel lengths must match.');
  }
  if (r.referenceLeft.length !== r.referenceRight.length) {
    throw new RangeError('Reference left and right channel lengths must match.');
  }
  return addon.masteringPairProcessStereo(
    r.processor,
    r.sourceLeft,
    r.sourceRight,
    r.referenceLeft,
    r.referenceRight,
    r.sampleRate,
    referenceRate,
    r.params ?? {},
  );
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
  const r = requestOnly<MasteringAnalyzePairRequest>('masteringAnalyzePair', [request]);
  const fn = 'masteringAnalyzePair';
  const referenceRate = assertPairAudio(fn, r, ['source', r.source], ['reference', r.reference]);
  return JSON.parse(
    addon.masteringPairAnalyze(
      r.analysis,
      r.source,
      r.reference,
      r.sampleRate,
      referenceRate,
      r.params ?? {},
    ),
  ) as PairAnalysisResultMap[A];
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
  const r = requestOnly<MasteringAnalyzeStereoRequest>('masteringAnalyzeStereo', [request]);
  const fn = 'masteringAnalyzeStereo';
  assertAudioInput(fn, r.left, r.sampleRate, r, 'left');
  assertAudioInput(fn, r.right, r.sampleRate, r, 'right');
  return JSON.parse(
    addon.masteringStereoAnalyze(r.analysis, r.left, r.right, r.sampleRate, r.params ?? {}),
  ) as StereoAnalysisResultMap[A];
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
  const r = requestOnly<MasteringAbMatchLoudnessRequest>('masteringAbMatchLoudness', [request]);
  const referenceRate = assertPairAudio(
    'masteringAbMatchLoudness',
    r,
    ['source', r.source],
    ['reference', r.reference],
  );
  return addon.masteringAbMatchLoudness(r.source, r.reference, r.sampleRate, referenceRate);
}

/** Gain-match a stereo source to a stereo reference with one shared gain. */
export function masteringAbMatchLoudnessStereo(
  request: MasteringAbMatchLoudnessStereoRequest,
): LoudnessMatchStereoResult {
  const r = requestOnly<MasteringAbMatchLoudnessStereoRequest>('masteringAbMatchLoudnessStereo', [
    request,
  ]);
  const fn = 'masteringAbMatchLoudnessStereo';
  assertAudioInput(fn, r.sourceLeft, r.sampleRate, r, 'sourceLeft');
  assertAudioInput(fn, r.sourceRight, r.sampleRate, r, 'sourceRight');
  const referenceRate = r.referenceSampleRate ?? r.sampleRate;
  assertSampleRate(fn, referenceRate, 'referenceSampleRate');
  assertAudioInput(fn, r.referenceLeft, referenceRate, r, 'referenceLeft');
  assertAudioInput(fn, r.referenceRight, referenceRate, r, 'referenceRight');
  if (r.sourceLeft.length !== r.sourceRight.length) {
    throw new RangeError('Source left and right channel lengths must match.');
  }
  if (r.referenceLeft.length !== r.referenceRight.length) {
    throw new RangeError('Reference left and right channel lengths must match.');
  }
  return addon.masteringAbMatchLoudnessStereo(
    r.sourceLeft,
    r.sourceRight,
    r.referenceLeft,
    r.referenceRight,
    r.sampleRate,
    referenceRate,
  );
}

type NativeResult = Record<string, unknown>;

// Native still names the loudness range `loudnessRange` / `outputLra`.
function chainResultFromNative<R>(raw: object): R {
  const { outputLra, report, ...rest } = raw as NativeResult & {
    outputLra: number;
    report: { before: NativeResult; after: NativeResult } & NativeResult;
  };
  const summary = (native: NativeResult): NativeResult => {
    const { loudnessRange, ...others } = native as NativeResult & { loudnessRange: number };
    return { ...others, loudnessRangeLu: loudnessRange };
  };
  return {
    ...rest,
    outputLoudnessRangeLu: outputLra,
    report: { ...report, before: summary(report.before), after: summary(report.after) },
  } as R;
}

/** Throws `TypeError` unless `onProgress` and `cancel` are each absent or a function. */
function assertChainCallbacks(fnName: string, request: object): void {
  for (const name of ['onProgress', 'cancel'] as const) {
    const callback = (request as Record<string, unknown>)[name];
    if (callback !== undefined && callback !== null && typeof callback !== 'function') {
      throw new TypeError(`${fnName}: ${name} must be a function`);
    }
  }
}

/** Request for {@link streamingLoudnessGain}. */
export interface StreamingLoudnessGainRequest extends ValidateOptions {
  /** Mono input samples. */
  samples: Float32Array;
  /** Sample rate in Hz, an integer in `[8000, 384000]`; there is no default. */
  sampleRate: number;
  /** The chain config the streaming chain will run; read like {@link masterAudio}'s `overrides`. */
  config?: MasteringChainConfig;
}

/** Request for {@link streamingLoudnessGainStereo}. */
export interface StreamingLoudnessGainStereoRequest extends ValidateOptions {
  /** Left channel samples. */
  left: Float32Array;
  /** Right channel samples. */
  right: Float32Array;
  /** Sample rate in Hz, an integer in `[8000, 384000]`; there is no default. */
  sampleRate: number;
  /** The chain config the streaming chain will run; read like `masterAudio`'s `overrides`. */
  config?: MasteringChainConfig;
}

// Native still names the input true peak `truePeakDb`.
function streamingGainFromNative(raw: object): StreamingLoudnessGainResult {
  const { truePeakDb, ...rest } = raw as NativeResult & { truePeakDb: number };
  return { ...rest, truePeakDbtp: truePeakDb } as unknown as StreamingLoudnessGainResult;
}

/**
 * Measures the loudness numbers a {@link StreamingMasteringChain} needs.
 *
 * Runs the offline chain described by `config` up to its loudness stage and
 * measures there, so `loudnessStaticGainDb` equals the gain {@link masterAudio}
 * applies (ceiling clamp included) and `truePeakDbtp` is the peak that clamp
 * used. The gain is computed from `config.loudness` whether or not the stage is
 * enabled; a silent or below-gate stage input yields 0 dB.
 *
 * @throws {TypeError} when `sampleRate` is missing
 * @throws {RangeError} on audio out of domain or an out-of-range `sampleRate`
 *
 * @example
 * const { loudnessStaticGainDb, truePeakDbtp } = streamingLoudnessGain(samples, 48000, { config });
 * const chain = new StreamingMasteringChain({
 *   ...config,
 *   sampleRate: 48000,
 *   loudnessStaticGainDb,
 *   loudnessStaticGainPeakDb: truePeakDbtp,
 * });
 */
export function streamingLoudnessGain(
  request: StreamingLoudnessGainRequest,
): StreamingLoudnessGainResult;
export function streamingLoudnessGain(
  samples: Float32Array,
  sampleRate: number,
  options?: OptionsOf<StreamingLoudnessGainRequest>,
): StreamingLoudnessGainResult;
export function streamingLoudnessGain(...args: unknown[]): StreamingLoudnessGainResult {
  const r = sampleOneShot<StreamingLoudnessGainRequest>('streamingLoudnessGain', args);
  assertAudioInput('streamingLoudnessGain', r.samples, r.sampleRate, r);
  return streamingGainFromNative(
    addon.masteringStreamingLoudnessGain(
      r.samples,
      r.sampleRate,
      flattenChainConfig(r.config ?? {}),
    ),
  );
}

/** Stereo counterpart of {@link streamingLoudnessGain}, with BS.1770 channel summing. */
export function streamingLoudnessGainStereo(
  request: StreamingLoudnessGainStereoRequest,
): StreamingLoudnessGainResult {
  const r = requestOnly<StreamingLoudnessGainStereoRequest>('streamingLoudnessGainStereo', [
    request,
  ]);
  assertAudioInput('streamingLoudnessGainStereo', r.left, r.sampleRate, r, 'left');
  assertAudioInput('streamingLoudnessGainStereo', r.right, r.sampleRate, r, 'right');
  if (r.left.length !== r.right.length) {
    throw new RangeError('Stereo channel lengths must match.');
  }
  return streamingGainFromNative(
    addon.masteringStreamingLoudnessGainStereo(
      r.left,
      r.right,
      r.sampleRate,
      flattenChainConfig(r.config ?? {}),
    ),
  );
}

/** Request form of {@link masterAudio}. */
export interface MasterAudioRequest extends ValidateOptions {
  /** Mono input samples. */
  samples: Float32Array;
  /** Sample rate in Hz, an integer in `[8000, 384000]`; there is no default. */
  sampleRate: number;
  /** Built-in preset to start from. Absent: the chain runs `overrides` alone. */
  preset?: MasteringPreset;
  /** Chain config applied over the preset, or run alone when `preset` is absent. */
  overrides?: MasteringChainConfig;
  /** Called between chain stages with `progress` in `[0, 1]` and the stage name. */
  onProgress?: ProgressCallback;
  /** Polled between chain stages; return `true` to stop with a `Cancelled` error. */
  cancel?: () => boolean;
}

/** Request form of {@link masterAudioStereo}. */
export interface MasterAudioStereoRequest extends ValidateOptions {
  /** Left channel samples. */
  left: Float32Array;
  /** Right channel samples. */
  right: Float32Array;
  /** Sample rate in Hz, an integer in `[8000, 384000]`; there is no default. */
  sampleRate: number;
  /** Built-in preset to start from. Absent: the chain runs `overrides` alone. */
  preset?: MasteringPreset;
  /** Chain config applied over the preset, or run alone when `preset` is absent. */
  overrides?: MasteringChainConfig;
  /** Called between chain stages with `progress` in `[0, 1]` and the stage name. */
  onProgress?: ProgressCallback;
  /** Polled between chain stages; return `true` to stop with a `Cancelled` error. */
  cancel?: () => boolean;
}

/**
 * Master mono audio. With a `preset` the chain starts from that preset and
 * `overrides` is applied on top; without one the chain runs `overrides` alone.
 *
 * @param samples - Audio samples (mono, float32)
 * @param sampleRate - Sample rate in Hz, an integer in `[8000, 384000]`
 * @param options - `preset`, `overrides`, `onProgress`, `cancel`, `validate`
 * @returns Processed audio, loudness metadata, and applied stage names
 * @throws {TypeError} when `sampleRate` is missing or a request field has the wrong type
 * @throws {RangeError} on audio out of domain, an unknown `preset` or an invalid override
 */
export function masterAudio(request: MasterAudioRequest): MasteringChainResult;
export function masterAudio(
  samples: Float32Array,
  sampleRate: number,
  options?: OptionsOf<MasterAudioRequest>,
): MasteringChainResult;
export function masterAudio(...args: unknown[]): MasteringChainResult {
  const r = sampleOneShot<MasterAudioRequest>('masterAudio', args);
  assertChainCallbacks('masterAudio', r);
  assertAudioInput('masterAudio', r.samples, r.sampleRate, r);
  const flat = flattenChainConfig(r.overrides ?? {});
  const progress = r.onProgress ?? undefined;
  const cancel = r.cancel ?? undefined;
  if (r.preset === undefined || r.preset === null) {
    return chainResultFromNative(
      progress || cancel
        ? addon.masteringChainWithProgress(
            r.samples,
            r.sampleRate,
            flat,
            progress ?? (() => {}),
            cancel ?? (() => false),
          )
        : addon.masteringChain(r.samples, r.sampleRate, flat),
    );
  }
  return chainResultFromNative(
    progress || cancel
      ? addon.masterAudioWithProgress(
          r.preset,
          r.samples,
          r.sampleRate,
          flat,
          progress ?? (() => {}),
          cancel ?? (() => false),
        )
      : addon.masterAudio(r.preset, r.samples, r.sampleRate, flat),
  );
}

/**
 * Stereo counterpart of {@link masterAudio}; takes a request object only.
 *
 * @returns Processed stereo audio, loudness metadata, and applied stage names
 * @throws {TypeError} when `sampleRate` is missing or a request field has the wrong type
 * @throws {RangeError} on audio out of domain, unequal channel lengths, an unknown `preset`
 *   or an invalid override
 */
export function masterAudioStereo(request: MasterAudioStereoRequest): MasteringChainStereoResult {
  const r = requestOnly<MasterAudioStereoRequest>('masterAudioStereo', [request]);
  assertChainCallbacks('masterAudioStereo', r);
  assertAudioInput('masterAudioStereo', r.left, r.sampleRate, r, 'left');
  assertAudioInput('masterAudioStereo', r.right, r.sampleRate, r, 'right');
  if (r.left.length !== r.right.length) {
    throw new RangeError('Stereo channel lengths must match.');
  }
  const flat = flattenChainConfig(r.overrides ?? {});
  const progress = r.onProgress ?? undefined;
  const cancel = r.cancel ?? undefined;
  if (r.preset === undefined || r.preset === null) {
    return chainResultFromNative(
      progress || cancel
        ? addon.masteringChainStereoWithProgress(
            r.left,
            r.right,
            r.sampleRate,
            flat,
            progress ?? (() => {}),
            cancel ?? (() => false),
          )
        : addon.masteringChainStereo(r.left, r.right, r.sampleRate, flat),
    );
  }
  return chainResultFromNative(
    progress || cancel
      ? addon.masterAudioStereoWithProgress(
          r.preset,
          r.left,
          r.right,
          r.sampleRate,
          flat,
          progress ?? (() => {}),
          cancel ?? (() => false),
        )
      : addon.masterAudioStereo(r.preset, r.left, r.right, r.sampleRate, flat),
  );
}

/** Request form of {@link masterAudioAsync}. */
export interface MasterAudioAsyncRequest extends ValidateOptions {
  /** Mono input samples. */
  samples: Float32Array;
  /** Sample rate in Hz, an integer in `[8000, 384000]`; there is no default. */
  sampleRate: number;
  /** Built-in preset to start from; required because the worker resolves a preset. */
  preset: MasteringPreset;
  /** Chain config applied over the preset. */
  overrides?: MasteringChainConfig;
}

/** Request form of {@link masterAudioStereoAsync}. */
export interface MasterAudioStereoAsyncRequest extends ValidateOptions {
  /** Left channel samples. */
  left: Float32Array;
  /** Right channel samples. */
  right: Float32Array;
  /** Sample rate in Hz, an integer in `[8000, 384000]`; there is no default. */
  sampleRate: number;
  /** Built-in preset to start from; required because the worker resolves a preset. */
  preset: MasteringPreset;
  /** Chain config applied over the preset. */
  overrides?: MasteringChainConfig;
}

/**
 * Asynchronous variant of {@link masterAudio}. Runs the full chain on a libuv
 * worker thread and resolves with the same shape as the synchronous call. A
 * `preset` is required and there is no progress or cancellation; use the
 * synchronous `masterAudio` for those. Every validation failure is a rejected
 * promise, never a synchronous throw.
 *
 * @throws {TypeError} (rejection) when `sampleRate` or `preset` is missing or a field has the
 *   wrong type
 * @throws {RangeError} (rejection) on audio out of domain, an unknown `preset` or an invalid override
 */
export function masterAudioAsync(request: MasterAudioAsyncRequest): Promise<MasteringChainResult>;
export function masterAudioAsync(
  samples: Float32Array,
  sampleRate: number,
  options: OptionsOf<MasterAudioAsyncRequest>,
): Promise<MasteringChainResult>;
export function masterAudioAsync(...args: unknown[]): Promise<MasteringChainResult> {
  try {
    const r = sampleOneShot<MasterAudioAsyncRequest>('masterAudioAsync', args);
    assertAudioInput('masterAudioAsync', r.samples, r.sampleRate, r);
    if (r.preset === undefined || r.preset === null) {
      throw new TypeError('masterAudioAsync: preset is required');
    }
    return addon
      .masterAudioAsync(r.preset, r.samples, r.sampleRate, flattenChainConfig(r.overrides ?? {}))
      .then((result: object) => chainResultFromNative<MasteringChainResult>(result));
  } catch (error) {
    return Promise.reject(error);
  }
}

/** Asynchronous variant of {@link masterAudioStereo}; takes a request object only. */
export function masterAudioStereoAsync(
  request: MasterAudioStereoAsyncRequest,
): Promise<MasteringChainStereoResult> {
  try {
    const r = requestOnly<MasterAudioStereoAsyncRequest>('masterAudioStereoAsync', [request]);
    assertAudioInput('masterAudioStereoAsync', r.left, r.sampleRate, r, 'left');
    assertAudioInput('masterAudioStereoAsync', r.right, r.sampleRate, r, 'right');
    if (r.left.length !== r.right.length) {
      throw new RangeError('Stereo channel lengths must match.');
    }
    if (r.preset === undefined || r.preset === null) {
      throw new TypeError('masterAudioStereoAsync: preset is required');
    }
    return addon
      .masterAudioStereoAsync(
        r.preset,
        r.left,
        r.right,
        r.sampleRate,
        flattenChainConfig(r.overrides ?? {}),
      )
      .then((result: object) => chainResultFromNative<MasteringChainStereoResult>(result));
  } catch (error) {
    return Promise.reject(error);
  }
}
