/**
 * One repair analysis and one repair application over any number of channels.
 */

import { declipReportFromNative } from './internal/metering_native.js';
import { dbFromLinear, linearFromDb } from './internal/repair_common.js';
import { getSonareModule } from './module_state.js';
import type { ProgressCallback } from './public_types.js';
import type {
  DeclickReport,
  DeclipReport,
  DecrackleReport,
  DehumReport,
  DenoiseReport,
  DereverbReport,
} from './public_types_repair.js';
import type { DereverbClassicalOptions } from './repair_dereverb.js';
import type { DeclickOptions, DeclipOptions, DecrackleOptions } from './repair_impulsive.js';
import type { DehumOptions, DenoiseClassicalOptions } from './repair_noise.js';
import { assertAudioChannels, requestOnly, type ValidateOptions } from './validation.js';

function requireModule() {
  return getSonareModule();
}

/** The repair stages, in the order {@link masteringRepairApply} runs them. */
export type MasteringRepairStageName =
  | 'declip'
  | 'declick'
  | 'decrackle'
  | 'dehum'
  | 'denoise'
  | 'dereverb';

/**
 * One stage to apply: the stage name plus that stage's own options (without `validate`). A
 * setting left out takes the stage's default; a misspelt one is a type error and, at runtime,
 * refused by name. `declip` takes `clipThresholdDb` in dBFS.
 */
export type MasteringRepairStage =
  | ({ stage: 'declip' } & Omit<DeclipOptions, 'validate'>)
  | ({ stage: 'declick' } & Omit<DeclickOptions, 'validate'>)
  | ({ stage: 'decrackle' } & Omit<DecrackleOptions, 'validate'>)
  | ({ stage: 'dehum' } & Omit<DehumOptions, 'validate'>)
  | ({ stage: 'denoise' } & Omit<DenoiseClassicalOptions, 'validate'>)
  | ({ stage: 'dereverb' } & Omit<DereverbClassicalOptions, 'validate'>);

/** Native stage settings still carry the declip level as the linear `clipThreshold`. */
function stageToNative(stage: MasteringRepairStage): Record<string, unknown> {
  const native: Record<string, unknown> = { ...stage };
  if ('clipThreshold' in native) {
    throw new TypeError(
      "masteringRepairApply: unknown setting 'clipThreshold' (use clipThresholdDb)",
    );
  }
  if (native.stage === 'declip' && native.clipThresholdDb !== undefined) {
    native.clipThreshold = linearFromDb(
      'masteringRepairApply',
      'clipThresholdDb',
      native.clipThresholdDb,
    );
  }
  delete native.clipThresholdDb;
  return native;
}

/** The inverse of {@link stageToNative}, for the stages the analysis recommends. */
function stageFromNative(native: Record<string, unknown>): MasteringRepairStage {
  const stage = { ...native };
  if (stage.stage === 'declip' && typeof stage.clipThreshold === 'number') {
    stage.clipThresholdDb = dbFromLinear(stage.clipThreshold);
    delete stage.clipThreshold;
  }
  return stage as unknown as MasteringRepairStage;
}

/**
 * One channel's repair defects, or their aggregate, as {@link masteringRepairAnalyze} measures them.
 * Flat, unlike the grouped `defects` of the audio profile.
 */
export interface MasteringRepairDefects {
  /** False when the detectors did not run; every count is then 0. */
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
  /** Linear amplitude of the flat runs. */
  clipFlatLevel: number;
  noiseFloorDbfs: number;
  noiseBandPeakDbfs: number;
  noiseBandPeakIndex: number;
  humFundamentalHz: number;
  /** Linear energy ratio of the winning hum candidate over the median candidate (not dB); 1.0 means no peak. */
  humFundamentalProminence: number;
  humHarmonics: number;
  humFundamentalDbfs: number;
  humPeakHarmonicDbfs: number;
  lateDecayRatioDb: number;
}

/**
 * What {@link masteringRepairAnalyze} measured and recommends.
 *
 * Crosses as a string; this declaration is held to the paths the C++ writer publishes.
 */
export interface MasteringRepairAnalysis {
  /** The channels' defects aggregated as the stereo audio profile aggregates them. */
  defects: MasteringRepairDefects;
  /** Each channel's own defects, in input order. */
  channels: MasteringRepairDefects[];
  /**
   * Whether a declip at the flat level leaves louder audio alone. When false, declip is withheld
   * from `recommended` even though flat runs were found.
   */
  declipThresholdSafe: boolean;
  /** Programme loudness the noise floor is compared against. */
  integratedLufs: number;
  /**
   * Stages the measurement supports, in application order, each with every setting. Pass it to
   * {@link masteringRepairApply} as it stands. Dereverb is never recommended.
   */
  recommended: MasteringRepairStage[];
  /** Why each stage was or was not chosen. */
  explanation: string[];
}

/** One applied stage's reports: one per channel, or one for a linked stage's whole set. */
export type MasteringRepairStageReports =
  | { stage: 'declip'; scope: 'channel'; reports: DeclipReport[] }
  | { stage: 'declick'; scope: 'channel'; reports: DeclickReport[] }
  | { stage: 'decrackle'; scope: 'channel'; reports: DecrackleReport[] }
  | { stage: 'dehum'; scope: 'channel'; reports: DehumReport[] }
  | { stage: 'denoise'; scope: 'linked'; reports: [DenoiseReport] }
  | { stage: 'dereverb'; scope: 'linked'; reports: [DereverbReport] };

/** Result of {@link masteringRepairApply}. */
export interface MasteringRepairApplyResult {
  /** One processed plane per input channel, in input order. */
  channels: Float32Array[];
  /** One entry per applied stage, in application order. */
  reports: MasteringRepairStageReports[];
}

/** Request for {@link masteringRepairAnalyze}. */
export interface MasteringRepairAnalyzeRequest extends ValidateOptions {
  /** One plane per channel, all of the same length. At least one. */
  channels: Float32Array[];
  /** Sample rate in Hz. Required. */
  sampleRate: number;
  /** When a denoise is recommended, track the noise frame by frame. Default true. */
  preferStreamingSafe?: boolean;
}

/** Request for {@link masteringRepairApply}. */
export interface MasteringRepairApplyRequest extends ValidateOptions {
  /** One plane per channel, all of the same length. At least one. */
  channels: Float32Array[];
  /** Sample rate in Hz. Required. */
  sampleRate: number;
  /** Stages to apply; each at most once. Their order does not matter. */
  stages: MasteringRepairStage[];
  /** Called after each stage with the fraction done and `repair.<stage>`. */
  onProgress?: ProgressCallback;
  /** Polled after each progress report; returning true cancels with `SonareError` `Cancelled`. */
  cancel?: () => boolean;
}

/** The JSON form of a stage's reports, before the per-band arrays become Float32Arrays. */
type StageReportsJson = { stage: string; scope: string; reports: Record<string, unknown>[] };

/** Gives the band and harmonic arrays the Float32Array type, and declip its public detection names. */
function toStageReports(entry: StageReportsJson): MasteringRepairStageReports {
  if (entry.stage === 'denoise' || entry.stage === 'dehum') {
    const key = entry.stage === 'denoise' ? 'bandFloorDbfs' : 'harmonicDbfs';
    for (const report of entry.reports) {
      const detected = report.detected as Record<string, unknown>;
      detected[key] = Float32Array.from(detected[key] as number[]);
    }
  }
  if (entry.stage === 'declip') {
    const reports = entry.reports.map((r) => declipReportFromNative(r as never));
    return { ...entry, reports } as unknown as MasteringRepairStageReports;
  }
  return entry as unknown as MasteringRepairStageReports;
}

/**
 * Measures every channel's repair defects and recommends stages.
 *
 * The six detectors run on each channel and aggregate the way the stereo audio profile
 * aggregates them; the recommendation is the mastering assistant's own repair selection, so
 * its thresholds and its settings (the declip threshold at the measured flat level, the dehum
 * fundamental the search found) are the ones `masteringAssistantSuggest` would choose.
 *
 * @example
 * ```ts
 * const analysis = masteringRepairAnalyze({ channels: [left, right], sampleRate: 48000 });
 * const { channels } = masteringRepairApply({
 *   channels: [left, right],
 *   sampleRate: 48000,
 *   stages: analysis.recommended,
 * });
 * ```
 */
export function masteringRepairAnalyze(
  request: MasteringRepairAnalyzeRequest,
): MasteringRepairAnalysis;
export function masteringRepairAnalyze(...args: unknown[]): MasteringRepairAnalysis {
  const request = requestOnly<MasteringRepairAnalyzeRequest>('masteringRepairAnalyze', args);
  assertAudioChannels('masteringRepairAnalyze', request.channels, request.sampleRate, request);
  const options =
    request.preferStreamingSafe === undefined
      ? {}
      : { preferStreamingSafe: request.preferStreamingSafe };
  const analysis = JSON.parse(
    requireModule().masteringRepairAnalyze(
      request.channels,
      request.sampleRate,
      JSON.stringify(options),
    ),
  ) as MasteringRepairAnalysis;
  analysis.recommended = (analysis.recommended as unknown as Record<string, unknown>[]).map(
    stageFromNative,
  );
  return analysis;
}

/**
 * Applies repair stages to every channel, in the fixed order declip, declick, decrackle, dehum,
 * denoise, dereverb whatever order `stages` gives.
 *
 * Declip, declick and dehum decide over the whole channel set (a defect found in one channel is
 * repaired in all of them, and dehum tracks one fundamental) and report per channel; decrackle
 * runs each channel alone; denoise and dereverb apply one linked mask and report once for the
 * set. A stage named twice is refused.
 *
 * @example
 * ```ts
 * const { channels, reports } = masteringRepairApply({
 *   channels: [left, right],
 *   sampleRate: 48000,
 *   stages: [{ stage: 'denoise', mode: 'logMmse' }, { stage: 'declick' }],
 * });
 * console.log(reports[0].stage); // 'declick'
 * ```
 */
export function masteringRepairApply(
  request: MasteringRepairApplyRequest,
): MasteringRepairApplyResult;
export function masteringRepairApply(...args: unknown[]): MasteringRepairApplyResult {
  const request = requestOnly<MasteringRepairApplyRequest>('masteringRepairApply', args);
  assertAudioChannels('masteringRepairApply', request.channels, request.sampleRate, request);
  if (!Array.isArray(request.stages)) {
    throw new TypeError('masteringRepairApply: stages must be an array');
  }
  const result = requireModule().masteringRepairApply(
    request.channels,
    request.sampleRate,
    JSON.stringify(request.stages.map(stageToNative)),
    request.onProgress ?? null,
    request.cancel ?? null,
  );
  return {
    channels: result.channels,
    reports: (JSON.parse(result.reportsJson) as StageReportsJson[]).map(toStageReports),
  };
}
