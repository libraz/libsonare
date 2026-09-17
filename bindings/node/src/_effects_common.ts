/**
 * Input shape and checks the effect families share.
 */

import type { VoicedFlags } from './types.js';
import { assertInt64 } from './validation.js';

// The addon reads the companion voicing array as an Int32Array and silently
// ignores any other type, so normalize here rather than at the N-API boundary.
// A flag is a decision, not a magnitude: collapse to 1/0 on truthiness, which
// is the same reduction the WASM facade applies, so both surfaces agree on
// every accepted input type.
export function toVoicedInt32(voiced: VoicedFlags): Int32Array {
  const out = new Int32Array(voiced.length);
  for (let index = 0; index < voiced.length; index += 1) {
    out[index] = voiced[index] ? 1 : 0;
  }
  return out;
}

/**
 * Check the pending time offsets of a note or event set before the addon
 * narrows them. Zero is this field's identity rather than a default, so a
 * truncated sub-sample shift renders the set unmoved and reports success.
 */
export function assertEditTimeOffsets(
  fnName: string,
  entries: ReadonlyArray<{ edit?: { timeOffsetSamples?: number } }>,
  arrayName: string,
): void {
  entries.forEach((entry, index) => {
    const offset = entry?.edit?.timeOffsetSamples;
    if (offset !== undefined) {
      assertInt64(fnName, offset, `${arrayName}[${index}].edit.timeOffsetSamples`);
    }
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
  f0Hz: Float32Array,
  voiced?: VoicedFlags,
  voicedProb?: Float32Array,
): void {
  if (voiced !== undefined && voiced.length !== f0Hz.length) {
    throw new RangeError('voiced must have the same length as f0Hz');
  }
  if (voicedProb !== undefined && voicedProb.length !== f0Hz.length) {
    throw new RangeError('voicedProb must have the same length as f0Hz');
  }
}
