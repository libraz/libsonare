import { projectModule } from './project_internal';
import type { AlignTakeToReferenceRequest, AlignTakeToReferenceResult } from './project_types';
import { assertSampleRate, assertSamples } from './validation';

/**
 * Aligns one take to a reference timeline and returns the warp anchors that place
 * the take under it.
 *
 * A chromagram is measured for each signal and the two are aligned, then the
 * alignment is reduced to anchors {@link Project.setWarpMap} accepts: at least
 * two finite, strictly increasing pairs. The reduction is needed rather than
 * decorative — an alignment path advances one axis at a time, so the raw
 * correspondence repeats a coordinate wherever one signal carries more frames
 * than the other, and those pairs are refused as a warp map.
 *
 * **The anchors are oriented for the take's own clip.** `warpSample` is a
 * position on the REFERENCE timeline and `sourceSample` the corresponding
 * position in the TAKE, which is the direction a clip whose source is that take
 * needs. This is why the entry point exists rather than the core alignment
 * primitive being exposed directly: that one names its arguments the other way
 * round, so passing the reference as its reference yields the inverse map and
 * nothing reports it.
 *
 * Both signals are read at `sampleRate`; resample first if they differ, since
 * the alignment does no I/O and no rate conversion.
 *
 * {@link AlignTakeToReferenceResult.alignment} is descriptive only — it says how
 * far the path strayed from a constant rate, so a caller can tell a take the
 * reference genuinely fits from one it does not, and supplies its own threshold.
 *
 * @param request - The two signals, their shared sample rate, and the chroma
 *   resolution
 * @returns The anchors and how well the alignment was conditioned
 * @throws {RangeError} on an empty buffer, a non-finite sample, or a `sampleRate`
 *   outside `[8000, 384000]`
 * @throws {SonareError} `InvalidParameter` on a non-positive `hopLength` or
 *   `binsPerOctave`, on a `binsPerOctave` that is not a multiple of 12, on a
 *   signal too short to measure two chroma frames, or when the two signals produce
 *   no pair of distinct anchors — which is what an unalignable pair looks like,
 *   and is reported rather than answered with a map you cannot use
 *
 * @example
 * ```typescript
 * const { anchors } = alignTakeToReference({ reference: guide, take, sampleRate });
 * const project = new Project();
 * const trackId = project.addTrack({ kind: 'audio' });
 * const clipId = project.addClip({ trackId, lengthPpq: 4 * 960, audio: take, audioSampleRate: sampleRate });
 * project.setWarpMap({ id: 1, anchors });
 * project.setClipWarpRef(clipId, 1);
 * ```
 */
export function alignTakeToReference(
  request: AlignTakeToReferenceRequest,
): AlignTakeToReferenceResult {
  assertSamples('alignTakeToReference', request.reference, true, 'reference');
  assertSamples('alignTakeToReference', request.take, true, 'take');
  assertSampleRate('alignTakeToReference', request.sampleRate);
  return projectModule().alignTakeToReference(
    request.reference,
    request.take,
    request.sampleRate,
    request,
  );
}
