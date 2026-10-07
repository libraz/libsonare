/**
 * Input conversion the effect families share.
 */

import type { VoicedFlags } from './public_types.js';

// The embind layer reads the companion voicing array as Float32Array. Keep the
// public union a runtime contract before conversion: strings, Float64Arrays and
// arbitrary array-like objects must not become flags through truthiness.
// Same 1/0 reduction as the Node facade's toVoicedInt32.
export function toVoicedFloat32(fnName: string, voiced: VoicedFlags): Float32Array {
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
  const out = new Float32Array(voiced.length);
  for (let index = 0; index < voiced.length; index += 1) {
    const value = voiced[index];
    if (typeof value !== 'number' && typeof value !== 'boolean') {
      throw new TypeError(`${fnName}: voiced array entries must be numbers or booleans`);
    }
    out[index] = value ? 1 : 0;
  }
  return out;
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
