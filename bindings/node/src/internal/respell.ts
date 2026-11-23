/**
 * The facade half of a converted argument's refusal: the native translation point attaches the
 * refusal's parts, and this re-renders them under the caller's name through the core renderer.
 */

import { addon } from '../native.js';
import {
  type RefusalRecord,
  type Respelled,
  type Respelling,
  refusalOf,
  retextRefusal,
} from './entry.js';

interface Rendered {
  message: string;
  refusal: RefusalRecord;
}

const RENDER = (refusal: RefusalRecord, subjects: string[], scales: number[]): Rendered =>
  addon.renderRefusal(refusal, subjects, scales) as Rendered;

/**
 * Re-renders a core refusal under the name and unit the caller wrote, for an argument the facade
 * converted before the native call (seconds given in place of samples).
 */
export function respellRefusal(error: unknown, respelled: Respelled): unknown {
  const refusal = refusalOf(error);
  if (refusal === undefined || !(error instanceof Error)) {
    return error;
  }
  const subjects = refusal.renamedTo
    ? [...refusal.subjects, refusal.renamedTo]
    : [...refusal.subjects];
  const own = (subject: string): Respelling | undefined =>
    Object.hasOwn(respelled, subject) ? respelled[subject] : undefined;
  if (!subjects.some((subject) => own(subject) !== undefined)) {
    return error;
  }
  const rendered = RENDER(
    refusal,
    subjects.map((subject) => own(subject)?.name ?? subject),
    subjects.map((subject) => own(subject)?.scale ?? 1),
  );
  retextRefusal(error, rendered.refusal, rendered.message);
  return error;
}

/** Runs a native call whose converted arguments @p respelled declares; a promise is followed too. */
export function withRespelled<T>(respelled: Respelled, call: () => T): T {
  try {
    const result = call();
    if (result instanceof Promise) {
      return result.catch((error: unknown) => {
        throw respellRefusal(error, respelled);
      }) as T;
    }
    return result;
  } catch (error) {
    throw respellRefusal(error, respelled);
  }
}

/**
 * The counts a native entry derives from the length of an array the caller passed, by entry. The
 * caller never wrote the count, so a refusal of one names the array's length instead.
 */
const DERIVED_COUNTS: Readonly<Record<string, Readonly<Record<string, string>>>> = {
  pitchCorrectTimevarying: { nFrames: 'f0Hz' },
  renderNotes: { nFrames: 'f0Hz' },
  splitNote: { nFrames: 'f0Hz', noteCount: 'notes' },
  mergeNotes: { nFrames: 'f0Hz', noteCount: 'notes' },
  'Project.setWarpMap': { 'desc.anchorCount': 'anchors' },
  'RealtimeEngine.setTrackLanes': { laneCount: 'lanes' },
  'RealtimeEngine.renderOffline': { numChannels: 'channels' },
  'StreamingRetune.processMono': { numSamples: 'samples' },
};

/** The respelling of every count @p entry derives from an array's length, as `<array>.length`. */
export function derivedCounts(entry: string): Respelled {
  const counts = Object.hasOwn(DERIVED_COUNTS, entry) ? DERIVED_COUNTS[entry] : {};
  return Object.fromEntries(
    Object.entries(counts ?? {}).map(([count, array]) => [
      count,
      { name: `${array}.length`, scale: 1 },
    ]),
  );
}
