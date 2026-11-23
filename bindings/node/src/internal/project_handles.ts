import { refuseType } from './entry.js';

/**
 * Native handles of compiled timelines, kept out of the public class so the
 * realtime engine can read them without the handle being a property of the
 * timeline a caller sees.
 */

const timelineHandles = new WeakMap<object, unknown>();

/** Associate a timeline with its native handle. */
export function setTimelineHandle(timeline: object, native: unknown): void {
  timelineHandles.set(timeline, native);
}

/** The native handle of a timeline; `TypeError` for anything that is not one. */
export function timelineHandle(timeline: unknown): unknown {
  const native =
    typeof timeline === 'object' && timeline !== null ? timelineHandles.get(timeline) : undefined;
  if (native === undefined) {
    throw refuseType('timeline must be a ProjectTimeline instance');
  }
  return native;
}
