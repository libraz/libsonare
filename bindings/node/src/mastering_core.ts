import { refuse, refuseType, withEntry } from './internal/entry.js';
import { refuseZeroSentinel } from './internal/metering_native.js';
import { resolveProcessorParams } from './internal/processor_params.js';
import { parseRecordJson, type ResultFamily } from './internal/record_json.js';
import { isAbsent } from './internal/value_guards.js';
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
  /** The take being analyzed: one mono buffer or one or two planar channels. */
  source: Float32Array | readonly Float32Array[];
  /** The take it is compared against: one mono buffer or one or two planar channels. */
  reference: Float32Array | readonly Float32Array[];
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
function nativeLoudnessArgs(r: MasteringNormalizeLoudnessRequest) {
  if (!isAbsent(r.targetLufs)) {
    assertFloatValue(r.targetLufs, 'targetLufs');
  }
  if (!isAbsent(r.ceilingDbtp)) {
    assertFloatValue(r.ceilingDbtp, 'ceilingDbtp');
  }
  refuseZeroSentinel(r, ['truePeakOversample']);
  if (!isAbsent(r.truePeakOversample)) {
    assertIntegerValue(r.truePeakOversample, 'truePeakOversample');
  }
  if (!isAbsent(r.releaseMs)) {
    assertPositiveFloatValue(r.releaseMs, 'releaseMs');
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
    const result = addon.mastering(r.samples, r.sampleRate, ...nativeLoudnessArgs(r));
    return {
      ...result,
      latencySamples: result.latencySamples ?? 0,
    } as MasteringNormalizeLoudnessResult;
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
    const result = addon.masteringProcess(
      r.processor,
      r.samples,
      r.sampleRate,
      withEntry('masteringApplyProcessor', () =>
        resolveProcessorParams(r.processor, r.params ?? {}),
      ),
    );
    return { ...result, latencySamples: result.latencySamples ?? 0 } as MasteringResult;
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
    return addon.masteringProcessStereo(
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

interface PairAnalysisChannels {
  left: Float32Array;
  right?: Float32Array;
}

function normalizePairAnalysisAudio(
  value: unknown,
  sampleRate: number,
  options: object,
  name: string,
): PairAnalysisChannels {
  if (value instanceof Float32Array) {
    assertAudioInput(value, sampleRate, options, name);
    return { left: value };
  }
  if (!Array.isArray(value)) {
    throw refuseType(`${name} must be a Float32Array or an array of Float32Array`);
  }
  if (value.length === 0 || value.length > 2) {
    throw refuse(`${name} must hold 1 or 2 channels`);
  }
  for (let index = 0; index < value.length; index++) {
    assertAudioInput(value[index], sampleRate, options, `${name}[${index}]`);
  }
  const planes = value as Float32Array[];
  const left = planes[0] as Float32Array;
  if (planes.length === 1) {
    return { left };
  }
  const right = planes[1] as Float32Array;
  if (left.length !== right.length) {
    throw refuse(`${name}[1] must have the same length as ${name}[0]`);
  }
  return { left, right };
}

function assertPairAnalysisAudio(
  r: { sampleRate: number; referenceSampleRate?: number },
  source: unknown,
  reference: unknown,
  options: object,
): {
  source: PairAnalysisChannels;
  reference: PairAnalysisChannels;
  referenceRate: number;
} {
  assertSampleRate(r.sampleRate);
  const referenceRate = r.referenceSampleRate ?? r.sampleRate;
  assertSampleRate(referenceRate, 'referenceSampleRate');
  const sourceChannels = normalizePairAnalysisAudio(source, r.sampleRate, options, 'source');
  const referenceChannels = normalizePairAnalysisAudio(
    reference,
    referenceRate,
    options,
    'reference',
  );
  if ((sourceChannels.right === undefined) !== (referenceChannels.right === undefined)) {
    throw refuse(`source and reference must have the same number of channels`);
  }
  return { source: sourceChannels, reference: referenceChannels, referenceRate };
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
    const result = addon.masteringPairProcess(
      r.processor,
      r.source,
      r.reference,
      r.sampleRate,
      referenceRate,
      r.params ?? {},
    );
    return { ...result, latencySamples: result.latencySamples ?? 0 } as MasteringResult;
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
  });
}

/** The result family whose nulls each pair or stereo analysis restores; the others have none. */
const ANALYSIS_FAMILIES: Readonly<Record<PairAnalysis | StereoAnalysis, ResultFamily>> = {
  'match.referenceLoudness': 'match_reference_loudness',
  'match.tonalBalance': 'match_tonal_balance',
  'match.tonalBalanceLogBands': 'match_tonal_balance',
  'match.matchEqCurve': 'match_eq_curve',
  'match.estimateReferenceDelaySamples': 'match_reference_delay',
  'stereo.monoCompatCheck': 'stereo_mono_compat',
  'stereo.monoCompatCheckLogBands': 'stereo_mono_compat_log_bands',
};

/**
 * Analyze mono buffers or planar mono/stereo `source` and `reference` inputs.
 * The takes may have independent lengths and rates; a delay estimate is in
 * samples at `sampleRate`. Takes a request object only; the result type follows
 * `analysis` through {@link PairAnalysisResultMap}.
 *
 * @throws {TypeError} when `sampleRate` or `analysis` is missing or a field has the wrong type
 * @throws {RangeError} on audio out of domain, channel mismatch or an unknown analysis
 */
export function masteringAnalyzePair<A extends PairAnalysis>(
  request: MasteringAnalyzePairRequest<A>,
): PairAnalysisResultMap[A] {
  return withEntry('masteringAnalyzePair', () => {
    const r = requestOnly<MasteringAnalyzePairRequest>('masteringAnalyzePair', [request]);
    assertString(r.analysis, 'analysis');
    const audio = assertPairAnalysisAudio(r, r.source, r.reference, r);
    const json =
      audio.source.right === undefined
        ? addon.masteringPairAnalyze(
            r.analysis,
            audio.source.left,
            audio.reference.left,
            r.sampleRate,
            audio.referenceRate,
            r.params ?? {},
          )
        : addon.masteringPairAnalyzeStereo(
            r.analysis,
            audio.source.left,
            audio.source.right,
            audio.reference.left,
            audio.reference.right as Float32Array,
            r.sampleRate,
            audio.referenceRate,
            r.params ?? {},
          );
    return parseRecordJson(json, ANALYSIS_FAMILIES[r.analysis]) as PairAnalysisResultMap[A];
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
      addon.masteringStereoAnalyze(r.analysis, r.left, r.right, r.sampleRate, r.params ?? {}),
      ANALYSIS_FAMILIES[r.analysis],
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
    return addon.masteringAbMatchLoudness(r.source, r.reference, r.sampleRate, referenceRate);
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
    return addon.masteringAbMatchLoudnessStereo(
      r.sourceLeft,
      r.sourceRight,
      r.referenceLeft,
      r.referenceRight,
      r.sampleRate,
      referenceRate,
    );
  });
}
