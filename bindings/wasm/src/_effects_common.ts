/**
 * Input conversion the effect families share.
 */

import type { VoicedFlags } from './public_types';

// The embind layer reads the companion voicing array as Float32Array. A flag is
// a decision, not a magnitude: collapse to 1/0 on truthiness, which is the same
// reduction the Node facade applies, so both surfaces agree on every accepted
// input type.
export function toVoicedFloat32(voiced: VoicedFlags): Float32Array {
  const out = new Float32Array(voiced.length);
  for (let index = 0; index < voiced.length; index += 1) {
    out[index] = voiced[index] ? 1 : 0;
  }
  return out;
}
