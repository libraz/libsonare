export type WorkletInput = readonly (readonly Float32Array[])[];
export type WorkletOutput = Float32Array[][];

/**
 * Copies one plane per output channel. Shared by every worklet output — the
 * engine's program and cue buses and the voice changer — so they cannot drift
 * in their padding behaviour. Allocation-free.
 *
 * A host output wider than the engine's plane count is mapped the way Web Audio
 * up-mixes rather than by padding with a copy of plane 0:
 * - A single plane fans out to every output channel, so a mono engine driving a
 *   stereo host stays centred instead of hard-panned left.
 * - With two or more planes, an output channel past the last plane is silence.
 *   Duplicating plane 0 into it would put the left signal in a rear or centre
 *   channel and add correlated energy the engine never produced.
 *
 * Everything the source does not fill is zeroed — the tail past `frames`, and
 * the remainder of a plane shorter than `frames` — so no sample of the previous
 * block survives into this one.
 */
export function copyPlanesToOutput(
  output: Float32Array[],
  planes: readonly Float32Array[],
  frames: number,
): void {
  const monoFanOut = planes.length === 1;
  for (let ch = 0; ch < output.length; ch++) {
    const target = output[ch];
    const source = monoFanOut ? planes[0] : planes[ch];
    let copied = 0;
    if (source) {
      copied = Math.min(target.length, frames, source.length);
      target.set(source.subarray(0, copied));
    }
    if (copied < target.length) {
      target.fill(0, copied);
    }
  }
}
