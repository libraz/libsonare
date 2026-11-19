import { withEntry } from './internal/entry.js';
import { refuseZeroSentinel } from './internal/metering_native.js';
import { resolveProcessorParams } from './internal/processor_params.js';
import { parseRecordJson } from './internal/record_json.js';
import { addon } from './native.js';
import type {
  LoudnessMatchResult,
  LoudnessMatchStereoResult,
  MasteringNormalizeLoudnessOptions,
  MasteringNormalizeLoudnessResult,
  MasteringPairParams,
  MasteringProcessorParams,
  MasteringResult,
  MasteringStereoResult,
  PairAnalysis,
  PairAnalysisResultMap,
  PairProcessor,
  SoloProcessor,
  StereoAnalysis,
  StereoAnalysisResultMap,
  StereoPairProcessor,
} from './types.js';
import type { OptionsOf } from './validation.js';
import {
  assertAudioInput,
  assertFloatValue,
  assertIntegerValue,
  assertPositiveFloatValue,
  assertSampleRate,
  assertString,
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

// `releaseMs` and `truePeakOversample` read 0 as "library default" natively, so an omitted one is passed as that 0.
function nativeLoudnessArgs(fnName: string, r: MasteringNormalizeLoudnessRequest) {
  if (r.targetLufs !== undefined && r.targetLufs !== null) {
    assertFloatValue(fnName, r.targetLufs, 'targetLufs');
  }
  if (r.ceilingDbtp !== undefined && r.ceilingDbtp !== null) {
    assertFloatValue(fnName, r.ceilingDbtp, 'ceilingDbtp');
  }
  refuseZeroSentinel(fnName, r, ['truePeakOversample']);
  if (r.truePeakOversample !== undefined && r.truePeakOversample !== null) {
    assertIntegerValue(fnName, r.truePeakOversample, 'truePeakOversample');
  }
  if (r.releaseMs !== undefined && r.releaseMs !== null) {
    assertPositiveFloatValue(fnName, r.releaseMs, 'releaseMs');
  }
  return [
    r.targetLufs ?? -14.0,
    r.ceilingDbtp ?? -1.0,
    r.truePeakOversample ?? 0,
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
 * @param options - `processor` (required), `params`
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
  assertString('masteringApplyProcessor', r.processor, 'processor');
  assertAudioInput('masteringApplyProcessor', r.samples, r.sampleRate, r);
  const result = addon.masteringProcess(
    r.processor,
    r.samples,
    r.sampleRate,
    withEntry('masteringApplyProcessor', () => resolveProcessorParams(r.processor, r.params ?? {})),
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
  assertString('masteringApplyProcessorStereo', r.processor, 'processor');
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
    withEntry('masteringApplyProcessorStereo', () =>
      resolveProcessorParams(r.processor, r.params ?? {}),
    ),
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
  assertString(fn, r.processor, 'processor');
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
  assertString(fn, r.processor, 'processor');
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
  assertString(fn, r.analysis, 'analysis');
  const referenceRate = assertPairAudio(fn, r, ['source', r.source], ['reference', r.reference]);
  return parseRecordJson(
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
  assertString(fn, r.analysis, 'analysis');
  assertAudioInput(fn, r.left, r.sampleRate, r, 'left');
  assertAudioInput(fn, r.right, r.sampleRate, r, 'right');
  return parseRecordJson(
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
