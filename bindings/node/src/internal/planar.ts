import { refuse, refuseType } from './entry.js';
import { requireListBudget } from './list_budget.js';
/** Planar channel arrays and the interleaved layout the native project calls read and write. */

/** One interleaved buffer and the geometry it was built from. */
export interface InterleavedAudio {
  data: Float32Array;
  numChannels: number;
}

/**
 * Interleave planar channels, refusing a non-array, an empty list, a plane that is
 * not a `Float32Array` and planes of unequal length, each by name.
 */
export function interleavePlanar(channels: unknown, argName = 'channels'): InterleavedAudio {
  if (!Array.isArray(channels)) {
    throw refuseType(`${argName} must be an array of Float32Array`);
  }
  requireListBudget(channels, argName);
  if (channels.length === 0) {
    throw refuse(`${argName} must not be empty`);
  }
  channels.forEach((plane, index) => {
    if (!(plane instanceof Float32Array)) {
      throw refuseType(`${argName}[${index}] must be a Float32Array`);
    }
  });
  const planes = channels as Float32Array[];
  const frames = (planes[0] as Float32Array).length;
  if (planes.some((plane) => plane.length !== frames)) {
    throw refuse(`${argName} must all have the same length`);
  }
  const numChannels = planes.length;
  const data = new Float32Array(frames * numChannels);
  for (let c = 0; c < numChannels; c++) {
    const plane = planes[c] as Float32Array;
    for (let i = 0; i < frames; i++) {
      data[i * numChannels + c] = plane[i] as number;
    }
  }
  return { data, numChannels };
}

/** Split an interleaved buffer into one `Float32Array` per channel. */
export function deinterleave(data: Float32Array, numChannels: number): Float32Array[] {
  const frames = Math.floor(data.length / numChannels);
  const planes = Array.from({ length: numChannels }, () => new Float32Array(frames));
  for (let c = 0; c < numChannels; c++) {
    const plane = planes[c] as Float32Array;
    for (let i = 0; i < frames; i++) {
      plane[i] = data[i * numChannels + c] as number;
    }
  }
  return planes;
}
