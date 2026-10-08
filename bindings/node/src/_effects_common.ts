/**
 * Input shape and checks the effect families share.
 */

import type { VoicedFlags } from './types.js';
import { assertInt64, resolveSampleBound } from './validation.js';

// The addon reads the companion voicing array as an Int32Array and silently
// ignores any other type, so validate and normalize here rather than at the
// N-API boundary. A flag is a decision, not a magnitude: collapse to 1/0 on
// truthiness, which is the same reduction the WASM facade applies, so both
// surfaces agree on every accepted input type.
export function toVoicedInt32(fnName: string, voiced: VoicedFlags): Int32Array {
  if (
    !(
      voiced instanceof Int32Array ||
      voiced instanceof Uint8Array ||
      voiced instanceof Float32Array ||
      Array.isArray(voiced)
    )
  ) {
    throw new TypeError(
      `${fnName}: voiced must be an Int32Array, Uint8Array, Float32Array, number[], or boolean[]`,
    );
  }
  const out = new Int32Array(voiced.length);
  for (let index = 0; index < voiced.length; index += 1) {
    if (Array.isArray(voiced)) {
      const value = voiced[index];
      if (typeof value !== 'number' && typeof value !== 'boolean') {
        throw new TypeError(`${fnName}: voiced array entries must be numbers or booleans`);
      }
    }
    out[index] = voiced[index] ? 1 : 0;
  }
  return out;
}

function isPlainObject(value: unknown): value is Record<string, unknown> {
  return typeof value === 'object' && value !== null && !Array.isArray(value);
}

/** The time fields a note or event entry may spell in samples or in seconds. */
interface TimedEntry {
  onsetSample?: number;
  onsetSec?: number;
  offsetSample?: number;
  offsetSec?: number;
  edit?: { timeOffsetSamples?: number; timeOffsetSec?: number };
}

/**
 * Resolve the time fields of a note or event set into the samples the addon
 * reads, and check the pending time offsets before the addon narrows them.
 *
 * A bound or offset may be spelled in samples or in seconds (rounded to the
 * nearest sample at `sampleRate`), one spelling each. Zero is the offset's
 * identity rather than a default, so a truncated sub-sample shift would render
 * the set unmoved and report success. `spans` is false for entries identified by
 * frame bounds, which carry no sample span.
 */
export function resolveEntryTimes<T extends TimedEntry>(
  fnName: string,
  entries: readonly T[],
  arrayName: string,
  sampleRate: number,
  spans: boolean,
): T[] {
  return entries.map((entry, index) => {
    if (entry === null || typeof entry !== 'object') {
      return entry;
    }
    const at = `${arrayName}[${index}]`;
    const { onsetSec, offsetSec, ...rest } = entry;
    const resolved: TimedEntry = { ...rest };
    if (spans) {
      resolved.onsetSample = resolveSampleBound(
        fnName,
        entry.onsetSample,
        onsetSec,
        sampleRate,
        `${at}.onsetSample`,
        `${at}.onsetSec`,
      );
      resolved.offsetSample = resolveSampleBound(
        fnName,
        entry.offsetSample,
        offsetSec,
        sampleRate,
        `${at}.offsetSample`,
        `${at}.offsetSec`,
      );
    }
    if (isPlainObject(entry.edit)) {
      const { timeOffsetSec, ...edit } = entry.edit;
      const offset = resolveSampleBound(
        fnName,
        edit.timeOffsetSamples,
        timeOffsetSec,
        sampleRate,
        `${at}.edit.timeOffsetSamples`,
        `${at}.edit.timeOffsetSec`,
        'signed',
      );
      if (offset !== undefined) {
        assertInt64(fnName, offset, `${at}.edit.timeOffsetSamples`);
      }
      resolved.edit = { ...entry.edit, timeOffsetSamples: offset };
      delete (resolved.edit as { timeOffsetSec?: number }).timeOffsetSec;
    }
    return resolved as T;
  });
}

/** Common audio input fields for stateless effect requests. */
export interface EffectSamplesRequest {
  samples: Float32Array;
  sampleRate?: number;
}

/**
 * The addon reads audio as a `Float32Array` and rejects anything else, so a
 * plain number array is converted here rather than at the N-API boundary.
 */
export function toSamples(samples: Float32Array | readonly number[]): Float32Array {
  return samples instanceof Float32Array ? samples : Float32Array.from(samples);
}

export function assertPitchTrackLengths(
  fnName: string,
  f0Hz: Float32Array,
  voiced?: VoicedFlags | null,
  voicedProb?: Float32Array | null,
): void {
  if (voiced != null && voiced.length !== f0Hz.length) {
    throw new RangeError(`${fnName}: voiced must have the same length as f0Hz`);
  }
  if (voiced == null && voicedProb != null && voicedProb.length !== f0Hz.length) {
    throw new RangeError(`${fnName}: voicedProb must have the same length as f0Hz`);
  }
}
