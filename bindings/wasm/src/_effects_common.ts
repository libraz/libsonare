/**
 * Input conversion the effect families share.
 */

import type { VoicedFlags } from './public_types';

// The embind layer reads the companion voicing array as Float32Array. Keep the
// public union a runtime contract before conversion: strings, Float64Arrays and
// arbitrary array-like objects must not become flags through truthiness.
export function toVoicedFloat32(voiced: VoicedFlags): Float32Array {
  if (
    !(
      voiced instanceof Int32Array ||
      voiced instanceof Uint8Array ||
      voiced instanceof Float32Array ||
      Array.isArray(voiced)
    )
  ) {
    throw new TypeError(
      'voiced must be an Int32Array, Uint8Array, Float32Array, number[], or boolean[]',
    );
  }
  const out = new Float32Array(voiced.length);
  for (let index = 0; index < voiced.length; index += 1) {
    const value = voiced[index];
    if (typeof value !== 'number' && typeof value !== 'boolean') {
      throw new TypeError('voiced array entries must be numbers or booleans');
    }
    out[index] = value ? 1 : 0;
  }
  return out;
}
