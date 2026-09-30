import { addon } from './native.js';
import type { AlignTakeToReferenceRequest, AlignTakeToReferenceResult } from './types.js';
import { assertSampleRate, assertSamples } from './validation.js';

/**
 * Align one take to a reference timeline, producing the warp anchors that place
 * the take under it.
 *
 * Measures a chromagram for each signal and aligns them, then reduces the
 * alignment to anchors {@link Project.setWarpMap} accepts: at least two finite,
 * strictly increasing pairs. The reduction is needed rather than decorative — an
 * alignment path advances one axis at a time, so the raw correspondence repeats a
 * coordinate wherever one signal carries more frames than the other, and those
 * pairs are refused as a warp map.
 *
 * **The anchors are oriented for the take's own clip.** `warpSample` is a
 * position on the REFERENCE timeline and `sourceSample` the corresponding
 * position in the TAKE, which is the direction a clip whose source is that take
 * needs. This is why the entry point exists rather than the core alignment being
 * exposed directly: that one names its arguments the other way round, so passing
 * the reference as its reference yields the inverse map and nothing reports it.
 *
 * `alignment` reports how well the alignment was conditioned, so a take the
 * reference genuinely fits can be told from one it does not. It is descriptive
 * only: no field makes the call fail.
 *
 * @example
 * ```typescript
 * const { anchors, alignment } = alignTakeToReference({
 *   reference: guide,
 *   take: comp,
 *   sampleRate: 48000,
 * });
 * console.log(alignment.meanResidualFrames);
 * project.setWarpMap({ id: 1, name: 'comp', anchors });
 * project.setClipWarpRef(takeClipId, 1);
 * ```
 *
 * @throws `RangeError` when either buffer is empty or carries a non-finite
 *         sample, or when `sampleRate` is out of range; `TypeError` when
 *         `hopLength` or `binsPerOctave` carries the wrong type; and a
 *         `SonareError` with `InvalidParameter` for a `binsPerOctave` that is not
 *         a multiple of 12 or that `sampleRate` cannot carry, and when the two
 *         signals produce no pair of distinct anchors — which is what an
 *         unalignable pair looks like, and is reported rather than answered with
 *         a map a caller cannot use.
 */
export function alignTakeToReference(
  request: AlignTakeToReferenceRequest,
): AlignTakeToReferenceResult {
  assertSamples('alignTakeToReference', request.reference, true, 'reference');
  assertSamples('alignTakeToReference', request.take, true, 'take');
  assertSampleRate('alignTakeToReference', request.sampleRate);
  // The config keys are forwarded unvalidated on purpose: both default at 0,
  // which the library reads as "keep the default", so the addon's property
  // readers are the layer that refuses a wrong type by name.
  return addon.alignTakeToReference(request.reference, request.take, request.sampleRate, {
    hopLength: request.hopLength,
    binsPerOctave: request.binsPerOctave,
  });
}
