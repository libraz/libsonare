/**
 * One repair analysis and one repair application over any number of channels.
 */

import { refuseType, withEntry } from './internal/entry.js';
import { parseRecordJson } from './internal/record_json.js';
import { audioDefectsFromNative, humDetection, noiseDetection } from './internal/repair_common.js';
import { getSonareModule } from './module_state.js';
import type { ProgressCallback } from './public_types.js';
import type { MasteringAudioProfileDefects } from './public_types_mastering.js';
import type {
  DeclickReport,
  DeclipReport,
  DecrackleReport,
  DehumReport,
  DenoiseReport,
  DereverbReport,
} from './public_types_repair.js';
import type { DereverbClassicalOptions } from './repair_dereverb.js';
import type {
  DeclickOptions,
  DeclipOptions,
  DecrackleOptions,
  MasteringRepairDeclipStereoRequest,
} from './repair_impulsive.js';
import type { DehumOptions, DenoiseClassicalOptions } from './repair_noise.js';
import { assertAudioChannels, requestOnly } from './validation.js';

/** The repair stages, in the order {@link masteringRepairApply} runs them. */
export type MasteringRepairStageName =
  | 'declip'
  | 'declick'
  | 'decrackle'
  | 'dehum'
  | 'denoise'
  | 'dereverb';

/**
 * One stage to apply: the stage name plus that stage's own options. A
 * setting left out takes the stage's default; a misspelt one is a type error and, at runtime,
 * refused by name. `declip` takes `clipThresholdDb` in dBFS.
 */
export type MasteringRepairStage =
  | ({
      /** Stage to apply. */
      stage: 'declip';
    } & DeclipOptions &
      Pick<MasteringRepairDeclipStereoRequest, 'linkChannels'>)
  | ({
      /** Stage to apply. */
      stage: 'declick';
    } & DeclickOptions)
  | ({
      /** Stage to apply. */
      stage: 'decrackle';
    } & DecrackleOptions)
  | ({
      /** Stage to apply. */
      stage: 'dehum';
    } & DehumOptions)
  | ({
      /** Stage to apply. */
      stage: 'denoise';
    } & DenoiseClassicalOptions)
  | ({
      /** Stage to apply. */
      stage: 'dereverb';
    } & DereverbClassicalOptions);

/**
 * What {@link masteringRepairAnalyze} measured and recommends.
 *
 * Crosses as a string; this declaration is held to the paths the C++ writer publishes.
 */
export interface MasteringRepairAnalysis {
  /** The channels' defects aggregated as the stereo audio profile aggregates them; absent when the detectors did not run. */
  defects?: MasteringAudioProfileDefects;
  /** Each channel's own defects, in input order; absent exactly when `defects` is. */
  channels?: MasteringAudioProfileDefects[];
  /**
   * Whether a declip at the flat level leaves louder audio alone. When false, declip is withheld
   * from `recommended` even though flat runs were found.
   */
  declipThresholdSafe: boolean;
  /** Programme loudness in LUFS the noise floor is compared against. `-Infinity` for silence. */
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
  | {
      /** Stage applied. */
      stage: 'declip';
      /** `'channel'`: one report per channel, in channel order. */
      scope: 'channel';
      /** The stage's reports. */
      reports: DeclipReport[];
    }
  | {
      /** Stage applied. */
      stage: 'declick';
      /** `'channel'`: one report per channel, in channel order. */
      scope: 'channel';
      /** The stage's reports. */
      reports: DeclickReport[];
    }
  | {
      /** Stage applied. */
      stage: 'decrackle';
      /** `'channel'`: one report per channel, in channel order. */
      scope: 'channel';
      /** The stage's reports. */
      reports: DecrackleReport[];
    }
  | {
      /** Stage applied. */
      stage: 'dehum';
      /** `'channel'`: one report per channel, in channel order. */
      scope: 'channel';
      /** The stage's reports. */
      reports: DehumReport[];
    }
  | {
      /** Stage applied. */
      stage: 'denoise';
      /** `'linked'`: one report covering every channel. */
      scope: 'linked';
      /** The stage's reports. */
      reports: [DenoiseReport];
    }
  | {
      /** Stage applied. */
      stage: 'dereverb';
      /** `'linked'`: one report covering every channel. */
      scope: 'linked';
      /** The stage's reports. */
      reports: [DereverbReport];
    };

/** Result of {@link masteringRepairApply}. */
export interface MasteringRepairApplyResult {
  /** One processed plane per input channel, in input order. */
  channels: Float32Array[];
  /** One entry per applied stage, in application order. */
  reports: MasteringRepairStageReports[];
}

/** Request for {@link masteringRepairAnalyze}. */
export interface MasteringRepairAnalyzeRequest {
  /** One plane per channel, all of the same length. At least one. */
  channels: Float32Array[];
  /** Sample rate in Hz. Required. */
  sampleRate: number;
  /** When a denoise is recommended, track the noise frame by frame. Default true. */
  preferStreamingSafe?: boolean;
}

/** Request for {@link masteringRepairApply}. */
export interface MasteringRepairApplyRequest {
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

/** Gives the band and harmonic level arrays the Float32Array type and the band indices Int32Array. */
function toStageReports(entry: StageReportsJson): MasteringRepairStageReports {
  for (const report of entry.reports) {
    const detected = report.detected as Record<string, unknown>;
    if (entry.stage === 'denoise') {
      report.detected = noiseDetection(detected as unknown as Parameters<typeof noiseDetection>[0]);
    } else if (entry.stage === 'dehum') {
      report.detected = humDetection(detected as unknown as Parameters<typeof humDetection>[0]);
    }
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
  return withEntry('masteringRepairAnalyze', () => {
    const request = requestOnly<MasteringRepairAnalyzeRequest>('masteringRepairAnalyze', args);
    assertAudioChannels(request.channels, request.sampleRate, request);
    const options =
      request.preferStreamingSafe === undefined
        ? {}
        : { preferStreamingSafe: request.preferStreamingSafe };
    const analysis = parseRecordJson<MasteringRepairAnalysis>(
      getSonareModule().masteringRepairAnalyze(
        request.channels,
        request.sampleRate,
        JSON.stringify(options),
      ),
      'repair_analysis',
    );
    if (analysis.defects !== undefined) {
      analysis.defects = audioDefectsFromNative(analysis.defects);
      analysis.channels = analysis.channels?.map(audioDefectsFromNative);
    }
    return analysis;
  });
}

/**
 * Applies repair stages to every channel, in the fixed order declip, declick, decrackle, dehum,
 * denoise, dereverb whatever order `stages` gives.
 *
 * Declick and dehum decide over the whole channel set (a defect found in one channel is repaired
 * in all of them, and dehum tracks one fundamental) and report per channel; declip repairs each
 * channel's own clipped runs unless `linkChannels` is set, and reports per channel; decrackle
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
  return withEntry('masteringRepairApply', () => {
    const request = requestOnly<MasteringRepairApplyRequest>('masteringRepairApply', args);
    assertAudioChannels(request.channels, request.sampleRate, request);
    if (!Array.isArray(request.stages)) {
      throw refuseType('stages must be an array');
    }
    const result = getSonareModule().masteringRepairApply(
      request.channels,
      request.sampleRate,
      JSON.stringify(request.stages),
      request.onProgress ?? null,
      request.cancel ?? null,
    );
    return {
      channels: result.channels,
      reports: (JSON.parse(result.reportsJson) as StageReportsJson[]).map(toStageReports),
    };
  });
}
