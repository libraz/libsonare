/**
 * Facade-to-native conversions for the metering, loudness, dynamics and clip-detection calls.
 * The native layer still uses the older spellings, so every mapping lives here, one helper per concept.
 */

import type { ClippingReport } from '../metering.js';
import type { ClipDetection, DeclipReport } from '../public_types_repair.js';
import type { WasmClipDetection, WasmDeclipReport } from '../sonare.js';
import { assertAudioChannels, assertAudioSamples, assertFloatValue } from '../validation.js';
import { refuse, refuseType } from './entry.js';
import { requireListBudget } from './list_budget.js';
import { interleavePlanes } from './pod_config.js';

/**
 * An optional numeric field whose native `0` means "library default": an explicit 0 is refused,
 * absent and `null` pass, and any other value is left to the native reader's own checks.
 */
export function refuseZeroSentinel(request: object, fields: readonly string[]): void {
  for (const field of fields) {
    const value = (request as Record<string, unknown>)[field];
    if (value === undefined || value === null) {
      continue;
    }
    if (value === 0) {
      throw refuse(`${field} must not be 0; omit it for the default`);
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
 * Planar channels as the interleaved buffer the native multichannel calls read. With a
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
  } else {
    if (!Array.isArray(channels)) {
      throw refuseType(`channels must be an array of Float32Array`);
    }
    requireListBudget(channels, 'channels');
    if (channels.length === 0) {
      throw refuse(`channels must not be empty`);
    }
    channels.forEach((plane, i) => {
      assertAudioSamples(plane, options, `channels[${i}]`);
    });
  }
  const planes = channels as Float32Array[];
  const frames = (planes[0] as Float32Array).length;
  if (planes.some((plane) => plane.length !== frames)) {
    throw refuse(`channels must all have the same length`);
  }
  return { data: interleavePlanes(planes), numChannels: planes.length };
}

/** The native waveform buckets (channel-major flat arrays plus a count) as one array per channel. */
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
  return {
    min: deinterleaveChannelMajor(native.min, native.channels, native.bucketCount),
    max: deinterleaveChannelMajor(native.max, native.channels, native.bucketCount),
    bucketCount: native.bucketCount,
    samplesPerBucket: native.samplesPerBucket,
  };
}

function deinterleaveChannelMajor(
  data: Float32Array,
  numChannels: number,
  bucketCount: number,
): Float32Array[] {
  return Array.from({ length: numChannels }, (_, c) =>
    data.slice(c * bucketCount, (c + 1) * bucketCount),
  );
}

/** The native loudness series, whose keys still lack the unit suffix, under the public names. */
export function lufsSeriesFromNative(native: {
  momentary: Float32Array;
  shortTerm: Float32Array;
}): {
  momentaryLufs: Float32Array;
  shortTermLufs: Float32Array;
} {
  return { momentaryLufs: native.momentary, shortTermLufs: native.shortTerm };
}

/** The native clipping report, whose ratio is still called `clippingRatio`, under the public name. */
export function clippingReportFromNative(
  native: Omit<ClippingReport, 'clippedRatio'> & { clippingRatio: number },
): ClippingReport {
  const { clippingRatio, ...rest } = native;
  return { ...rest, clippedRatio: clippingRatio };
}

/** The native declip analysis, whose counts are still `sampleCount` / `sampleFraction`, under the public names. */
export function clipDetectionFromNative(native: WasmClipDetection): ClipDetection {
  const { sampleCount, sampleFraction, ...rest } = native;
  return { clippedSamples: sampleCount, clippedRatio: sampleFraction, ...rest };
}

/** A native declip report with its analysis renamed through {@link clipDetectionFromNative}. */
export function declipReportFromNative(native: WasmDeclipReport): DeclipReport {
  return { ...native, detected: clipDetectionFromNative(native.detected) };
}

/** A single measured number: NaN (not measured) is `undefined`. */
export function measured(value: number): number | undefined {
  return Number.isNaN(value) ? undefined : value;
}
