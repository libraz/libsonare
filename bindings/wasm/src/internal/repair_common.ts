/**
 * Request-to-native conversions the repair families share.
 */

/** Request fields that are audio, rate or JS-side validation and never reach the native options. */
const NON_NATIVE_FIELDS = new Set([
  'samples',
  'left',
  'right',
  'channels',
  'sampleRate',
  'validate',
]);

/** A dBFS level at or below 0 as a linear amplitude; `TypeError` unless a number, `RangeError` above 0 dBFS or NaN. */
export function linearFromDb(fnName: string, field: string, db: unknown): number {
  if (typeof db !== 'number') {
    throw new TypeError(`${fnName}: ${field} must be a number`);
  }
  if (Number.isNaN(db) || db > 0) {
    throw new RangeError(`${fnName}: ${field} must be at most 0 dBFS`);
  }
  return 10 ** (db / 20);
}

/** A linear amplitude as dBFS. */
export function dbFromLinear(linear: number): number {
  return 20 * Math.log10(linear);
}

/**
 * The options object the native repair call reads: the request without audio, rate and
 * `validate`. `null` on an optional field is absence.
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
