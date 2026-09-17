/**
 * Input shape and geometry check the repair families share.
 */

import { assertIntegerValue, assertPositiveInteger } from './validation.js';

/**
 * Check the STFT geometry the denoise and dereverb entries share, before the
 * addon's options reader narrows it onto a size the core accepts.
 *
 * Only what the caller supplied, because the whole request travels to that
 * reader. The two halves take different guards because the core states
 * different rules: `nFft` must be a POWER OF TWO, which positivity does not
 * imply, so only the truncation is closed here and the size rule stays with the
 * reader that names it; `hopLength` is checked `> 0` outright, so positivity is
 * this field's own domain and the `hopLength <= nFft` pairing stays the core's.
 */
export function assertRepairGeometry(
  fnName: string,
  options: { nFft?: number; hopLength?: number },
): void {
  if (options.nFft !== undefined) {
    assertIntegerValue(fnName, options.nFft, 'nFft');
  }
  if (options.hopLength !== undefined) {
    assertPositiveInteger(fnName, options.hopLength, 'hopLength');
  }
}

/** Common input fields for offline repair processors. */
export interface MasteringRepairSamplesRequest {
  samples: Float32Array;
  sampleRate?: number;
}
