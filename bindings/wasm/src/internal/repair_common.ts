/**
 * Request-to-native conversions the repair families share.
 */

import type { MasteringAudioProfileDefects } from '../public_types_mastering.js';

/** Grouped defects as the profile and repair documents carry them, with the detection arrays as `Float32Array`. */
export function audioDefectsFromNative(native: unknown): MasteringAudioProfileDefects {
  const d = native as MasteringAudioProfileDefects;
  return {
    ...d,
    noise: { ...d.noise, bandFloorDbfs: Float32Array.from(d.noise.bandFloorDbfs) },
    hum: { ...d.hum, harmonicDbfs: Float32Array.from(d.hum.harmonicDbfs) },
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
    if (!NON_NATIVE_FIELDS.has(key) && value !== undefined && value !== null) {
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
