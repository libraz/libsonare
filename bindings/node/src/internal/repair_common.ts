/**
 * Input shape and geometry check the repair families share.
 */

import type { MasteringAudioProfileDefects } from '../types_mastering.js';
import type { DehumReport, DenoiseReport, HumDetection, NoiseDetection } from '../types_repair.js';
import { assertIntegerValue, assertPositiveInteger } from '../validation.js';
import { isAbsent } from './value_guards.js';

/**
 * Check the STFT geometry the denoise and dereverb entries share, before the
 * addon's options reader narrows it onto a size the core accepts.
 *
 * Only what the caller supplied, because the whole request travels to that
 * reader. The two halves take different guards because the core states
 * different rules: `nFft` must be a POWER OF TWO, which positivity does not
 * imply, so only the truncation is closed here and the size rule stays with the
 * reader that names it; `hopLength` is checked `> 0` outright, so positivity is
 * this field's own domain and the `hopLength <= nFft` pairing stays the core's.
 */
export function assertRepairGeometry(options: { nFft?: number; hopLength?: number }): void {
  if (options.nFft !== undefined) {
    assertIntegerValue(options.nFft, 'nFft');
  }
  if (options.hopLength !== undefined) {
    assertPositiveInteger(options.hopLength, 'hopLength');
  }
}

/** Request fields that are audio, rate or JS-side validation and never reach the addon options. */
const NON_NATIVE_FIELDS = new Set(['samples', 'left', 'right', 'channels', 'sampleRate']);

/**
 * The options object the addon reads: the request without audio and rate. `null`
 * is absence.
 */
export function repairNativeOptions(request: object): Record<string, unknown> {
  const native: Record<string, unknown> = {};
  for (const [key, value] of Object.entries(request)) {
    if (!NON_NATIVE_FIELDS.has(key) && !isAbsent(value)) {
      native[key] = value;
    }
  }
  return native;
}

/** Grouped defects as the profile and repair documents carry them, with the detection arrays as `Float32Array`. */
export function audioDefectsFromNative(native: unknown): MasteringAudioProfileDefects {
  const d = native as MasteringAudioProfileDefects;
  return { ...d, noise: noiseDetection(d.noise), hum: humDetection(d.hum) };
}

/** A hum detection with the addon's `harmonicDbfs` array as a `Float32Array`. */
export function humDetection(
  native: Omit<HumDetection, 'harmonicDbfs'> & { harmonicDbfs: ArrayLike<number> },
): HumDetection {
  return { ...native, harmonicDbfs: Float32Array.from(native.harmonicDbfs) };
}

/** A noise detection with the addon's band arrays as a `Float32Array` and an `Int32Array`. */
export function noiseDetection(
  native: Omit<NoiseDetection, 'bandFloorDbfs' | 'bandFloorBand'> & {
    bandFloorDbfs: ArrayLike<number>;
    bandFloorBand: ArrayLike<number>;
  },
): NoiseDetection {
  return {
    ...native,
    bandFloorDbfs: Float32Array.from(native.bandFloorDbfs),
    bandFloorBand: Int32Array.from(native.bandFloorBand),
  };
}

/** A dehum report with its detection's array typed. */
export function dehumReport(
  native: Omit<DehumReport, 'detected'> & { detected: Parameters<typeof humDetection>[0] },
): DehumReport {
  return { ...native, detected: humDetection(native.detected) };
}

/** A denoise report with its detection's array typed. */
export function denoiseReport(
  native: Omit<DenoiseReport, 'detected'> & { detected: Parameters<typeof noiseDetection>[0] },
): DenoiseReport {
  return { ...native, detected: noiseDetection(native.detected) };
}

/** Native per-channel pair result as `{channels, reports}`, in left, right order. */
export function pairedResult<N, R = N>(
  native: { left: Float32Array; right: Float32Array; leftReport: N; rightReport: N },
  report: (native: N) => R = (r) => r as unknown as R,
): { channels: Float32Array[]; reports: R[] } {
  return {
    channels: [native.left, native.right],
    reports: [report(native.leftReport), report(native.rightReport)],
  };
}

/** Native one-report result, stereo or linked, as `{channels, reports: [report]}`. */
export function sharedReportResult<N, R = N>(
  native: { report: N } & (
    | { channels: Float32Array[] }
    | { left: Float32Array; right: Float32Array }
  ),
  report: (native: N) => R = (r) => r as unknown as R,
): { channels: Float32Array[]; reports: R[] } {
  return {
    channels: 'channels' in native ? native.channels : [native.left, native.right],
    reports: [report(native.report)],
  };
}
