/**
 * Request-to-native conversions the repair families share.
 */

import type { MasteringAudioProfileDefects } from '../public_types_mastering.js';
import type { HumDetection, NoiseDetection } from '../public_types_repair.js';
import { isAbsent } from './value_guards.js';

/** Grouped defects as the profile and repair documents carry them, with the detection arrays typed. */
export function audioDefectsFromNative(native: unknown): MasteringAudioProfileDefects {
  const d = native as MasteringAudioProfileDefects;
  return { ...d, noise: noiseDetection(d.noise), hum: humDetection(d.hum) };
}

/** A document hum detection with its `harmonicDbfs` array as a `Float32Array`. */
export function humDetection(
  native: Omit<HumDetection, 'harmonicDbfs'> & { harmonicDbfs: ArrayLike<number> },
): HumDetection {
  return { ...native, harmonicDbfs: Float32Array.from(native.harmonicDbfs) };
}

/** A document noise detection with its band arrays as a `Float32Array` and an `Int32Array`. */
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

/** Request fields that are audio, rate or JS-side validation and never reach the native options. */
const NON_NATIVE_FIELDS = new Set(['samples', 'left', 'right', 'channels', 'sampleRate']);

/**
 * The options object the native repair call reads: the request without audio, rate and
 * `null` on an optional field is absence.
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

/** Native per-channel pair result as `{channels, reports}`, in left, right order. */
export function pairedResult<R>(native: {
  left: Float32Array;
  right: Float32Array;
  leftReport: R;
  rightReport: R;
}): { channels: Float32Array[]; reports: R[] } {
  return {
    channels: [native.left, native.right],
    reports: [native.leftReport, native.rightReport],
  };
}

/** Native one-report result, stereo or linked, as `{channels, reports: [report]}`. */
export function sharedReportResult<R>(
  native: { report: R } & (
    | { channels: Float32Array[] }
    | { left: Float32Array; right: Float32Array }
  ),
): { channels: Float32Array[]; reports: R[] } {
  return {
    channels: 'channels' in native ? native.channels : [native.left, native.right],
    reports: [native.report],
  };
}
