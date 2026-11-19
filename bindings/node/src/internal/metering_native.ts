/**
 * Facade-to-addon conversions for the metering, loudness, dynamics and clip-detection calls.
 * The addon still uses the older spellings, so every mapping lives here, one helper per concept.
 */

import type { ClippingReport } from '../metering.js';
import type { ClipDetection, DeclipReport } from '../types_repair.js';
import { assertAudioChannels, assertAudioSamples, assertFloatValue } from '../validation.js';
import { refuse, refuseType } from './entry.js';
import { interleavePlanar } from './planar.js';

/**
 * An optional numeric field whose addon `0` means "library default": an explicit 0 is refused,
 * absent and `null` pass, and any other value is left to the addon reader's own checks.
 */
export function refuseZeroSentinel(request: object, fields: readonly string[]): void {
  for (const field of fields) {
    const value = (request as Record<string, unknown>)[field];
    if (value === undefined || value === null) {
      continue;
    }
    if (value === 0) {
      throw refuse(`${field} must not be 0 (omit it for the default)`);
    }
  }
}

/** A percentile rank: `TypeError` unless a number, `RangeError` outside [0, 1]. */
export function assertUnitInterval(value: unknown, argName: string): void {
  assertFloatValue(value, argName);
  if ((value as number) < 0 || (value as number) > 1) {
    throw refuse(`${argName} must be in [0, 1]`);
  }
}

/**
 * Planar channels as the interleaved buffer the addon's multichannel calls read. With a
 * `sampleRate` each plane gets the full audio preflight; without one (waveform buckets are in
 * samples) only the buffer checks run.
 */
export function interleaveMeterChannels(
  channels: unknown,
  options: object,
  sampleRate?: unknown,
): { data: Float32Array; numChannels: number } {
  if (sampleRate !== undefined) {
    assertAudioChannels(channels, sampleRate, options);
  } else if (Array.isArray(channels)) {
    channels.forEach((plane, i) => {
      assertAudioSamples(plane, options, `channels[${i}]`);
    });
  }
  return interleavePlanar(channels);
}

/** The addon's waveform buckets (channel-major flat arrays plus a count) as one array per channel. */
export function waveformReportFromNative(native: {
  min: Float32Array;
  max: Float32Array;
  channels: number;
  bucketCount: number;
  samplesPerBucket: number;
}): {
  min: Float32Array[];
  max: Float32Array[];
  bucketCount: number;
  samplesPerBucket: number;
} {
  const split = (data: Float32Array): Float32Array[] =>
    Array.from({ length: native.channels }, (_, c) =>
      data.slice(c * native.bucketCount, (c + 1) * native.bucketCount),
    );
  return {
    min: split(native.min),
    max: split(native.max),
    bucketCount: native.bucketCount,
    samplesPerBucket: native.samplesPerBucket,
  };
}

/** The addon's loudness series, whose keys still lack the unit suffix, under the public names. */
export function lufsSeriesFromNative(native: {
  momentary: Float32Array;
  shortTerm: Float32Array;
}): {
  momentaryLufs: Float32Array;
  shortTermLufs: Float32Array;
} {
  return { momentaryLufs: native.momentary, shortTermLufs: native.shortTerm };
}

/** The addon's clipping report, whose ratio is still called `clippingRatio`, under the public name. */
export function clippingReportFromNative(
  native: Omit<ClippingReport, 'clippedRatio'> & { clippingRatio: number },
): ClippingReport {
  const { clippingRatio, ...rest } = native;
  return { ...rest, clippedRatio: clippingRatio };
}

type NativeClipDetection = Omit<ClipDetection, 'clippedSamples' | 'clippedRatio'> & {
  sampleCount: number;
  sampleFraction: number;
};

/**
 * The linear amplitude the clip detector still reads, from `thresholdDb` in dBFS.
 * Absent is the native 0, which selects the library's threshold; a level that
 * would reach the detector as 0 is refused, since it would select it silently.
 */
export function clipThresholdLinear(thresholdDb: unknown): number {
  if (thresholdDb === undefined || thresholdDb === null) {
    return 0;
  }
  if (typeof thresholdDb !== 'number') {
    throw refuseType(`thresholdDb must be a number`);
  }
  if (Number.isNaN(thresholdDb) || thresholdDb > 0) {
    throw refuse(`thresholdDb must be at most 0 dBFS`);
  }
  const linear = Math.fround(10 ** (thresholdDb / 20));
  if (linear === 0) {
    throw refuse(`thresholdDb is below the smallest level the detector can hold`);
  }
  return linear;
}

/** The addon's declip analysis, whose counts are still `sampleCount` / `sampleFraction`, under the public names. */
export function clipDetectionFromNative(native: NativeClipDetection): ClipDetection {
  const { sampleCount, sampleFraction, ...rest } = native;
  return { clippedSamples: sampleCount, clippedRatio: sampleFraction, ...rest };
}

/** An addon declip report with its analysis renamed through {@link clipDetectionFromNative}. */
export function declipReportFromNative(
  native: Omit<DeclipReport, 'detected'> & { detected: NativeClipDetection },
): DeclipReport {
  return { ...native, detected: clipDetectionFromNative(native.detected) };
}
